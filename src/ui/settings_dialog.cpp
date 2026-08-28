#include "settings_dialog.h"

#include "ui/app_icon.h"
#include "ui/color_picker_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "ui/media_control_icons.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kSettingsPageCount = 8;
constexpr int kPerformancePage = 1;
constexpr int kFloatingCardPage = 2;
constexpr int kMediaPopupPage = 3;
constexpr int kSpectrumPage = 4;
constexpr int kLyricsPage = 5;
constexpr int kSongToastPage = 6;
constexpr int kIdlePage = kSettingsPageCount - 1;
constexpr int kIdNav = 400;
constexpr int kRenderModeMinimal = 3;
constexpr int kIdSongInfo = 410;
constexpr int kIdAlbumCover = 411;
constexpr int kIdPlatformIcon = 412;
constexpr int kIdCoverEffect = 413;
constexpr int kIdSpectrum = 414;
constexpr int kIdSpectrumStyle = 419;
constexpr int kIdSpectrumOpacity = 423;
constexpr int kIdSpectrumBackground = 428;
constexpr int kIdProgressBackground = 438;
constexpr int kIdProgressBackgroundOpacity = 439;
constexpr int kIdTaskbarBackground = 444;
constexpr int kIdCoverBackgroundOpacity = 445;
constexpr int kIdHoverControls = 415;
constexpr int kIdRenderMode = 416;
constexpr int kIdHoverControlStyle = 417;
constexpr int kIdFloatingCardTrigger = 437;
constexpr int kIdFloatingCardBackground = 418;
constexpr int kIdFloatingCardFollowAlbum = 426;
constexpr int kIdFloatingCardAutoTextContrast = 427;
constexpr int kIdSongToast = 440;
constexpr int kIdSongToastDuration = 441;
constexpr int kIdSongToastSkipFullscreen = 442;
constexpr int kIdSongToastPosition = 443;
constexpr int kIdPickFont = 420;
constexpr int kIdFontColor = 421;
constexpr int kIdFollowAlbum = 422;
constexpr int kIdTaskbarTheme = 424;
constexpr int kIdWindowTheme = 425;
constexpr int kIdDoubleLine = 430;
constexpr int kIdAlignment = 431;
constexpr int kIdSecondaryOn = 432;
constexpr int kIdSecondaryType = 433;
constexpr int kIdQqLocalLyricsEnabled = 434;
constexpr int kIdQqLocalLyricsPath = 435;
constexpr int kIdQqLocalLyricsPersistOrder = 436;
constexpr int kIdIdleEntry = 446;
constexpr int kIdIdleQuoteSource = 447;
constexpr int kIdIdleQuoteRefreshInterval = 448;
constexpr int kIdIdleApps = 449;
constexpr int kIdFloatingCardBackgroundColor = 451;
constexpr int kIdIdleQuoteAlignment = 453;
constexpr int kIdIdleQuote = 454;
constexpr int kIdIdleQuoteBackground = 458;
constexpr int kIdIdleQuoteBackgroundScope = 459;
constexpr int kIdContentScrollBar = 401;
// 应用列表卡片内嵌开关的键盘焦点 ID，不对应独立设置行。
constexpr int kIdIdleAppNames = 460;
constexpr int kIdleAppNamesOption = -2;
constexpr int kIdleAppEditOptionBase = 1000;

constexpr float kWindowW = 760.0f;
constexpr float kWindowH = 552.0f;
constexpr float kMinClientWidthDip = 600.0f;
constexpr float kMinClientHeightDip = 552.0f;
constexpr float kMinClientAspectRatio = kMinClientWidthDip / kMinClientHeightDip;
constexpr float kNavW = 176.0f;
constexpr float kRowH = 56.0f;
constexpr float kRowTallH = 96.0f;
constexpr float kHeaderH = 30.0f;
constexpr float kTitleMinHeight = 22.0f;
constexpr float kTitleTopPadding = 12.0f;
constexpr float kTitleHintGap = 2.0f;
constexpr float kHintBottomPadding = 8.0f;
constexpr float kHintTextSize = 12.0f;
constexpr float kHintMeasureHeight = 10000.0f;
constexpr float kRowGap = 8.0f;
constexpr float kModeGridGap = 8.0f;
constexpr float kModeGridCardH = 56.0f;
constexpr float kModeGridTopGap = 10.0f;
constexpr float kModeGridNoteGap = 8.0f;
constexpr float kModeGridMinH = 196.0f;
constexpr float kSpectrumStyleCardH = 96.0f;
constexpr float kSpectrumStyleCardGap = 8.0f;
constexpr float kSpectrumStyleRowH = 148.0f;
constexpr float kSpectrumBackgroundArtworkMaxW = 144.0f;
constexpr float kSpectrumBackgroundArtworkMinW = 96.0f;
constexpr float kSpectrumBackgroundArtworkH = 34.0f;
constexpr float kSpectrumBackgroundArtworkGap = 12.0f;
constexpr float kCoverEffectCardH = 72.0f;
constexpr float kCoverEffectCardGap = 8.0f;
constexpr float kCoverEffectRowH = 124.0f;
constexpr float kSongToastPositionCardW = 156.0f;
constexpr float kSongToastPositionCardH = 92.0f;
constexpr float kSongToastPositionCardGap = 8.0f;
constexpr float kSongToastPositionRowH = 144.0f;
constexpr float kHoverControlStyleCardH = 156.0f;
constexpr float kHoverControlStyleRowH = 292.0f;
constexpr float kSliderValueW = 44.0f;
constexpr float kSliderValueGap = 10.0f;
constexpr float kScrollBarWidth = 3.0f;
constexpr float kScrollBarHitWidth = 12.0f;
constexpr float kScrollBarInset = 8.0f;
constexpr float kScrollBarOutsideGap = 8.0f;
constexpr float kScrollMinThumbHeight = 24.0f;
constexpr float kScrollWheelDip = 72.0f;
constexpr float kIdleAppItemH = 52.0f;
constexpr float kIdleAppsListTop = 76.0f;
constexpr float kIdleAppsBaseH = 128.0f;
constexpr float kIdleAppActionW = 58.0f;
constexpr float kIdleAppActionGap = 8.0f;
constexpr float kIdleAppInfoRightInset = 144.0f;
constexpr float kIdleAppNamesToggleW = 40.0f;
constexpr float kIdleAppNamesToggleLabelW = 56.0f;
constexpr float kIdleAppNamesToggleGap = 8.0f;
constexpr float kIdleQuoteBackgroundCardH = 112.0f;
constexpr float kIdleQuoteBackgroundCardGap = 8.0f;
constexpr float kIdleQuoteBackgroundRowH = 324.0f;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

float estimateRadioWidth(const std::vector<std::wstring>& options) {
    float w = 0.0f;
    for (const auto& option : options)
        w += 16.0f + 8.0f + static_cast<float>(option.size()) * 14.0f + 20.0f;
    return w > 0.0f ? w - 20.0f : 0.0f;
}

fluent::ThemeMode themeModeFromIndex(int index) {
    switch (index) {
    case 1:
        return fluent::ThemeMode::FollowApp;
    case 2:
        return fluent::ThemeMode::Light;
    case 3:
        return fluent::ThemeMode::Dark;
    case 0:
    default:
        return fluent::ThemeMode::FollowSystem;
    }
}

int themeModeIndex(fluent::ThemeMode mode) {
    switch (mode) {
    case fluent::ThemeMode::FollowApp:
        return 1;
    case fluent::ThemeMode::Light:
        return 2;
    case fluent::ThemeMode::Dark:
        return 3;
    case fluent::ThemeMode::FollowSystem:
    default:
        return 0;
    }
}

std::wstring colorText(COLORREF color) {
    wchar_t text[10]{};
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}

constexpr const wchar_t* kFontSettingNotice =
    L"字体修改不会影响到界面字体，只会影响到任务栏歌词、每日一言、歌曲信息";

} // namespace

struct SettingsDialog::Impl {
    enum class ControlKind {
        Toggle,
        Radio,
        ModeGrid, // 2×2 模式选择卡片
        Slider,
        Button,
        AppList,
        Header, // 分组标题：无卡片背景，不参与交互与焦点
    };

    struct Row {
        int id = 0;
        ControlKind kind = ControlKind::Toggle;
        std::wstring text;
        std::wstring hint;
        std::wstring valueText;
        std::wstring controlText;
        std::vector<std::wstring> options;
        bool showHint = false;
        bool checked = false;
        bool enabled = true;
        int selected = -1;
        int value = 0;
        int minValue = 0;
        int maxValue = 100;
        std::wstring valueSuffix = L"%";
        float controlW = 0.0f;
        float minHeight = kRowH;
        float height = kRowH;
        float titleHeight = kTitleMinHeight;
        float valueHeight = 0.0f;
        D2D1_RECT_F cardRect{};
        D2D1_RECT_F labelRect{};
        D2D1_RECT_F valueRect{};
        D2D1_RECT_F hintRect{};
        D2D1_RECT_F controlRect{};
        D2D1_RECT_F artworkRect{};
        D2D1_RECT_F addAppRect{};
        std::vector<D2D1_RECT_F> appEditRects;
        std::vector<D2D1_RECT_F> appDeleteRects;
        std::vector<D2D1_RECT_F> optionRects;
        std::vector<std::wstring> optionHints;
    };

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr;
    bool backdrop = false;
    SettingsState state;
    SettingsActions actions;
    int activePage = 0;

    fluent::FluentDialogSurface surface;
    std::unique_ptr<ColorPickerDialog> colorPicker;
    std::array<std::wstring, kSettingsPageCount> navItems{
        L"显示", L"性能", L"悬浮卡片", L"悬浮媒体控件", L"频谱", L"歌词", L"切歌弹窗",
        L"每日一言与应用速启"};
    std::array<std::wstring, kSettingsPageCount> pageTitles{
        L"显示", L"性能", L"悬浮卡片", L"悬浮媒体控件", L"频谱", L"歌词", L"切歌弹窗",
        L"每日一言与应用速启"};
    std::vector<Row> rows[kSettingsPageCount];
    D2D1_RECT_F navRect{};
    std::array<D2D1_RECT_F, kSettingsPageCount> navItemRects{};
    std::array<D2D1_RECT_F, kSettingsPageCount> pageTitleRects{};
    D2D1_RECT_F contentViewportRect{};
    D2D1_RECT_F scrollTrackRect{};
    D2D1_RECT_F scrollThumbRect{};
    float contentHeight = 0.0f;
    float contentScroll = 0.0f;
    float contentMaxScroll = 0.0f;
    bool scrollDragging = false;
    float scrollDragOffset = 0.0f;

    int hoverId = 0;
    int hoverOption = -1;
    int pressedId = 0;
    int pressedOption = -1;
    int focusedId = kIdNav;
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

    Row* findRow(int id) {
        for (auto& page : rows) {
            for (auto& row : page) {
                if (row.id == id)
                    return &row;
            }
        }
        return nullptr;
    }

    const Row* findRow(int id) const {
        for (const auto& page : rows) {
            for (const auto& row : page) {
                if (row.id == id)
                    return &row;
            }
        }
        return nullptr;
    }

    bool minimalModeActive() const {
        if (state.renderMode == kRenderModeMinimal)
            return true;
        const auto* mode = findRow(kIdRenderMode);
        return mode && mode->selected == kRenderModeMinimal;
    }

    Row& addRow(int page, int id, ControlKind kind, const wchar_t* text, const wchar_t* hint,
                float controlW, float height) {
        Row row;
        row.id = id;
        row.kind = kind;
        row.text = text ? text : L"";
        row.hint = hint ? hint : L"";
        row.showHint = !row.hint.empty();
        row.controlW = controlW;
        row.minHeight = height;
        row.height = height;
        rows[page].push_back(std::move(row));
        return rows[page].back();
    }

    Row& addToggle(int page, int id, const wchar_t* text, bool checked) {
        Row& row = addRow(page, id, ControlKind::Toggle, text, nullptr, 40.0f, kRowH);
        row.checked = checked;
        return row;
    }

    void updateFloatingCardRowsEnabled() {
        const auto* idleEntry = findRow(kIdIdleEntry);
        const auto* controls = findRow(kIdHoverControls);
        const auto* style = findRow(kIdHoverControlStyle);
        const auto* background = findRow(kIdFloatingCardBackground);
        const bool mediaCardEnabled = controls && controls->checked && style &&
                                      style->selected == 1;
        const bool cardEnabled = !minimalModeActive() &&
                                 ((idleEntry && idleEntry->checked) || mediaCardEnabled);
        const bool frosted = cardEnabled && background && background->selected == 1;
        if (auto* row = findRow(kIdFloatingCardTrigger))
            row->enabled = cardEnabled;
        if (auto* row = findRow(kIdFloatingCardBackground))
            row->enabled = cardEnabled;
        if (auto* row = findRow(kIdFloatingCardBackgroundColor))
            row->enabled = frosted;
        if (auto* row = findRow(kIdFloatingCardFollowAlbum))
            row->enabled = frosted;
        if (auto* row = findRow(kIdFloatingCardAutoTextContrast))
            row->enabled = frosted;
    }

    // 播放进度背景与背景波浪共用同一层背景，二者互斥：
    // 背景波浪生效（频谱开 + 梦幻波浪 + 背景开关开）时进度背景不可用
    bool backgroundWaveActive() const {
        const auto* spectrum = findRow(kIdSpectrum);
        const auto* style = findRow(kIdSpectrumStyle);
        const auto* background = findRow(kIdSpectrumBackground);
        return spectrum && spectrum->checked && style && style->selected == 2 && background &&
               background->checked;
    }

    // 背景波浪行的可用态：频谱开 + 梦幻波浪样式
    bool spectrumBackgroundAvailable() const {
        const auto* spectrum = findRow(kIdSpectrum);
        const auto* style = findRow(kIdSpectrumStyle);
        return spectrum && spectrum->checked && style && style->selected == 2;
    }

    void updateProgressBackgroundRowsEnabled() {
        const bool available = !minimalModeActive() && !backgroundWaveActive();
        if (auto* row = findRow(kIdProgressBackground))
            row->enabled = available;
        if (auto* row = findRow(kIdProgressBackgroundOpacity))
            row->enabled = available && findRow(kIdProgressBackground) &&
                           findRow(kIdProgressBackground)->checked;
    }

    Row& addRadio(int page, int id, const wchar_t* text, const wchar_t* hint,
                  std::vector<std::wstring> options, int selected, bool enabled, float height) {
        Row& row = addRow(page, id, ControlKind::Radio, text, hint,
                          estimateRadioWidth(options), height);
        row.options = std::move(options);
        row.selected = selected;
        row.enabled = enabled;
        return row;
    }

    Row& addModeGrid(int page, int id, const wchar_t* text, const wchar_t* hint,
                     std::vector<std::wstring> options,
                     std::vector<std::wstring> optionHints, int selected, bool enabled,
                     float height) {
        Row& row = addRow(page, id, ControlKind::ModeGrid, text, hint, 0.0f, height);
        row.options = std::move(options);
        row.optionHints = std::move(optionHints);
        row.selected = selected;
        row.enabled = enabled;
        return row;
    }

    Row& addSlider(int page, int id, const wchar_t* text, int value, bool enabled) {
        Row& row = addRow(page, id, ControlKind::Slider, text, nullptr, 216.0f, kRowH);
        row.value = std::clamp(value, 0, 100);
        row.minValue = 0;
        row.maxValue = 100;
        row.enabled = enabled;
        return row;
    }

    Row& addButton(int page, int id, const wchar_t* text, const wchar_t* hint,
                   const wchar_t* controlText, float height = kRowH) {
        Row& row = addRow(page, id, ControlKind::Button, text, hint, 132.0f, height);
        row.controlText = controlText ? controlText : L"";
        return row;
    }

    void updateIdleAppsRowHeight() {
        if (auto* row = findRow(kIdIdleApps)) {
            const size_t count = std::max<size_t>(1, state.idleApps.size());
            row->minHeight = kIdleAppsBaseH + static_cast<float>(count) * kIdleAppItemH;
            row->height = row->minHeight;
        }
    }

    void updateIdleQuoteSourceRow() {
        if (auto* row = findRow(kIdIdleQuoteSource)) {
            const bool privacy = state.idleQuoteSource == 1;
            row->hint = privacy
                            ? L"今日诗词会根据客户端网络 IP、时间等信息进行推荐，并在本地保存接口 Token。"
                            : L"默认使用一言获取每日一言。";
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = kRowTallH;
        }
    }

    void updateIdleRowsEnabled() {
        const bool quoteEnabled = state.idleQuoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteSource))
            row->enabled = quoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
            row->enabled = quoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteAlignment))
            row->enabled = quoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteBackground))
            row->enabled = !minimalModeActive();
        if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
            row->enabled = !minimalModeActive() && state.idleQuoteBackground != 0;
        if (auto* row = findRow(kIdIdleApps))
            row->enabled = !minimalModeActive() && state.idleEntryEnabled;
    }

    void openColorPicker(int id) {
        if (colorPicker && colorPicker->isOpen()) {
            SetForegroundWindow(colorPicker->hwnd());
            return;
        }

        COLORREF initial = 0;
        const wchar_t* title = nullptr;
        if (id == kIdFloatingCardBackgroundColor) {
            initial = state.floatingCardBackgroundColor;
            title = L"悬浮卡片背景颜色";
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
                state.floatingCardBackgroundColor = color;
                if (actions.onFloatingCardBackgroundColor)
                    actions.onFloatingCardBackgroundColor(color);
            }
            if (auto* row = findRow(id))
                row->controlText = colorText(color);
            surface.invalidate();
        });
        colorPicker->show();
    }

    Row& addHeader(int page, const wchar_t* text) {
        Row& row = addRow(page, 0, ControlKind::Header, text, nullptr, 0.0f, kHeaderH);
        row.enabled = false;
        return row;
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
        const float trackLeft = contentViewportRect.right + kScrollBarOutsideGap;
        scrollTrackRect =
            D2D1::RectF(trackLeft, trackTop, trackLeft + kScrollBarWidth, trackBottom);
        scrollThumbRect = D2D1::RectF(scrollTrackRect.left, thumbTop, scrollTrackRect.right,
                                      thumbTop + thumbHeight);
    }

    void setContentScroll(float offset) {
        const float next = std::clamp(offset, 0.0f, contentMaxScroll);
        if (std::fabs(next - contentScroll) < 0.01f)
            return;
        contentScroll = next;
        layout();
        surface.invalidate();
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

    void scrollBy(float delta) {
        if (contentMaxScroll <= 0.0f)
            return;
        setContentScroll(contentScroll + delta);
    }

    void createControls() {
        const bool minimal = state.renderMode == kRenderModeMinimal;
        addHeader(0, L"主题");
        addRadio(0, kIdTaskbarTheme, L"任务栏内容主题",
             L"系统表示跟随Windows系统/全局深浅色，应用表示跟随自定义应用深浅色模式",
                 {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.taskbarThemeMode),
                 true, kRowTallH);
        addRadio(0, kIdWindowTheme, L"对话框与悬浮窗主题",
             L"系统表示跟随Windows系统/全局深浅色，应用表示跟随自定义应用深浅色模式",
                 {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.windowThemeMode),
                 true, kRowTallH);
        addHeader(0, L"字体");
        Row& font = addButton(0, kIdPickFont, L"通用字体", kFontSettingNotice,
                              L"选择字体…", kRowTallH);
        font.valueText = state.fontDesc;
        addHeader(0, L"任务栏歌词");
        addToggle(0, kIdSongInfo, L"显示歌曲信息", state.songInfoVisible);
        addToggle(0, kIdAlbumCover, L"显示专辑封面", state.albumCoverVisible);
        Row& platformIcon = addToggle(0, kIdPlatformIcon, L"显示平台图标",
                                       state.platformIconVisible);
        platformIcon.enabled = state.albumCoverVisible;
        addRadio(0, kIdCoverEffect, L"专辑封面效果", nullptr, {L"默认", L"黑胶唱片"},
                 minimal ? 0 : (state.coverEffectVinyl ? 1 : 0),
                 state.albumCoverVisible && !minimal, kCoverEffectRowH);
        Row& idleEntry = addRow(
            kIdlePage, kIdIdleEntry, ControlKind::Toggle, L"无播放时保留任务栏入口",
            L"播放器未运行时，任务栏显示空闲内容；悬浮后可打开已配置的应用。"
            L"关闭后，播放器未运行时隐藏任务栏入口。",
            40.0f, kRowTallH);
        idleEntry.checked = state.idleEntryEnabled;
        Row& idleQuote = addRow(
            kIdlePage, kIdIdleQuote, ControlKind::Toggle, L"显示每日一言",
            L"关闭后显示按时间和日期类型生成的默认欢迎语；每日一言来源、更新频率和缓存会保留。",
            40.0f, kRowTallH);
        idleQuote.checked = state.idleQuoteEnabled;
        addRadio(kIdlePage, kIdIdleQuoteSource, L"每日一言来源",
                 state.idleQuoteSource == 1
                     ? L"今日诗词会根据客户端网络 IP、时间等信息进行推荐，并在本地保存接口 Token。"
                     : L"默认使用一言获取每日一言。",
                 {L"一言", L"今日诗词"}, state.idleQuoteSource, state.idleQuoteEnabled,
                 kRowTallH);
        addRadio(kIdlePage, kIdIdleQuoteRefreshInterval, L"每日一言更新频率", nullptr,
                 {L"每天", L"每 12 小时", L"每小时"}, state.idleQuoteRefreshInterval,
                 state.idleQuoteEnabled, kRowTallH);
        addRadio(kIdlePage, kIdIdleQuoteAlignment, L"每日一言字体对齐方式",
                 L"仅影响任务栏和悬浮卡片中的每日一言，不影响正常歌词对齐。",
                 {L"左对齐", L"居中", L"右对齐"}, state.idleQuoteAlignment,
                 state.idleQuoteEnabled, kRowH);
        Row& idleApps = addRow(
            kIdlePage, kIdIdleApps, ControlKind::AppList, L"可打开的应用",
            L"通过文件选择器添加本地 EXE，最多添加 20 个应用；开关统一控制名称显示。",
            0.0f, kIdleAppsBaseH + static_cast<float>(
                                            std::max<size_t>(1, state.idleApps.size())) *
                                            kIdleAppItemH);
        idleApps.checked = state.idleAppNamesVisible;
        idleApps.height = idleApps.minHeight;
        idleApps.enabled = state.idleEntryEnabled;
        updateIdleRowsEnabled();
        Row& progressBackground = addRow(
            0, kIdProgressBackground, ControlKind::Toggle, L"播放进度背景",
            L"从窗口左缘到歌词右缘，按播放进度填充专辑主题色；与频谱的背景波浪互斥",
            40.0f, kRowTallH);
        progressBackground.checked = minimal ? false : state.progressBackground;
        addSlider(0, kIdProgressBackgroundOpacity, L"进度背景不透明度",
                  state.progressBackgroundOpacity, !minimal && state.progressBackground);
        addRadio(0, kIdTaskbarBackground, L"任务栏内容背景",
             L"封面模糊将专辑封面高斯模糊后铺满背景；纯色跟随任务栏深浅色；画在最底层，可与播放进度背景、背景波浪叠加",
                 {L"无", L"封面模糊", L"纯色"}, minimal ? 0 : state.taskbarBackground,
                 !minimal, kRowTallH);
        addSlider(0, kIdCoverBackgroundOpacity, L"封面背景不透明度",
                  state.coverBackgroundOpacity,
                  !minimal && state.taskbarBackground == 1);
        addRadio(0, kIdIdleQuoteBackground, L"任务栏内容动态背景",
                 L"选择任务栏动态背景效果；作用范围在下方单独设置，独立频谱容器和悬浮卡片不使用此效果，极简模式下暂时关闭。",
                 {L"无", L"落叶", L"闪烁星星", L"二进制", L"流光粒子"},
                 state.idleQuoteBackground,
                 !minimal,
                 kIdleQuoteBackgroundRowH);
        addModeGrid(0, kIdIdleQuoteBackgroundScope, L"动态背景作用范围",
                    L"选择动态背景显示在每日一言、歌词或两者；独立频谱容器和悬浮卡片不使用。",
                    {L"都不启用", L"仅每日一言启用", L"仅歌词启用", L"都启用"},
                    {L"不显示动态背景", L"只在每日一言时显示", L"只在播放歌词时显示",
                     L"每日一言和歌词都显示"},
                    state.idleQuoteBackgroundScope,
                    !minimal && state.idleQuoteBackground != 0,
                    kModeGridMinH);
        addToggle(kMediaPopupPage, kIdHoverControls, L"悬浮时显示播放控件",
                  state.hoverControls);
        addRadio(kMediaPopupPage, kIdHoverControlStyle, L"悬浮控件样式",
                 L"内嵌控件：在歌词和频谱上悬浮显示上一首、播放和下一首，没有多余信息；媒体卡片额外支持显示歌词进度信息，并且支持点击软件图标或者软件名称快速打开音乐软件",
                 {L"内嵌控件", L"媒体卡片"}, minimal ? 0 : state.hoverControlStyle,
                 state.hoverControls && !minimal,
                 kHoverControlStyleRowH);
        addHeader(kFloatingCardPage, L"行为");
        addRadio(kFloatingCardPage, kIdFloatingCardTrigger, L"悬浮卡片展开方式",
                 L"悬浮展开：鼠标在歌词区域停留片刻后展开；点击展开：点击歌词区域任意位置立即展开。媒体卡片、每日一言和快捷启动卡片共用此设置。",
                 {L"悬浮展开", L"点击展开"}, state.floatingCardTrigger, !minimal,
                 kRowTallH);
        addHeader(kFloatingCardPage, L"背景");
        addRadio(kFloatingCardPage, kIdFloatingCardBackground, L"悬浮卡片背景",
                 L"音乐控件、每日一言和快捷启动卡片共用纯色或 Windows 磨砂玻璃背景。",
                 {L"纯色", L"磨砂玻璃"}, state.floatingCardBackground, !minimal,
                 kRowTallH);
        Row& floatingCardBackgroundColor = addButton(
            kFloatingCardPage, kIdFloatingCardBackgroundColor, L"悬浮卡片背景颜色",
            L"磨砂背景未使用有效专辑色时，媒体卡片、每日一言和快捷启动卡片共用此颜色。",
            colorText(state.floatingCardBackgroundColor).c_str(), kRowTallH);
        floatingCardBackgroundColor.enabled = !minimal && state.floatingCardBackground == 1;
        Row& floatingCardFollowAlbum = addRow(
            kFloatingCardPage, kIdFloatingCardFollowAlbum, ControlKind::Toggle,
            L"磨砂玻璃颜色跟随专辑",
            L"播放音乐且提取到有效专辑色时，磨砂卡片使用专辑色；无播放时使用上面的自定义颜色。",
            40.0f, kRowTallH);
        floatingCardFollowAlbum.checked = state.floatingCardFollowAlbum;
        addHeader(kFloatingCardPage, L"文字");
        Row& floatingCardAutoTextContrast = addRow(
            kFloatingCardPage, kIdFloatingCardAutoTextContrast, ControlKind::Toggle,
            L"磨砂卡片字体颜色动态变化",
            L"根据卡片背后内容的明暗，自动切换黑色或白色文字；媒体卡片、每日一言和快捷启动卡片共用此设置。",
            40.0f, kRowTallH);
        floatingCardAutoTextContrast.checked = state.floatingCardAutoTextContrast;
        updateFloatingCardRowsEnabled();
        Row& songToast = addRow(kSongToastPage, kIdSongToast, ControlKind::Toggle,
                                L"切歌时弹出歌曲信息",
                                L"在主屏幕中下方短暂弹出封面、标题和艺术家；弹窗磨砂半透明，"
                                L"始终不响应鼠标操作",
                                40.0f, kRowTallH);
        songToast.checked = minimal ? false : state.songToastEnabled;
        songToast.enabled = !minimal;
        Row& songToastDuration =
            addSlider(kSongToastPage, kIdSongToastDuration, L"切歌弹窗显示时长",
                      state.songToastDurationSec, !minimal && state.songToastEnabled);
        songToastDuration.minValue = 1;
        songToastDuration.maxValue = 10;
        songToastDuration.valueSuffix = L" 秒";
        Row& songToastSkipFullscreen =
            addToggle(kSongToastPage, kIdSongToastSkipFullscreen, L"全屏应用时关闭弹窗",
                      state.songToastSkipFullscreen);
        songToastSkipFullscreen.enabled = !minimal && state.songToastEnabled;
        addRadio(kSongToastPage, kIdSongToastPosition, L"切歌弹窗位置", nullptr,
                 {L"中上", L"中下"},
                 state.songToastPosition, !minimal && state.songToastEnabled,
                 kSongToastPositionRowH);
        addModeGrid(kPerformancePage, kIdRenderMode, L"性能模式",
                    L"模式仅本次运行有效，重启软件后恢复正常模式",
                    {L"正常", L"低渲染", L"完全停止", L"极简"},
                    {L"完整视觉效果", L"降低帧率，节省 GPU", L"停止渲染，保留监听",
                     L"横向歌词，关闭逐字与转场"},
                    std::clamp(state.renderMode, 0, kRenderModeMinimal), true, kModeGridMinH);

        Row& spectrum = addToggle(kSpectrumPage, kIdSpectrum, L"频谱",
                                  minimal ? false : state.spectrumOn);
        spectrum.enabled = !minimal;
        addRadio(kSpectrumPage, kIdSpectrumStyle, L"频谱样式", nullptr,
                 {L"默认", L"柱状图", L"梦幻波浪"}, state.spectrumStyle,
                 state.spectrumOn && !minimal,
                 kSpectrumStyleRowH);
        Row& spectrumBackground =
            addToggle(kSpectrumPage, kIdSpectrumBackground, L"背景波浪",
                      minimal ? false : state.spectrumBackground);
        spectrumBackground.enabled = state.spectrumOn && state.spectrumStyle == 2 && !minimal;
        addSlider(kSpectrumPage, kIdSpectrumOpacity, L"背景波浪不透明度", state.spectrumOpacity,
                  state.spectrumOn && state.spectrumStyle == 2 && state.spectrumBackground &&
                      !minimal);
        // 频谱行创建完毕后才能按互斥关系刷新进度背景行的可用态
        updateProgressBackgroundRowsEnabled();

        addToggle(kLyricsPage, kIdDoubleLine, L"双行歌词", state.doubleLineLyrics);
        addRadio(kLyricsPage, kIdAlignment, L"歌词对齐", nullptr,
                 {L"左对齐", L"居中", L"右对齐"},
                 state.lyricAlignment, true, kRowH);
        addButton(kLyricsPage, kIdFontColor, L"歌词字体颜色与效果", nullptr, L"打开…");
        addToggle(kLyricsPage, kIdFollowAlbum, L"歌词已播放颜色跟随专辑", state.followAlbum);
        addToggle(kLyricsPage, kIdSecondaryOn, L"开启翻译/罗马音", state.secondaryEnabled);
        const wchar_t* secondaryHint = state.secondaryAvailability == 1
                                            ? L"正在检查翻译和罗马音…"
                                            : state.secondaryAvailability == 2
                                                  ? L"当前歌曲无翻译或罗马音"
                                                  : L"";
        addRadio(kLyricsPage, kIdSecondaryType, L"辅助歌词类型", secondaryHint,
                 {L"翻译", L"罗马音"}, state.preferRomanization ? 1 : 0,
                 state.secondaryEnabled && state.secondaryAvailability == 0,
                 *secondaryHint ? kRowTallH : kRowH);
        addToggle(kLyricsPage, kIdQqLocalLyricsEnabled, L"使用 QQ 音乐本地歌词",
                  state.qqLocalLyricsEnabled);
        Row& persistOrder = addRow(kLyricsPage, kIdQqLocalLyricsPersistOrder,
                                   ControlKind::Toggle,
                                   L"切换版本持久化",
                                   L"记住每首歌切换后的本地/在线版本；关闭后不保存新记录，但仍读取已有记录",
                                   40.0f, kRowTallH);
        persistOrder.checked = state.qqLocalLyricsPersistOrder;
        persistOrder.enabled = state.qqLocalLyricsEnabled;
        const std::wstring localPathHint = state.qqLocalLyricsPath.empty()
                                                ? std::wstring(L"未配置")
                                                : state.qqLocalLyricsPath;
        Row& localPath = addButton(kLyricsPage, kIdQqLocalLyricsPath, L"QQ音乐本地歌词目录",
                                   localPathHint.c_str(), L"选择文件夹…", kRowTallH);
        localPath.enabled = state.qqLocalLyricsEnabled;
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

    void updateHintHeights(fluent::FluentDialogSurface::Painter& painter) {
        bool changed = false;
        for (auto& row : rows[activePage]) {
            if (row.kind == ControlKind::Header)
                continue;
            const float labelW = row.labelRect.right - row.labelRect.left;
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
            if (row.id == kIdPickFont) {
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
                    if (row.id == kIdIdleQuoteBackground) {
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
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);

        navRect = D2D1::RectF(12.0f, 12.0f, 12.0f + kNavW, std::max(12.0f, h - 12.0f));
        for (int i = 0; i < kSettingsPageCount; ++i) {
            navItemRects[i] = D2D1::RectF(navRect.left, navRect.top + i * 32.0f,
                                          navRect.right, navRect.top + (i + 1) * 32.0f);
            pageTitleRects[i] = D2D1::RectF(0, 0, 0, 0);
        }

        const float contentX = 12.0f + kNavW + 16.0f;
        const float contentW = std::max(20.0f, w - contentX - 24.0f);
        pageTitleRects[activePage] =
            D2D1::RectF(contentX, 14.0f, contentX + contentW, 42.0f);
        const float contentTop = 14.0f + 28.0f + 16.0f;
        const float contentBottom = std::max(contentTop, h - 12.0f);
        contentViewportRect =
            D2D1::RectF(contentX, contentTop, contentX + contentW, contentBottom);

        float contentEnd = contentTop;
        for (const auto& row : rows[activePage])
            contentEnd += row.height + kRowGap;
        if (!rows[activePage].empty())
            contentEnd -= kRowGap;
        contentHeight = std::max(0.0f, contentEnd - contentTop);
        const float viewportHeight =
            std::max(0.0f, contentViewportRect.bottom - contentViewportRect.top);
        contentMaxScroll = std::max(0.0f, contentHeight - viewportHeight);
        contentScroll = std::clamp(contentScroll, 0.0f, contentMaxScroll);
        updateScrollBarGeometry();

        float y = contentTop - contentScroll;
        for (auto& page : rows) {
            for (auto& row : page) {
                row.cardRect = D2D1::RectF(0, 0, 0, 0);
                row.labelRect = D2D1::RectF(0, 0, 0, 0);
                row.valueRect = D2D1::RectF(0, 0, 0, 0);
                row.hintRect = D2D1::RectF(0, 0, 0, 0);
                row.controlRect = D2D1::RectF(0, 0, 0, 0);
                row.artworkRect = D2D1::RectF(0, 0, 0, 0);
                row.addAppRect = D2D1::RectF(0, 0, 0, 0);
                row.appEditRects.clear();
                row.appDeleteRects.clear();
                row.optionRects.clear();
            }
        }

        for (auto& row : rows[activePage]) {
            const float rowH = row.height;
            if (row.kind == ControlKind::Header) {
                // 分组标题无卡片，文字贴内容区左缘、垂直居中
                row.labelRect = D2D1::RectF(contentX + 4.0f, y, contentX + contentW,
                                            y + rowH);
                y += rowH + kRowGap;
                continue;
            }
            row.cardRect = D2D1::RectF(contentX, y, contentX + contentW, y + rowH);
            const float innerX = contentX + 16.0f;
            const float innerRight = contentX + contentW - 16.0f;
            if (row.id == kIdIdleApps) {
                const float titleTop = y + kTitleTopPadding;
                const float toggleLeft = innerRight - kIdleAppNamesToggleW;
                const float toggleLabelRight = toggleLeft - kIdleAppNamesToggleGap;
                const float toggleLabelLeft = toggleLabelRight - kIdleAppNamesToggleLabelW;
                row.labelRect = D2D1::RectF(innerX, titleTop,
                                            std::max(innerX, toggleLabelLeft - kIdleAppNamesToggleGap),
                                            titleTop + row.titleHeight);
                row.controlRect = D2D1::RectF(toggleLeft, titleTop, innerRight,
                                              titleTop + row.titleHeight);
                const float hintTop = titleTop + row.titleHeight + kTitleHintGap;
                row.hintRect = D2D1::RectF(innerX, hintTop, innerRight,
                                           hintTop + 30.0f);
                const float listTop = y + kIdleAppsListTop;
                const size_t count = state.idleApps.size();
                row.appEditRects.reserve(count);
                row.appDeleteRects.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    const float itemTop = listTop + static_cast<float>(i) * kIdleAppItemH;
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
                y += rowH + kRowGap;
                continue;
            }
            if (row.id == kIdCoverEffect) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kCoverEffectCardH);
                if (row.showHint) {
                    const float hintTop = row.controlRect.bottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(innerX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + kRowGap;
                continue;
            }
            if (row.id == kIdIdleQuoteBackground) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                const float gridH = kIdleQuoteBackgroundCardH * 2.0f +
                                    kIdleQuoteBackgroundCardGap;
                row.controlRect = D2D1::RectF(innerX, gridTop, innerRight, gridTop + gridH);
                if (row.showHint) {
                    const float hintTop = row.controlRect.bottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(innerX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + kRowGap;
                continue;
            }
            if (row.id == kIdHoverControlStyle) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kHoverControlStyleCardH);
                if (row.showHint) {
                    const float hintTop = row.controlRect.bottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(innerX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + kRowGap;
                continue;
            }
            if (row.id == kIdSongToastPosition) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kSongToastPositionCardH);
                y += rowH + kRowGap;
                continue;
            }
            if (row.id == kIdSpectrumStyle) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                row.controlRect = D2D1::RectF(
                    innerX, gridTop, innerRight, gridTop + kSpectrumStyleCardH);
                y += rowH + kRowGap;
                continue;
            }
            if (row.id == kIdSpectrumBackground) {
                const float controlX = innerRight - row.controlW;
                const float controlH = 24.0f;
                const float availableW = std::max(0.0f, controlX - innerX);
                const float artworkW = std::min(
                    kSpectrumBackgroundArtworkMaxW,
                    std::max(kSpectrumBackgroundArtworkMinW, availableW * 0.42f));
                const float artworkRight = controlX - kSpectrumBackgroundArtworkGap;
                const float artworkLeft = std::max(innerX, artworkRight - artworkW);
                const float labelRight = std::max(innerX + 20.0f,
                                                  artworkLeft - kSpectrumBackgroundArtworkGap);
                const float titleTop = y + (rowH - row.titleHeight) * 0.5f;
                row.labelRect = D2D1::RectF(innerX, titleTop, labelRight,
                                            titleTop + row.titleHeight);
                const float artworkTop = y + (rowH - kSpectrumBackgroundArtworkH) * 0.5f;
                row.artworkRect = D2D1::RectF(artworkLeft, artworkTop, artworkRight,
                                              artworkTop + kSpectrumBackgroundArtworkH);
                const float controlY = y + (rowH - controlH) * 0.5f;
                row.controlRect = D2D1::RectF(controlX, controlY,
                                              controlX + row.controlW, controlY + controlH);
                y += rowH + kRowGap;
                continue;
            }
            if (row.kind == ControlKind::ModeGrid) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerRight,
                                            titleTop + row.titleHeight);

                const float gridTop = titleTop + row.titleHeight + kModeGridTopGap;
                const float gridH = kModeGridCardH * 2.0f + kModeGridGap;
                const float gridBottom = gridTop + gridH;
                row.controlRect = D2D1::RectF(innerX, gridTop, innerRight, gridBottom);

                if (row.showHint) {
                    const float hintTop = gridBottom + kModeGridNoteGap;
                    row.hintRect = D2D1::RectF(innerX, hintTop, innerRight,
                                              y + rowH - kHintBottomPadding);
                }
                y += rowH + kRowGap;
                continue;
            }

            const float controlX = innerRight - row.controlW;
            const float labelW = std::max(20.0f, controlX - innerX - 12.0f);
            const float controlH = row.kind == ControlKind::Button
                                       ? fluent::metrics::controlHeight
                                       : 24.0f;
            if (row.id == kIdPickFont) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerX + labelW,
                                            titleTop + row.titleHeight);
                const float valueTop = titleTop + row.titleHeight + kTitleHintGap;
                row.valueRect = D2D1::RectF(innerX, valueTop, innerX + labelW,
                                            valueTop + row.valueHeight);
                const float hintTop = valueTop + row.valueHeight +
                                      (row.valueHeight > 0.0f ? kTitleHintGap : 0.0f);
                row.hintRect = D2D1::RectF(innerX, hintTop, innerX + labelW,
                                           y + rowH - kHintBottomPadding);
            } else if (row.showHint) {
                const float titleTop = y + kTitleTopPadding;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerX + labelW,
                                            titleTop + row.titleHeight);
                // 提示区域从标题实际高度之后开始，避免窄窗口下标题换行后侵入提示文字。
                const float hintTop = titleTop + row.titleHeight + kTitleHintGap;
                row.hintRect = D2D1::RectF(innerX, hintTop, innerX + labelW,
                                           y + rowH - kHintBottomPadding);
            } else {
                const float titleTop = y + (rowH - row.titleHeight) / 2.0f;
                row.labelRect = D2D1::RectF(innerX, titleTop, innerX + labelW,
                                            titleTop + row.titleHeight);
            }
            const float controlY = y + (rowH - controlH) / 2.0f;
            row.controlRect = D2D1::RectF(controlX, controlY,
                                          controlX + row.controlW,
                                          controlY + controlH);
            y += rowH + kRowGap;
        }
    }

    void showPage(int page) {
        if (page < 0 || page >= kSettingsPageCount || page == activePage)
            return;
        activePage = page;
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
        const auto& p = fluent::palette();
        painter.fillRoundRect(p.cardFill, navRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, navRect, 1.0f, fluent::metrics::cardRadius);
        if (focusedId == kIdNav && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(navRect.left + 1.5f, navRect.top + 1.5f, navRect.right - 1.5f,
                            navRect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }
        auto* format = painter.textFormat(13.0f, 400, false, true);
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
            if (selected)
                painter.fillRoundRect(
                    p.accent,
                    D2D1::RectF(navItemRects[i].left + 7.0f,
                                (navItemRects[i].top + navItemRects[i].bottom) / 2.0f - 8.0f,
                                navItemRects[i].left + 10.0f,
                                (navItemRects[i].top + navItemRects[i].bottom) / 2.0f + 8.0f),
                    1.5f);
            painter.drawText(navItems[i], format,
                             D2D1::RectF(navItemRects[i].left + 16.0f, navItemRects[i].top,
                                         navItemRects[i].right - 12.0f, navItemRects[i].bottom),
                             p.text);
        }
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == row.id;
        const bool pressed = pressedId == row.id;
        D2D1_COLOR_F fill = pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill;
        D2D1_COLOR_F textColor = row.enabled ? p.text : p.disabled;
        if (!row.enabled)
            fill = p.listHover;
        painter.fillRoundRect(fill, row.controlRect);
        painter.strokeRoundRect(p.cardStroke, row.controlRect);
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
            painter.fillRoundRect(fluent::toD2D(state.floatingCardBackgroundColor), previewRect, 4.0f);
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
        const auto& p = fluent::palette();
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
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
        } else if (row.checked) {
            painter.fillRoundRect(hovered ? p.accentHover : p.accent, track, radius);
        } else {
            painter.fillRoundRect(hovered ? p.controlHover : p.controlFill, track, radius);
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
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
        const auto& p = fluent::palette();
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
        painter.drawText(std::to_wstring(row.value) + row.valueSuffix,
                         painter.textFormat(12.0f, 400, false, true),
                         D2D1::RectF(trackRight + kSliderValueGap, row.controlRect.top,
                                     row.controlRect.right, row.controlRect.bottom),
                         row.enabled ? p.textSecondary : p.disabled);
        if (focusedId == row.id && focusVisible && row.enabled)
            painter.strokeRoundRect(p.accent,
                                    D2D1::RectF(track.left - 3.0f, track.top - 5.0f,
                                                track.right + 3.0f, track.bottom + 5.0f),
                                    1.0f, 5.0f);
    }

    void drawSpectrumStyleArtwork(fluent::FluentDialogSurface::Painter& painter,
                                  const D2D1_RECT_F& bounds, int style, bool enabled) {
        const auto& p = fluent::palette();
        const auto tone = [enabled](D2D1_COLOR_F color) {
            if (!enabled)
                color.a *= 0.42f;
            return color;
        };
        const auto withOpacity = [](D2D1_COLOR_F color, float opacity) {
            color.a *= opacity;
            return color;
        };
        const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from,
                                  D2D1_POINT_2F to, float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->DrawLine(from, to, brush, stroke);
        };

        const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                             bounds.right - 1.0f, bounds.bottom - 1.0f);
        painter.fillRoundRect(tone(p.controlFill), panel, 7.0f);
        painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 7.0f);

        const float left = panel.left + 8.0f;
        const float right = panel.right - 8.0f;
        const float top = panel.top + 8.0f;
        const float bottom = panel.bottom - 8.0f;
        const float width = std::max(0.0f, right - left);
        const float height = std::max(0.0f, bottom - top);
        if (width <= 0.0f || height <= 0.0f)
            return;

        if (style == 2) {
            // 梦幻波浪：三层半透明波面向同一底线收束，避免画成几条装饰线。
            if (!painter.target())
                return;
            ID2D1Factory* factory = nullptr;
            painter.target()->GetFactory(&factory);
            if (!factory)
                return;

            const float baseY = bottom - 2.0f;
            constexpr int kSamples = 24;
            constexpr float kTwoPi = 6.28318530718f;
            const auto waveY = [&](float x, float amplitude, float offset, float phase) {
                const float t = (x - left) / width;
                const float profile = 0.18f +
                                      0.82f *
                                          (0.5f + 0.5f *
                                                       std::sin(kTwoPi * (t * 1.12f) + phase));
                return baseY - offset - amplitude * profile;
            };
            const auto drawFilledWave = [&](float amplitude, float offset, float phase,
                                            float opacity) {
                ID2D1PathGeometry* geometry = nullptr;
                if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
                    return;
                ID2D1GeometrySink* sink = nullptr;
                if (FAILED(geometry->Open(&sink)) || !sink) {
                    geometry->Release();
                    return;
                }

                sink->BeginFigure(D2D1::Point2F(left, waveY(left, amplitude, offset, phase)),
                                  D2D1_FIGURE_BEGIN_FILLED);
                for (int i = 1; i <= kSamples; ++i) {
                    const float t = static_cast<float>(i) / kSamples;
                    const float x = left + width * t;
                    sink->AddLine(D2D1::Point2F(x, waveY(x, amplitude, offset, phase)));
                }
                sink->AddLine(D2D1::Point2F(right, baseY));
                sink->AddLine(D2D1::Point2F(left, baseY));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                const HRESULT closeHr = sink->Close();
                sink->Release();
                if (SUCCEEDED(closeHr)) {
                    if (auto* brush = painter.brush(tone(withOpacity(p.accent, opacity))))
                        painter.target()->FillGeometry(geometry, brush);
                }
                geometry->Release();
            };

            drawFilledWave(height * 0.25f, height * 0.08f, 1.05f, 0.24f);
            drawFilledWave(height * 0.34f, height * 0.04f, 0.35f, 0.36f);
            drawFilledWave(height * 0.44f, 0.0f, -0.35f, 0.58f);
            drawLine(p.separator, D2D1::Point2F(left, baseY),
                     D2D1::Point2F(right, baseY), 0.8f);

            D2D1_POINT_2F previous =
                D2D1::Point2F(left, waveY(left, height * 0.44f, 0.0f, -0.35f));
            for (int i = 1; i <= kSamples; ++i) {
                const float t = static_cast<float>(i) / kSamples;
                const float x = left + width * t;
                const D2D1_POINT_2F current =
                    D2D1::Point2F(x, waveY(x, height * 0.44f, 0.0f, -0.35f));
                drawLine(withOpacity(p.accent, 0.62f), previous, current, 1.0f);
                previous = current;
            }
            factory->Release();
            return;
        }

        // 预览按实际频谱的 12 个频段绘制，并保持任务栏中 5:3 的柱宽/间隙比例。
        // 这样缩小到设置卡片后仍是窄柱，不会因为用少量柱体铺满宽度而显得粗重。
        constexpr int kBars = 12;
        constexpr float kLevels[kBars] = {
            0.26f, 0.42f, 0.60f, 0.82f, 0.44f, 0.70f,
            0.52f, 0.90f, 0.66f, 0.86f, 0.48f, 0.30f};
        constexpr float kBarUnitDenominator =
            kBars * 5.0f + (kBars - 1) * 3.0f;
        const float barUnit = width / kBarUnitDenominator;
        const float barW = barUnit * 5.0f;
        const float gap = barUnit * 3.0f;
        if (barW <= 0.0f)
            return;

        if (style == 1) {
            // 柱状图：柱底统一贴近底线，电平只向上增长。
            const float baseY = bottom - 2.0f;
            drawLine(p.separator, D2D1::Point2F(left, baseY),
                     D2D1::Point2F(right, baseY), 0.8f);
            for (int i = 0; i < kBars; ++i) {
                const float barH = std::max(3.0f, height * (0.16f + kLevels[i] * 0.72f));
                const float x = left + i * (barW + gap);
                painter.fillRoundRect(
                    tone(p.accent), D2D1::RectF(x, baseY - barH, x + barW, baseY),
                    barW * 0.5f);
            }
            return;
        }

        // 默认：柱体以中线为基准上下对称，保留原样式的视觉特征。
        const float centerY = (top + bottom) * 0.5f;
        for (int i = 0; i < kBars; ++i) {
            const float barH = std::max(3.0f, height * (0.16f + kLevels[i] * 0.68f));
            const float x = left + i * (barW + gap);
            painter.fillRoundRect(
                tone(p.accent), D2D1::RectF(x, centerY - barH * 0.5f, x + barW,
                                             centerY + barH * 0.5f),
                barW * 0.5f);
        }
    }

    void drawSpectrumBackgroundArtwork(fluent::FluentDialogSurface::Painter& painter,
                                       const D2D1_RECT_F& bounds, bool enabled) {
        const auto& p = fluent::palette();
        const auto tone = [enabled](D2D1_COLOR_F color) {
            if (!enabled)
                color.a *= 0.42f;
            return color;
        };
        const auto withOpacity = [](D2D1_COLOR_F color, float opacity) {
            color.a *= opacity;
            return color;
        };
        const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from,
                                  D2D1_POINT_2F to, float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->DrawLine(from, to, brush, stroke);
        };

        const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                             bounds.right - 1.0f, bounds.bottom - 1.0f);
        painter.fillRoundRect(tone(p.controlFill), panel, 7.0f);
        painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 7.0f);

        const float left = panel.left + 7.0f;
        const float right = panel.right - 7.0f;
        const float top = panel.top + 5.0f;
        const float bottom = panel.bottom - 5.0f;
        const float width = std::max(0.0f, right - left);
        const float height = std::max(0.0f, bottom - top);
        if (width <= 0.0f || height <= 0.0f || !painter.target())
            return;

        // 背景波浪：波面先铺在歌词横线下面，明确表达“歌词在波浪前景之上”。
        ID2D1Factory* factory = nullptr;
        painter.target()->GetFactory(&factory);
        if (!factory)
            return;

        const float baseY = bottom;
        constexpr int kSamples = 24;
        constexpr float kTwoPi = 6.28318530718f;
        const auto waveY = [&](float x, float amplitude, float offset, float phase) {
            const float t = (x - left) / width;
            const float profile = 0.18f +
                                  0.82f *
                                      (0.5f + 0.5f *
                                                   std::sin(kTwoPi * (t * 1.05f) + phase));
            return baseY - offset - amplitude * profile;
        };
        const auto drawFilledWave = [&](float amplitude, float offset, float phase,
                                        float opacity) {
            ID2D1PathGeometry* geometry = nullptr;
            if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
                return;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED(geometry->Open(&sink)) || !sink) {
                geometry->Release();
                return;
            }

            sink->BeginFigure(D2D1::Point2F(left, waveY(left, amplitude, offset, phase)),
                              D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i <= kSamples; ++i) {
                const float t = static_cast<float>(i) / kSamples;
                const float x = left + width * t;
                sink->AddLine(D2D1::Point2F(x, waveY(x, amplitude, offset, phase)));
            }
            sink->AddLine(D2D1::Point2F(right, baseY));
            sink->AddLine(D2D1::Point2F(left, baseY));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            const HRESULT closeHr = sink->Close();
            sink->Release();
            if (SUCCEEDED(closeHr)) {
                if (auto* brush = painter.brush(tone(withOpacity(p.accent, opacity))))
                    painter.target()->FillGeometry(geometry, brush);
            }
            geometry->Release();
        };

        drawFilledWave(height * 0.54f, 0.0f, -0.35f, 0.24f);
        drawFilledWave(height * 0.38f, height * 0.10f, 0.55f, 0.16f);

        // 前景歌词：横线覆盖在波面上，和“梦幻波浪”独立展示波形的预览区分开。
        drawLine(withOpacity(p.text, 0.78f),
                 D2D1::Point2F(left + 12.0f, top + height * 0.36f),
                 D2D1::Point2F(right - 12.0f, top + height * 0.36f), 1.4f);
        drawLine(withOpacity(p.textSecondary, 0.72f),
                 D2D1::Point2F(left + 12.0f, top + height * 0.66f),
                 D2D1::Point2F(left + width * 0.62f, top + height * 0.66f), 1.1f);
        factory->Release();
    }

    void drawSpectrumStyleRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
        auto* titleFormat = painter.textFormat(13.0f, 500, true, true);
        if (!titleFormat)
            return;

        const float gridW = row.controlRect.right - row.controlRect.left;
        row.optionRects.clear();
        const size_t count = std::min<size_t>(row.options.size(), 3);
        if (count == 0)
            return;
        const float cardW = (gridW - kSpectrumStyleCardGap * static_cast<float>(count - 1)) /
                            static_cast<float>(count);
        for (size_t i = 0; i < count; ++i) {
            const float left = row.controlRect.left + static_cast<float>(i) *
                               (cardW + kSpectrumStyleCardGap);
            const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                                 row.controlRect.bottom);
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

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 8.0f, card.top + 8.0f, card.right - 24.0f,
                            card.top + 60.0f);
            drawSpectrumStyleArtwork(painter, artwork, static_cast<int>(i), row.enabled);

            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 6.0f, card.bottom - 25.0f,
                                         card.right - 6.0f, card.bottom - 6.0f),
                             row.enabled ? p.text : p.disabled);

            // 示意图和文字先画，单选框最后画，避免缩放时被内容盖住。
            const D2D1_POINT_2F radioCenter =
                D2D1::Point2F(card.right - 14.0f, card.top + 14.0f);
            if (!row.enabled) {
                if (auto* brush = painter.brush(p.disabled))
                    painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush, 1.0f);
                if (selected) {
                    if (auto* brush = painter.brush(p.disabled))
                        painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                      brush);
                }
            } else if (selected) {
                if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush);
                if (auto* brush = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                  brush);
            } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
                painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                              pressed ? 1.5f : 1.0f);
            }
        }
    }

    void drawCoverEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                const D2D1_RECT_F& bounds, bool vinyl, bool enabled) {
        const auto tone = [enabled](D2D1_COLOR_F color) {
            if (!enabled)
                color.a *= 0.42f;
            return color;
        };
        const auto fillEllipse = [&](D2D1_COLOR_F color, D2D1_POINT_2F center, float radius) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->FillEllipse(D2D1::Ellipse(center, radius, radius), brush);
        };
        const auto drawEllipse = [&](D2D1_COLOR_F color, D2D1_POINT_2F center, float radius,
                                     float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush,
                                              stroke);
        };
        const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                                  float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->DrawLine(from, to, brush, stroke);
        };

        const float left = bounds.left;
        const float top = bounds.top;
        const float right = bounds.right;
        const float bottom = bounds.bottom;
        const D2D1_COLOR_F clay = D2D1::ColorF(0.73f, 0.39f, 0.31f);
        const D2D1_COLOR_F paper = D2D1::ColorF(0.95f, 0.84f, 0.68f);
        const D2D1_COLOR_F ink = D2D1::ColorF(0.13f, 0.14f, 0.16f);
        const D2D1_COLOR_F vinylColor = D2D1::ColorF(0.10f, 0.11f, 0.13f);
        const D2D1_COLOR_F vinylEdge = D2D1::ColorF(0.25f, 0.26f, 0.28f);

        if (!vinyl) {
            // 默认模式：歌曲封面会占满这块圆角方形，不再画成“封面套封面”。
            const D2D1_RECT_F cover = D2D1::RectF(left + 2.0f, top + 2.0f, right - 2.0f,
                                                  bottom - 2.0f);
            const D2D1_RECT_F artwork = D2D1::RectF(left + 5.0f, top + 5.0f, right - 5.0f,
                                                   bottom - 5.0f);
            painter.fillRoundRect(tone(clay), cover, 6.0f);
            painter.fillRoundRect(tone(paper), artwork, 4.0f);
            drawLine(ink, D2D1::Point2F(artwork.left + 3.0f, artwork.top + 8.0f),
                     D2D1::Point2F(artwork.right - 4.0f, artwork.top + 8.0f), 1.5f);
            drawLine(ink, D2D1::Point2F(artwork.left + 3.0f, artwork.top + 12.0f),
                     D2D1::Point2F(artwork.left + 14.0f, artwork.top + 12.0f), 1.5f);
            drawLine(clay, D2D1::Point2F(artwork.left + 4.0f, artwork.bottom - 6.0f),
                     D2D1::Point2F(artwork.right - 4.0f, artwork.top + 16.0f), 1.8f);
            return;
        }

        // 黑胶模式：唱片为外层，歌曲封面实际位于中央圆形裁剪区。
        const float cx = left + (right - left) * 0.54f;
        const float cy = top + (bottom - top) * 0.52f;
        const float radius = std::min(right - left, bottom - top) * 0.42f;
        const D2D1_POINT_2F center = D2D1::Point2F(cx, cy);
        fillEllipse(vinylColor, center, radius);
        drawEllipse(vinylEdge, center, radius - 3.0f, 1.0f);
        drawEllipse(vinylEdge, center, radius * 0.68f, 0.8f);
        drawLine(paper, D2D1::Point2F(cx - radius * 0.14f, cy - radius * 0.82f),
                 D2D1::Point2F(cx + radius * 0.36f, cy - radius * 0.66f), 1.0f);

        // 真实渲染中的圆形封面半径约为封面槽边长的 30%，这里用同样比例表现封面落点。
        const float coverRadius = std::min(right - left, bottom - top) * 0.30f;
        const D2D1_POINT_2F coverCenter = D2D1::Point2F(cx, cy);
        fillEllipse(paper, coverCenter, coverRadius);
        fillEllipse(clay,
                    D2D1::Point2F(cx - coverRadius * 0.18f, cy + coverRadius * 0.16f),
                    coverRadius * 0.44f);
        drawLine(ink, D2D1::Point2F(cx - coverRadius * 0.55f, cy - coverRadius * 0.12f),
                 D2D1::Point2F(cx + coverRadius * 0.50f, cy - coverRadius * 0.12f), 1.1f);
        drawLine(clay, D2D1::Point2F(cx - coverRadius * 0.52f, cy + coverRadius * 0.48f),
                 D2D1::Point2F(cx + coverRadius * 0.48f, cy - coverRadius * 0.34f), 1.4f);
        drawEllipse(vinylEdge, coverCenter, coverRadius, 0.9f);
        fillEllipse(ink, center, 1.7f);
    }

    void drawCoverEffectRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
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

            const D2D1_POINT_2F radioCenter =
                D2D1::Point2F(card.right - 16.0f, card.top + 16.0f);
            if (!row.enabled) {
                if (auto* brush = painter.brush(p.disabled))
                    painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush, 1.0f);
                if (selected) {
                    if (auto* brush = painter.brush(p.disabled))
                        painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                      brush);
                }
            } else if (selected) {
                if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush);
                if (auto* brush = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                  brush);
            } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
                painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                              pressed ? 1.5f : 1.0f);
            }

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 12.0f, card.top + 14.0f, card.left + 56.0f,
                            card.top + 58.0f);
            drawCoverEffectArtwork(painter, artwork, i == 1, row.enabled);
            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 68.0f, card.top + 20.0f,
                                         card.right - 28.0f, card.bottom - 14.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawSongToastPositionArtwork(fluent::FluentDialogSurface::Painter& painter,
                                      const D2D1_RECT_F& bounds, bool top, bool enabled) {
        const auto& p = fluent::palette();
        const auto tone = [enabled](D2D1_COLOR_F color) {
            if (!enabled)
                color.a *= 0.42f;
            return color;
        };
        const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from,
                                  D2D1_POINT_2F to, float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->DrawLine(from, to, brush, stroke);
        };

        // 用一个简化的屏幕框表示位置参照，弹窗本身保留封面和文字两块信息。
        const D2D1_RECT_F screen = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                               bounds.right - 1.0f, bounds.bottom - 1.0f);
        painter.fillRoundRect(tone(p.controlFill), screen, 7.0f);
        painter.strokeRoundRect(tone(p.cardStroke), screen, 1.0f, 7.0f);

        const float centerY = (screen.top + screen.bottom) * 0.5f;
        drawLine(p.separator, D2D1::Point2F(screen.left + 12.0f, centerY),
                 D2D1::Point2F(screen.right - 12.0f, centerY), 1.0f);

        const float toastW = std::min(62.0f, screen.right - screen.left - 18.0f);
        const float toastH = 16.0f;
        const float toastLeft = (screen.left + screen.right - toastW) * 0.5f;
        const float toastTop = top ? screen.top + 7.0f : screen.bottom - toastH - 7.0f;
        const D2D1_RECT_F toast =
            D2D1::RectF(toastLeft, toastTop, toastLeft + toastW, toastTop + toastH);
        const float toastRadius = toastH * 0.5f;
        painter.fillRoundRect(tone(p.listSelected), toast, toastRadius);
        painter.strokeRoundRect(tone(p.accent), toast, 1.0f, toastRadius);

        const float coverSize = toastH - 6.0f;
        const D2D1_RECT_F cover = D2D1::RectF(
            toast.left + 3.0f, toast.top + 3.0f, toast.left + 3.0f + coverSize,
            toast.bottom - 3.0f);
        const D2D1_POINT_2F coverCenter =
            D2D1::Point2F((cover.left + cover.right) * 0.5f,
                          (cover.top + cover.bottom) * 0.5f);
        if (auto* brush = painter.brush(tone(p.accent)))
            painter.target()->FillEllipse(
                D2D1::Ellipse(coverCenter, coverSize * 0.5f, coverSize * 0.5f), brush);
        drawLine(p.textOnAccent, D2D1::Point2F(cover.left + 2.0f, cover.top + 4.0f),
                 D2D1::Point2F(cover.right - 2.0f, cover.top + 4.0f), 0.8f);
        drawLine(p.text, D2D1::Point2F(cover.right + 4.0f, toast.top + 5.0f),
                 D2D1::Point2F(toast.right - 5.0f, toast.top + 5.0f), 1.2f);
        drawLine(p.textSecondary, D2D1::Point2F(cover.right + 4.0f, toast.top + 11.0f),
                 D2D1::Point2F(toast.right - 17.0f, toast.top + 11.0f), 1.0f);
    }

    void drawSongToastPositionRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
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

            const float artworkW = std::min(100.0f, cardW - 20.0f);
            const D2D1_RECT_F artwork = D2D1::RectF(
                card.left + (cardW - artworkW) * 0.5f, card.top + 8.0f,
                card.left + (cardW + artworkW) * 0.5f, card.top + 58.0f);
            drawSongToastPositionArtwork(painter, artwork, i == 0, row.enabled);

            // 示意图先画，单选框最后画，避免位置指示被单选框遮住。
            const D2D1_POINT_2F radioCenter =
                D2D1::Point2F(card.right - 16.0f, card.top + 16.0f);
            if (!row.enabled) {
                if (auto* brush = painter.brush(p.disabled))
                    painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush, 1.0f);
                if (selected) {
                    if (auto* brush = painter.brush(p.disabled))
                        painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                      brush);
                }
            } else if (selected) {
                if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush);
                if (auto* brush = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                  brush);
            } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
                painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                              pressed ? 1.5f : 1.0f);
            }

            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 10.0f, card.top + 65.0f,
                                         card.right - 10.0f, card.bottom - 8.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawHoverControlStyleArtwork(fluent::FluentDialogSurface::Painter& painter,
                                       const D2D1_RECT_F& bounds, bool mediaCard,
                                       bool enabled) {
        const auto& p = fluent::palette();
        const auto tone = [enabled](D2D1_COLOR_F color) {
            if (!enabled)
                color.a *= 0.42f;
            return color;
        };
        const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                                  float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                painter.target()->DrawLine(from, to, brush, stroke);
        };

        const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                             bounds.right - 1.0f, bounds.bottom - 1.0f);
        painter.fillRoundRect(tone(p.controlFill), panel, 9.0f);
        painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 9.0f);

        // 设置页直接复用任务栏和媒体卡片使用的播放控件几何，避免预览和真实图标各画一套。
        media_control::Geometry controls;
        ID2D1Factory* factory = nullptr;
        bool hasControls = false;
        if (painter.target()) {
            painter.target()->GetFactory(&factory);
            if (factory) {
                hasControls = media_control::create(factory, controls);
                factory->Release();
            }
        }
        const auto drawControl = [&](int index, bool playing, D2D1_POINT_2F center,
                                     float radius, D2D1_COLOR_F color) {
            if (!hasControls)
                return;
            if (auto* brush = painter.brush(tone(color)))
                media_control::draw(painter.target(), controls, index, playing, center, radius,
                                    brush);
        };
        const auto fillButton = [&](D2D1_COLOR_F color, D2D1_POINT_2F center, float size) {
            painter.fillRoundRect(tone(color),
                                  D2D1::RectF(center.x - size * 0.5f, center.y - size * 0.5f,
                                              center.x + size * 0.5f, center.y + size * 0.5f),
                                  size * 0.5f);
        };

        if (!mediaCard) {
            // 内嵌控件：保留两条等长歌词基线，四个控件统一使用任务栏里的普通单色字形。
            const float panelH = panel.bottom - panel.top;
            const float cx = (panel.left + panel.right) * 0.5f;
            const float cy = (panel.top + panel.bottom) * 0.5f;
            const float lineLeft = panel.left + 18.0f;
            const float lineRight = panel.right - 18.0f;
            drawLine(p.separator, D2D1::Point2F(lineLeft, panel.top + 20.0f),
                     D2D1::Point2F(lineRight, panel.top + 20.0f), 1.2f);
            drawLine(p.separator, D2D1::Point2F(lineLeft, panel.bottom - 20.0f),
                     D2D1::Point2F(lineRight, panel.bottom - 20.0f), 1.2f);

            const float preferredControlSize = std::clamp(panelH * 0.30f, 28.0f, 34.0f);
            constexpr float kControlGap = 10.0f;
            constexpr float kVolumeGap = 14.0f;
            const float preferredGroupW = preferredControlSize * 4.0f +
                                          kControlGap * 2.0f + kVolumeGap;
            // 卡片变窄时按同一比例缩放控件组，保留 4 DIP 安全边距，避免图标越界。
            const float availableW = std::max(0.0f, panel.right - panel.left - 8.0f);
            const float groupScale = preferredGroupW > 0.0f
                                         ? std::clamp(availableW / preferredGroupW, 0.0f, 1.0f)
                                         : 1.0f;
            const float controlSize = preferredControlSize * groupScale;
            const float controlGap = kControlGap * groupScale;
            const float volumeGap = kVolumeGap * groupScale;
            const float groupW = controlSize * 4.0f + controlGap * 2.0f + volumeGap;
            const float groupLeft = cx - groupW * 0.5f;
            const float previousX = groupLeft + controlSize * 0.5f;
            const float playX = previousX + controlSize + controlGap;
            const float nextX = playX + controlSize + controlGap;
            const float volumeX = nextX + controlSize + volumeGap;
            const D2D1_POINT_2F previous = D2D1::Point2F(previousX, cy);
            const D2D1_POINT_2F play = D2D1::Point2F(playX, cy);
            const D2D1_POINT_2F next = D2D1::Point2F(nextX, cy);
            const D2D1_POINT_2F volume = D2D1::Point2F(volumeX, cy);
            const float iconRadius = controlSize * 0.25f;
            const D2D1_COLOR_F iconColor = enabled ? p.text : p.disabled;
            drawControl(0, false, previous, iconRadius, iconColor);
            drawControl(1, false, play, iconRadius, iconColor);
            drawControl(2, false, next, iconRadius, iconColor);
            if (auto* brush = painter.brush(tone(iconColor)))
                media_control::drawVolume(painter.target(), volume, iconRadius * 0.8f, brush,
                                          enabled ? 3 : 0);
            media_control::release(controls);
            return;
        }

        // 媒体卡片按实际弹窗 384x208 DIP 等比缩放，结构顺序也与真实弹窗一致。
        const float panelW = panel.right - panel.left;
        const float panelH = panel.bottom - panel.top;
        const float popupScale = std::min(panelW / 384.0f, panelH / 208.0f);
        const float popupW = 384.0f * popupScale;
        const float popupH = 208.0f * popupScale;
        const float originX = panel.left + (panelW - popupW) * 0.5f;
        const float originY = panel.top + (panelH - popupH) * 0.5f;
        const auto popupPoint = [=](float x, float y) {
            return D2D1::Point2F(originX + x * popupScale, originY + y * popupScale);
        };
        const auto popupRect = [=](float left, float top, float right, float bottom) {
            return D2D1::RectF(originX + left * popupScale, originY + top * popupScale,
                               originX + right * popupScale, originY + bottom * popupScale);
        };

        const D2D1_COLOR_F primary = enabled ? p.text : p.disabled;
        const D2D1_COLOR_F secondary = enabled ? p.textSecondary : p.disabled;

        // 来源行：左侧是应用图标，右侧留出真实媒体卡片的音量入口。
        const D2D1_RECT_F sourceIcon = popupRect(16.0f, 14.0f, 34.0f, 32.0f);
        painter.fillRoundRect(tone(p.accent), sourceIcon, 4.0f);
        drawLine(p.textOnAccent, popupPoint(21.0f, 20.0f), popupPoint(29.0f, 20.0f), 1.3f);
        drawLine(p.textOnAccent, popupPoint(21.0f, 24.0f), popupPoint(27.0f, 24.0f), 1.3f);
        drawLine(primary, popupPoint(42.0f, 23.0f), popupPoint(126.0f, 23.0f), 1.5f);
        const D2D1_RECT_F volumeButton = popupRect(344.0f, 8.0f, 376.0f, 40.0f);
        painter.fillRoundRect(tone(p.controlHover), volumeButton, 16.0f);
        if (auto* brush = painter.brush(tone(secondary)))
            media_control::drawVolume(painter.target(), popupPoint(360.0f, 24.0f),
                                      7.0f * popupScale, brush, 3);

        // 封面与歌曲信息：封面比例保持 80x80，右侧用不同长度的线表现标题/歌手。
        drawCoverEffectArtwork(painter, popupRect(16.0f, 44.0f, 96.0f, 124.0f), false,
                               enabled);
        drawLine(primary, popupPoint(112.0f, 57.0f), popupPoint(286.0f, 57.0f), 2.0f);
        drawLine(secondary, popupPoint(112.0f, 86.0f), popupPoint(236.0f, 86.0f), 1.6f);
        drawLine(secondary, popupPoint(112.0f, 97.0f), popupPoint(198.0f, 97.0f), 1.2f);

        // 进度：上方两段短时间标记，下面是实际弹窗的 4 DIP 进度轨道。
        drawLine(secondary, popupPoint(16.0f, 136.0f), popupPoint(42.0f, 136.0f), 1.4f);
        drawLine(secondary, popupPoint(342.0f, 136.0f), popupPoint(368.0f, 136.0f), 1.4f);
        const D2D1_RECT_F progressTrack = popupRect(16.0f, 148.0f, 368.0f, 152.0f);
        painter.fillRoundRect(tone(p.separator), progressTrack, 2.0f);
        painter.fillRoundRect(tone(p.accent), popupRect(16.0f, 148.0f, 178.0f, 152.0f), 2.0f);

        // 底部三键使用真实控件尺寸关系：两侧 36 DIP，中间 40 DIP。
        const float controlY = originY + 182.0f * popupScale;
        const float centerX = originX + 192.0f * popupScale;
        const float sideSize = 36.0f * popupScale;
        const float playSize = 40.0f * popupScale;
        const D2D1_POINT_2F previous =
            D2D1::Point2F(centerX - 84.0f * popupScale, controlY);
        const D2D1_POINT_2F play = D2D1::Point2F(centerX, controlY);
        const D2D1_POINT_2F next = D2D1::Point2F(centerX + 84.0f * popupScale, controlY);
        fillButton(p.controlHover, previous, sideSize);
        fillButton(p.accent, play, playSize);
        fillButton(p.controlHover, next, sideSize);
        drawControl(0, false, previous, 9.0f * popupScale, primary);
        drawControl(1, false, play, 11.0f * popupScale, p.textOnAccent);
        drawControl(2, false, next, 9.0f * popupScale, primary);
        media_control::release(controls);
    }

    void drawIdleQuoteBackgroundArtwork(fluent::FluentDialogSurface::Painter& painter,
                                        const D2D1_RECT_F& bounds, int style, bool enabled) {
        auto* rt = painter.target();
        if (!rt)
            return;

        const auto& p = fluent::palette();
        const auto tone = [enabled](D2D1_COLOR_F color) {
            if (!enabled)
                color.a *= 0.42f;
            return color;
        };
        const auto withOpacity = [](D2D1_COLOR_F color, float opacity) {
            color.a *= opacity;
            return color;
        };
        const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from,
                                  D2D1_POINT_2F to, float stroke) {
            if (auto* brush = painter.brush(tone(color)))
                rt->DrawLine(from, to, brush, stroke);
        };

        const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                             bounds.right - 1.0f, bounds.bottom - 1.0f);
        painter.fillRoundRect(tone(p.controlFill), panel, 8.0f);
        painter.fillRoundRect(tone(withOpacity(p.accent, 0.055f)), panel, 8.0f);
        painter.strokeRoundRect(tone(withOpacity(p.cardStroke, 0.72f)), panel, 1.0f, 8.0f);

        const float left = panel.left + 8.0f;
        const float right = panel.right - 8.0f;
        const float top = panel.top + 7.0f;
        const float bottom = panel.bottom - 7.0f;
        const float width = std::max(0.0f, right - left);
        const float height = std::max(0.0f, bottom - top);
        if (width <= 0.0f || height <= 0.0f)
            return;

        rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        switch (style) {
        case 1: { // 落叶
            struct Leaf {
                float x;
                float y;
                float size;
                float angle;
            };
            static constexpr Leaf leaves[] = {
                {0.04f, 0.30f, 5.0f, -28.0f}, {0.14f, 0.76f, 3.6f, 22.0f},
                {0.23f, 0.16f, 4.2f, 48.0f},  {0.34f, 0.58f, 5.6f, -12.0f},
                {0.46f, 0.86f, 3.8f, 34.0f},  {0.57f, 0.28f, 4.8f, -46.0f},
                {0.69f, 0.70f, 4.0f, 16.0f},   {0.80f, 0.12f, 5.2f, -34.0f},
                {0.92f, 0.48f, 4.0f, 42.0f},
            };
            for (size_t i = 0; i < _countof(leaves); ++i) {
                const float x = left + leaves[i].x * width;
                const float y = top + leaves[i].y * height;
                const D2D1_POINT_2F center = D2D1::Point2F(x, y);
                const D2D1_COLOR_F color = withOpacity(p.accent, i % 3 == 0 ? 0.90f : 0.68f);
                if (auto* brush = painter.brush(tone(color))) {
                    D2D1_MATRIX_3X2_F previous{};
                    rt->GetTransform(&previous);
                    rt->SetTransform(D2D1::Matrix3x2F::Rotation(leaves[i].angle, center));
                    rt->FillEllipse(D2D1::Ellipse(center, leaves[i].size,
                                                   leaves[i].size * 0.44f),
                                    brush);
                    rt->DrawLine(D2D1::Point2F(x - leaves[i].size * 0.72f, y),
                                 D2D1::Point2F(x + leaves[i].size * 0.72f, y), brush, 0.8f);
                    rt->SetTransform(previous);
                }
            }
            drawLine(withOpacity(p.textSecondary, 0.55f), D2D1::Point2F(left, bottom - 2.0f),
                     D2D1::Point2F(right, bottom - 2.0f), 0.8f);
            break;
        }
        case 2: { // 闪烁星星
            struct Star {
                float x;
                float y;
                float radius;
                bool cross;
            };
            static constexpr Star stars[] = {
                {0.04f, 0.22f, 1.5f, true},  {0.11f, 0.72f, 1.0f, false},
                {0.18f, 0.44f, 1.2f, false}, {0.25f, 0.14f, 1.0f, false},
                {0.31f, 0.84f, 1.4f, true},  {0.38f, 0.36f, 0.9f, false},
                {0.45f, 0.64f, 1.1f, false}, {0.52f, 0.16f, 1.3f, true},
                {0.59f, 0.48f, 0.9f, false}, {0.66f, 0.78f, 1.2f, false},
                {0.73f, 0.28f, 1.0f, true},  {0.80f, 0.58f, 1.4f, false},
                {0.88f, 0.12f, 0.9f, false}, {0.95f, 0.82f, 1.2f, true},
            };
            for (size_t i = 0; i < _countof(stars); ++i) {
                const D2D1_POINT_2F center =
                    D2D1::Point2F(left + stars[i].x * width, top + stars[i].y * height);
                const D2D1_COLOR_F color = withOpacity(p.accent, stars[i].cross ? 0.90f : 0.64f);
                if (auto* brush = painter.brush(tone(color))) {
                    rt->FillEllipse(D2D1::Ellipse(center, stars[i].radius, stars[i].radius),
                                    brush);
                    if (stars[i].cross) {
                        const float ray = stars[i].radius * 2.8f;
                        rt->DrawLine(D2D1::Point2F(center.x - ray, center.y),
                                     D2D1::Point2F(center.x + ray, center.y), brush, 0.8f);
                        rt->DrawLine(D2D1::Point2F(center.x, center.y - ray),
                                     D2D1::Point2F(center.x, center.y + ray), brush, 0.8f);
                    }
                }
            }
            break;
        }
        case 3: { // 二进制
            auto* format = painter.textFormat(11.0f, 600, true, true);
            if (format) {
                constexpr wchar_t digits[] = L"101100101011010010110101";
                constexpr int kColumns = 8;
                constexpr int kRows = 3;
                const float columnW = width / static_cast<float>(kColumns);
                const float rowH = height / static_cast<float>(kRows);
                for (int col = 0; col < kColumns; ++col) {
                    for (int row = 0; row < kRows; ++row) {
                        const int index = (col * 3 + row * 5) %
                                          (static_cast<int>(_countof(digits)) - 1);
                        const wchar_t digit[2] = {digits[index], L'\0'};
                        const D2D1_RECT_F rect = D2D1::RectF(
                            left + col * columnW, top + row * rowH,
                            left + (col + 1) * columnW, top + (row + 1) * rowH);
                        painter.drawText(std::wstring(digit), format, rect,
                                         tone(withOpacity(p.accent, 0.52f +
                                                                   ((col + row) % 3) * 0.16f)),
                                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
            break;
        }
        case 4: { // 流光粒子
            constexpr int kFlowLines = 3;
            constexpr int kFlowSamples = 28;
            constexpr float kTwoPi = 6.28318530718f;
            const auto flowY = [&](float x, float lane, float phase) {
                const float t = std::clamp((x - left) / width, 0.0f, 1.0f);
                const float center = 0.28f + lane * 0.22f;
                const float wave = std::sin(t * kTwoPi * 1.15f + phase) * 0.07f +
                                   std::sin(t * kTwoPi * 2.25f + phase * 0.65f) * 0.035f;
                return top + (center + wave) * height;
            };
            for (int lane = 0; lane < kFlowLines; ++lane) {
                const float phase = 0.45f + lane * 1.12f;
                D2D1_POINT_2F previous =
                    D2D1::Point2F(left, flowY(left, static_cast<float>(lane), phase));
                for (int sample = 1; sample <= kFlowSamples; ++sample) {
                    const float t = static_cast<float>(sample) / kFlowSamples;
                    const float x = left + width * t;
                    const D2D1_POINT_2F current =
                        D2D1::Point2F(x, flowY(x, static_cast<float>(lane), phase));
                    drawLine(withOpacity(lane == 1 ? p.accent : p.textSecondary,
                                         lane == 1 ? 0.34f : 0.20f),
                             previous, current, lane == 1 ? 1.4f : 0.9f);
                    previous = current;
                }
            }

            struct Particle {
                float x;
                float lane;
                float radius;
                float phase;
                bool accent;
            };
            static constexpr Particle particles[] = {
                {0.04f, 0.0f, 1.5f, 0.3f, true},   {0.10f, 1.0f, 1.1f, 1.8f, false},
                {0.17f, 2.0f, 1.8f, 3.4f, true},   {0.25f, 0.0f, 1.0f, 4.6f, false},
                {0.32f, 1.0f, 2.0f, 5.5f, true},   {0.39f, 2.0f, 1.2f, 0.8f, false},
                {0.47f, 0.0f, 1.4f, 2.6f, true},   {0.54f, 1.0f, 1.0f, 4.0f, false},
                {0.61f, 2.0f, 1.9f, 5.1f, true},   {0.68f, 0.0f, 1.1f, 1.2f, false},
                {0.75f, 1.0f, 1.7f, 2.2f, true},   {0.82f, 2.0f, 1.0f, 3.8f, false},
                {0.89f, 0.0f, 1.8f, 5.0f, true},   {0.96f, 1.0f, 1.2f, 0.6f, false},
            };
            for (size_t i = 0; i < _countof(particles); ++i) {
                const float x = left + particles[i].x * width;
                const float y = flowY(x, particles[i].lane, particles[i].phase);
                const D2D1_POINT_2F center = D2D1::Point2F(x, y);
                const D2D1_COLOR_F base = particles[i].accent ? p.accent : p.textSecondary;
                if (auto* brush = painter.brush(tone(withOpacity(base, 0.14f))))
                    rt->FillEllipse(D2D1::Ellipse(center, particles[i].radius * 2.3f,
                                                   particles[i].radius * 2.3f),
                                    brush);
                if (auto* brush = painter.brush(tone(withOpacity(base,
                                                                   particles[i].accent ? 0.92f
                                                                                       : 0.72f)))) {
                    const float trail = 5.0f + particles[i].radius * 2.8f;
                    rt->DrawLine(D2D1::Point2F(x - trail, y + 0.5f),
                                 D2D1::Point2F(x - particles[i].radius, y), brush, 0.9f);
                    rt->FillEllipse(D2D1::Ellipse(center, particles[i].radius,
                                                   particles[i].radius),
                                    brush);
                    if (particles[i].accent && i % 3 == 0) {
                        const float ray = particles[i].radius * 2.1f;
                        rt->DrawLine(D2D1::Point2F(x - ray, y), D2D1::Point2F(x + ray, y),
                                     brush, 0.65f);
                        rt->DrawLine(D2D1::Point2F(x, y - ray), D2D1::Point2F(x, y + ray),
                                     brush, 0.65f);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        rt->PopAxisAlignedClip();
    }

    void drawIdleQuoteBackgroundRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
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

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 10.0f,
                            card.bottom - 31.0f);
            drawIdleQuoteBackgroundArtwork(painter, artwork, optionIndex, row.enabled);

            const D2D1_POINT_2F radioCenter =
                D2D1::Point2F(card.right - 16.0f, card.top + 14.0f);
            if (!row.enabled) {
                if (auto* brush = painter.brush(p.disabled))
                    painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush, 1.0f);
                if (selected) {
                    if (auto* brush = painter.brush(p.disabled))
                        painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                      brush);
                }
            } else if (selected) {
                if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush);
                if (auto* brush = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                  brush);
            } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
                painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                              pressed ? 1.5f : 1.0f);
            }

            painter.drawText(row.options[optionIndex], titleFormat,
                             D2D1::RectF(card.left + 10.0f, card.bottom - 24.0f,
                                         card.right - 10.0f, card.bottom - 5.0f),
                             row.enabled ? p.text : p.disabled);
        }
    }

    void drawHoverControlStyleRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
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

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 30.0f,
                            card.bottom - 27.0f);
            drawHoverControlStyleArtwork(painter, artwork, i == 1, row.enabled);

            // 示意图先画，单选框最后画，确保它始终位于卡片内容之上。
            const D2D1_POINT_2F radioCenter =
                D2D1::Point2F(card.right - 16.0f, card.top + 14.0f);
            if (!row.enabled) {
                if (auto* brush = painter.brush(p.disabled))
                    painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush, 1.0f);
                if (selected) {
                    if (auto* brush = painter.brush(p.disabled))
                        painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                      brush);
                }
            } else if (selected) {
                if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f),
                                                  brush);
                if (auto* brush = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                                  brush);
            } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
                painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                              pressed ? 1.5f : 1.0f);
            }

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

        const auto& p = fluent::palette();
        auto* format = painter.textFormat(13.0f, 400, false, true);
        if (!format)
            return;
        const float cy = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        constexpr float kCircle = 16.0f;
        constexpr float kTextGap = 8.0f;
        constexpr float kOptionGap = 20.0f;
        float x = row.controlRect.left;
        row.optionRects.clear();
        for (size_t i = 0; i < row.options.size(); ++i) {
            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id && hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            const float textW = painter.measureTextWidth(row.options[i], format);
            D2D1_ELLIPSE circle{D2D1::Point2F(x + kCircle * 0.5f, cy), kCircle * 0.5f,
                                kCircle * 0.5f};
            if (!row.enabled) {
                if (auto* br = painter.brush(p.disabled)) {
                    painter.target()->DrawEllipse(circle, br, 1.0f);
                    if (selected)
                        painter.target()->FillEllipse(
                            D2D1::Ellipse(circle.point, 4.0f, 4.0f), br);
                }
            } else if (selected) {
                if (auto* br = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(circle, br);
                if (auto* br = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(circle.point, 4.0f, 4.0f), br);
            } else {
                if (hovered)
                    painter.target()->FillEllipse(circle, painter.brush(p.listHover));
                if (auto* br = painter.brush(p.textSecondary))
                    painter.target()->DrawEllipse(circle, br, pressed ? 1.5f : 1.0f);
            }
            painter.drawText(row.options[i], format,
                             D2D1::RectF(x + kCircle + kTextGap, row.controlRect.top,
                                         x + kCircle + kTextGap + textW + 1.0f,
                                         row.controlRect.bottom),
                             row.enabled ? p.text : p.disabled);
            row.optionRects.push_back(D2D1::RectF(
                x, row.controlRect.top, x + kCircle + kTextGap + textW,
                row.controlRect.bottom));
            x += kCircle + kTextGap + textW + kOptionGap;
        }
    }

    void drawModeGrid(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
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
        const auto& p = fluent::palette();
        const float innerX = row.cardRect.left + 16.0f;
        const float innerRight = row.cardRect.right - 16.0f;
        const float listTop = row.cardRect.top + kIdleAppsListTop;
        const size_t count = state.idleApps.size();
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
            const auto& app = state.idleApps[i];
            const float top = listTop + static_cast<float>(i) * kIdleAppItemH;
            const D2D1_RECT_F item =
                D2D1::RectF(innerX, top, innerRight, top + 44.0f);
            const int option = hoverId == row.id ? hoverOption : -1;
            const int pressedOptionValue = pressedId == row.id ? pressedOption : -1;
            const bool appHovered = option == static_cast<int>(i) ||
                                    option == kIdleAppEditOptionBase + static_cast<int>(i);
            const bool appPressed =
                pressedOptionValue == static_cast<int>(i) ||
                pressedOptionValue == kIdleAppEditOptionBase + static_cast<int>(i);
            const bool hovered = row.enabled && hoverId == row.id &&
                                 appHovered;
            const bool pressed = row.enabled && pressedId == row.id &&
                                 appPressed;
            painter.fillRoundRect(row.enabled ? (hovered ? p.controlHover : p.controlFill)
                                              : p.listHover,
                                  item, 7.0f);
            if (pressed)
                painter.fillRoundRect(p.controlPressed, item, 7.0f);

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
        const auto& p = fluent::palette();
        const bool active = hoverId == kIdContentScrollBar ||
                            pressedId == kIdContentScrollBar || scrollDragging;
        painter.fillRoundRect(active ? p.accent : p.textSecondary, scrollThumbRect,
                              kScrollBarWidth * 0.5f);
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        updateHintHeights(painter);
        const auto& p = fluent::palette();
        drawNav(painter);
        painter.drawText(pageTitles[activePage], painter.textFormat(20.0f, 600),
                         pageTitleRects[activePage], p.text);

        painter.target()->PushAxisAlignedClip(
            contentViewportRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (auto& row : rows[activePage]) {
            if (row.kind == ControlKind::Header) {
                painter.drawText(row.text, painter.textFormat(12.0f, 600), row.labelRect,
                                 p.textSecondary);
                continue;
            }
            painter.fillRoundRect(p.cardFill, row.cardRect, fluent::metrics::cardRadius);
            painter.strokeRoundRect(p.cardStroke, row.cardRect, 1.0f, fluent::metrics::cardRadius);
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
                drawSpectrumBackgroundArtwork(painter, row.artworkRect, row.enabled);
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
        painter.target()->PopAxisAlignedClip();
        drawScrollBar(painter);
    }

    int hitTest(float x, float y, int* option = nullptr) const {
        if (option)
            *option = -1;
        for (int i = 0; i < kSettingsPageCount; ++i) {
            if (contains(navItemRects[i], x, y)) {
                if (option)
                    *option = i;
                return kIdNav;
            }
        }
        if (contains(scrollBarHitRect(), x, y))
            return kIdContentScrollBar;
        for (const auto& row : rows[activePage]) {
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
                if (state.idleApps.size() < kMaxIdleApps && contains(row.addAppRect, x, y))
                    return row.id;
                continue;
            }
            bool inControl = contains(row.controlRect, x, y);
            if (row.id == kIdSpectrumBackground)
                inControl = inControl || contains(row.artworkRect, x, y);
            if (row.id == kIdIdleQuoteBackground && !row.optionRects.empty())
                inControl = inControl || contains(row.optionRects.front(), x, y);
            if (!inControl)
                continue;
            if (option && (row.kind == ControlKind::Radio || row.kind == ControlKind::ModeGrid)) {
                for (size_t i = 0; i < row.optionRects.size(); ++i) {
                    if (contains(row.optionRects[i], x, y)) {
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
        std::vector<int> order{kIdNav};
        for (const auto& row : rows[activePage]) {
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

    void onCommand(int id, int option = -1) {
        if (id == kIdNav)
            return;
        if (id == kIdIdleAppNames) {
            Row* appRow = findRow(kIdIdleApps);
            if (!appRow || !appRow->enabled)
                return;
            appRow->checked = !appRow->checked;
            state.idleAppNamesVisible = appRow->checked;
            if (actions.onIdleAppNamesVisible)
                actions.onIdleAppNamesVisible(appRow->checked);
            if (hwnd)
                surface.invalidate();
            return;
        }
        Row* row = findRow(id);
        if (!row || !row->enabled)
            return;
        switch (id) {
        case kIdIdleEntry:
            row->checked = !row->checked;
            state.idleEntryEnabled = row->checked;
            updateIdleRowsEnabled();
            updateFloatingCardRowsEnabled();
            if (actions.onIdleEntryEnabled)
                actions.onIdleEntryEnabled(row->checked);
            break;
        case kIdIdleQuote:
            row->checked = !row->checked;
            state.idleQuoteEnabled = row->checked;
            updateIdleRowsEnabled();
            if (actions.onIdleQuoteEnabled)
                actions.onIdleQuoteEnabled(row->checked);
            break;
        case kIdIdleQuoteSource:
            state.idleQuoteSource = row->selected;
            updateIdleQuoteSourceRow();
            if (actions.onIdleQuoteSource)
                actions.onIdleQuoteSource(row->selected);
            break;
        case kIdIdleQuoteRefreshInterval:
            state.idleQuoteRefreshInterval = row->selected;
            if (actions.onIdleQuoteRefreshInterval)
                actions.onIdleQuoteRefreshInterval(row->selected);
            break;
        case kIdIdleQuoteAlignment:
            state.idleQuoteAlignment = row->selected;
            if (actions.onIdleQuoteAlignment)
                actions.onIdleQuoteAlignment(row->selected);
            break;
        case kIdIdleQuoteBackground:
            state.idleQuoteBackground = row->selected;
            if (actions.onIdleQuoteBackground)
                actions.onIdleQuoteBackground(row->selected);
            updateIdleRowsEnabled();
            break;
        case kIdIdleQuoteBackgroundScope:
            state.idleQuoteBackgroundScope = row->selected;
            if (actions.onIdleQuoteBackgroundScope)
                actions.onIdleQuoteBackgroundScope(row->selected);
            break;
        case kIdIdleApps:
            if (option == kIdleAppNamesOption) {
                row->checked = !row->checked;
                state.idleAppNamesVisible = row->checked;
                if (actions.onIdleAppNamesVisible)
                    actions.onIdleAppNamesVisible(row->checked);
            } else if (option >= kIdleAppEditOptionBase) {
                const int index = option - kIdleAppEditOptionBase;
                if (index >= 0 && static_cast<size_t>(index) < state.idleApps.size() &&
                    actions.onEditIdleApp)
                    actions.onEditIdleApp(index);
            } else if (option >= 0) {
                if (actions.onRemoveIdleApp)
                    actions.onRemoveIdleApp(option);
            } else if (state.idleApps.size() < kMaxIdleApps && actions.onAddIdleApp) {
                actions.onAddIdleApp();
            }
            break;
        case kIdSongInfo:
            row->checked = !row->checked;
            if (actions.onSongInfoVisible)
                actions.onSongInfoVisible(row->checked);
            break;
        case kIdAlbumCover:
            row->checked = !row->checked;
            if (actions.onAlbumCoverVisible)
                actions.onAlbumCoverVisible(row->checked);
            if (auto* effect = findRow(kIdCoverEffect))
                effect->enabled = row->checked && !minimalModeActive();
            if (auto* platformIcon = findRow(kIdPlatformIcon))
                platformIcon->enabled = row->checked;
            break;
        case kIdPlatformIcon:
            row->checked = !row->checked;
            if (actions.onPlatformIconVisible)
                actions.onPlatformIconVisible(row->checked);
            break;
        case kIdCoverEffect:
            if (actions.onCoverEffectVinyl)
                actions.onCoverEffectVinyl(row->selected == 1);
            break;
        case kIdSpectrum:
            row->checked = !row->checked;
            if (auto* style = findRow(kIdSpectrumStyle))
                style->enabled = row->checked;
            if (auto* background = findRow(kIdSpectrumBackground))
                background->enabled = spectrumBackgroundAvailable();
            if (auto* opacity = findRow(kIdSpectrumOpacity))
                opacity->enabled = spectrumBackgroundAvailable() &&
                                   findRow(kIdSpectrumBackground) &&
                                   findRow(kIdSpectrumBackground)->checked;
            if (actions.onSpectrum)
                actions.onSpectrum(row->checked);
            updateProgressBackgroundRowsEnabled();
            break;
        case kIdSpectrumStyle:
            if (auto* background = findRow(kIdSpectrumBackground))
                background->enabled = spectrumBackgroundAvailable();
            if (auto* opacity = findRow(kIdSpectrumOpacity))
                opacity->enabled = spectrumBackgroundAvailable() &&
                                   findRow(kIdSpectrumBackground) &&
                                   findRow(kIdSpectrumBackground)->checked;
            if (actions.onSpectrumStyle)
                actions.onSpectrumStyle(row->selected);
            updateProgressBackgroundRowsEnabled();
            break;
        case kIdSpectrumBackground:
            row->checked = !row->checked;
            if (auto* opacity = findRow(kIdSpectrumOpacity))
                opacity->enabled = row->checked && spectrumBackgroundAvailable();
            if (actions.onSpectrumBackground)
                actions.onSpectrumBackground(row->checked);
            updateProgressBackgroundRowsEnabled();
            break;
        case kIdSpectrumOpacity:
            if (actions.onSpectrumOpacity)
                actions.onSpectrumOpacity(row->value);
            break;
        case kIdTaskbarBackground:
            if (auto* opacity = findRow(kIdCoverBackgroundOpacity))
                opacity->enabled = row->selected == 1;
            if (actions.onTaskbarBackground)
                actions.onTaskbarBackground(row->selected);
            break;
        case kIdCoverBackgroundOpacity:
            if (actions.onCoverBackgroundOpacity)
                actions.onCoverBackgroundOpacity(row->value);
            break;
        case kIdProgressBackground:
            row->checked = !row->checked;
            updateProgressBackgroundRowsEnabled();
            if (actions.onProgressBackground)
                actions.onProgressBackground(row->checked);
            break;
        case kIdProgressBackgroundOpacity:
            if (actions.onProgressBackgroundOpacity)
                actions.onProgressBackgroundOpacity(row->value);
            break;
        case kIdHoverControls:
            row->checked = !row->checked;
            if (actions.onHoverControls)
                actions.onHoverControls(row->checked);
            if (auto* style = findRow(kIdHoverControlStyle))
                style->enabled = row->checked && !minimalModeActive();
            updateFloatingCardRowsEnabled();
            break;
        case kIdHoverControlStyle:
            if (actions.onHoverControlStyle)
                actions.onHoverControlStyle(row->selected);
            updateFloatingCardRowsEnabled();
            updateIdleRowsEnabled();
            break;
        case kIdFloatingCardTrigger:
            state.floatingCardTrigger = row->selected;
            if (actions.onFloatingCardTrigger)
                actions.onFloatingCardTrigger(row->selected);
            break;
        case kIdFloatingCardBackground:
            state.floatingCardBackground = row->selected;
            if (actions.onFloatingCardBackground)
                actions.onFloatingCardBackground(row->selected);
            updateFloatingCardRowsEnabled();
            break;
        case kIdFloatingCardBackgroundColor:
            openColorPicker(kIdFloatingCardBackgroundColor);
            break;
        case kIdFloatingCardFollowAlbum:
            row->checked = !row->checked;
            state.floatingCardFollowAlbum = row->checked;
            if (actions.onFloatingCardFollowAlbum)
                actions.onFloatingCardFollowAlbum(row->checked);
            break;
        case kIdFloatingCardAutoTextContrast:
            row->checked = !row->checked;
            state.floatingCardAutoTextContrast = row->checked;
            if (actions.onFloatingCardAutoTextContrast)
                actions.onFloatingCardAutoTextContrast(row->checked);
            break;
        case kIdSongToast:
            row->checked = !row->checked;
            if (auto* duration = findRow(kIdSongToastDuration))
                duration->enabled = row->checked;
            if (auto* skipFullscreen = findRow(kIdSongToastSkipFullscreen))
                skipFullscreen->enabled = row->checked;
            if (auto* position = findRow(kIdSongToastPosition))
                position->enabled = row->checked;
            if (actions.onSongToastEnabled)
                actions.onSongToastEnabled(row->checked);
            break;
        case kIdSongToastDuration:
            if (actions.onSongToastDuration)
                actions.onSongToastDuration(row->value);
            break;
        case kIdSongToastSkipFullscreen:
            row->checked = !row->checked;
            if (actions.onSongToastSkipFullscreen)
                actions.onSongToastSkipFullscreen(row->checked);
            break;
        case kIdSongToastPosition:
            if (actions.onSongToastPosition)
                actions.onSongToastPosition(row->selected);
            break;
        case kIdTaskbarTheme:
            if (actions.onTaskbarTheme)
                actions.onTaskbarTheme(themeModeFromIndex(row->selected));
            break;
        case kIdWindowTheme:
            if (actions.onWindowTheme)
                actions.onWindowTheme(themeModeFromIndex(row->selected));
            break;
        case kIdRenderMode:
            if (actions.onRenderMode)
                actions.onRenderMode(row->selected);
            break;
        case kIdPickFont:
            if (actions.onPickFont)
                actions.onPickFont();
            break;
        case kIdFontColor:
            if (actions.onFontColorEffect)
                actions.onFontColorEffect();
            break;
        case kIdFollowAlbum:
            row->checked = !row->checked;
            if (actions.onFollowAlbum)
                actions.onFollowAlbum(row->checked);
            break;
        case kIdDoubleLine:
            row->checked = !row->checked;
            if (actions.onDoubleLineLyrics)
                actions.onDoubleLineLyrics(row->checked);
            break;
        case kIdAlignment:
            if (actions.onLyricAlignment)
                actions.onLyricAlignment(row->selected);
            break;
        case kIdSecondaryOn:
            row->checked = !row->checked;
            if (actions.onSecondaryEnabled)
                actions.onSecondaryEnabled(row->checked);
            if (auto* type = findRow(kIdSecondaryType))
                type->enabled = row->checked && state.secondaryAvailability == 0;
            break;
        case kIdSecondaryType:
            if (actions.onPreferRomanization)
                actions.onPreferRomanization(row->selected == 1);
            break;
        case kIdQqLocalLyricsEnabled:
            row->checked = !row->checked;
            if (auto* persist = findRow(kIdQqLocalLyricsPersistOrder))
                persist->enabled = row->checked;
            if (auto* path = findRow(kIdQqLocalLyricsPath))
                path->enabled = row->checked;
            if (actions.onQqLocalLyricsEnabled)
                actions.onQqLocalLyricsEnabled(row->checked);
            break;
        case kIdQqLocalLyricsPersistOrder:
            row->checked = !row->checked;
            if (actions.onQqLocalLyricsPersistOrder)
                actions.onQqLocalLyricsPersistOrder(row->checked);
            break;
        case kIdQqLocalLyricsPath:
            if (actions.onPickQqLocalLyricsPath)
                actions.onPickQqLocalLyricsPath();
            break;
        }
        if (hwnd)
            surface.invalidate();
    }

    void updateState(const SettingsState& s) {
        state = s;
        if (auto* row = findRow(kIdIdleEntry))
            row->checked = s.idleEntryEnabled;
        if (auto* row = findRow(kIdIdleQuote))
            row->checked = s.idleQuoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteSource))
            row->selected = std::clamp(s.idleQuoteSource, 0, 1);
        if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
            row->selected = std::clamp(s.idleQuoteRefreshInterval, 0, 2);
        if (auto* row = findRow(kIdIdleQuoteAlignment))
            row->selected = std::clamp(s.idleQuoteAlignment, 0, 2);
        if (auto* row = findRow(kIdIdleQuoteBackground))
            row->selected = std::clamp(s.idleQuoteBackground, 0, 4);
        if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
            row->selected = std::clamp(s.idleQuoteBackgroundScope, 0, 3);
        if (auto* row = findRow(kIdIdleApps))
            row->checked = s.idleAppNamesVisible;
        updateIdleQuoteSourceRow();
        updateIdleAppsRowHeight();
        const bool minimal = s.renderMode == kRenderModeMinimal;
        if (auto* row = findRow(kIdSongInfo))
            row->checked = s.songInfoVisible;
        if (auto* row = findRow(kIdAlbumCover))
            row->checked = s.albumCoverVisible;
        if (auto* row = findRow(kIdPlatformIcon)) {
            row->checked = s.platformIconVisible;
            row->enabled = s.albumCoverVisible;
        }
        if (auto* row = findRow(kIdCoverEffect)) {
            row->selected = minimal ? 0 : (s.coverEffectVinyl ? 1 : 0);
            row->enabled = s.albumCoverVisible && !minimal;
        }
        if (auto* row = findRow(kIdSpectrum)) {
            row->checked = minimal ? false : s.spectrumOn;
            row->enabled = !minimal;
        }
        if (auto* row = findRow(kIdSpectrumStyle)) {
            row->selected = std::clamp(s.spectrumStyle, 0, 2);
            row->enabled = s.spectrumOn && !minimal;
        }
        if (auto* row = findRow(kIdSpectrumBackground)) {
            row->checked = minimal ? false : s.spectrumBackground;
            row->enabled = s.spectrumOn && s.spectrumStyle == 2 && !minimal;
        }
        if (auto* row = findRow(kIdSpectrumOpacity)) {
            row->value = std::clamp(s.spectrumOpacity, 0, 100);
            row->enabled = s.spectrumOn && s.spectrumStyle == 2 && s.spectrumBackground &&
                           !minimal;
        }
        if (auto* row = findRow(kIdTaskbarBackground)) {
            row->selected = minimal ? 0 : std::clamp(s.taskbarBackground, 0, 2);
            row->enabled = !minimal;
        }
        if (auto* row = findRow(kIdCoverBackgroundOpacity)) {
            row->value = std::clamp(s.coverBackgroundOpacity, 0, 100);
            row->enabled = !minimal && s.taskbarBackground == 1;
        }
        if (auto* row = findRow(kIdProgressBackground))
            row->checked = minimal ? false : s.progressBackground;
        if (auto* row = findRow(kIdProgressBackgroundOpacity))
            row->value = std::clamp(s.progressBackgroundOpacity, 0, 100);
        updateProgressBackgroundRowsEnabled();
        if (auto* row = findRow(kIdHoverControls))
            row->checked = s.hoverControls;
        if (auto* row = findRow(kIdHoverControlStyle)) {
            row->selected = minimal ? 0 : s.hoverControlStyle;
            row->enabled = s.hoverControls && !minimal;
        }
        if (auto* row = findRow(kIdFloatingCardTrigger))
            row->selected = std::clamp(s.floatingCardTrigger, 0, 1);
        if (auto* row = findRow(kIdFloatingCardBackground))
            row->selected = std::clamp(s.floatingCardBackground, 0, 1);
        if (auto* row = findRow(kIdFloatingCardBackgroundColor))
            row->controlText = colorText(s.floatingCardBackgroundColor);
        if (auto* row = findRow(kIdFloatingCardFollowAlbum))
            row->checked = s.floatingCardFollowAlbum;
        if (auto* row = findRow(kIdFloatingCardAutoTextContrast))
            row->checked = s.floatingCardAutoTextContrast;
        updateFloatingCardRowsEnabled();
        updateIdleRowsEnabled();
        if (auto* row = findRow(kIdSongToast)) {
            row->checked = minimal ? false : s.songToastEnabled;
            row->enabled = !minimal;
        }
        if (auto* row = findRow(kIdSongToastDuration)) {
            row->value = std::clamp(s.songToastDurationSec, 1, 10);
            row->enabled = !minimal && s.songToastEnabled;
        }
        if (auto* row = findRow(kIdSongToastSkipFullscreen)) {
            row->checked = s.songToastSkipFullscreen;
            row->enabled = !minimal && s.songToastEnabled;
        }
        if (auto* row = findRow(kIdSongToastPosition)) {
            row->selected = std::clamp(s.songToastPosition, 0, 1);
            row->enabled = !minimal && s.songToastEnabled;
        }
        if (auto* row = findRow(kIdTaskbarTheme))
            row->selected = themeModeIndex(s.taskbarThemeMode);
        if (auto* row = findRow(kIdWindowTheme))
            row->selected = themeModeIndex(s.windowThemeMode);
        if (auto* row = findRow(kIdRenderMode))
            row->selected = std::clamp(s.renderMode, 0, kRenderModeMinimal);
        if (auto* row = findRow(kIdPickFont)) {
            row->valueText = s.fontDesc;
            row->hint = kFontSettingNotice;
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
        }
        if (auto* row = findRow(kIdFollowAlbum))
            row->checked = s.followAlbum;
        if (auto* row = findRow(kIdDoubleLine))
            row->checked = s.doubleLineLyrics;
        if (auto* row = findRow(kIdAlignment))
            row->selected = s.lyricAlignment;
        if (auto* row = findRow(kIdSecondaryOn))
            row->checked = s.secondaryEnabled;
        if (auto* row = findRow(kIdSecondaryType)) {
            row->hint = s.secondaryAvailability == 1
                            ? L"正在检查翻译和罗马音…"
                            : s.secondaryAvailability == 2 ? L"当前歌曲无翻译或罗马音" : L"";
            row->showHint = !row->hint.empty();
            row->minHeight = row->showHint ? kRowTallH : kRowH;
            row->height = row->minHeight;
            row->selected = s.preferRomanization ? 1 : 0;
            row->enabled = s.secondaryEnabled && s.secondaryAvailability == 0;
        }
        if (auto* row = findRow(kIdQqLocalLyricsEnabled))
            row->checked = s.qqLocalLyricsEnabled;
        if (auto* row = findRow(kIdQqLocalLyricsPersistOrder)) {
            row->checked = s.qqLocalLyricsPersistOrder;
            row->enabled = s.qqLocalLyricsEnabled;
        }
        if (auto* row = findRow(kIdQqLocalLyricsPath)) {
            row->hint = s.qqLocalLyricsPath.empty() ? std::wstring(L"未配置")
                                                    : s.qqLocalLyricsPath;
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
            row->enabled = s.qqLocalLyricsEnabled;
        }
        layout();
        surface.invalidate();
    }

    void updateFontDescription(const std::wstring& description) {
        state.fontDesc = description;
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
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd, false);
            surface.initialize(hwnd, backdrop);
            createControls();
            layout();
            return 0;
        case WM_SIZE:
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
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop, false);
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
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            const float s = surface.dipScale();
            if (scrollDragging) {
                scrollFromPointer(GET_Y_LPARAM(lp) / s);
                if (hoverId != kIdContentScrollBar || hoverOption != -1) {
                    hoverId = kIdContentScrollBar;
                    hoverOption = -1;
                    surface.invalidate();
                }
                return 0;
            }
            if (pressedId != 0) {
                Row* row = findRow(pressedId);
                if (row && row->kind == ControlKind::Slider && row->enabled) {
                    updateSliderFromPointer(*row, GET_X_LPARAM(lp) / s);
                    return 0;
                }
            }
            int option = -1;
            const int id = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
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
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            int option = -1;
            pressedId = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
            pressedOption = option;
            if (pressedId != 0) {
                Row* row = findRow(pressedId);
                if (row && row->kind == ControlKind::Slider && row->enabled)
                    updateSliderFromPointer(*row, GET_X_LPARAM(lp) / s);
            }
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
            const float s = surface.dipScale();
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
            else if (pressed != 0 && pressed == hit) {
                Row* row = findRow(pressed);
                if (row && row->enabled) {
                    if (row->kind == ControlKind::Radio || row->kind == ControlKind::ModeGrid) {
                        if (pressedOptionValue >= 0 && pressedOptionValue == option &&
                            pressedOptionValue != row->selected) {
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
            pressedId = 0;
            pressedOption = -1;
            scrollDragging = false;
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
                         kScrollWheelDip);
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
            if (wp == VK_UP || wp == VK_DOWN) {
                if (focusedId == kIdNav) {
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
                        next = (row->selected + direction +
                                static_cast<int>(row->options.size())) %
                               static_cast<int>(row->options.size());
                    }
                    if (next != row->selected) {
                        row->selected = next;
                        onCommand(row->id);
                    }
                }
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                if (focusedId == kIdIdleAppNames) {
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
    impl_->state = state;
    impl_->actions = std::move(actions);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
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
    if (impl_->hwnd)
        app_icon::applyWindowIcon(impl_->hwnd);
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
