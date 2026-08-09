#include "lyric_provider.h"

#include "util/base64.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#include <windows.h>

namespace {

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
constexpr size_t kCacheCapacity = 32;
constexpr int64_t kDurationToleranceMs = 2000;

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

bool httpGet(CURL* curl, const std::string& url, const char* referer, std::string& out) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    out.clear();
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return rc == CURLE_OK && code == 200;
}

struct Candidate {
    std::string songmid;
    std::wstring name;
    std::wstring singer;
    int64_t intervalMs = 0;
};

// 解析 LRC 时间戳 "mm:ss.xx"；非纯数字（如 ti:/ar:）返回 false
bool parseTimeStamp(const std::string& s, int64_t& msOut) {
    auto colon = s.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    char* end = nullptr;
    double sec = std::strtod(s.c_str() + colon + 1, &end);
    if (end == s.c_str() + colon + 1) return false;
    long long min = std::strtoll(s.c_str(), nullptr, 10);
    msOut = (int64_t)((min * 60.0 + sec) * 1000.0 + 0.5);
    return true;
}

std::vector<LyricLine> parseLrc(const std::string& lrc) {
    std::vector<LyricLine> lines;
    std::istringstream ss(lrc);
    std::string raw;
    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        // 支持一行多时间戳：[t1][t2]文本
        std::vector<int64_t> stamps;
        size_t pos = 0;
        while (pos < raw.size() && raw[pos] == '[') {
            size_t close = raw.find(']', pos);
            if (close == std::string::npos) break;
            int64_t ms = 0;
            if (!parseTimeStamp(raw.substr(pos + 1, close - pos - 1), ms)) {
                stamps.clear();
                break; // [ti:] 等元数据行
            }
            stamps.push_back(ms);
            pos = close + 1;
        }
        if (stamps.empty()) continue;
        std::wstring text = toWide(raw.substr(pos));
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
        size_t first = text.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue; // 空行歌词跳过
        text = text.substr(first);
        for (int64_t ms : stamps) lines.push_back({ms, text});
    }
    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    return lines;
}

} // namespace

struct LyricProvider::Impl {
    mutable std::mutex mtx;
    std::vector<LyricLine> current;
    std::unordered_map<std::wstring, std::vector<LyricLine>> cache;
    std::list<std::wstring> lru; // 前 = 最近使用
    std::unordered_map<std::wstring, std::list<std::wstring>::iterator> lruIt;
    std::atomic<uint64_t> generation{0};
    std::vector<std::thread> workers;

    ~Impl() {
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    bool cacheGet(const std::wstring& key, std::vector<LyricLine>& out) {
        auto it = cache.find(key);
        if (it == cache.end()) return false;
        lru.erase(lruIt[key]);
        lru.push_front(key);
        lruIt[key] = lru.begin();
        out = it->second;
        return true;
    }

    void cachePut(const std::wstring& key, std::vector<LyricLine> lines) {
        cache[key] = std::move(lines);
        auto it = lruIt.find(key);
        if (it != lruIt.end()) lru.erase(it->second);
        lru.push_front(key);
        lruIt[key] = lru.begin();
        while (lru.size() > kCacheCapacity) {
            const std::wstring victim = lru.back();
            cache.erase(victim);
            lruIt.erase(victim);
            lru.pop_back();
        }
    }

    // 搜索 -> 匹配 -> 下载 -> 解析；成功填充 out 返回 true
    bool fetch(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
               std::vector<LyricLine>& out) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        // 1) 搜索
        std::string keyword = toUtf8(title + L' ' + artist);
        char* esc = curl_easy_escape(curl, keyword.c_str(), (int)keyword.size());
        std::string url = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=" +
                          std::string(esc ? esc : "") + "&n=10&p=1&format=json";
        if (esc) curl_free(esc);

        std::string body;
        bool ok = httpGet(curl, url, "https://y.qq.com/", body);
        std::vector<Candidate> cands;
        if (ok) {
            auto j = nlohmann::json::parse(body, nullptr, false);
            if (!j.is_discarded()) {
                auto& list = j["data"]["song"]["list"];
                if (list.is_array()) {
                    for (auto& s : list) {
                        Candidate c;
                        c.songmid = s.value("songmid", "");
                        c.name = toWide(s.value("songname", ""));
                        std::wstring singers;
                        if (s["singer"].is_array()) {
                            for (auto& sg : s["singer"]) {
                                if (!singers.empty()) singers += L'/';
                                singers += toWide(sg.value("name", ""));
                            }
                        }
                        c.singer = singers;
                        c.intervalMs = (int64_t)s.value("interval", 0) * 1000;
                        if (!c.songmid.empty()) cands.push_back(std::move(c));
                    }
                }
            }
        }

        // 2) 三重匹配：标题完全匹配 + 歌手包含 + 时长差 <= 2s；失败取第一条
        const Candidate* picked = nullptr;
        for (auto& c : cands) {
            bool titleEq = (c.name == title);
            bool singerHit = c.singer.find(artist) != std::wstring::npos ||
                             (!c.singer.empty() && artist.find(c.singer) != std::wstring::npos);
            bool durOk = durationMs <= 0 ||
                         std::llabs((long long)(c.intervalMs - durationMs)) <= kDurationToleranceMs;
            if (titleEq && singerHit && durOk) {
                picked = &c;
                break;
            }
        }
        if (!picked && !cands.empty()) picked = &cands.front();
        if (!picked) {
            curl_easy_cleanup(curl);
            return false;
        }

        // 3) 下载歌词（不带 Referer 会返回 -1310）
        url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" +
              picked->songmid + "&g_tk=5381&format=json&nobase64=0";
        ok = httpGet(curl, url, "https://y.qq.com/portal/player.html", body);
        curl_easy_cleanup(curl);
        if (!ok) return false;

        auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || j.value("retcode", -1) != 0) return false;
        std::string lrc = base64Decode(j.value("lyric", std::string()));
        if (lrc.empty()) return false;
        out = parseLrc(lrc);
        return !out.empty();
    }
};

LyricProvider::LyricProvider() : impl_(std::make_unique<Impl>()) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

LyricProvider::~LyricProvider() = default;

void LyricProvider::requestAsync(const std::wstring& title, const std::wstring& artist,
                                 int64_t durationMs, ReadyCallback cb) {
    const std::wstring key = makeKey(title, artist);
    uint64_t gen = ++impl_->generation;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        std::vector<LyricLine> cached;
        if (impl_->cacheGet(key, cached)) {
            impl_->current = std::move(cached);
            if (cb) cb(true);
            return;
        }
    }
    Impl* impl = impl_.get();
    std::thread t([impl, gen, key, title, artist, durationMs, cb = std::move(cb)]() mutable {
        std::vector<LyricLine> result;
        bool ok = impl->fetch(title, artist, durationMs, result);
        if (ok) {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation == gen) { // 防止过期请求覆盖新歌
                impl->current = result;
                impl->cachePut(key, std::move(result));
                if (cb) cb(true);
            }
        } else if (impl->generation == gen && cb) {
            cb(false);
        }
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->workers.push_back(std::move(t));
}

const std::vector<LyricLine>& LyricProvider::lines() const {
    return impl_->current;
}

std::wstring LyricProvider::makeKey(const std::wstring& title, const std::wstring& artist) {
    return title + L'|' + artist;
}

int LyricProvider::findLine(const std::vector<LyricLine>& lines, int64_t positionMs) {
    int lo = 0, hi = (int)lines.size() - 1, ans = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (lines[mid].ms <= positionMs) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}
