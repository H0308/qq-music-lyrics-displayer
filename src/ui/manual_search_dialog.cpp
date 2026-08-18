#include "ui/manual_search_dialog.h"

#include "ui/lyric_renderer.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_theme.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>

namespace {

constexpr int kIdTitleEdit = 101;
constexpr int kIdArtistEdit = 102;
constexpr int kIdSearchBtn = 103;
constexpr int kIdCandidateList = 104;
constexpr int kIdOkBtn = 105;
constexpr int kIdCancelBtn = 106;
constexpr int kIdPreviewPanel = 107;
constexpr int kIdStatus = 108;
constexpr int kIdHint = 109;
constexpr int kIdAdvanceLyric = 110;
constexpr int kIdDelayLyric = 111;
constexpr int kIdTitleLabel = 112;
constexpr int kIdSubtitleLabel = 113;

constexpr UINT kMsgCandidatesReady = WM_APP + 10;
constexpr UINT kMsgPreviewLyricReady = WM_APP + 11;

// 右侧桌面歌词风格预览面板（7 行，高亮当前播放行，支持鼠标滚轮手动滚动）
class LyricPreviewPanel : public fluent::LayeredChild {
public:
    bool create(HWND parent, HINSTANCE, int, int, int, int) {
        return createLayered(parent, L"QQMusicLyricPreviewPanel", wndProc, kIdPreviewPanel);
    }

    void setLyrics(const std::vector<LyricLine>& lines) {
        lines_ = lines;
        bool hasTranslation = false;
        bool hasRomanization = false;
        for (const auto& line : lines_) {
            hasTranslation = hasTranslation || !line.translation.empty();
            hasRomanization = hasRomanization || !line.romanization.empty();
        }
        if (hasTranslation && hasRomanization)
            capabilityLabel_ = L"[支持翻译+罗马音]";
        else if (hasTranslation)
            capabilityLabel_ = L"[支持翻译]";
        else if (hasRomanization)
            capabilityLabel_ = L"[支持罗马音]";
        else
            capabilityLabel_.clear();
        positionMs_ = 0;
        topLine_ = 0;
        currentLine_ = -1;
        manualScroll_ = false;
        wheelAccum_ = 0;
        syncToCurrentLine();
        if (hwnd_)
            renderNow();
    }

    void setPosition(int64_t positionMs) {
        positionMs_ = positionMs;
        currentLine_ = LyricProvider::findLine(lines_, positionMs);
        if (!manualScroll_) {
            syncToCurrentLine();
            if (hwnd_)
                renderNow();
        }
    }

private:
    static constexpr int kVisible = 7;
    static constexpr int kMid = kVisible / 2;          // 3，当前行居中
    static constexpr UINT kTimerResume = 1;
    static constexpr DWORD kResumeDelayMs = 2000;      // 停止滚动后 2 秒恢复同步
    static constexpr int kWheelLinesPerNotch = 3;

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        LyricPreviewPanel* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<LyricPreviewPanel*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<LyricPreviewPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd_, kTimerResume, 100, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            renderNow();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_TIMER:
            onTimer();
            return 0;
        case WM_MOUSEWHEEL:
            onMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerResume);
            hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void onTimer() {
        if (manualScroll_) {
            DWORD now = GetTickCount();
            if (now - lastScrollTick_ >= kResumeDelayMs) {
                manualScroll_ = false;
                syncToCurrentLine();
                renderNow();
            }
        }
    }

    void onMouseWheel(int delta) {
        if (lines_.empty())
            return;
        wheelAccum_ += delta;
        int notch = wheelAccum_ / WHEEL_DELTA;
        if (notch == 0)
            return;
        wheelAccum_ -= notch * WHEEL_DELTA;
        // 滚轮向上（delta > 0）内容向上走，topLine 减小；向下则增大
        scrollBy(-notch * kWheelLinesPerNotch);
    }

    void scrollBy(int lines) {
        int maxTop = std::max(0, static_cast<int>(lines_.size()) - kVisible);
        topLine_ = std::clamp(topLine_ + lines, 0, maxTop);
        manualScroll_ = true;
        lastScrollTick_ = GetTickCount();
        renderNow();
    }

    void syncToCurrentLine() {
        int total = static_cast<int>(lines_.size());
        if (total == 0) {
            topLine_ = 0;
            return;
        }
        int maxTop = std::max(0, total - kVisible);
        if (currentLine_ < 0)
            topLine_ = 0;
        else
            topLine_ = std::clamp(currentLine_ - kMid, 0, maxTop);
    }

    float karaokeProgressX(IDWriteTextLayout* layout, const LyricLine& line) const {
        if (!layout || line.chars.empty() || line.text.empty())
            return 0.0f;

        UINT32 sungLength = 0;
        const LyricChar* currentChar = nullptr;
        for (const auto& ch : line.chars) {
            if (ch.startMs > positionMs_)
                break;
            currentChar = &ch;
            sungLength += static_cast<UINT32>(ch.text.size());
        }
        if (!currentChar || sungLength == 0)
            return 0.0f;

        const UINT32 textLength = static_cast<UINT32>(line.text.size());
        sungLength = std::min(sungLength, textLength);
        const UINT32 charLength = std::min(static_cast<UINT32>(currentChar->text.size()), sungLength);
        const UINT32 charOffset = sungLength - charLength;

        DWRITE_HIT_TEST_METRICS metrics{};
        float startX = 0.0f;
        float hitY = 0.0f;
        if (FAILED(layout->HitTestTextPosition(charOffset, FALSE, &startX, &hitY, &metrics)))
            return 0.0f;

        float endX = startX;
        if (FAILED(layout->HitTestTextPosition(sungLength - 1, TRUE, &endX, &hitY, &metrics)))
            endX = startX;

        const int64_t durationMs = std::max<int64_t>(currentChar->endMs - currentChar->startMs, 1);
        const float fraction = static_cast<float>(std::clamp(
            static_cast<double>(positionMs_ - currentChar->startMs) /
                static_cast<double>(durationMs),
            0.0, 1.0));
        return startX + (endX - startX) * fraction;
    }

    void drawCurrentLine(ID2D1DCRenderTarget* rt, ID2D1SolidColorBrush* br,
                         const fluent::Palette& palette, const LyricLine& line,
                         const D2D1_RECT_F& rect) {
        auto* fmt = textFormat(15.0f, 700, true);
        if (!fmt)
            return;

        if (line.chars.empty()) {
            br->SetColor(fluent::toD2D(RGB(49, 194, 124)));
            rt->DrawTextW(line.text.c_str(), static_cast<UINT32>(line.text.size()), fmt, rect, br);
            return;
        }

        IDWriteTextLayout* layout = nullptr;
        IDWriteFactory* dw = dwrite();
        if (!dw || FAILED(dw->CreateTextLayout(
                       line.text.c_str(), static_cast<UINT32>(line.text.size()), fmt,
                       std::max(rect.right - rect.left, 1.0f),
                       std::max(rect.bottom - rect.top, 1.0f), &layout)) ||
            !layout) {
            return;
        }

        const D2D1_POINT_2F origin = D2D1::Point2F(rect.left, rect.top);
        br->SetColor(palette.textSecondary);
        rt->DrawTextLayout(origin, layout, br);

        const float progressX = karaokeProgressX(layout, line);
        if (progressX > 0.0f) {
            rt->PushAxisAlignedClip(
                D2D1::RectF(rect.left, rect.top,
                            std::min(rect.left + progressX, rect.right), rect.bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            br->SetColor(fluent::toD2D(RGB(49, 194, 124)));
            rt->DrawTextLayout(origin, layout, br);
            rt->PopAxisAlignedClip();
        }
        layout->Release();
    }

    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        const fluent::Palette& p = fluent::palette();
        auto* br = brush(rt);
        if (!br)
            return;
        // 半透明圆角卡片，叠在 Mica 背景上
        D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
        br->SetColor(p.cardFill);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(card, fluent::metrics::cardRadius, fluent::metrics::cardRadius), br);
        br->SetColor(p.cardStroke);
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(card, fluent::metrics::cardRadius, fluent::metrics::cardRadius), br,
            1.0f);

        float pad = 12.0f;
        if (lines_.empty()) {
            if (auto* fmt = textFormat(15.0f, 400, true)) {
                const std::wstring placeholder = L"选择左侧歌曲预览歌词";
                br->SetColor(p.textSecondary);
                rt->DrawTextW(placeholder.c_str(), static_cast<UINT32>(placeholder.size()), fmt,
                              D2D1::RectF(pad, hDip * 0.4f, wDip - pad, hDip * 0.6f), br);
            }
            return;
        }
        float contentTop = 0.0f;
        if (!capabilityLabel_.empty()) {
            if (auto* fmt = textFormat(11.0f, 400, true)) {
                br->SetColor(p.textSecondary);
                rt->DrawTextW(capabilityLabel_.c_str(), static_cast<UINT32>(capabilityLabel_.size()),
                              fmt, D2D1::RectF(pad, 7.0f, wDip - pad, 27.0f), br);
            }
            contentTop = 27.0f;
        }
        int total = static_cast<int>(lines_.size());
        int maxTop = std::max(0, total - kVisible);
        int top = std::clamp(topLine_, 0, maxTop);
        int count = std::min(kVisible, total - top);
        float slotH = std::max(1.0f, (hDip - contentTop) / kVisible);
        for (int i = 0; i < count; ++i) {
            int lineIdx = top + i;
            bool cur = (currentLine_ >= 0 && lineIdx == currentLine_);
            const std::wstring& text = lines_[lineIdx].text;
            D2D1_RECT_F rect = D2D1::RectF(pad, contentTop + i * slotH, wDip - pad,
                                           contentTop + (i + 1) * slotH);
            // 每行按需取格式：不同参数会重建缓存格式，跨行持有指针会悬空
            if (cur) {
                drawCurrentLine(rt, br, p, lines_[lineIdx], rect);
            } else if (auto* fmt = textFormat(14.0f, 400, true)) {
                br->SetColor(p.text);
                rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, br);
            }
        }
    }

    std::vector<LyricLine> lines_;
    std::wstring capabilityLabel_;
    int64_t positionMs_ = 0;
    int topLine_ = 0;
    int currentLine_ = -1;
    bool manualScroll_ = false;
    DWORD lastScrollTick_ = 0;
    int wheelAccum_ = 0;
};

} // namespace

struct ManualSearchDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;

    fluent::FluentLabel titleLabel;
    fluent::FluentLabel subtitleLabel;
    fluent::FluentEdit titleEdit;
    fluent::FluentEdit artistEdit;
    fluent::FluentButton searchBtn;
    fluent::FluentList list;
    fluent::FluentLabel statusLabel;
    fluent::FluentLabel hintLabel;
    fluent::FluentButton okBtn;
    fluent::FluentButton cancelBtn;
    fluent::FluentButton advanceLyricBtn;
    fluent::FluentButton delayLyricBtn;
    LyricPreviewPanel preview;

    LyricProvider* provider = nullptr;
    std::wstring targetTitle;
    std::wstring targetArtist;
    int64_t targetDurationMs = 0;
    std::wstring targetNeteaseSongId;
    std::vector<SearchCandidate> candidates;
    std::vector<int> itemToCand; // 列表行号 -> candidates 下标（-1 为分组标题行）
    int selectedIdx = -1;
    std::vector<LyricLine> previewLines;
    SongInfo previewInfo;
    int64_t previewOffsetMs = 0;
    int64_t playbackPositionMs = 0;

    ManualSearchDialog::ApplyCallback onApply;

    void refreshTheme() {
        titleLabel.refreshTheme();
        subtitleLabel.refreshTheme();
        titleEdit.refreshTheme();
        artistEdit.refreshTheme();
        searchBtn.refreshTheme();
        list.refreshTheme();
        statusLabel.refreshTheme();
        hintLabel.refreshTheme();
        okBtn.refreshTheme();
        cancelBtn.refreshTheme();
        advanceLyricBtn.refreshTheme();
        delayLyricBtn.refreshTheme();
        preview.refreshTheme();
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
            fluent::styleDialogWindow(hwnd);
            createControls();
            layout();
            statusLabel.setText(L"请输入歌名和歌手，点击搜索。");
            return 0;
        case WM_SIZE:
            layout();
            // 分层子窗口移动后不会替父窗口擦除拉伸产生的新客户区，完整重绘父子窗口。
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            fluent::styleDialogWindow(hwnd);
            refreshTheme();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            // 分层子窗口需要一个稳定的不透明宿主底色，避免拉伸后暴露未初始化区域。
            fluent::paintDialogBackground(hwnd, hdc, false);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            fluent::paintDialogBackground(hwnd, reinterpret_cast<HDC>(wp), false);
            return 1;
        case WM_MOUSEWHEEL: {
            // 滚轮消息默认发往焦点窗口，这里按光标位置转发给列表或预览面板
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            HWND child = ChildWindowFromPoint(hwnd, pt);
            if (child == preview.hwnd())
                SendMessageW(preview.hwnd(), WM_MOUSEWHEEL, wp, lp);
            else if (child == list.hwnd())
                SendMessageW(list.hwnd(), WM_MOUSEWHEEL, wp, lp);
            return 0;
        }
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case kMsgCandidatesReady: {
            auto* payload = reinterpret_cast<std::vector<SearchCandidate>*>(lp);
            if (payload) {
                this->onCandidatesReady(*payload);
                delete payload;
            }
            return 0;
        }
        case kMsgPreviewLyricReady: {
            auto* payload = reinterpret_cast<std::tuple<int, bool, std::vector<LyricLine>, SongInfo>*>(lp);
            if (payload) {
                this->onPreviewLyricReady(std::get<0>(*payload), std::get<1>(*payload),
                                          std::get<2>(*payload), std::get<3>(*payload));
                delete payload;
            }
            return 0;
        }
        case WM_CLOSE:
            this->destroy();
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void createControls() {
        titleLabel.create(hwnd, kIdTitleLabel, L"手动搜索歌词", false, 20.0f, 600);
        subtitleLabel.create(hwnd, kIdSubtitleLabel, L"从多个来源选择并预览歌词", true, 13.0f, 400);
        titleEdit.create(hwnd, kIdTitleEdit, L"歌名");
        titleEdit.setText(targetTitle);
        artistEdit.create(hwnd, kIdArtistEdit, L"歌手");
        artistEdit.setText(targetArtist);
        searchBtn.create(hwnd, kIdSearchBtn, L"搜索", true);

        statusLabel.create(hwnd, kIdStatus, L"", false);
        hintLabel.create(hwnd, kIdHint,
                         L"YRC/LRC 为网易云歌词；QRC 为 QQ 原生逐字歌词；KRC 为酷狗逐字歌词",
                         true);

        list.create(hwnd, kIdCandidateList);

        okBtn.create(hwnd, kIdOkBtn, L"使用此歌词", true);
        okBtn.setEnabled(false);
        cancelBtn.create(hwnd, kIdCancelBtn, L"取消", false);
        advanceLyricBtn.create(hwnd, kIdAdvanceLyric, L"提前 0.5 秒", false);
        advanceLyricBtn.setEnabled(false);
        delayLyricBtn.create(hwnd, kIdDelayLyric, L"延后 0.5 秒", false);
        delayLyricBtn.setEnabled(false);

        preview.create(hwnd, inst, 0, 0, 0, 0);
    }

    void layout() {
        RECT rc;
        GetClientRect(hwnd, &rc);
        float s = fluent::dipScale(GetDpiForWindow(hwnd));
        auto px = [&](float dip) { return static_cast<int>(dip * s); };
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int pad = px(fluent::metrics::pagePadding);
        int gap = px(fluent::metrics::controlGap);
        int editH = px(fluent::metrics::controlHeight);
        int btnW = px(88);
        int statusH = px(22);

        int titleH = px(28.0f);
        int subtitleH = px(20.0f);
        titleLabel.move(pad, pad, w - pad * 2, titleH);
        subtitleLabel.move(pad, pad + titleH, w - pad * 2, subtitleH);

        // 顶部输入区：歌名 [编辑框] 歌手 [编辑框] [搜索]
        int topRowW = w - pad * 2;
        int editW = (topRowW - gap * 2 - btnW) / 2;
        int inputY = pad + titleH + subtitleH + px(fluent::metrics::sectionGap);
        titleEdit.move(pad, inputY, editW, editH);
        artistEdit.move(pad + editW + gap, inputY, editW, editH);
        searchBtn.move(w - pad - btnW, inputY, btnW, editH);

        int statusY = inputY + editH + px(fluent::metrics::compactGap);
        statusLabel.move(pad, statusY, w - pad * 2, statusH);

        int hintY = statusY + statusH + px(4);
        hintLabel.move(pad, hintY, w - pad * 2, statusH);

        int contentTop = hintY + statusH + px(fluent::metrics::sectionGap);
        int bottomH = pad + px(fluent::metrics::controlHeight) +
                      px(fluent::metrics::sectionGap);
        int contentH = h - contentTop - bottomH;
        int availableW = w - pad * 2 - gap;
        int listW = availableW * 2 / 5;
        int rightX = pad + listW + gap;
        int rightW = w - rightX - pad;

        // 左侧候选列表
        list.move(pad, contentTop, listW, contentH);
        // 右侧预览
        preview.move(rightX, contentTop, rightW, contentH);

        // 底部按钮
        int okW = px(120);
        int cancelW = px(88);
        int btnH = px(fluent::metrics::controlHeight);
        int btnY = h - pad - btnH;
        int timingW = px(100);
        advanceLyricBtn.move(pad, btnY, timingW, btnH);
        delayLyricBtn.move(pad + timingW + gap, btnY, timingW, btnH);
        okBtn.move(w - pad - okW - cancelW - gap, btnY, okW, btnH);
        cancelBtn.move(w - pad - cancelW, btnY, cancelW, btnH);
    }

    void onCommand(int id, int code) {
        if (id == kIdSearchBtn && code == BN_CLICKED) {
            this->doSearch();
        } else if (id == kIdCancelBtn && code == BN_CLICKED) {
            this->destroy();
        } else if (id == kIdOkBtn && code == BN_CLICKED) {
            this->applySelection();
        } else if (id == kIdAdvanceLyric && code == BN_CLICKED) {
            this->shiftPreviewTimes(-500);
        } else if (id == kIdDelayLyric && code == BN_CLICKED) {
            this->shiftPreviewTimes(500);
        } else if (id == kIdCandidateList && code == LBN_SELCHANGE) {
            this->onSelectionChanged();
        } else if ((id == kIdTitleEdit || id == kIdArtistEdit) && code == EN_KILLFOCUS) {
            // 无需处理，仅避免未命中分支告警
        }
    }

    void doSearch() {
        if (!provider)
            return;
        searchBtn.setEnabled(false);
        list.clear();
        preview.setLyrics({});
        selectedIdx = -1;
        previewLines.clear();
        previewOffsetMs = 0;
        candidates.clear();
        itemToCand.clear();
        okBtn.setEnabled(false);
        advanceLyricBtn.setEnabled(false);
        delayLyricBtn.setEnabled(false);
        statusLabel.setText(L"搜索中，请稍候…");

        std::wstring title = titleEdit.text();
        std::wstring artist = artistEdit.text();
        HWND hwndCopy = hwnd;
        provider->searchCandidatesAsync(title, artist,
            [hwndCopy](const std::vector<SearchCandidate>& result) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::vector<SearchCandidate>(result);
                PostMessageW(hwndCopy, kMsgCandidatesReady, 0, reinterpret_cast<LPARAM>(payload));
            });
    }

    void onCandidatesReady(const std::vector<SearchCandidate>& cands) {
        candidates = cands;
        itemToCand.clear();
        searchBtn.setEnabled(true);

        int yrcCount = 0, qrcCount = 0, krcCount = 0, lrcCount = 0;
        for (const auto& c : candidates) {
            if (c.source == LyricSource::Yrc) ++yrcCount;
            else if (c.source == LyricSource::Qrc) ++qrcCount;
            else if (c.source == LyricSource::Krc) ++krcCount;
            else ++lrcCount;
        }

        std::vector<fluent::FluentListItem> items;
        auto addHeader = [&](const wchar_t* text) {
            items.push_back({text, true});
            itemToCand.push_back(-1);
        };
        auto addItem = [&](int idx) {
            const auto& c = candidates[idx];
            items.push_back({c.name + L" - " + c.singer, false});
            itemToCand.push_back(idx);
        };
        if (yrcCount > 0) {
            addHeader(L"网易云歌词（YRC/LRC）");
            for (int i = 0; i < (int)candidates.size(); ++i)
                if (candidates[i].source == LyricSource::Yrc)
                    addItem(i);
        }
        if (krcCount > 0) {
            addHeader(L"酷狗逐字歌词（KRC）");
            for (int i = 0; i < (int)candidates.size(); ++i)
                if (candidates[i].source == LyricSource::Krc)
                    addItem(i);
        }
        if (qrcCount > 0) {
            addHeader(L"QQ 音乐逐字歌词（QRC）");
            for (int i = 0; i < (int)candidates.size(); ++i)
                if (candidates[i].source == LyricSource::Qrc)
                    addItem(i);
        }
        if (lrcCount > 0) {
            addHeader(L"QQ 音乐逐行歌词（LRC）");
            for (int i = 0; i < (int)candidates.size(); ++i)
                if (candidates[i].source == LyricSource::Lrc)
                    addItem(i);
        }
        list.setItems(std::move(items));

        if (candidates.empty()) {
            statusLabel.setText(L"未找到相关歌曲，请尝试更换关键词。");
        } else {
            statusLabel.setText(L"找到 " + std::to_wstring(yrcCount) + L" 条网易云（YRC/LRC）、 " +
                                std::to_wstring(krcCount) + L" 条酷狗逐字（KRC）、 " +
                                std::to_wstring(qrcCount) + L" 条 QQ 逐字（QRC）、 " +
                                std::to_wstring(lrcCount) + L" 条 QQ 逐行（LRC）候选，点击选择以预览歌词。");
            // 默认选中第一条非分组标题的候选
            for (int i = 0; i < (int)itemToCand.size(); ++i) {
                if (itemToCand[i] >= 0) {
                    list.setSelectedIndex(i);
                    break;
                }
            }
            this->onSelectionChanged();
        }
    }

    void onSelectionChanged() {
        int listIdx = list.selectedIndex();
        if (listIdx < 0 || listIdx >= (int)itemToCand.size())
            return;
        int idx = itemToCand[listIdx];
        if (idx < 0)
            return; // 分组标题不可选（FluentList 已保证不会选中标题行）
        selectedIdx = idx;
        preview.setLyrics({});
        previewLines.clear();
        previewOffsetMs = 0;
        okBtn.setEnabled(false);
        advanceLyricBtn.setEnabled(false);
        delayLyricBtn.setEnabled(false);
        statusLabel.setText(L"正在加载《" + candidates[idx].name + L"》的歌词预览…");
        HWND hwndCopy = hwnd;
        int idxCopy = idx;
        provider->fetchLyricAsync(candidates[idx],
            [hwndCopy, idxCopy](bool ok, const std::vector<LyricLine>& lines, const SongInfo& info) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(
                    idxCopy, ok, lines, info);
                PostMessageW(hwndCopy, kMsgPreviewLyricReady, 0,
                             reinterpret_cast<LPARAM>(payload));
            });
    }

    void onPreviewLyricReady(int idx, bool ok, const std::vector<LyricLine>& lines,
                             const SongInfo& info) {
        if (idx != selectedIdx || !ok) {
            if (idx == selectedIdx)
                statusLabel.setText(L"该候选没有可用歌词，请尝试其他歌曲。");
            return;
        }
        previewLines = lines;
        previewInfo = info;
        previewOffsetMs = 0;
        preview.setLyrics(previewLines);
        okBtn.setEnabled(true);
        advanceLyricBtn.setEnabled(true);
        delayLyricBtn.setEnabled(true);
        // QRC/KRC/YRC 通常带逐字时间轴，LRC 或没有逐字数据时显示整行时间轴。
        bool wordByWord = false;
        for (const auto& l : lines) {
            if (!l.chars.empty()) {
                wordByWord = true;
                break;
            }
        }
        statusLabel.setText(L"已加载《" + candidates[idx].name +
                            (wordByWord ? L"》的逐字歌词预览，点击“使用此歌词”应用。"
                                        : L"》的整行歌词预览，点击“使用此歌词”应用。"));
    }

    void applySelection() {
        if (selectedIdx < 0 || selectedIdx >= (int)candidates.size())
            return;
        if (previewLines.empty())
            return;
        if (provider) {
            SongInfo info = previewInfo;
            if (!targetNeteaseSongId.empty())
                info.neteaseSongId = targetNeteaseSongId;
            provider->setManualOverride(targetTitle, targetArtist, targetDurationMs,
                                          std::vector<LyricLine>(previewLines), info);
        }
        if (onApply)
            onApply();
        this->destroy();
    }

    void shiftPreviewTimes(int64_t deltaMs) {
        if (previewLines.empty())
            return;
        previewOffsetMs += deltaMs;
        for (auto& line : previewLines) {
            line.ms += deltaMs;
            for (auto& ch : line.chars) {
                ch.startMs += deltaMs;
                ch.endMs += deltaMs;
            }
        }
        preview.setLyrics(previewLines);
        preview.setPosition(playbackPositionMs);
        std::wstring offsetText = previewOffsetMs >= 0 ? L"+" : L"";
        offsetText += std::to_wstring(previewOffsetMs / 1000);
        std::wstring status = L"预览时间已调整 ";
        status += offsetText;
        status += L" 秒，确认后点击“使用此歌词”。";
        statusLabel.setText(status);
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

ManualSearchDialog::ManualSearchDialog() : impl_(std::make_unique<Impl>()) {}

ManualSearchDialog::~ManualSearchDialog() {
    destroy();
}

bool ManualSearchDialog::create(HINSTANCE inst, HWND parent, LyricProvider* provider,
                                const std::wstring& targetTitle,
                                const std::wstring& targetArtist,
                                int64_t targetDurationMs,
                                const std::wstring& targetNeteaseSongId) {
    impl_->inst = inst;
    impl_->provider = provider;
    impl_->targetTitle = targetTitle;
    impl_->targetArtist = targetArtist;
    impl_->targetDurationMs = targetDurationMs;
    impl_->targetNeteaseSongId = targetNeteaseSongId;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricManualSearch";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    // 期望的客户区尺寸，按标题栏/边框反推窗口整体尺寸
    RECT rc{0, 0, static_cast<LONG>(std::lround(920 * s)),
            static_cast<LONG>(std::lround(640 * s))};
    AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE,
                             WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd =
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, L"QQMusicLyricManualSearch",
                        L"手动搜索歌词", WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y,
                        w, h, nullptr, nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;
    return true;
}

void ManualSearchDialog::show() {
    if (impl_->hwnd)
        ShowWindow(impl_->hwnd, SW_SHOW);
}

void ManualSearchDialog::destroy() {
    impl_->destroy();
}

bool ManualSearchDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND ManualSearchDialog::hwnd() const {
    return impl_->hwnd;
}

void ManualSearchDialog::onCandidatesReady(const std::vector<SearchCandidate>& cands) {
    if (impl_->hwnd)
        PostMessageW(impl_->hwnd, kMsgCandidatesReady, 0,
                     reinterpret_cast<LPARAM>(new std::vector<SearchCandidate>(cands)));
}

void ManualSearchDialog::onPreviewLyricReady(int idx, bool ok,
                                             const std::vector<LyricLine>& lines,
                                             const SongInfo& info) {
    if (impl_->hwnd)
        PostMessageW(impl_->hwnd, kMsgPreviewLyricReady, 0,
                     reinterpret_cast<LPARAM>(
                         new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(idx, ok, lines, info)));
}

void ManualSearchDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}

void ManualSearchDialog::setPlaybackPosition(int64_t positionMs) {
    impl_->playbackPositionMs = positionMs;
    impl_->preview.setPosition(positionMs);
}
