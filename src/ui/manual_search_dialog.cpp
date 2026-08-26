#include "ui/manual_search_dialog.h"

#include "ui/app_icon.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "logging/runtime_logger.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr int kIdTitleEdit = 101;
constexpr int kIdArtistEdit = 102;
constexpr int kIdSearchButton = 103;
constexpr int kIdCandidateList = 104;
constexpr int kIdApplyButton = 105;
constexpr int kIdCancelButton = 106;
constexpr int kIdPreview = 107;
constexpr int kIdPreviewRomanization = 108;
constexpr int kIdPreviewTranslation = 109;
constexpr int kIdAdvanceButton = 110;
constexpr int kIdDelayButton = 111;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

constexpr float kMinClientWidthDip = 520.0f;
constexpr float kMinClientHeightDip = 504.0f;
constexpr float kMinClientAspectRatio = kMinClientWidthDip / kMinClientHeightDip;

constexpr float kCandidateHeaderHeight = 28.0f;
constexpr float kCandidateRowHeight = 40.0f;
constexpr float kScrollBarWidth = 3.0f;
constexpr float kScrollBarHitWidth = 12.0f;
constexpr float kScrollBarInset = 8.0f;

constexpr UINT kPreviewTimerId = 1;
constexpr DWORD kPreviewResumeDelayMs = 2000;
constexpr int kPreviewWheelLinesPerNotch = 3;
constexpr float kLyricTextWidthDip = 240.0f;
constexpr float kLyricRowHeightDip = 50.0f;
constexpr float kPreviewSecondaryLineHeight = 18.0f;
constexpr float kPreviewSecondaryGap = 2.0f;
constexpr float kPreviewToggleSize = 28.0f;
constexpr float kPreviewToggleGap = 6.0f;
constexpr float kPreviewToggleMargin = 10.0f;
constexpr float kPreviewToggleTextGap = 10.0f;

constexpr UINT kMsgCandidatesReady = WM_APP + 10;
constexpr UINT kMsgPreviewLyricReady = WM_APP + 11;

void releaseResultPayload(const MSG& msg) {
    if (msg.message == kMsgCandidatesReady) {
        delete reinterpret_cast<std::vector<SearchCandidate>*>(msg.lParam);
    } else if (msg.message == kMsgPreviewLyricReady) {
        delete reinterpret_cast<std::tuple<int, bool, std::vector<LyricLine>, SongInfo>*>(
            msg.lParam);
    }
}

void discardPendingResults(HWND hwnd) {
    if (!hwnd)
        return;
    MSG msg{};
    while (PeekMessageW(&msg, hwnd, kMsgCandidatesReady, kMsgPreviewLyricReady, PM_REMOVE))
        releaseResultPayload(msg);
}

struct ScrollBarGeometry {
    float trackTop = 0.0f;
    float thumbHeight = 0.0f;
    float usable = 0.0f;
};

ScrollBarGeometry scrollBarGeometry(float viewHeight, float contentHeight) {
    const float trackHeight = std::max(0.0f, viewHeight - kScrollBarInset * 2.0f);
    const float thumbHeight = std::min(
        trackHeight, std::max(20.0f, trackHeight * viewHeight / std::max(1.0f, contentHeight)));
    return {kScrollBarInset, thumbHeight, std::max(0.0f, trackHeight - thumbHeight)};
}

float scrollBarThumbY(const ScrollBarGeometry& bar, float scrollY, float maxScroll) {
    if (maxScroll <= 0.0f || bar.usable <= 0.0f)
        return bar.trackTop;
    return bar.trackTop + scrollY / maxScroll * bar.usable;
}

} // namespace

struct ManualSearchDialog::Impl {
    struct TextField {
        std::wstring text;
        std::wstring cue;
        size_t caret = 0;
        size_t anchor = 0;
    };

    struct CandidateItem {
        std::wstring text;
        int candidateIndex = -1; // -1 表示分组标题
        bool header = false;
    };

    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;

    fluent::FluentDialogSurface surface;

    TextField titleEdit;
    TextField artistEdit;
    std::wstring statusText;
    bool searching = false;

    LyricProvider* provider = nullptr;
    std::wstring targetTitle;
    std::wstring targetArtist;
    int64_t targetDurationMs = 0;
    std::wstring targetNeteaseSongId;

    std::vector<SearchCandidate> candidates;
    std::vector<CandidateItem> candidateItems;
    int selectedItemRow = -1;
    int selectedIdx = -1;
    int hoverItemRow = -1;

    std::vector<LyricLine> previewLines;
    SongInfo previewInfo;
    std::wstring previewCapabilityLabel;
    bool previewHasTranslation = false;
    bool previewHasRomanization = false;
    bool previewShowTranslation = false;
    bool previewShowRomanization = false;
    int64_t previewOffsetMs = 0;
    int64_t playbackPositionMs = 0;
    int64_t previewPositionMs = 0;
    int previewTopLine = 0;
    int previewCurrentLine = -1;
    bool previewManualScroll = false;
    DWORD previewLastScrollTick = 0;
    int previewWheelAccum = 0;

    float candidateScroll = 0.0f;
    int candidateWheelAccum = 0;
    bool candidateScrollDragging = false;
    float candidateScrollGrabDy = 0.0f;

    D2D1_RECT_F titleRect{};
    D2D1_RECT_F subtitleRect{};
    D2D1_RECT_F titleEditRect{};
    D2D1_RECT_F artistEditRect{};
    D2D1_RECT_F searchRect{};
    D2D1_RECT_F statusRect{};
    D2D1_RECT_F hintRect{};
    D2D1_RECT_F compatibilityRect{};
    D2D1_RECT_F candidateListRect{};
    D2D1_RECT_F previewRect{};
    D2D1_RECT_F previewRomanizationRect{};
    D2D1_RECT_F previewTranslationRect{};
    D2D1_RECT_F advanceRect{};
    D2D1_RECT_F delayRect{};
    D2D1_RECT_F applyRect{};
    D2D1_RECT_F cancelRect{};

    int hoverId = 0;
    int pressedId = 0;
    int focusedId = kIdTitleEdit;
    bool focusVisible = false;

    ManualSearchDialog::ApplyCallback onApply;

    ~Impl() = default;

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

    static bool isCtrlDown() {
        return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    static bool isShiftDown() {
        return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    TextField* editFor(int id) {
        if (id == kIdTitleEdit)
            return &titleEdit;
        if (id == kIdArtistEdit)
            return &artistEdit;
        return nullptr;
    }

    const TextField* editFor(int id) const {
        if (id == kIdTitleEdit)
            return &titleEdit;
        if (id == kIdArtistEdit)
            return &artistEdit;
        return nullptr;
    }

    static size_t selectionStart(const TextField& field) {
        return std::min(field.caret, field.anchor);
    }

    static size_t selectionEnd(const TextField& field) {
        return std::max(field.caret, field.anchor);
    }

    static void collapseSelection(TextField& field, bool toEnd) {
        const size_t position = toEnd ? selectionEnd(field) : selectionStart(field);
        field.caret = position;
        field.anchor = position;
    }

    void moveCaret(TextField& field, size_t next, bool extend) {
        next = std::min(next, field.text.size());
        if (extend)
            field.caret = next;
        else
            field.caret = field.anchor = next;
        surface.invalidate();
    }

    void replaceSelection(TextField& field, const std::wstring& inserted) {
        const size_t start = selectionStart(field);
        const size_t end = selectionEnd(field);
        std::wstring accepted;
        for (wchar_t character : inserted) {
            if (character >= L' ' && character != 0x7f)
                accepted.push_back(character);
        }

        field.text.erase(start, end - start);
        field.text.insert(start, accepted);
        field.caret = start + accepted.size();
        field.anchor = field.caret;
    }

    void copySelection(const TextField& field) const {
        const size_t start = selectionStart(field);
        const size_t end = selectionEnd(field);
        if (start == end || !OpenClipboard(hwnd))
            return;

        EmptyClipboard();
        const std::wstring text = field.text.substr(start, end - start);
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* data = GlobalLock(memory);
            if (data) {
                std::memcpy(data, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory))
                    GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    std::wstring clipboardText() const {
        std::wstring result;
        if (!OpenClipboard(hwnd))
            return result;
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text)
                result = text;
            if (text)
                GlobalUnlock(handle);
        }
        CloseClipboard();
        return result;
    }

    void editChanged() {
        if (hwnd)
            surface.invalidate();
    }

    bool handleEditKey(WPARAM key) {
        TextField* field = editFor(focusedId);
        if (!field)
            return false;

        const bool ctrl = isCtrlDown();
        const bool shift = isShiftDown();
        if (ctrl && (key == 'A' || key == 'a')) {
            field->anchor = 0;
            field->caret = field->text.size();
            surface.invalidate();
            return true;
        }
        if (ctrl && (key == 'C' || key == 'c')) {
            copySelection(*field);
            return true;
        }
        if (ctrl && (key == 'X' || key == 'x')) {
            copySelection(*field);
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"");
                editChanged();
            }
            return true;
        }
        if (ctrl && (key == 'V' || key == 'v')) {
            replaceSelection(*field, clipboardText());
            editChanged();
            return true;
        }

        switch (key) {
        case VK_LEFT:
            if (!shift && selectionStart(*field) != selectionEnd(*field))
                collapseSelection(*field, false);
            else if (field->caret > 0)
                moveCaret(*field, field->caret - 1, shift);
            else
                surface.invalidate();
            return true;
        case VK_RIGHT:
            if (!shift && selectionStart(*field) != selectionEnd(*field))
                collapseSelection(*field, true);
            else if (field->caret < field->text.size())
                moveCaret(*field, field->caret + 1, shift);
            else
                surface.invalidate();
            return true;
        case VK_HOME:
            moveCaret(*field, 0, shift);
            return true;
        case VK_END:
            moveCaret(*field, field->text.size(), shift);
            return true;
        case VK_BACK:
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"");
            } else if (field->caret > 0) {
                const size_t start = field->caret - 1;
                field->text.erase(start, 1);
                field->caret = field->anchor = start;
            }
            editChanged();
            return true;
        case VK_DELETE:
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"");
            } else if (field->caret < field->text.size()) {
                field->text.erase(field->caret, 1);
            }
            editChanged();
            return true;
        default:
            return false;
        }
    }

    bool handleCharacter(wchar_t character) {
        TextField* field = editFor(focusedId);
        if (!field || character < L' ' || character == 0x7f)
            return false;
        replaceSelection(*field, std::wstring(1, character));
        editChanged();
        return true;
    }

    void createControls() {
        titleEdit.cue = L"歌名";
        titleEdit.text = targetTitle;
        titleEdit.caret = titleEdit.anchor = titleEdit.text.size();
        artistEdit.cue = L"歌手";
        artistEdit.text = targetArtist;
        artistEdit.caret = artistEdit.anchor = artistEdit.text.size();

        statusText = L"请输入歌名和歌手，点击搜索。";
        searching = false;
        focusedId = kIdTitleEdit;
        focusVisible = false;
        candidateScroll = 0.0f;
        candidateWheelAccum = 0;
        candidateScrollDragging = false;
        previewWheelAccum = 0;
        resetPreviewView();
    }

    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float scale = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / scale);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / scale);
        const float pad = fluent::metrics::pagePadding;
        const float gap = fluent::metrics::controlGap;
        const float compactGap = fluent::metrics::compactGap;
        const float controlH = fluent::metrics::controlHeight;

        const float titleH = 30.0f;
        const float subtitleH = 20.0f;
        titleRect = D2D1::RectF(pad, pad, std::max(pad, w - pad), pad + titleH);
        subtitleRect = D2D1::RectF(pad, pad + titleH, std::max(pad, w - pad),
                                    pad + titleH + subtitleH);

        const float inputY = pad + titleH + subtitleH + fluent::metrics::sectionGap;
        const float searchW = 88.0f;
        const float inputW = std::max(40.0f, (w - pad * 2.0f - gap * 2.0f - searchW) / 2.0f);
        titleEditRect = D2D1::RectF(pad, inputY, pad + inputW, inputY + controlH);
        artistEditRect = D2D1::RectF(pad + inputW + gap, inputY,
                                     pad + inputW * 2.0f + gap, inputY + controlH);
        searchRect = D2D1::RectF(w - pad - searchW, inputY, w - pad, inputY + controlH);

        const float statusY = inputY + controlH + compactGap;
        const float statusH = 22.0f;
        statusRect = D2D1::RectF(pad, statusY, std::max(pad, w - pad), statusY + statusH);
        const float hintY = statusY + statusH + 4.0f;
        hintRect = D2D1::RectF(pad, hintY, std::max(pad, w - pad), hintY + statusH);
        const float compatibilityY = hintY + statusH + 2.0f;
        compatibilityRect = D2D1::RectF(pad, compatibilityY, std::max(pad, w - pad),
                                          compatibilityY + statusH);

        const float contentTop = compatibilityY + statusH + fluent::metrics::sectionGap;
        const float buttonH = controlH;
        const float buttonY = std::max(contentTop, h - pad - buttonH);
        const float contentBottom = buttonY - fluent::metrics::sectionGap;
        const float contentH = std::max(0.0f, contentBottom - contentTop);
        const float availableW = std::max(0.0f, w - pad * 2.0f - gap);
        const float listW = availableW * 0.4f;
        const float rightX = pad + listW + gap;
        const float rightW = std::max(0.0f, w - rightX - pad);
        candidateListRect = D2D1::RectF(pad, contentTop, pad + listW, contentTop + contentH);
        previewRect = D2D1::RectF(rightX, contentTop, rightX + rightW, contentTop + contentH);
        previewRomanizationRect = {};
        previewTranslationRect = {};
        const int previewToggleCount = (previewHasRomanization ? 1 : 0) +
                                       (previewHasTranslation ? 1 : 0);
        if (previewToggleCount > 0) {
            const float toggleRailHeight =
                previewToggleCount * kPreviewToggleSize +
                (previewToggleCount - 1) * kPreviewToggleGap;
            const float toggleBottom = previewRect.bottom - kPreviewToggleMargin;
            const float toggleTop =
                std::max(previewRect.top + previewContentTop(), toggleBottom - toggleRailHeight);
            const float toggleRight = previewRect.right - kPreviewToggleMargin;
            float nextTop = toggleTop;
            if (previewHasRomanization) {
                previewRomanizationRect =
                    D2D1::RectF(toggleRight - kPreviewToggleSize, nextTop, toggleRight,
                                 nextTop + kPreviewToggleSize);
                nextTop += kPreviewToggleSize + kPreviewToggleGap;
            }
            if (previewHasTranslation) {
                previewTranslationRect =
                    D2D1::RectF(toggleRight - kPreviewToggleSize, nextTop, toggleRight,
                                 nextTop + kPreviewToggleSize);
            }
        }

        const float timingW = 100.0f;
        advanceRect = D2D1::RectF(pad, buttonY, pad + timingW, buttonY + buttonH);
        delayRect = D2D1::RectF(pad + timingW + gap, buttonY,
                                pad + timingW + gap + timingW, buttonY + buttonH);
        const float cancelW = 88.0f;
        const float applyW = 120.0f;
        cancelRect = D2D1::RectF(w - pad - cancelW, buttonY, w - pad, buttonY + buttonH);
        applyRect = D2D1::RectF(w - pad - cancelW - gap - applyW, buttonY,
                                w - pad - cancelW - gap, buttonY + buttonH);
        candidateScroll = std::clamp(candidateScroll, 0.0f, candidateMaxScroll());
        syncPreviewToCurrentLine();
    }

    float candidateItemHeight(int row) const {
        if (row < 0 || row >= static_cast<int>(candidateItems.size()))
            return 0.0f;
        return candidateItems[row].header ? kCandidateHeaderHeight : kCandidateRowHeight;
    }

    float candidateContentHeight() const {
        float height = 0.0f;
        for (int i = 0; i < static_cast<int>(candidateItems.size()); ++i)
            height += candidateItemHeight(i);
        return height;
    }

    float candidateMaxScroll() const {
        return std::max(0.0f, candidateContentHeight() -
                                  (candidateListRect.bottom - candidateListRect.top));
    }

    int candidateRowAt(float y) const {
        if (y < candidateListRect.top || y > candidateListRect.bottom)
            return -1;
        float localY = y - candidateListRect.top + candidateScroll;
        if (localY < 0.0f)
            return -1;
        for (int row = 0; row < static_cast<int>(candidateItems.size()); ++row) {
            const float rowH = candidateItemHeight(row);
            if (localY < rowH)
                return row;
            localY -= rowH;
        }
        return -1;
    }

    bool candidateHasScroll() const {
        return candidateContentHeight() > candidateListRect.bottom - candidateListRect.top;
    }

    bool isCandidateScrollBarHit(float x) const {
        return candidateHasScroll() && x >= candidateListRect.right - kScrollBarHitWidth;
    }

    void scrollCandidatesBy(int delta) {
        candidateWheelAccum += delta;
        const int notches = candidateWheelAccum / WHEEL_DELTA;
        if (notches == 0)
            return;
        candidateWheelAccum -= notches * WHEEL_DELTA;
        candidateScroll = std::clamp(candidateScroll - notches * 3.0f * kCandidateRowHeight,
                                     0.0f, candidateMaxScroll());
        surface.invalidate();
    }

    void ensureCandidateVisible(int row) {
        if (row < 0 || row >= static_cast<int>(candidateItems.size()))
            return;
        float top = 0.0f;
        for (int i = 0; i < row; ++i)
            top += candidateItemHeight(i);
        const float bottom = top + candidateItemHeight(row);
        const float viewHeight = candidateListRect.bottom - candidateListRect.top;
        if (top < candidateScroll)
            candidateScroll = top;
        else if (bottom > candidateScroll + viewHeight)
            candidateScroll = bottom - viewHeight;
        candidateScroll = std::clamp(candidateScroll, 0.0f, candidateMaxScroll());
    }

    int nextCandidateRow(int from, int direction) const {
        int row = from;
        while (true) {
            row += direction;
            if (row < 0 || row >= static_cast<int>(candidateItems.size()))
                return -1;
            if (!candidateItems[row].header && candidateItems[row].candidateIndex >= 0)
                return row;
        }
    }

    bool beginCandidateScrollDrag(float x, float y) {
        if (!isCandidateScrollBarHit(x))
            return false;
        const float viewHeight = candidateListRect.bottom - candidateListRect.top;
        const auto bar = scrollBarGeometry(viewHeight, candidateContentHeight());
        const float thumbY = candidateListRect.top +
                             scrollBarThumbY(bar, candidateScroll, candidateMaxScroll());
        if (y >= thumbY && y <= thumbY + bar.thumbHeight) {
            candidateScrollDragging = true;
            candidateScrollGrabDy = y - thumbY;
            SetCapture(hwnd);
        } else {
            candidateScroll = std::clamp(
                candidateScroll + (y < thumbY ? -viewHeight : viewHeight), 0.0f,
                candidateMaxScroll());
            surface.invalidate();
            SetCapture(hwnd);
        }
        return true;
    }

    void updateCandidateScrollDrag(float y) {
        if (!candidateScrollDragging)
            return;
        const float viewHeight = candidateListRect.bottom - candidateListRect.top;
        const auto bar = scrollBarGeometry(viewHeight, candidateContentHeight());
        if (bar.usable > 0.0f) {
            const float localY = y - candidateListRect.top;
            candidateScroll = std::clamp(
                (localY - bar.trackTop - candidateScrollGrabDy) / bar.usable *
                    candidateMaxScroll(),
                0.0f, candidateMaxScroll());
        }
        surface.invalidate();
    }

    void updatePreviewCapability() {
        previewHasTranslation = false;
        previewHasRomanization = false;
        for (const auto& line : previewLines) {
            previewHasTranslation = previewHasTranslation || !line.translation.empty();
            previewHasRomanization = previewHasRomanization || !line.romanization.empty();
        }
        previewShowTranslation = previewShowTranslation && previewHasTranslation;
        previewShowRomanization = previewShowRomanization && previewHasRomanization;
        if (previewHasTranslation && previewHasRomanization)
            previewCapabilityLabel = L"[支持翻译 + 罗马音]";
        else if (previewHasTranslation)
            previewCapabilityLabel = L"[支持翻译]";
        else if (previewHasRomanization)
            previewCapabilityLabel = L"[支持罗马音]";
        else
            previewCapabilityLabel.clear();
    }

    float previewContentTop() const {
        return previewCapabilityLabel.empty() ? 0.0f : 27.0f;
    }

    int previewSecondaryLineCount() const {
        return (previewShowRomanization ? 1 : 0) + (previewShowTranslation ? 1 : 0);
    }

    float previewSecondaryHeight() const {
        return previewSecondaryLineCount() *
               (kPreviewSecondaryLineHeight + kPreviewSecondaryGap);
    }

    float previewLyricRowHeight() const {
        return kLyricRowHeightDip + previewSecondaryHeight();
    }

    float measurePreviewMainHeight(fluent::FluentDialogSurface::Painter& painter,
                                   const std::wstring& text, float width) const {
        if (text.empty())
            return kLyricRowHeightDip;

        auto* format = painter.textFormat(15.0f, 700, true);
        auto* layout = painter.textLayout(text, format, width, 10000.0f);
        if (!layout)
            return kLyricRowHeightDip;

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)))
            return kLyricRowHeightDip;
        return std::max(kLyricRowHeightDip,
                        static_cast<float>(std::ceil(std::max(0.0f, metrics.height))));
    }

    int visiblePreviewRows(float height) const {
        const float availableHeight = std::max(0.0f, height - previewContentTop());
        return std::max(1, static_cast<int>(std::floor(availableHeight / previewLyricRowHeight())));
    }

    int visiblePreviewRows() const {
        return visiblePreviewRows(std::max(0.0f, previewRect.bottom - previewRect.top));
    }

    void syncPreviewToCurrentLine() {
        const int total = static_cast<int>(previewLines.size());
        if (total == 0) {
            previewTopLine = 0;
            return;
        }
        const int visible = visiblePreviewRows();
        const int maxTop = std::max(0, total - 1);
        if (previewCurrentLine < 0)
            previewTopLine = 0;
        else
            previewTopLine = std::clamp(previewCurrentLine - visible / 2, 0, maxTop);
    }

    void resetPreviewView() {
        previewPositionMs = playbackPositionMs;
        previewCurrentLine = LyricProvider::findLine(previewLines, previewPositionMs);
        previewTopLine = 0;
        previewManualScroll = false;
        previewLastScrollTick = 0;
        previewWheelAccum = 0;
        syncPreviewToCurrentLine();
    }

    void setPreviewLines(const std::vector<LyricLine>& lines) {
        previewLines = lines;
        updatePreviewCapability();
        resetPreviewView();
        layout();
        surface.invalidate();
    }

    void setPreviewPosition(int64_t positionMs) {
        previewPositionMs = positionMs;
        previewCurrentLine = LyricProvider::findLine(previewLines, positionMs);
        if (!previewManualScroll)
            syncPreviewToCurrentLine();
        if (hwnd)
            surface.invalidate();
    }

    void scrollPreviewBy(int lines) {
        if (previewLines.empty())
            return;
        const int maxTop = std::max(0, static_cast<int>(previewLines.size()) - 1);
        previewTopLine = std::clamp(previewTopLine + lines, 0, maxTop);
        previewManualScroll = true;
        previewLastScrollTick = GetTickCount();
        surface.invalidate();
    }

    void scrollPreviewByWheel(int delta) {
        previewWheelAccum += delta;
        const int notches = previewWheelAccum / WHEEL_DELTA;
        if (notches == 0)
            return;
        previewWheelAccum -= notches * WHEEL_DELTA;
        scrollPreviewBy(-notches * kPreviewWheelLinesPerNotch);
    }

    void onPreviewTimer() {
        if (previewManualScroll &&
            GetTickCount() - previewLastScrollTick >= kPreviewResumeDelayMs) {
            previewManualScroll = false;
            syncPreviewToCurrentLine();
            surface.invalidate();
        }
    }

    std::vector<D2D1_RECT_F> karaokeHighlightClips(IDWriteTextLayout* layout,
                                                   const LyricLine& line) const {
        std::vector<D2D1_RECT_F> clips;
        if (!layout || line.chars.empty() || line.text.empty())
            return clips;

        const UINT32 textLength = static_cast<UINT32>(line.text.size());
        UINT32 textOffset = 0;
        auto appendClip = [&clips](const DWRITE_HIT_TEST_METRICS& metrics, float width) {
            if (width <= 0.0f || metrics.height <= 0.0f)
                return;
            const D2D1_RECT_F clip =
                D2D1::RectF(metrics.left, metrics.top, metrics.left + width,
                             metrics.top + metrics.height);
            // 同一视觉行的已唱片段合并成一块，避免每个字都 Push/Pop 一次裁剪区域。
            for (auto& existing : clips) {
                if (std::fabs(existing.top - clip.top) < 0.5f &&
                    std::fabs(existing.bottom - clip.bottom) < 0.5f) {
                    existing.left = std::min(existing.left, clip.left);
                    existing.right = std::max(existing.right, clip.right);
                    return;
                }
            }
            clips.push_back(clip);
        };

        std::vector<DWRITE_HIT_TEST_METRICS> metrics;
        for (const auto& character : line.chars) {
            const UINT32 tokenLength = static_cast<UINT32>(character.text.size());
            if (tokenLength == 0)
                continue;
            if (textOffset >= textLength || character.startMs > previewPositionMs)
                break;

            const UINT32 rangeLength = std::min(tokenLength, textLength - textOffset);
            const int64_t durationMs =
                std::max<int64_t>(character.endMs - character.startMs, 1);
            const float fraction = static_cast<float>(std::clamp(
                static_cast<double>(previewPositionMs - character.startMs) /
                    static_cast<double>(durationMs),
                0.0, 1.0));
            if (fraction > 0.0f && rangeLength > 0) {
                metrics.resize(rangeLength + 1);
                UINT32 actualCount = 0;
                if (SUCCEEDED(layout->HitTestTextRange(
                        textOffset, rangeLength, 0.0f, 0.0f, metrics.data(),
                        static_cast<UINT32>(metrics.size()), &actualCount))) {
                    float totalWidth = 0.0f;
                    for (UINT32 i = 0; i < actualCount; ++i)
                        totalWidth += std::max(0.0f, metrics[i].width);

                    float remainingWidth = totalWidth * fraction;
                    for (UINT32 i = 0; i < actualCount && remainingWidth > 0.0f; ++i) {
                        const float metricWidth = std::max(0.0f, metrics[i].width);
                        if (metricWidth <= 0.0f)
                            continue;
                        const float highlightWidth = std::min(metricWidth, remainingWidth);
                        appendClip(metrics[i], highlightWidth);
                        remainingWidth -= metricWidth;
                    }
                }
            }
            textOffset += tokenLength;
        }
        return clips;
    }

    void drawCurrentLyric(fluent::FluentDialogSurface::Painter& painter, const LyricLine& line,
                          const D2D1_RECT_F& rect) {
        const auto& p = fluent::palette();
        auto* format = painter.textFormat(15.0f, 700, true);
        if (!format)
            return;

        if (line.chars.empty()) {
            painter.drawText(line.text, format, rect, fluent::toD2D(RGB(49, 194, 124)));
            return;
        }

        auto* layout = painter.textLayout(line.text, format, rect.right - rect.left,
                                          rect.bottom - rect.top);
        if (!layout)
            return;

        const D2D1_POINT_2F origin = D2D1::Point2F(rect.left, rect.top);
        painter.drawTextLayout(layout, origin, p.textSecondary);
        for (const auto& clip : karaokeHighlightClips(layout, line)) {
            painter.target()->PushAxisAlignedClip(
                D2D1::RectF(rect.left + clip.left, rect.top + clip.top,
                            std::min(rect.right, rect.left + clip.right),
                            std::min(rect.bottom, rect.top + clip.bottom)),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            painter.drawTextLayout(layout, origin, fluent::toD2D(RGB(49, 194, 124)));
            painter.target()->PopAxisAlignedClip();
        }
    }

    void drawPreviewLyricLine(fluent::FluentDialogSurface::Painter& painter,
                              const LyricLine& line, const D2D1_RECT_F& rowRect,
                              float mainHeight, bool current) {
        const auto& p = fluent::palette();
        const int secondaryCount = previewSecondaryLineCount();
        const float contentHeight =
            mainHeight + secondaryCount * (kPreviewSecondaryLineHeight + kPreviewSecondaryGap);
        const float top = rowRect.top +
                          std::max(0.0f, (rowRect.bottom - rowRect.top - contentHeight) * 0.5f);
        const D2D1_RECT_F mainRect =
            D2D1::RectF(rowRect.left, top, rowRect.right, top + mainHeight);
        if (current) {
            drawCurrentLyric(painter, line, mainRect);
        } else {
            painter.drawText(line.text, painter.textFormat(14.0f, 400, true), mainRect, p.text);
        }

        float secondaryTop = mainRect.bottom + kPreviewSecondaryGap;
        auto drawSecondary = [&](const std::wstring& text) {
            if (!text.empty()) {
                painter.drawTrimmedText(
                    text, painter.textFormat(12.0f, 400, true, true),
                    D2D1::RectF(rowRect.left, secondaryTop, rowRect.right,
                                secondaryTop + kPreviewSecondaryLineHeight),
                    p.textSecondary);
            }
            secondaryTop += kPreviewSecondaryLineHeight + kPreviewSecondaryGap;
        };
        if (previewShowRomanization)
            drawSecondary(line.romanization);
        if (previewShowTranslation)
            drawSecondary(line.translation);
    }

    void drawPreviewToggleButton(fluent::FluentDialogSurface::Painter& painter,
                                 const D2D1_RECT_F& rect, const wchar_t* label, bool checked,
                                 int id) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        const bool showFill = hovered || pressed;
        const D2D1_COLOR_F border = checked || hovered || pressed ? p.accent : p.cardStroke;
        const D2D1_COLOR_F textColor = checked ? p.accent : (hovered ? p.text : p.textSecondary);
        if (showFill) {
            painter.fillRoundRect(pressed ? p.controlPressed : p.controlHover, rect,
                                  fluent::metrics::controlRadius);
        }
        painter.strokeRoundRect(border, rect, 1.0f, fluent::metrics::controlRadius);
        if (focusedId == id && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
        }
        painter.drawText(label, painter.textFormat(13.0f, 600, true, true), rect, textColor);
    }

    void drawTextField(fluent::FluentDialogSurface::Painter& painter,
                       const D2D1_RECT_F& rect, const TextField& field, int id) {
        const auto& p = fluent::palette();
        painter.fillRoundRect(p.cardFill, rect);
        painter.strokeRoundRect(p.cardStroke, rect);
        if (focusedId == id && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, fluent::metrics::controlRadius - 1.0f);
        }

        auto* format = painter.textFormat(14.0f, 400, false, true);
        if (!format)
            return;
        const D2D1_RECT_F textRect =
            D2D1::RectF(rect.left + 12.0f, rect.top, rect.right - 12.0f, rect.bottom);
        const bool hasSelection = selectionStart(field) != selectionEnd(field);
        if (hasSelection) {
            const std::wstring before = field.text.substr(0, selectionStart(field));
            const std::wstring selected =
                field.text.substr(selectionStart(field), selectionEnd(field) - selectionStart(field));
            const float left = textRect.left + painter.measureTextWidth(before, format);
            const float right = std::min(textRect.right,
                                         left + painter.measureTextWidth(selected, format));
            if (right > left)
                painter.target()->FillRectangle(
                    D2D1::RectF(left, textRect.top + 6.0f, right, textRect.bottom - 6.0f),
                    painter.brush(p.listSelected));
        }

        if (field.text.empty())
            painter.drawText(field.cue, format, textRect, p.textSecondary);
        else
            painter.drawText(field.text, format, textRect, p.text);

        if (focusedId == id) {
            const std::wstring before = field.text.substr(0, field.caret);
            const float caretX = std::clamp(textRect.left + painter.measureTextWidth(before, format),
                                            textRect.left, textRect.right - 1.0f);
            if (auto* brush = painter.brush(p.accent))
                painter.target()->FillRectangle(
                    D2D1::RectF(caretX, textRect.top + 7.0f, caretX + 1.0f,
                                textRect.bottom - 7.0f),
                    brush);
        }
    }

    bool isEnabled(int id) const {
        if (id == kIdSearchButton)
            return !searching;
        if (id == kIdPreviewRomanization)
            return previewHasRomanization;
        if (id == kIdPreviewTranslation)
            return previewHasTranslation;
        if (id == kIdApplyButton || id == kIdAdvanceButton || id == kIdDelayButton)
            return !previewLines.empty();
        return id != 0;
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                    const wchar_t* text, bool accent, int id) {
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
            painter.strokeRoundRect(
                accent ? p.textOnAccent : p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, fluent::metrics::controlRadius - 1.0f);
        }
        painter.drawText(text, painter.textFormat(14.0f, 400, true, true),
                         D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f, rect.bottom),
                         textColor);
    }

    void drawCandidateList(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const float height = candidateListRect.bottom - candidateListRect.top;
        candidateScroll = std::clamp(candidateScroll, 0.0f, candidateMaxScroll());

        painter.fillRoundRect(p.cardFill, candidateListRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, candidateListRect, 1.0f,
                                fluent::metrics::cardRadius);
        if (focusedId == kIdCandidateList && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(candidateListRect.left + 1.5f, candidateListRect.top + 1.5f,
                            candidateListRect.right - 1.5f, candidateListRect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }

        painter.target()->PushAxisAlignedClip(candidateListRect,
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float y = candidateListRect.top - candidateScroll;
        for (int row = 0; row < static_cast<int>(candidateItems.size()); ++row) {
            const float rowH = candidateItemHeight(row);
            const D2D1_RECT_F rowRect = D2D1::RectF(candidateListRect.left, y,
                                                   candidateListRect.right, y + rowH);
            if (rowRect.bottom >= candidateListRect.top && rowRect.top <= candidateListRect.bottom) {
                const auto& item = candidateItems[row];
                if (!item.header) {
                    const bool selected = row == selectedItemRow;
                    const bool hovered = row == hoverItemRow;
                    if (selected)
                        painter.fillRoundRect(
                            p.listSelected,
                            D2D1::RectF(candidateListRect.left + 4.0f, y + 2.0f,
                                        candidateListRect.right - 4.0f, y + rowH - 2.0f));
                    else if (hovered)
                        painter.fillRoundRect(
                            p.listHover,
                            D2D1::RectF(candidateListRect.left + 4.0f, y + 2.0f,
                                        candidateListRect.right - 4.0f, y + rowH - 2.0f));
                    if (selected)
                        painter.fillRoundRect(
                            p.accent,
                            D2D1::RectF(candidateListRect.left + 7.0f,
                                        y + rowH * 0.5f - 8.0f,
                                        candidateListRect.left + 10.0f,
                                        y + rowH * 0.5f + 8.0f),
                            1.5f);
                    painter.drawTrimmedText(
                        item.text, painter.textFormat(13.0f, 400, false, true),
                        D2D1::RectF(candidateListRect.left + 16.0f, y,
                                    candidateListRect.right - 12.0f, y + rowH),
                        p.text);
                } else {
                    painter.drawText(
                        item.text, painter.textFormat(12.0f, 600, false, true),
                        D2D1::RectF(candidateListRect.left + 16.0f, y,
                                    candidateListRect.right - 12.0f, y + rowH),
                        p.textSecondary);
                }
            }
            y += rowH;
        }

        if (candidateHasScroll() && height > 0.0f) {
            const auto bar = scrollBarGeometry(height, candidateContentHeight());
            const float thumbY = candidateListRect.top +
                                 scrollBarThumbY(bar, candidateScroll, candidateMaxScroll());
            painter.fillRoundRect(
                p.textSecondary,
                D2D1::RectF(candidateListRect.right - kScrollBarInset - kScrollBarWidth, thumbY,
                            candidateListRect.right - kScrollBarInset, thumbY + bar.thumbHeight),
                kScrollBarWidth * 0.5f);
        }
        painter.target()->PopAxisAlignedClip();
    }

    void drawPreview(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const float width = previewRect.right - previewRect.left;
        const float height = previewRect.bottom - previewRect.top;
        painter.fillRoundRect(p.cardFill, previewRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, previewRect, 1.0f, fluent::metrics::cardRadius);
        if (focusedId == kIdPreview && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(previewRect.left + 1.5f, previewRect.top + 1.5f,
                            previewRect.right - 1.5f, previewRect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }

        if (previewLines.empty()) {
            painter.drawText(
                L"选择左侧歌曲预览歌词", painter.textFormat(15.0f, 400, true),
                D2D1::RectF(previewRect.left + 12.0f, previewRect.top + height * 0.4f,
                            previewRect.right - 12.0f, previewRect.top + height * 0.6f),
                p.textSecondary);
            return;
        }

        const float contentTop = previewContentTop();
        if (!previewCapabilityLabel.empty())
            painter.drawText(previewCapabilityLabel, painter.textFormat(11.0f, 400, true),
                             D2D1::RectF(previewRect.left + 12.0f, previewRect.top + 7.0f,
                                         previewRect.right - 12.0f, previewRect.top + 27.0f),
                             p.textSecondary);

        const bool hasToggleRail = previewHasRomanization || previewHasTranslation;
        const float lyricLeftLimit = previewRect.left + 12.0f;
        const float lyricRightLimit = previewRect.right - 12.0f -
                                      (hasToggleRail ? kPreviewToggleSize + kPreviewToggleTextGap
                                                     : 0.0f);
        const float lyricAreaWidth = std::max(0.0f, lyricRightLimit - lyricLeftLimit);
        const float lyricWidth = std::min(kLyricTextWidthDip, lyricAreaWidth);
        const float lyricLeft = lyricLeftLimit + (lyricAreaWidth - lyricWidth) * 0.5f;
        const float lyricRight = lyricLeft + lyricWidth;

        const int total = static_cast<int>(previewLines.size());
        const int visibleEstimate = visiblePreviewRows(height);
        const int maxTop = std::max(0, total - 1);
        if (!previewManualScroll) {
            if (previewCurrentLine >= 0)
                previewTopLine =
                    std::clamp(previewCurrentLine - visibleEstimate / 2, 0, maxTop);
            else
                previewTopLine = std::clamp(previewTopLine, 0, maxTop);
        }
        const float availableHeight = std::max(0.0f, height - contentTop);

        std::vector<float> mainHeights(total, kLyricRowHeightDip);
        std::vector<float> rowHeights(total, previewLyricRowHeight());
        std::vector<bool> measured(total, false);
        auto ensureRowMetric = [&](int index) {
            if (!measured[index]) {
                mainHeights[index] =
                    measurePreviewMainHeight(painter, previewLines[index].text, lyricWidth);
                rowHeights[index] = mainHeights[index] + previewSecondaryHeight();
                measured[index] = true;
            }
            return rowHeights[index];
        };
        auto countRowsFrom = [&](int start, float& blockHeight) {
            blockHeight = 0.0f;
            int count = 0;
            for (int index = start; index < total; ++index) {
                const float rowHeight = ensureRowMetric(index);
                if (count > 0 && blockHeight + rowHeight > availableHeight)
                    break;
                blockHeight += rowHeight;
                ++count;
            }
            return std::max(1, count);
        };

        int top = std::clamp(previewTopLine, 0, maxTop);
        float lyricBlockHeight = 0.0f;
        int count = countRowsFrom(top, lyricBlockHeight);
        if (!previewManualScroll && previewCurrentLine >= top + count) {
            top = std::clamp(previewCurrentLine, 0, maxTop);
            count = countRowsFrom(top, lyricBlockHeight);
        }
        previewTopLine = top;
        const float lyricTop = previewRect.top + contentTop +
                               std::max(0.0f, (availableHeight - lyricBlockHeight) * 0.5f);

        painter.target()->PushAxisAlignedClip(
            D2D1::RectF(previewRect.left, previewRect.top, previewRect.right, previewRect.bottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float rowTop = lyricTop;
        for (int i = 0; i < count; ++i) {
            const int lineIndex = top + i;
            const bool current = previewCurrentLine >= 0 && lineIndex == previewCurrentLine;
            const float rowHeight = ensureRowMetric(lineIndex);
            const D2D1_RECT_F rect =
                D2D1::RectF(lyricLeft, rowTop, lyricRight, rowTop + rowHeight);
            drawPreviewLyricLine(painter, previewLines[lineIndex], rect,
                                 mainHeights[lineIndex], current);
            rowTop += rowHeight;
        }
        painter.target()->PopAxisAlignedClip();

        if (previewHasRomanization)
            drawPreviewToggleButton(painter, previewRomanizationRect, L"音",
                                    previewShowRomanization, kIdPreviewRomanization);
        if (previewHasTranslation)
            drawPreviewToggleButton(painter, previewTranslationRect, L"译", previewShowTranslation,
                                    kIdPreviewTranslation);
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        const auto& p = fluent::palette();
        painter.drawText(L"手动搜索歌词", painter.textFormat(20.0f, 600), titleRect, p.text);
        painter.drawText(L"从多个来源选择并预览歌词", painter.textFormat(13.0f, 400), subtitleRect,
                         p.textSecondary);
        drawTextField(painter, titleEditRect, titleEdit, kIdTitleEdit);
        drawTextField(painter, artistEditRect, artistEdit, kIdArtistEdit);
        drawButton(painter, searchRect, L"搜索", true, kIdSearchButton);

        painter.drawTrimmedText(statusText, painter.textFormat(13.0f, 400, false, true), statusRect,
                                 p.text);
        painter.drawTrimmedText(
            L"YRC/LRC 为网易云歌词；QRC 为 QQ 原生逐字歌词；KRC 为酷狗逐字歌词",
            painter.textFormat(12.0f, 400, false, true), hintRect, p.textSecondary);
        painter.drawTrimmedText(
            L"提示：网易云歌词保存后不能给 QQ 音乐使用；QQ 音乐/酷狗音乐歌词保存后不能给网易云音乐使用。",
            painter.textFormat(13.0f, 600, false, true), compatibilityRect, p.text);

        drawCandidateList(painter);
        drawPreview(painter);
        drawButton(painter, advanceRect, L"提前 0.5 秒", false, kIdAdvanceButton);
        drawButton(painter, delayRect, L"延后 0.5 秒", false, kIdDelayButton);
        drawButton(painter, applyRect, L"使用此歌词", true, kIdApplyButton);
        drawButton(painter, cancelRect, L"取消", false, kIdCancelButton);
    }

    int hitTest(float x, float y) const {
        if (contains(titleEditRect, x, y))
            return kIdTitleEdit;
        if (contains(artistEditRect, x, y))
            return kIdArtistEdit;
        if (contains(searchRect, x, y))
            return kIdSearchButton;
        if (contains(candidateListRect, x, y))
            return kIdCandidateList;
        if (previewHasRomanization && contains(previewRomanizationRect, x, y))
            return kIdPreviewRomanization;
        if (previewHasTranslation && contains(previewTranslationRect, x, y))
            return kIdPreviewTranslation;
        if (contains(previewRect, x, y))
            return kIdPreview;
        if (contains(advanceRect, x, y))
            return kIdAdvanceButton;
        if (contains(delayRect, x, y))
            return kIdDelayButton;
        if (contains(applyRect, x, y))
            return kIdApplyButton;
        if (contains(cancelRect, x, y))
            return kIdCancelButton;
        return 0;
    }

    std::vector<int> focusOrder() const {
        std::vector<int> order{kIdTitleEdit, kIdArtistEdit, kIdSearchButton,
                               kIdCandidateList, kIdPreviewRomanization,
                               kIdPreviewTranslation, kIdAdvanceButton, kIdDelayButton,
                               kIdApplyButton, kIdCancelButton};
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
        index = (index + direction + static_cast<int>(order.size())) %
                static_cast<int>(order.size());
        focusedId = order[index];
        focusVisible = true;
        surface.invalidate();
    }

    void selectCandidateRow(int row) {
        if (row < 0 || row >= static_cast<int>(candidateItems.size()) ||
            candidateItems[row].header || candidateItems[row].candidateIndex < 0 ||
            candidateItems[row].candidateIndex >= static_cast<int>(candidates.size()))
            return;
        selectedItemRow = row;
        selectedIdx = candidateItems[row].candidateIndex;
        const auto& candidate = candidates[selectedIdx];
        runtime_log::writef(L"[action][manual-search] candidate-selected index=%d name=\"%ls\" "
                            L"artist=\"%ls\" source=%d",
                            selectedIdx, candidate.name.c_str(), candidate.singer.c_str(),
                            static_cast<int>(candidate.source));
        ensureCandidateVisible(row);
        onSelectionChanged();
    }

    void onCommand(int id) {
        switch (id) {
        case kIdSearchButton:
            doSearch();
            break;
        case kIdAdvanceButton:
            runtime_log::writef(L"[action][manual-search] preview-offset delta=-500ms");
            shiftPreviewTimes(-500);
            break;
        case kIdDelayButton:
            runtime_log::writef(L"[action][manual-search] preview-offset delta=+500ms");
            shiftPreviewTimes(500);
            break;
        case kIdPreviewRomanization:
            previewShowRomanization = !previewShowRomanization;
            runtime_log::writef(L"[action][manual-search] preview-romanization=%s",
                                previewShowRomanization ? L"on" : L"off");
            syncPreviewToCurrentLine();
            surface.invalidate();
            break;
        case kIdPreviewTranslation:
            previewShowTranslation = !previewShowTranslation;
            runtime_log::writef(L"[action][manual-search] preview-translation=%s",
                                previewShowTranslation ? L"on" : L"off");
            syncPreviewToCurrentLine();
            surface.invalidate();
            break;
        case kIdApplyButton:
            runtime_log::writef(L"[action][manual-search] apply-click");
            applySelection();
            break;
        case kIdCancelButton:
            runtime_log::writef(L"[action][manual-search] cancel-click");
            destroy();
            break;
        default:
            break;
        }
    }

    void doSearch() {
        if (!provider)
            return;
        searching = true;
        candidateItems.clear();
        candidates.clear();
        selectedItemRow = -1;
        selectedIdx = -1;
        hoverItemRow = -1;
        candidateScroll = 0.0f;
        candidateWheelAccum = 0;
        previewOffsetMs = 0;
        setPreviewLines({});
        statusText = L"搜索中，请稍候…";
        surface.invalidate();

        const std::wstring title = titleEdit.text;
        const std::wstring artist = artistEdit.text;
        HWND hwndCopy = hwnd;
        provider->searchCandidatesAsync(title, artist,
            [hwndCopy](const std::vector<SearchCandidate>& result) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::vector<SearchCandidate>(result);
                if (!PostMessageW(hwndCopy, kMsgCandidatesReady, 0,
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            });
    }

    void handleCandidatesReady(const std::vector<SearchCandidate>& result) {
        candidates = result;
        candidateItems.clear();
        selectedItemRow = -1;
        selectedIdx = -1;
        hoverItemRow = -1;
        candidateScroll = 0.0f;
        candidateWheelAccum = 0;
        searching = false;
        previewOffsetMs = 0;
        setPreviewLines({});

        int yrcCount = 0;
        int qrcCount = 0;
        int krcCount = 0;
        int lrcCount = 0;
        for (const auto& candidate : candidates) {
            if (candidate.source == LyricSource::Yrc)
                ++yrcCount;
            else if (candidate.source == LyricSource::Qrc)
                ++qrcCount;
            else if (candidate.source == LyricSource::Krc)
                ++krcCount;
            else
                ++lrcCount;
        }

        auto addHeader = [this](const wchar_t* text) {
            candidateItems.push_back({text, -1, true});
        };
        auto addCandidate = [this](int index) {
            const auto& candidate = candidates[index];
            candidateItems.push_back(
                {candidate.name + L" - " + candidate.singer, index, false});
        };
        if (yrcCount > 0) {
            addHeader(L"网易云歌词（YRC/LRC）");
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
                if (candidates[i].source == LyricSource::Yrc)
                    addCandidate(i);
        }
        if (krcCount > 0) {
            addHeader(L"酷狗逐字歌词（KRC）");
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
                if (candidates[i].source == LyricSource::Krc)
                    addCandidate(i);
        }
        if (qrcCount > 0) {
            addHeader(L"QQ 音乐逐字歌词（QRC）");
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
                if (candidates[i].source == LyricSource::Qrc)
                    addCandidate(i);
        }
        if (lrcCount > 0) {
            addHeader(L"QQ 音乐逐行歌词（LRC）");
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
                if (candidates[i].source == LyricSource::Lrc)
                    addCandidate(i);
        }

        if (candidates.empty()) {
            statusText = L"未找到相关歌曲，请尝试更换关键词。";
        } else {
            statusText = L"找到 " + std::to_wstring(yrcCount) + L" 条网易云（YRC/LRC）、 " +
                         std::to_wstring(krcCount) + L" 条酷狗逐字（KRC）、 " +
                         std::to_wstring(qrcCount) + L" 条 QQ 逐字（QRC）、 " +
                         std::to_wstring(lrcCount) + L" 条 QQ 逐行（LRC）候选，点击选择以预览歌词。";
            for (int row = 0; row < static_cast<int>(candidateItems.size()); ++row) {
                if (!candidateItems[row].header) {
                    selectCandidateRow(row);
                    break;
                }
            }
        }
        surface.invalidate();
    }

    void onSelectionChanged() {
        if (selectedIdx < 0 || selectedIdx >= static_cast<int>(candidates.size()) || !provider)
            return;
        setPreviewLines({});
        previewOffsetMs = 0;
        statusText = L"正在加载《" + candidates[selectedIdx].name + L"》的歌词预览…";
        surface.invalidate();

        HWND hwndCopy = hwnd;
        const int index = selectedIdx;
        provider->fetchLyricAsync(candidates[index],
            [hwndCopy, index](bool ok, const std::vector<LyricLine>& lines,
                              const SongInfo& info) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(
                    index, ok, lines, info);
                if (!PostMessageW(hwndCopy, kMsgPreviewLyricReady, 0,
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            });
    }

    void handlePreviewLyricReady(int index, bool ok, const std::vector<LyricLine>& lines,
                                 const SongInfo& info) {
        if (index != selectedIdx || !ok) {
            if (index == selectedIdx)
                statusText = L"该候选没有可用歌词，请尝试其他歌曲。";
            surface.invalidate();
            return;
        }
        previewInfo = info;
        previewOffsetMs = 0;
        setPreviewLines(lines);
        bool wordByWord = false;
        for (const auto& line : lines) {
            if (!line.chars.empty()) {
                wordByWord = true;
                break;
            }
        }
        statusText = L"已加载《" + candidates[index].name +
                     (wordByWord ? L"》的逐字歌词预览，点击“使用此歌词”应用。"
                                 : L"》的整行歌词预览，点击“使用此歌词”应用。");
        surface.invalidate();
    }

    void applySelection() {
        if (selectedIdx < 0 || selectedIdx >= static_cast<int>(candidates.size()) ||
            previewLines.empty())
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
        destroy();
    }

    void shiftPreviewTimes(int64_t deltaMs) {
        if (previewLines.empty())
            return;
        previewOffsetMs += deltaMs;
        for (auto& line : previewLines) {
            line.ms += deltaMs;
            for (auto& character : line.chars) {
                character.startMs += deltaMs;
                character.endMs += deltaMs;
            }
        }
        resetPreviewView();
        std::wstring offsetText = previewOffsetMs >= 0 ? L"+" : L"";
        offsetText += std::to_wstring(previewOffsetMs / 1000);
        statusText = L"预览时间已调整 " + offsetText + L" 秒，确认后点击“使用此歌词”。";
        surface.invalidate();
    }

    void destroy() {
        if (hwnd) {
            HWND target = hwnd;
            DestroyWindow(target);
            discardPendingResults(target);
            hwnd = nullptr;
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            surface.initialize(hwnd, backdrop);
            createControls();
            layout();
            SetTimer(hwnd, kPreviewTimerId, 100, nullptr);
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
        case WM_GETMINMAXINFO:
            fluent::setDialogMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp),
                                               kDialogStyle, kDialogExStyle,
                                               kMinClientWidthDip, kMinClientHeightDip);
            return 0;
        case WM_SIZING:
            fluent::enforceDialogMinimumAspectRatio(hwnd, wp, reinterpret_cast<RECT*>(lp),
                                                     kMinClientAspectRatio);
            return TRUE;
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
        case WM_TIMER:
            if (wp == kPreviewTimerId)
                onPreviewTimer();
            return 0;
        case kMsgCandidatesReady: {
            auto* payload = reinterpret_cast<std::vector<SearchCandidate>*>(lp);
            if (payload) {
                handleCandidatesReady(*payload);
                delete payload;
            }
            return 0;
        }
        case kMsgPreviewLyricReady: {
            auto* payload =
                reinterpret_cast<std::tuple<int, bool, std::vector<LyricLine>, SongInfo>*>(lp);
            if (payload) {
                handlePreviewLyricReady(std::get<0>(*payload), std::get<1>(*payload),
                                        std::get<2>(*payload), std::get<3>(*payload));
                delete payload;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (candidateScrollDragging) {
                const float scale = surface.dipScale();
                updateCandidateScrollDrag(GET_Y_LPARAM(lp) / scale);
                return 0;
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            const float scale = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / scale;
            const float y = GET_Y_LPARAM(lp) / scale;
            const int id = hitTest(x, y);
            const int row = id == kIdCandidateList ? candidateRowAt(y) : -1;
            if (id != hoverId || row != hoverItemRow) {
                hoverId = id;
                hoverItemRow = row;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!candidateScrollDragging) {
                hoverId = 0;
                hoverItemRow = -1;
                surface.invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float scale = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / scale;
            const float y = GET_Y_LPARAM(lp) / scale;
            const int id = hitTest(x, y);
            focusedId = id;
            pressedId = 0;

            if (id == kIdTitleEdit || id == kIdArtistEdit) {
                if (auto* field = editFor(id))
                    field->caret = field->anchor = field->text.size();
            } else if (id == kIdCandidateList) {
                if (beginCandidateScrollDrag(x, y)) {
                    focusedId = kIdCandidateList;
                } else {
                    const int row = candidateRowAt(y);
                    if (row >= 0 && row < static_cast<int>(candidateItems.size()) &&
                        !candidateItems[row].header)
                        selectCandidateRow(row);
                }
            } else if (isEnabled(id) && id != kIdPreview) {
                pressedId = id;
                SetCapture(hwnd);
            }
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (candidateScrollDragging) {
                candidateScrollDragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                surface.invalidate();
                return 0;
            }
            const float scale = surface.dipScale();
            const int hit = hitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
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
            candidateScrollDragging = false;
            pressedId = 0;
            surface.invalidate();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float scale = surface.dipScale();
            const float x = point.x / scale;
            const float y = point.y / scale;
            if (contains(candidateListRect, x, y))
                scrollCandidatesBy(GET_WHEEL_DELTA_WPARAM(wp));
            else if (contains(previewRect, x, y))
                scrollPreviewByWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                focusStep(isShiftDown() ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (handleEditKey(wp))
                return 0;
            if (focusedId == kIdCandidateList &&
                (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT ||
                 wp == VK_HOME || wp == VK_END)) {
                int target = -1;
                if (wp == VK_UP || wp == VK_DOWN) {
                    const int direction = wp == VK_DOWN ? 1 : -1;
                    target = nextCandidateRow(
                        selectedItemRow < 0
                            ? (direction > 0 ? -1 : static_cast<int>(candidateItems.size()))
                            : selectedItemRow,
                        direction);
                } else if (wp == VK_HOME || wp == VK_END) {
                    target = wp == VK_HOME
                                 ? nextCandidateRow(-1, 1)
                                 : nextCandidateRow(static_cast<int>(candidateItems.size()), -1);
                } else {
                    const int direction = wp == VK_NEXT ? 1 : -1;
                    int cursor = selectedItemRow < 0
                                     ? (direction > 0 ? -1
                                                      : static_cast<int>(candidateItems.size()))
                                     : selectedItemRow;
                    const int page = std::max(
                        1, static_cast<int>((candidateListRect.bottom - candidateListRect.top) /
                                            kCandidateRowHeight) -
                               1);
                    for (int i = 0; i < page; ++i) {
                        const int next = nextCandidateRow(cursor, direction);
                        if (next < 0)
                            break;
                        cursor = next;
                    }
                    target = cursor == selectedItemRow ? -1 : cursor;
                }
                if (target >= 0)
                    selectCandidateRow(target);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                if (focusedId == kIdTitleEdit || focusedId == kIdArtistEdit) {
                    if (wp == VK_RETURN && isEnabled(kIdSearchButton))
                        doSearch();
                } else if (isEnabled(focusedId) && focusedId != kIdCandidateList &&
                           focusedId != kIdPreview) {
                    onCommand(focusedId);
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wp)))
                return 0;
            break;
        case WM_IME_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wp)))
                return 0;
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            focusVisible = false;
            surface.invalidate();
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            discardPendingResults(hwnd);
            KillTimer(hwnd, kPreviewTimerId);
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::ManualSearch), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
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
    impl_->notifyHwnd = parent; // 仅用于关闭通知
    impl_->provider = provider;
    impl_->targetTitle = targetTitle;
    impl_->targetArtist = targetArtist;
    impl_->targetDurationMs = targetDurationMs;
    impl_->targetNeteaseSongId = targetNeteaseSongId;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricManualSearch";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const float scale = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(920.0f * scale)),
            static_cast<LONG>(std::lround(640.0f * scale))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricManualSearch", L"手动搜索歌词",
                                  kDialogStyle, x, y, width, height, nullptr, nullptr, inst,
                                  impl_.get());
    if (impl_->hwnd)
        app_icon::applyWindowIcon(impl_->hwnd);
    return impl_->hwnd != nullptr;
}

void ManualSearchDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }
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
    if (!impl_->hwnd)
        return;
    auto* payload = new std::vector<SearchCandidate>(cands);
    if (!PostMessageW(impl_->hwnd, kMsgCandidatesReady, 0,
                      reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void ManualSearchDialog::onPreviewLyricReady(int idx, bool ok,
                                             const std::vector<LyricLine>& lines,
                                             const SongInfo& info) {
    if (!impl_->hwnd)
        return;
    auto* payload =
        new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(idx, ok, lines, info);
    if (!PostMessageW(impl_->hwnd, kMsgPreviewLyricReady, 0,
                      reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void ManualSearchDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}

void ManualSearchDialog::setPlaybackPosition(int64_t positionMs) {
    impl_->playbackPositionMs = positionMs;
    impl_->setPreviewPosition(positionMs);
}
