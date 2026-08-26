#include "settings_dialog.h"

#include "ui/app_icon.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

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
constexpr int kIdMediaPopupTrigger = 437;
constexpr int kIdMediaPopupBackground = 418;
constexpr int kIdMediaPopupFollowAlbum = 426;
constexpr int kIdMediaPopupAutoTextContrast = 427;
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
constexpr int kIdContentScrollBar = 401;

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
constexpr float kScrollBarWidth = 3.0f;
constexpr float kScrollBarHitWidth = 12.0f;
constexpr float kScrollBarInset = 8.0f;
constexpr float kScrollBarOutsideGap = 8.0f;
constexpr float kScrollMinThumbHeight = 24.0f;
constexpr float kScrollWheelDip = 72.0f;

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

} // namespace

struct SettingsDialog::Impl {
    enum class ControlKind {
        Toggle,
        Radio,
        ModeGrid, // 2×2 模式选择卡片
        Slider,
        Button,
        Header, // 分组标题：无卡片背景，不参与交互与焦点
    };

    struct Row {
        int id = 0;
        ControlKind kind = ControlKind::Toggle;
        std::wstring text;
        std::wstring hint;
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
        D2D1_RECT_F cardRect{};
        D2D1_RECT_F labelRect{};
        D2D1_RECT_F hintRect{};
        D2D1_RECT_F controlRect{};
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
    std::array<std::wstring, 4> navItems{L"显示", L"频谱", L"字体与颜色", L"歌词"};
    std::array<std::wstring, 4> pageTitles{L"显示", L"频谱", L"字体与颜色", L"歌词"};
    std::vector<Row> rows[4];
    D2D1_RECT_F navRect{};
    std::array<D2D1_RECT_F, 4> navItemRects{};
    std::array<D2D1_RECT_F, 4> pageTitleRects{};
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

    void updateMediaPopupBackgroundRowsEnabled() {
        const auto* controls = findRow(kIdHoverControls);
        const auto* style = findRow(kIdHoverControlStyle);
        const bool popupEnabled = !minimalModeActive() && controls && controls->checked && style &&
                                  style->selected == 1;
        auto* background = findRow(kIdMediaPopupBackground);
        if (background)
            background->enabled = popupEnabled;
        if (auto* trigger = findRow(kIdMediaPopupTrigger))
            trigger->enabled = popupEnabled;
        if (auto* followAlbum = findRow(kIdMediaPopupFollowAlbum))
            followAlbum->enabled = popupEnabled && background && background->selected == 1;
        if (auto* autoTextContrast = findRow(kIdMediaPopupAutoTextContrast))
            autoTextContrast->enabled = popupEnabled && background && background->selected == 1;
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
        addRadio(0, kIdTaskbarTheme, L"任务栏歌词主题",
             L"系统表示跟随Windows系统/全局深浅色，应用表示跟随自定义应用深浅色模式",
                 {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.taskbarThemeMode),
                 true, kRowTallH);
        addRadio(0, kIdWindowTheme, L"对话框与悬浮窗主题",
             L"系统表示跟随Windows系统/全局深浅色，应用表示跟随自定义应用深浅色模式",
                 {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.windowThemeMode),
                 true, kRowTallH);
        addHeader(0, L"任务栏歌词");
        addToggle(0, kIdSongInfo, L"显示歌曲信息", state.songInfoVisible);
        addToggle(0, kIdAlbumCover, L"显示专辑封面", state.albumCoverVisible);
        Row& platformIcon = addToggle(0, kIdPlatformIcon, L"显示平台图标",
                                       state.platformIconVisible);
        platformIcon.enabled = state.albumCoverVisible;
        addRadio(0, kIdCoverEffect, L"专辑封面效果", nullptr, {L"默认", L"黑胶唱片"},
                 minimal ? 0 : (state.coverEffectVinyl ? 1 : 0),
                 state.albumCoverVisible && !minimal, kRowH);
        Row& progressBackground = addRow(
            0, kIdProgressBackground, ControlKind::Toggle, L"播放进度背景",
            L"从窗口左缘到歌词右缘，按播放进度填充专辑主题色；与频谱的背景波浪互斥",
            40.0f, kRowTallH);
        progressBackground.checked = minimal ? false : state.progressBackground;
        addSlider(0, kIdProgressBackgroundOpacity, L"进度背景不透明度",
                  state.progressBackgroundOpacity, !minimal && state.progressBackground);
        addRadio(0, kIdTaskbarBackground, L"任务栏歌词背景",
             L"封面模糊将专辑封面高斯模糊后铺满背景；纯色跟随任务栏深浅色；画在最底层，可与播放进度背景、背景波浪叠加",
                 {L"无", L"封面模糊", L"纯色"}, minimal ? 0 : state.taskbarBackground,
                 !minimal, kRowTallH);
        addSlider(0, kIdCoverBackgroundOpacity, L"封面背景不透明度",
                  state.coverBackgroundOpacity,
                  !minimal && state.taskbarBackground == 1);
        addHeader(0, L"悬浮控件与媒体卡片");
        addToggle(0, kIdHoverControls, L"悬浮时显示播放控件", state.hoverControls);
        addRadio(0, kIdHoverControlStyle, L"悬浮控件样式",
                 L"内嵌控件：在歌词和频谱上悬浮显示上一首、播放和下一首，没有多余信息；媒体卡片额外支持显示歌词进度信息，并且支持点击软件图标或者软件名称快速打开音乐软件",
                 {L"内嵌控件", L"媒体卡片"}, minimal ? 0 : state.hoverControlStyle,
                 state.hoverControls && !minimal,
                 kRowTallH);
        addRadio(0, kIdMediaPopupTrigger, L"媒体卡片展开方式",
                 L"悬浮展开：鼠标在歌词区域停留片刻后展开；点击展开：点击歌词区域任意位置立即展开",
                 {L"悬浮展开", L"点击展开"}, state.mediaPopupTrigger,
                 state.hoverControls && state.hoverControlStyle == 1, kRowTallH);
        addRadio(0, kIdMediaPopupBackground, L"媒体卡片背景",
                 L"纯色保持当前外观；磨砂玻璃使用 Windows 系统背景材质",
                 {L"纯色", L"磨砂玻璃"}, state.mediaPopupBackground,
                 state.hoverControls && state.hoverControlStyle == 1, kRowTallH);
        Row& followAlbum = addRow(
            0, kIdMediaPopupFollowAlbum, ControlKind::Toggle, L"磨砂背景跟随专辑",
            L"根据专辑主题颜色实时改变磨砂玻璃颜色", 40.0f,
            kRowTallH);
        followAlbum.checked = state.mediaPopupFollowAlbum;
        Row& autoTextContrast = addRow(
            0, kIdMediaPopupAutoTextContrast, ControlKind::Toggle,
            L"磨砂媒体卡片字体颜色动态变化",
            L"根据卡片背后应用的明暗，自动切换黑色或白色文字以提高可读性", 40.0f,
            kRowTallH);
        autoTextContrast.checked = state.mediaPopupAutoTextContrast;
        updateMediaPopupBackgroundRowsEnabled();
        addHeader(0, L"切歌弹窗");
        Row& songToast = addRow(0, kIdSongToast, ControlKind::Toggle, L"切歌时弹出歌曲信息",
                                L"在主屏幕中下方短暂弹出封面、标题和艺术家；弹窗磨砂半透明，"
                                L"始终不响应鼠标操作",
                                40.0f, kRowTallH);
        songToast.checked = minimal ? false : state.songToastEnabled;
        songToast.enabled = !minimal;
        Row& songToastDuration =
            addSlider(0, kIdSongToastDuration, L"切歌弹窗显示时长",
                      state.songToastDurationSec, !minimal && state.songToastEnabled);
        songToastDuration.minValue = 1;
        songToastDuration.maxValue = 10;
        songToastDuration.valueSuffix = L" 秒";
        Row& songToastSkipFullscreen =
            addToggle(0, kIdSongToastSkipFullscreen, L"全屏应用时关闭弹窗",
                      state.songToastSkipFullscreen);
        songToastSkipFullscreen.enabled = !minimal && state.songToastEnabled;
        addRadio(0, kIdSongToastPosition, L"切歌弹窗位置", nullptr, {L"中上", L"中下"},
                 state.songToastPosition, !minimal && state.songToastEnabled, kRowH);
        addHeader(0, L"性能");
        addModeGrid(0, kIdRenderMode, L"性能模式",
                    L"模式仅本次运行有效，重启软件后恢复正常模式",
                    {L"正常", L"低渲染", L"完全停止", L"极简"},
                    {L"完整视觉效果", L"降低帧率，节省 GPU", L"停止渲染，保留监听",
                     L"横向歌词，关闭逐字与转场"},
                    std::clamp(state.renderMode, 0, kRenderModeMinimal), true, kModeGridMinH);

        Row& spectrum = addToggle(1, kIdSpectrum, L"频谱", minimal ? false : state.spectrumOn);
        spectrum.enabled = !minimal;
        addRadio(1, kIdSpectrumStyle, L"频谱样式", nullptr,
                 {L"默认", L"柱状图", L"梦幻波浪"}, state.spectrumStyle,
                 state.spectrumOn && !minimal,
                 kRowH);
        Row& spectrumBackground =
            addToggle(1, kIdSpectrumBackground, L"背景波浪",
                      minimal ? false : state.spectrumBackground);
        spectrumBackground.enabled = state.spectrumOn && state.spectrumStyle == 2 && !minimal;
        addSlider(1, kIdSpectrumOpacity, L"背景波浪不透明度", state.spectrumOpacity,
                  state.spectrumOn && state.spectrumStyle == 2 && state.spectrumBackground &&
                      !minimal);
        // 频谱行创建完毕后才能按互斥关系刷新进度背景行的可用态
        updateProgressBackgroundRowsEnabled();

        addButton(2, kIdPickFont, L"字体", state.fontDesc.c_str(), L"选择字体…");
        addButton(2, kIdFontColor, L"字体颜色与效果", nullptr, L"打开…");
        addToggle(2, kIdFollowAlbum, L"已播放颜色跟随专辑", state.followAlbum);

        addToggle(3, kIdDoubleLine, L"双行歌词", state.doubleLineLyrics);
        addRadio(3, kIdAlignment, L"歌词对齐", nullptr, {L"左对齐", L"居中", L"右对齐"},
                 state.lyricAlignment, true, kRowH);
        addToggle(3, kIdSecondaryOn, L"开启翻译/罗马音", state.secondaryEnabled);
        const wchar_t* secondaryHint = state.secondaryAvailability == 1
                                            ? L"正在检查翻译和罗马音…"
                                            : state.secondaryAvailability == 2
                                                  ? L"当前歌曲无翻译或罗马音"
                                                  : L"";
        addRadio(3, kIdSecondaryType, L"辅助歌词类型", secondaryHint,
                 {L"翻译", L"罗马音"}, state.preferRomanization ? 1 : 0,
                 state.secondaryEnabled && state.secondaryAvailability == 0,
                 *secondaryHint ? kRowTallH : kRowH);
        addToggle(3, kIdQqLocalLyricsEnabled, L"使用 QQ 音乐本地歌词",
                  state.qqLocalLyricsEnabled);
        Row& persistOrder = addRow(3, kIdQqLocalLyricsPersistOrder, ControlKind::Toggle,
                                   L"切换版本持久化",
                                   L"记住每首歌切换后的本地/在线版本；关闭后不保存新记录，但仍读取已有记录",
                                   40.0f, kRowTallH);
        persistOrder.checked = state.qqLocalLyricsPersistOrder;
        persistOrder.enabled = state.qqLocalLyricsEnabled;
        const std::wstring localPathHint = state.qqLocalLyricsPath.empty()
                                                ? std::wstring(L"未配置")
                                                : state.qqLocalLyricsPath;
        Row& localPath = addButton(3, kIdQqLocalLyricsPath, L"QQ音乐本地歌词目录",
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
            float requiredHeight = row.minHeight;
            if (row.showHint && !row.hint.empty()) {
                const float hintHeight = measureHintHeight(painter, row);
                if (hintHeight > 0.0f)
                    requiredHeight = std::max(
                        requiredHeight, kTitleTopPadding + titleHeight + kTitleHintGap +
                                            hintHeight + kHintBottomPadding);
            } else if (measuredTitleHeight > 0.0f) {
                requiredHeight = std::max(requiredHeight, kTitleTopPadding + titleHeight +
                                                           kHintBottomPadding);
            }
            if (std::fabs(row.titleHeight - titleHeight) > 0.5f ||
                std::fabs(row.height - requiredHeight) > 0.5f) {
                row.titleHeight = titleHeight;
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
        for (int i = 0; i < 4; ++i) {
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
                row.hintRect = D2D1::RectF(0, 0, 0, 0);
                row.controlRect = D2D1::RectF(0, 0, 0, 0);
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
            if (row.showHint) {
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
        if (page < 0 || page >= 4 || page == activePage)
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
        for (int i = 0; i < 4; ++i) {
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
        painter.drawText(row.controlText, painter.textFormat(14.0f, 400, true, true),
                         D2D1::RectF(row.controlRect.left + 4.0f, row.controlRect.top,
                                     row.controlRect.right - 4.0f, row.controlRect.bottom),
                         textColor);
    }

    void drawToggle(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == row.id;
        const bool enabled = row.enabled;
        const bool focused = focusedId == row.id && focusVisible;
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

    void drawSlider(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = fluent::palette();
        const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        constexpr float valueW = 38.0f;
        constexpr float trackH = 4.0f;
        const float trackLeft = row.controlRect.left;
        const float trackRight = row.controlRect.right - valueW;
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
                         D2D1::RectF(trackRight + 6.0f, row.controlRect.top,
                                     row.controlRect.right, row.controlRect.bottom),
                         row.enabled ? p.textSecondary : p.disabled);
        if (focusedId == row.id && focusVisible && row.enabled)
            painter.strokeRoundRect(p.accent,
                                    D2D1::RectF(track.left - 3.0f, track.top - 5.0f,
                                                track.right + 3.0f, track.bottom + 5.0f),
                                    1.0f, 5.0f);
    }

    void drawRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
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
            if (row.showHint)
                painter.drawText(row.hint, painter.textFormat(12.0f, 400), row.hintRect,
                                 row.enabled ? p.textSecondary : p.disabled);
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
        for (int i = 0; i < 4; ++i) {
            if (contains(navItemRects[i], x, y)) {
                if (option)
                    *option = i;
                return kIdNav;
            }
        }
        if (contains(scrollBarHitRect(), x, y))
            return kIdContentScrollBar;
        for (const auto& row : rows[activePage]) {
            if (!contains(row.controlRect, x, y))
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
            if (row.enabled)
                order.push_back(row.id);
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
        constexpr float valueW = 38.0f;
        const float left = row.controlRect.left;
        const float right = row.controlRect.right - valueW;
        const float ratio = std::clamp((x - left) / std::max(1.0f, right - left), 0.0f, 1.0f);
        const int value = row.minValue + static_cast<int>(std::lround(
                                                   ratio * (row.maxValue - row.minValue)));
        if (value == row.value)
            return;
        row.value = std::clamp(value, row.minValue, row.maxValue);
        onCommand(row.id);
        surface.invalidate();
    }

    void onCommand(int id) {
        if (id == kIdNav)
            return;
        Row* row = findRow(id);
        if (!row || !row->enabled)
            return;
        switch (id) {
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
            updateMediaPopupBackgroundRowsEnabled();
            break;
        case kIdHoverControlStyle:
            if (actions.onHoverControlStyle)
                actions.onHoverControlStyle(row->selected);
            updateMediaPopupBackgroundRowsEnabled();
            break;
        case kIdMediaPopupTrigger:
            if (actions.onMediaPopupTrigger)
                actions.onMediaPopupTrigger(row->selected);
            break;
        case kIdMediaPopupBackground:
            if (actions.onMediaPopupBackground)
                actions.onMediaPopupBackground(row->selected);
            updateMediaPopupBackgroundRowsEnabled();
            break;
        case kIdMediaPopupFollowAlbum:
            row->checked = !row->checked;
            if (actions.onMediaPopupFollowAlbum)
                actions.onMediaPopupFollowAlbum(row->checked);
            break;
        case kIdMediaPopupAutoTextContrast:
            row->checked = !row->checked;
            if (actions.onMediaPopupAutoTextContrast)
                actions.onMediaPopupAutoTextContrast(row->checked);
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
        if (auto* row = findRow(kIdMediaPopupBackground)) {
            row->selected = s.mediaPopupBackground;
        }
        if (auto* row = findRow(kIdMediaPopupTrigger))
            row->selected = s.mediaPopupTrigger;
        if (auto* row = findRow(kIdMediaPopupFollowAlbum))
            row->checked = s.mediaPopupFollowAlbum;
        if (auto* row = findRow(kIdMediaPopupAutoTextContrast))
            row->checked = s.mediaPopupAutoTextContrast;
        updateMediaPopupBackgroundRowsEnabled();
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
            row->hint = s.fontDesc;
            row->showHint = !row->hint.empty();
            row->minHeight = kRowH;
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
            row->hint = description;
            row->showHint = !description.empty();
            row->minHeight = kRowH;
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
                focusedId = pressedId;
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
                        next = 2;
                    if (next >= 4)
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
                if (focusedId != kIdNav) {
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
