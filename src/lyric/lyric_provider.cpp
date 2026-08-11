#include "lyric_provider.h"

#include "util/base64.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cwctype>
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
    std::string albummid;
    std::wstring name;
    std::wstring singer;
    int64_t intervalMs = 0;
};

// ---------- 歌曲匹配辅助函数 ----------

std::wstring trimW(const std::wstring& s) {
    size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    size_t last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

// 规范化：小写、合并连续空白、去首尾空白、去除尾部括号版本信息
std::wstring normalizeTitle(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool prevSpace = true;
    for (wchar_t ch : s) {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            if (!prevSpace) {
                out.push_back(L' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == L' ')
        out.pop_back();

    // 去除尾部 (...)/（...)/[...]/【...】版本标注，如 （剧情版）、(Live)
    while (!out.empty()) {
        wchar_t open = 0;
        wchar_t close = out.back();
        if (close == L')')
            open = L'(';
        else if (close == L'）')
            open = L'（';
        else if (close == L']')
            open = L'[';
        else if (close == L'】')
            open = L'【';
        else
            break;
        size_t pos = out.find_last_of(open);
        if (pos == std::wstring::npos || pos == 0)
            break;
        size_t end = pos;
        while (end > 0 && out[end - 1] == L' ')
            --end;
        out.resize(end);
    }
    return out;
}

// 判断歌名末尾是否已经包含 " - 歌手"、" / 歌手" 等歌手信息，避免搜索时重复附加歌手
bool titleEndsWithArtist(const std::wstring& title, const std::wstring& artist) {
    if (artist.empty())
        return false;
    const wchar_t* seps[] = {L" - ", L" – ", L" — ", L" / "};
    for (const wchar_t* sep : seps) {
        std::wstring suffix = sep + artist;
        if (title.size() > suffix.size() &&
            title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    }
    return false;
}

// 去除歌名尾部的 (...)/（...)/[...]/【...】版本标注（不改变大小写），用于净化搜索词。
// SMTC 歌名常带 (DJ版)、（剧情版）等后缀，带着它们搜索命中率很低。
std::wstring stripVersionSuffix(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
        s.pop_back();
    while (!s.empty()) {
        wchar_t open = 0;
        wchar_t close = s.back();
        if (close == L')')
            open = L'(';
        else if (close == L'）')
            open = L'（';
        else if (close == L']')
            open = L'[';
        else if (close == L'】')
            open = L'【';
        else
            break;
        size_t pos = s.find_last_of(open);
        if (pos == std::wstring::npos || pos == 0)
            break;
        size_t end = pos;
        while (end > 0 && s[end - 1] == L' ')
            --end;
        s.resize(end);
    }
    return s;
}

// 0-100，100 表示完全匹配
int titleSimilarity(const std::wstring& a, const std::wstring& b) {
    std::wstring na = normalizeTitle(a);
    std::wstring nb = normalizeTitle(b);
    if (na.empty() || nb.empty())
        return 0;
    if (na == nb)
        return 100;
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos)
        return 85;
    size_t minLen = std::min(na.size(), nb.size());
    size_t commonPrefix = 0;
    while (commonPrefix < minLen && na[commonPrefix] == nb[commonPrefix])
        ++commonPrefix;
    if (commonPrefix >= minLen * 8 / 10)
        return 70;
    return 0;
}

std::vector<std::wstring> splitSingers(const std::wstring& s) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t ch : s) {
        if (ch == L'/' || ch == L'、' || ch == L'&' || ch == L';' || ch == L'；') {
            auto t = trimW(current);
            if (!t.empty())
                parts.push_back(t);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    auto t = trimW(current);
    if (!t.empty())
        parts.push_back(t);
    return parts;
}

int singerSimilarity(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return 50; // 有一方未知，给中性分
    std::wstring na = normalizeTitle(a);
    std::wstring nb = normalizeTitle(b);
    if (na == nb)
        return 100;
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos)
        return 90;
    auto pa = splitSingers(na);
    auto pb = splitSingers(nb);
    if (pa.empty() || pb.empty())
        return 0;
    int matches = 0;
    for (auto& x : pa) {
        for (auto& y : pb) {
            if (x == y || (!x.empty() && !y.empty() &&
                           (x.find(y) != std::wstring::npos || y.find(x) != std::wstring::npos))) {
                ++matches;
                break;
            }
        }
    }
    if (matches == 0)
        return 0;
    return std::min(100, matches * 100 / static_cast<int>(std::max(pa.size(), pb.size())));
}

int durationScore(int64_t queryMs, int64_t candMs) {
    if (queryMs <= 0 || candMs <= 0)
        return 50; // 未知时长给中性分
    int64_t diff = std::llabs(static_cast<long long>(candMs - queryMs));
    if (diff <= 2000)
        return 100;
    if (diff <= 5000)
        return 70;
    if (diff <= 10000)
        return 40;
    return 0;
}

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

// 按关键词搜索歌曲，填充 out；返回 true 表示 HTTP/JSON 解析成功（结果可能为空）
bool searchSongs(CURL* curl, const std::wstring& query, std::vector<Candidate>& out) {
    out.clear();
    std::string keyword = toUtf8(query);
    char* esc = curl_easy_escape(curl, keyword.c_str(), (int)keyword.size());
    std::string url = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=" +
                      std::string(esc ? esc : "") + "&n=10&p=1&format=json&new_json=1";
    if (esc)
        curl_free(esc);

    std::string body;
    if (!httpGet(curl, url, "https://y.qq.com/", body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    auto& list = j["data"]["song"]["list"];
    if (!list.is_array())
        return false;
    for (auto& s : list) {
        Candidate c;
        c.songmid = s.value("mid", s.value("songmid", ""));
        c.albummid = s.value("album", nlohmann::json::object())
                         .value("mid", s.value("albummid", ""));
        c.name = toWide(s.value("name", s.value("songname", "")));
        std::wstring singers;
        if (s["singer"].is_array()) {
            for (auto& sg : s["singer"]) {
                if (!singers.empty())
                    singers += L'/';
                singers += toWide(sg.value("name", ""));
            }
        }
        c.singer = singers;
        c.intervalMs = (int64_t)s.value("interval", 0) * 1000;
        if (!c.songmid.empty())
            out.push_back(std::move(c));
    }
    return true;
}

// 按 songmid 下载并解析歌词；成功返回 true 并填充 out
bool downloadLyric(CURL* curl, const std::string& songmid, std::vector<LyricLine>& out) {
    std::string url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" +
                      songmid + "&g_tk=5381&format=json&nobase64=0";
    std::string body;
    if (!httpGet(curl, url, "https://y.qq.com/portal/player.html", body))
        return false;

    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || j.value("retcode", -1) != 0) return false;
    std::string lrc = base64Decode(j.value("lyric", std::string()));
    if (lrc.empty()) return false;
    out = parseLrc(lrc);
    return !out.empty();
}

} // namespace

struct CacheEntry {
    std::vector<LyricLine> lines;
    SongInfo info;
};

struct LyricProvider::Impl {
    mutable std::mutex mtx;
    std::vector<LyricLine> current;
    SongInfo currentSongInfo;
    std::unordered_map<std::wstring, CacheEntry> cache;
    std::unordered_map<std::wstring, CacheEntry> manualOverrides; // 用户手动选择，优先于自动匹配
    std::list<std::wstring> lru; // 前 = 最近使用
    std::unordered_map<std::wstring, std::list<std::wstring>::iterator> lruIt;
    std::atomic<uint64_t> generation{0};
    std::vector<std::thread> workers;

    ~Impl() {
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    bool cacheGet(const std::wstring& key, CacheEntry& out) {
        auto it = cache.find(key);
        if (it == cache.end()) return false;
        lru.erase(lruIt[key]);
        lru.push_front(key);
        lruIt[key] = lru.begin();
        out = it->second;
        return true;
    }

    void cachePut(const std::wstring& key, CacheEntry entry) {
        cache[key] = std::move(entry);
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

    // 搜索 -> 匹配 -> 下载 -> 解析；成功填充 out 与 info 返回 true
    bool fetch(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
               std::vector<LyricLine>& out, SongInfo& info) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        // 打分合格（歌名相似度非 0 且总分 >= 700）的候选按分数降序排列
        auto rankCandidates = [&](const std::vector<Candidate>& cands) {
            std::vector<std::pair<int, const Candidate*>> scored;
            for (auto& c : cands) {
                int t = titleSimilarity(title, c.name);
                if (t == 0)
                    continue;
                int s = singerSimilarity(artist, c.singer);
                int d = durationScore(durationMs, c.intervalMs);
                // 标题权重最高，歌手次之，时长再次
                int score = t * 12 + s * 5 + d * 3;
                if (score < 700)
                    continue;
                scored.push_back({score, &c});
            }
            std::stable_sort(scored.begin(), scored.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            return scored;
        };

        // 依次尝试候选：第一名没有歌词（伴奏版/剧情版等）时自动试次优候选
        constexpr int kMaxDownloadTries = 5;
        auto tryDownload = [&](const std::vector<Candidate>& cands) -> bool {
            int tries = 0;
            for (const auto& [score, c] : rankCandidates(cands)) {
                if (++tries > kMaxDownloadTries)
                    break;
                // 不带 Referer 会返回 -1310
                if (downloadLyric(curl, c->songmid, out)) {
                    info.songmid = toWide(c->songmid);
                    info.albummid = toWide(c->albummid);
                    return true;
                }
            }
            return false;
        };

        // 净化搜索词：剥掉歌名尾部版本标注，避免过长精确词搜不到
        std::wstring queryTitle = stripVersionSuffix(title);
        std::wstring query = queryTitle;
        if (!artist.empty() && !titleEndsWithArtist(title, artist)) {
            query += L' ';
            query += artist;
        }
        std::vector<Candidate> cands;
        if (!searchSongs(curl, query, cands)) {
            curl_easy_cleanup(curl);
            return false;
        }
        if (!tryDownload(cands)) {
            // 带歌手搜不到合适歌词时，尝试只按歌名搜索（部分歌曲 SMTC 歌手名与平台不一致）
            if (!searchSongs(curl, queryTitle, cands) || !tryDownload(cands)) {
                curl_easy_cleanup(curl);
                return false;
            }
        }
        curl_easy_cleanup(curl);
        return true;
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
        // 1) 用户手动选择的歌词优先级最高
        auto manualIt = impl_->manualOverrides.find(key);
        if (manualIt != impl_->manualOverrides.end()) {
            impl_->current = manualIt->second.lines;
            impl_->currentSongInfo = manualIt->second.info;
            if (cb) cb(true);
            return;
        }
        // 2) 普通缓存
        CacheEntry cached;
        if (impl_->cacheGet(key, cached)) {
            impl_->current = std::move(cached.lines);
            impl_->currentSongInfo = std::move(cached.info);
            if (cb) cb(true);
            return;
        }
    }
    Impl* impl = impl_.get();
    std::thread t([impl, gen, key, title, artist, durationMs, cb = std::move(cb)]() mutable {
        std::vector<LyricLine> result;
        SongInfo info;
        bool ok = impl->fetch(title, artist, durationMs, result, info);
        if (ok) {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation == gen) { // 防止过期请求覆盖新歌
                impl->current = result;
                impl->currentSongInfo = info;
                impl->cachePut(key, CacheEntry{std::move(result), std::move(info)});
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

const SongInfo& LyricProvider::songInfo() const {
    return impl_->currentSongInfo;
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

void LyricProvider::searchCandidatesAsync(const std::wstring& title, const std::wstring& artist,
                                          SearchCallback cb) {
    Impl* impl = impl_.get();
    std::thread t([impl, title, artist, cb = std::move(cb)]() mutable {
        CURL* curl = curl_easy_init();
        std::vector<Candidate> cands;
        if (curl) {
            std::wstring query = title;
            if (!artist.empty() && !titleEndsWithArtist(title, artist))
                query += L' ' + artist;
            searchSongs(curl, query, cands);
            if (cands.empty())
                searchSongs(curl, title, cands);
            curl_easy_cleanup(curl);
        }
        std::vector<SearchCandidate> result;
        result.reserve(std::min<size_t>(cands.size(), 5));
        for (size_t i = 0; i < cands.size() && i < 5; ++i) {
            const auto& c = cands[i];
            result.push_back(
                {toWide(c.songmid), toWide(c.albummid), c.name, c.singer, c.intervalMs});
        }
        if (cb)
            cb(std::move(result));
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->workers.push_back(std::move(t));
}

void LyricProvider::fetchLyricAsync(const SearchCandidate& cand, FetchCallback cb) {
    Impl* impl = impl_.get();
    std::thread t([impl, cand, cb = std::move(cb)]() mutable {
        CURL* curl = curl_easy_init();
        std::vector<LyricLine> lines;
        bool ok = false;
        if (curl) {
            ok = downloadLyric(curl, toUtf8(cand.songmid), lines);
            curl_easy_cleanup(curl);
        }
        SongInfo info{cand.songmid, cand.albummid};
        if (cb)
            cb(ok, std::move(lines), info);
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->workers.push_back(std::move(t));
}

void LyricProvider::setManualOverride(const std::wstring& title, const std::wstring& artist,
                                      std::vector<LyricLine> lines, SongInfo info) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    const std::wstring key = makeKey(title, artist);
    auto it = impl_->manualOverrides
                  .emplace(key, CacheEntry{std::move(lines), std::move(info)})
                  .first;
    impl_->current = it->second.lines;
    impl_->currentSongInfo = it->second.info;
}
