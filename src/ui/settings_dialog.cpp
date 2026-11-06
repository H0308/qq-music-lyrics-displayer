#include "settings_dialog.h"

#include "app_info.h"
#include "logging/runtime_logger.h"
#include "logging/ui_timing.h"
#include "ui/app_icon.h"
#include "ui/color_picker_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "ui/media_control_icons.h"
#include "ui/settings/artwork.h"
#include "ui/settings/ids.h"
#include "ui/settings/row.h"
#include "ui/settings/sections.h"
#include "ui/settings/section_labels.h"
#include "ui/settings/section_metrics.h"
#include "ui/settings/search_palette.h"
#include "ui/settings/slider_editor.h"
#include "ui/settings_icons.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <climits>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace settings_ui;

namespace {

constexpr int kFooterItemCount = 3;
constexpr float kFooterItemH = 32.0f;
constexpr UINT_PTR kNavIndicatorTimer = 1;
constexpr UINT kNavIndicatorDurationMs = 180;
constexpr UINT kNavIndicatorFrameMs = USER_TIMER_MINIMUM;
constexpr UINT_PTR kContentScrollTimer = 2;
constexpr UINT_PTR kSearchLocateTimer = 3;
constexpr ULONGLONG kSearchLocateFadeMs = 800;
constexpr float kContentScrollFrequency = 28.0f;
// 应用列表卡片内嵌开关的键盘焦点 ID，不对应独立设置行。

constexpr float kWindowW = 988.0f;
constexpr float kWindowH = 718.0f;
constexpr float kMinClientWidthDip = 780.0f;
constexpr float kMinClientHeightDip = 718.0f;
constexpr float kMinClientAspectRatio = kMinClientWidthDip / kMinClientHeightDip;
constexpr float kNavW = 176.0f;
constexpr float kContentMaxWidthDip = 1000.0f;
constexpr float kNavIconSize = 16.0f;
constexpr float kNavIconLeft = 18.0f;
constexpr float kNavItemH = 40.0f;
constexpr float kNavIconTextGap = 10.0f;
constexpr float kTitleTopPadding = 12.0f;
constexpr float kTitleHintGap = 2.0f;
constexpr float kHintBottomPadding = 8.0f;
constexpr float kHintTextSize = 12.0f;
constexpr float kHintMeasureHeight = 10000.0f;
constexpr float kRowGap = 8.0f;
constexpr float kModeGridGap = 8.0f;
constexpr float kModeGridCardH = 56.0f;
constexpr float kModeGridTopGap = 10.0f;
constexpr float kModeGridNoteGap = 12.0f;
constexpr float kSpectrumStyleCardH = 96.0f;
constexpr float kSpectrumStyleCardGap = 8.0f;
constexpr float kSpectrumBackgroundArtworkMaxW = 144.0f;
constexpr float kSpectrumBackgroundArtworkMinW = 96.0f;
constexpr float kSpectrumBackgroundArtworkH = 34.0f;
constexpr float kSpectrumEffectArtworkH = 44.0f;
constexpr float kSpectrumBackgroundArtworkGap = 12.0f;
constexpr float kCoverEffectCardH = 72.0f;
constexpr float kCoverEffectCardGap = 8.0f;
constexpr float kSongToastPositionCardW = 156.0f;
constexpr float kSongToastPositionCardH = 92.0f;
constexpr float kSongToastPositionCardGap = 8.0f;
constexpr float kHoverControlStyleCardH = 156.0f;
constexpr float kSliderValueW = 62.0f;
constexpr float kSliderValueGap = 10.0f;
constexpr float kSliderSuffixW = 16.0f;
constexpr float kScrollBarWidth = 3.0f;
constexpr float kScrollBarHitWidth = 12.0f;
constexpr float kScrollBarInset = 8.0f;
constexpr float kScrollMinThumbHeight = 24.0f;
constexpr float kScrollWheelDip = 72.0f;
constexpr float kRowIconSize = 16.0f;
constexpr float kRowIconTextGap = 10.0f;
constexpr float kRowIconSlotW = kRowIconSize + kRowIconTextGap;
constexpr float kPageTitleIconSize = 20.0f;
constexpr float kPageTitleIconTextGap = 10.0f;
constexpr float kIdleAppsListTop = 76.0f;
constexpr float kIdleAppActionW = 58.0f;
constexpr float kIdleAppActionGap = 8.0f;
constexpr float kIdleAppInfoRightInset = 144.0f;
constexpr float kIdleAppNamesToggleW = 40.0f;
constexpr float kIdleAppNamesToggleLabelW = 56.0f;
constexpr float kIdleAppNamesToggleGap = 8.0f;
constexpr float kIdleAppDragThreshold = 5.0f;
constexpr float kIdleQuoteBackgroundCardH = 112.0f;
constexpr float kIdleQuoteBackgroundCardGap = 8.0f;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

settings_icon::Kind iconForPage(int page) {
    switch (page) {
    case 0:
        return settings_icon::Kind::Display;
    case 1:
        return settings_icon::Kind::Background;
    case 2:
        return settings_icon::Kind::Performance;
    case 3:
        return settings_icon::Kind::Card;
    case 4:
        return settings_icon::Kind::Media;
    case 5:
        return settings_icon::Kind::Spectrum;
    case 6:
        return settings_icon::Kind::Lyrics;
    case 7:
        return settings_icon::Kind::Toast;
    case 8:
        return settings_icon::Kind::Idle;
    default:
        return settings_icon::Kind::None;
    }
}


} // namespace

struct SettingsDialog::Impl {
    SettingsArtwork artwork_;
    using ControlKind = settings_ui::ControlKind;
    using Row = settings_ui::Row;

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr;
    HWND contentWindow = nullptr;
    DCompRenderer contentRenderer;
    fluent::FluentDialogSurface::TextCache contentTextCache;
    bool contentLayerReady = false;
    bool hintHeightsDirty = true;
    bool backdrop = false;
    SettingsSections sections_;
    Row* findRow(int id) { return sections_.findRow(id); }
    bool minimalModeActive() const { return sections_.minimalModeActive(); }
    void updateFloatingCardRowsEnabled() { sections_.updateFloatingCardRowsEnabled(); }
    bool backgroundWaveActive() const { return sections_.backgroundWaveActive(); }
    bool spectrumBackgroundAvailable() const { return sections_.spectrumBackgroundAvailable(); }
    void updateProgressBackgroundRowsEnabled() { sections_.updateProgressBackgroundRowsEnabled(); }
    void updateBackgroundAdjustmentPresentation() {
        sections_.updateBackgroundAdjustmentPresentation();
    }
    void updateImmersiveRowsEnabled() { sections_.updateImmersiveRowsEnabled(); }
    void updateIdleAppsRowHeight() { sections_.updateIdleAppsRowHeight(); }
    void updateTickTickRows() { sections_.updateTickTickRows(); }
    void updateIdleQuoteSourceRow() { sections_.updateIdleQuoteSourceRow(); }
    int idleTextMode() const { return sections_.idleTextMode(); }
    bool embeddedResourceOnlyAvailable() const { return sections_.embeddedResourceOnlyAvailable(); }
    void updateIdleRowsEnabled() { sections_.updateIdleRowsEnabled(); }
    void updateCompactLyricRows() { sections_.updateCompactLyricRows(); }
    const Row* findRow(int id) const { return sections_.findRow(id); }
    void createControls() { sections_.create(); }
    void onCommand(int id, int option = -1) {
        hintHeightsDirty = true;
        const auto* row = sections_.findRow(id);
        if ((id == kIdSpectrumColor || id == kIdFloatingCardBackgroundColor) && row &&
            row->enabled) {
            openColorPicker(id);
            return;
        }
        sections_.onCommand(id, option);
        if (hwnd)
            surface.invalidate();
    }
    void updateState(const SettingsState& s) {
        if (slider_.state().open)
            commitSliderEditor();
        resetIdleAppDrag();
        sections_.update(s);
        layout();
        surface.invalidate();
    }

    int activePage = 0;

    fluent::FluentDialogSurface surface;

    int searchLocatedId = 0;
    ULONGLONG searchLocatedStartMs = 0;
    bool searchLocateFading = false;
    POINT searchLocateInitialCursorPt{LONG_MIN, LONG_MIN};
    std::unique_ptr<ColorPickerDialog> colorPicker;
    std::array<std::wstring, kSettingsPageCount> navItems{
        L"显示", L"展示模式", L"性能", L"悬浮卡片", L"媒体控件", L"频谱", L"歌词",
        L"切歌弹窗", L"每日一言与更多"};
    std::array<std::wstring, kSettingsPageCount> pageTitles{
        L"显示", L"展示模式", L"性能", L"悬浮卡片", L"媒体控件", L"频谱", L"歌词",
        L"切歌弹窗", L"每日一言与更多"};
    D2D1_RECT_F navRect{};
    std::array<D2D1_RECT_F, kSettingsPageCount> navItemRects{};
    std::array<D2D1_RECT_F, kFooterItemCount> footerItemRects{};
    D2D1_RECT_F versionRect{};
    bool navIndicatorAnimating = false;
    ULONGLONG navIndicatorStartMs = 0;
    float navIndicatorFrom = 0.0f;
    float navIndicatorPosition = 0.0f;
    float navIndicatorStretch = 0.0f;
    float navIndicatorFromStretch = 0.0f;
    std::array<D2D1_RECT_F, kSettingsPageCount> pageTitleIconRects{};
    std::array<D2D1_RECT_F, kSettingsPageCount> pageTitleRects{};
    D2D1_RECT_F contentViewportRect{};
    D2D1_RECT_F scrollTrackRect{};
    D2D1_RECT_F scrollThumbRect{};
    float contentHeight = 0.0f;
    float contentScroll = 0.0f;
    float contentPaintScroll = 0.0f;
    bool contentScrollAnimating = false;
    std::chrono::steady_clock::time_point contentScrollLastTick{};
    float contentScrollVelocity = 0.0f;
    float contentScrollTarget = 0.0f;
    float contentMaxScroll = 0.0f;
    bool scrollDragging = false;
    float scrollDragOffset = 0.0f;
    bool idleAppDragging = false;
    int idleAppDragIndex = -1;
    int idleAppDropIndex = -1;
    float idleAppDragStartX = 0.0f;
    float idleAppDragStartY = 0.0f;

    int hoverId = 0;
    int hoverOption = -1;
    int pressedId = 0;
    int pressedOption = -1;
    int focusedId = kIdNav;
    bool focusVisible = false;

    SliderEditor slider_{{hwnd, focusedId, contentViewportRect,
                          [this](int id) { return findRow(id); }, [this](int id) { onCommand(id); },
                          [this] { stopContentScroll(); }, [this] { surface.invalidate(); }}};

    void resetSliderCaret() { slider_.resetCaret(); }
    void hideSliderEditor() { slider_.hide(); }
    void cancelSliderEditor() { slider_.cancel(); }
    void commitSliderEditor() { slider_.commit(); }
    void layoutSliderEditor() { slider_.layout(); }
    void openSliderEditor(Row& row) { slider_.open(row); }
    void replaceSliderSelection(const std::wstring& inserted) {
        slider_.replaceSelection(inserted);
    }
    bool handleSliderEditKey(WPARAM key) { return slider_.handleKey(key); }
    void placeSliderCaret(float pointerX, bool extend) { slider_.placeCaret(pointerX, extend); }

    SettingsSearchPalette search_{{hwnd, inst, surface, sections_, pageTitles, focusVisible},
                                  [this] {
                                      stopSearchLocateHighlight();
                                      if (slider_.state().open)
                                          commitSliderEditor();
                                  },
                                  [this](const SettingsSearchResult& result) {
                                      showPage(result.page);
                                      surface.invalidate();
                                      UpdateWindow(hwnd);
                                      if (auto* row = findRow(result.id)) {
                                          setContentScroll(contentScroll + row->cardRect.top -
                                                           contentViewportRect.top - 12.0f);
                                          startSearchLocateHighlight(result.id);
                                          focusedId = row->enabled ? result.id : kIdNav;
                                      }
                                      SetFocus(hwnd);
                                      focusVisible = false;
                                      surface.invalidate();
                                  }};
    bool searching() const { return search_.searching(); }
    void refreshSearch() { search_.refreshSearch(); }
    void openSearchPalette() { search_.openSearchPalette(); }
    void closeSearchPalette(bool focus, bool immediate = false) {
        search_.closeSearchPalette(focus, immediate);
    }
    void layoutSearchPalette() { search_.layoutSearchPalette(); }
    void advanceSearchPaletteAnimation() { search_.advanceSearchPaletteAnimation(); }

    static LRESULT CALLBACK contentWndProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCHITTEST)
            return HTTRANSPARENT;
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    bool createContentWindow() {
        constexpr wchar_t className[] = L"QQMusicLyricSettingsContent";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = contentWndProc;
        windowClass.hInstance = inst;
        windowClass.lpszClassName = className;
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        contentWindow = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP, className, L"", WS_CHILD | WS_DISABLED | WS_VISIBLE,
            0, 0, 1, 1, hwnd, nullptr, inst, nullptr);
        return contentWindow != nullptr;
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

    static bool contains(const D2D1_RECT_F& rect, float x, float y) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    static bool isIdleAppDragOption(int option) {
        return SettingsSections::isIdleAppDragOption(option);
    }

    void resetIdleAppDrag() {
        idleAppDragging = false;
        idleAppDragIndex = -1;
        idleAppDropIndex = -1;
        idleAppDragStartX = 0.0f;
        idleAppDragStartY = 0.0f;
    }

    bool updateIdleAppDropTarget(float x, float y) {
        const Row* row = findRow(kIdIdleApps);
        const size_t count = sections_.state.idleApps.size();
        if (!row || !row->enabled || count == 0) {
            idleAppDropIndex = -1;
            return false;
        }

        const float innerX = row->cardRect.left + 16.0f;
        const float innerRight = row->cardRect.right - 16.0f;
        const float listTop = row->cardRect.top + kIdleAppsListTop;
        const D2D1_RECT_F dropRect =
            D2D1::RectF(innerX, listTop - 4.0f, innerRight,
                        listTop + static_cast<float>(count) * kIdleAppItemH - 4.0f);
        if (!contains(dropRect, x, y)) {
            idleAppDropIndex = -1;
            return false;
        }

        const int index = static_cast<int>(std::floor(
            (y - listTop + kIdleAppItemH * 0.5f) / kIdleAppItemH));
        idleAppDropIndex = std::clamp(index, 0, static_cast<int>(count));
        return true;
    }



    static D2D1_RECT_F sliderValueRect(const Row& row) {
        const float trackRight = row.controlRect.right - kSliderValueW;
        return D2D1::RectF(trackRight + kSliderValueGap, row.controlRect.top,
                           row.controlRect.right, row.controlRect.bottom);
    }

    static D2D1_RECT_F sliderNumberRect(const Row& row) {
        const auto rect = sliderValueRect(row);
        return D2D1::RectF(rect.left + 4.0f, rect.top + 1.0f,
                           rect.right - 4.0f - kSliderSuffixW, rect.bottom - 1.0f);
    }











    static bool isOptionEnabled(const Row& row, size_t option) {
        return option >= row.optionEnabled.size() || row.optionEnabled[option];
    }





    // 播放进度背景与背景波浪共用同一层背景，二者互斥：
    // 背景波浪生效（频谱开 + 梦幻波浪 + 背景开关开）时进度背景不可用

    // 背景波浪行的可用态：频谱开 + 梦幻波浪样式














    void openColorPicker(int id) {
        if (colorPicker && colorPicker->isOpen()) {
            SetForegroundWindow(colorPicker->hwnd());
            return;
        }

        COLORREF initial = 0;
        const wchar_t* title = nullptr;
        if (id == kIdFloatingCardBackgroundColor) {
            initial = sections_.state.floatingCardBackgroundColor;
            title = L"悬浮卡片背景颜色";
        } else if (id == kIdSpectrumColor) {
            initial = sections_.state.spectrumColor;
            title = L"自定义频谱颜色";
        } else {
            return;
        }

        colorPicker = std::make_unique<ColorPickerDialog>();
        if (!colorPicker->create(inst, hwnd, initial, title)) {
            colorPicker.reset();
            return;
        }
        colorPicker->setApplyCallback([this, id](COLORREF color) {
            if (id == kIdFloatingCardBackgroundColor) {
                sections_.state.floatingCardBackgroundColor = color;
                if (sections_.actions.onFloatingCardBackgroundColor)
                    sections_.actions.onFloatingCardBackgroundColor(color);
            } else if (id == kIdSpectrumColor) {
                sections_.state.spectrumColor = color;
                sections_.state.spectrumCustomColor = true;
                sections_.state.spectrumFollowAlbum = false;
                if (auto* mode = findRow(kIdSpectrumColorMode))
                    mode->selected = 2;
                if (sections_.actions.onSpectrumColor)
                    sections_.actions.onSpectrumColor(color);
            }
            if (auto* row = findRow(id))
                row->controlText = colorText(color);
            updateImmersiveRowsEnabled();
            surface.invalidate();
        });
        colorPicker->show();
    }


    D2D1_RECT_F scrollBarHitRect() const {
        if (contentMaxScroll <= 0.0f || scrollTrackRect.bottom <= scrollTrackRect.top)
            return {};
        const float extra = std::max(0.0f, (kScrollBarHitWidth - kScrollBarWidth) * 0.5f);
        return D2D1::RectF(scrollTrackRect.left - extra, scrollTrackRect.top - 4.0f,
                           scrollTrackRect.right + extra, scrollTrackRect.bottom + 4.0f);
    }

    void updateScrollBarGeometry() {
        scrollTrackRect = {};
        scrollThumbRect = {};
        if (contentMaxScroll <= 0.0f)
            return;

        const float trackTop = contentViewportRect.top + kScrollBarInset;
        const float trackBottom = contentViewportRect.bottom - kScrollBarInset;
        const float trackHeight = std::max(0.0f, trackBottom - trackTop);
        const float viewportHeight =
            std::max(0.0f, contentViewportRect.bottom - contentViewportRect.top);
        if (trackHeight <= 0.0f || viewportHeight <= 0.0f)
            return;

        const float thumbHeight = std::min(
            trackHeight, std::max(kScrollMinThumbHeight,
                                  trackHeight * viewportHeight / std::max(1.0f, contentHeight)));
        const float usable = std::max(0.0f, trackHeight - thumbHeight);
        const float thumbTop =
            trackTop + contentScroll / std::max(1.0f, contentMaxScroll) * usable;
        RECT client{};
        GetClientRect(hwnd, &client);
        const float clientWidth = static_cast<float>(client.right - client.left) /
                                  surface.dipScale();
        const float trackLeft = clientWidth - kScrollBarInset - kScrollBarWidth;
        scrollTrackRect =
            D2D1::RectF(trackLeft, trackTop, trackLeft + kScrollBarWidth, trackBottom);
        scrollThumbRect = D2D1::RectF(scrollTrackRect.left, thumbTop, scrollTrackRect.right,
                                      thumbTop + thumbHeight);
    }

    void stopContentScroll() {
        if (hwnd)
            KillTimer(hwnd, kContentScrollTimer);
        contentScrollAnimating = false;
        contentScrollVelocity = 0.0f;
        contentScrollTarget = contentScroll;
        contentRenderer.setRootOffsetY(-std::round(contentScroll * surface.dipScale()));
        contentRenderer.commit();
    }

    void applyContentScroll(float offset) {
        const float next = std::clamp(offset, 0.0f, contentMaxScroll);
        contentScroll = next;
        const float scale = surface.dipScale();
        const float paintScroll = std::round(next * scale) / scale;
        const float displacement = contentPaintScroll - paintScroll;
        if (!contentScrollAnimating) {
            contentRenderer.setRootOffsetY(-std::round(contentScroll * scale));
            contentRenderer.commit();
        }
        if (displacement == 0.0f)
            return;
        contentPaintScroll = paintScroll;
        const auto shift = [displacement](D2D1_RECT_F& rect) {
            if (rect.right > rect.left && rect.bottom > rect.top) {
                rect.top += displacement;
                rect.bottom += displacement;
            }
        };
        for (auto& row : sections_.rows[activePage]) {
            for (auto* rect : {&row.cardRect, &row.iconRect, &row.labelRect, &row.valueRect,
                               &row.hintRect, &row.controlRect, &row.artworkRect,
                               &row.addAppRect})
                shift(*rect);
            for (auto* rects : {&row.appItemRects, &row.appEditRects, &row.appDeleteRects,
                                &row.optionRects}) {
                for (auto& rect : *rects)
                    shift(rect);
            }
        }
        updateScrollBarGeometry();
        layoutSliderEditor();
        RECT dirty{};
        GetClientRect(hwnd, &dirty);
        dirty.left = static_cast<LONG>(std::floor((scrollTrackRect.left - kScrollBarHitWidth) * scale));
        dirty.top = static_cast<LONG>(std::floor(contentViewportRect.top * surface.dipScale()));
        surface.invalidate(&dirty);
    }

    void setContentScroll(float offset) {
        stopContentScroll();
        applyContentScroll(offset);
    }

    void advanceContentScroll() {
        if (!contentScrollAnimating)
            return;
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - contentScrollLastTick).count();
        contentScrollLastTick = now;
        contentScrollTarget = std::clamp(contentScrollTarget, 0.0f, contentMaxScroll);
        const float displacement = contentScroll - contentScrollTarget;
        const float coefficient = contentScrollVelocity + kContentScrollFrequency * displacement;
        const float decay = std::exp(-kContentScrollFrequency * elapsed);
        const float next = contentScrollTarget + (displacement + coefficient * elapsed) * decay;
        contentScrollVelocity =
            (contentScrollVelocity - kContentScrollFrequency * coefficient * elapsed) * decay;
        applyContentScroll(next);
        if (std::fabs(next - contentScrollTarget) < 0.1f &&
            std::fabs(contentScrollVelocity) < 2.0f) {
            applyContentScroll(contentScrollTarget);
            stopContentScroll();
        }
    }

    void scrollFromPointer(float y) {
        if (contentMaxScroll <= 0.0f)
            return;
        const float trackHeight = scrollTrackRect.bottom - scrollTrackRect.top;
        const float thumbHeight = scrollThumbRect.bottom - scrollThumbRect.top;
        const float usable = trackHeight - thumbHeight;
        if (usable <= 0.0f)
            return;
        const float thumbTop = std::clamp(y - scrollDragOffset, scrollTrackRect.top,
                                          scrollTrackRect.bottom - thumbHeight);
        const float ratio = (thumbTop - scrollTrackRect.top) / usable;
        setContentScroll(ratio * contentMaxScroll);
    }

    void scrollBy(float delta, bool smooth = false) {
        if (contentMaxScroll <= 0.0f)
            return;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        if (!smooth || !animations) {
            setContentScroll(contentScroll + delta);
            return;
        }
        advanceContentScroll();
        const bool continuing = contentScrollAnimating &&
                                delta * (contentScrollTarget - contentScroll) > 0.0f;
        const float target = std::clamp(
            (continuing ? contentScrollTarget : contentScroll) + delta,
            0.0f, contentMaxScroll);
        if (std::fabs(target - contentScroll) < 0.01f) {
            stopContentScroll();
            return;
        }
        if (delta * contentScrollVelocity < 0.0f)
            contentScrollVelocity = 0.0f;
        contentScrollTarget = target;
        contentScrollLastTick = std::chrono::steady_clock::now();
        if (!contentScrollAnimating) {
            if (SetTimer(hwnd, kContentScrollTimer, USER_TIMER_MINIMUM, nullptr))
                contentScrollAnimating = true;
            else
                setContentScroll(target);
        }
        if (contentScrollAnimating) {
            if (slider_.state().open)
                commitSliderEditor();
            if (!contentLayerReady) {
                surface.invalidate();
                UpdateWindow(hwnd);
            }
            contentRenderer.animateRootScroll(-contentScroll * surface.dipScale(),
                                              -std::round(contentScrollTarget * surface.dipScale()),
                                              -contentScrollVelocity * surface.dipScale(),
                                              kContentScrollFrequency);
            contentRenderer.commit();
        }
    }

    void stopSearchLocateHighlight() {
        searchLocatedId = 0;
        searchLocatedStartMs = 0;
        searchLocateFading = false;
        searchLocateInitialCursorPt = {LONG_MIN, LONG_MIN};
        if (hwnd)
            KillTimer(hwnd, kSearchLocateTimer);
    }

    void startSearchLocateHighlight(int rowId) {
        stopSearchLocateHighlight();
        if (rowId == 0)
            return;
        searchLocatedId = rowId;
        searchLocatedStartMs = 0;
        searchLocateFading = false;
        POINT cursor{};
        if (hwnd && GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor))
            searchLocateInitialCursorPt = cursor;
    }

    bool pointerOverLocatedRow(float pointerX, float pointerY) const {
        if (searchLocatedId == 0 || !contains(contentViewportRect, pointerX, pointerY))
            return false;
        const Row* located = findRow(searchLocatedId);
        return located && contains(located->cardRect, pointerX, pointerY);
    }

    void beginSearchLocateFade() {
        if (searchLocatedId == 0 || searchLocateFading)
            return;
        BOOL clientAnimations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimations, 0);
        if (!clientAnimations || !hwnd) {
            stopSearchLocateHighlight();
            renderContentLayer();
            return;
        }
        searchLocateFading = true;
        searchLocatedStartMs = GetTickCount64();
        SetTimer(hwnd, kSearchLocateTimer, kNavIndicatorFrameMs, nullptr);
    }

    float searchLocatedAlpha() const {
        if (searchLocatedId == 0)
            return 0.0f;
        if (!searchLocateFading)
            return 1.0f;
        if (searchLocatedStartMs == 0)
            return 0.0f;
        const ULONGLONG fadeElapsed = GetTickCount64() - searchLocatedStartMs;
        if (fadeElapsed >= kSearchLocateFadeMs)
            return 0.0f;
        const float progress =
            static_cast<float>(fadeElapsed) / static_cast<float>(kSearchLocateFadeMs);
        const float remaining = 1.0f - std::clamp(progress, 0.0f, 1.0f);
        return remaining * remaining * (3.0f - 2.0f * remaining);
    }

    float measureTextHeight(fluent::FluentDialogSurface::Painter& painter,
                            const std::wstring& text, float width, float textSize) const {
        if (text.empty() || width <= 0.0f)
            return 0.0f;

        auto* format = painter.textFormat(textSize, 400);
        auto* textLayout = painter.textLayout(text, format, width, kHintMeasureHeight);
        if (!textLayout)
            return 0.0f;

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(textLayout->GetMetrics(&metrics)))
            return 0.0f;
        return static_cast<float>(std::ceil(std::max(0.0f, metrics.height)));
    }

    float measureHintHeight(fluent::FluentDialogSurface::Painter& painter,
                            const Row& row) const {
        if (!row.showHint || row.hint.empty())
            return 0.0f;
        return measureTextHeight(painter, row.hint, row.hintRect.right - row.hintRect.left,
                                 kHintTextSize);
    }

    static bool isSpectrumEffectRow(int id) {
        return id >= kIdSpectrumEffectFirst && id < kIdSpectrumEffectFirst + kSpectrumEffectCount;
    }

    static bool isLyricEffectRow(int id) {
        return id >= kIdLyricEffectFirst && id < kIdLyricEffectFirst + kLyricEffectCount;
    }

    static bool isSegmentedChoice(const Row& row) {
        return row.kind == ControlKind::Radio && row.id != kIdCoverEffect &&
               row.id != kIdSpectrumStyle && row.id != kIdHoverControlStyle &&
               row.id != kIdSongToastPosition && row.id != kIdIdleQuoteBackground;
    }

    void updateHintHeights(fluent::FluentDialogSurface::Painter& painter) {
        bool changed = false;
        for (auto& row : sections_.rows[activePage]) {
            if (row.kind == ControlKind::Header)
                continue;
            float labelW = row.labelRect.right - row.labelRect.left;
            if (isSegmentedChoice(row)) {
                const float availableWidth = std::max(
                    0.0f, contentViewportRect.right - contentViewportRect.left -
                              32.0f - kRowIconSlotW);
                float optionWidth = 0.0f;
                auto* optionFormat = painter.textFormat(13.0f, 400, false, true);
                for (const auto& option : row.options)
                    optionWidth = std::max(optionWidth,
                        painter.measureTextWidth(option, optionFormat));
                const float controlWidth =
                    std::max(64.0f, std::ceil(optionWidth) + 24.0f) *
                    static_cast<float>(row.options.size());
                const float textWidth = std::max(
                    painter.measureTextWidth(row.text,
                        painter.textFormat(14.0f, 400, false, true)),
                    row.showHint ? painter.measureTextWidth(row.hint,
                        painter.textFormat(kHintTextSize, 400, false, true)) : 0.0f);
                const bool inlineChoice =
                    std::ceil(textWidth) + fluent::metrics::pagePadding + controlWidth <=
                    availableWidth;
                if (row.choiceInline != inlineChoice ||
                    std::fabs(row.controlW - controlWidth) > 0.5f) {
                    row.choiceInline = inlineChoice;
                    row.controlW = controlWidth;
                    changed = true;
                }
                labelW = inlineChoice
                             ? availableWidth - controlWidth - fluent::metrics::pagePadding
                             : availableWidth;
            }
            const float measuredTitleHeight =
                measureTextHeight(painter, row.text, labelW, 14.0f);
            const float titleHeight =
                std::max(kTitleMinHeight, measuredTitleHeight);
            const float measuredValueHeight =
                row.id == kIdPickFont
                    ? measureTextHeight(painter, row.valueText, labelW, 13.0f)
                    : 0.0f;
            const float valueHeight =
                row.id == kIdPickFont && !row.valueText.empty()
                    ? std::max(kTitleMinHeight, measuredValueHeight)
                    : 0.0f;
            float requiredHeight = row.minHeight;
            if (isSegmentedChoice(row)) {
                const float hintHeight = row.showHint
                    ? measureTextHeight(painter, row.hint, labelW, kHintTextSize) : 0.0f;
                const float controlBlockHeight = row.choiceInline
                    ? 0.0f : kModeGridTopGap + fluent::metrics::controlHeight;
                requiredHeight = std::max(
                    requiredHeight, kTitleTopPadding + titleHeight +
                                        (hintHeight > 0.0f ? kTitleHintGap + hintHeight : 0.0f) +
                                        controlBlockHeight + kHintBottomPadding);
            } else if (row.id == kIdPickFont) {
                const float hintHeight =
                    measureTextHeight(painter, row.hint, labelW, kHintTextSize);
                if (hintHeight > 0.0f) {
                    const float valueBlockHeight =
                        valueHeight > 0.0f
                            ? kTitleHintGap + valueHeight + kTitleHintGap
                            : 0.0f;
                    requiredHeight = std::max(
                        requiredHeight, kTitleTopPadding + titleHeight + valueBlockHeight +
                                            hintHeight + kHintBottomPadding);
                }
            } else if (row.showHint && !row.hint.empty()) {
                const float hintHeight = measureHintHeight(painter, row);
                if (hintHeight > 0.0f) {
                    if (row.kind == ControlKind::ModeGrid) {
                        // ModeGrid 的提示文字在卡片网格下方，行高必须包含
                        // 网格和提示的完整高度，否则多行提示会溢出卡片底边。
                        const float gridH = kModeGridCardH * 2.0f + kModeGridGap;
                        requiredHeight = std::max(
                            requiredHeight, kTitleTopPadding + titleHeight + kModeGridTopGap +
                                                gridH + kModeGridNoteGap + hintHeight +
                                                kHintBottomPadding);
                    } else if (row.id == kIdIdleQuoteBackground) {
                        const float gridH = kIdleQuoteBackgroundCardH * 2.0f +
                                            kIdleQuoteBackgroundCardGap;
                        requiredHeight = std::max(
                            requiredHeight, kTitleTopPadding + titleHeight + kModeGridTopGap +
                                                gridH + kModeGridNoteGap + hintHeight +
                                                kHintBottomPadding);
                    } else if (row.id == kIdHoverControlStyle) {
                        requiredHeight = std::max(
                            requiredHeight, kTitleTopPadding + titleHeight + kModeGridTopGap +
                                                kHoverControlStyleCardH + kModeGridNoteGap +
                                                hintHeight + kHintBottomPadding);
                    } else {
                        requiredHeight = std::max(
                            requiredHeight, kTitleTopPadding + titleHeight + kTitleHintGap +
                                                hintHeight + kHintBottomPadding);
                    }
                }
            } else if (measuredTitleHeight > 0.0f) {
                requiredHeight = std::max(requiredHeight, kTitleTopPadding + titleHeight +
                                                           kHintBottomPadding);
            }
            if (std::fabs(row.titleHeight - titleHeight) > 0.5f ||
                std::fabs(row.valueHeight - valueHeight) > 0.5f ||
                std::fabs(row.height - requiredHeight) > 0.5f) {
                row.titleHeight = titleHeight;
                row.valueHeight = valueHeight;
                row.height = requiredHeight;
                changed = true;
            }
        }
        if (changed)
            layout();
    }

    void layout() {
        hintHeightsDirty = true;
        contentTextCache.clear();
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);

        search_.setTriggerRect(D2D1::RectF(16.0f, 14.0f, 12.0f + kNavW - 4.0f, 48.0f));
        navRect = D2D1::RectF(12.0f, 58.0f, 12.0f + kNavW, std::max(58.0f, h - 12.0f));
        for (int i = 0; i < kSettingsPageCount; ++i) {
            navItemRects[i] = D2D1::RectF(navRect.left, navRect.top + i * kNavItemH,
                                          navRect.right, navRect.top + (i + 1) * kNavItemH);
            pageTitleIconRects[i] = D2D1::RectF(0, 0, 0, 0);
            pageTitleRects[i] = D2D1::RectF(0, 0, 0, 0);
        }

        const float contentLeft = 12.0f + kNavW + 16.0f;
        const float footerTop = h - 24.0f - kFooterItemCount * kFooterItemH - 20.0f;
        for (int index = 0; index < kFooterItemCount; ++index) {
            footerItemRects[index] = D2D1::RectF(
                navRect.left + 4.0f, footerTop + index * kFooterItemH,
                navRect.right - 4.0f, footerTop + (index + 1) * kFooterItemH);
        }
        versionRect = D2D1::RectF(navRect.left + kNavIconLeft, h - 36.0f,
                                  navRect.right - 12.0f, h - 16.0f);
        const float availableContentW = std::max(20.0f, w - contentLeft - 24.0f);
        const float contentW = std::min(availableContentW, kContentMaxWidthDip);
        // 内容达到最大宽度后，在导航栏右侧的可用区域内居中。
        const float contentX = contentLeft + (availableContentW - contentW) * 0.5f;
        pageTitleIconRects[activePage] =
            D2D1::RectF(contentX, 18.0f, contentX + kPageTitleIconSize,
                        18.0f + kPageTitleIconSize);
        pageTitleRects[activePage] =
            D2D1::RectF(contentX + kPageTitleIconSize + kPageTitleIconTextGap, 14.0f,
                        contentX + contentW, 42.0f);
        const float contentTop = 14.0f + 28.0f + 16.0f;
        const float contentBottom = std::max(contentTop, h - 12.0f);
        contentViewportRect =
            D2D1::RectF(contentX, contentTop, contentX + contentW, contentBottom);
        if (contentWindow) {
            const int left = static_cast<int>(std::floor(contentViewportRect.left * s));
            const int top = static_cast<int>(std::floor(contentViewportRect.top * s));
            const int right = static_cast<int>(std::ceil(contentViewportRect.right * s));
            const int bottom = static_cast<int>(std::ceil(contentViewportRect.bottom * s));
            SetWindowPos(contentWindow, nullptr, left, top, std::max(1, right - left),
                         std::max(1, bottom - top), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        contentLayerReady = false;

        float contentEnd = contentTop;
        auto& pageRows = sections_.rows[activePage];
        for (size_t i = 0; i < pageRows.size(); ++i) {
            auto& row = pageRows[i];
            // 同组设置连续排列，仅在标题和分组之间留白。
            row.gapAfter = row.kind == ControlKind::Header ? kRowGap : 0.0f;
            if (i + 1 < pageRows.size() && pageRows[i + 1].kind == ControlKind::Header)
                row.gapAfter = fluent::metrics::sectionGap;
            contentEnd += row.height + row.gapAfter;
        }
        if (!sections_.rows[activePage].empty())
            contentEnd -= sections_.rows[activePage].back().gapAfter;
        contentHeight = std::max(0.0f, contentEnd - contentTop);
        const float viewportHeight =
            std::max(0.0f, contentViewportRect.bottom - contentViewportRect.top);
        contentMaxScroll = std::max(0.0f, contentHeight - viewportHeight);
        contentScroll = std::clamp(contentScroll, 0.0f, contentMaxScroll);
        contentPaintScroll = std::round(contentScroll * surface.dipScale()) / surface.dipScale();
        updateScrollBarGeometry();

        float y = contentTop - contentPaintScroll;
        for (auto& page : sections_.rows) {
            for (auto& row : page) {
                row.cardRect = D2D1::RectF(0, 0, 0, 0);
                row.iconRect = D2D1::RectF(0, 0, 0, 0);
                row.labelRect = D2D1::RectF(0, 0, 0, 0);
                row.valueRect = D2D1::RectF(0, 0, 0, 0);
                row.hintRect = D2D1::RectF(0, 0, 0, 0);
                row.controlRect = D2D1::RectF(0, 0, 0, 0);
                row.artworkRect = D2D1::RectF(0, 0, 0, 0);
                row.addAppRect = D2D1::RectF(0, 0, 0, 0);
                row.appItemRects.clear();
                row.appEditRects.clear();
                row.appDeleteRects.clear();
                row.optionRects.clear();
            }
        }

        for (auto& row : sections_.rows[activePage]) {
            const float rowH = row.height;
            if (row.kind == ControlKind::Header) {
                // 分组标题无卡片，文字贴内容区左缘、垂直居中
                row.labelRect = D2D1::RectF(contentX + 4.0f, y, contentX + contentW,
                                            y + rowH);
                y += rowH + row.gapAfter;
                continue;
            }
            row.cardRect = D2D1::RectF(contentX, y, contentX + contentW, y + rowH);
            const float innerX = contentX + 16.0f;
            const float innerRight = contentX + contentW - 16.0f;
            const float labelX = innerX + kRowIconSlotW;
            const auto placeIcon = [&](float titleTop) {
                const float iconTop = titleTop +
                                      (row.titleHeight - kRowIconSize) * 0.5f;
                row.iconRect = D2D1::RectF(innerX, iconTop, innerX + kRowIconSize,
                                           iconTop + kRowIconSize);
            };
            if (row.id == kIdIdleApps) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                const float toggleLeft = innerRight - kIdleAppNamesToggleW;
                const float toggleLabelRight = toggleLeft - kIdleAppNamesToggleGap;
                const float toggleLabelLeft = toggleLabelRight - kIdleAppNamesToggleLabelW;
                row.labelRect = D2D1::RectF(labelX, titleTop,
                                            std::max(labelX, toggleLabelLeft - kIdleAppNamesToggleGap),
                                            titleTop + row.titleHeight);
                row.controlRect = D2D1::RectF(toggleLeft, titleTop, innerRight,
                                              titleTop + row.titleHeight);
                const float hintTop = titleTop + row.titleHeight + kTitleHintGap;
                row.hintRect = D2D1::RectF(labelX, hintTop, innerRight,
                                           hintTop + 30.0f);
                const float listTop = y + kIdleAppsListTop;
                const size_t count = sections_.state.idleApps.size();
                row.appItemRects.reserve(count);
                row.appEditRects.reserve(count);
                row.appDeleteRects.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    const float itemTop = listTop + static_cast<float>(i) * kIdleAppItemH;
                    row.appItemRects.push_back(
                        D2D1::RectF(innerX, itemTop, innerRight, itemTop + 44.0f));
                    row.appEditRects.push_back(
                        D2D1::RectF(innerRight - 2.0f * kIdleAppActionW -
                                        kIdleAppActionGap - 12.0f,
                                    itemTop + 10.0f,
                                    innerRight - kIdleAppActionW - kIdleAppActionGap - 12.0f,
                                    itemTop + 40.0f));
                    row.appDeleteRects.push_back(
                        D2D1::RectF(innerRight - kIdleAppActionW - 12.0f, itemTop + 10.0f,
                                    innerRight - 12.0f, itemTop + 40.0f));
                }
                row.addAppRect = D2D1::RectF(
                    innerX + (innerRight - innerX - 132.0f) * 0.5f,
                    y + rowH - 44.0f,
                    innerX + (innerRight - innerX + 132.0f) * 0.5f,
                    y + rowH - 12.0f);
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.id == kIdCoverEffect) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kCoverEffectCardH);
                if (row.showHint) {
                    const float hintTop = row.controlRect.bottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(labelX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.id == kIdIdleQuoteBackground) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                const float gridH = kIdleQuoteBackgroundCardH * 2.0f +
                                    kIdleQuoteBackgroundCardGap;
                row.controlRect = D2D1::RectF(innerX, gridTop, innerRight, gridTop + gridH);
                if (row.showHint) {
                    const float hintTop = row.controlRect.bottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(labelX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.id == kIdHoverControlStyle) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kHoverControlStyleCardH);
                if (row.showHint) {
                    const float hintTop = row.controlRect.bottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(labelX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.id == kIdSongToastPosition) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kSongToastPositionCardH);
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.id == kIdSpectrumStyle) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight,
                    gridTop + kSpectrumStyleCardH * 2.0f + kSpectrumStyleCardGap);
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.id == kIdSpectrumBackground || row.id == kIdSpectrumGradient ||
                isSpectrumEffectRow(row.id) || isLyricEffectRow(row.id)) {
                const float controlX = innerRight - row.controlW;
                const float controlH = 24.0f;
                const float availableW = std::max(0.0f, controlX - innerX);
                const float artworkW = std::min(
                    kSpectrumBackgroundArtworkMaxW,
                    std::max(kSpectrumBackgroundArtworkMinW, availableW * 0.42f));
                const float artworkRight = controlX - kSpectrumBackgroundArtworkGap;
                const float artworkLeft = std::max(innerX, artworkRight - artworkW);
                const float labelRight = std::max(labelX + 20.0f,
                                                  artworkLeft - kSpectrumBackgroundArtworkGap);
                const float titleTop = row.showHint ? y + kTitleTopPadding
                                                    : y + (rowH - row.titleHeight) * 0.5f;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, labelRight,
                                            titleTop + row.titleHeight);
                if (row.showHint) {
                    row.hintRect = D2D1::RectF(labelX,
                        titleTop + row.titleHeight + kTitleHintGap, labelRight,
                        y + rowH - kHintBottomPadding);
                }
                const float artworkH = isSpectrumEffectRow(row.id) || isLyricEffectRow(row.id) ||
                                               row.id == kIdSpectrumGradient
                                           ? kSpectrumEffectArtworkH : kSpectrumBackgroundArtworkH;
                const float artworkTop = y + (rowH - artworkH) * 0.5f;
                row.artworkRect = D2D1::RectF(artworkLeft, artworkTop, artworkRight,
                                              artworkTop + artworkH);
                const float controlY = y + (rowH - controlH) * 0.5f;
                row.controlRect = D2D1::RectF(controlX, controlY,
                                              controlX + row.controlW, controlY + controlH);
                y += rowH + row.gapAfter;
                continue;
            }
            if (row.kind == ControlKind::ModeGrid) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                const float gridH = kModeGridCardH * 2.0f + kModeGridGap;
                const float gridBottom = gridTop + gridH;
                row.controlRect = D2D1::RectF(innerX, gridTop, innerRight, gridBottom);

                if (row.showHint) {
                    const float hintTop = gridBottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(labelX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + row.gapAfter;
                continue;
            }

            if (isSegmentedChoice(row)) {
                const float labelRight = row.choiceInline
                    ? innerRight - row.controlW - fluent::metrics::pagePadding : innerRight;
                const float titleTop = row.choiceInline && !row.showHint
                    ? y + (rowH - row.titleHeight) * 0.5f : y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, labelRight,
                                            titleTop + row.titleHeight);
                const float hintTop = row.labelRect.bottom + kTitleHintGap;
                const float controlTop = row.choiceInline
                    ? y + (rowH - fluent::metrics::controlHeight) * 0.5f
                    : y + rowH - kHintBottomPadding - fluent::metrics::controlHeight;
                row.hintRect = D2D1::RectF(labelX, hintTop, labelRight,
                    row.choiceInline ? y + rowH - kHintBottomPadding
                                     : controlTop - kModeGridTopGap);
                row.controlRect = D2D1::RectF(
                    row.choiceInline ? innerRight - row.controlW : labelX, controlTop, innerRight,
                    controlTop + fluent::metrics::controlHeight);
                y += rowH + row.gapAfter;
                continue;
            }

            const float controlX = innerRight - row.controlW;
            const float labelW = std::max(20.0f, controlX - labelX - 12.0f);
            const float controlH = row.kind == ControlKind::Button
                                       ? fluent::metrics::controlHeight
                                       : 24.0f;
            if (row.id == kIdPickFont) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, labelX + labelW,
                                            titleTop + row.titleHeight);
                const float valueTop = titleTop + row.titleHeight + kTitleHintGap;
                row.valueRect = D2D1::RectF(labelX, valueTop, labelX + labelW,
                                            valueTop + row.valueHeight);
                const float hintTop = valueTop + row.valueHeight +
                                      (row.valueHeight > 0.0f ? kTitleHintGap : 0.0f);
                row.hintRect = D2D1::RectF(labelX, hintTop, labelX + labelW,
                                           y + rowH - kHintBottomPadding);
            } else if (row.showHint) {
                const float titleTop = y + kTitleTopPadding;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, labelX + labelW,
                                            titleTop + row.titleHeight);
                // 提示区域从标题实际高度之后开始，避免窄窗口下标题换行后侵入提示文字。
                const float hintTop = titleTop + row.titleHeight + kTitleHintGap;
                row.hintRect = D2D1::RectF(labelX, hintTop, labelX + labelW,
                                           y + rowH - kHintBottomPadding);
            } else {
                const float titleTop = y + (rowH - row.titleHeight) / 2.0f;
                placeIcon(titleTop);
                row.labelRect = D2D1::RectF(labelX, titleTop, labelX + labelW,
                                            titleTop + row.titleHeight);
            }
            const float controlY = y + (rowH - controlH) / 2.0f;
            row.controlRect = D2D1::RectF(controlX, controlY,
                                          controlX + row.controlW,
                                          controlY + controlH);
            y += rowH + row.gapAfter;
        }
        layoutSliderEditor();
        if (search_.isOpen() || search_.animating())
            layoutSearchPalette();
    }

    void stopNavIndicator() {
        if (hwnd)
            KillTimer(hwnd, kNavIndicatorTimer);
        navIndicatorAnimating = false;
        navIndicatorPosition = static_cast<float>(activePage);
        navIndicatorStretch = 0.0f;
    }

    void advanceNavIndicator() {
        if (!navIndicatorAnimating)
            return;
        const float progress = std::clamp(
            static_cast<float>(GetTickCount64() - navIndicatorStartMs) /
                static_cast<float>(kNavIndicatorDurationMs),
            0.0f, 1.0f);
        if (progress >= 1.0f) {
            stopNavIndicator();
            return;
        }
        float lower = 0.0f;
        float upper = 1.0f;
        for (int iteration = 0; iteration < 16; ++iteration) {
            const float parameter = (lower + upper) * 0.5f;
            const float inverse = 1.0f - parameter;
            const float sample = 3.0f * inverse * inverse * parameter * 0.77f +
                                 3.0f * inverse * parameter * parameter * 0.175f +
                                 parameter * parameter * parameter;
            if (sample < progress)
                lower = parameter;
            else
                upper = parameter;
        }
        const float parameter = (lower + upper) * 0.5f;
        const float eased = parameter * parameter * (3.0f - 2.0f * parameter);
        navIndicatorPosition = navIndicatorFrom +
                               (static_cast<float>(activePage) - navIndicatorFrom) * eased;
        navIndicatorStretch = navIndicatorFromStretch * (1.0f - eased) +
                              8.0f * 4.0f * eased * (1.0f - eased);
    }

    void showPage(int page) {
        stopSearchLocateHighlight();
        if (page < 0 || page >= kSettingsPageCount || page == activePage)
            return;
        stopContentScroll();
        if (slider_.state().open)
            commitSliderEditor();
        advanceNavIndicator();
        navIndicatorFrom = navIndicatorPosition;
        navIndicatorFromStretch = navIndicatorStretch;
        activePage = page;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        if (animations && hwnd && IsWindowVisible(hwnd) &&
            SetTimer(hwnd, kNavIndicatorTimer, kNavIndicatorFrameMs, nullptr)) {
            navIndicatorAnimating = true;
            navIndicatorStartMs = GetTickCount64();
        } else {
            stopNavIndicator();
        }
        hoverId = 0;
        hoverOption = -1;
        pressedId = 0;
        pressedOption = -1;
        focusedId = kIdNav;
        contentScroll = 0.0f;
        layout();
        if (hwnd)
            surface.invalidate();
    }

    void drawNav(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = painter.colors();
        // 导航融入窗口底色，右侧设置卡片承担主要的表面层级。
        const bool triggerHovered = hoverId == kIdSettingsSearch;
        const bool triggerPressed = pressedId == kIdSettingsSearch;
        const bool triggerActive = triggerPressed || search_.isOpen();
        const D2D1_COLOR_F triggerFill =
            triggerActive
                ? p.listSelected
                : (triggerHovered
                       ? p.listHover
                       : D2D1::ColorF(p.listHover.r, p.listHover.g, p.listHover.b,
                                      p.listHover.a * 0.85f));
        painter.fillRoundRect(triggerFill, search_.triggerRect(), 6.0f);
        if (focusedId == kIdSettingsSearch && focusVisible) {
            painter.strokeRoundRect(p.accent, search_.triggerRect(), 1.5f, 6.0f);
        } else {
            painter.strokeRoundRect(triggerHovered || search_.isOpen() ? p.controlStroke
                                                                       : p.cardStroke,
                                    search_.triggerRect(), 1.0f, 6.0f);
        }
        const float triggerCenterY =
            (search_.triggerRect().top + search_.triggerRect().bottom) * 0.5f;
        const D2D1_RECT_F searchIconRect = D2D1::RectF(
            navRect.left + kNavIconLeft, triggerCenterY - kNavIconSize * 0.5f,
            navRect.left + kNavIconLeft + kNavIconSize, triggerCenterY + kNavIconSize * 0.5f);
        if (auto* triggerIconBrush =
                painter.brush(triggerHovered || search_.isOpen() ? p.accent : p.textSecondary)) {
            settings_icon::draw(painter.target(), settings_icon::Kind::Search, searchIconRect,
                                triggerIconBrush, 1.3f);
        }
        const D2D1_RECT_F shortcutBadgeRect =
            D2D1::RectF(search_.triggerRect().right - 48.0f, triggerCenterY - 9.5f,
                        search_.triggerRect().right - 7.0f, triggerCenterY + 9.5f);
        painter.fillRoundRect(p.listHover, shortcutBadgeRect, 4.0f);
        painter.strokeRoundRect(p.cardStroke, shortcutBadgeRect, 1.0f, 4.0f);
        painter.drawText(L"Ctrl+F", painter.textFormat(10.5f, 500, true, true),
                         shortcutBadgeRect, p.textSecondary);
        painter.drawText(L"搜索设置", painter.textFormat(13.5f, 400, false, true),
                         D2D1::RectF(navRect.left + kNavIconLeft + kNavIconSize + kNavIconTextGap,
                                     search_.triggerRect().top, shortcutBadgeRect.left - 6.0f,
                                     search_.triggerRect().bottom),
                         triggerHovered || search_.isOpen() ? p.text : p.textSecondary);
        if (auto* navSeparatorBrush = painter.brush(p.separator)) {
            painter.target()->DrawLine(
                D2D1::Point2F(navRect.left + 12.0f, 53.0f),
                D2D1::Point2F(navRect.right - 12.0f, 53.0f), navSeparatorBrush, 1.0f);
        }

        if (focusedId == kIdNav && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(navRect.left + 1.5f, navRect.top + 1.5f, navRect.right - 1.5f,
                            navRect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }
        auto* format = painter.textFormat(14.0f, 400, false, true);
        for (int i = 0; i < kSettingsPageCount; ++i) {
            const bool selected = i == activePage;
            const bool hovered = hoverId == kIdNav && hoverOption == i;
            if (selected)
                painter.fillRoundRect(p.listSelected,
                                      D2D1::RectF(navItemRects[i].left + 4.0f,
                                                  navItemRects[i].top + 2.0f,
                                                  navItemRects[i].right - 4.0f,
                                                  navItemRects[i].bottom - 2.0f));
            else if (hovered)
                painter.fillRoundRect(p.listHover,
                                      D2D1::RectF(navItemRects[i].left + 4.0f,
                                                  navItemRects[i].top + 2.0f,
                                                  navItemRects[i].right - 4.0f,
                                                  navItemRects[i].bottom - 2.0f));
            const auto icon = iconForPage(i);
            if (icon != settings_icon::Kind::None) {
                const float centerY = (navItemRects[i].top + navItemRects[i].bottom) * 0.5f;
                const D2D1_RECT_F iconRect = D2D1::RectF(
                    navItemRects[i].left + kNavIconLeft, centerY - kNavIconSize * 0.5f,
                    navItemRects[i].left + kNavIconLeft + kNavIconSize,
                    centerY + kNavIconSize * 0.5f);
                if (auto* iconBrush = painter.brush(selected ? p.accent : p.textSecondary))
                    settings_icon::draw(painter.target(), icon, iconRect, iconBrush);
            }
            painter.drawText(navItems[i], format,
                             D2D1::RectF(navItemRects[i].left + kNavIconLeft + kNavIconSize +
                                             kNavIconTextGap,
                                         navItemRects[i].top,
                                         navItemRects[i].right - 12.0f, navItemRects[i].bottom),
                             p.text);
        }
        advanceNavIndicator();
        const float centerY = navRect.top + (navIndicatorPosition + 0.5f) * kNavItemH;
        const float halfHeight = 8.0f + navIndicatorStretch * 0.5f;
        painter.fillRoundRect(
            p.accent,
            D2D1::RectF(navRect.left + 7.0f, centerY - halfHeight,
                        navRect.left + 10.0f, centerY + halfHeight),
            1.5f);
        const wchar_t* footerLabels[] = {L"文档中心", L"问题反馈", L"关于与更新"};
        const settings_icon::Kind footerIcons[] = {
            settings_icon::Kind::Lyrics, settings_icon::Kind::Toast, settings_icon::Kind::Info};
        for (int index = 0; index < kFooterItemCount; ++index) {
            const int id = kIdOpenDocs + index;
            const auto& rect = footerItemRects[index];
            if (pressedId == id && hoverId == id)
                painter.fillRoundRect(p.controlPressed, rect, 6.0f);
            else if (hoverId == id)
                painter.fillRoundRect(p.listHover, rect, 6.0f);
            if (focusedId == id && focusVisible)
                painter.strokeRoundRect(p.accent, rect, 1.5f, 6.0f);
            const float iconTop = (rect.top + rect.bottom - kNavIconSize) * 0.5f;
            if (auto* brush = painter.brush(p.textSecondary))
                settings_icon::draw(painter.target(), footerIcons[index],
                    D2D1::RectF(navRect.left + kNavIconLeft, iconTop,
                                navRect.left + kNavIconLeft + kNavIconSize,
                                iconTop + kNavIconSize), brush);
            painter.drawText(footerLabels[index], painter.textFormat(12.5f, 400, false, true),
                D2D1::RectF(navRect.left + kNavIconLeft + kNavIconSize + kNavIconTextGap,
                            rect.top, rect.right - 8.0f, rect.bottom), p.textSecondary);
        }
        painter.drawText(std::wstring(L"版本 ") + app_info::kVersion,
                         painter.textFormat(11.0f, 400, false, true), versionRect,
                         p.textSecondary);
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = painter.colors();
        const bool hovered = hoverId == row.id;
        const bool pressed = pressedId == row.id;
        D2D1_COLOR_F fill = pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill;
        D2D1_COLOR_F textColor = row.enabled ? p.text : p.disabled;
        if (!row.enabled)
            fill = p.listHover;
        painter.fillRoundRect(fill, row.controlRect);
        painter.strokeRoundRect(p.controlStroke, row.controlRect);
        if (focusedId == row.id && focusVisible && row.enabled) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(row.controlRect.left + 1.5f, row.controlRect.top + 1.5f,
                            row.controlRect.right - 1.5f, row.controlRect.bottom - 1.5f),
                1.5f, fluent::metrics::controlRadius - 1.0f);
        }
        if (row.id == kIdFloatingCardBackgroundColor) {
            auto* format = painter.textFormat(14.0f, 400, false, true);
            constexpr float kPreviewSize = 18.0f;
            constexpr float kPreviewGap = 8.0f;
            const float textW = format ? painter.measureTextWidth(row.controlText, format) : 0.0f;
            const float groupW = kPreviewSize + kPreviewGap + textW;
            const float centerX = (row.controlRect.left + row.controlRect.right) * 0.5f;
            const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
            const float groupLeft = centerX - groupW * 0.5f;
            const D2D1_RECT_F previewRect =
                D2D1::RectF(groupLeft, centerY - kPreviewSize * 0.5f,
                            groupLeft + kPreviewSize, centerY + kPreviewSize * 0.5f);
            painter.fillRoundRect(fluent::toD2D(sections_.state.floatingCardBackgroundColor),
                                  previewRect, 4.0f);
            painter.strokeRoundRect(row.enabled ? p.cardStroke : p.disabled, previewRect, 1.0f,
                                    4.0f);
            if (format) {
                painter.drawText(
                    row.controlText, format,
                    D2D1::RectF(groupLeft + kPreviewSize + kPreviewGap, row.controlRect.top,
                                row.controlRect.right - 4.0f, row.controlRect.bottom),
                    textColor);
            }
            return;
        }
        painter.drawText(row.controlText, painter.textFormat(14.0f, 400, true, true),
                         D2D1::RectF(row.controlRect.left + 4.0f, row.controlRect.top,
                                     row.controlRect.right - 4.0f, row.controlRect.bottom),
                         textColor);
    }

    void drawToggle(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                    bool hovered, bool focused) {
        const auto& p = painter.colors();
        const bool enabled = row.enabled;
        const float trackH = std::min(20.0f, row.controlRect.bottom - row.controlRect.top);
        const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(
            row.controlRect.left + 0.5f, centerY - trackH * 0.5f,
            row.controlRect.right - 0.5f, centerY + trackH * 0.5f);
        const float radius = trackH * 0.5f;
        const float knobR = trackH * 0.5f - 3.5f;
        const float knobX = row.checked ? track.right - trackH * 0.5f
                                        : track.left + trackH * 0.5f;
        if (!enabled) {
            painter.fillRoundRect(p.listHover, track, radius);
            painter.strokeRoundRect(p.controlStroke, track, 1.0f, radius);
        } else if (row.checked) {
            painter.fillRoundRect(hovered ? p.accentHover : p.accent, track, radius);
        } else {
            painter.fillRoundRect(hovered ? p.controlHover : p.controlFill, track, radius);
            painter.strokeRoundRect(p.textSecondary, track, 1.0f, radius);
        }
        if (auto* br = painter.brush(!enabled ? p.disabled
                                               : row.checked ? p.textOnAccent : p.textSecondary)) {
            painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR),
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

    void drawToggle(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        drawToggle(painter, row, hoverId == row.id, focusedId == row.id && focusVisible);
    }

    void drawSlider(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = painter.colors();
        const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        constexpr float trackH = 4.0f;
        const float trackLeft = row.controlRect.left;
        const float trackRight = row.controlRect.right - kSliderValueW;
        const float range = static_cast<float>(std::max(1, row.maxValue - row.minValue));
        const float t = std::clamp(
            (static_cast<float>(row.value - row.minValue) / range), 0.0f, 1.0f);
        const float knobX = trackLeft + (trackRight - trackLeft) * t;
        const D2D1_RECT_F track =
            D2D1::RectF(trackLeft, centerY - trackH * 0.5f, trackRight,
                        centerY + trackH * 0.5f);
        const D2D1_RECT_F filled =
            D2D1::RectF(track.left, track.top, knobX, track.bottom);
        painter.fillRoundRect(row.enabled ? p.controlFill : p.listHover, track, trackH * 0.5f);
        if (knobX > track.left)
            painter.fillRoundRect(row.enabled ? p.accent : p.disabled, filled, trackH * 0.5f);
        if (auto* br = painter.brush(row.enabled
                                         ? (hoverId == row.id ? p.accentHover : p.accent)
                                         : p.disabled)) {
            painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), 7.0f, 7.0f),
                                                       br);
        }
        const bool editing = slider_.state().open && slider_.state().rowId == row.id;
        const auto valueRect = sliderValueRect(row);
        const auto numberRect = sliderNumberRect(row);
        painter.fillRoundRect(row.enabled ? p.controlFill : p.listHover, valueRect);
        painter.strokeRoundRect(editing ? p.accent : p.controlStroke, valueRect);
        auto* numberFormat = painter.textFormat(12.0f, 600, true, true);
        const std::wstring number = editing ? slider_.state().text : std::to_wstring(row.value);
        painter.target()->PushAxisAlignedClip(numberRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (editing && numberFormat) {
            const float textWidth = painter.measureTextWidth(number, numberFormat);
            const float textLeft = (numberRect.left + numberRect.right - textWidth) * 0.5f;
            for (size_t i = 0; i <= number.size(); ++i)
                slider_.setCaretStop(
                    i, std::clamp(textLeft +
                                      painter.measureTextWidth(number.substr(0, i), numberFormat),
                                  numberRect.left + 1.0f, numberRect.right - 1.0f));
            const size_t first = std::min(slider_.state().caret, slider_.state().anchor);
            const size_t last = std::max(slider_.state().caret, slider_.state().anchor);
            if (first != last)
                painter.fillRoundRect(
                    p.listSelected,
                    D2D1::RectF(slider_.state().caretStops[first], numberRect.top + 4.0f,
                                slider_.state().caretStops[last], numberRect.bottom - 4.0f),
                    1.0f);
        }
        painter.drawText(number, numberFormat, numberRect, row.enabled ? p.text : p.disabled);
        if (editing && slider_.state().caretVisible &&
            slider_.state().caret == slider_.state().anchor) {
            if (auto* brush = painter.brush(p.accent))
                painter.target()->FillRectangle(
                    D2D1::RectF(slider_.state().caretStops[slider_.state().caret],
                                numberRect.top + 5.0f,
                                slider_.state().caretStops[slider_.state().caret] + 1.0f,
                                numberRect.bottom - 5.0f),
                    brush);
        }
        painter.target()->PopAxisAlignedClip();
        painter.drawText(row.valueSuffix, painter.textFormat(12.0f, 600, true, true),
                         D2D1::RectF(numberRect.right, numberRect.top,
                                     valueRect.right - 4.0f, numberRect.bottom),
                         row.enabled ? p.text : p.disabled);
        if (focusedId == row.id && focusVisible && row.enabled)
            painter.strokeRoundRect(p.accent,
                                    D2D1::RectF(track.left - 3.0f, track.top - 5.0f,
                                                track.right + 3.0f, track.bottom + 5.0f),
                                    1.0f, 5.0f);
    }

    void drawRadioCardBackground(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                                 const D2D1_RECT_F& card, bool selected, bool hovered,
                                 bool pressed) const {
        const auto& p = painter.colors();
        D2D1_COLOR_F fill = p.controlFill;
        if (!row.enabled)
            fill = p.listHover;
        else if (pressed)
            fill = p.controlPressed;
        else if (selected)
            fill = p.listSelected;
        else if (hovered)
            fill = p.controlHover;
        painter.fillRoundRect(fill, card, fluent::metrics::controlRadius);

        const bool focused = focusedId == row.id && focusVisible && row.enabled;
        if (selected) {
            painter.strokeRoundRect(row.enabled ? (hovered ? p.accentHover : p.accent)
                                                 : p.disabled,
                                    card, focused ? 2.0f : 1.5f,
                                    fluent::metrics::controlRadius);
        } else {
            painter.strokeRoundRect(p.cardStroke, card, 1.0f,
                                    fluent::metrics::controlRadius);
        }
    }

    void drawRadioIndicator(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                            const D2D1_RECT_F& card, bool selected, bool hovered, bool pressed,
                            float rightInset, float topInset) const {
        const auto& p = painter.colors();
        const D2D1_POINT_2F radioCenter =
            D2D1::Point2F(card.right - rightInset, card.top + topInset);
        if (!row.enabled) {
            if (auto* brush = painter.brush(p.disabled))
                painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                              1.0f);
            if (selected) {
                if (auto* brush = painter.brush(p.disabled))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                  brush);
            }
        } else if (selected) {
            if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush);
            if (auto* brush = painter.brush(p.textOnAccent))
                painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f), brush);
        } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
            painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                          pressed ? 1.5f : 1.0f);
        }
    }

    void drawSpectrumStyleRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = painter.colors();
        auto* titleFormat = painter.textFormat(13.0f, 500, true, true);
        if (!titleFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        row.optionRects.clear();
        const size_t count = row.options.size();
        if (count == 0)
            return;
        const size_t columns = std::min<size_t>(count, 3);
        const float cardW = (gridW - kSpectrumStyleCardGap * static_cast<float>(columns - 1)) /
                            static_cast<float>(columns);
        for (size_t i = 0; i < count; ++i) {
            const float left = row.controlRect.left + static_cast<float>(i % columns) *
                               (cardW + kSpectrumStyleCardGap);
            const float top = row.controlRect.top + static_cast<float>(i / columns) *
                              (kSpectrumStyleCardH + kSpectrumStyleCardGap);
            const D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW,
                                             top + kSpectrumStyleCardH);
            row.optionRects.push_back(card);

            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id &&
                                 hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 8.0f, card.top + 8.0f, card.right - 24.0f,
                            card.top + 60.0f);
            artwork_.drawSpectrumStyleArtwork(painter, artwork, static_cast<int>(i), row.enabled);

            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 6.0f, card.bottom - 25.0f,
                                         card.right - 6.0f, card.bottom - 6.0f),
                             row.enabled ? p.text : p.disabled);

            // 示意图和文字先画，单选框最后画，避免缩放时被内容盖住。
            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 14.0f, 14.0f);
        }
    }

    void drawCoverEffectRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = painter.colors();
        auto* titleFormat = painter.textFormat(13.0f, 500, false, true);
        if (!titleFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        const float cardW = (gridW - kCoverEffectCardGap) * 0.5f;
        row.optionRects.clear();
        const size_t count = std::min<size_t>(row.options.size(), 2);
        for (size_t i = 0; i < count; ++i) {
            const float left = row.controlRect.left + static_cast<float>(i) *
                               (cardW + kCoverEffectCardGap);
            const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                                 row.controlRect.bottom);
            row.optionRects.push_back(card);

            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id &&
                                 hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 16.0f);

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 12.0f, card.top + 14.0f, card.left + 56.0f,
                            card.top + 58.0f);
            artwork_.drawCoverEffectArtwork(painter, artwork, i == 1, row.enabled);
            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 68.0f, card.top + 20.0f,
                                         card.right - 28.0f, card.bottom - 14.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawSongToastPositionRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = painter.colors();
        auto* titleFormat = painter.textFormat(13.0f, 500, true, true);
        if (!titleFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        row.optionRects.clear();
        const size_t count = std::min<size_t>(row.options.size(), 2);
        if (count == 0)
            return;
        const float cardW = std::min(
            kSongToastPositionCardW,
            (gridW - kSongToastPositionCardGap * static_cast<float>(count - 1)) /
                static_cast<float>(count));
        const float cardsW = cardW * static_cast<float>(count) +
                             kSongToastPositionCardGap * static_cast<float>(count - 1);
        const float cardsLeft = row.controlRect.left + (gridW - cardsW) * 0.5f;
        for (size_t i = 0; i < count; ++i) {
            const float left = cardsLeft + static_cast<float>(i) *
                               (cardW + kSongToastPositionCardGap);
            const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                                 row.controlRect.bottom);
            row.optionRects.push_back(card);

            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id &&
                                 hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const float artworkW = std::min(100.0f, cardW - 20.0f);
            const D2D1_RECT_F artwork = D2D1::RectF(
                card.left + (cardW - artworkW) * 0.5f, card.top + 8.0f,
                card.left + (cardW + artworkW) * 0.5f, card.top + 58.0f);
            artwork_.drawSongToastPositionArtwork(painter, artwork, i == 0, row.enabled);

            // 示意图先画，单选框最后画，避免位置指示被单选框遮住。
            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 16.0f);

            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 10.0f, card.top + 65.0f,
                                         card.right - 10.0f, card.bottom - 8.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawIdleQuoteBackgroundRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = painter.colors();
        auto* titleFormat = painter.textFormat(12.5f, 600, true, true);
        auto* noneFormat = painter.textFormat(12.0f, 600, true, true);
        if (!titleFormat || !noneFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        const float cardW = (gridW - kIdleQuoteBackgroundCardGap) * 0.5f;
        row.optionRects.clear();

        const D2D1_RECT_F noneRect =
            D2D1::RectF(row.cardRect.right - 72.0f, row.cardRect.top + 10.0f,
                        row.cardRect.right - 16.0f, row.cardRect.top + 36.0f);
        row.optionRects.push_back(noneRect);

        const bool noneSelected = row.selected == 0;
        const bool noneHovered = row.enabled && hoverId == row.id && hoverOption == 0;
        const bool nonePressed = row.enabled && pressedId == row.id && pressedOption == 0;
        D2D1_COLOR_F noneFill = p.controlFill;
        if (!row.enabled)
            noneFill = p.listHover;
        else if (nonePressed)
            noneFill = p.controlPressed;
        else if (noneSelected)
            noneFill = p.listSelected;
        else if (noneHovered)
            noneFill = p.controlHover;
        painter.fillRoundRect(noneFill, noneRect, 13.0f);
        if (noneSelected) {
            painter.strokeRoundRect(row.enabled ? (noneHovered ? p.accentHover : p.accent)
                                                 : p.disabled,
                                    noneRect,
                                    focusedId == row.id && focusVisible ? 2.0f : 1.0f, 13.0f);
        } else {
            painter.strokeRoundRect(p.cardStroke, noneRect, 1.0f, 13.0f);
        }
        const std::wstring noneText = row.options.empty() ? std::wstring(L"无") : row.options[0];
        painter.drawText(noneText, noneFormat, noneRect,
                         row.enabled ? p.text : p.disabled);

        const size_t count = row.options.size() > 1
                                 ? std::min<size_t>(row.options.size() - 1, 4)
                                 : 0;
        for (size_t i = 0; i < count; ++i) {
            const int optionIndex = static_cast<int>(i + 1);
            const int column = static_cast<int>(i % 2);
            const int line = static_cast<int>(i / 2);
            const float left = row.controlRect.left +
                               column * (cardW + kIdleQuoteBackgroundCardGap);
            const float top = row.controlRect.top +
                              line * (kIdleQuoteBackgroundCardH + kIdleQuoteBackgroundCardGap);
            const D2D1_RECT_F card =
                D2D1::RectF(left, top, left + cardW, top + kIdleQuoteBackgroundCardH);
            row.optionRects.push_back(card);

            const bool selected = optionIndex == row.selected;
            const bool hovered = row.enabled && hoverId == row.id &&
                                 hoverOption == optionIndex;
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == optionIndex;
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 10.0f,
                            card.bottom - 31.0f);
            artwork_.drawIdleQuoteBackgroundArtwork(painter, artwork, optionIndex, row.enabled);

            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 14.0f);

            painter.drawText(row.options[optionIndex], titleFormat,
                             D2D1::RectF(card.left + 10.0f, card.bottom - 24.0f,
                                         card.right - 10.0f, card.bottom - 5.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawHoverControlStyleRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = painter.colors();
        auto* titleFormat = painter.textFormat(12.5f, 500, true, true);
        if (!titleFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        const float cardW = (gridW - kCoverEffectCardGap) * 0.5f;
        row.optionRects.clear();
        const size_t count = std::min<size_t>(row.options.size(), 2);
        for (size_t i = 0; i < count; ++i) {
            const float left = row.controlRect.left + static_cast<float>(i) *
                               (cardW + kCoverEffectCardGap);
            const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                                 row.controlRect.bottom);
            row.optionRects.push_back(card);

            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id &&
                                 hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 30.0f,
                            card.bottom - 27.0f);
            artwork_.drawHoverControlStyleArtwork(painter, artwork, i == 1, row.enabled);

            // 示意图先画，单选框最后画，确保它始终位于卡片内容之上。
            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 14.0f);

            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 10.0f, card.bottom - 24.0f,
                                         card.right - 10.0f, card.bottom - 5.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        if (row.id == kIdSpectrumStyle) {
            drawSpectrumStyleRadio(painter, row);
            return;
        }
        if (row.id == kIdCoverEffect) {
            drawCoverEffectRadio(painter, row);
            return;
        }
        if (row.id == kIdHoverControlStyle) {
            drawHoverControlStyleRadio(painter, row);
            return;
        }
        if (row.id == kIdSongToastPosition) {
            drawSongToastPositionRadio(painter, row);
            return;
        }
        if (row.id == kIdIdleQuoteBackground) {
            drawIdleQuoteBackgroundRadio(painter, row);
            return;
        }

        const auto& p = painter.colors();
        auto* format = painter.textFormat(13.0f, 400, true, true);
        if (!format)
            return;
        const float width = row.controlRect.right - row.controlRect.left;
        const float optionWidth =
            width / static_cast<float>(std::max<size_t>(1, row.options.size()));
        painter.fillRoundRect(p.listHover, row.controlRect);
        row.optionRects.clear();
        for (size_t i = 0; i < row.options.size(); ++i) {
            const bool optionEnabled = row.enabled && isOptionEnabled(row, i);
            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = optionEnabled && hoverId == row.id &&
                                 hoverOption == static_cast<int>(i);
            const bool pressed = optionEnabled && pressedId == row.id &&
                                  pressedOption == static_cast<int>(i);
            const float left = row.controlRect.left + static_cast<float>(i) * optionWidth;
            const D2D1_RECT_F optionRect = D2D1::RectF(
                left, row.controlRect.top, left + optionWidth, row.controlRect.bottom);
            const D2D1_RECT_F face = D2D1::RectF(
                optionRect.left + 2.0f, optionRect.top + 2.0f,
                optionRect.right - 2.0f, optionRect.bottom - 2.0f);
            if (selected) {
                painter.fillRoundRect(
                    optionEnabled ? (pressed ? p.accentPressed : hovered ? p.accentHover : p.accent)
                                  : p.listSelected,
                    face);
            } else if (hovered || pressed) {
                painter.fillRoundRect(pressed ? p.controlPressed : p.controlHover, face);
            }
            if (selected && optionEnabled && focusedId == row.id && focusVisible)
                painter.strokeRoundRect(
                    p.textOnAccent,
                    D2D1::RectF(face.left + 1.5f, face.top + 1.5f,
                                face.right - 1.5f, face.bottom - 1.5f),
                    1.0f, fluent::metrics::controlRadius - 1.0f);
            painter.drawText(row.options[i], format,
                             D2D1::RectF(face.left + 4.0f, face.top,
                                         face.right - 4.0f, face.bottom),
                             optionEnabled ? (selected ? p.textOnAccent : p.text) : p.disabled);
            row.optionRects.push_back(optionRect);
        }
    }

    void drawModeGrid(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = painter.colors();
        auto* titleFormat = painter.textFormat(13.0f, 600, false, true);
        auto* hintFormat = painter.textFormat(11.0f, 400);
        if (!titleFormat || !hintFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        const float cardW = (gridW - kModeGridGap) * 0.5f;
        row.optionRects.clear();
        const size_t count = std::min<size_t>(row.options.size(), 4);
        for (size_t i = 0; i < count; ++i) {
            const int column = static_cast<int>(i % 2);
            const int line = static_cast<int>(i / 2);
            const float left = row.controlRect.left + column * (cardW + kModeGridGap);
            const float top = row.controlRect.top + line * (kModeGridCardH + kModeGridGap);
            const D2D1_RECT_F card =
                D2D1::RectF(left, top, left + cardW, top + kModeGridCardH);
            row.optionRects.push_back(card);

            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id &&
                                 hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            D2D1_COLOR_F fill = p.controlFill;
            if (!row.enabled)
                fill = p.listHover;
            else if (pressed)
                fill = p.controlPressed;
            else if (selected)
                fill = p.listSelected;
            else if (hovered)
                fill = p.controlHover;
            painter.fillRoundRect(fill, card, fluent::metrics::controlRadius);

            if (selected) {
                painter.strokeRoundRect(hovered ? p.accentHover : p.accent, card,
                                        focusedId == row.id && focusVisible ? 2.0f : 1.5f,
                                        fluent::metrics::controlRadius);
                if (auto* br = painter.brush(row.enabled ? p.accent : p.disabled)) {
                    painter.target()->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(card.right - 14.0f, card.top + 14.0f),
                                      3.0f, 3.0f),
                        br);
                }
            } else {
                painter.strokeRoundRect(p.cardStroke, card, 1.0f,
                                        fluent::metrics::controlRadius);
            }

            const D2D1_COLOR_F titleColor = row.enabled ? p.text : p.disabled;
            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 12.0f, card.top + 8.0f,
                                         card.right - 12.0f, card.top + 28.0f),
                             titleColor);
            if (i < row.optionHints.size() && !row.optionHints[i].empty()) {
                painter.drawText(row.optionHints[i], hintFormat,
                                 D2D1::RectF(card.left + 12.0f, card.top + 29.0f,
                                             card.right - 12.0f, card.bottom - 8.0f),
                                 row.enabled ? p.textSecondary : p.disabled);
            }
        }
    }

    void drawIdleApps(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = painter.colors();
        const float innerX = row.cardRect.left + 16.0f;
        const float innerRight = row.cardRect.right - 16.0f;
        const float listTop = row.cardRect.top + kIdleAppsListTop;
        const size_t count = sections_.state.idleApps.size();
        auto* nameFormat = painter.textFormat(13.0f, 600, false, true);
        auto* pathFormat = painter.textFormat(11.0f, 400, false, true);
        auto* actionFormat = painter.textFormat(11.0f, 600, true, true);
        if (!nameFormat || !pathFormat || !actionFormat)
            return;

        auto drawAction = [&](const D2D1_RECT_F& rect, const std::wstring& text, bool enabled,
                              bool hovered, bool pressed) {
            const D2D1_COLOR_F fill = !enabled ? p.listHover
                                               : pressed ? p.controlPressed
                                                         : hovered ? p.controlHover
                                                                   : p.controlFill;
            painter.fillRoundRect(fill, rect, 6.0f);
            painter.strokeRoundRect(enabled ? p.cardStroke : p.disabled, rect, 1.0f, 6.0f);
            painter.drawText(text, actionFormat, rect, enabled ? p.text : p.disabled);
        };

        const D2D1_RECT_F toggleLabelRect =
            D2D1::RectF(row.controlRect.left - kIdleAppNamesToggleGap -
                            kIdleAppNamesToggleLabelW,
                        row.controlRect.top, row.controlRect.left - kIdleAppNamesToggleGap,
                        row.controlRect.bottom);
        painter.drawText(L"显示名称", painter.textFormat(12.0f, 400, true, true),
                         toggleLabelRect, row.enabled ? p.textSecondary : p.disabled);
        drawToggle(painter, row,
                   row.enabled && hoverId == row.id && hoverOption == kIdleAppNamesOption,
                   focusedId == kIdIdleAppNames && focusVisible);

        for (size_t i = 0; i < count; ++i) {
            const auto& app = sections_.state.idleApps[i];
            const D2D1_RECT_F item = row.appItemRects[i];
            const int option = hoverId == row.id ? hoverOption : -1;
            const int pressedOptionValue = pressedId == row.id ? pressedOption : -1;
            const bool appHovered = option == static_cast<int>(i) ||
                                    option == kIdleAppEditOptionBase + static_cast<int>(i) ||
                                    option == kIdleAppDragOptionBase + static_cast<int>(i);
            const bool appPressed =
                pressedOptionValue == static_cast<int>(i) ||
                pressedOptionValue == kIdleAppEditOptionBase + static_cast<int>(i) ||
                pressedOptionValue == kIdleAppDragOptionBase + static_cast<int>(i);
            const bool hovered = row.enabled && hoverId == row.id &&
                                 appHovered;
            const bool pressed = row.enabled && pressedId == row.id &&
                                 appPressed;
            const bool dragging = idleAppDragging && idleAppDragIndex == static_cast<int>(i);
            painter.fillRoundRect(row.enabled ? (hovered ? p.controlHover : p.controlFill)
                                              : p.listHover,
                                  item, 7.0f);
            if (pressed)
                painter.fillRoundRect(p.controlPressed, item, 7.0f);
            if (dragging)
                painter.strokeRoundRect(p.accent, item, 1.5f, 7.0f);

            const D2D1_RECT_F iconRect =
                D2D1::RectF(item.left + 8.0f, item.top + 8.0f, item.left + 36.0f,
                            item.top + 36.0f);
            if (app.iconPixels && !app.iconPixels->empty() && app.iconWidth > 0 &&
                app.iconHeight > 0 && painter.target()) {
                ID2D1Bitmap* bitmap = nullptr;
                const auto props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED),
                    static_cast<float>(surface.dpi()), static_cast<float>(surface.dpi()));
                if (SUCCEEDED(painter.target()->CreateBitmap(
                        D2D1::SizeU(app.iconWidth, app.iconHeight), app.iconPixels->data(),
                        app.iconWidth * 4, &props, &bitmap)) &&
                    bitmap) {
                    painter.target()->DrawBitmap(bitmap, iconRect, row.enabled ? 1.0f : 0.45f,
                                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    bitmap->Release();
                }
            }

            const D2D1_RECT_F nameRect =
                D2D1::RectF(item.left + 46.0f, item.top + 3.0f,
                            item.right - kIdleAppInfoRightInset, item.top + 23.0f);
            const D2D1_RECT_F pathRect =
                D2D1::RectF(item.left + 46.0f, item.top + 22.0f,
                            item.right - kIdleAppInfoRightInset, item.bottom - 3.0f);
            painter.drawTrimmedText(app.name.empty() ? L"未命名应用" : app.name, nameFormat,
                                    nameRect, row.enabled && app.pathValid ? p.text : p.disabled);
            painter.drawTrimmedText(app.pathValid ? app.path : L"路径失效，请重新选择",
                                    pathFormat, pathRect,
                                    row.enabled && app.pathValid ? p.textSecondary : p.disabled);

            const D2D1_RECT_F editRect = row.appEditRects[i];
            const D2D1_RECT_F deleteRect = row.appDeleteRects[i];
            drawAction(editRect, L"修改", row.enabled,
                       row.enabled && option == kIdleAppEditOptionBase + static_cast<int>(i),
                       row.enabled && pressedOptionValue == kIdleAppEditOptionBase +
                                                      static_cast<int>(i));
            drawAction(deleteRect, L"删除", row.enabled,
                       row.enabled && option == static_cast<int>(i),
                       row.enabled && pressedOptionValue == static_cast<int>(i));
        }

        if (idleAppDragging && idleAppDropIndex >= 0) {
            const float dropY = listTop + static_cast<float>(idleAppDropIndex) * kIdleAppItemH - 4.0f;
            painter.fillRoundRect(
                p.accent,
                D2D1::RectF(innerX + 4.0f, dropY - 1.0f, innerRight - 4.0f, dropY + 1.0f),
                1.0f);
        }

        if (count == 0)
            painter.drawText(L"未添加应用", pathFormat,
                             D2D1::RectF(innerX + 8.0f, listTop + 10.0f, innerRight - 8.0f,
                                         listTop + 34.0f),
                             row.enabled ? p.textSecondary : p.disabled);

        const bool canAdd = row.enabled && count < kMaxIdleApps;
        const bool addHovered = canAdd && hoverId == row.id && hoverOption == -1;
        const bool addPressed = canAdd && pressedId == row.id && pressedOption == -1;
        painter.fillRoundRect(
            canAdd ? (addPressed ? p.accentPressed : addHovered ? p.accentHover : p.accent)
                   : p.listHover,
            row.addAppRect, 7.0f);
        painter.strokeRoundRect(canAdd ? p.accent : p.disabled, row.addAppRect, 1.0f, 7.0f);
        painter.drawText(L"添加应用", painter.textFormat(13.0f, 600, true, true),
                         row.addAppRect, canAdd ? p.textOnAccent : p.disabled);
    }

    void drawScrollBar(fluent::FluentDialogSurface::Painter& painter) {
        if (contentMaxScroll <= 0.0f || scrollThumbRect.bottom <= scrollThumbRect.top)
            return;
        const auto& p = painter.colors();
        const bool active = hoverId == kIdContentScrollBar ||
                            pressedId == kIdContentScrollBar || scrollDragging;
        painter.fillRoundRect(active ? p.accent : p.textSecondary, scrollThumbRect,
                              kScrollBarWidth * 0.5f);
    }

    void drawSettingGroups(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = painter.colors();
        const auto& pageRows = sections_.rows[activePage];
        for (size_t first = 0; first < pageRows.size();) {
            if (pageRows[first].kind == ControlKind::Header) {
                ++first;
                continue;
            }
            size_t end = first + 1;
            while (end < pageRows.size() && pageRows[end].kind != ControlKind::Header)
                ++end;
            const auto& top = pageRows[first].cardRect;
            const auto& bottom = pageRows[end - 1].cardRect;
            const auto group = D2D1::RectF(top.left, top.top, top.right, bottom.bottom);
            {
                painter.fillRoundRect(p.cardFill, group, fluent::metrics::cardRadius);
                painter.strokeRoundRect(p.cardStroke, group, 1.0f, fluent::metrics::cardRadius);
                if (auto* brush = painter.brush(p.separator)) {
                    for (size_t i = first + 1; i < end; ++i) {
                        const float y = pageRows[i].cardRect.top;
                        painter.target()->DrawLine(
                            D2D1::Point2F(group.left + 16.0f, y),
                            D2D1::Point2F(group.right - 16.0f, y), brush, 1.0f);
                    }
                }
            }
            first = end;
        }
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float,
               bool navOnly = false, bool contentOnly = false, bool scrollBarOnly = false) {
        if (scrollBarOnly) {
            drawScrollBar(painter);
            return;
        }
        if (hintHeightsDirty && !navOnly && !contentOnly && !contentScrollAnimating) {
            updateHintHeights(painter);
            // updateHintHeights 内部可能调整行高并重新布局；本次已完成相应量测。
            hintHeightsDirty = false;
        }
        const auto& p = painter.colors();
        if (!contentOnly)
            drawNav(painter);
        if (navOnly)
            return;
        if (!contentOnly) {
            if (const auto icon = iconForPage(activePage);
                icon != settings_icon::Kind::None) {
                if (auto* iconBrush = painter.brush(p.accent))
                    settings_icon::draw(painter.target(), icon, pageTitleIconRects[activePage],
                                        iconBrush, 1.35f);
            }
            painter.drawText(pageTitles[activePage], painter.textFormat(20.0f, 600),
                             pageTitleRects[activePage], p.text);
        }

        renderContentLayer();
        drawScrollBar(painter);
    }

    void paintContent(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = painter.colors();
        drawSettingGroups(painter);
        const float locatedAlpha = searchLocatedAlpha();
        if (const auto* located = findRow(searchLocatedId);
            located && searchLocatedId != 0 && locatedAlpha > 0.004f) {
            const bool dark = fluent::isDarkMode();
            const D2D1_RECT_F highlightRect = D2D1::RectF(
                located->cardRect.left + 4.0f, located->cardRect.top + 3.0f,
                located->cardRect.right - 4.0f, located->cardRect.bottom - 3.0f);
            painter.fillRoundRect(
                D2D1::ColorF(p.accent.r, p.accent.g, p.accent.b,
                             (dark ? 0.14f : 0.085f) * locatedAlpha),
                highlightRect, 6.0f);
            painter.strokeRoundRect(
                D2D1::ColorF(p.accent.r, p.accent.g, p.accent.b,
                             (dark ? 0.28f : 0.18f) * locatedAlpha),
                highlightRect, 1.0f, 6.0f);
            const float indicatorCenterY =
                located->iconRect.bottom > located->iconRect.top
                    ? (located->iconRect.top + located->iconRect.bottom) * 0.5f
                    : (located->cardRect.top + located->cardRect.bottom) * 0.5f;
            painter.fillRoundRect(
                D2D1::ColorF(p.accent.r, p.accent.g, p.accent.b, locatedAlpha),
                D2D1::RectF(located->cardRect.left + 7.5f, indicatorCenterY - 9.0f,
                            located->cardRect.left + 10.5f, indicatorCenterY + 9.0f),
                1.5f);
        }
        for (auto& row : sections_.rows[activePage]) {
            if (row.kind == ControlKind::Header) {
                painter.drawText(row.text, painter.textFormat(13.0f, 600), row.labelRect,
                                 p.text);
                continue;
            }
            const auto icon = iconForRow(row.id);
            if (icon != settings_icon::Kind::None) {
                D2D1_COLOR_F iconColor = row.enabled ? p.textSecondary : p.disabled;
                if (row.enabled && row.id == searchLocatedId && locatedAlpha > 0.004f) {
                    const float inverseAlpha = 1.0f - locatedAlpha;
                    iconColor = D2D1::ColorF(
                        iconColor.r * inverseAlpha + p.accent.r * locatedAlpha,
                        iconColor.g * inverseAlpha + p.accent.g * locatedAlpha,
                        iconColor.b * inverseAlpha + p.accent.b * locatedAlpha,
                        iconColor.a * inverseAlpha + p.accent.a * locatedAlpha);
                }
                if (auto* iconBrush = painter.brush(iconColor))
                    settings_icon::draw(painter.target(), icon, row.iconRect, iconBrush);
            }
            painter.drawText(row.text, painter.textFormat(14.0f, 400), row.labelRect,
                             row.enabled ? p.text : p.disabled);
            if (row.id == kIdPickFont && !row.valueText.empty())
                painter.drawText(row.valueText, painter.textFormat(13.0f, 600),
                                 row.valueRect, row.enabled ? p.accent : p.disabled);
            if (row.showHint)
                painter.drawText(row.hint, painter.textFormat(12.0f, 400), row.hintRect,
                                 row.enabled ? p.textSecondary : p.disabled);
            if (row.kind == ControlKind::AppList) {
                drawIdleApps(painter, row);
                continue;
            }
            if (row.id == kIdSpectrumBackground)
                artwork_.drawSpectrumBackgroundArtwork(painter, row.artworkRect, row.enabled);
            else if (row.id == kIdSpectrumGradient)
                artwork_.drawSpectrumEffectArtwork(painter, row.artworkRect, SpectrumEffect::Glow,
                                           row.enabled, true);
            else if (isSpectrumEffectRow(row.id))
                artwork_.drawSpectrumEffectArtwork(painter, row.artworkRect,
                    kSpectrumEffects[row.id - kIdSpectrumEffectFirst], row.enabled);
            else if (isLyricEffectRow(row.id))
                artwork_.drawLyricEffectArtwork(painter, row.artworkRect,
                    static_cast<LyricEffect>(row.id - kIdLyricEffectFirst), row.enabled);
            if (row.kind == ControlKind::Toggle)
                drawToggle(painter, row);
            else if (row.kind == ControlKind::Radio)
                drawRadio(painter, row);
            else if (row.kind == ControlKind::ModeGrid)
                drawModeGrid(painter, row);
            else if (row.kind == ControlKind::Slider)
                drawSlider(painter, row);
            else
                drawButton(painter, row);
        }
    }

    void renderContentLayer() {
        if (!contentWindow)
            return;
        const auto started = runtime_log::uiTimeMs();
        const float scale = surface.dipScale();
        const float left = std::floor(contentViewportRect.left * scale) / scale;
        const float top = std::floor(contentViewportRect.top * scale) / scale;
        const int width = static_cast<int>(std::ceil(contentViewportRect.right * scale) -
                                           std::floor(contentViewportRect.left * scale));
        const int height = static_cast<int>(std::ceil(
            std::max(contentHeight + 2.0f,
                     contentViewportRect.bottom - contentViewportRect.top) * scale));
        contentRenderer.setDpi(surface.dpi());
        if (!contentRenderer.bind(contentWindow, std::max(1, width), std::max(1, height))) {
            contentLayerReady = false;
            return;
        }
        auto* target = contentRenderer.renderTarget();
        const auto bound = runtime_log::uiTimeMs();
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Translation(-left, contentPaintScroll - top));
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        {
            fluent::FluentDialogSurface::Painter painter(target, contentRenderer.dwrite(),
                                                        &contentTextCache);
            paintContent(painter);
        }
        const auto painted = runtime_log::uiTimeMs();
        const HRESULT result = target->EndDraw();
        const auto drawn = runtime_log::uiTimeMs();
        if (FAILED(result)) {
            contentRenderer.discard();
            contentLayerReady = false;
            return;
        }
        if (contentScrollAnimating) {
            advanceContentScroll();
            contentRenderer.animateRootScroll(-contentScroll * scale, -std::round(contentScrollTarget * scale),
                                               -contentScrollVelocity * scale,
                                               kContentScrollFrequency);
        } else {
            contentRenderer.setRootOffsetY(-std::round(contentScroll * scale));
        }
        contentLayerReady = contentRenderer.present();
        const auto presented = runtime_log::uiTimeMs();
        if (presented - started >= 24) {
            runtime_log::writef(
                L"[ui][settings-content] page=%d size=%dx%d bind=%llums paint=%llums "
                L"end-draw=%llums present=%llums", activePage, width, height,
                static_cast<unsigned long long>(bound - started),
                static_cast<unsigned long long>(painted - bound),
                static_cast<unsigned long long>(drawn - painted),
                static_cast<unsigned long long>(presented - drawn));
        }
        if (!contentLayerReady)
            contentRenderer.discard();
    }

    int hitTest(float x, float y, int* option = nullptr) const {
        if (option)
            *option = -1;
        if (contains(search_.triggerRect(), x, y))
            return kIdSettingsSearch;
        for (int index = 0; index < kFooterItemCount; ++index) {
            if (contains(footerItemRects[index], x, y))
                return kIdOpenDocs + index;
        }
        for (int i = 0; i < kSettingsPageCount; ++i) {
            if (contains(navItemRects[i], x, y)) {
                if (option)
                    *option = i;
                return kIdNav;
            }
        }
        if (contains(scrollBarHitRect(), x, y))
            return kIdContentScrollBar;
        if (!contains(contentViewportRect, x, y))
            return 0;
        for (const auto& row : sections_.rows[activePage]) {
            if (!row.enabled)
                continue;
            if (row.kind == ControlKind::AppList) {
                if (contains(row.controlRect, x, y)) {
                    if (option)
                        *option = kIdleAppNamesOption;
                    return row.id;
                }
                for (size_t i = 0; i < row.appEditRects.size(); ++i) {
                    if (contains(row.appEditRects[i], x, y)) {
                        if (option)
                            *option = kIdleAppEditOptionBase + static_cast<int>(i);
                        return row.id;
                    }
                }
                for (size_t i = 0; i < row.appDeleteRects.size(); ++i) {
                    if (contains(row.appDeleteRects[i], x, y)) {
                        if (option)
                            *option = static_cast<int>(i);
                        return row.id;
                    }
                }
                for (size_t i = 0; i < row.appItemRects.size(); ++i) {
                    if (contains(row.appItemRects[i], x, y)) {
                        if (option)
                            *option = kIdleAppDragOptionBase + static_cast<int>(i);
                        return row.id;
                    }
                }
                if (sections_.state.idleApps.size() < kMaxIdleApps &&
                    contains(row.addAppRect, x, y))
                    return row.id;
                continue;
            }
            bool inControl = contains(row.controlRect, x, y);
            if (row.id == kIdSpectrumBackground || row.id == kIdSpectrumGradient ||
                isSpectrumEffectRow(row.id) || isLyricEffectRow(row.id))
                inControl = inControl || contains(row.artworkRect, x, y);
            if (row.id == kIdIdleQuoteBackground && !row.optionRects.empty())
                inControl = inControl || contains(row.optionRects.front(), x, y);
            if (!inControl)
                continue;
            if (option && (row.kind == ControlKind::Radio || row.kind == ControlKind::ModeGrid)) {
                for (size_t i = 0; i < row.optionRects.size(); ++i) {
                    if (contains(row.optionRects[i], x, y)) {
                        if (!isOptionEnabled(row, i))
                            return 0;
                        *option = static_cast<int>(i);
                        break;
                    }
                }
            }
            return row.id;
        }
        return 0;
    }

    std::vector<int> focusOrder() const {
        std::vector<int> order{kIdSettingsSearch, kIdNav, kIdOpenDocs, kIdOpenFeedback,
                               kIdOpenAbout};
        for (const auto& row : sections_.rows[activePage]) {
            if (row.enabled && row.id == kIdIdleApps) {
                order.push_back(kIdIdleAppNames);
                order.push_back(row.id);
            } else if (row.enabled) {
                order.push_back(row.id);
            }
        }
        return order;
    }

    void focusStep(int direction) {
        advanceContentScroll();
        stopContentScroll();
        const auto order = focusOrder();
        if (order.empty())
            return;
        auto it = std::find(order.begin(), order.end(), focusedId);
        int index = it == order.end() ? (direction > 0 ? -1 : 0)
                                      : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) % order.size();
        focusedId = order[index];
        focusVisible = true;
        // 焦点跟随分组后的真实行位置，避免 Tab 导航停在可视区域之外。
        if (const Row* row = findRow(focusedId == kIdIdleAppNames ? kIdIdleApps : focusedId)) {
            const float viewHeight = contentViewportRect.bottom - contentViewportRect.top;
            if (row->cardRect.top < contentViewportRect.top || row->height > viewHeight)
                setContentScroll(contentScroll + row->cardRect.top - contentViewportRect.top);
            else if (row->cardRect.bottom > contentViewportRect.bottom)
                setContentScroll(contentScroll + row->cardRect.bottom - contentViewportRect.bottom);
        }
        surface.invalidate();
    }

    void updateSliderFromPointer(Row& row, float x) {
        const float left = row.controlRect.left;
        const float right = row.controlRect.right - kSliderValueW;
        const float ratio = std::clamp((x - left) / std::max(1.0f, right - left), 0.0f, 1.0f);
        const int value = row.minValue + static_cast<int>(std::lround(
                                                   ratio * (row.maxValue - row.minValue)));
        if (value == row.value)
            return;
        row.value = std::clamp(value, row.minValue, row.maxValue);
        onCommand(row.id);
        surface.invalidate();
    }

    void updateFontDescription(const std::wstring& description) {
        sections_.state.fontDesc = description;
        if (auto* row = findRow(kIdPickFont)) {
            row->valueText = description;
            row->hint = kFontSettingNotice;
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
            layout();
            surface.invalidate();
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_TIMER:
            if (wp == kSliderCaretTimer) {
                if (slider_.state().open) {
                    slider_.blink();
                    surface.invalidate();
                }
                return 0;
            }
            if (wp == kContentScrollTimer) {
                advanceContentScroll();
                UpdateWindow(hwnd);
                return 0;
            }
            if (wp == kSearchLocateTimer) {
                if (searchLocatedAlpha() <= 0.004f) {
                    stopSearchLocateHighlight();
                    renderContentLayer();
                    return 0;
                }
                renderContentLayer();
                return 0;
            }
            if (wp == kSearchPaletteAnimTimer) {
                advanceSearchPaletteAnimation();
                return 0;
            }
            if (wp == kNavIndicatorTimer) {
                const float scale = surface.dipScale();
                const RECT dirty{
                    static_cast<LONG>(std::floor((navRect.left + 6.0f) * scale)),
                    static_cast<LONG>(std::floor(navRect.top * scale)),
                    static_cast<LONG>(std::ceil((navRect.left + 11.0f) * scale)),
                    static_cast<LONG>(std::ceil(navRect.bottom * scale))};
                surface.invalidate(&dirty);
                UpdateWindow(hwnd);
                return 0;
            }
            break;
        case WM_CREATE:
            search_.resetResults();
            searchLocatedId = 0;
            searchLocatedStartMs = 0;
            searchLocateFading = false;
            searchLocateInitialCursorPt = {LONG_MIN, LONG_MIN};
            backdrop = fluent::styleDialogWindow(hwnd, false);
            surface.initialize(hwnd, backdrop, true);
            if (!createContentWindow())
                return -1;
            createControls();
            layout();
            return 0;
        case WM_MOVE:
            if (search_.isOpen() || search_.animating())
                layoutSearchPalette();
            return 0;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                closeSearchPalette(false, true);
                return 0;
            }
            stopContentScroll();
            layout();
            surface.invalidate();
            return 0;
        case WM_GETMINMAXINFO:
            fluent::setDialogMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp),
                                               kDialogStyle, kDialogExStyle,
                                               kMinClientWidthDip, kMinClientHeightDip);
            return 0;
        case WM_SIZING:
            fluent::enforceDialogMinimumAspectRatio(hwnd, wp, reinterpret_cast<RECT*>(lp),
                                                     kMinClientAspectRatio);
            return TRUE;
        case WM_DPICHANGED: {
            stopContentScroll();
            stopNavIndicator();
            fluent::applyDialogDpiChange(hwnd, wp, reinterpret_cast<RECT*>(lp), kDialogStyle,
                                         kDialogExStyle, kMinClientWidthDip,
                                         kMinClientHeightDip);
            layout();
            surface.invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            stopContentScroll();
            stopNavIndicator();
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop, false);
            surface.setBackdrop(backdrop);
            search_.refreshTheme();
            surface.invalidate();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            const bool navOnly = ps.rcPaint.right <=
                                 static_cast<LONG>(std::floor(navRect.right * surface.dipScale()));
            const bool contentOnly =
                ps.rcPaint.left >= static_cast<LONG>(
                    std::floor(contentViewportRect.left * surface.dipScale())) &&
                ps.rcPaint.top >= static_cast<LONG>(
                    std::floor(contentViewportRect.top * surface.dipScale()));
            const bool scrollBarOnly = contentOnly && contentLayerReady &&
                                      ps.rcPaint.left >= static_cast<LONG>(
                                          std::ceil(contentViewportRect.right * surface.dipScale()));
            surface.paint(hdc, backdrop,
                          [this, navOnly, contentOnly, scrollBarOnly](
                              fluent::FluentDialogSurface::Painter& painter, float w, float h) {
                              paint(painter, w, h, navOnly, contentOnly, scrollBarOnly);
                          });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            surface.eraseBackground(reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_MOUSEMOVE: {
            advanceContentScroll();
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            const float s = surface.dipScale();
            if (slider_.state().open && slider_.state().selecting && GetCapture() == hwnd) {
                placeSliderCaret(GET_X_LPARAM(lp) / s, true);
                return 0;
            }
            if (scrollDragging) {
                scrollFromPointer(GET_Y_LPARAM(lp) / s);
                if (hoverId != kIdContentScrollBar || hoverOption != -1) {
                    hoverId = kIdContentScrollBar;
                    hoverOption = -1;
                    surface.invalidate();
                }
                return 0;
            }
            const float pointerX = GET_X_LPARAM(lp) / s;
            const float pointerY = GET_Y_LPARAM(lp) / s;
            if (idleAppDragging) {
                updateIdleAppDropTarget(pointerX, pointerY);
                hoverId = 0;
                hoverOption = -1;
                surface.invalidate();
                return 0;
            }
            if (pressedId == kIdIdleApps && isIdleAppDragOption(pressedOption)) {
                const float dx = pointerX - idleAppDragStartX;
                const float dy = pointerY - idleAppDragStartY;
                if (dx * dx + dy * dy >= kIdleAppDragThreshold * kIdleAppDragThreshold) {
                    const int index = pressedOption - kIdleAppDragOptionBase;
                    if (index >= 0 &&
                        static_cast<size_t>(index) < sections_.state.idleApps.size()) {
                        idleAppDragging = true;
                        idleAppDragIndex = index;
                        pressedId = 0;
                        pressedOption = -1;
                        updateIdleAppDropTarget(pointerX, pointerY);
                        hoverId = 0;
                        hoverOption = -1;
                        surface.invalidate();
                        return 0;
                    }
                }
            }
            if (pressedId != 0) {
                Row* row = findRow(pressedId);
                if (row && row->kind == ControlKind::Slider && row->enabled) {
                    const float pointerX = GET_X_LPARAM(lp) / s;
                    const float pointerY = GET_Y_LPARAM(lp) / s;
                    if (!contains(sliderValueRect(*row), pointerX, pointerY))
                        updateSliderFromPointer(*row, pointerX);
                    return 0;
                }
            }
            int option = -1;
            const int id = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
            if (searchLocatedId != 0 && !searchLocateFading) {
                const POINT currentCursorPt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (currentCursorPt.x != searchLocateInitialCursorPt.x ||
                    currentCursorPt.y != searchLocateInitialCursorPt.y) {
                    searchLocateInitialCursorPt = {LONG_MIN, LONG_MIN};
                    if (pointerOverLocatedRow(pointerX, pointerY))
                        beginSearchLocateFade();
                }
            }
            if (id != hoverId || option != hoverOption) {
                hoverId = id;
                hoverOption = option;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!scrollDragging) {
                hoverId = 0;
                hoverOption = -1;
            }
            surface.invalidate();
            return 0;
        case WM_LBUTTONDBLCLK: {
            const float s = surface.dipScale();
            const float pointerX = GET_X_LPARAM(lp) / s;
            const float pointerY = GET_Y_LPARAM(lp) / s;
            Row* row = findRow(hitTest(pointerX, pointerY));
            if (row && row->kind == ControlKind::Slider && row->enabled &&
                contains(sliderValueRect(*row), pointerX, pointerY))
                openSliderEditor(*row);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const bool suppressSearchTrigger = search_.consumeTriggerSuppression();
            advanceContentScroll();
            stopContentScroll();
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            resetIdleAppDrag();
            int option = -1;
            const float pointerX = GET_X_LPARAM(lp) / s;
            const float pointerY = GET_Y_LPARAM(lp) / s;
            if (pointerOverLocatedRow(pointerX, pointerY))
                beginSearchLocateFade();
            pressedId = hitTest(pointerX, pointerY, &option);
            pressedOption = option;
            if (pressedId == kIdSettingsSearch && suppressSearchTrigger) {
                pressedId = 0;
                pressedOption = -1;
                surface.invalidate();
                return 0;
            }
            if (pressedId == kIdIdleApps && isIdleAppDragOption(pressedOption)) {
                const int index = pressedOption - kIdleAppDragOptionBase;
                if (index >= 0 && static_cast<size_t>(index) < sections_.state.idleApps.size()) {
                    idleAppDragIndex = index;
                    idleAppDragStartX = pointerX;
                    idleAppDragStartY = pointerY;
                }
            }
            Row* pressedRow = findRow(pressedId);
            const bool numberHit = pressedRow && pressedRow->kind == ControlKind::Slider &&
                pressedRow->enabled && contains(sliderValueRect(*pressedRow), pointerX, pointerY);
            if (slider_.state().open && (!numberHit || pressedId != slider_.state().rowId))
                commitSliderEditor();
            if (numberHit) {
                if (slider_.state().open) {
                    placeSliderCaret(pointerX, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                    slider_.setSelecting(true);
                    SetCapture(hwnd);
                } else {
                    openSliderEditor(*pressedRow);
                }
                pressedId = 0;
                pressedOption = -1;
                return 0;
            }
            if (pressedRow && pressedRow->kind == ControlKind::Slider && pressedRow->enabled)
                updateSliderFromPointer(*pressedRow, pointerX);
            if (pressedId != 0 && pressedId != kIdContentScrollBar)
                focusedId = pressedId == kIdIdleApps && option == kIdleAppNamesOption
                                 ? kIdIdleAppNames
                                 : pressedId;
            if (pressedId == kIdContentScrollBar) {
                const float y = GET_Y_LPARAM(lp) / s;
                scrollDragOffset = contains(scrollThumbRect, GET_X_LPARAM(lp) / s, y)
                                        ? y - scrollThumbRect.top
                                        : (scrollThumbRect.bottom - scrollThumbRect.top) * 0.5f;
                scrollDragging = true;
                scrollFromPointer(y);
            }
            if (pressedId != 0)
                SetCapture(hwnd);
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (slider_.state().selecting) {
                slider_.setSelecting(false);
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                return 0;
            }
            const float s = surface.dipScale();
            if (idleAppDragging) {
                const int fromIndex = idleAppDragIndex;
                const int toIndex = idleAppDropIndex;
                const bool validDrop =
                    fromIndex >= 0 &&
                    static_cast<size_t>(fromIndex) < sections_.state.idleApps.size() &&
                    toIndex >= 0 && toIndex <= static_cast<int>(sections_.state.idleApps.size());
                resetIdleAppDrag();
                pressedId = 0;
                pressedOption = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                if (validDrop && sections_.actions.onReorderIdleApps)
                    sections_.actions.onReorderIdleApps(fromIndex, toIndex);
                surface.invalidate();
                return 0;
            }
            int option = -1;
            const int hit = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
            const int pressed = pressedId;
            const int pressedOptionValue = pressedOption;
            pressedId = 0;
            pressedOption = -1;
            const bool wasScroll = scrollDragging || pressed == kIdContentScrollBar;
            scrollDragging = false;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (wasScroll) {
                surface.invalidate();
                return 0;
            }
            if (pressed == kIdNav && hit == kIdNav && option >= 0)
                showPage(option);
            else if (pressed == kIdSettingsSearch && hit == kIdSettingsSearch)
                openSearchPalette();
            else if (pressed >= kIdOpenDocs && pressed <= kIdOpenAbout && pressed == hit)
                onCommand(pressed);
            else if (pressed != 0 && pressed == hit) {
                Row* row = findRow(pressed);
                if (row && row->enabled) {
                    if (row->kind == ControlKind::Radio || row->kind == ControlKind::ModeGrid) {
                        if (pressedOptionValue >= 0 && pressedOptionValue == option &&
                            pressedOptionValue != row->selected &&
                            isOptionEnabled(*row, static_cast<size_t>(pressedOptionValue))) {
                            row->selected = pressedOptionValue;
                            onCommand(pressed);
                        }
                    } else if (row->kind == ControlKind::AppList) {
                        if (pressedOptionValue == option)
                            onCommand(pressed, pressedOptionValue);
                    } else if (row->kind == ControlKind::Slider) {
                        onCommand(pressed);
                    } else {
                        onCommand(pressed);
                    }
                }
            }
            surface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            slider_.setSelecting(false);
            pressedId = 0;
            pressedOption = -1;
            scrollDragging = false;
            resetIdleAppDrag();
            surface.invalidate();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float s = surface.dipScale();
            if (contains(contentViewportRect, point.x / s, point.y / s) &&
                contentMaxScroll > 0.0f) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                scrollBy(-static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) *
                         kScrollWheelDip, true);
            }
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB;
        case WM_CHAR:
            if (slider_.state().open && wp >= L'0' && wp <= L'9')
                replaceSliderSelection(std::wstring(1, static_cast<wchar_t>(wp)));
            return 0;
        case WM_KEYDOWN:
            if (slider_.state().open && handleSliderEditKey(wp))
                return 0;
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                openSearchPalette();
                return 0;
            }
            if (wp == VK_TAB) {
                focusStep((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (wp == VK_UP || wp == VK_DOWN) {
                if (focusedId == kIdSettingsSearch && wp == VK_DOWN) {
                    focusedId = kIdNav;
                    focusVisible = true;
                    surface.invalidate();
                } else if (focusedId == kIdNav) {
                    int next = activePage + (wp == VK_DOWN ? 1 : -1);
                    if (next < 0)
                        next = kSettingsPageCount - 1;
                    if (next >= kSettingsPageCount)
                        next = 0;
                    showPage(next);
                } else if (Row* row = findRow(focusedId);
                           row && row->kind == ControlKind::ModeGrid && row->enabled &&
                               row->options.size() >= 4) {
                    int next = row->selected;
                    if (wp == VK_UP && row->selected >= 2)
                        next -= 2;
                    else if (wp == VK_DOWN && row->selected < 2)
                        next += 2;
                    if (next != row->selected && next >= 0 && next < 4) {
                        row->selected = next;
                        onCommand(row->id);
                    }
                } else if (Row* row = findRow(focusedId);
                           row && row->id == kIdIdleQuoteBackground && row->enabled &&
                               row->options.size() >= 5) {
                    int next = row->selected;
                    if (wp == VK_DOWN) {
                        if (next == 0)
                            next = 1;
                        else if (next == 1)
                            next = 3;
                        else if (next == 2)
                            next = 4;
                    } else {
                        if (next == 3)
                            next = 1;
                        else if (next == 4)
                            next = 2;
                        else if (next == 1 || next == 2)
                            next = 0;
                    }
                    if (next != row->selected) {
                        row->selected = next;
                        onCommand(row->id);
                    }
                }
                return 0;
            }
            if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_HOME || wp == VK_END) {
                if (wp == VK_PRIOR)
                    scrollBy(-std::max(0.0f, contentViewportRect.bottom -
                                             contentViewportRect.top - kRowGap));
                else if (wp == VK_NEXT)
                    scrollBy(std::max(0.0f, contentViewportRect.bottom -
                                      contentViewportRect.top - kRowGap));
                else if (wp == VK_HOME)
                    setContentScroll(0.0f);
                else
                    setContentScroll(contentMaxScroll);
                return 0;
            }
            if (wp == VK_LEFT || wp == VK_RIGHT) {
                Row* row = findRow(focusedId);
                if (row && row->kind == ControlKind::Slider && row->enabled) {
                    const int direction = wp == VK_LEFT ? -1 : 1;
                    row->value = std::clamp(row->value + direction, row->minValue,
                                            row->maxValue);
                    onCommand(row->id);
                } else if (row && (row->kind == ControlKind::Radio ||
                                   row->kind == ControlKind::ModeGrid) &&
                           row->enabled && !row->options.empty()) {
                    int direction = wp == VK_LEFT ? -1 : 1;
                    int next = row->selected;
                    if (row->kind == ControlKind::ModeGrid && row->options.size() >= 4) {
                        const int column = row->selected % 2;
                        if (direction < 0 && column > 0)
                            --next;
                        else if (direction > 0 && column == 0)
                            ++next;
                    } else {
                        const int optionCount = static_cast<int>(row->options.size());
                        for (int attempt = 0; attempt < optionCount; ++attempt) {
                            next = (next + direction + optionCount) % optionCount;
                            if (isOptionEnabled(*row, static_cast<size_t>(next)))
                                break;
                        }
                    }
                    if (next != row->selected &&
                        isOptionEnabled(*row, static_cast<size_t>(next))) {
                        row->selected = next;
                        onCommand(row->id);
                    }
                }
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                if (focusedId == kIdSettingsSearch) {
                    openSearchPalette();
                } else if (focusedId >= kIdOpenDocs && focusedId <= kIdOpenAbout) {
                    onCommand(focusedId);
                } else if (focusedId == kIdIdleAppNames) {
                    onCommand(kIdIdleAppNames);
                } else if (focusedId != kIdNav) {
                    Row* row = findRow(focusedId);
                    if (row && row->enabled && row->kind != ControlKind::Radio)
                        onCommand(focusedId);
                }
                return 0;
            }
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            commitSliderEditor();
            focusVisible = false;
            surface.invalidate();
            return 0;
        case kMsgColorPickerClosed:
            if (colorPicker && !colorPicker->isOpen())
                colorPicker.reset();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == LBN_SELCHANGE)
                onCommand(LOWORD(wp));
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kSliderCaretTimer);
            stopContentScroll();
            stopNavIndicator();
            stopSearchLocateHighlight();
            KillTimer(hwnd, kSearchPaletteAnimTimer);
            search_.destroy();
            contentRenderer.releaseAll();
            contentTextCache.clear();
            contentLayerReady = false;
            contentWindow = nullptr;
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::Settings), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void destroy() {
        closeSearchPalette(false, true);
        search_.destroy();
        if (slider_.state().open)
            cancelSliderEditor();
        if (colorPicker) {
            colorPicker->destroy();
            colorPicker.reset();
        }
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

SettingsDialog::SettingsDialog() : impl_(std::make_unique<Impl>()) {}

SettingsDialog::~SettingsDialog() {
    if (impl_ && impl_->hwnd)
        impl_->destroy();
}

bool SettingsDialog::create(HINSTANCE inst, HWND parent, const SettingsState& state,
                            SettingsActions actions) {
    impl_->notifyHwnd = parent;
    impl_->inst = inst;
    impl_->sections_.state = state;
    impl_->sections_.actions = std::move(actions);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricSettingsDialog";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(kWindowW * s)),
            static_cast<LONG>(std::lround(kWindowH * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricSettingsDialog", L"设置",
                                  kDialogStyle, x, y, w, h, nullptr, nullptr, inst,
                                  impl_.get());
    if (impl_->hwnd) {
        fluent::ensureDialogMinimumSize(impl_->hwnd, kDialogStyle, kDialogExStyle,
                                         kMinClientWidthDip, kMinClientHeightDip);
        app_icon::applyWindowIcon(impl_->hwnd);
    }
    return impl_->hwnd != nullptr;
}

void SettingsDialog::updateState(const SettingsState& state) {
    if (impl_->hwnd)
        impl_->updateState(state);
}

void SettingsDialog::updateFontDescription(const std::wstring& description) {
    if (impl_->hwnd)
        impl_->updateFontDescription(description);
}

void SettingsDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }
}

void SettingsDialog::destroy() {
    impl_->destroy();
}

bool SettingsDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND SettingsDialog::hwnd() const {
    return impl_->hwnd;
}
