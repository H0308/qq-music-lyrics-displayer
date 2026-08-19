#include "lyric_provider.h"
#include "qrc_decoder.h"

#include "util/base64.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <windows.h>

namespace {

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
constexpr size_t kCacheCapacity = 32;

// 线程退出置位守卫：保证工作线程任何 return 路径都能被 sweepFinished 回收
struct DoneFlag {
    std::shared_ptr<std::atomic<bool>> done;
    ~DoneFlag() { done->store(true); }
};

// 进程退出标志：LyricProvider 析构前置位，curl 进度回调据此立即中断在途请求，
// 候选循环在请求之间检查并提前退出，避免退出时主线程被 join 堵在串行请求链上
//（单个请求最坏吃满 10s 超时，变体×来源×候选的串行链可达数分钟）。
std::atomic<bool> g_shutdown{false};

int curlXferAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_shutdown.load() ? 1 : 0; // 返回非 0：curl 以 CURLE_ABORTED_BY_CALLBACK 立即返回
}

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
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); // 进度回调用于退出时立即中断
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferAbort);
    out.clear();
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return rc == CURLE_OK && code == 200;
}

struct Candidate {
    std::string songmid;
    std::string songid;
    std::string albummid;
    std::wstring name;
    std::wstring singer;
    int64_t intervalMs = 0;
};

struct NeteaseSong {
    std::string id;
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
};

std::string jsonString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    return {};
}

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

// 剥掉尾部 (...)/（...)/[...]/【...】内容；用于放宽搜索词，不参与身份评分。
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

std::wstring lowerW(std::wstring value) {
    for (auto& ch : value)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

bool containsVersionWord(const std::wstring& raw, const wchar_t* needle) {
    const std::wstring text = lowerW(raw);
    const std::wstring word = lowerW(needle);
    if (text.empty() || word.empty())
        return false;

    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::wstring::npos) {
        const bool asciiWord = std::all_of(word.begin(), word.end(), [](wchar_t ch) {
            return ch < 0x80 && (std::iswalnum(ch) || ch == L'.');
        });
        const bool leftBoundary = pos == 0 || !asciiWord ||
                                  !std::iswalnum(text[pos - 1]);
        const size_t end = pos + word.size();
        const bool rightBoundary = end >= text.size() || !asciiWord ||
                                   !std::iswalnum(text[end]);
        if (leftBoundary && rightBoundary)
            return true;
        pos = end;
    }
    return false;
}

std::vector<std::wstring> versionTagsInText(const std::wstring& raw) {
    std::vector<std::wstring> tags;
    auto hasAny = [&](std::initializer_list<const wchar_t*> aliases) {
        return std::any_of(aliases.begin(), aliases.end(), [&](const wchar_t* alias) {
            return containsVersionWord(raw, alias);
        });
    };
    auto add = [&](const wchar_t* tag) {
        if (std::find(tags.begin(), tags.end(), tag) == tags.end())
            tags.emplace_back(tag);
    };

    bool specific = false;
    if (hasAny({L"live", L"现场", L"演唱会", L"concert"})) {
        add(L"live");
        specific = true;
    }
    if (hasAny({L"remix", L"混音"})) {
        add(L"remix");
        specific = true;
    }
    if (hasAny({L"acoustic", L"unplugged", L"不插电"})) {
        add(L"acoustic");
        specific = true;
    }
    if (hasAny({L"instrumental", L"karaoke", L"伴奏", L"纯音乐"})) {
        add(L"instrumental");
        specific = true;
    }
    if (hasAny({L"demo", L"小样"})) {
        add(L"demo");
        specific = true;
    }
    if (hasAny({L"simlish"})) {
        add(L"simlish");
        specific = true;
    }
    if (hasAny({L"japanese", L"japan", L"日文", L"日语", L"日本語"})) {
        add(L"japanese");
        specific = true;
    }
    if (hasAny({L"english", L"英文", L"英语"})) {
        add(L"english");
        specific = true;
    }
    if (hasAny({L"chinese", L"中文", L"中文版", L"国语", L"普通话"})) {
        add(L"chinese");
        specific = true;
    }
    if (hasAny({L"dj", L"dj版", L"剧情", L"剧情版"})) {
        add(L"special");
        specific = true;
    }
    if (hasAny({L"vma", L"award", L"颁奖", L"典礼"})) {
        add(L"award");
        specific = true;
    }
    if (hasAny({L"remastered", L"重制"})) {
        add(L"remastered");
        specific = true;
    }
    if (!specific && hasAny({L"version", L"ver.", L"edit", L"special", L"anniversary"}))
        add(L"version");
    return tags;
}

void appendUniqueVersionTags(std::vector<std::wstring>& out, const std::wstring& text) {
    for (const auto& tag : versionTagsInText(text)) {
        if (std::find(out.begin(), out.end(), tag) == out.end())
            out.push_back(tag);
    }
}

bool isBracketOpen(wchar_t ch, wchar_t& close) {
    switch (ch) {
    case L'(': close = L')'; return true;
    case L'[': close = L']'; return true;
    case L'（': close = L'）'; return true;
    case L'【': close = L'】'; return true;
    default: return false;
    }
}

bool isNoiseBracketContent(const std::wstring& content) {
    const std::wstring value = lowerW(trimW(content));
    for (const wchar_t* noise : {L"explicit", L"deluxe", L"digital", L"premium",
                                 L"album", L"edit", L"version", L"special",
                                 L"anniversary", L"studio", L"remastered"}) {
        if (value == noise)
            return true;
    }
    return false;
}

std::wstring stripFeatureSuffix(std::wstring s) {
    const std::wstring lower = lowerW(s);
    size_t cut = std::wstring::npos;
    for (const wchar_t* marker : {L" feat.", L" feat ", L" ft.", L" ft ", L" with "}) {
        size_t pos = 0;
        while ((pos = lower.find(marker, pos)) != std::wstring::npos) {
            if (pos > 0 && (pos + 1 < lower.size() || marker[0] == L' '))
                cut = cut == std::wstring::npos ? pos : std::min(cut, pos);
            pos += std::wcslen(marker);
        }
    }
    if (cut != std::wstring::npos)
        s.resize(cut);
    return trimW(s);
}

bool isMatchSeparator(wchar_t ch) {
    if (std::iswspace(ch))
        return true;
    if (ch < 0x80 && !std::isalnum(static_cast<unsigned char>(ch)))
        return true;
    switch (ch) {
    case L'，': case L'。': case L'、': case L'；': case L'：': case L'！':
    case L'？': case L'（': case L'）': case L'【': case L'】': case L'《':
    case L'》': case L'「': case L'」': case L'『': case L'』': case L'—':
    case L'–': case L'…': case L'·':
        return true;
    default:
        return false;
    }
}

// 规范化：保留普通括号内容，仅去除明确的平台噪声、特征艺人后缀、大小写和标点差异。
std::wstring normalizeTitle(const std::wstring& s) {
    const std::wstring source = stripFeatureSuffix(s);
    std::wstring out;
    out.reserve(source.size());
    bool prevSpace = true;
    for (size_t i = 0; i < source.size(); ++i) {
        wchar_t close = 0;
        if (isBracketOpen(source[i], close)) {
            const size_t end = source.find(close, i + 1);
            if (end != std::wstring::npos) {
                const std::wstring content = source.substr(i + 1, end - i - 1);
                if (!isNoiseBracketContent(content)) {
                    if (!prevSpace) {
                        out.push_back(L' ');
                        prevSpace = true;
                    }
                    for (wchar_t ch : content) {
                        if (isMatchSeparator(ch)) {
                            if (!prevSpace) {
                                out.push_back(L' ');
                                prevSpace = true;
                            }
                        } else {
                            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
                            prevSpace = false;
                        }
                    }
                    if (!prevSpace) {
                        out.push_back(L' ');
                        prevSpace = true;
                    }
                } else if (!prevSpace) {
                    out.push_back(L' ');
                    prevSpace = true;
                }
                i = end;
                continue;
            }
        }
        wchar_t ch = source[i];
        if (isMatchSeparator(ch)) {
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
    return out;
}

// 提取明确版本标记；普通括号内容不会被当作版本。
std::vector<std::wstring> extractVersionTags(const std::wstring& s) {
    std::vector<std::wstring> tags;
    auto collectBracket = [&](wchar_t open, wchar_t close) {
        size_t pos = 0;
        while ((pos = s.find(open, pos)) != std::wstring::npos) {
            const size_t end = s.find(close, pos + 1);
            if (end == std::wstring::npos)
                break;
            appendUniqueVersionTags(tags, s.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        }
    };
    collectBracket(L'(', L')');
    collectBracket(L'[', L']');
    collectBracket(L'（', L'）');
    collectBracket(L'【', L'】');

    size_t delimiter = std::wstring::npos;
    size_t delimiterLength = 0;
    for (const wchar_t* sep : {L" - ", L" – ", L" — ", L"-", L"–", L"—"}) {
        const size_t pos = s.rfind(sep);
        if (pos != std::wstring::npos && (delimiter == std::wstring::npos || pos > delimiter)) {
            delimiter = pos;
            delimiterLength = std::wcslen(sep);
        }
    }
    if (delimiter != std::wstring::npos)
        appendUniqueVersionTags(tags, s.substr(delimiter + delimiterLength));

    const std::wstring trimmed = trimW(s);
    const bool endsWithBracket = !trimmed.empty() &&
                                 (trimmed.back() == L')' || trimmed.back() == L']' ||
                                  trimmed.back() == L'）' || trimmed.back() == L'】');
    if (!endsWithBracket) {
        const size_t lastSpace = trimmed.find_last_of(L" \t\r\n");
        const std::wstring lastToken = lastSpace == std::wstring::npos
                                           ? trimmed
                                           : trimmed.substr(lastSpace + 1);
        appendUniqueVersionTags(tags, lastToken);
        if (containsVersionWord(lastToken, L"version") ||
            containsVersionWord(lastToken, L"ver.")) {
            const size_t previousSpace = lastSpace == std::wstring::npos || lastSpace == 0
                                             ? std::wstring::npos
                                             : trimmed.find_last_of(L" \t\r\n", lastSpace - 1);
            const size_t start = previousSpace == std::wstring::npos ? 0 : previousSpace + 1;
            appendUniqueVersionTags(tags, trimmed.substr(start));
        }
    }
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

// 去除特征艺人和尾部括号内容，用于放宽搜索词；不改变原始身份评分输入。
std::wstring stripVersionSuffix(std::wstring s) {
    s = stripFeatureSuffix(std::move(s));
    stripTrailingBrackets(s, nullptr);
    return trimW(s);
}

// 仅去除明确的版本分隔后缀，例如 "歌曲 - Live"；普通括号内容不触发该阶段。
std::wstring stripDelimitedVersionSuffix(const std::wstring& s) {
    size_t delimiter = std::wstring::npos;
    size_t delimiterLength = 0;
    for (const wchar_t* sep : {L" - ", L" – ", L" — "}) {
        const size_t pos = s.rfind(sep);
        if (pos != std::wstring::npos && (delimiter == std::wstring::npos || pos > delimiter)) {
            delimiter = pos;
            delimiterLength = std::wcslen(sep);
        }
    }
    if (delimiter == std::wstring::npos ||
        extractVersionTags(s.substr(delimiter + delimiterLength)).empty())
        return s;
    return trimW(s.substr(0, delimiter));
}

// Jaro-Winkler：短标题和歌手名比简单子串/前缀更适合做身份相似度。
double jaroWinkler(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return 0.0;
    if (a == b)
        return 1.0;

    const size_t maxLen = std::max(a.size(), b.size());
    const int matchDistance = std::max(0, static_cast<int>(maxLen / 2) - 1);
    std::vector<bool> aMatched(a.size(), false);
    std::vector<bool> bMatched(b.size(), false);
    size_t matches = 0;

    for (size_t i = 0; i < a.size(); ++i) {
        const size_t start = i > static_cast<size_t>(matchDistance)
                                 ? i - static_cast<size_t>(matchDistance)
                                 : 0;
        const size_t end = std::min(b.size(), i + static_cast<size_t>(matchDistance) + 1);
        for (size_t j = start; j < end; ++j) {
            if (bMatched[j] || a[i] != b[j])
                continue;
            aMatched[i] = true;
            bMatched[j] = true;
            ++matches;
            break;
        }
    }
    if (matches == 0)
        return 0.0;

    size_t transpositions = 0;
    size_t j = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!aMatched[i])
            continue;
        while (!bMatched[j])
            ++j;
        if (a[i] != b[j])
            ++transpositions;
        ++j;
    }

    const double matchCount = static_cast<double>(matches);
    double score = (matchCount / a.size() + matchCount / b.size() +
                    (matchCount - transpositions / 2.0) / matchCount) /
                   3.0;
    if (score > 0.7) {
        size_t prefix = 0;
        while (prefix < std::min({a.size(), b.size(), static_cast<size_t>(4)}) &&
               a[prefix] == b[prefix])
            ++prefix;
        score += static_cast<double>(prefix) * 0.1 * (1.0 - score);
    }
    return std::clamp(score, 0.0, 1.0);
}

// 0-100，100 表示完全匹配；低于 50 分视为标题明显不一致。
int titleSimilarity(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = normalizeTitle(a);
    const std::wstring nb = normalizeTitle(b);
    if (na.empty() || nb.empty())
        return 0;
    const double score = jaroWinkler(na, nb);
    if (score < 0.50)
        return 0;
    return static_cast<int>(std::lround(score * 100.0));
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

// 在原始文本上拆分歌手（normalizeTitle 会把 / 、 等分隔符转为空格，必须先拆分再规范化），
// 并对每个拆分结果展开 feat./ft./with/vs 等合作标注；返回的每个 token 均已规范化。
std::vector<std::wstring> splitArtists(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (const auto& part : splitSingers(s)) {
        std::wstring n = L' ' + normalizeTitle(part) + L' ';
        for (const wchar_t* kw : {L" feat. ", L" feat ", L" ft. ", L" ft ", L" featuring ",
                                  L" with ", L" vs ", L" vs. "})
            replaceAllW(n, kw, L" / ");
        for (const auto& p : splitSingers(n)) {
            auto t = trimW(p);
            if (!t.empty())
                out.push_back(t);
        }
    }
    return out;
}

int singerSimilarity(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return 0;
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
    auto pa = splitArtists(a);
    auto pb = splitArtists(b);
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
    const int tokenScore =
        std::min(100, matches * 100 / static_cast<int>(std::max(pa.size(), pb.size())));
    const int textScore = static_cast<int>(std::lround(jaroWinkler(na, nb) * 100.0));
    return std::max(tokenScore, textScore);
}

double durationSimilarity(int64_t queryMs, int64_t candMs) {
    if (queryMs <= 0 || candMs <= 0)
        return 0.0;
    int64_t diff = std::llabs(static_cast<long long>(candMs - queryMs));
    if (diff <= 1000)
        return 1.0;
    if (diff >= 10000)
        return 0.0;
    return 1.0 - static_cast<double>(diff - 1000) / 9000.0;
}

// ---------- 统一候选打分 ----------

bool sameVersionTags(std::vector<std::wstring> left, std::vector<std::wstring> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

// 一次匹配请求的本地歌曲信息
struct MatchQuery {
    std::wstring title;
    std::wstring artist;
    int64_t durationMs = 0;
    std::vector<std::wstring> versionTags; // 从原始（未净化）标题提取
};

struct SearchVariant {
    std::wstring title;
    std::wstring artist;
};

void addSearchVariant(std::vector<SearchVariant>& variants, std::wstring title,
                      std::wstring artist) {
    title = trimW(title);
    artist = trimW(artist);
    if (title.empty())
        return;
    for (const auto& existing : variants) {
        if (existing.title == title && existing.artist == artist)
            return;
    }
    variants.push_back({std::move(title), std::move(artist)});
}

std::wstring primaryArtist(const std::wstring& artist) {
    // 合作标注统一为分隔符后先尝试拆分（normalizeTitle 会保留替换进来的 /）
    std::wstring normalized = normalizeTitle(artist);
    for (const wchar_t* kw : {L" feat ", L" ft ", L" featuring ", L" with ", L" vs "})
        replaceAllW(normalized, kw, L"/");
    const auto collab = splitSingers(normalized);
    if (collab.size() > 1)
        return collab.front();
    // 原始文本上的 / 、 等分隔（如酷狗 "S.H.E、飞轮海"、QQ "S.H.E/飞轮海"），
    // 取第一个歌手并保留原始写法（如 "S.H.E" 中的点号）
    const auto raw = splitSingers(artist);
    if (!raw.empty())
        return trimW(raw.front());
    return trimW(artist);
}

std::vector<SearchVariant> buildSearchVariants(const MatchQuery& q) {
    std::vector<SearchVariant> variants;
    addSearchVariant(variants, q.title, q.artist); // exact
    addSearchVariant(variants, normalizeTitle(q.title), normalizeTitle(q.artist));
    addSearchVariant(variants, q.title, primaryArtist(q.artist));
    addSearchVariant(variants, stripVersionSuffix(q.title), q.artist);
    addSearchVariant(variants, stripDelimitedVersionSuffix(q.title), q.artist);
    return variants;
}

std::wstring makeSearchQuery(const SearchVariant& variant) {
    std::wstring query = variant.title;
    if (!variant.artist.empty() && !titleEndsWithArtist(variant.title, variant.artist)) {
        if (!query.empty())
            query += L' ';
        query += variant.artist;
    }
    return query;
}

// 录用阈值：标题/歌手/时长加权后的 0~100 分。
constexpr int kScoreThreshold = 80;

bool isUnknownTitle(const std::wstring& value) {
    const std::wstring normalized = lowerW(trimW(value));
    return normalized.empty() || normalized == L"unknown title";
}

bool isUsefulArtist(const std::wstring& value) {
    const std::wstring normalized = lowerW(trimW(value));
    return !normalized.empty() && normalized != L"unknown artist" && normalized != L"未知歌手";
}

// 先执行硬拒绝，再按可用字段计算 0~100 分：标题 50、歌手 30、时长 20。
int scoreCandidate(const MatchQuery& q, const std::wstring& name, const std::wstring& singer,
                   int64_t candDurationMs) {
    if (isUnknownTitle(q.title) || isUnknownTitle(name))
        return 0;

    int t = titleSimilarity(q.title, name);
    if (t == 0)
        return 0;
    const bool hasArtist = isUsefulArtist(q.artist) && isUsefulArtist(singer);
    int s = hasArtist ? singerSimilarity(q.artist, singer) : 0;
    if (hasArtist && s == 0)
        return 0;
    if (q.durationMs > 0 && candDurationMs > 0 &&
        std::llabs(static_cast<long long>(candDurationMs - q.durationMs)) > 15000)
        return 0;
    if (!sameVersionTags(q.versionTags, extractVersionTags(name)))
        return 0;

    const bool hasDuration = q.durationMs > 0 && candDurationMs > 0;
    const double titleScore = t / 100.0;
    const double artistScore = hasArtist ? s / 100.0 : 0.0;
    const double durationScore = hasDuration ? durationSimilarity(q.durationMs, candDurationMs)
                                             : 0.0;
    double total = titleScore;
    if (hasArtist && hasDuration)
        total = titleScore * 0.50 + artistScore * 0.30 + durationScore * 0.20;
    else if (hasArtist)
        total = titleScore * 0.60 + artistScore * 0.40;
    else if (hasDuration)
        total = titleScore * 0.75 + durationScore * 0.25;
    return static_cast<int>(std::lround(std::clamp(total, 0.0, 1.0) * 100.0));
}

void deduplicateCandidates(std::vector<Candidate>& candidates) {
    std::unordered_set<std::string> seen;
    std::vector<Candidate> unique;
    unique.reserve(candidates.size());
    for (auto& candidate : candidates) {
        const std::string id = !candidate.songmid.empty()
                                    ? "mid:" + candidate.songmid
                                    : "id:" + candidate.songid;
        if (candidate.songmid.empty() && candidate.songid.empty())
            continue;
        if (!seen.insert(id).second)
            continue;
        unique.push_back(std::move(candidate));
    }
    candidates = std::move(unique);
}

std::vector<std::pair<int, const Candidate*>> rankCandidates(const MatchQuery& q,
                                                              std::vector<Candidate>& candidates) {
    deduplicateCandidates(candidates);
    std::vector<std::pair<int, const Candidate*>> ranked;
    for (const auto& candidate : candidates) {
        const int score =
            scoreCandidate(q, candidate.name, candidate.singer, candidate.intervalMs);
        if (score >= kScoreThreshold)
            ranked.push_back({score, &candidate});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    return ranked;
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

bool parseIntegerField(const std::string& value, int64_t& out) {
    if (value.empty())
        return false;
    char* end = nullptr;
    long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end != value.c_str() + value.size())
        return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

// 解析网易云 YRC 文本（[行开始,行时长](字开始,字时长,0)文本）。
// YRC 中每个括号后的文本块可以是一个字，也可以是一个词，LyricChar 均可承载。
std::vector<LyricLine> parseYrc(std::string content) {
    if (!content.empty() && content.compare(0, 3, "\xEF\xBB\xBF") == 0)
        content.erase(0, 3); // 去 BOM

    std::vector<LyricLine> lines;
    std::istringstream ss(content);
    std::string raw;
    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        size_t begin = raw.find_first_not_of(" \t");
        if (begin == std::string::npos || raw[begin] != '[')
            continue; // YRC 可能包含 JSON 信息行

        size_t close = raw.find(']', begin + 1);
        if (close == std::string::npos)
            continue;
        std::string header = raw.substr(begin + 1, close - begin - 1);
        size_t comma = header.find(',');
        if (comma == std::string::npos) {
            // 个别接口响应会在 yrc 字段中混入普通 LRC 行，保留它们作为非逐字行。
            int64_t lineStart = 0;
            if (!parseTimeStamp(header, lineStart))
                continue;
            std::wstring plain = toWide(raw.substr(close + 1));
            while (!plain.empty() && (plain.back() == L' ' || plain.back() == L'\t'))
                plain.pop_back();
            if (!plain.empty())
                lines.push_back({lineStart, std::move(plain)});
            continue;
        }
        int64_t lineStart = 0;
        int64_t lineDuration = 0;
        if (!parseIntegerField(header.substr(0, comma), lineStart) ||
            !parseIntegerField(header.substr(comma + 1), lineDuration) || lineStart < 0)
            continue;
        (void)lineDuration;

        std::vector<LyricChar> chars;
        std::wstring text;
        size_t pos = close + 1;
        while (pos < raw.size() && raw[pos] == '(') {
            size_t tokenClose = raw.find(')', pos + 1);
            if (tokenClose == std::string::npos)
                break;
            std::string token = raw.substr(pos + 1, tokenClose - pos - 1);
            size_t comma1 = token.find(',');
            size_t comma2 = comma1 == std::string::npos ? comma1 : token.find(',', comma1 + 1);
            if (comma2 == std::string::npos)
                break;

            int64_t charOffset = 0;
            int64_t charDuration = 0;
            if (!parseIntegerField(token.substr(0, comma1), charOffset) ||
                !parseIntegerField(token.substr(comma1 + 1, comma2 - comma1 - 1), charDuration))
                break;

            size_t nextToken = std::string::npos;
            for (size_t candidate = raw.find('(', tokenClose + 1);
                 candidate != std::string::npos;
                 candidate = raw.find('(', candidate + 1)) {
                size_t candidateClose = raw.find(')', candidate + 1);
                if (candidateClose == std::string::npos)
                    break;
                std::string candidateToken =
                    raw.substr(candidate + 1, candidateClose - candidate - 1);
                size_t candidateComma1 = candidateToken.find(',');
                size_t candidateComma2 = candidateComma1 == std::string::npos
                                             ? candidateComma1
                                             : candidateToken.find(',', candidateComma1 + 1);
                int64_t ignoredStart = 0;
                int64_t ignoredDuration = 0;
                if (candidateComma2 != std::string::npos &&
                    parseIntegerField(candidateToken.substr(0, candidateComma1),
                                      ignoredStart) &&
                    parseIntegerField(candidateToken.substr(candidateComma1 + 1,
                                                             candidateComma2 - candidateComma1 - 1),
                                      ignoredDuration)) {
                    nextToken = candidate;
                    break;
                }
            }
            std::string tokenText = raw.substr(
                tokenClose + 1,
                nextToken == std::string::npos ? std::string::npos : nextToken - tokenClose - 1);
            std::wstring tokenWide = toWide(tokenText);
            if (!tokenWide.empty()) {
                // YRC 的字时间戳是相对整首歌曲的绝对时间；例如行首和首字通常相同。
                int64_t start = charOffset;
                int64_t end = start + std::max<int64_t>(charDuration, 1);
                chars.push_back({start, end, tokenWide});
                text += tokenWide;
            }
            if (nextToken == std::string::npos)
                break;
            pos = nextToken;
        }

        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            text.pop_back();
        if (text.empty() || chars.empty())
            continue;
        lines.push_back({lineStart, std::move(text), std::move(chars)});
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
        if (s.contains("id"))
            c.songid = jsonString(s["id"]);
        if (c.songid.empty() && s.contains("songid"))
            c.songid = jsonString(s["songid"]);
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
        if (!c.songmid.empty() || !c.songid.empty())
            out.push_back(std::move(c));
    }
    return true;
}

// 按关键词搜索网易云单曲，填充 out；返回 true 表示 HTTP/JSON 解析成功。
bool searchNeteaseSongs(CURL* curl, const std::wstring& query, int maxN,
                        std::vector<NeteaseSong>& out) {
    out.clear();
    std::string keyword = toUtf8(query);
    char* esc = curl_easy_escape(curl, keyword.c_str(), static_cast<int>(keyword.size()));
    std::string url = "https://music.163.com/api/cloudsearch/pc?csrf_token=&s=" +
                      std::string(esc ? esc : "") + "&type=1&offset=0&limit=" +
                      std::to_string(maxN);
    if (esc)
        curl_free(esc);

    std::string body;
    if (!httpGet(curl, url, "https://music.163.com/", body))
        return false;
    auto root = nlohmann::json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;
    auto resultIt = root.find("result");
    if (resultIt == root.end() || !resultIt->is_object())
        return false;
    auto songsIt = resultIt->find("songs");
    if (songsIt == resultIt->end() || !songsIt->is_array())
        return false;

    for (const auto& item : *songsIt) {
        if (!item.is_object())
            continue;
        NeteaseSong song;
        auto idIt = item.find("id");
        if (idIt != item.end())
            song.id = jsonString(*idIt);
        song.name = toWide(item.value("name", std::string()));
        if (song.name.empty() || song.id.empty())
            continue;

        auto artistsIt = item.find("ar");
        if (artistsIt != item.end() && artistsIt->is_array()) {
            for (const auto& artist : *artistsIt) {
                if (!artist.is_object())
                    continue;
                const std::wstring name = toWide(artist.value("name", std::string()));
                if (!name.empty()) {
                    if (!song.singer.empty())
                        song.singer += L'/';
                    song.singer += name;
                }
            }
        }
        song.durationMs = static_cast<int64_t>(item.value("dt", 0LL));
        if (song.durationMs <= 0)
            song.durationMs = static_cast<int64_t>(item.value("duration", 0LL));
        out.push_back(std::move(song));
        if (static_cast<int>(out.size()) >= maxN)
            break;
    }
    return true;
}

// 将独立 LRC 时间轴附着到最近的主歌词行。QQ 的 QRC 与 LRC 起始时间偶有少量偏差，
// 因此不能只按完全相同的毫秒值匹配；超过 1500ms 则视为不同歌词行。
void attachSecondary(std::vector<LyricLine>& mainLines, const std::vector<LyricLine>& secondary,
                     bool translation) {
    if (mainLines.empty() || secondary.empty())
        return;
    size_t cursor = 0;
    for (const auto& extra : secondary) {
        while (cursor + 1 < mainLines.size() &&
               std::llabs(mainLines[cursor + 1].ms - extra.ms) <=
                   std::llabs(mainLines[cursor].ms - extra.ms))
            ++cursor;
        if (std::llabs(mainLines[cursor].ms - extra.ms) > 1500)
            continue;
        std::wstring& target = translation ? mainLines[cursor].translation
                                           : mainLines[cursor].romanization;
        if (target.empty())
            target = extra.text;
    }
}

std::string xmlInnerText(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag;
    size_t begin = xml.find(open);
    if (begin == std::string::npos) return {};
    begin = xml.find('>', begin + open.size());
    if (begin == std::string::npos) return {};
    ++begin;
    const std::string close = "</" + tag + ">";
    size_t end = xml.find(close, begin);
    if (end == std::string::npos) return {};
    std::string text = xml.substr(begin, end - begin);
    if (text.compare(0, 9, "<![CDATA[") == 0 && text.size() >= 12 &&
        text.compare(text.size() - 3, 3, "]]>") == 0)
        text = text.substr(9, text.size() - 12);
    return text;
}

// QQ 音乐客户端原生逐字歌词：XML content 字段为十六进制 3DES+zlib QRC。
bool downloadQrc(CURL* curl, const std::string& songid, std::vector<LyricLine>& out) {
    if (songid.empty()) {
        return false;
    }
    std::string body;
    std::string url = "https://c.y.qq.com/qqmusic/fcgi-bin/lyric_download.fcg"
                      "?musicid=" + songid + "&version=15&miniversion=82&lrctype=4";
    if (!httpGet(curl, url, "https://c.y.qq.com/", body)) {
        return false;
    }
    std::string encrypted = xmlInnerText(body, "content");
    if (encrypted.empty()) {
        return false;
    }
    return decodeQrcLyrics(encrypted, out);
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
    const std::string trans = base64Decode(j.value("trans", std::string()));
    const std::string roma = base64Decode(j.value("roma", std::string()));
    if (!trans.empty())
        attachSecondary(out, parseLrc(trans), true);
    if (!roma.empty())
        attachSecondary(out, parseLrc(roma), false);
    return !out.empty();
}

std::string lyricField(const nlohmann::json& root, const char* name) {
    if (!root.is_object())
        return {};
    const auto it = root.find(name);
    if (it == root.end() || !it->is_object())
        return {};
    return it->value("lyric", std::string());
}

// 按网易云歌曲 ID 下载歌词；YRC 不可用时回退同一响应中的 LRC。
bool downloadNeteaseLyric(CURL* curl, const std::wstring& songId,
                          std::vector<LyricLine>& out) {
    if (songId.empty())
        return false;

    std::string body;
    const std::string id = toUtf8(songId);
    const std::string url = "https://music.163.com/api/song/lyric?id=" + id +
                            "&lv=1&kv=1&tv=1&yv=1&ytv=1&rv=1&yrv=1";
    if (!httpGet(curl, url, "https://music.163.com/", body))
        return false;

    auto root = nlohmann::json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;

    std::vector<LyricLine> lines;
    const std::string yrc = lyricField(root, "yrc");
    if (!yrc.empty())
        lines = parseYrc(yrc);
    if (lines.empty()) {
        const std::string lrc = lyricField(root, "lrc");
        if (lrc.empty())
            return false;
        lines = parseLrc(lrc);
    }
    if (lines.empty())
        return false;

    // YRC 主歌词也可以附着网易云返回的翻译/罗马音；新字段优先，
    // 旧字段为空时使用兼容字段。
    std::string translation = lyricField(root, "ytlrc");
    if (translation.empty())
        translation = lyricField(root, "tlyric");
    std::string romanization = lyricField(root, "yromalrc");
    if (romanization.empty())
        romanization = lyricField(root, "romalrc");
    if (!translation.empty()) {
        auto translatedLines = parseYrc(translation);
        if (translatedLines.empty())
            translatedLines = parseLrc(translation);
        attachSecondary(lines, translatedLines, true);
    }
    if (!romanization.empty()) {
        auto romanizedLines = parseYrc(romanization);
        if (romanizedLines.empty())
            romanizedLines = parseLrc(romanization);
        attachSecondary(lines, romanizedLines, false);
    }

    out = std::move(lines);
    return true;
}

// QRC/KRC 提供主逐字时间轴时，仍从 QQ LRC 接口补充同歌曲的翻译与罗马音。
void attachQqSecondary(CURL* curl, const std::string& songmid, std::vector<LyricLine>& out) {
    if (songmid.empty() || out.empty())
        return;
    std::vector<LyricLine> qqLines;
    if (!downloadLyric(curl, songmid, qqLines))
        return;
    std::vector<LyricLine> trans;
    std::vector<LyricLine> roma;
    for (const auto& line : qqLines) {
        if (!line.translation.empty())
            trans.push_back({line.ms, line.translation});
        if (!line.romanization.empty())
            roma.push_back({line.ms, line.romanization});
    }
    attachSecondary(out, trans, true);
    attachSecondary(out, roma, false);
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
    std::string languagePayload;
    std::istringstream ss(content);
    std::string raw;
    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.compare(0, 10, "[language:") == 0 && raw.size() > 11 && raw.back() == ']') {
            languagePayload = raw.substr(10, raw.size() - 11);
            continue;
        }
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
    // KRC 的 language 元数据是 Base64 JSON；type=0 为音译，type=1 为翻译，
    // lyricContent 按主歌词行号对应，不另带时间戳。
    if (!languagePayload.empty()) {
        auto lang = nlohmann::json::parse(base64Decode(languagePayload), nullptr, false);
        if (!lang.is_discarded() && lang["content"].is_array()) {
            for (const auto& block : lang["content"]) {
                int type = block.value("type", -1);
                if (type != 0 && type != 1)
                    continue;
                const auto& contentLines = block["lyricContent"];
                if (!contentLines.is_array())
                    continue;
                size_t count = std::min(lines.size(), contentLines.size());
                for (size_t i = 0; i < count; ++i) {
                    if (!contentLines[i].is_array())
                        continue;
                    std::wstring text;
                    for (const auto& token : contentLines[i]) {
                        if (!token.is_string())
                            continue;
                        std::wstring part = toWide(token.get<std::string>());
                        if (type == 0 && !text.empty() && !part.empty() && text.back() != L' ' &&
                            part.front() != L' ')
                            text += L' ';
                        text += part;
                    }
                    text = trimW(text);
                    if (type == 0)
                        lines[i].romanization = std::move(text);
                    else
                        lines[i].translation = std::move(text);
                }
            }
        }
    }
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
                      std::string(esc ? esc : "") + "&page=1&pagesize=" +
                      std::to_string(std::max(maxN, 1)) + "&showtype=1";
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

void deduplicateKugouCandidates(std::vector<KugouSong>& candidates) {
    std::unordered_set<std::string> seen;
    std::vector<KugouSong> unique;
    unique.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (candidate.hash.empty() || !seen.insert(candidate.hash).second)
            continue;
        unique.push_back(std::move(candidate));
    }
    candidates = std::move(unique);
}

std::vector<std::pair<int, const KugouSong*>> rankKugouCandidates(
    const MatchQuery& q, std::vector<KugouSong>& candidates) {
    deduplicateKugouCandidates(candidates);
    std::vector<std::pair<int, const KugouSong*>> ranked;
    for (const auto& candidate : candidates) {
        const int score = scoreCandidate(q, candidate.name, candidate.singer, candidate.durationMs);
        if (score >= kScoreThreshold)
            ranked.push_back({score, &candidate});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    return ranked;
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
        out.clear();
        if (kugouDownloadKrc(curl, id, accesskey, out) && !out.empty())
            return true;
    }
    return false;
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

// 手动歌词文件名：平台前缀 + 可读部分 + 来源键哈希，
// 如 "qq-歌名 - 歌手-a1b2c3d4e5f60708.json"。
std::wstring manualFileName(const std::wstring& title, const std::wstring& artist,
                            const std::wstring& key, bool netease) {
    std::wstring readable = sanitizeFileName(title + L" - " + artist);
    wchar_t hex[17];
    std::swprintf(hex, 17, L"%016llx", (unsigned long long)fnv1a64(toUtf8(key)));
    return std::wstring(netease ? L"netease-" : L"qq-") + readable + L"-" + hex + L".json";
}

// 旧版本文件名没有平台前缀；保留此函数用于读取已有手动歌词。
std::wstring legacyManualFileName(const std::wstring& title, const std::wstring& artist,
                                  const std::wstring& key) {
    std::wstring readable = sanitizeFileName(title + L" - " + artist);
    wchar_t hex[17];
    std::swprintf(hex, 17, L"%016llx", (unsigned long long)fnv1a64(toUtf8(key)));
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

    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };
    std::vector<Worker> workers;

    ~Impl() {
        // 先置退出标志：在途 curl 请求经进度回调立即中断，候选循环提前退出
        g_shutdown.store(true);
        for (auto& w : workers)
            if (w.thread.joinable()) w.thread.join();
    }

    // 回收已结束的线程：在每次新请求入口顺手清理，向量规模稳定在并发中的请求数
    void sweepFinished() {
        std::erase_if(workers, [](Worker& w) {
            if (w.done->load()) {
                w.thread.join();
                return true;
            }
            return false;
        });
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
    std::wstring overrideFilePath(const std::wstring& key, const std::wstring& title,
                                  const std::wstring& artist, bool netease,
                                  bool legacy) const {
        if (overrideDir.empty())
            return {};
        const std::wstring fileName = legacy
                                          ? legacyManualFileName(title, artist, key)
                                          : manualFileName(title, artist, key, netease);
        return overrideDir + L"\\" + fileName;
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
            entry.info.neteaseSongId = toWide(e.value("neteaseSongId", std::string()));
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
                    // 第四、五项分别为翻译和罗马音；旧文件缺失时保持为空。
                    if (l.size() >= 4 && l[3].is_string())
                        line.translation = toWide(l[3].get<std::string>());
                    if (l.size() >= 5 && l[4].is_string())
                        line.romanization = toWide(l[4].get<std::string>());
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
            e["neteaseSongId"] = toUtf8(entry.info.neteaseSongId);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& l : entry.lines) {
                nlohmann::json chars = nlohmann::json::array();
                for (const auto& c : l.chars)
                    chars.push_back({c.startMs, c.endMs, toUtf8(c.text)});
                arr.push_back({l.ms, toUtf8(l.text), std::move(chars), toUtf8(l.translation),
                               toUtf8(l.romanization)});
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
                           const std::wstring& artist, bool netease, CacheEntry& out) {
        auto it = manualOverrides.find(key);
        if (it != manualOverrides.end()) {
            out = it->second;
            return true;
        }
        std::wstring path = overrideFilePath(key, title, artist, netease, false);
        if (path.empty())
            return false;
        CacheEntry entry;
        if (!loadOverrideFile(path, entry)) {
            // 兼容改名前已经存在的无前缀文件；新保存的文件只使用带前缀路径。
            const std::wstring legacyPath = overrideFilePath(key, title, artist, netease, true);
            if (!loadOverrideFile(legacyPath, entry))
                return false;
        }
        manualOverrides[key] = entry;
        out = std::move(entry);
        return true;
    }

    // 用 QQ 搜索补全 songmid/albummid（KRC 命中后调用，供封面兜底）；失败静默忽略
    void fillSongInfo(CURL* curl, const std::wstring& title, const std::wstring& artist,
                      int64_t durationMs, SongInfo& info) {
        MatchQuery q{title, artist, durationMs, extractVersionTags(title)};
        for (const auto& variant : buildSearchVariants(q)) {
            std::vector<Candidate> cands;
            if (!searchSongs(curl, makeSearchQuery(variant), cands))
                continue;
            const auto ranked = rankCandidates(q, cands);
            if (!ranked.empty()) {
                const Candidate* best = ranked.front().second;
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
        if (!curl)
            return false;

        MatchQuery q{title, artist, durationMs, extractVersionTags(title)};

        const std::vector<SearchVariant> variants = buildSearchVariants(q);
        auto tryKugou = [&]() -> bool {
            for (const auto& variant : variants) {
                if (g_shutdown.load())
                    return false;
                std::vector<KugouSong> songs;
                if (!kugouSearchSongs(curl, variant.title, variant.artist, 10, songs))
                    continue;
                const auto ranked = rankKugouCandidates(q, songs);
                if (ranked.empty())
                    continue;
                for (const auto& [score, song] : ranked) {
                    if (g_shutdown.load())
                        return false;
                    out.clear();
                    const int64_t lyricDuration = q.durationMs > 0 ? q.durationMs
                                                                    : song->durationMs;
                    if (fetchKrcByHash(curl, song->hash, lyricDuration, out) && !out.empty()) {
                        fillSongInfo(curl, title, artist, durationMs, info);
                        if (!info.songmid.empty())
                            attachQqSecondary(curl, toUtf8(info.songmid), out);
                        return true;
                    }
                }
            }
            return false;
        };

        auto tryQq = [&](LyricSource source) -> bool {
            for (const auto& variant : variants) {
                if (g_shutdown.load())
                    return false;
                std::vector<Candidate> cands;
                if (!searchSongs(curl, makeSearchQuery(variant), cands))
                    continue;
                const auto ranked = rankCandidates(q, cands);
                if (ranked.empty())
                    continue;
                for (const auto& [score, candidate] : ranked) {
                    if (g_shutdown.load())
                        return false;
                    if (source == LyricSource::Qrc && candidate->songid.empty())
                        continue;
                    if (source == LyricSource::Lrc && candidate->songmid.empty())
                        continue;
                    out.clear();
                    const bool downloaded = source == LyricSource::Qrc
                                                ? downloadQrc(curl, candidate->songid, out)
                                                : downloadLyric(curl, candidate->songmid, out);
                    if (downloaded && !out.empty()) {
                        if (source == LyricSource::Qrc)
                            attachQqSecondary(curl, candidate->songmid, out);
                        info.songmid = toWide(candidate->songmid);
                        info.albummid = toWide(candidate->albummid);
                        return true;
                    }
                }
            }
            return false;
        };

        // 保持现有来源优先级：KRC -> QQ QRC -> QQ LRC。
        if (tryKugou() || tryQq(LyricSource::Qrc) || tryQq(LyricSource::Lrc)) {
            curl_easy_cleanup(curl);
            return true;
        }
        curl_easy_cleanup(curl);
        return false;
    }

    bool fetchNetease(const std::wstring& songId, std::vector<LyricLine>& out,
                     SongInfo& info) {
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;
        bool ok = downloadNeteaseLyric(curl, songId, out);
        curl_easy_cleanup(curl);
        if (ok) {
            info = SongInfo{}; // 网易云歌词不使用 QQ songmid/albummid 封面兜底
            info.neteaseSongId = songId;
        }
        return ok;
    }
};

LyricProvider::LyricProvider() : impl_(std::make_unique<Impl>()) {
    g_shutdown.store(false); // 防御性复位：退出流程只会置位一次
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
        if (impl_->manualOverrideGet(key, title, artist, false, manual)) {
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
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, gen, key, title, artist, durationMs, cb = std::move(cb),
                                 done]() mutable {
        DoneFlag flag{done};
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
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::requestNeteaseAsync(const std::wstring& songId,
                                       const std::wstring& title,
                                       const std::wstring& artist,
                                       int64_t durationMs, ReadyCallback cb) {
    if (songId.empty()) {
        if (cb)
            cb(false);
        return;
    }

    // 网易云使用歌曲 ID 作为缓存键，避免同名歌曲复用 QQ 或其他网易云歌曲的歌词。
    const std::wstring key = L"netease|" + songId;
    uint64_t gen = ++impl_->generation;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        // 网易云手动选择按歌曲 ID 保存，优先级高于网络缓存。
        CacheEntry manual;
        if (impl_->manualOverrideGet(key, title, artist, true, manual)) {
            impl_->current = std::move(manual.lines);
            impl_->currentSongInfo = std::move(manual.info);
            if (cb)
                cb(true);
            return;
        }
        CacheEntry cached;
        if (impl_->cacheGet(key, cached)) {
            impl_->current = std::move(cached.lines);
            impl_->currentSongInfo = std::move(cached.info);
            if (cb)
                cb(true);
            return;
        }
    }

    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, gen, key, songId, cb = std::move(cb), done]() mutable {
        DoneFlag flag{done};
        std::vector<LyricLine> result;
        SongInfo info;
        bool ok = impl->fetchNetease(songId, result, info);
        if (ok) {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation == gen) {
                impl->current = result;
                impl->currentSongInfo = info;
                impl->cachePut(key, CacheEntry{std::move(result), std::move(info)});
                if (cb)
                    cb(true);
            }
        } else if (impl->generation == gen && cb) {
            cb(false);
        }
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
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
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, title, artist, cb = std::move(cb), done]() mutable {
        DoneFlag flag{done};
        CURL* curl = curl_easy_init();
        std::vector<SearchCandidate> result;
        if (curl) {
            std::wstring queryTitle = stripVersionSuffix(title);
            std::wstring query = queryTitle;
            if (!artist.empty() && !titleEndsWithArtist(title, artist))
                query += L' ' + artist;

            // 网易云候选按歌曲 ID 获取歌词；同一响应内优先 YRC，不可用时由歌词接口回退 LRC。
            std::vector<NeteaseSong> neteaseSongs;
            bool neteaseSearchOk = searchNeteaseSongs(curl, query, 5, neteaseSongs);
            if (neteaseSongs.empty())
                neteaseSearchOk = searchNeteaseSongs(curl, queryTitle, 5, neteaseSongs);
            if (neteaseSearchOk) {
                for (const auto& s : neteaseSongs) {
                    result.push_back({{}, {}, {}, {}, s.name, s.singer, s.durationMs,
                                      LyricSource::Yrc, toWide(s.id)});
                }
            }

            // QQ 候选同时提供 QRC 逐字与 LRC 整行入口。
            std::vector<Candidate> cands;
            searchSongs(curl, query, cands);
            if (cands.empty())
                searchSongs(curl, queryTitle, cands);
            for (size_t i = 0; i < cands.size() && i < 5; ++i) {
                const auto& c = cands[i];
                if (!c.songid.empty())
                    result.push_back({toWide(c.songmid), toWide(c.songid), toWide(c.albummid), {},
                                      c.name, c.singer, c.intervalMs, LyricSource::Qrc});
            }

            std::vector<KugouSong> kugouSongs;
            if (kugouSearchSongs(curl, queryTitle, artist, 5, kugouSongs)) {
                for (auto& s : kugouSongs) {
                    result.push_back({{}, {}, {}, toWide(s.hash), s.name, s.singer, s.durationMs,
                                      LyricSource::Krc});
                }
            }
            for (size_t i = 0; i < cands.size() && i < 5; ++i) {
                const auto& c = cands[i];
                result.push_back({toWide(c.songmid), toWide(c.songid), toWide(c.albummid), {},
                                  c.name, c.singer, c.intervalMs, LyricSource::Lrc});
            }
            curl_easy_cleanup(curl);
        }
        if (cb)
            cb(std::move(result));
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::fetchLyricAsync(const SearchCandidate& cand, FetchCallback cb) {
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, cand, cb = std::move(cb), done]() mutable {
        DoneFlag flag{done};
        CURL* curl = curl_easy_init();
        std::vector<LyricLine> lines;
        bool ok = false;
        if (curl) {
            if (cand.source == LyricSource::Qrc)
                ok = downloadQrc(curl, toUtf8(cand.songid), lines);
            else if (cand.source == LyricSource::Krc)
                ok = fetchKrcByHash(curl, toUtf8(cand.kugouHash), cand.durationMs, lines);
            else if (cand.source == LyricSource::Yrc)
                ok = downloadNeteaseLyric(curl, cand.neteaseSongId, lines);
            else
                ok = downloadLyric(curl, toUtf8(cand.songmid), lines);
            if (ok && cand.source != LyricSource::Lrc && !cand.songmid.empty())
                attachQqSecondary(curl, toUtf8(cand.songmid), lines);
            curl_easy_cleanup(curl);
        }
        SongInfo info{cand.songmid, cand.albummid, cand.neteaseSongId};
        if (cb)
            cb(ok, std::move(lines), info);
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::setManualOverride(const std::wstring& title, const std::wstring& artist,
                                      int64_t durationMs, std::vector<LyricLine> lines,
                                      SongInfo info) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    const bool netease = !info.neteaseSongId.empty();
    const std::wstring key = netease
                                 ? L"netease|" + info.neteaseSongId
                                 : makeKey(title, artist, durationMs);
    auto& stored = impl_->manualOverrides[key]; // 同一首歌重复手动选择时覆盖旧记录
    stored = CacheEntry{std::move(lines), std::move(info)};
    impl_->current = stored.lines;
    impl_->currentSongInfo = stored.info;
    // 写入这首歌自己的文件
    std::wstring path = impl_->overrideFilePath(key, title, artist, netease, false);
    if (!path.empty())
        impl_->saveOverrideFile(path, title, artist, durationMs, stored);
}

void LyricProvider::setManualOverrideDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->overrideDir = dir;
}
