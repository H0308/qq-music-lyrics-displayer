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
#include <cwchar>
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

// 配对查找尾部括号组的开括号位置：从末尾向前计数配对，兼容嵌套；无配对返回 npos
size_t matchTrailingOpen(const std::wstring& s, wchar_t open, wchar_t close) {
    int depth = 0;
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] == close) {
            ++depth;
        } else if (s[i] == open) {
            if (--depth == 0)
                return i;
        }
    }
    return std::wstring::npos;
}

// 剥掉尾部 (...)/（...)/[...]/【...】版本标注；tagsOut 非空时输出各括号内容（原始大小写）
void stripTrailingBrackets(std::wstring& s, std::vector<std::wstring>* tagsOut) {
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
        size_t pos = matchTrailingOpen(s, open, close);
        if (pos == std::wstring::npos || pos == 0)
            break;
        if (tagsOut) {
            std::wstring tag = trimW(s.substr(pos + 1, s.size() - pos - 2));
            if (!tag.empty())
                tagsOut->push_back(tag);
        }
        size_t end = pos;
        while (end > 0 && s[end - 1] == L' ')
            --end;
        s.resize(end);
    }
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

    stripTrailingBrackets(out, nullptr);
    return out;
}

// 提取歌名尾部的版本标注（小写，自尾向首收集），如 "平凡之路 (Live)（现场）" -> {"现场", "live"}；无标注返回空
std::vector<std::wstring> extractVersionTags(const std::wstring& s) {
    std::wstring rest = s;
    std::vector<std::wstring> tags;
    stripTrailingBrackets(rest, &tags);
    for (auto& tag : tags)
        for (auto& ch : tag)
            ch = static_cast<wchar_t>(std::towlower(ch));
    return tags;
}

// 判断歌名末尾是否已经包含 " - 歌手"、" / 歌手" 等歌手信息，避免搜索时重复附加歌手
bool titleEndsWithArtist(const std::wstring& title, const std::wstring& artist) {
    if (artist.empty())
        return false;
    // 大小写不敏感比较
    std::wstring t = title;
    std::wstring a = artist;
    for (auto& ch : t)
        ch = static_cast<wchar_t>(std::towlower(ch));
    for (auto& ch : a)
        ch = static_cast<wchar_t>(std::towlower(ch));
    const wchar_t* seps[] = {L" - ", L"-", L" – ", L"–", L" — ", L"—", L" / ", L"/"};
    for (const wchar_t* sep : seps) {
        std::wstring suffix = sep + a;
        if (t.size() > suffix.size() &&
            t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    }
    return false;
}

// 去除歌名尾部的 (...)/（...)/[...]/【...】版本标注（不改变大小写），用于净化搜索词。
// SMTC 歌名常带 (DJ版)、（剧情版）等后缀，带着它们搜索命中率很低。
std::wstring stripVersionSuffix(std::wstring s) {
    stripTrailingBrackets(s, nullptr);
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
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos) {
        // 互为子串：按长度比例给分，惩罚候选比原标题多出大段内容（如拼盘/串烧/主题曲）
        size_t lo = std::min(na.size(), nb.size());
        size_t hi = std::max(na.size(), nb.size());
        return 60 + static_cast<int>(40 * lo / hi);
    }
    size_t minLen = std::min(na.size(), nb.size());
    size_t commonPrefix = 0;
    while (commonPrefix < minLen && na[commonPrefix] == nb[commonPrefix])
        ++commonPrefix;
    if (commonPrefix >= minLen * 8 / 10)
        return 70;
    return 0;
}

// 歌手分隔符：/ 、 & ; ；, ，× 以及 feat./ft./with/vs 等合作标注
std::vector<std::wstring> splitSingers(const std::wstring& s) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t ch : s) {
        if (ch == L'/' || ch == L'、' || ch == L'&' || ch == L';' || ch == L'；' ||
            ch == L',' || ch == L'，' || ch == L'×') {
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

void replaceAllW(std::wstring& s, const std::wstring& from, const std::wstring& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

int singerSimilarity(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return 50; // 有一方未知，给中性分
    std::wstring na = normalizeTitle(a);
    std::wstring nb = normalizeTitle(b);
    if (na == nb)
        return 100;
    // 合作标注统一为分隔符（na/nb 已小写）；首尾补空格避免误匹配单词内部
    for (std::wstring* n : {&na, &nb}) {
        *n = L' ' + *n + L' ';
        for (const wchar_t* kw : {L" feat. ", L" feat ", L" ft. ", L" ft ", L" featuring ",
                                  L" with ", L" vs ", L" vs. "})
            replaceAllW(*n, kw, L" / ");
        *n = trimW(*n);
    }
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
            if (x == y) { // 拆分后只认完全相等，避免 "张韶涵" 与 "张韶涵xx" 误配
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

// ---------- 统一候选打分 ----------

// 版本标注匹配：返回 -1 表示硬冲突（本地带版本标注而候选没有任何相同标注）；
// 否则 0-100：100 = 版本一致（双方都无标注或标注有交集），本地无标注而候选带标注给 60
int versionScore(const std::vector<std::wstring>& localTags, const std::wstring& candName) {
    std::vector<std::wstring> candTags = extractVersionTags(candName);
    if (localTags.empty())
        return candTags.empty() ? 100 : 60;
    for (const auto& lt : localTags) {
        for (const auto& ct : candTags) {
            if (lt == ct || lt.find(ct) != std::wstring::npos ||
                ct.find(lt) != std::wstring::npos)
                return 100;
        }
    }
    return -1;
}

// 一次匹配请求的本地歌曲信息
struct MatchQuery {
    std::wstring title;
    std::wstring artist;
    int64_t durationMs = 0;
    std::vector<std::wstring> versionTags; // 从原始（未净化）标题提取
};

// 总分阈值（满分 2200 = 标题 1000 + 歌手 500 + 时长 500 + 版本 200）
constexpr int kScoreThreshold = 1300;

// 对候选打分，低于阈值或触及单维度下限返回 -1：
// - 标题相似度为 0；
// - 双方歌手都已知但完全不匹配；
// - 双方时长都已知且差超过 15 秒；
// - enforceVersion 时版本标注硬冲突（本地有标注而候选无相同标注）
int scoreCandidate(const MatchQuery& q, const std::wstring& name, const std::wstring& singer,
                   int64_t candDurationMs, bool enforceVersion) {
    int t = titleSimilarity(q.title, name);
    if (t == 0)
        return -1;
    int s = singerSimilarity(q.artist, singer);
    if (!q.artist.empty() && !singer.empty() && s == 0)
        return -1;
    if (q.durationMs > 0 && candDurationMs > 0 &&
        std::llabs(static_cast<long long>(candDurationMs - q.durationMs)) > 15000)
        return -1;
    int v = versionScore(q.versionTags, name);
    if (v < 0) {
        if (enforceVersion)
            return -1;
        v = 0; // 回退轮：版本冲突仅作扣分，不淘汰
    }
    int d = durationScore(q.durationMs, candDurationMs);
    // 标题权重最高，歌手与时长次之，版本再次
    return t * 10 + s * 5 + d * 5 + v * 2;
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

// 酷狗搜歌打分选最佳：先按版本标注严格匹配（本地有标注则候选必须有相同标注），
// 无合适结果时回退到仅扣分的模糊匹配
bool kugouSearchSong(CURL* curl, const MatchQuery& q, std::string& hashOut,
                     int64_t& kugouDurMsOut) {
    std::vector<KugouSong> songs;
    if (!kugouSearchSongs(curl, stripVersionSuffix(q.title), q.artist, 5, songs))
        return false;
    for (bool enforce : {true, false}) {
        const KugouSong* best = nullptr;
        int bestScore = 0;
        for (auto& song : songs) {
            int score = scoreCandidate(q, song.name, song.singer, song.durationMs, enforce);
            if (score < kScoreThreshold || score <= bestScore)
                continue;
            bestScore = score;
            best = &song;
        }
        if (best) {
            hashOut = best->hash;
            kugouDurMsOut = best->durationMs;
            return true;
        }
    }
    return false;
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
    MatchQuery q{title, artist, durationMs, extractVersionTags(title)};
    std::string hash;
    int64_t kugouDurMs = durationMs;
    if (!kugouSearchSong(curl, q, hash, kugouDurMs))
        return false;
    // 搜词优先用本地播放时长（与正在播放的版本对齐），本地未知时才用酷狗返回的时长
    return fetchKrcByHash(curl, hash, durationMs > 0 ? durationMs : kugouDurMs, out);
}

} // namespace

struct CacheEntry {
    std::vector<LyricLine> lines;
    SongInfo info;
};

// ---------- 手动歌词按歌分文件持久化 ----------

// 文件名可读部分：替换 Windows 非法字符，截断防超长
std::wstring sanitizeFileName(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
            ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || ch < 0x20)
            out.push_back(L'_');
        else
            out.push_back(ch);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    if (out.size() > 50)
        out.resize(50);
    return out.empty() ? L"lyric" : out;
}

// FNV-1a 64 位哈希（对 key 的 UTF-8 字节），避免可读部分截断后重名
uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// 手动歌词文件名：可读部分 + key 哈希，如 "歌名 - 歌手-a1b2c3d4e5f60708.json"
std::wstring manualFileName(const std::wstring& title, const std::wstring& artist,
                            int64_t durationMs) {
    std::wstring readable = sanitizeFileName(title + L" - " + artist);
    wchar_t hex[17];
    std::swprintf(hex, 17, L"%016llx",
                  (unsigned long long)fnv1a64(toUtf8(LyricProvider::makeKey(title, artist,
                                                                          durationMs))));
    return readable + L"-" + hex + L".json";
}

struct LyricProvider::Impl {
    mutable std::mutex mtx;
    std::vector<LyricLine> current;
    SongInfo currentSongInfo;
    std::unordered_map<std::wstring, CacheEntry> cache;
    std::unordered_map<std::wstring, CacheEntry> manualOverrides; // 已加载的手动歌词（懒加载缓存）
    std::wstring overrideDir; // 手动歌词持久化目录（每首歌一个 JSON 文件），空表示不持久化
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

    // ---------- 手动歌词持久化（每首歌一个 JSON 文件，UTF-8） ----------

    // 某首歌对应的手动歌词文件路径；overrideDir 为空时返回空
    std::wstring overrideFilePath(const std::wstring& title, const std::wstring& artist,
                                  int64_t durationMs) const {
        if (overrideDir.empty())
            return {};
        return overrideDir + L"\\" + manualFileName(title, artist, durationMs);
    }

    // 从单个文件解析手动歌词；文件不存在或解析失败返回 false
    bool loadOverrideFile(const std::wstring& path, CacheEntry& entry) {
        try {
            std::ifstream f(std::filesystem::path(path), std::ios::binary);
            if (!f)
                return false;
            auto e = nlohmann::json::parse(f, nullptr, false);
            if (e.is_discarded() || !e.is_object())
                return false;
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
            return !entry.lines.empty();
        } catch (...) {
            return false;
        }
    }

    // 把一首手动歌词写入它自己的文件
    void saveOverrideFile(const std::wstring& path, const std::wstring& title,
                          const std::wstring& artist, int64_t durationMs,
                          const CacheEntry& entry) {
        try {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            nlohmann::json e;
            e["title"] = toUtf8(title);
            e["artist"] = toUtf8(artist);
            e["durationMs"] = durationMs;
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
            std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
            f << e.dump(2);
        } catch (...) {
        }
    }

    // 查找一首歌的手动歌词：先查内存，未命中再查对应文件（找到则载入内存）
    // 调用方需已持有 mtx
    bool manualOverrideGet(const std::wstring& key, const std::wstring& title,
                           const std::wstring& artist, int64_t durationMs, CacheEntry& out) {
        auto it = manualOverrides.find(key);
        if (it != manualOverrides.end()) {
            out = it->second;
            return true;
        }
        std::wstring path = overrideFilePath(title, artist, durationMs);
        if (path.empty())
            return false;
        CacheEntry entry;
        if (!loadOverrideFile(path, entry))
            return false;
        manualOverrides[key] = entry;
        out = std::move(entry);
        return true;
    }

    // 用 QQ 搜索补全 songmid/albummid（KRC 命中后调用，供封面兜底）；失败静默忽略
    void fillSongInfo(CURL* curl, const std::wstring& title, const std::wstring& artist,
                      int64_t durationMs, SongInfo& info) {
        MatchQuery q{title, artist, durationMs, extractVersionTags(title)};
        std::wstring query = stripVersionSuffix(title);
        if (!artist.empty() && !titleEndsWithArtist(title, artist))
            query += L' ' + artist;
        std::vector<Candidate> cands;
        if (!searchSongs(curl, query, cands))
            return;
        for (bool enforce : {true, false}) {
            const Candidate* best = nullptr;
            int bestScore = 0;
            for (auto& c : cands) {
                int score = scoreCandidate(q, c.name, c.singer, c.intervalMs, enforce);
                if (score < kScoreThreshold || score <= bestScore)
                    continue;
                bestScore = score;
                best = &c;
            }
            if (best) {
                info.songmid = toWide(best->songmid);
                info.albummid = toWide(best->albummid);
                return;
            }
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
        MatchQuery q{title, artist, durationMs, extractVersionTags(title)};

        // 打分合格（总分 >= 阈值且各维度不触及下限）的候选按分数降序排列；
        // 优先按版本标注严格匹配，无合适结果时回退到仅扣分的模糊匹配
        auto rankCandidates = [&](const std::vector<Candidate>& cands) {
            std::vector<std::pair<int, const Candidate*>> scored;
            for (bool enforce : {true, false}) {
                for (auto& c : cands) {
                    int score = scoreCandidate(q, c.name, c.singer, c.intervalMs, enforce);
                    if (score >= kScoreThreshold)
                        scored.push_back({score, &c});
                }
                if (!scored.empty())
                    break;
            }
            std::stable_sort(scored.begin(), scored.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            return scored;
        };

        // 依次尝试候选：第一名没有歌词（伴奏版/剧情版等）时自动试次优候选；
        // 行数过少视为无效歌词（纯音乐占位词等），继续试下一个
        constexpr int kMaxDownloadTries = 5;
        auto tryDownload = [&](const std::vector<Candidate>& cands) -> bool {
            int tries = 0;
            for (const auto& [score, c] : rankCandidates(cands)) {
                if (++tries > kMaxDownloadTries)
                    break;
                // 不带 Referer 会返回 -1310
                if (downloadLyric(curl, c->songmid, out) && out.size() >= 3) {
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
    const std::wstring key = makeKey(title, artist, durationMs);
    uint64_t gen = ++impl_->generation;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        // 1) 用户手动选择的歌词优先级最高（内存未命中时按 key 找对应文件）
        CacheEntry manual;
        if (impl_->manualOverrideGet(key, title, artist, durationMs, manual)) {
            impl_->current = std::move(manual.lines);
            impl_->currentSongInfo = std::move(manual.info);
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

std::wstring LyricProvider::makeKey(const std::wstring& title, const std::wstring& artist,
                                    int64_t durationMs) {
    // 时长按 5 秒分桶：区分同名不同版本（专辑版/Live 版等），同时容忍轻微的上报差异；
    // 时长未知固定为 -1
    return title + L'|' + artist + L'|' + std::to_wstring(durationMs > 0 ? durationMs / 5000 : -1);
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
            // QQ LRC 整行候选（最多 5 条）；与自动链路一致使用净化后的标题
            std::wstring queryTitle = stripVersionSuffix(title);
            std::wstring query = queryTitle;
            if (!artist.empty() && !titleEndsWithArtist(title, artist))
                query += L' ' + artist;
            std::vector<Candidate> cands;
            searchSongs(curl, query, cands);
            if (cands.empty())
                searchSongs(curl, queryTitle, cands);
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
                                      int64_t durationMs, std::vector<LyricLine> lines,
                                      SongInfo info) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    const std::wstring key = makeKey(title, artist, durationMs);
    auto& stored = impl_->manualOverrides[key]; // 同一首歌重复手动选择时覆盖旧记录
    stored = CacheEntry{std::move(lines), std::move(info)};
    impl_->current = stored.lines;
    impl_->currentSongInfo = stored.info;
    // 写入这首歌自己的文件
    std::wstring path = impl_->overrideFilePath(title, artist, durationMs);
    if (!path.empty())
        impl_->saveOverrideFile(path, title, artist, durationMs, stored);
}

void LyricProvider::setManualOverrideDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->overrideDir = dir;
}
