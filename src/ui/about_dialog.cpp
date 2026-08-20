#include "about_dialog.h"

#include "app_info.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "resource.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kIdInfoCard = 301;
constexpr int kIdVersionLabel = 302;
constexpr int kIdReleaseTitleLabel = 303;
constexpr int kIdProjectButton = 304;
constexpr int kIdStatusLabel = 305;
constexpr int kIdLatestLabel = 306;
constexpr int kIdCheckButton = 307;
constexpr int kIdReleaseButton = 308;
constexpr int kIdCloseButton = 309;
constexpr int kIdTitleLabel = 310;
constexpr int kIdSubtitleLabel = 311;
constexpr int kIdReleaseNotes = 312;
constexpr int kIdAutoCheckLabel = 313;
constexpr int kIdAutoCheckSwitch = 314;

constexpr UINT kMsgUpdateReady = WM_APP + 30;
constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

std::atomic<uint64_t> gNextRequestId{0};

enum class UpdateState {
    Current,
    Available,
    NoRelease,
    Failed,
};

struct ReleaseInfo {
    std::wstring version;
    std::wstring publishedAt;
    std::wstring body;
    std::wstring releaseUrl;
};

struct UpdatePayload {
    uint64_t requestId = 0;
    UpdateState state = UpdateState::Failed;
    std::wstring version;
    std::wstring releaseUrl;
    std::wstring detail;
    std::optional<ReleaseInfo> release;
};

void discardPendingUpdateResults(HWND hwnd) {
    if (!hwnd)
        return;
    MSG msg{};
    while (PeekMessageW(&msg, hwnd, kMsgUpdateReady, kMsgUpdateReady, PM_REMOVE))
        delete reinterpret_cast<UpdatePayload*>(msg.lParam);
}

std::wstring wideOf(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    constexpr size_t kMaxResponseBytes = 4 * 1024 * 1024;
    if (out->size() > kMaxResponseBytes || total > kMaxResponseBytes - out->size())
        return 0;
    out->append(ptr, total);
    return total;
}

std::optional<std::vector<int>> parseVersion(const std::wstring& value) {
    if (value.empty())
        return std::nullopt;

    size_t begin = (value.front() == L'v' || value.front() == L'V') ? 1 : 0;
    if (begin == value.size())
        return std::nullopt;

    std::vector<int> parts;
    size_t pos = begin;
    while (pos < value.size()) {
        size_t end = value.find(L'.', pos);
        if (end == std::wstring::npos)
            end = value.size();
        if (end == pos)
            return std::nullopt;

        int number = 0;
        for (size_t i = pos; i < end; ++i) {
            if (value[i] < L'0' || value[i] > L'9')
                return std::nullopt;
            number = number * 10 + static_cast<int>(value[i] - L'0');
        }
        parts.push_back(number);
        pos = end == value.size() ? value.size() : end + 1;
    }
    return parts.empty() ? std::nullopt : std::optional<std::vector<int>>(std::move(parts));
}

int compareVersions(const std::vector<int>& lhs, const std::vector<int>& rhs) {
    size_t count = std::max(lhs.size(), rhs.size());
    for (size_t i = 0; i < count; ++i) {
        int l = i < lhs.size() ? lhs[i] : 0;
        int r = i < rhs.size() ? rhs[i] : 0;
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}

std::wstring displayVersion(const std::wstring& version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V'))
        return version.substr(1);
    return version;
}

std::wstring displayReleaseDate(const std::wstring& value) {
    return value.size() >= 10 ? value.substr(0, 10) : value;
}

void openUrl(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void openUrl(const std::wstring& url) {
    if (!url.empty())
        openUrl(url.c_str());
}

void postUpdateResult(HWND hwnd, uint64_t requestId, UpdateState state,
                      std::wstring version = {}, std::wstring releaseUrl = {},
                      std::wstring detail = {}, std::optional<ReleaseInfo> release = std::nullopt) {
    auto* payload = new UpdatePayload{requestId, state, std::move(version), std::move(releaseUrl),
                                      std::move(detail), std::move(release)};
    if (!PostMessageW(hwnd, kMsgUpdateReady, 0, reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void requestLatestRelease(HWND hwnd, uint64_t requestId) {
    std::thread([hwnd, requestId] {
        CURL* curl = curl_easy_init();
        if (!curl) {
            postUpdateResult(hwnd, requestId, UpdateState::Failed, {}, {}, L"无法初始化网络请求");
            return;
        }

        std::string body;
        constexpr char kUserAgent[] = "QQMusicLyric";
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");

        curl_easy_setopt(curl, CURLOPT_URL, app_info::kLatestReleaseApi);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);

        CURLcode rc = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        if (headers)
            curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            postUpdateResult(hwnd, requestId, UpdateState::Failed, {}, {},
                             L"网络请求失败：" + wideOf(curl_easy_strerror(rc)));
            return;
        }
        if (responseCode == 404) {
            postUpdateResult(hwnd, requestId, UpdateState::NoRelease, {}, {},
                             L"GitHub 尚未发布可供检查的正式版本");
            return;
        }
        if (responseCode != 200) {
            postUpdateResult(hwnd, requestId, UpdateState::Failed, {}, {},
                             L"GitHub 返回 HTTP " + std::to_wstring(responseCode));
            return;
        }

        auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            postUpdateResult(hwnd, requestId, UpdateState::Failed, {}, {},
                             L"无法读取 GitHub 返回的数据");
            return;
        }

        if (!json.contains("tag_name") || !json["tag_name"].is_string() ||
            !json.contains("html_url") || !json["html_url"].is_string()) {
            postUpdateResult(hwnd, requestId, UpdateState::Failed, {}, {},
                             L"最新版本信息不完整");
            return;
        }

        ReleaseInfo latestRelease;
        latestRelease.version = wideOf(json["tag_name"].get<std::string>());
        latestRelease.releaseUrl = wideOf(json["html_url"].get<std::string>());
        if (json.contains("published_at") && json["published_at"].is_string())
            latestRelease.publishedAt = wideOf(json["published_at"].get<std::string>());
        if (json.contains("body") && json["body"].is_string())
            latestRelease.body = wideOf(json["body"].get<std::string>());

        std::wstring version = latestRelease.version;
        std::wstring releaseUrl = latestRelease.releaseUrl;
        auto remoteVersion = parseVersion(version);
        auto localVersion = parseVersion(app_info::kVersion);
        if (version.empty() || !remoteVersion || !localVersion) {
            postUpdateResult(hwnd, requestId, UpdateState::Failed, {}, {},
                             L"最新版本标签格式无法识别");
            return;
        }

        UpdateState state = compareVersions(*remoteVersion, *localVersion) > 0
                                ? UpdateState::Available
                                : UpdateState::Current;
        postUpdateResult(hwnd, requestId, state, std::move(version), std::move(releaseUrl), {},
                         std::move(latestRelease));
    }).detach();
}

uint64_t nextRequestId() {
    return gNextRequestId.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct MarkdownSpanStyle {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool link = false;

    bool operator==(const MarkdownSpanStyle& other) const {
        return bold == other.bold && italic == other.italic && strike == other.strike &&
               code == other.code && link == other.link;
    }
};

struct MarkdownSpan {
    size_t start = 0;
    size_t length = 0;
    MarkdownSpanStyle style;
};

enum class MarkdownBlockKind {
    Paragraph,
    Heading,
    Bullet,
    Ordered,
    Quote,
    Code,
    Rule,
};

struct MarkdownBlock {
    MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;
    int level = 0;
    int number = 0;
    std::wstring prefix;
    std::wstring text;
    std::vector<MarkdownSpan> spans;
};

std::wstring trimMarkdown(std::wstring_view value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == L' ' || value[begin] == L'\t'))
        ++begin;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == L' ' || value[end - 1] == L'\t'))
        --end;
    return std::wstring(value.substr(begin, end - begin));
}

bool isMarkdownWord(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'0' && ch <= L'9') || ch == L'_';
}

bool isMarkdownRule(std::wstring_view value) {
    std::wstring compact;
    for (wchar_t ch : value) {
        if (ch != L' ' && ch != L'\t')
            compact.push_back(ch);
    }
    if (compact.size() < 3)
        return false;
    wchar_t marker = compact.front();
    if (marker != L'-' && marker != L'*' && marker != L'_')
        return false;
    return std::all_of(compact.begin(), compact.end(), [marker](wchar_t ch) {
        return ch == marker;
    });
}

bool parseMarkdownHeading(std::wstring_view line, int* level, std::wstring* text) {
    size_t count = 0;
    while (count < line.size() && count < 6 && line[count] == L'#')
        ++count;
    if (count == 0 || count >= line.size() || line[count] != L' ')
        return false;

    std::wstring heading = trimMarkdown(line.substr(count + 1));
    while (!heading.empty() && heading.back() == L'#')
        heading.pop_back();
    *level = static_cast<int>(count);
    *text = trimMarkdown(heading);
    return true;
}

bool parseMarkdownBullet(std::wstring_view line, int* depth, std::wstring* text) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t'))
        ++pos;
    if (pos + 1 >= line.size() ||
        (line[pos] != L'-' && line[pos] != L'*' && line[pos] != L'+') ||
        line[pos + 1] != L' ')
        return false;
    *depth = static_cast<int>(pos / 2);
    *text = trimMarkdown(line.substr(pos + 2));
    return true;
}

bool parseMarkdownOrdered(std::wstring_view line, int* depth, int* number,
                          std::wstring* text) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t'))
        ++pos;
    size_t digitBegin = pos;
    int parsedNumber = 0;
    while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'9') {
        parsedNumber = parsedNumber * 10 + static_cast<int>(line[pos] - L'0');
        ++pos;
    }
    if (pos == digitBegin || pos + 1 >= line.size() ||
        (line[pos] != L'.' && line[pos] != L')') || line[pos + 1] != L' ')
        return false;
    *depth = static_cast<int>(digitBegin / 2);
    *number = parsedNumber;
    *text = trimMarkdown(line.substr(pos + 2));
    return true;
}

void appendMarkdownText(MarkdownBlock& block, std::wstring_view text,
                        const MarkdownSpanStyle& style) {
    if (text.empty())
        return;
    size_t start = block.text.size();
    block.text.append(text);
    if (!block.spans.empty() && block.spans.back().style == style &&
        block.spans.back().start + block.spans.back().length == start) {
        block.spans.back().length += text.size();
    } else {
        block.spans.push_back(MarkdownSpan{start, text.size(), style});
    }
}

void parseMarkdownInline(std::wstring_view source, MarkdownBlock& block,
                         MarkdownSpanStyle style = {}) {
    size_t plainStart = 0;
    auto flushPlain = [&](size_t end) {
        if (end > plainStart)
            appendMarkdownText(block, source.substr(plainStart, end - plainStart), style);
    };

    size_t i = 0;
    while (i < source.size()) {
        if (source[i] == L'\\' && i + 1 < source.size()) {
            flushPlain(i);
            appendMarkdownText(block, source.substr(i + 1, 1), style);
            i += 2;
            plainStart = i;
            continue;
        }

        if (source[i] == L'`') {
            size_t close = source.find(L'`', i + 1);
            if (close != std::wstring_view::npos && close > i + 1) {
                flushPlain(i);
                MarkdownSpanStyle codeStyle = style;
                codeStyle.code = true;
                appendMarkdownText(block, source.substr(i + 1, close - i - 1), codeStyle);
                i = close + 1;
                plainStart = i;
                continue;
            }
        }

        std::wstring_view marker;
        if (i + 1 < source.size() &&
            ((source[i] == L'*' && source[i + 1] == L'*') ||
             (source[i] == L'_' && source[i + 1] == L'_') ||
             (source[i] == L'~' && source[i + 1] == L'~'))) {
            marker = source.substr(i, 2);
            size_t close = source.find(marker, i + 2);
            if (close != std::wstring_view::npos && close > i + 2) {
                flushPlain(i);
                MarkdownSpanStyle nested = style;
                if (marker[0] == L'~')
                    nested.strike = true;
                else
                    nested.bold = true;
                parseMarkdownInline(source.substr(i + 2, close - i - 2), block, nested);
                i = close + 2;
                plainStart = i;
                continue;
            }
        }

        if (source[i] == L'[') {
            size_t closeLabel = source.find(L"](", i + 1);
            if (closeLabel != std::wstring_view::npos) {
                size_t closeUrl = source.find(L')', closeLabel + 2);
                if (closeUrl != std::wstring_view::npos && closeUrl > i + 1) {
                    flushPlain(i);
                    MarkdownSpanStyle linkStyle = style;
                    linkStyle.link = true;
                    parseMarkdownInline(source.substr(i + 1, closeLabel - i - 1), block,
                                        linkStyle);
                    i = closeUrl + 1;
                    plainStart = i;
                    continue;
                }
            }
        }

        if (source[i] == L'*' || source[i] == L'_') {
            if (source[i] == L'_' && i > 0 && i + 1 < source.size() &&
                isMarkdownWord(source[i - 1]) && isMarkdownWord(source[i + 1])) {
                ++i;
                continue;
            }
            size_t close = source.find(source[i], i + 1);
            if (close != std::wstring_view::npos && close > i + 1) {
                flushPlain(i);
                MarkdownSpanStyle nested = style;
                nested.italic = true;
                parseMarkdownInline(source.substr(i + 1, close - i - 1), block, nested);
                i = close + 1;
                plainStart = i;
                continue;
            }
        }
        ++i;
    }
    flushPlain(source.size());
}

std::vector<MarkdownBlock> parseMarkdown(const std::wstring& markdown) {
    std::vector<MarkdownBlock> blocks;
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= markdown.size()) {
        size_t end = markdown.find(L'\n', start);
        if (end == std::wstring::npos)
            end = markdown.size();
        std::wstring line = markdown.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == markdown.size())
            break;
        start = end + 1;
    }

    std::wstring paragraph;
    bool inFence = false;
    MarkdownBlock codeBlock;
    auto flushParagraph = [&] {
        if (paragraph.empty())
            return;
        MarkdownBlock block;
        block.kind = MarkdownBlockKind::Paragraph;
        parseMarkdownInline(paragraph, block);
        blocks.push_back(std::move(block));
        paragraph.clear();
    };
    auto addInlineBlock = [&](MarkdownBlockKind kind, const std::wstring& text,
                              const std::wstring& prefix, int level, int number) {
        MarkdownBlock block;
        block.kind = kind;
        block.prefix = prefix;
        block.level = level;
        block.number = number;
        parseMarkdownInline(text, block);
        blocks.push_back(std::move(block));
    };

    for (const auto& line : lines) {
        std::wstring trimmed = trimMarkdown(line);
        bool fenceLine = trimmed.rfind(L"```", 0) == 0 || trimmed.rfind(L"~~~", 0) == 0;
        if (inFence) {
            if (fenceLine) {
                blocks.push_back(std::move(codeBlock));
                codeBlock = MarkdownBlock{};
                inFence = false;
            } else {
                if (!codeBlock.text.empty())
                    codeBlock.text.push_back(L'\n');
                codeBlock.text += line;
            }
            continue;
        }
        if (fenceLine) {
            flushParagraph();
            inFence = true;
            codeBlock = MarkdownBlock{};
            codeBlock.kind = MarkdownBlockKind::Code;
            continue;
        }
        if (trimmed.empty()) {
            flushParagraph();
            continue;
        }

        if (trimmed.front() == L'>') {
            flushParagraph();
            std::wstring quote = trimMarkdown(trimmed.substr(1));
            addInlineBlock(MarkdownBlockKind::Quote, quote, L"│ ", 0, 0);
            continue;
        }

        int headingLevel = 0;
        std::wstring headingText;
        if (parseMarkdownHeading(trimmed, &headingLevel, &headingText)) {
            flushParagraph();
            addInlineBlock(MarkdownBlockKind::Heading, headingText, L"", headingLevel, 0);
            continue;
        }
        if (isMarkdownRule(trimmed)) {
            flushParagraph();
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::Rule;
            blocks.push_back(std::move(block));
            continue;
        }

        int depth = 0;
        std::wstring listText;
        if (parseMarkdownBullet(line, &depth, &listText)) {
            flushParagraph();
            std::wstring prefix(static_cast<size_t>(depth) * 2, L' ');
            if (listText.size() >= 3 && listText[0] == L'[' && listText[2] == L']' &&
                (listText[1] == L' ' || listText[1] == L'x' || listText[1] == L'X')) {
                prefix += listText[1] == L' ' ? L"☐ " : L"☑ ";
                listText = trimMarkdown(listText.substr(3));
            } else {
                prefix += L"• ";
            }
            addInlineBlock(MarkdownBlockKind::Bullet, listText, prefix, depth, 0);
            continue;
        }

        int number = 0;
        if (parseMarkdownOrdered(line, &depth, &number, &listText)) {
            flushParagraph();
            std::wstring prefix(static_cast<size_t>(depth) * 2, L' ');
            prefix += std::to_wstring(number) + L". ";
            addInlineBlock(MarkdownBlockKind::Ordered, listText, prefix, depth, number);
            continue;
        }

        if (!paragraph.empty())
            paragraph.push_back(L' ');
        paragraph += trimmed;
    }
    flushParagraph();
    if (inFence)
        blocks.push_back(std::move(codeBlock));
    return blocks;
}

// 关于页的更新日志逻辑模型。它不创建子窗口，只保存解析结果和滚动位置，
// 由 AboutDialog 的单一绘制表面调用 paint()。
class ReleaseNotesModel {
public:
    void setRelease(ReleaseInfo release) {
        release_ = std::move(release);
        blocks_ = parseMarkdown(release_->body);
        message_.clear();
        scrollY_ = 0.0f;
    }

    void setMessage(const std::wstring& message) {
        release_.reset();
        blocks_.clear();
        message_ = message;
        scrollY_ = 0.0f;
    }

    void scrollBy(float delta) { scrollY_ += delta; }
    float contentHeight() const { return contentHeight_; }

private:
    void applyMarkdownStyles(IDWriteTextLayout* layout, const MarkdownBlock& block,
                             ID2D1SolidColorBrush* linkBrush) {
        for (const auto& span : block.spans) {
            if (span.length == 0)
                continue;

            DWRITE_TEXT_RANGE range{
                static_cast<UINT32>(block.prefix.size() + span.start),
                static_cast<UINT32>(span.length)};
            if (span.style.bold)
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
            if (span.style.italic)
                layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
            if (span.style.strike)
                layout->SetStrikethrough(TRUE, range);
            if (span.style.code) {
                layout->SetFontFamilyName(L"Consolas", range);
                layout->SetFontSize(12.0f, range);
            }
            if (span.style.link) {
                layout->SetUnderline(TRUE, range);
                if (linkBrush)
                    layout->SetDrawingEffect(linkBrush, range);
            }
        }
    }

public:
    void paint(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect) {
        ID2D1DCRenderTarget* rt = painter.target();
        if (!rt)
            return;

        const auto& palette = fluent::palette();
        const float wDip = std::max(0.0f, rect.right - rect.left);
        const float hDip = std::max(0.0f, rect.bottom - rect.top);
        D2D1_RECT_F panelRect = D2D1::RectF(
            rect.left + 0.5f, rect.top + 0.5f, std::max(rect.left + 0.5f, rect.right - 0.5f),
            std::max(rect.top + 0.5f, rect.bottom - 0.5f));
        painter.fillRoundRect(palette.cardFill, panelRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(palette.cardStroke, panelRect, 1.0f,
                                fluent::metrics::cardRadius);

        auto* bodyFormat = painter.textFormat(13.0f, 400);
        if (bodyFormat)
            bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (!release_) {
            const std::wstring& message = message_.empty() ? emptyMessage() : message_;
            painter.drawText(message, bodyFormat,
                             D2D1::RectF(rect.left + 16.0f, rect.top + 12.0f,
                                          std::max(rect.left + 16.0f, rect.right - 16.0f),
                                          std::max(rect.top + 12.0f, rect.bottom - 12.0f)),
                             palette.textSecondary);
            contentHeight_ = hDip;
            return;
        }

        auto* factory = painter.dwrite();
        auto* headerFormat = painter.textFormat(14.0f, 600);
        auto* codeFormat = painter.textFormat(12.0f, 400, false, false, L"Consolas");
        auto* heading1Format = painter.textFormat(18.0f, 700);
        auto* heading2Format = painter.textFormat(16.0f, 700);
        auto* heading3Format = painter.textFormat(14.0f, 600);
        ID2D1SolidColorBrush* linkBrush = painter.createBrush(palette.accent);
        if (!factory || !bodyFormat || !headerFormat || !codeFormat || !heading1Format ||
            !heading2Format || !heading3Format)
            return;

        const auto& release = *release_;
        std::wstring header = displayVersion(release.version);
        if (!release.publishedAt.empty())
            header += L"  ·  " + displayReleaseDate(release.publishedAt);
        painter.drawText(header, headerFormat,
                         D2D1::RectF(rect.left + 16.0f, rect.top + 8.0f,
                                      std::max(rect.left + 16.0f, rect.right - 16.0f),
                                      rect.top + 31.0f),
                         palette.text);
        if (auto* br = painter.brush(palette.separator)) {
            rt->DrawLine(D2D1::Point2F(rect.left + 16.0f, rect.top + 39.0f),
                          D2D1::Point2F(std::max(rect.left + 16.0f, rect.right - 16.0f),
                                        rect.top + 39.0f),
                         br, 1.0f);
        }

        const float bodyTop = rect.top + 48.0f;
        const float bodyViewHeight = std::max(0.0f, hDip - 48.0f);
        if (blocks_.empty()) {
            painter.drawText(L"暂无发布说明", bodyFormat,
                             D2D1::RectF(rect.left + 16.0f, bodyTop,
                                          std::max(rect.left + 16.0f, rect.right - 16.0f),
                                          std::max(bodyTop + 18.0f, rect.bottom - 12.0f)),
                             palette.textSecondary);
            contentHeight_ = bodyViewHeight;
            return;
        }

        codeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        heading1Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        heading2Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        heading3Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        struct RenderedBlock {
            const MarkdownBlock* block = nullptr;
            IDWriteTextLayout* layout = nullptr;
            float height = 0.0f;
            float gap = 0.0f;
        };

        std::vector<RenderedBlock> rendered;
        rendered.reserve(blocks_.size());
        float totalHeight = 0.0f;
        for (const auto& block : blocks_) {
            RenderedBlock item;
            item.block = &block;
            if (block.kind == MarkdownBlockKind::Rule) {
                item.height = 12.0f;
                item.gap = 8.0f;
                rendered.push_back(item);
                totalHeight += item.height + item.gap;
                continue;
            }

            IDWriteTextFormat* format = bodyFormat;
            if (block.kind == MarkdownBlockKind::Code) {
                format = codeFormat;
            } else if (block.kind == MarkdownBlockKind::Heading) {
                format = block.level <= 1 ? heading1Format
                                         : block.level == 2 ? heading2Format : heading3Format;
            }
            std::wstring displayText = block.prefix + block.text;
            const float textWidth = block.kind == MarkdownBlockKind::Code
                                        ? std::max(1.0f, wDip - 48.0f)
                                        : std::max(1.0f, wDip - 32.0f);
            item.layout = painter.textLayout(displayText, format, textWidth, 100000.0f);
            if (item.layout) {
                DWRITE_TEXT_METRICS textMetrics{};
                item.layout->GetMetrics(&textMetrics);
                item.height = std::max(18.0f, textMetrics.height);
                if (block.kind == MarkdownBlockKind::Code)
                    item.height += 12.0f;
                if (block.kind != MarkdownBlockKind::Code)
                    applyMarkdownStyles(item.layout, block, linkBrush);
            } else {
                item.height = block.kind == MarkdownBlockKind::Code ? 30.0f : 18.0f;
            }

            item.gap = block.kind == MarkdownBlockKind::Code
                           ? 12.0f
                           : block.kind == MarkdownBlockKind::Heading ? 8.0f : 6.0f;
            rendered.push_back(item);
            totalHeight += item.height + item.gap;
        }

        if (!rendered.empty())
            totalHeight = std::max(0.0f, totalHeight - rendered.back().gap);
        contentHeight_ = totalHeight;
        const float maxScroll = std::max(0.0f, contentHeight_ - bodyViewHeight);
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);

        rt->PushAxisAlignedClip(D2D1::RectF(rect.left, bodyTop, rect.right, rect.bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float y = bodyTop - scrollY_;
        for (const auto& item : rendered) {
            const auto& block = *item.block;
            if (block.kind == MarkdownBlockKind::Rule) {
                if (y + item.height >= bodyTop && y <= rect.bottom) {
                    if (auto* br = painter.brush(palette.separator)) {
                        const float lineY = y + item.height / 2.0f;
                        rt->DrawLine(D2D1::Point2F(rect.left + 16.0f, lineY),
                                     D2D1::Point2F(std::max(rect.left + 16.0f, rect.right - 16.0f),
                                                   lineY),
                                     br, 1.0f);
                    }
                }
            } else if (item.layout && y + item.height >= bodyTop && y <= rect.bottom) {
                if (block.kind == MarkdownBlockKind::Code) {
                    D2D1_RECT_F codeRect = D2D1::RectF(
                        rect.left + 16.0f, y, std::max(rect.left + 16.0f, rect.right - 16.0f),
                        y + item.height);
                    painter.fillRoundRect(palette.cardFillSolid, codeRect, 6.0f);
                    painter.strokeRoundRect(palette.cardStroke, codeRect, 1.0f, 6.0f);
                    painter.drawTextLayout(item.layout,
                                           D2D1::Point2F(rect.left + 24.0f, y + 6.0f),
                                           palette.text);
                } else {
                    painter.drawTextLayout(
                        item.layout, D2D1::Point2F(rect.left + 16.0f, y),
                        block.kind == MarkdownBlockKind::Quote ? palette.textSecondary
                                                               : palette.text);
                }
            }
            y += item.height + item.gap;
        }
        rt->PopAxisAlignedClip();

        if (contentHeight_ > bodyViewHeight && bodyViewHeight > 0.0f) {
            const float trackTop = bodyTop + 8.0f;
            const float trackHeight = std::max(0.0f, bodyViewHeight - 16.0f);
            const float thumbHeight =
                std::min(trackHeight, std::max(24.0f, trackHeight * bodyViewHeight / contentHeight_));
            const float thumbTravel = std::max(0.0f, trackHeight - thumbHeight);
            const float thumbTop =
                trackTop + (maxScroll > 0.0f ? scrollY_ / maxScroll * thumbTravel : 0.0f);
            painter.fillRoundRect(
                palette.textSecondary,
                D2D1::RectF(std::max(rect.left, rect.right - 11.0f), thumbTop,
                             std::max(rect.left, rect.right - 8.0f), thumbTop + thumbHeight),
                1.5f);
        }
    }

    static const std::wstring& emptyMessage() {
        static const std::wstring message = L"暂无更新日志";
        return message;
    }

    std::optional<ReleaseInfo> release_;
    std::vector<MarkdownBlock> blocks_;
    std::wstring message_;
    float scrollY_ = 0.0f;
    float contentHeight_ = 0.0f;
};

} // namespace

struct AboutDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;
    bool checking = false;
    bool autoCheckOnStartup = true;
    uint64_t activeRequest = 0;
    std::wstring releaseUrl = app_info::kLatestReleasePage;
    std::function<void(bool)> onAutoCheckChanged;

    fluent::FluentDialogSurface surface;
    ReleaseNotesModel releaseNotes;
    std::wstring versionText;
    std::wstring statusText;
    std::wstring latestText;
    bool releaseButtonAccent = false;

    D2D1_RECT_F autoSwitchRect{};
    D2D1_RECT_F titleRect{};
    D2D1_RECT_F subtitleRect{};
    D2D1_RECT_F infoCardRect{};
    D2D1_RECT_F versionRect{};
    D2D1_RECT_F autoCheckLabelRect{};
    D2D1_RECT_F releaseTitleRect{};
    D2D1_RECT_F statusRect{};
    D2D1_RECT_F latestRect{};
    D2D1_RECT_F projectRect{};
    D2D1_RECT_F checkRect{};
    D2D1_RECT_F releaseRect{};
    D2D1_RECT_F closeRect{};
    D2D1_RECT_F notesRect{};

    int hoverId = 0;
    int pressedId = 0;
    int focusedId = kIdAutoCheckSwitch;
    bool focusVisible = false;

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    static bool contains(const D2D1_RECT_F& rect, float x, float y) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    bool isEnabled(int id) const {
        return id != kIdCheckButton || !checking;
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                    const std::wstring& text, bool accent, int id) {
        const auto& p = fluent::palette();
        const bool enabled = isEnabled(id);
        const bool hovered = enabled && hoverId == id;
        const bool pressed = enabled && pressedId == id;
        D2D1_COLOR_F fill{};
        D2D1_COLOR_F textColor{};
        if (!enabled) {
            fill = p.listHover;
            textColor = p.disabled;
        } else if (accent) {
            fill = pressed ? p.accentPressed : hovered ? p.accentHover : p.accent;
            textColor = p.textOnAccent;
        } else {
            fill = pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill;
            textColor = p.text;
        }
        painter.fillRoundRect(fill, rect);
        if (!accent || !enabled)
            painter.strokeRoundRect(p.cardStroke, rect);
        if (focusedId == id && focusVisible && enabled) {
            painter.strokeRoundRect(accent ? p.textOnAccent : p.accent,
                                    D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f,
                                                rect.right - 1.5f, rect.bottom - 1.5f),
                                    1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
        }
        auto* format = painter.textFormat(14.0f, 400, true, true);
        painter.drawText(text, format,
                         D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f, rect.bottom),
                         textColor);
    }

    void drawToggle(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                    bool checked) {
        const auto& p = fluent::palette();
        const bool enabled = true;
        const bool hovered = hoverId == kIdAutoCheckSwitch;
        const bool focused = focusedId == kIdAutoCheckSwitch && focusVisible;
        const float trackH = std::min(20.0f, rect.bottom - rect.top);
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(
            rect.left + 0.5f, centerY - trackH * 0.5f, rect.right - 0.5f,
            centerY + trackH * 0.5f);
        const float radius = trackH * 0.5f;
        const float knobR = trackH * 0.5f - 3.5f;
        const float knobX = checked ? track.right - trackH * 0.5f : track.left + trackH * 0.5f;
        if (checked) {
            painter.fillRoundRect(hovered ? p.accentHover : p.accent, track, radius);
            if (auto* br = painter.brush(p.textOnAccent))
                painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR,
                                                            knobR),
                                             br);
        } else {
            painter.fillRoundRect(hovered ? p.controlHover : p.controlFill, track, radius);
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
            if (auto* br = painter.brush(p.textSecondary))
                painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR,
                                                            knobR),
                                             br);
        }
        if (focused && enabled) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(track.left + 1.5f, track.top + 1.5f,
                            track.right - 1.5f, track.bottom - 1.5f),
                1.5f, std::max(1.0f, radius - 1.5f));
        }
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        const auto& p = fluent::palette();
        painter.drawText(L"QQ 音乐/网易云任务栏歌词", painter.textFormat(21.0f, 600), titleRect,
                         p.text);
        painter.drawText(L"把歌词带到 Windows 任务栏", painter.textFormat(13.0f, 400), subtitleRect,
                         p.textSecondary);

        painter.fillRoundRect(p.cardFill, infoCardRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, infoCardRect, 1.0f, fluent::metrics::cardRadius);
        painter.drawText(versionText, painter.textFormat(13.0f, 400), versionRect, p.text);
        painter.drawText(L"启动时自动检查更新", painter.textFormat(13.0f, 400), autoCheckLabelRect,
                         p.textSecondary);
        drawToggle(painter, autoSwitchRect, autoCheckOnStartup);
        drawButton(painter, projectRect, L"查看 GitHub 项目", false, kIdProjectButton);

        painter.drawText(L"更新日志", painter.textFormat(14.0f, 600), releaseTitleRect, p.text);
        releaseNotes.paint(painter, notesRect);
        painter.drawText(statusText, painter.textFormat(13.0f, 400), statusRect, p.textSecondary);
        painter.drawText(latestText, painter.textFormat(13.0f, 400), latestRect, p.textSecondary);

        drawButton(painter, checkRect, L"检查更新", true, kIdCheckButton);
        drawButton(painter, releaseRect, L"打开发布页", releaseButtonAccent, kIdReleaseButton);
        drawButton(painter, closeRect, L"关闭", false, kIdCloseButton);
    }

    int hitTest(float x, float y) const {
        if (contains(autoSwitchRect, x, y))
            return kIdAutoCheckSwitch;
        if (contains(projectRect, x, y))
            return kIdProjectButton;
        if (contains(checkRect, x, y))
            return kIdCheckButton;
        if (contains(releaseRect, x, y))
            return kIdReleaseButton;
        if (contains(closeRect, x, y))
            return kIdCloseButton;
        if (contains(notesRect, x, y))
            return kIdReleaseNotes;
        return 0;
    }

    std::vector<int> focusOrder() const {
        std::vector<int> order{kIdAutoCheckSwitch, kIdProjectButton, kIdCheckButton,
                               kIdReleaseButton, kIdCloseButton};
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [this](int id) { return !isEnabled(id); }),
                    order.end());
        return order;
    }

    void focusStep(int direction) {
        const auto order = focusOrder();
        if (order.empty())
            return;
        auto it = std::find(order.begin(), order.end(), focusedId);
        int index = it == order.end() ? (direction > 0 ? -1 : 0)
                                      : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) % order.size();
        focusedId = order[index];
        focusVisible = true;
        surface.invalidate();
    }

    void onCommand(int id) {
        switch (id) {
        case kIdProjectButton:
            openUrl(app_info::kProjectUrl);
            break;
        case kIdAutoCheckSwitch:
            autoCheckOnStartup = !autoCheckOnStartup;
            if (onAutoCheckChanged)
                onAutoCheckChanged(autoCheckOnStartup);
            break;
        case kIdCheckButton:
            startCheck();
            break;
        case kIdReleaseButton:
            openUrl(releaseUrl);
            break;
        case kIdCloseButton:
            destroy();
            return;
        }
        if (hwnd)
            surface.invalidate();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            surface.initialize(hwnd, backdrop);
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            surface.invalidate();
            return 0;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            surface.invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop);
            surface.setBackdrop(backdrop);
            surface.invalidate();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            surface.paint(hdc, backdrop,
                          [this](fluent::FluentDialogSurface::Painter& painter, float w, float h) {
                              paint(painter, w, h);
                          });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            surface.eraseBackground(reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_MOUSEMOVE: {
            if (!GetCapture()) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            const float s = surface.dipScale();
            const int id = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            if (id != hoverId) {
                hoverId = id;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverId != 0) {
                hoverId = 0;
                surface.invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            pressedId = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            if (pressedId == kIdReleaseNotes)
                pressedId = 0;
            if (pressedId != 0) {
                focusedId = pressedId;
                SetCapture(hwnd);
            }
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const float s = surface.dipScale();
            const int hit = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            const int pressed = pressedId;
            pressedId = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (pressed != 0 && pressed == hit && isEnabled(pressed))
                onCommand(pressed);
            surface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressedId = 0;
            surface.invalidate();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float s = surface.dipScale();
            if (contains(notesRect, point.x / s, point.y / s)) {
                releaseNotes.scrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
                                       WHEEL_DELTA * 64.0f);
                surface.invalidate();
            }
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB;
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                focusStep((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                if (focusedId != 0 && isEnabled(focusedId))
                    onCommand(focusedId);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            focusVisible = false;
            surface.invalidate();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED)
                onCommand(LOWORD(wp));
            return 0;
        case kMsgUpdateReady: {
            std::unique_ptr<UpdatePayload> result(reinterpret_cast<UpdatePayload*>(lp));
            if (!result || result->requestId != activeRequest)
                return 0;
            checking = false;
            if (result->release)
                releaseNotes.setRelease(std::move(*result->release));
            else
                releaseNotes.setMessage(result->detail.empty() ? L"暂无更新日志" : result->detail);
            std::wstring visibleVersion = displayVersion(result->version);
            if (result->state == UpdateState::Available) {
                releaseUrl = result->releaseUrl.empty() ? app_info::kLatestReleasePage
                                                        : result->releaseUrl;
                statusText = L"发现新版本：" + visibleVersion + L"，请点击“打开发布页”";
                latestText = L"最新版本：" + visibleVersion;
                releaseButtonAccent = true;
            } else if (result->state == UpdateState::Current) {
                releaseUrl = result->releaseUrl.empty() ? app_info::kLatestReleasePage
                                                        : result->releaseUrl;
                statusText = L"当前已是最新版本";
                latestText = L"当前正式版本：" + visibleVersion;
                releaseButtonAccent = false;
            } else {
                releaseUrl = app_info::kLatestReleasePage;
                statusText = result->detail;
                latestText.clear();
                releaseButtonAccent = false;
            }
            surface.invalidate();
            return 0;
        }
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            discardPendingUpdateResults(hwnd);
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::About), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void createControls() {
        versionText = std::wstring(L"当前版本：") + displayVersion(app_info::kVersion);
        statusText = L"点击“检查更新”获取最新版本";
        latestText.clear();
        releaseNotes.setMessage(L"点击“检查更新”加载更新日志");
    }

    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);
        const float pad = fluent::metrics::pagePadding;
        const float gap = fluent::metrics::controlGap;

        const float titleH = 30.0f;
        const float subtitleH = 20.0f;
        titleRect = D2D1::RectF(pad, pad, std::max(pad, w - pad), pad + titleH);
        subtitleRect = D2D1::RectF(pad, pad + titleH, std::max(pad, w - pad),
                                    pad + titleH + subtitleH);

        const float cardY = pad + titleH + subtitleH + fluent::metrics::sectionGap;
        const float cardH = 100.0f;
        infoCardRect = D2D1::RectF(pad, cardY, std::max(pad, w - pad), cardY + cardH);
        const float contentX = pad + 16.0f;
        const float contentW = std::max(20.0f, w - contentX - pad - 16.0f);
        versionRect = D2D1::RectF(contentX, cardY + 14.0f, contentX + contentW,
                                   cardY + 36.0f);
        projectRect = D2D1::RectF(contentX, cardY + 54.0f, contentX + 148.0f,
                                  cardY + 86.0f);

        const float switchW = 40.0f;
        const float switchH = 20.0f;
        const float switchX = w - pad - 16.0f - switchW;
        autoCheckLabelRect = D2D1::RectF(
            contentX + 164.0f, cardY + 60.0f,
            std::max(contentX + 184.0f, switchX - 8.0f), cardY + 80.0f);
        autoSwitchRect = D2D1::RectF(switchX, cardY + 60.0f + (32.0f - switchH) / 2.0f,
                                     switchX + switchW,
                                     cardY + 60.0f + (32.0f - switchH) / 2.0f + switchH);

        const float btnH = fluent::metrics::controlHeight;
        const float closeW = 88.0f;
        const float releaseW = 116.0f;
        const float checkW = 116.0f;
        const float btnY = h - pad - btnH;
        closeRect = D2D1::RectF(w - pad - closeW, btnY, w - pad, btnY + btnH);
        releaseRect = D2D1::RectF(w - pad - closeW - gap - releaseW, btnY,
                                  w - pad - closeW - gap, btnY + btnH);
        checkRect = D2D1::RectF(w - pad - closeW - gap - releaseW - gap - checkW, btnY,
                                w - pad - closeW - gap - releaseW - gap, btnY + btnH);

        const float releaseTitleY = cardY + cardH + fluent::metrics::sectionGap;
        releaseTitleRect = D2D1::RectF(pad, releaseTitleY, std::max(pad, w - pad),
                                       releaseTitleY + 22.0f);
        const float notesY = releaseTitleY + 22.0f + fluent::metrics::compactGap;
        const float statusH = 22.0f;
        const float latestH = 20.0f;
        const float latestY = btnY - gap - latestH;
        const float statusY = latestY - statusH;
        const float notesBottom = statusY - gap;
        notesRect = D2D1::RectF(pad, notesY, std::max(pad, w - pad),
                                std::max(notesY + 80.0f, notesBottom));
        statusRect = D2D1::RectF(pad, statusY, std::max(pad, w - pad), statusY + statusH);
        latestRect = D2D1::RectF(pad, latestY, std::max(pad, w - pad), latestY + latestH);
    }

    void startCheck() {
        if (checking || !hwnd)
            return;
        checking = true;
        activeRequest = nextRequestId();
        releaseUrl = app_info::kLatestReleasePage;
        // 打开瞬间只保留一处即时反馈；旧结果（更新日志/版本标签/按钮高亮）
        // 留到 kMsgUpdateReady 统一刷新。
        statusText = L"正在检查更新…";
        requestLatestRelease(hwnd, activeRequest);
    }

    void destroy() {
        if (hwnd) {
            HWND target = hwnd;
            DestroyWindow(target);
            discardPendingUpdateResults(target);
            hwnd = nullptr;
        }
    }
};

AboutDialog::AboutDialog() : impl_(std::make_unique<Impl>()) {}

AboutDialog::~AboutDialog() {
    destroy();
}

bool AboutDialog::create(HINSTANCE inst, HWND parent, bool autoCheckOnStartup,
                         std::function<void(bool)> onAutoCheckChanged) {
    impl_->notifyHwnd = parent; // 仅用于关闭通知；托盘窗口不能作为普通窗口的可见所有者。
    impl_->inst = inst;
    impl_->autoCheckOnStartup = autoCheckOnStartup;
    impl_->onAutoCheckChanged = std::move(onAutoCheckChanged);
    impl_->checking = false;
    impl_->activeRequest = nextRequestId();
    impl_->releaseUrl = app_info::kLatestReleasePage;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricAboutDialog";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(520.0f * s)),
            static_cast<LONG>(std::lround(620.0f * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricAboutDialog", L"关于",
                                  kDialogStyle, x, y, w, h, nullptr, nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;

    // 启动检查受设置控制；AboutDialog::show() 不受此设置影响。
    if (impl_->autoCheckOnStartup)
        impl_->startCheck();
    return true;
}

void AboutDialog::show() {
    if (impl_->hwnd) {
        // 每次打开关于窗口都重新检查一次；若上一次启动检查仍在进行则保持单请求。
        impl_->startCheck();
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
    }
}

void AboutDialog::destroy() {
    impl_->destroy();
}

bool AboutDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND AboutDialog::hwnd() const {
    return impl_->hwnd;
}
