#include "about_dialog.h"

#include "app_info.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_controls.h"
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

// 关于页的 Win11 风格开关：点击后向父窗口发送 WM_COMMAND/BN_CLICKED。
class ReleaseNotesPanel : public fluent::LayeredChild {
public:
    bool create(HWND parent, int id) {
        clearAlpha_ = 1.0f / 255.0f;
        return createLayered(parent, L"QQMusicLyricReleaseNotes", wndProc, id);
    }

    void setRelease(ReleaseInfo release) {
        release_ = std::move(release);
        blocks_ = parseMarkdown(release_->body);
        message_.clear();
        scrollY_ = 0.0f;
        renderNow();
    }

    void setMessage(const std::wstring& message) {
        release_.reset();
        blocks_.clear();
        message_ = message;
        scrollY_ = 0.0f;
        renderNow();
    }

private:
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<ReleaseNotesPanel*>(
            GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<ReleaseNotesPanel*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    IDWriteTextFormat* createFormat(float dipSize, int weight, bool monospace = false) {
        IDWriteFactory* factory = dwrite();
        if (!factory)
            return nullptr;

        IDWriteTextFormat* format = nullptr;
        const wchar_t* familyName = monospace ? L"Consolas" : fluent::uiFontFamily();
        HRESULT hr = factory->CreateTextFormat(
            familyName, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, dipSize, L"zh-cn", &format);
        if (FAILED(hr) || !format)
            return nullptr;

        fluent::applyUiFontFallback(format);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return format;
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEWHEEL:
            scrollY_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 64.0f;
            renderNow();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            renderNow();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

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

    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        const auto& palette = fluent::palette();
        auto* br = brush(rt);
        if (!br)
            return;

        D2D1_RECT_F panelRect =
            D2D1::RectF(0.5f, 0.5f, std::max(0.5f, wDip - 0.5f),
                        std::max(0.5f, hDip - 0.5f));
        br->SetColor(palette.cardFill);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(panelRect, fluent::metrics::cardRadius,
                              fluent::metrics::cardRadius),
            br);
        br->SetColor(palette.cardStroke);
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(panelRect, fluent::metrics::cardRadius,
                              fluent::metrics::cardRadius),
            br, 1.0f);

        if (!release_) {
            IDWriteTextFormat* format = createFormat(13.0f, 400);
            if (format) {
                br->SetColor(palette.textSecondary);
                const std::wstring& message =
                    message_.empty() ? emptyMessage() : message_;
                rt->DrawTextW(
                    message.c_str(), static_cast<UINT32>(message.size()), format,
                    D2D1::RectF(16.0f, 12.0f, std::max(16.0f, wDip - 16.0f),
                                std::max(12.0f, hDip - 12.0f)),
                    br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                format->Release();
            }
            contentHeight_ = hDip;
            return;
        }

        IDWriteFactory* factory = dwrite();
        IDWriteTextFormat* headerFormat = createFormat(14.0f, 600);
        IDWriteTextFormat* bodyFormat = createFormat(13.0f, 400);
        IDWriteTextFormat* codeFormat = createFormat(12.0f, 400, true);
        IDWriteTextFormat* heading1Format = createFormat(18.0f, 700);
        IDWriteTextFormat* heading2Format = createFormat(16.0f, 700);
        IDWriteTextFormat* heading3Format = createFormat(14.0f, 600);
        ID2D1SolidColorBrush* linkBrush = nullptr;
        rt->CreateSolidColorBrush(palette.accent, &linkBrush);

        auto releaseResources = [&] {
            if (linkBrush)
                linkBrush->Release();
            if (headerFormat)
                headerFormat->Release();
            if (bodyFormat)
                bodyFormat->Release();
            if (codeFormat)
                codeFormat->Release();
            if (heading1Format)
                heading1Format->Release();
            if (heading2Format)
                heading2Format->Release();
            if (heading3Format)
                heading3Format->Release();
        };

        if (!factory || !headerFormat || !bodyFormat || !codeFormat ||
            !heading1Format || !heading2Format || !heading3Format) {
            releaseResources();
            return;
        }

        const auto& release = *release_;
        std::wstring header = displayVersion(release.version);
        if (!release.publishedAt.empty())
            header += L"  ·  " + displayReleaseDate(release.publishedAt);

        br->SetColor(palette.text);
        rt->DrawTextW(
            header.c_str(), static_cast<UINT32>(header.size()), headerFormat,
            D2D1::RectF(16.0f, 8.0f, std::max(16.0f, wDip - 16.0f), 31.0f),
            br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        br->SetColor(palette.separator);
        rt->DrawLine(
            D2D1::Point2F(16.0f, 39.0f),
            D2D1::Point2F(std::max(16.0f, wDip - 16.0f), 39.0f), br, 1.0f);

        const float bodyTop = 48.0f;
        const float bodyViewHeight = std::max(0.0f, hDip - bodyTop);
        if (blocks_.empty()) {
            const std::wstring emptyBody = L"暂无发布说明";
            br->SetColor(palette.textSecondary);
            rt->DrawTextW(
                emptyBody.c_str(), static_cast<UINT32>(emptyBody.size()), bodyFormat,
                D2D1::RectF(16.0f, bodyTop, std::max(16.0f, wDip - 16.0f),
                            std::max(bodyTop + 18.0f, hDip - 12.0f)),
                br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            contentHeight_ = bodyViewHeight;
            releaseResources();
            return;
        }

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
                if (block.level <= 1)
                    format = heading1Format;
                else if (block.level == 2)
                    format = heading2Format;
                else
                    format = heading3Format;
            }

            std::wstring displayText = block.prefix + block.text;
            float textWidth = block.kind == MarkdownBlockKind::Code
                                  ? std::max(1.0f, wDip - 48.0f)
                                  : std::max(1.0f, wDip - 32.0f);
            HRESULT hr = factory->CreateTextLayout(
                displayText.c_str(), static_cast<UINT32>(displayText.size()), format,
                textWidth, 100000.0f, &item.layout);
            if (SUCCEEDED(hr) && item.layout) {
                DWRITE_TEXT_METRICS metrics{};
                item.layout->GetMetrics(&metrics);
                item.height = std::max(18.0f, metrics.height);
                if (block.kind == MarkdownBlockKind::Code)
                    item.height += 12.0f;
                if (block.kind != MarkdownBlockKind::Code)
                    applyMarkdownStyles(item.layout, block, linkBrush);
            } else {
                item.height = block.kind == MarkdownBlockKind::Code ? 30.0f : 18.0f;
            }

            if (block.kind == MarkdownBlockKind::Code)
                item.gap = 12.0f;
            else if (block.kind == MarkdownBlockKind::Heading)
                item.gap = 8.0f;
            else
                item.gap = 6.0f;

            rendered.push_back(item);
            totalHeight += item.height + item.gap;
        }

        if (!rendered.empty())
            totalHeight = std::max(0.0f, totalHeight - rendered.back().gap);
        contentHeight_ = totalHeight;
        float maxScroll = std::max(0.0f, contentHeight_ - bodyViewHeight);
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);

        rt->PushAxisAlignedClip(
            D2D1::RectF(0.0f, bodyTop, wDip, std::max(bodyTop, hDip)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        float y = bodyTop - scrollY_;
        for (const auto& item : rendered) {
            const auto& block = *item.block;
            if (block.kind == MarkdownBlockKind::Rule) {
                if (y + item.height >= bodyTop && y <= hDip) {
                    br->SetColor(palette.separator);
                    float lineY = y + item.height / 2.0f;
                    rt->DrawLine(
                        D2D1::Point2F(16.0f, lineY),
                        D2D1::Point2F(std::max(16.0f, wDip - 16.0f), lineY), br, 1.0f);
                }
            } else if (item.layout && y + item.height >= bodyTop && y <= hDip) {
                if (block.kind == MarkdownBlockKind::Code) {
                    D2D1_RECT_F codeRect =
                        D2D1::RectF(16.0f, y, std::max(16.0f, wDip - 16.0f),
                                    y + item.height);
                    br->SetColor(palette.cardFillSolid);
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(codeRect, 6.0f, 6.0f), br);
                    br->SetColor(palette.cardStroke);
                    rt->DrawRoundedRectangle(
                        D2D1::RoundedRect(codeRect, 6.0f, 6.0f), br, 1.0f);
                    br->SetColor(palette.text);
                    rt->DrawTextLayout(
                        D2D1::Point2F(24.0f, y + 6.0f), item.layout, br,
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                } else {
                    br->SetColor(block.kind == MarkdownBlockKind::Quote
                                     ? palette.textSecondary
                                     : palette.text);
                    rt->DrawTextLayout(
                        D2D1::Point2F(16.0f, y), item.layout, br,
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                }
            }
            y += item.height + item.gap;
        }
        rt->PopAxisAlignedClip();

        if (contentHeight_ > bodyViewHeight && bodyViewHeight > 0.0f) {
            const float trackTop = bodyTop + 8.0f;
            const float trackHeight = std::max(0.0f, bodyViewHeight - 16.0f);
            const float thumbHeight =
                std::min(trackHeight, std::max(24.0f,
                    trackHeight * bodyViewHeight / contentHeight_));
            const float thumbTravel = std::max(0.0f, trackHeight - thumbHeight);
            const float thumbTop =
                trackTop + (maxScroll > 0.0f ? scrollY_ / maxScroll * thumbTravel : 0.0f);
            br->SetColor(palette.textSecondary);
            D2D1_RECT_F thumbRect =
                D2D1::RectF(std::max(0.0f, wDip - 11.0f), thumbTop,
                            std::max(0.0f, wDip - 8.0f), thumbTop + thumbHeight);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(thumbRect, 1.5f, 1.5f), br);
        }

        for (auto& item : rendered) {
            if (item.layout)
                item.layout->Release();
        }
        releaseResources();
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

    fluent::FluentLabel titleLabel;
    fluent::FluentLabel subtitleLabel;
    fluent::FluentCard infoCard;
    fluent::FluentLabel versionLabel;
    fluent::FluentLabel autoCheckLabel;
    fluent::FluentToggle autoCheckSwitch;
    fluent::FluentLabel releaseTitleLabel;
    ReleaseNotesPanel releaseNotes;
    fluent::FluentButton projectButton;
    fluent::FluentLabel statusLabel;
    fluent::FluentLabel latestLabel;
    fluent::FluentButton checkButton;
    fluent::FluentButton releaseButton;
    fluent::FluentButton closeButton;

    void refreshTheme() {
        titleLabel.refreshTheme();
        subtitleLabel.refreshTheme();
        infoCard.refreshTheme();
        versionLabel.refreshTheme();
        autoCheckLabel.refreshTheme();
        autoCheckSwitch.refreshTheme();
        releaseTitleLabel.refreshTheme();
        releaseNotes.refreshTheme();
        projectButton.refreshTheme();
        statusLabel.refreshTheme();
        latestLabel.refreshTheme();
        checkButton.refreshTheme();
        releaseButton.refreshTheme();
        closeButton.refreshTheme();
    }

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

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop);
            refreshTheme();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            fluent::paintDialogBackground(hwnd, hdc, backdrop);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            fluent::paintDialogBackground(hwnd, reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case kMsgUpdateReady: {
            std::unique_ptr<UpdatePayload> result(reinterpret_cast<UpdatePayload*>(lp));
            if (!result || result->requestId != activeRequest)
                return 0;
            checking = false;
            checkButton.setEnabled(true);
            if (result->release)
                releaseNotes.setRelease(std::move(*result->release));
            else
                releaseNotes.setMessage(result->detail.empty() ? L"暂无更新日志" : result->detail);
            std::wstring visibleVersion = displayVersion(result->version);
            if (result->state == UpdateState::Available) {
                releaseUrl = result->releaseUrl.empty() ? app_info::kLatestReleasePage
                                                        : result->releaseUrl;
                statusLabel.setText(L"发现新版本：" + visibleVersion +
                                    L"，请点击“打开发布页”");
                latestLabel.setText(L"最新版本：" + visibleVersion);
                releaseButton.setAccent(true);
            } else if (result->state == UpdateState::Current) {
                releaseUrl = result->releaseUrl.empty() ? app_info::kLatestReleasePage
                                                        : result->releaseUrl;
                statusLabel.setText(L"当前已是最新版本");
                latestLabel.setText(L"当前正式版本：" + visibleVersion);
                releaseButton.setAccent(false);
            } else {
                releaseUrl = app_info::kLatestReleasePage;
                statusLabel.setText(result->detail);
                latestLabel.setText(L"");
                releaseButton.setAccent(false);
            }
            return 0;
        }
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::About), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void createControls() {
        titleLabel.create(hwnd, kIdTitleLabel, L"QQ 音乐/网易云任务栏歌词", false, 21.0f, 600);
        subtitleLabel.create(hwnd, kIdSubtitleLabel, L"把歌词带到 Windows 任务栏", true, 13.0f,
                             400);
        infoCard.create(hwnd, kIdInfoCard);
        std::wstring versionText = std::wstring(L"当前版本：") + displayVersion(app_info::kVersion);
        versionLabel.create(hwnd, kIdVersionLabel, versionText.c_str());
        autoCheckLabel.create(hwnd, kIdAutoCheckLabel, L"启动时自动检查更新", true, 13.0f, 400);
        autoCheckSwitch.create(hwnd, kIdAutoCheckSwitch, autoCheckOnStartup);
        releaseTitleLabel.create(hwnd, kIdReleaseTitleLabel, L"更新日志", false, 14.0f, 600);
        releaseNotes.create(hwnd, kIdReleaseNotes);
        releaseNotes.setMessage(L"点击“检查更新”加载更新日志");
        projectButton.create(hwnd, kIdProjectButton, L"查看 GitHub 项目");
        statusLabel.create(hwnd, kIdStatusLabel, L"点击“检查更新”获取最新版本", true, 13.0f, 400);
        latestLabel.create(hwnd, kIdLatestLabel, L"", true, 13.0f, 400);
        checkButton.create(hwnd, kIdCheckButton, L"检查更新", true);
        releaseButton.create(hwnd, kIdReleaseButton, L"打开发布页");
        closeButton.create(hwnd, kIdCloseButton, L"关闭");

        // 卡片必须位于卡片内容下方，避免半透明表面冲淡文字和按钮。
        SetWindowPos(infoCard.hwnd(), HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        float s = fluent::dipScale(GetDpiForWindow(hwnd));
        auto px = [&](float dip) { return static_cast<int>(std::lround(dip * s)); };
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int pad = px(fluent::metrics::pagePadding);
        int gap = px(fluent::metrics::controlGap);

        int titleH = px(30.0f);
        int subtitleH = px(20.0f);
        titleLabel.move(pad, pad, w - pad * 2, titleH);
        subtitleLabel.move(pad, pad + titleH, w - pad * 2, subtitleH);

        int cardY = pad + titleH + subtitleH + px(fluent::metrics::sectionGap);
        int cardH = px(100.0f);
        infoCard.move(pad, cardY, w - pad * 2, cardH);
        int contentX = pad + px(16.0f);
        int contentW = w - contentX - pad - px(16.0f);
        versionLabel.move(contentX, cardY + px(14.0f), contentW, px(22.0f));
        projectButton.move(contentX, cardY + px(54.0f), px(148.0f), px(32.0f));
        int switchW = px(fluent::FluentToggle::kWidth);
        int switchH = px(fluent::FluentToggle::kHeight);
        int switchX = w - pad - px(16.0f) - switchW;
        autoCheckLabel.move(contentX + px(164.0f), cardY + px(60.0f),
                            std::max(px(20.0f), switchX - px(8.0f) - contentX - px(164.0f)),
                            px(20.0f));
        autoCheckSwitch.move(switchX, cardY + px(60.0f) + (px(32.0f) - switchH) / 2,
                             switchW, switchH);

        int btnH = px(fluent::metrics::controlHeight);
        int closeW = px(88.0f);
        int releaseW = px(116.0f);
        int checkW = px(116.0f);
        int btnY = h - pad - btnH;
        closeButton.move(w - pad - closeW, btnY, closeW, btnH);
        releaseButton.move(w - pad - closeW - gap - releaseW, btnY, releaseW, btnH);
        checkButton.move(w - pad - closeW - gap - releaseW - gap - checkW, btnY, checkW, btnH);

        int releaseTitleY = cardY + cardH + px(fluent::metrics::sectionGap);
        releaseTitleLabel.move(pad, releaseTitleY, w - pad * 2, px(22.0f));
        int notesY = releaseTitleY + px(22.0f) + px(fluent::metrics::compactGap);
        int statusH = px(22.0f);
        int latestH = px(20.0f);
        int latestY = btnY - gap - latestH;
        int statusY = latestY - statusH;
        int notesBottom = statusY - gap;
        releaseNotes.move(pad, notesY, w - pad * 2,
                          std::max(px(80.0f), notesBottom - notesY));
        statusLabel.move(pad, statusY, w - pad * 2, statusH);
        latestLabel.move(pad, latestY, w - pad * 2, latestH);
    }

    void startCheck() {
        if (checking || !hwnd)
            return;
        checking = true;
        activeRequest = nextRequestId();
        releaseUrl = app_info::kLatestReleasePage;
        // 打开瞬间只保留一处即时反馈；旧结果（更新日志/版本标签/按钮高亮）
        // 留到 kMsgUpdateReady 统一刷新，避免 show() 时串行触发多次 D2D 重绘
        checkButton.setEnabled(false);
        statusLabel.setText(L"正在检查更新…");
        requestLatestRelease(hwnd, activeRequest);
    }

    void onCommand(int id, int code) {
        if (code != BN_CLICKED)
            return;
        switch (id) {
        case kIdProjectButton:
            openUrl(app_info::kProjectUrl);
            break;
        case kIdAutoCheckSwitch:
            autoCheckOnStartup = autoCheckSwitch.checked();
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
            break;
        }
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
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
