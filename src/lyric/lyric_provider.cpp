#include "lyric_provider.h"

#include "util/base64.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
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

// ---------- 酷狗 KRC 逐字歌词 ----------

// 酷狗搜歌结果
struct KugouSong {
    std::string hash;
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
};

// KRC 解密：酷狗客户端硬编码的 16 字节 XOR 密钥
constexpr uint8_t kKrcXorKey[16] = {0x40, 0x47, 0x61, 0x77, 0x5E, 0x32, 0x74, 0x47,
                                    0x51, 0x36, 0x31, 0x2D, 0xCE, 0xD2, 0x6E, 0x69};

// base64 -> 跳过 "krc1" 头 -> XOR -> zlib 解压 -> UTF-8 文本
bool krcDecode(const std::string& b64, std::string& out) {
    std::string raw = base64Decode(b64);
    if (raw.size() <= 4 || raw.compare(0, 4, "krc1") != 0)
        return false;
    std::string xored = raw.substr(4);
    for (size_t i = 0; i < xored.size(); ++i)
        xored[i] = (char)((uint8_t)xored[i] ^ kKrcXorKey[i % 16]);

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK)
        return false;
    zs.next_in = reinterpret_cast<Bytef*>(xored.data());
    zs.avail_in = (uInt)xored.size();
    out.clear();
    char buf[16384];
    int ret = Z_OK;
    while (ret == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    }
    inflateEnd(&zs);
    return ret == Z_STREAM_END && !out.empty();
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// 行内无 <...> token 时按字符数均分行时长生成逐字（与 echo-music 的兜底一致）
void buildFallbackChars(const std::wstring& text, int64_t lineStart, int64_t lineDuration,
                        std::vector<LyricChar>& out) {
    if (text.empty())
        return;
    int64_t total = std::max<int64_t>(lineDuration, (int64_t)text.size() * 120);
    for (size_t i = 0; i < text.size(); ++i) {
        int64_t start = lineStart + (int64_t)(i * total / text.size());
        int64_t end = lineStart + (int64_t)((i + 1) * total / text.size());
        out.push_back({start, std::max(end, start + 1), text.substr(i, 1)});
    }
}

// 解析 KRC 文本（[行开始,行时长]<字偏移,字时长,0>文字...）；成功返回 true 并填充 out
bool parseKrc(std::string content, std::vector<LyricLine>& out) {
    if (!content.empty() && content.compare(0, 3, "\xEF\xBB\xBF") == 0)
        content.erase(0, 3); // 去 BOM
    replaceAll(content, "&apos;", "'");
    replaceAll(content, "&quot;", "\"");
    replaceAll(content, "&amp;", "&");

    std::vector<LyricLine> lines;
    std::istringstream ss(content);
    std::string raw;
    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        // 行头必须是 [数字,数字]，否则为元数据行（ti:/ar:/language: 等）
        if (raw.size() < 3 || raw[0] != '[' || !std::isdigit((unsigned char)raw[1]))
            continue;
        size_t close = raw.find(']');
        if (close == std::string::npos)
            continue;
        std::string header = raw.substr(1, close - 1);
        size_t comma = header.find(',');
        if (comma == std::string::npos)
            continue;
        char* end = nullptr;
        long long lineStart = std::strtoll(header.c_str(), &end, 10);
        if (end != header.c_str() + comma)
            continue;
        long long lineDuration = std::strtoll(header.c_str() + comma + 1, nullptr, 10);

        // 逐字 token：<偏移,时长,0>文本
        std::vector<LyricChar> chars;
        std::wstring text;
        size_t pos = close + 1;
        while (pos < raw.size()) {
            if (raw[pos] != '<')
                break;
            size_t gt = raw.find('>', pos);
            if (gt == std::string::npos)
                break;
            std::string tag = raw.substr(pos + 1, gt - pos - 1);
            size_t c1 = tag.find(',');
            size_t c2 = c1 == std::string::npos ? c1 : tag.find(',', c1 + 1);
            if (c2 == std::string::npos)
                break;
            long long charOff = std::strtoll(tag.c_str(), nullptr, 10);
            long long charDur = std::strtoll(tag.c_str() + c1 + 1, nullptr, 10);
            size_t next = raw.find('<', gt + 1);
            std::wstring token = toWide(raw.substr(gt + 1, next - gt - 1));
            if (!token.empty()) {
                int64_t start = lineStart + charOff;
                chars.push_back({start, start + std::max<long long>(charDur, 1), token});
                text += token;
            }
            pos = next;
        }
        if (chars.empty()) {
            // 无逐字 token：去掉残留标签后按均分兜底
            std::string plain = raw.substr(close + 1);
            std::string cleaned;
            for (size_t i = 0; i < plain.size();) {
                if (plain[i] == '<') {
                    size_t gt = plain.find('>', i);
                    if (gt == std::string::npos) break;
                    i = gt + 1;
                } else {
                    cleaned += plain[i++];
                }
            }
            std::wstring t = trimW(toWide(cleaned));
            if (t.empty())
                continue;
            buildFallbackChars(t, lineStart, lineDuration, chars);
            text = t;
        }
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            text.pop_back();
        if (text.empty())
            continue;
        lines.push_back({(int64_t)lineStart, text, std::move(chars)});
    }
    if (lines.empty())
        return false;
    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    out = std::move(lines);
    return true;
}

// 酷狗搜歌：关键词（歌名+歌手）搜索，返回前 maxN 条候选；
// 返回 true 表示 HTTP/JSON 解析成功（结果可能为空）
bool kugouSearchSongs(CURL* curl, const std::wstring& title, const std::wstring& artist, int maxN,
                      std::vector<KugouSong>& out) {
    out.clear();
    std::wstring query = title;
    if (!artist.empty() && !titleEndsWithArtist(title, artist))
        query += L' ' + artist;
    std::string keyword = toUtf8(query);
    char* esc = curl_easy_escape(curl, keyword.c_str(), (int)keyword.size());
    std::string url = "http://mobilecdn.kugou.com/api/v3/search/song?format=json&keyword=" +
                      std::string(esc ? esc : "") + "&page=1&pagesize=5&showtype=1";
    if (esc)
        curl_free(esc);
    std::string body;
    if (!httpGet(curl, url, nullptr, body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    auto& info = j["data"]["info"];
    if (!info.is_array())
        return false;
    for (auto& s : info) {
        KugouSong song;
        song.hash = s.value("hash", std::string());
        if (song.hash.empty())
            continue;
        song.name = toWide(s.value("songname", std::string()));
        song.singer = toWide(s.value("singername", std::string()));
        song.durationMs = (int64_t)s.value("duration", 0) * 1000;
        out.push_back(std::move(song));
        if ((int)out.size() >= maxN)
            break;
    }
    return true;
}

// 酷狗搜歌打分选最佳：标题分非 0 且总分 >= 700 的候选中分数最高者
bool kugouSearchSong(CURL* curl, const std::wstring& title, const std::wstring& artist,
                     int64_t durationMs, std::string& hashOut, int64_t& kugouDurMsOut) {
    std::vector<KugouSong> songs;
    if (!kugouSearchSongs(curl, title, artist, 5, songs))
        return false;
    const KugouSong* best = nullptr;
    int bestScore = 0;
    for (auto& song : songs) {
        int t = titleSimilarity(title, song.name);
        if (t == 0)
            continue;
        int score = t * 12 + singerSimilarity(artist, song.singer) * 5 +
                    durationScore(durationMs, song.durationMs) * 3;
        if (score < 700 || score <= bestScore)
            continue;
        bestScore = score;
        best = &song;
    }
    if (!best)
        return false;
    hashOut = best->hash;
    kugouDurMsOut = best->durationMs;
    return true;
}

// 酷狗搜词：按 hash 取前 3 个歌词候选（官方推荐优先）的 id + accesskey
bool kugouSearchLyric(CURL* curl, const std::string& hash, int64_t durationMs,
                      std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    std::string url = "http://lyrics.kugou.com/search?ver=1&man=yes&client=pc&hash=" + hash +
                      "&duration=" + std::to_string(durationMs);
    std::string body;
    if (!httpGet(curl, url, nullptr, body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    auto& cands = j["candidates"];
    if (!cands.is_array())
        return false;
    for (auto& c : cands) {
        std::string id = c.value("id", std::string());
        std::string accesskey = c.value("accesskey", std::string());
        if (!id.empty() && !accesskey.empty())
            out.emplace_back(std::move(id), std::move(accesskey));
        if (out.size() >= 3)
            break;
    }
    return !out.empty();
}

// 酷狗下载 KRC 并解析为逐字歌词
bool kugouDownloadKrc(CURL* curl, const std::string& id, const std::string& accesskey,
                      std::vector<LyricLine>& out) {
    std::string url = "http://lyrics.kugou.com/download?ver=1&client=pc&id=" + id +
                      "&accesskey=" + accesskey + "&fmt=krc&charset=utf8";
    std::string body;
    if (!httpGet(curl, url, nullptr, body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    std::string krcText;
    if (!krcDecode(j.value("content", std::string()), krcText))
        return false;
    return parseKrc(krcText, out);
}

// 按酷狗 hash 下载 KRC：搜词后依次尝试歌词候选，任一下载解析成功即返回
bool fetchKrcByHash(CURL* curl, const std::string& hash, int64_t durationMs,
                    std::vector<LyricLine>& out) {
    std::vector<std::pair<std::string, std::string>> cands;
    if (!kugouSearchLyric(curl, hash, durationMs, cands))
        return false;
    for (const auto& [id, accesskey] : cands) {
        if (kugouDownloadKrc(curl, id, accesskey, out))
            return true;
    }
    return false;
}

// 酷狗 KRC 完整链路：搜歌 -> 搜词 -> 下载解码解析；任一步失败返回 false
bool fetchKrc(CURL* curl, const std::wstring& title, const std::wstring& artist,
              int64_t durationMs, std::vector<LyricLine>& out) {
    std::wstring queryTitle = stripVersionSuffix(title);
    std::string hash;
    int64_t kugouDurMs = durationMs;
    if (!kugouSearchSong(curl, queryTitle, artist, durationMs, hash, kugouDurMs))
        return false;
    return fetchKrcByHash(curl, hash, kugouDurMs > 0 ? kugouDurMs : durationMs, out);
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
    std::wstring overridePath; // 手动歌词持久化文件（JSON），空表示不持久化
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

    // ---------- 手动歌词持久化（JSON，UTF-8） ----------

    // 调用方需已持有 mtx
    void saveOverridesLocked() {
        if (overridePath.empty())
            return;
        try {
            nlohmann::json j = nlohmann::json::object();
            for (const auto& [key, entry] : manualOverrides) {
                nlohmann::json e;
                e["songmid"] = toUtf8(entry.info.songmid);
                e["albummid"] = toUtf8(entry.info.albummid);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& l : entry.lines) {
                    nlohmann::json chars = nlohmann::json::array();
                    for (const auto& c : l.chars)
                        chars.push_back({c.startMs, c.endMs, toUtf8(c.text)});
                    arr.push_back({l.ms, toUtf8(l.text), std::move(chars)});
                }
                e["lines"] = std::move(arr);
                j[toUtf8(key)] = std::move(e);
            }
            std::ofstream f(std::filesystem::path(overridePath),
                            std::ios::binary | std::ios::trunc);
            f << j.dump();
        } catch (...) {
        }
    }

    // 调用方需已持有 mtx
    void loadOverridesLocked() {
        if (overridePath.empty())
            return;
        try {
            std::ifstream f(std::filesystem::path(overridePath), std::ios::binary);
            if (!f)
                return;
            auto j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_discarded() || !j.is_object())
                return;
            for (const auto& [k, e] : j.items()) {
                CacheEntry entry;
                entry.info.songmid = toWide(e.value("songmid", std::string()));
                entry.info.albummid = toWide(e.value("albummid", std::string()));
                if (e["lines"].is_array()) {
                    for (const auto& l : e["lines"]) {
                        if (!l.is_array() || l.size() < 2)
                            continue;
                        LyricLine line{l[0].get<int64_t>(), toWide(l[1].get<std::string>()), {}};
                        // 第三项为逐字时间轴（旧格式没有则留空，兼容旧文件）
                        if (l.size() >= 3 && l[2].is_array()) {
                            for (const auto& c : l[2]) {
                                if (!c.is_array() || c.size() < 3)
                                    continue;
                                line.chars.push_back({c[0].get<int64_t>(), c[1].get<int64_t>(),
                                                      toWide(c[2].get<std::string>())});
                            }
                        }
                        entry.lines.push_back(std::move(line));
                    }
                }
                if (!entry.lines.empty())
                    manualOverrides[toWide(k)] = std::move(entry);
            }
        } catch (...) {
        }
    }

    // 用 QQ 搜索补全 songmid/albummid（KRC 命中后调用，供封面兜底）；失败静默忽略
    void fillSongInfo(CURL* curl, const std::wstring& title, const std::wstring& artist,
                      int64_t durationMs, SongInfo& info) {
        std::wstring query = stripVersionSuffix(title);
        if (!artist.empty() && !titleEndsWithArtist(title, artist))
            query += L' ' + artist;
        std::vector<Candidate> cands;
        if (!searchSongs(curl, query, cands))
            return;
        const Candidate* best = nullptr;
        int bestScore = 0;
        for (auto& c : cands) {
            int t = titleSimilarity(title, c.name);
            if (t == 0)
                continue;
            int score = t * 12 + singerSimilarity(artist, c.singer) * 5 +
                        durationScore(durationMs, c.intervalMs) * 3;
            if (score < 700 || score <= bestScore)
                continue;
            bestScore = score;
            best = &c;
        }
        if (best) {
            info.songmid = toWide(best->songmid);
            info.albummid = toWide(best->albummid);
        }
    }

    // 搜索 -> 匹配 -> 下载 -> 解析；成功填充 out 与 info 返回 true
    bool fetch(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
               std::vector<LyricLine>& out, SongInfo& info) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        // 优先酷狗 KRC 逐字歌词
        if (fetchKrc(curl, title, artist, durationMs, out)) {
            fillSongInfo(curl, title, artist, durationMs, info); // 补齐 albummid 供封面兜底
            curl_easy_cleanup(curl);
            return true;
        }

        // 回退 QQ 音乐 LRC

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
        std::vector<SearchCandidate> result;
        if (curl) {
            // 酷狗 KRC 逐字候选（最多 5 条，排在前面）
            std::vector<KugouSong> kugouSongs;
            if (kugouSearchSongs(curl, stripVersionSuffix(title), artist, 5, kugouSongs)) {
                for (auto& s : kugouSongs) {
                    result.push_back({{}, {}, toWide(s.hash), s.name, s.singer, s.durationMs,
                                      true});
                }
            }
            // QQ LRC 整行候选（最多 5 条）
            std::wstring query = title;
            if (!artist.empty() && !titleEndsWithArtist(title, artist))
                query += L' ' + artist;
            std::vector<Candidate> cands;
            searchSongs(curl, query, cands);
            if (cands.empty())
                searchSongs(curl, title, cands);
            curl_easy_cleanup(curl);
            for (size_t i = 0; i < cands.size() && i < 5; ++i) {
                const auto& c = cands[i];
                result.push_back({toWide(c.songmid), toWide(c.albummid), {}, c.name, c.singer,
                                  c.intervalMs, false});
            }
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
            // 按候选来源取词：KRC 候选用酷狗 hash 拉逐字歌词，LRC 候选用 QQ songmid
            if (cand.krc)
                ok = fetchKrcByHash(curl, toUtf8(cand.kugouHash), cand.durationMs, lines);
            else
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
    impl_->saveOverridesLocked();
}

void LyricProvider::setManualOverridePath(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->overridePath = path;
    impl_->loadOverridesLocked();
}
