#include "settings_dialog.h"

#include "app_info.h"
#include "ui/app_icon.h"
#include "ui/color_picker_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "ui/media_control_icons.h"
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
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kSettingsPageCount = 9;
constexpr int kTaskbarModePage = 1;
constexpr int kPerformancePage = 2;
constexpr int kFloatingCardPage = 3;
constexpr int kMediaPopupPage = 4;
constexpr int kSpectrumPage = 5;
constexpr int kLyricsPage = 6;
constexpr int kSongToastPage = 7;
constexpr int kIdlePage = kSettingsPageCount - 1;
constexpr int kIdNav = 400;
constexpr int kIdSettingsSearch = 4900;
constexpr int kIdOpenDocs = 4901;
constexpr int kIdOpenFeedback = 4902;
constexpr int kIdOpenAbout = 4903;
constexpr int kFooterItemCount = 3;
constexpr float kFooterItemH = 32.0f;
constexpr float kSearchPaletteHeaderH = 52.0f;
constexpr float kSearchPaletteFooterH = 32.0f;
constexpr float kSearchPaletteRowH = 56.0f;
constexpr float kSearchPaletteListPadY = 6.0f;
constexpr int kSearchPaletteMaxVisibleRows = 6;
constexpr float kSearchPaletteEmptyPromptH = 54.0f;
constexpr float kSearchPaletteNoResultH = 70.0f;
constexpr int kSearchPaletteHitNone = 0;
constexpr int kSearchPaletteHitClear = 1;
constexpr int kSearchPaletteHitClose = 2;
constexpr int kSearchPaletteHitRow = 3;
constexpr UINT_PTR kNavIndicatorTimer = 1;
constexpr UINT kNavIndicatorDurationMs = 180;
constexpr UINT kNavIndicatorFrameMs = USER_TIMER_MINIMUM;
constexpr UINT_PTR kContentScrollTimer = 2;
constexpr UINT_PTR kSearchLocateTimer = 3;
constexpr UINT_PTR kSearchPaletteAnimTimer = 4;
constexpr ULONGLONG kSearchLocateFadeMs = 800;
constexpr float kSearchPaletteOpenDurationMs = 165.0f;
constexpr float kSearchPaletteCloseDurationMs = 120.0f;
constexpr float kSearchPaletteOpenSlideDip = -10.0f;
constexpr float kSearchPaletteCloseSlideDip = -8.0f;
constexpr float kContentScrollFrequency = 28.0f;
constexpr int kRenderModeMinimal = 3;
constexpr int kIdSongInfo = 410;
constexpr int kIdAlbumCover = 411;
constexpr int kIdPlatformIcon = 412;
constexpr int kIdCoverEffect = 413;
constexpr int kIdSpectrum = 414;
constexpr int kIdSpectrumStyle = 419;
constexpr int kIdSpectrumOpacity = 423;
constexpr int kIdSpectrumBackground = 428;
constexpr int kIdSpectrumGradient = 429;
constexpr int kIdSpectrumEffectFirst = 486;
constexpr int kIdSpectrumColorMode = 450;
constexpr int kIdSpectrumColor = 452;
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
constexpr int kIdSongToastMonitor = 484;
constexpr int kIdSongToastPreview = 485;
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
constexpr int kIdIdleCustomWelcome = 461;
constexpr int kIdTickTickApiToken = 462;
constexpr int kIdTickTickConnect = 463;
constexpr int kIdTickTickRefresh = 464;
constexpr int kIdTickTickDisconnect = 465;
constexpr int kIdTickTickEnabled = 466;
constexpr int kIdTaskbarContextMenu = 467;
constexpr int kIdTaskbarViewMode = 468;
constexpr int kIdAppBarEdge = 469;
constexpr int kIdTaskbarMonitor = 470;
constexpr int kIdImmersiveBackgroundBlur = 471;
constexpr int kIdDockBackgroundBlur = 474;
constexpr int kIdEmbeddedResourceMonitor = 481;
constexpr int kIdDockResourceDownload = 482;
constexpr int kIdDockResourceUpload = 483;
constexpr int kIdDockResourceGpuUsage = 473;
constexpr int kIdDockResourceCpuFrequency = 477;
constexpr int kIdSliderValueEdit = 478;
constexpr int kIdImmersiveBackgroundAdjustment = 479;
constexpr int kIdDockBackgroundAdjustment = 480;
constexpr int kIdContentScrollBar = 401;
// 应用列表卡片内嵌开关的键盘焦点 ID，不对应独立设置行。
constexpr int kIdIdleAppNames = 460;
constexpr int kIdleAppNamesOption = -2;
constexpr int kIdleAppEditOptionBase = 1000;
constexpr int kIdleAppDragOptionBase = 2000;

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
constexpr float kRowH = 56.0f;
constexpr float kRowTallH = 72.0f;
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
constexpr float kModeGridNoteGap = 12.0f;
constexpr float kModeGridMinH = 196.0f;
constexpr float kSpectrumStyleCardH = 96.0f;
constexpr float kSpectrumStyleCardGap = 8.0f;
constexpr float kSpectrumStyleRowH = 252.0f;
constexpr float kSpectrumBackgroundArtworkMaxW = 144.0f;
constexpr float kSpectrumBackgroundArtworkMinW = 96.0f;
constexpr float kSpectrumBackgroundArtworkH = 34.0f;
constexpr float kSpectrumEffectArtworkH = 44.0f;
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
constexpr float kSliderValueW = 62.0f;
constexpr float kSliderValueGap = 10.0f;
constexpr float kSliderSuffixW = 16.0f;
constexpr UINT_PTR kSliderEditSubclassId = 1;
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
constexpr float kIdleAppItemH = 52.0f;
constexpr float kIdleAppsListTop = 76.0f;
constexpr float kIdleAppsBaseH = 128.0f;
constexpr float kIdleAppActionW = 58.0f;
constexpr float kIdleAppActionGap = 8.0f;
constexpr float kIdleAppInfoRightInset = 144.0f;
constexpr float kIdleAppNamesToggleW = 40.0f;
constexpr float kIdleAppNamesToggleLabelW = 56.0f;
constexpr float kIdleAppNamesToggleGap = 8.0f;
constexpr float kIdleAppDragThreshold = 5.0f;
constexpr float kIdleQuoteBackgroundCardH = 112.0f;
constexpr float kIdleQuoteBackgroundCardGap = 8.0f;
constexpr float kIdleQuoteBackgroundRowH = 324.0f;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

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
    L"字体修改不会影响到界面字体，只会影响到任务栏歌词、每日一言、歌曲信息、沉浸模式时间/日期和 Dock、内嵌模式资源监视";

const wchar_t* idleQuoteSourceHint(int source) {
    if (source == 1)
        return L"今日诗词会根据客户端网络 IP、时间等信息进行推荐，并在本地保存接口 Token。";
    if (source == 2)
        return L"通过一言的网易云分类获取每日一句";
    return L"默认使用一言获取每日一言。";
}

const wchar_t* taskbarViewModeHint(bool mediaSessionAlive, bool verticalTaskbar) {
    if (!mediaSessionAlive)
        return L"当前没有音乐播放，沉浸模式暂不可用；开始播放后即可选择。Dock 模式仍可使用。";
    if (verticalTaskbar)
        return L"侧边任务栏不支持沉浸模式；Dock 模式以独立顶栏或底栏占用桌面工作区。";
    return L"沉浸模式覆盖 Windows 任务栏；Dock 模式以独立顶栏或底栏占用桌面工作区。";
}

const wchar_t* backgroundAdjustmentHint(bool supported, int mode) {
    if (!supported)
        return L"背景模糊层初始化失败，请查看运行日志中的 dock-backdrop 信息。";
    return mode == 1
               ? L"0% 完全透明，100% 显示随 Windows 应用主题变化的纯底色；中间值为底色半透明。"
               : L"0% 完全透明，100% 完全模糊；中间值混合清晰桌面与模糊背景。";
}

const wchar_t* backgroundAdjustmentLabel(bool immersive, int mode) {
    if (immersive)
        return mode == 1 ? L"沉浸模式背景不透明度" : L"沉浸模式背景模糊程度";
    return mode == 1 ? L"背景不透明度" : L"背景模糊程度";
}

bool parseBoundedInteger(const std::wstring& text, int minValue, int maxValue, int& value) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(text[begin]))
        ++begin;
    while (end > begin && std::iswspace(text[end - 1]))
        --end;
    if (begin == end)
        return false;

    int parsed = 0;
    for (size_t i = begin; i < end; ++i) {
        const wchar_t ch = text[i];
        if (ch < L'0' || ch > L'9')
            return false;
        const int digit = static_cast<int>(ch - L'0');
        if (parsed > (INT_MAX - digit) / 10)
            parsed = INT_MAX;
        else
            parsed = parsed * 10 + digit;
    }
    value = std::clamp(parsed, minValue, maxValue);
    return true;
}

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

settings_icon::Kind iconForRow(int id) {
    switch (id) {
    case kIdTaskbarTheme:
    case kIdWindowTheme:
        return settings_icon::Kind::Theme;
    case kIdPickFont:
        return settings_icon::Kind::Font;
    case kIdSongInfo:
        return settings_icon::Kind::SongInfo;
    case kIdAlbumCover:
        return settings_icon::Kind::AlbumCover;
    case kIdPlatformIcon:
        return settings_icon::Kind::Platform;
    case kIdCoverEffect:
        return settings_icon::Kind::Vinyl;
    case kIdProgressBackground:
        return settings_icon::Kind::Progress;
    case kIdProgressBackgroundOpacity:
    case kIdCoverBackgroundOpacity:
        return settings_icon::Kind::Opacity;
    case kIdTaskbarBackground:
        return settings_icon::Kind::Background;
    case kIdIdleQuoteBackground:
        return settings_icon::Kind::DynamicBackground;
    case kIdIdleQuoteBackgroundScope:
        return settings_icon::Kind::Scope;
    case kIdRenderMode:
        return settings_icon::Kind::RenderMode;
    case kIdTaskbarContextMenu:
    case kIdTaskbarViewMode:
    case kIdTaskbarMonitor:
        return settings_icon::Kind::Display;
    case kIdAppBarEdge:
        return settings_icon::Kind::Position;
    case kIdImmersiveBackgroundBlur:
    case kIdDockBackgroundBlur:
    case kIdImmersiveBackgroundAdjustment:
    case kIdDockBackgroundAdjustment:
        return settings_icon::Kind::Background;
    case kIdEmbeddedResourceMonitor:
    case kIdDockResourceGpuUsage:
    case kIdDockResourceDownload:
    case kIdDockResourceUpload:
    case kIdDockResourceCpuFrequency:
        return settings_icon::Kind::Performance;
    case kIdHoverControls:
        return settings_icon::Kind::Hover;
    case kIdHoverControlStyle:
        return settings_icon::Kind::Control;
    case kIdFloatingCardTrigger:
        return settings_icon::Kind::Trigger;
    case kIdFloatingCardBackground:
        return settings_icon::Kind::Background;
    case kIdFloatingCardBackgroundColor:
    case kIdSpectrumColorMode:
    case kIdSpectrumColor:
    case kIdFontColor:
        return settings_icon::Kind::Color;
    case kIdFloatingCardFollowAlbum:
    case kIdFollowAlbum:
        return settings_icon::Kind::Follow;
    case kIdFloatingCardAutoTextContrast:
        return settings_icon::Kind::Contrast;
    case kIdSpectrum:
    case kIdSpectrumStyle:
        return settings_icon::Kind::Spectrum;
    case kIdSpectrumEffectFirst:
    case kIdSpectrumEffectFirst + 1:
    case kIdSpectrumEffectFirst + 2:
    case kIdSpectrumEffectFirst + 3:
    case kIdSpectrumGradient:
        return settings_icon::Kind::Gradient;
    case kIdSpectrumBackground:
        return settings_icon::Kind::Wave;
    case kIdSpectrumOpacity:
        return settings_icon::Kind::Opacity;
    case kIdSongToast:
    case kIdSongToastPreview:
        return settings_icon::Kind::Toast;
    case kIdSongToastMonitor:
        return settings_icon::Kind::Display;
    case kIdSongToastDuration:
        return settings_icon::Kind::Duration;
    case kIdSongToastSkipFullscreen:
        return settings_icon::Kind::Fullscreen;
    case kIdSongToastPosition:
        return settings_icon::Kind::Position;
    case kIdIdleEntry:
        return settings_icon::Kind::Idle;
    case kIdIdleQuote:
    case kIdIdleCustomWelcome:
        return settings_icon::Kind::Quote;
    case kIdIdleQuoteSource:
        return settings_icon::Kind::Language;
    case kIdIdleQuoteRefreshInterval:
        return settings_icon::Kind::Refresh;
    case kIdIdleApps:
        return settings_icon::Kind::Apps;
    case kIdTickTickApiToken:
        return settings_icon::Kind::Api;
    case kIdTickTickConnect:
        return settings_icon::Kind::Connect;
    case kIdTickTickRefresh:
        return settings_icon::Kind::Sync;
    case kIdTickTickDisconnect:
        return settings_icon::Kind::Disconnect;
    case kIdTickTickEnabled:
        return settings_icon::Kind::Apps;
    case kIdDoubleLine:
    case kIdSecondaryOn:
        return settings_icon::Kind::DoubleLine;
    case kIdAlignment:
    case kIdIdleQuoteAlignment:
        return settings_icon::Kind::Alignment;
    case kIdSecondaryType:
        return settings_icon::Kind::Language;
    case kIdQqLocalLyricsEnabled:
        return settings_icon::Kind::LocalLyrics;
    case kIdQqLocalLyricsPath:
        return settings_icon::Kind::Folder;
    case kIdQqLocalLyricsPersistOrder:
        return settings_icon::Kind::Persist;
    default:
        return settings_icon::Kind::None;
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
        std::vector<bool> optionEnabled;
        bool showHint = false;
        bool choiceInline = false;
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
        float gapAfter = 0.0f;
        float titleHeight = kTitleMinHeight;
        float valueHeight = 0.0f;
        D2D1_RECT_F cardRect{};
        D2D1_RECT_F iconRect{};
        D2D1_RECT_F labelRect{};
        D2D1_RECT_F valueRect{};
        D2D1_RECT_F hintRect{};
        D2D1_RECT_F controlRect{};
        D2D1_RECT_F artworkRect{};
        D2D1_RECT_F addAppRect{};
        std::vector<D2D1_RECT_F> appItemRects;
        std::vector<D2D1_RECT_F> appEditRects;
        std::vector<D2D1_RECT_F> appDeleteRects;
        std::vector<D2D1_RECT_F> optionRects;
        std::vector<std::wstring> optionHints;
    };

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr;
    HWND contentWindow = nullptr;
    DCompRenderer contentRenderer;
    bool contentLayerReady = false;
    bool backdrop = false;
    SettingsState state;
    SettingsActions actions;
    int activePage = 0;

    fluent::FluentDialogSurface surface;
    fluent::FluentEdit sliderValueEdit;
    fluent::FluentEdit settingsSearch;
    struct SearchResult {
        int page;
        int id;
        std::wstring path;
    };
    std::wstring searchQuery;
    std::vector<SearchResult> searchResults;
    int selectedSearchResult = -1;
    int searchLocatedId = 0;
    ULONGLONG searchLocatedStartMs = 0;
    bool searchLocateFading = false;
    POINT searchLocateInitialCursorPt{LONG_MIN, LONG_MIN};
    D2D1_RECT_F searchTriggerRect{};
    HWND searchScrimHwnd = nullptr;
    LyricRenderer searchScrimRenderer;
    int searchScrimLastWidth = 0;
    int searchScrimLastHeight = 0;
    UINT searchScrimLastDpi = 0;
    bool searchScrimLastDark = false;
    bool searchScrimLastZoomed = false;
    HWND searchPaletteHwnd = nullptr;
    fluent::FluentDialogSurface searchPaletteSurface;
    bool searchPaletteOpen = false;
    bool closingSearchPalette = false;
    bool suppressNextSearchTriggerPress = false;
    bool searchPaletteAnimating = false;
    ULONGLONG searchPaletteAnimStartMs = 0;
    float searchPaletteAnimDurationMs = kSearchPaletteOpenDurationMs;
    float searchPaletteAnimFrom = 0.0f;
    float searchPaletteAnimTarget = 0.0f;
    float searchPaletteProgress = 0.0f;
    int searchPaletteBaseX = 0;
    int searchPaletteBaseY = 0;
    float searchPaletteScale = 1.0f;
    float searchPaletteScroll = 0.0f;
    float searchPaletteMaxScroll = 0.0f;
    int searchPaletteHoverKind = kSearchPaletteHitNone;
    int searchPaletteHoverRow = -1;
    int searchPalettePressedKind = kSearchPaletteHitNone;
    int searchPalettePressedRow = -1;
    POINT searchPaletteLastMovePt{-1, -1};
    D2D1_RECT_F searchPaletteClearRect{};
    D2D1_RECT_F searchPaletteEscRect{};
    D2D1_RECT_F searchPaletteListRect{};
    int sliderEditRowId = 0;
    bool sliderEditOpen = false;
    bool sliderEditCommitting = false;
    std::unique_ptr<ColorPickerDialog> colorPicker;
    std::array<std::wstring, kSettingsPageCount> navItems{
        L"显示", L"展示模式", L"性能", L"悬浮卡片", L"媒体控件", L"频谱", L"歌词",
        L"切歌弹窗", L"每日一言与更多"};
    std::array<std::wstring, kSettingsPageCount> pageTitles{
        L"显示", L"展示模式", L"性能", L"悬浮卡片", L"媒体控件", L"频谱", L"歌词",
        L"切歌弹窗", L"每日一言与更多"};
    std::vector<Row> rows[kSettingsPageCount];
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

    static LRESULT CALLBACK sliderEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR subclassId, DWORD_PTR refData) {
        auto* self = reinterpret_cast<Impl*>(refData);
        if (msg == WM_NCDESTROY) {
            RemoveWindowSubclass(h, &Impl::sliderEditProc, subclassId);
            return DefSubclassProc(h, msg, wp, lp);
        }
        if (self && msg == WM_KEYDOWN) {
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                self->openSearchPalette();
                return 0;
            }
            if (wp == VK_RETURN) {
                self->commitSliderEditor();
                SetFocus(self->hwnd);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                self->cancelSliderEditor();
                SetFocus(self->hwnd);
                return 0;
            }
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    static bool isIdleAppDragOption(int option) {
        return option >= kIdleAppDragOptionBase;
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
        const size_t count = state.idleApps.size();
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

    void hideSliderEditor() {
        sliderEditOpen = false;
        sliderEditRowId = 0;
        if (sliderValueEdit.hwnd())
            ShowWindow(sliderValueEdit.hwnd(), SW_HIDE);
        if (hwnd)
            surface.invalidate();
    }

    void cancelSliderEditor() {
        if (!sliderEditOpen)
            return;
        if (const Row* row = findRow(sliderEditRowId))
            sliderValueEdit.setText(std::to_wstring(row->value));
        hideSliderEditor();
    }

    void commitSliderEditor() {
        if (!sliderEditOpen || sliderEditCommitting)
            return;
        sliderEditCommitting = true;
        Row* row = findRow(sliderEditRowId);
        if (row && row->enabled) {
            int value = row->value;
            if (parseBoundedInteger(sliderValueEdit.text(), row->minValue, row->maxValue,
                                    value)) {
                if (value != row->value) {
                    row->value = value;
                    onCommand(row->id);
                }
            } else {
                // 空值或非数字不进入设置，恢复编辑前的有效值。
                sliderValueEdit.setText(std::to_wstring(row->value));
            }
        }
        hideSliderEditor();
        sliderEditCommitting = false;
    }

    void layoutSliderEditor() {
        if (!sliderEditOpen)
            return;
        Row* row = findRow(sliderEditRowId);
        if (!row || row->kind != ControlKind::Slider || !row->enabled ||
            row->cardRect.right <= row->cardRect.left ||
            row->cardRect.bottom <= contentViewportRect.top ||
            row->cardRect.top >= contentViewportRect.bottom) {
            hideSliderEditor();
            return;
        }

        const D2D1_RECT_F valueRect = sliderNumberRect(*row);
        const float s = surface.dipScale();
        const int x = static_cast<int>(std::lround(valueRect.left * s));
        const int y = static_cast<int>(std::lround(valueRect.top * s));
        const int w = static_cast<int>(std::lround((valueRect.right - valueRect.left) * s));
        const int h = static_cast<int>(std::lround((valueRect.bottom - valueRect.top) * s));
        sliderValueEdit.move(x, y, std::max(1, w), std::max(1, h));
    }

    void openSliderEditor(Row& row) {
        if (!row.enabled || !sliderValueEdit.editHwnd())
            return;
        if (sliderEditOpen && sliderEditRowId != row.id)
            commitSliderEditor();
        sliderEditRowId = row.id;
        sliderEditOpen = true;
        sliderValueEdit.setText(std::to_wstring(row.value));
        layoutSliderEditor();
        sliderValueEdit.focus();
        SendMessageW(sliderValueEdit.editHwnd(), EM_SETSEL, 0, -1);
        surface.invalidate();
    }

    static bool isOptionEnabled(const Row& row, size_t option) {
        return option >= row.optionEnabled.size() || row.optionEnabled[option];
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

    void updateBackgroundAdjustmentPresentation() {
        auto update = [this](int modeId, int sliderId, bool immersive) {
            const auto* modeRow = findRow(modeId);
            const int selected = modeRow ? std::clamp(modeRow->selected, 0, 1) : 0;
            if (auto* slider = findRow(sliderId)) {
                slider->text = backgroundAdjustmentLabel(immersive, selected);
                slider->hint = backgroundAdjustmentHint(
                    state.backgroundBlurSupported, selected);
                slider->showHint = true;
                slider->minHeight = kRowTallH;
                slider->height = slider->minHeight;
            }
        };
        update(kIdImmersiveBackgroundAdjustment, kIdImmersiveBackgroundBlur, true);
        update(kIdDockBackgroundAdjustment, kIdDockBackgroundBlur, false);
    }

    void updateImmersiveRowsEnabled() {
        auto* mode = findRow(kIdTaskbarViewMode);
        const int selectedMode = mode ? std::clamp(mode->selected, 0, 2) : 0;
        const bool immersiveEnabled = state.mediaSessionAlive && !state.verticalTaskbar;
        const bool appBarEnabled = selectedMode == 2;
        const bool expandedEnabled = appBarEnabled ||
                                     (selectedMode == 1 && immersiveEnabled);
        if (mode) {
            mode->enabled = true;
            mode->optionEnabled.assign(mode->options.size(), true);
            if (mode->optionEnabled.size() > 1)
                mode->optionEnabled[1] = immersiveEnabled;
        }
        if (auto* row = findRow(kIdAppBarEdge))
            row->enabled = appBarEnabled;
        if (auto* row = findRow(kIdTaskbarMonitor))
            row->enabled = selectedMode != 0;
        if (auto* row = findRow(kIdImmersiveBackgroundAdjustment)) {
            row->selected = std::clamp(state.immersiveBackgroundAdjustment, 0, 1);
            row->enabled = selectedMode == 1 && immersiveEnabled &&
                           state.backgroundBlurSupported;
        }
        if (auto* row = findRow(kIdDockBackgroundAdjustment)) {
            row->selected = std::clamp(state.dockBackgroundAdjustment, 0, 1);
            row->enabled = appBarEnabled && state.backgroundBlurSupported;
        }
        if (auto* row = findRow(kIdImmersiveBackgroundBlur)) {
            row->enabled = selectedMode == 1 && immersiveEnabled &&
                           state.backgroundBlurSupported;
            row->hint = backgroundAdjustmentHint(
                state.backgroundBlurSupported, state.immersiveBackgroundAdjustment);
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
        }
        if (auto* row = findRow(kIdDockBackgroundBlur)) {
            row->enabled = appBarEnabled && state.backgroundBlurSupported;
            row->hint = backgroundAdjustmentHint(
                state.backgroundBlurSupported, state.dockBackgroundAdjustment);
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
        }
        updateBackgroundAdjustmentPresentation();
        const bool embeddedResources = selectedMode == 0 && !state.verticalTaskbar;
        if (auto* row = findRow(kIdEmbeddedResourceMonitor))
            row->enabled = embeddedResources;
        const bool resourceExtrasEnabled = appBarEnabled ||
                                           (embeddedResources && state.embeddedResourceMonitor);
        if (auto* row = findRow(kIdDockResourceDownload))
            row->enabled = resourceExtrasEnabled;
        if (auto* row = findRow(kIdDockResourceUpload))
            row->enabled = resourceExtrasEnabled;
        if (auto* row = findRow(kIdDockResourceGpuUsage))
            row->enabled = resourceExtrasEnabled;
        if (auto* row = findRow(kIdDockResourceCpuFrequency))
            row->enabled = resourceExtrasEnabled;
        auto* controls = findRow(kIdHoverControls);
        if (controls)
            controls->enabled = !expandedEnabled;
        if (auto* row = findRow(kIdHoverControlStyle))
            row->enabled = !expandedEnabled && controls && controls->checked &&
                           !minimalModeActive();
        if (auto* row = findRow(kIdAlignment))
            row->enabled = !expandedEnabled && !state.verticalTaskbar;
    }

    Row& addRadio(int page, int id, const wchar_t* text, const wchar_t* hint,
                  std::vector<std::wstring> options, int selected, bool enabled, float height) {
        Row& row = addRow(page, id, ControlKind::Radio, text, hint, 0.0f, height);
        row.options = std::move(options);
        row.optionEnabled.assign(row.options.size(), true);
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

    Row& addSlider(int page, int id, const wchar_t* text, int value, bool enabled,
                   const wchar_t* hint = nullptr, float height = kRowH) {
        Row& row = addRow(page, id, ControlKind::Slider, text, hint, 216.0f, height);
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

    void updateTickTickRows() {
        const bool hasToken = state.tickTickApiTokenConfigured;
        const bool enabled = state.tickTickEnabled && hasToken;
        if (auto* row = findRow(kIdTickTickEnabled)) {
            row->checked = enabled;
            row->enabled = true;
            row->hint = hasToken
                            ? (enabled
                                   ? L"已开启自动同步，并在快捷启动卡片中显示今日任务。"
                                   : L"已关闭自动同步和今日任务入口；API 口令仍保留在本机。")
                            : L"未配置 API 口令，当前处于隐式关闭状态；开启时会先要求填写口令。";
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
        }
        if (auto* row = findRow(kIdTickTickApiToken)) {
            row->hint = L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建并复制；"
                        L"程序只将口令保存到 Windows 凭据管理器。";
            row->controlText = state.tickTickApiTokenConfigured ? L"已配置" : L"未配置";
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
            row->enabled = !hasToken || enabled;
        }
        if (auto* row = findRow(kIdTickTickConnect)) {
            row->controlText = state.tickTickConnecting
                                   ? L"连接中…"
                                   : state.tickTickConnected ? L"重新连接" : L"连接…";
            row->hint = state.tickTickStatus.empty()
                            ? (state.tickTickApiTokenConfigured
                                   ? L"连接后，卡片会显示今天的未完成任务。"
                                   : L"请先填写 API 口令。")
                            : state.tickTickStatus;
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = row->minHeight;
            row->enabled = enabled && !state.tickTickConnecting &&
                           !state.tickTickSyncing;
        }
        if (auto* row = findRow(kIdTickTickRefresh)) {
            row->enabled = enabled && state.tickTickConnected && !state.tickTickSyncing;
            row->controlText = state.tickTickSyncing ? L"同步中…" : L"立即同步";
        }
        if (auto* row = findRow(kIdTickTickDisconnect)) {
            row->enabled = hasToken && !state.tickTickConnecting &&
                           !state.tickTickSyncing;
        }
    }

    void updateIdleQuoteSourceRow() {
        if (auto* row = findRow(kIdIdleQuoteSource)) {
            row->hint = idleQuoteSourceHint(state.idleQuoteSource);
            row->showHint = true;
            row->minHeight = kRowTallH;
            row->height = kRowTallH;
        }
    }

    int idleTextMode() const {
        return state.idleQuoteEnabled ? 0 : state.idleWelcomeEnabled ? 1 : 2;
    }

    bool embeddedResourceOnlyAvailable() const {
        return state.taskbarViewMode == 0 && state.embeddedResourceMonitor &&
               !state.verticalTaskbar;
    }

    void updateIdleRowsEnabled() {
        const bool quoteEnabled = state.idleQuoteEnabled;
        if (auto* row = findRow(kIdIdleQuote)) {
            row->selected = idleTextMode();
            row->optionEnabled.assign(row->options.size(), true);
            if (row->optionEnabled.size() > 2)
                row->optionEnabled[2] = embeddedResourceOnlyAvailable() || row->selected == 2;
        }
        if (auto* row = findRow(kIdIdleQuoteSource))
            row->enabled = quoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
            row->enabled = quoteEnabled;
        if (auto* row = findRow(kIdIdleQuoteAlignment))
            row->enabled = quoteEnabled && !state.verticalTaskbar;
        if (auto* row = findRow(kIdIdleCustomWelcome))
            row->enabled = state.idleWelcomeEnabled;
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
        } else if (id == kIdSpectrumColor) {
            initial = state.spectrumColor;
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
                state.floatingCardBackgroundColor = color;
                if (actions.onFloatingCardBackgroundColor)
                    actions.onFloatingCardBackgroundColor(color);
            } else if (id == kIdSpectrumColor) {
                state.spectrumColor = color;
                state.spectrumCustomColor = true;
                state.spectrumFollowAlbum = false;
                if (auto* mode = findRow(kIdSpectrumColorMode))
                    mode->selected = 2;
                if (actions.onSpectrumColor)
                    actions.onSpectrumColor(color);
            }
            if (auto* row = findRow(id))
                row->controlText = colorText(color);
            updateImmersiveRowsEnabled();
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
        for (auto& row : rows[activePage]) {
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
            if (sliderEditOpen)
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

    void createControls() {
        const bool minimal = state.renderMode == kRenderModeMinimal;
        const bool vertical = state.verticalTaskbar;
        addHeader(0, L"主题");
        addRadio(0, kIdTaskbarTheme, L"任务栏内容主题",
             L"“系统”和“应用”分别跟随 Windows 的系统模式与应用模式。",
                 {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.taskbarThemeMode),
                 true, kRowTallH);
        addRadio(0, kIdWindowTheme, L"对话框与悬浮窗主题",
             L"独立设置普通对话框与悬浮窗的深浅色，不影响任务栏内容。",
                 {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.windowThemeMode),
                 true, kRowTallH);
        addHeader(0, L"字体");
        Row& font = addButton(0, kIdPickFont, L"通用字体", kFontSettingNotice,
                              L"选择字体…", kRowTallH);
        font.valueText = state.fontDesc;
        addHeader(0, L"歌曲信息与封面");
        Row& songInfo = addToggle(0, kIdSongInfo, L"显示歌曲信息",
                                  vertical ? false : state.songInfoVisible);
        songInfo.enabled = !vertical;
        addToggle(0, kIdAlbumCover, L"显示专辑封面", state.albumCoverVisible);
        Row& platformIcon = addToggle(0, kIdPlatformIcon, L"显示平台图标",
                                       state.platformIconVisible);
        platformIcon.enabled = state.albumCoverVisible;
        addRadio(0, kIdCoverEffect, L"专辑封面效果", nullptr, {L"默认", L"黑胶唱片"},
                 minimal ? 0 : (state.coverEffectVinyl ? 1 : 0),
                 state.albumCoverVisible && !minimal, kCoverEffectRowH);
        addHeader(0, L"菜单交互");
        Row& taskbarContextMenu = addRow(
            0, kIdTaskbarContextMenu, ControlKind::Toggle, L"右键显示任务栏歌词菜单",
            L"关闭后，右键任务栏歌词不再弹出菜单；托盘图标的右键菜单不受影响。",
            40.0f, kRowTallH);
        taskbarContextMenu.checked = state.taskbarContextMenu;
        addHeader(kTaskbarModePage, L"展示模式");
        addRadio(kTaskbarModePage, kIdTaskbarViewMode, L"任务栏歌词展示模式",
                 taskbarViewModeHint(state.mediaSessionAlive, vertical),
                 {L"内嵌", L"沉浸", L"Dock 模式"}, std::clamp(state.taskbarViewMode, 0, 2),
                 true, kRowTallH);
        Row& taskbarMonitor = addButton(
            kTaskbarModePage, kIdTaskbarMonitor, L"目标显示器",
            L"内嵌模式固定显示在主显示器；沉浸模式覆盖所选任务栏，全部显示器时只覆盖横向任务栏。Dock 模式占用对应显示器的工作区；断开后切回主显示器。",
            state.taskbarMonitorLabel.c_str(), kRowTallH);
        taskbarMonitor.enabled = state.taskbarViewMode != 0;
        addRadio(kTaskbarModePage, kIdAppBarEdge, L"Dock 位置",
                 L"当前仅支持水平顶部和底部。", {L"顶部", L"底部"},
                 std::clamp(state.appBarEdge, 0, 1), state.taskbarViewMode == 2, kRowH);
        addHeader(kTaskbarModePage, L"沉浸模式背景");
        addRadio(kTaskbarModePage, kIdImmersiveBackgroundAdjustment, L"调整方式", nullptr,
                 {L"模糊程度", L"不透明度"},
                 std::clamp(state.immersiveBackgroundAdjustment, 0, 1),
                 !vertical && state.taskbarViewMode == 1 && state.backgroundBlurSupported,
                 kRowH);
        addSlider(kTaskbarModePage, kIdImmersiveBackgroundBlur,
                  backgroundAdjustmentLabel(true, state.immersiveBackgroundAdjustment),
                  state.immersiveBackgroundBlur,
                  !vertical && state.taskbarViewMode == 1 && state.backgroundBlurSupported,
                  backgroundAdjustmentHint(state.backgroundBlurSupported,
                                           state.immersiveBackgroundAdjustment),
                  kRowTallH);
        addHeader(kTaskbarModePage, L"Dock 背景");
        addRadio(kTaskbarModePage, kIdDockBackgroundAdjustment, L"调整方式", nullptr,
                 {L"模糊程度", L"不透明度"},
                 std::clamp(state.dockBackgroundAdjustment, 0, 1),
                 state.taskbarViewMode == 2 && state.backgroundBlurSupported, kRowH);
        addSlider(kTaskbarModePage, kIdDockBackgroundBlur,
                  backgroundAdjustmentLabel(false, state.dockBackgroundAdjustment),
                  state.dockBackgroundBlur,
                  state.taskbarViewMode == 2 && state.backgroundBlurSupported,
                  backgroundAdjustmentHint(state.backgroundBlurSupported,
                                           state.dockBackgroundAdjustment),
                  kRowTallH);
        addHeader(kTaskbarModePage, L"资源监视");
        Row& embeddedResources = addRow(
            kTaskbarModePage, kIdEmbeddedResourceMonitor, ControlKind::Toggle,
            L"内嵌模式显示资源监视", L"在任务栏右侧显示 CPU 和内存；扩展指标依次补位，超出一页可用滚轮切换。",
            40.0f, kRowTallH);
        embeddedResources.checked = state.embeddedResourceMonitor;
        embeddedResources.enabled = state.taskbarViewMode == 0 && !vertical;
        Row& download = addRow(
            kTaskbarModePage, kIdDockResourceDownload, ControlKind::Toggle,
            L"显示下行速度", L"扩展指标；关闭后由后续指标补位，没有其他指标时释放对应宽度。",
            40.0f, kRowTallH);
        download.checked = state.dockResourceDownload;
        Row& upload = addRow(
            kTaskbarModePage, kIdDockResourceUpload, ControlKind::Toggle,
            L"显示上行速度", L"扩展指标；关闭后由后续指标补位，没有其他指标时释放对应宽度。",
            40.0f, kRowTallH);
        upload.checked = state.dockResourceUpload;
        Row& dockResourceGpu = addRow(
            kTaskbarModePage, kIdDockResourceGpuUsage, ControlKind::Toggle,
            L"显示 GPU 使用率", L"示例：GPU: 7%；没有可用 GPU 性能计数器时显示“—”。",
            40.0f, kRowTallH);
        dockResourceGpu.checked = state.dockResourceGpuUsage;
        Row& dockResourceCpuFrequency = addRow(
            kTaskbarModePage, kIdDockResourceCpuFrequency, ControlKind::Toggle,
            L"显示 CPU 频率", L"示例：频率: 3.6GHz；系统未提供当前频率时显示“—”。",
            40.0f, kRowTallH);
        dockResourceCpuFrequency.checked = state.dockResourceCpuFrequency;
        const bool resourceExtrasEnabled = state.taskbarViewMode == 2 ||
            (state.taskbarViewMode == 0 && !vertical && state.embeddedResourceMonitor);
        if (auto* row = findRow(kIdDockResourceDownload))
            row->enabled = resourceExtrasEnabled;
        if (auto* row = findRow(kIdDockResourceUpload))
            row->enabled = resourceExtrasEnabled;
        if (auto* row = findRow(kIdDockResourceGpuUsage))
            row->enabled = resourceExtrasEnabled;
        if (auto* row = findRow(kIdDockResourceCpuFrequency))
            row->enabled = resourceExtrasEnabled;
        addHeader(kIdlePage, L"空闲内容");
        Row& idleEntry = addRow(
            kIdlePage, kIdIdleEntry, ControlKind::Toggle, L"无播放时保留任务栏入口",
            L"播放器未运行时，任务栏显示空闲内容；悬浮后可打开已配置的应用。"
            L"关闭后，播放器未运行时隐藏任务栏入口。",
            40.0f, kRowTallH);
        idleEntry.checked = state.idleEntryEnabled;
        addRadio(kIdlePage, kIdIdleQuote, L"空闲文字",
                 L"“不显示”需先开启内嵌模式资源监视；滴答清单仍会在启动时播报一次今日任务。",
                 {L"每日一言", L"欢迎语", L"不显示"}, idleTextMode(), true, kRowTallH);
        addButton(kIdlePage, kIdIdleCustomWelcome, L"自定义欢迎语",
                  L"选择欢迎语时显示自定义内容，最多 20 个字符且不能为纯空格；留空则恢复按时间和日期类型生成的默认欢迎语。",
                  L"编辑…", kRowTallH);
        addRadio(kIdlePage, kIdIdleQuoteSource, L"每日一言来源",
                 idleQuoteSourceHint(state.idleQuoteSource),
                 {L"一言", L"今日诗词", L"网易云热评"}, state.idleQuoteSource,
                 state.idleQuoteEnabled, kRowTallH);
        addRadio(kIdlePage, kIdIdleQuoteRefreshInterval, L"每日一言更新频率", nullptr,
                 {L"每天", L"每 12 小时", L"每小时"}, state.idleQuoteRefreshInterval,
                 state.idleQuoteEnabled, kRowTallH);
        addRadio(kIdlePage, kIdIdleQuoteAlignment, L"每日一言字体对齐方式",
                 L"仅影响任务栏和悬浮卡片中的每日一言，不影响正常歌词对齐。",
                 {L"左对齐", L"居中", L"右对齐"}, vertical ? 0 : state.idleQuoteAlignment,
                 state.idleQuoteEnabled && !vertical, kRowH);
        addHeader(kIdlePage, L"快捷启动");
        Row& idleApps = addRow(
            kIdlePage, kIdIdleApps, ControlKind::AppList, L"可打开的应用",
            L"可拖动应用条目调整快速启动顺序；通过文件选择器添加本地 EXE，最多添加 10 个应用；开关统一控制名称显示。",
            0.0f, kIdleAppsBaseH + static_cast<float>(
                                            std::max<size_t>(1, state.idleApps.size())) *
                                            kIdleAppItemH);
        idleApps.checked = state.idleAppNamesVisible;
        idleApps.height = idleApps.minHeight;
        idleApps.enabled = state.idleEntryEnabled;
        updateIdleRowsEnabled();
        addHeader(kIdlePage, L"滴答清单");
        Row& tickTickEnabled = addRow(
            kIdlePage, kIdTickTickEnabled, ControlKind::Toggle, L"启用滴答清单",
            L"开启后自动同步今日任务，并在快捷启动卡片中显示今日任务入口。",
            40.0f, kRowTallH);
        tickTickEnabled.checked = state.tickTickEnabled;
        addButton(kIdlePage, kIdTickTickApiToken, L"API 口令",
                  L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建后填写。",
                  state.tickTickApiTokenConfigured ? L"已配置" : L"编辑…", kRowTallH);
        addButton(kIdlePage, kIdTickTickConnect, L"连接滴答清单",
                  L"验证普通用户 API 口令，并读取今天的未完成任务。",
                  state.tickTickConnected ? L"重新连接" : L"连接…", kRowTallH);
        addButton(kIdlePage, kIdTickTickRefresh, L"同步今日任务",
                  L"从滴答清单读取今天的未完成任务，并刷新卡片内容。",
                  state.tickTickSyncing ? L"同步中…" : L"立即同步", kRowH);
        addButton(kIdlePage, kIdTickTickDisconnect, L"断开连接",
                  L"删除本机保存的滴答清单 API 口令。",
                  L"断开", kRowH);
        updateTickTickRows();
        addHeader(0, L"背景效果");
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
                    L"选择动态背景显示在待机内容、歌词或两者；待机内容包括每日一言和默认欢迎语，独立频谱容器和悬浮卡片不使用。",
                    {L"都不启用", L"仅待机内容启用", L"仅歌词启用", L"都启用"},
                    {L"不显示动态背景", L"只在待机内容时显示", L"只在播放歌词时显示",
                     L"待机内容和歌词都显示"},
                    state.idleQuoteBackgroundScope,
                    !minimal && state.idleQuoteBackground != 0,
                    kModeGridMinH);
        addHeader(kMediaPopupPage, L"播放控件");
        addToggle(kMediaPopupPage, kIdHoverControls, L"显示播放控件",
                  state.hoverControls);
        addRadio(kMediaPopupPage, kIdHoverControlStyle, L"控件样式",
                 L"内嵌控件在鼠标悬浮时显示，仅提供播放操作；媒体卡片还显示歌词进度，其打开方式由“悬浮卡片”中的“悬浮卡片展开方式”决定，点击应用图标或名称可打开播放器。",
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
        addHeader(kSongToastPage, L"显示与时长");
        Row& songToast = addRow(kSongToastPage, kIdSongToast, ControlKind::Toggle,
                                L"切歌时弹出歌曲信息",
                                L"在所选显示器短暂弹出封面、标题和艺术家；弹窗磨砂半透明，"
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
        addHeader(kSongToastPage, L"弹出位置");
        addRadio(kSongToastPage, kIdSongToastMonitor, L"显示器选择",
                 L"在每次弹窗出现时确定显示器，不随鼠标移动。",
                 {L"主显示器", L"当前鼠标所在显示器"},
                 state.songToastFollowMouseMonitor ? 1 : 0,
                 !minimal && state.songToastEnabled, kRowTallH);
        addRadio(kSongToastPage, kIdSongToastPosition, L"切歌弹窗位置", nullptr,
                 {L"中上", L"中下"},
                 state.songToastPosition, !minimal && state.songToastEnabled,
                 kSongToastPositionRowH);
        addHeader(kSongToastPage, L"效果预览");
        Row& songToastPreview = addButton(
            kSongToastPage, kIdSongToastPreview, L"预览切歌弹窗",
            L"使用当前歌曲，按所选显示器、位置和停留时间展示；需要开启弹窗且当前有歌曲。",
            L"预览弹窗", kRowTallH);
        songToastPreview.enabled = !minimal && state.songToastEnabled &&
                                   state.songToastPreviewAvailable;
        addHeader(kPerformancePage, L"渲染与资源占用");
        addModeGrid(kPerformancePage, kIdRenderMode, L"性能模式",
                    L"模式仅本次运行有效，重启软件后恢复正常模式",
                    {L"正常", L"低渲染", L"完全停止", L"极简"},
                    {L"完整视觉效果", L"降低帧率，节省 GPU", L"停止渲染，保留监听",
                     L"横向歌词，关闭逐字与转场"},
                    std::clamp(state.renderMode, 0, kRenderModeMinimal), true, kModeGridMinH);

        const bool spectrumEnabled = !minimal && !vertical;
        addHeader(kSpectrumPage, L"启用与配色");
        Row& spectrum = addToggle(kSpectrumPage, kIdSpectrum, L"频谱",
                                  spectrumEnabled ? state.spectrumOn : false);
        spectrum.enabled = spectrumEnabled;
        addRadio(kSpectrumPage, kIdSpectrumColorMode, L"频谱颜色",
                 L"跟随已播放色恢复原有配色；跟随专辑主题色与歌词设置行为一致；自定义颜色作为频谱基色。",
                 {L"跟随已播放色", L"跟随专辑主题色", L"自定义颜色"},
                 state.spectrumCustomColor ? 2 : state.spectrumFollowAlbum ? 1 : 0,
                 state.spectrumOn && spectrumEnabled, kRowTallH);
        Row& spectrumColor = addButton(
            kSpectrumPage, kIdSpectrumColor, L"自定义频谱颜色",
            L"只在上面选择“自定义颜色”时生效。",
            colorText(state.spectrumColor).c_str(), kRowTallH);
        spectrumColor.enabled = state.spectrumOn && state.spectrumCustomColor && spectrumEnabled;
        addHeader(kSpectrumPage, L"频谱样式");
        addRadio(kSpectrumPage, kIdSpectrumStyle, L"选择样式", nullptr,
                 {L"默认", L"柱状图", L"梦幻波浪", L"分段电平", L"底缘频谱带"},
                 vertical ? 0 : state.spectrumStyle,
                 state.spectrumOn && spectrumEnabled,
                 kSpectrumStyleRowH);
        addHeader(kSpectrumPage, L"频谱特效");
        const wchar_t* effectNames[kSpectrumEffectCount] = {
            L"柔光辉晕", L"衰减残影", L"峰值散粒", L"流动光泽"};
        for (int i = 0; i < kSpectrumEffectCount; ++i) {
            Row& effect = addToggle(kSpectrumPage, kIdSpectrumEffectFirst + i,
                effectNames[i], (state.spectrumEffects & spectrumEffectBit(kSpectrumEffects[i])) != 0);
            effect.enabled = state.spectrumOn && spectrumEnabled;
        }
        Row& spectrumGradient =
            addToggle(kSpectrumPage, kIdSpectrumGradient, L"频谱渐变色",
                      spectrumEnabled ? state.spectrumGradient : false);
        spectrumGradient.enabled = state.spectrumOn && spectrumEnabled;
        Row& spectrumBackground =
            addRow(kSpectrumPage, kIdSpectrumBackground, ControlKind::Toggle, L"背景波浪",
                   L"仅在选择“梦幻波浪”样式时可开启。", 40.0f, kRowTallH);
        spectrumBackground.checked = spectrumEnabled ? state.spectrumBackground : false;
        spectrumBackground.enabled = state.spectrumOn && state.spectrumStyle == 2 &&
                                     spectrumEnabled;
        addSlider(kSpectrumPage, kIdSpectrumOpacity, L"背景波浪不透明度", state.spectrumOpacity,
                  state.spectrumOn && state.spectrumStyle == 2 && state.spectrumBackground &&
                      spectrumEnabled);
        // 频谱行创建完毕后才能按互斥关系刷新进度背景行的可用态
        updateProgressBackgroundRowsEnabled();

        addHeader(kLyricsPage, L"排版");
        addToggle(kLyricsPage, kIdDoubleLine, L"双行歌词",
                  vertical ? false : state.doubleLineLyrics).enabled = !vertical;
        addRadio(kLyricsPage, kIdAlignment, L"歌词对齐", nullptr,
                 {L"左对齐", L"居中", L"右对齐"},
                 vertical ? 0 : state.lyricAlignment,
                 !vertical && state.taskbarViewMode == 0, kRowH);
        addHeader(kLyricsPage, L"颜色与效果");
        addButton(kLyricsPage, kIdFontColor, L"歌词字体颜色与效果", nullptr, L"打开…");
        addToggle(kLyricsPage, kIdFollowAlbum, L"歌词已播放颜色跟随专辑", state.followAlbum);
        addHeader(kLyricsPage, L"翻译与罗马音");
        addToggle(kLyricsPage, kIdSecondaryOn, L"开启翻译/罗马音",
                  vertical ? false : state.secondaryEnabled).enabled = !vertical;
        const wchar_t* secondaryHint = state.secondaryAvailability == 1
                                            ? L"正在检查翻译和罗马音…"
                                            : state.secondaryAvailability == 2
                                                  ? L"当前歌曲无翻译或罗马音"
                                                  : L"";
        addRadio(kLyricsPage, kIdSecondaryType, L"辅助歌词类型", secondaryHint,
                 {L"翻译", L"罗马音"}, vertical ? 0 : (state.preferRomanization ? 1 : 0),
                 !vertical && state.secondaryEnabled && state.secondaryAvailability == 0,
                 *secondaryHint ? kRowTallH : kRowH);
        addHeader(kLyricsPage, L"本地歌词");
        addToggle(kLyricsPage, kIdQqLocalLyricsEnabled, L"使用 QQ 音乐本地歌词",
                  state.qqLocalLyricsEnabled);
        Row& persistOrder = addRow(kLyricsPage, kIdQqLocalLyricsPersistOrder,
                                   ControlKind::Toggle,
                                   L"记住歌词版本选择",
                                   L"记住每首歌切换后的本地/在线版本；关闭后不保存新记录，但仍读取已有记录",
                                   40.0f, kRowTallH);
        persistOrder.checked = state.qqLocalLyricsPersistOrder;
        persistOrder.enabled = state.qqLocalLyricsEnabled;
        const std::wstring localPathHint = state.qqLocalLyricsPath.empty()
                                                ? std::wstring(L"未配置")
                                                : state.qqLocalLyricsPath;
        Row& localPath = addButton(kLyricsPage, kIdQqLocalLyricsPath, L"QQ 音乐本地歌词目录",
                                   localPathHint.c_str(), L"选择文件夹…", kRowTallH);
        localPath.enabled = state.qqLocalLyricsEnabled;
        updateImmersiveRowsEnabled();
    }

    bool searching() const { return !searchQuery.empty(); }

    void refreshSearch() {
        searchQuery = settingsSearch.text();
        std::transform(searchQuery.begin(), searchQuery.end(), searchQuery.begin(),
                       [](wchar_t character) {
                           return static_cast<wchar_t>(std::towlower(character));
                       });
        std::vector<std::wstring> terms;
        for (size_t begin = 0; begin < searchQuery.size();) {
            while (begin < searchQuery.size() && std::iswspace(searchQuery[begin]))
                ++begin;
            size_t end = begin;
            while (end < searchQuery.size() && !std::iswspace(searchQuery[end]))
                ++end;
            if (end > begin)
                terms.push_back(searchQuery.substr(begin, end - begin));
            begin = end;
        }
        if (terms.empty())
            searchQuery.clear();
        searchResults.clear();
        if (searching()) {
            for (int page = 0; page < kSettingsPageCount; ++page) {
                std::wstring group;
                for (const auto& row : rows[page]) {
                    if (row.kind == ControlKind::Header) {
                        group = row.text;
                        continue;
                    }
                    std::wstring text = pageTitles[page] + L" " + group + L" " +
                                        row.text + L" " + row.hint;
                    for (const auto& option : row.options)
                        text += L" " + option;
                    std::transform(text.begin(), text.end(), text.begin(),
                        [](wchar_t character) {
                            return static_cast<wchar_t>(std::towlower(character));
                        });
                    if (std::all_of(terms.begin(), terms.end(), [&](const std::wstring& term) {
                            return text.find(term) != std::wstring::npos;
                        }))
                        searchResults.push_back({page, row.id, pageTitles[page] +
                            (group.empty() ? L"" : L" › " + group)});
                }
            }
        }
        selectedSearchResult = searchResults.empty() ? -1 : 0;
        searchPaletteScroll = 0.0f;
        searchPaletteHoverKind = kSearchPaletteHitNone;
        searchPaletteHoverRow = -1;
        if (searchPaletteOpen) {
            layoutSearchPalette();
            searchPaletteSurface.invalidate();
        }
    }

    void selectSearchResult(int direction) {
        if (searchResults.empty())
            return;
        const int count = static_cast<int>(searchResults.size());
        if (direction == 1 || direction == -1) {
            const int base = selectedSearchResult < 0 ? 0 : selectedSearchResult;
            selectedSearchResult = (base + direction + count) % count;
        } else {
            selectedSearchResult = std::clamp(selectedSearchResult + direction, 0, count - 1);
        }
        const float rowTop = kSearchPaletteListPadY + selectedSearchResult * kSearchPaletteRowH;
        const float rowBottom = rowTop + kSearchPaletteRowH + kSearchPaletteListPadY;
        const float viewportHeight =
            std::max(0.0f, searchPaletteListRect.bottom - searchPaletteListRect.top);
        if (rowTop < searchPaletteScroll + kSearchPaletteListPadY) {
            searchPaletteScroll =
                std::clamp(rowTop - kSearchPaletteListPadY, 0.0f, searchPaletteMaxScroll);
        } else if (rowBottom > searchPaletteScroll + viewportHeight) {
            searchPaletteScroll =
                std::clamp(rowBottom - viewportHeight, 0.0f, searchPaletteMaxScroll);
        }
        searchPaletteSurface.invalidate();
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

    void openSearchResult(int index) {
        if (index < 0 || index >= static_cast<int>(searchResults.size()))
            return;
        const auto result = searchResults[index];
        closeSearchPalette(true);
        showPage(result.page);
        // 先按真实字体测量目标页，再计算定位，避免多行说明改变高度后落点偏移。
        surface.invalidate();
        UpdateWindow(hwnd);
        if (auto* row = findRow(result.id)) {
            setContentScroll(contentScroll + row->cardRect.top - contentViewportRect.top - 12.0f);
            startSearchLocateHighlight(result.id);
            focusedId = row->enabled ? result.id : kIdNav;
        }
        SetFocus(hwnd);
        focusVisible = false;
        surface.invalidate();
    }

    static LRESULT CALLBACK searchEditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR subclassId, DWORD_PTR refData) {
        auto* self = reinterpret_cast<Impl*>(refData);
        if (msg == WM_NCDESTROY) {
            RemoveWindowSubclass(edit, &Impl::searchEditProc, subclassId);
            return DefSubclassProc(edit, msg, wp, lp);
        }
        if (self && msg == WM_KEYDOWN) {
            if (wp == VK_ESCAPE) {
                self->closeSearchPalette(true);
                return 0;
            }
            if (wp == VK_RETURN && self->searching() && self->selectedSearchResult >= 0) {
                self->openSearchResult(self->selectedSearchResult);
                return 0;
            }
            if ((wp == VK_UP || wp == VK_DOWN) && self->searching()) {
                self->selectSearchResult(wp == VK_DOWN ? 1 : -1);
                return 0;
            }
            if ((wp == VK_PRIOR || wp == VK_NEXT) && self->searching()) {
                self->selectSearchResult(wp == VK_NEXT ? 5 : -5);
                return 0;
            }
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                SendMessageW(edit, EM_SETSEL, 0, -1);
                return 0;
            }
            if (wp == VK_TAB) {
                return 0;
            }
        }
        if (self && msg == WM_MOUSEWHEEL && self->searchPaletteHwnd) {
            SendMessageW(self->searchPaletteHwnd, msg, wp, lp);
            return 0;
        }
        if (msg == WM_CHAR &&
            (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 6))
            return 0;
        return DefSubclassProc(edit, msg, wp, lp);
    }

    void styleSearchPaletteWindow() {
        if (!searchPaletteHwnd)
            return;
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(searchPaletteHwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                              &disableTransitions, sizeof(disableTransitions));
        fluent::applyRoundCorners(searchPaletteHwnd, false);
        const bool dark = fluent::isDarkMode();
        fluent::applyDarkCaption(searchPaletteHwnd, dark);
        fluent::applyBorderColor(searchPaletteHwnd,
                                 dark ? RGB(68, 68, 68) : RGB(214, 214, 214));
    }

    static void setWindowClickThrough(HWND window, bool clickThrough) {
        if (!window || !IsWindow(window))
            return;
        const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        const LONG_PTR nextStyle =
            clickThrough ? (exStyle | WS_EX_TRANSPARENT)
                         : (exStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
        if (nextStyle != exStyle)
            SetWindowLongPtrW(window, GWL_EXSTYLE, nextStyle);
    }

    BYTE searchPaletteAlphaByte() const {
        return static_cast<BYTE>(std::clamp(
            static_cast<int>(std::lround(searchPaletteProgress * 255.0f)), 0, 255));
    }

    int searchPaletteOffsetYPx(float scale) const {
        const float slideDip = searchPaletteAnimTarget > 0.5f
                                   ? kSearchPaletteOpenSlideDip
                                   : kSearchPaletteCloseSlideDip;
        return static_cast<int>(
            std::lround((1.0f - searchPaletteProgress) * slideDip * scale));
    }

    void applySearchPaletteAnimationVisuals() {
        const BYTE alphaByte = searchPaletteAlphaByte();
        if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
            SetLayeredWindowAttributes(searchPaletteHwnd, 0, alphaByte, LWA_ALPHA);
            SetWindowPos(searchPaletteHwnd, nullptr, searchPaletteBaseX,
                         searchPaletteBaseY + searchPaletteOffsetYPx(searchPaletteScale),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (searchScrimHwnd && IsWindow(searchScrimHwnd))
            searchScrimRenderer.present(searchScrimHwnd, alphaByte);
    }

    void finalizeSearchPaletteClosed() {
        if (hwnd)
            KillTimer(hwnd, kSearchPaletteAnimTimer);
        searchPaletteAnimating = false;
        searchPaletteProgress = 0.0f;
        searchPaletteAnimFrom = 0.0f;
        searchPaletteAnimTarget = 0.0f;
        closingSearchPalette = true;
        if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
            ShowWindow(searchPaletteHwnd, SW_HIDE);
            setWindowClickThrough(searchPaletteHwnd, false);
            settingsSearch.setText(L"");
        }
        if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
            ShowWindow(searchScrimHwnd, SW_HIDE);
            setWindowClickThrough(searchScrimHwnd, false);
        }
        searchQuery.clear();
        searchResults.clear();
        selectedSearchResult = -1;
        searchPaletteScroll = 0.0f;
        searchPaletteMaxScroll = 0.0f;
        closingSearchPalette = false;
    }

    void advanceSearchPaletteAnimation() {
        if (!searchPaletteAnimating)
            return;
        const float elapsed =
            static_cast<float>(GetTickCount64() - searchPaletteAnimStartMs);
        const float rawProgress =
            searchPaletteAnimDurationMs <= 1.0f
                ? 1.0f
                : std::clamp(elapsed / searchPaletteAnimDurationMs, 0.0f, 1.0f);
        const float inverse = 1.0f - rawProgress;
        if (searchPaletteAnimTarget > 0.5f) {
            const float eased = 1.0f - inverse * inverse * inverse;
            searchPaletteProgress =
                searchPaletteAnimFrom + (1.0f - searchPaletteAnimFrom) * eased;
        } else {
            const float eased = 1.0f - inverse * inverse;
            searchPaletteProgress = searchPaletteAnimFrom * (1.0f - eased);
        }
        if (rawProgress >= 1.0f) {
            if (searchPaletteAnimTarget <= 0.5f) {
                finalizeSearchPaletteClosed();
                return;
            }
            if (hwnd)
                KillTimer(hwnd, kSearchPaletteAnimTimer);
            searchPaletteAnimating = false;
            searchPaletteProgress = 1.0f;
        }
        applySearchPaletteAnimationVisuals();
    }

    float searchPaletteDesiredHeightDip() const {
        if (!searching())
            return kSearchPaletteHeaderH + kSearchPaletteEmptyPromptH;
        if (searchResults.empty())
            return kSearchPaletteHeaderH + kSearchPaletteNoResultH;
        const int visibleCount = std::min(static_cast<int>(searchResults.size()),
                                          kSearchPaletteMaxVisibleRows);
        const float bodyHeight =
            kSearchPaletteListPadY * 2.0f + visibleCount * kSearchPaletteRowH;
        return kSearchPaletteHeaderH + bodyHeight + kSearchPaletteFooterH;
    }

    void layoutSearchPalette() {
        if (!searchPaletteHwnd || !hwnd || !IsWindow(hwnd))
            return;
        RECT ownerClient{};
        GetClientRect(hwnd, &ownerClient);
        POINT topLeft{ownerClient.left, ownerClient.top};
        POINT bottomRight{ownerClient.right, ownerClient.bottom};
        ClientToScreen(hwnd, &topLeft);
        ClientToScreen(hwnd, &bottomRight);

        UINT dpi = GetDpiForWindow(searchPaletteHwnd);
        if (dpi == 0)
            dpi = GetDpiForWindow(hwnd);
        if (dpi == 0)
            dpi = 96;
        const float scale = fluent::dipScale(dpi);
        auto toPx = [scale](float dipValue) {
            return static_cast<int>(std::lround(dipValue * scale));
        };

        const int ownerWidthPx = std::max(1, static_cast<int>(bottomRight.x - topLeft.x));
        const int ownerHeightPx = std::max(1, static_cast<int>(bottomRight.y - topLeft.y));
        const float ownerWidthDip = static_cast<float>(ownerWidthPx) / scale;
        const float ownerHeightDip = static_cast<float>(ownerHeightPx) / scale;

        const float maxAllowedWidthDip = std::max(320.0f, ownerWidthDip - 48.0f);
        const float paletteWidthDip =
            std::min(maxAllowedWidthDip, std::clamp(ownerWidthDip * 0.58f, 480.0f, 560.0f));
        const float maxAllowedHeightDip = std::max(132.0f, ownerHeightDip - 88.0f);
        const float paletteHeightDip =
            std::min(searchPaletteDesiredHeightDip(), maxAllowedHeightDip);

        const bool hasInputText =
            settingsSearch.editHwnd() && GetWindowTextLengthW(settingsSearch.editHwnd()) > 0;
        searchPaletteEscRect =
            D2D1::RectF(paletteWidthDip - 48.0f, 16.0f, paletteWidthDip - 16.0f, 36.0f);
        searchPaletteClearRect =
            hasInputText
                ? D2D1::RectF(paletteWidthDip - 80.0f, 14.0f, paletteWidthDip - 56.0f, 38.0f)
                : D2D1_RECT_F{};
        const float editLeftDip = 46.0f;
        const float editRightDip =
            hasInputText ? searchPaletteClearRect.left - 8.0f : searchPaletteEscRect.left - 10.0f;

        if (searching() && !searchResults.empty()) {
            const float listTop = kSearchPaletteHeaderH;
            const float listBottom =
                std::max(listTop, paletteHeightDip - kSearchPaletteFooterH);
            searchPaletteListRect = D2D1::RectF(0.0f, listTop, paletteWidthDip, listBottom);
            const float totalListHeight =
                kSearchPaletteListPadY * 2.0f +
                static_cast<float>(searchResults.size()) * kSearchPaletteRowH;
            const float viewportHeight = std::max(0.0f, listBottom - listTop);
            searchPaletteMaxScroll = std::max(0.0f, totalListHeight - viewportHeight);
            searchPaletteScroll = std::clamp(searchPaletteScroll, 0.0f, searchPaletteMaxScroll);
        } else {
            searchPaletteListRect = D2D1_RECT_F{};
            searchPaletteMaxScroll = 0.0f;
            searchPaletteScroll = 0.0f;
        }

        const int paletteWidthPx = std::max(1, toPx(paletteWidthDip));
        const int paletteHeightPx = std::max(1, toPx(paletteHeightDip));
        int screenX = topLeft.x + (ownerWidthPx - paletteWidthPx) / 2;
        int screenY = topLeft.y + toPx(64.0f);

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const bool hasWorkArea =
            monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
        if (hasWorkArea) {
            const RECT& workArea = monitorInfo.rcWork;
            if (screenX + paletteWidthPx > workArea.right - 8)
                screenX = workArea.right - 8 - paletteWidthPx;
            if (screenX < workArea.left + 8)
                screenX = workArea.left + 8;
            if (screenY + paletteHeightPx > workArea.bottom - 8)
                screenY = workArea.bottom - 8 - paletteHeightPx;
            if (screenY < workArea.top + 8)
                screenY = workArea.top + 8;
        }

        searchPaletteBaseX = screenX;
        searchPaletteBaseY = screenY;
        searchPaletteScale = scale;
        const int animatedScreenY = screenY + searchPaletteOffsetYPx(scale);
        SetWindowPos(searchPaletteHwnd, HWND_TOP, screenX, animatedScreenY, paletteWidthPx,
                     paletteHeightPx, SWP_NOACTIVATE);
        if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
            RECT scrimBounds{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                             &scrimBounds, sizeof(scrimBounds))) ||
                scrimBounds.right <= scrimBounds.left ||
                scrimBounds.bottom <= scrimBounds.top) {
                scrimBounds = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
            } else if (IsZoomed(hwnd) != FALSE && hasWorkArea) {
                scrimBounds.left = std::max(scrimBounds.left, monitorInfo.rcWork.left);
                scrimBounds.top = std::max(scrimBounds.top, monitorInfo.rcWork.top);
                scrimBounds.right = std::min(scrimBounds.right, monitorInfo.rcWork.right);
                scrimBounds.bottom = std::min(scrimBounds.bottom, monitorInfo.rcWork.bottom);
            }
            const int scrimWidthPx =
                std::max(1, static_cast<int>(scrimBounds.right - scrimBounds.left));
            const int scrimHeightPx =
                std::max(1, static_cast<int>(scrimBounds.bottom - scrimBounds.top));
            SetWindowPos(searchScrimHwnd, searchPaletteHwnd, scrimBounds.left,
                         scrimBounds.top, scrimWidthPx, scrimHeightPx, SWP_NOACTIVATE);
            renderSearchScrim(scrimWidthPx, scrimHeightPx, dpi);
        }
        if (settingsSearch.hwnd()) {
            settingsSearch.move(toPx(editLeftDip), toPx(10.0f),
                                std::max(1, toPx(editRightDip - editLeftDip)), toPx(32.0f));
        }
    }

    void renderSearchScrim(int widthPx, int heightPx, UINT dpi) {
        if (!searchScrimHwnd || widthPx <= 0 || heightPx <= 0)
            return;
        const bool dark = fluent::isDarkMode();
        const bool zoomed = hwnd && IsZoomed(hwnd) != FALSE;
        const BYTE alphaByte = searchPaletteAlphaByte();
        if (widthPx == searchScrimLastWidth && heightPx == searchScrimLastHeight &&
            dpi == searchScrimLastDpi && dark == searchScrimLastDark &&
            zoomed == searchScrimLastZoomed) {
            searchScrimRenderer.present(searchScrimHwnd, alphaByte);
            return;
        }
        if (!searchScrimRenderer.initialize())
            return;
        searchScrimRenderer.setDpi(dpi);
        if (!searchScrimRenderer.bindDC(widthPx, heightPx))
            return;
        auto* target = searchScrimRenderer.renderTarget();
        if (!target)
            return;

        const float scale = fluent::dipScale(dpi);
        const float widthDip = static_cast<float>(widthPx) / scale;
        const float heightDip = static_cast<float>(heightPx) / scale;
        DWORD cornerPreference = 0;
        constexpr DWORD kWindowCornerPreferenceAttr = 33;
        const bool roundedWindow =
            !zoomed && hwnd &&
            SUCCEEDED(DwmGetWindowAttribute(hwnd, kWindowCornerPreferenceAttr,
                                            &cornerPreference, sizeof(cornerPreference))) &&
            cornerPreference != 1;
        const float cornerRadius =
            roundedWindow ? std::min(fluent::metrics::cardRadius,
                                     std::min(widthDip * 0.5f, heightDip * 0.5f))
                          : 0.0f;
        const D2D1_COLOR_F scrimColor =
            D2D1::ColorF(0.0f, 0.0f, 0.0f, dark ? 0.46f : 0.28f);

        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        ID2D1SolidColorBrush* scrimBrush = nullptr;
        if (SUCCEEDED(target->CreateSolidColorBrush(scrimColor, &scrimBrush)) && scrimBrush) {
            if (cornerRadius > 0.0f) {
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(0.0f, 0.0f, widthDip, heightDip),
                        cornerRadius, cornerRadius),
                    scrimBrush);
            } else {
                target->FillRectangle(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip),
                                      scrimBrush);
            }
            scrimBrush->Release();
        }
        if (FAILED(target->EndDraw())) {
            searchScrimRenderer.discard();
            return;
        }
        if (searchScrimRenderer.present(searchScrimHwnd, alphaByte)) {
            searchScrimLastWidth = widthPx;
            searchScrimLastHeight = heightPx;
            searchScrimLastDpi = dpi;
            searchScrimLastDark = dark;
            searchScrimLastZoomed = zoomed;
        }
    }

    static LRESULT CALLBACK searchScrimWndProc(HWND window, UINT message, WPARAM wp,
                                               LPARAM lp) {
        Impl* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->searchScrimHwnd = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(window, message, wp, lp);

        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            self->closeSearchPalette(true);
            return 0;
        case WM_MOUSEWHEEL:
            if (self->searchPaletteOpen && self->searchPaletteHwnd)
                SendMessageW(self->searchPaletteHwnd, message, wp, lp);
            return 0;
        case WM_DESTROY:
            self->searchScrimRenderer.releaseAll();
            self->searchScrimLastWidth = 0;
            self->searchScrimLastHeight = 0;
            self->searchScrimHwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    bool ensureSearchScrimWindow() {
        if (searchScrimHwnd && IsWindow(searchScrimHwnd))
            return true;
        WNDCLASSEXW scrimClass{};
        scrimClass.cbSize = sizeof(scrimClass);
        scrimClass.lpfnWndProc = &Impl::searchScrimWndProc;
        scrimClass.hInstance = inst;
        scrimClass.lpszClassName = L"QQMusicLyricSettingsSearchScrim";
        scrimClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&scrimClass);

        searchScrimHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"QQMusicLyricSettingsSearchScrim", L"",
            WS_POPUP, 0, 0, 1, 1, hwnd, nullptr, inst, this);
        searchScrimLastWidth = 0;
        searchScrimLastHeight = 0;
        return searchScrimHwnd != nullptr;
    }

    int searchPaletteHitTest(float xDip, float yDip, int* outRow = nullptr) const {
        if (outRow)
            *outRow = -1;
        if (searchPaletteClearRect.right > searchPaletteClearRect.left &&
            contains(searchPaletteClearRect, xDip, yDip))
            return kSearchPaletteHitClear;
        if (contains(searchPaletteEscRect, xDip, yDip))
            return kSearchPaletteHitClose;
        if (searching() && !searchResults.empty() &&
            contains(searchPaletteListRect, xDip, yDip)) {
            const float listOffset =
                yDip - searchPaletteListRect.top + searchPaletteScroll - kSearchPaletteListPadY;
            if (listOffset >= 0.0f) {
                const int rowIndex = static_cast<int>(listOffset / kSearchPaletteRowH);
                const float withinRow = listOffset - rowIndex * kSearchPaletteRowH;
                if (rowIndex >= 0 && rowIndex < static_cast<int>(searchResults.size()) &&
                    withinRow >= 2.0f && withinRow <= kSearchPaletteRowH - 2.0f &&
                    xDip >= 8.0f && xDip <= searchPaletteListRect.right - 8.0f) {
                    if (outRow)
                        *outRow = rowIndex;
                    return kSearchPaletteHitRow;
                }
            }
        }
        return kSearchPaletteHitNone;
    }

    void paintSearchPalette(fluent::FluentDialogSurface::Painter& painter,
                            float widthDip, float heightDip) {
        const auto& paletteColors = fluent::palette();
        if (auto* backgroundBrush = painter.brush(paletteColors.menuFill)) {
            painter.target()->FillRectangle(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip),
                                            backgroundBrush);
        }
        painter.strokeRoundRect(
            paletteColors.menuStroke,
            D2D1::RectF(0.5f, 0.5f, widthDip - 0.5f, heightDip - 0.5f),
            1.0f, fluent::metrics::cardRadius);

        if (auto* searchBrush = painter.brush(paletteColors.accent)) {
            settings_icon::draw(painter.target(), settings_icon::Kind::Search,
                                D2D1::RectF(18.0f, 17.0f, 36.0f, 35.0f), searchBrush, 1.4f);
        }

        if (searchPaletteClearRect.right > searchPaletteClearRect.left) {
            const bool clearHovered = searchPaletteHoverKind == kSearchPaletteHitClear;
            const bool clearPressed = searchPalettePressedKind == kSearchPaletteHitClear;
            if (clearPressed || clearHovered) {
                painter.fillRoundRect(
                    clearPressed ? paletteColors.listSelected : paletteColors.listHover,
                    searchPaletteClearRect, 5.0f);
            }
            const float centerX =
                (searchPaletteClearRect.left + searchPaletteClearRect.right) * 0.5f;
            const float centerY =
                (searchPaletteClearRect.top + searchPaletteClearRect.bottom) * 0.5f;
            if (auto* crossBrush = painter.brush(
                    clearHovered ? paletteColors.text : paletteColors.textSecondary)) {
                painter.target()->DrawLine(
                    D2D1::Point2F(centerX - 4.0f, centerY - 4.0f),
                    D2D1::Point2F(centerX + 4.0f, centerY + 4.0f), crossBrush, 1.35f);
                painter.target()->DrawLine(
                    D2D1::Point2F(centerX + 4.0f, centerY - 4.0f),
                    D2D1::Point2F(centerX - 4.0f, centerY + 4.0f), crossBrush, 1.35f);
            }
        }

        const bool escHovered = searchPaletteHoverKind == kSearchPaletteHitClose;
        const bool escPressed = searchPalettePressedKind == kSearchPaletteHitClose;
        painter.fillRoundRect(
            escPressed ? paletteColors.listSelected
                       : (escHovered ? paletteColors.controlHover : paletteColors.listHover),
            searchPaletteEscRect, 4.0f);
        painter.strokeRoundRect(paletteColors.cardStroke, searchPaletteEscRect, 1.0f, 4.0f);
        painter.drawText(L"Esc", painter.textFormat(11.0f, 500, true, true),
                         searchPaletteEscRect,
                         escHovered ? paletteColors.text : paletteColors.textSecondary);

        if (auto* separatorBrush = painter.brush(paletteColors.separator)) {
            painter.target()->DrawLine(
                D2D1::Point2F(0.0f, kSearchPaletteHeaderH - 0.5f),
                D2D1::Point2F(widthDip, kSearchPaletteHeaderH - 0.5f), separatorBrush, 1.0f);
        }

        if (!searching()) {
            painter.drawText(
                L"输入关键词搜索设置名称、选项或说明，支持空格分词筛选",
                painter.textFormat(13.0f, 400, false, true),
                D2D1::RectF(20.0f, kSearchPaletteHeaderH, widthDip - 20.0f, heightDip),
                paletteColors.textSecondary);
            return;
        }

        if (searchResults.empty()) {
            painter.drawText(
                L"未找到匹配的设置，请尝试其他关键词",
                painter.textFormat(13.5f, 400, false, true),
                D2D1::RectF(20.0f, kSearchPaletteHeaderH, widthDip - 20.0f, heightDip),
                paletteColors.textSecondary);
            return;
        }

        painter.target()->PushAxisAlignedClip(searchPaletteListRect,
                                              D2D1_ANTIALIAS_MODE_ALIASED);
        const bool hasScrollbar = searchPaletteMaxScroll > 0.5f;
        const float itemRight = widthDip - (hasScrollbar ? 14.0f : 8.0f);
        for (int index = 0; index < static_cast<int>(searchResults.size()); ++index) {
            const float rowTop = searchPaletteListRect.top + kSearchPaletteListPadY +
                                 index * kSearchPaletteRowH - searchPaletteScroll;
            if (rowTop + kSearchPaletteRowH < searchPaletteListRect.top ||
                rowTop > searchPaletteListRect.bottom)
                continue;
            const auto& result = searchResults[index];
            const auto* row = findRow(result.id);
            if (!row)
                continue;

            const D2D1_RECT_F itemRect =
                D2D1::RectF(8.0f, rowTop + 2.0f, itemRight, rowTop + kSearchPaletteRowH - 2.0f);
            const bool selected = index == selectedSearchResult;
            const bool hovered =
                searchPaletteHoverKind == kSearchPaletteHitRow && searchPaletteHoverRow == index;
            if (selected) {
                painter.fillRoundRect(paletteColors.listSelected, itemRect, 6.0f);
                const float midY = (itemRect.top + itemRect.bottom) * 0.5f;
                painter.fillRoundRect(
                    paletteColors.accent,
                    D2D1::RectF(itemRect.left + 4.0f, midY - 8.0f,
                                itemRect.left + 7.0f, midY + 8.0f),
                    1.5f);
            } else if (hovered) {
                painter.fillRoundRect(paletteColors.listHover, itemRect, 6.0f);
            }

            const float iconCenterY = (itemRect.top + itemRect.bottom) * 0.5f;
            const D2D1_RECT_F iconBadge =
                D2D1::RectF(itemRect.left + 14.0f, iconCenterY - 15.0f,
                            itemRect.left + 44.0f, iconCenterY + 15.0f);
            const D2D1_COLOR_F badgeFill =
                selected ? D2D1::ColorF(paletteColors.accent.r, paletteColors.accent.g,
                                        paletteColors.accent.b, 0.12f)
                         : paletteColors.listHover;
            painter.fillRoundRect(badgeFill, iconBadge, 6.0f);
            if (auto* iconBrush = painter.brush(
                    selected ? paletteColors.accent : paletteColors.textSecondary)) {
                settings_icon::draw(
                    painter.target(), iconForRow(result.id),
                    D2D1::RectF(iconBadge.left + 7.0f, iconBadge.top + 7.0f,
                                iconBadge.right - 7.0f, iconBadge.bottom - 7.0f),
                    iconBrush, 1.25f);
            }

            const float textRight = selected ? itemRect.right - 56.0f : itemRect.right - 14.0f;
            painter.drawTrimmedText(
                row->text, painter.textFormat(13.5f, selected ? 600 : 500, false, true),
                D2D1::RectF(itemRect.left + 54.0f, itemRect.top + 6.0f, textRight,
                            itemRect.top + 28.0f),
                paletteColors.text);
            painter.drawTrimmedText(
                result.path, painter.textFormat(12.0f, 400, false, true),
                D2D1::RectF(itemRect.left + 54.0f, itemRect.top + 27.0f, textRight,
                            itemRect.bottom - 5.0f),
                paletteColors.textSecondary);
            if (selected) {
                painter.drawText(
                    L"跳转 ↵", painter.textFormat(11.5f, 500, true, true),
                    D2D1::RectF(itemRect.right - 54.0f, itemRect.top, itemRect.right - 10.0f,
                                itemRect.bottom),
                    paletteColors.accent);
            }
        }

        if (hasScrollbar) {
            const float trackTop = searchPaletteListRect.top + 6.0f;
            const float trackBottom = searchPaletteListRect.bottom - 6.0f;
            const float trackHeight = std::max(12.0f, trackBottom - trackTop);
            const float viewportHeight =
                std::max(1.0f, searchPaletteListRect.bottom - searchPaletteListRect.top);
            const float totalHeight = viewportHeight + searchPaletteMaxScroll;
            const float thumbHeight =
                std::max(20.0f, trackHeight * (viewportHeight / totalHeight));
            const float scrollRatio =
                std::clamp(searchPaletteScroll / searchPaletteMaxScroll, 0.0f, 1.0f);
            const float thumbTop = trackTop + (trackHeight - thumbHeight) * scrollRatio;
            painter.fillRoundRect(
                paletteColors.controlStroke,
                D2D1::RectF(widthDip - 8.0f, thumbTop, widthDip - 4.0f, thumbTop + thumbHeight),
                2.0f);
        }
        painter.target()->PopAxisAlignedClip();

        const float footerTop = heightDip - kSearchPaletteFooterH;
        if (auto* separatorBrush = painter.brush(paletteColors.separator)) {
            painter.target()->DrawLine(
                D2D1::Point2F(0.0f, footerTop + 0.5f),
                D2D1::Point2F(widthDip, footerTop + 0.5f), separatorBrush, 1.0f);
        }
        auto* footerFormat = painter.textFormat(11.5f, 400, false, true);
        painter.drawText(L"↑↓ 选择    Enter 跳转    Esc 关闭", footerFormat,
                         D2D1::RectF(16.0f, footerTop, widthDip - 100.0f, heightDip),
                         paletteColors.textSecondary);
        const std::wstring countLabel =
            L"共 " + std::to_wstring(searchResults.size()) + L" 项";
        const float countWidth = painter.measureTextWidth(countLabel, footerFormat);
        painter.drawText(
            countLabel, footerFormat,
            D2D1::RectF(std::max(16.0f, widthDip - 16.0f - countWidth - 2.0f), footerTop,
                        widthDip - 14.0f, heightDip),
            paletteColors.textSecondary);
    }

    static LRESULT CALLBACK searchPaletteWndProc(HWND window, UINT message, WPARAM wp,
                                                 LPARAM lp) {
        Impl* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->searchPaletteHwnd = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(window, message, wp, lp);

        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paintStruct{};
            HDC dc = BeginPaint(window, &paintStruct);
            self->searchPaletteSurface.paint(
                dc, false,
                [self](fluent::FluentDialogSurface::Painter& painter, float widthDip,
                       float heightDip) {
                    self->paintSearchPalette(painter, widthDip, heightDip);
                });
            EndPaint(window, &paintStruct);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT trackEvent{sizeof(trackEvent), TME_LEAVE, window, 0};
            TrackMouseEvent(&trackEvent);
            const POINT currentPt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const bool pointerMoved =
                currentPt.x != self->searchPaletteLastMovePt.x ||
                currentPt.y != self->searchPaletteLastMovePt.y;
            self->searchPaletteLastMovePt = currentPt;
            const float scale = self->searchPaletteSurface.dipScale();
            int hitRow = -1;
            const int hitKind = self->searchPaletteHitTest(
                static_cast<float>(currentPt.x) / scale,
                static_cast<float>(currentPt.y) / scale, &hitRow);
            bool changed = hitKind != self->searchPaletteHoverKind ||
                           hitRow != self->searchPaletteHoverRow;
            self->searchPaletteHoverKind = hitKind;
            self->searchPaletteHoverRow = hitRow;
            if (pointerMoved && hitKind == kSearchPaletteHitRow && hitRow >= 0 &&
                hitRow != self->selectedSearchResult) {
                self->selectedSearchResult = hitRow;
                changed = true;
            }
            if (changed)
                self->searchPaletteSurface.invalidate();
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self->searchPaletteHoverKind != kSearchPaletteHitNone ||
                self->searchPaletteHoverRow != -1) {
                self->searchPaletteHoverKind = kSearchPaletteHitNone;
                self->searchPaletteHoverRow = -1;
                self->searchPaletteSurface.invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const float scale = self->searchPaletteSurface.dipScale();
            int hitRow = -1;
            const int hitKind = self->searchPaletteHitTest(
                GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, &hitRow);
            self->searchPalettePressedKind = hitKind;
            self->searchPalettePressedRow = hitRow;
            if (hitKind != kSearchPaletteHitNone)
                SetCapture(window);
            self->settingsSearch.focus();
            self->searchPaletteSurface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const float scale = self->searchPaletteSurface.dipScale();
            int hitRow = -1;
            const int hitKind = self->searchPaletteHitTest(
                GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, &hitRow);
            const int pressedKind = self->searchPalettePressedKind;
            const int pressedRow = self->searchPalettePressedRow;
            self->searchPalettePressedKind = kSearchPaletteHitNone;
            self->searchPalettePressedRow = -1;
            if (GetCapture() == window)
                ReleaseCapture();
            if (pressedKind == kSearchPaletteHitClear &&
                hitKind == kSearchPaletteHitClear) {
                self->settingsSearch.setText(L"");
                self->refreshSearch();
                self->settingsSearch.focus();
                return 0;
            }
            if (pressedKind == kSearchPaletteHitClose &&
                hitKind == kSearchPaletteHitClose) {
                self->closeSearchPalette(true);
                return 0;
            }
            if (pressedKind == kSearchPaletteHitRow &&
                hitKind == kSearchPaletteHitRow && pressedRow == hitRow && hitRow >= 0) {
                self->openSearchResult(hitRow);
                return 0;
            }
            self->searchPaletteSurface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            self->searchPalettePressedKind = kSearchPaletteHitNone;
            self->searchPalettePressedRow = -1;
            self->searchPaletteSurface.invalidate();
            return 0;
        case WM_MOUSEWHEEL: {
            if (self->searchPaletteMaxScroll > 0.0f) {
                const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wp);
                self->searchPaletteScroll = std::clamp(
                    self->searchPaletteScroll -
                        static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA) * 48.0f,
                    0.0f, self->searchPaletteMaxScroll);
                self->searchPaletteSurface.invalidate();
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                self->closeSearchPalette(true);
                return 0;
            }
            if (wp == VK_RETURN && self->searching() && self->selectedSearchResult >= 0) {
                self->openSearchResult(self->selectedSearchResult);
                return 0;
            }
            if ((wp == VK_UP || wp == VK_DOWN) && self->searching()) {
                self->selectSearchResult(wp == VK_DOWN ? 1 : -1);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == kIdSettingsSearch && HIWORD(wp) == EN_CHANGE) {
                if (!self->closingSearchPalette)
                    self->refreshSearch();
                return 0;
            }
            break;
        case WM_DPICHANGED:
            self->layoutSearchPalette();
            self->searchPaletteSurface.invalidate();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && self->searchPaletteOpen &&
                !self->closingSearchPalette) {
                if (self->hwnd && IsWindow(self->hwnd)) {
                    POINT cursor{};
                    if (GetCursorPos(&cursor) && ScreenToClient(self->hwnd, &cursor)) {
                        const float ownerScale = self->surface.dipScale();
                        const float cursorX = static_cast<float>(cursor.x) / ownerScale;
                        const float cursorY = static_cast<float>(cursor.y) / ownerScale;
                        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 &&
                            contains(self->searchTriggerRect, cursorX, cursorY)) {
                            self->suppressNextSearchTriggerPress = true;
                        }
                    }
                }
                self->closeSearchPalette(false);
                return 0;
            }
            break;
        case WM_CLOSE:
            self->closeSearchPalette(true);
            return 0;
        case WM_DESTROY:
            self->searchPaletteOpen = false;
            self->searchPaletteSurface.discard();
            self->searchPaletteHwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    bool ensureSearchPaletteWindow() {
        if (searchPaletteHwnd && IsWindow(searchPaletteHwnd))
            return true;
        WNDCLASSEXW paletteClass{};
        paletteClass.cbSize = sizeof(paletteClass);
        paletteClass.style = CS_HREDRAW | CS_VREDRAW;
        paletteClass.lpfnWndProc = &Impl::searchPaletteWndProc;
        paletteClass.hInstance = inst;
        paletteClass.lpszClassName = L"QQMusicLyricSettingsSearchPalette";
        paletteClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&paletteClass);

        searchPaletteHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"QQMusicLyricSettingsSearchPalette", L"",
            WS_POPUP, 0, 0, 1, 1, hwnd, nullptr, inst, this);
        if (!searchPaletteHwnd)
            return false;
        SetLayeredWindowAttributes(searchPaletteHwnd, 0, 255, LWA_ALPHA);
        styleSearchPaletteWindow();
        searchPaletteSurface.initialize(searchPaletteHwnd, false, true);
        if (!settingsSearch.create(searchPaletteHwnd, kIdSettingsSearch,
                                   L"搜索设置项、选项或说明…", false, 14.5f)) {
            DestroyWindow(searchPaletteHwnd);
            searchPaletteHwnd = nullptr;
            return false;
        }
        settingsSearch.setChromelessStyle(true);
        SendMessageW(settingsSearch.editHwnd(), EM_SETLIMITTEXT, 120, 0);
        SetWindowSubclass(settingsSearch.editHwnd(), &Impl::searchEditProc, 2,
                          reinterpret_cast<DWORD_PTR>(this));
        return true;
    }

    void openSearchPalette() {
        if (!hwnd || !IsWindow(hwnd))
            return;
        stopSearchLocateHighlight();
        if (sliderEditOpen)
            commitSliderEditor();
        if (searchPaletteOpen && searchPaletteHwnd && IsWindowVisible(searchPaletteHwnd)) {
            SetActiveWindow(searchPaletteHwnd);
            settingsSearch.focus();
            SendMessageW(settingsSearch.editHwnd(), EM_SETSEL, 0, -1);
            return;
        }
        if (!ensureSearchPaletteWindow())
            return;
        ensureSearchScrimWindow();

        const bool wasClosing =
            !searchPaletteOpen && searchPaletteAnimating && searchPaletteProgress > 0.01f;
        if (hwnd)
            KillTimer(hwnd, kSearchPaletteAnimTimer);
        searchPaletteAnimating = false;
        setWindowClickThrough(searchPaletteHwnd, false);
        setWindowClickThrough(searchScrimHwnd, false);

        closingSearchPalette = true;
        settingsSearch.setText(L"");
        closingSearchPalette = false;
        searchQuery.clear();
        searchResults.clear();
        selectedSearchResult = -1;
        searchPaletteScroll = 0.0f;
        searchPaletteMaxScroll = 0.0f;
        searchPaletteHoverKind = kSearchPaletteHitNone;
        searchPaletteHoverRow = -1;
        searchPalettePressedKind = kSearchPaletteHitNone;
        searchPalettePressedRow = -1;
        searchPaletteLastMovePt = {-1, -1};

        BOOL clientAnimations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimations, 0);
        const float startProgress =
            clientAnimations ? (wasClosing ? searchPaletteProgress : 0.0f) : 1.0f;
        searchPaletteProgress = startProgress;
        searchPaletteAnimFrom = startProgress;
        searchPaletteAnimTarget = 1.0f;
        searchPaletteOpen = true;

        layoutSearchPalette();
        SetLayeredWindowAttributes(searchPaletteHwnd, 0, searchPaletteAlphaByte(), LWA_ALPHA);
        if (searchScrimHwnd && IsWindow(searchScrimHwnd))
            ShowWindow(searchScrimHwnd, SW_SHOWNOACTIVATE);
        ShowWindow(searchPaletteHwnd, SW_SHOW);
        SetWindowPos(searchPaletteHwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
            SetWindowPos(searchScrimHwnd, searchPaletteHwnd, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        searchPaletteSurface.invalidate();
        UpdateWindow(searchPaletteHwnd);
        SetActiveWindow(searchPaletteHwnd);
        settingsSearch.focus();
        if (clientAnimations && startProgress < 0.999f) {
            searchPaletteAnimating = true;
            searchPaletteAnimStartMs = GetTickCount64();
            searchPaletteAnimDurationMs =
                std::max(40.0f, kSearchPaletteOpenDurationMs * (1.0f - startProgress));
            SetTimer(hwnd, kSearchPaletteAnimTimer, kNavIndicatorFrameMs, nullptr);
        }
        surface.invalidate();
    }

    void closeSearchPalette(bool restoreOwnerFocus, bool immediate = false) {
        if (immediate) {
            if (!searchPaletteOpen && !searchPaletteAnimating)
                return;
            searchPaletteOpen = false;
            searchPaletteHoverKind = kSearchPaletteHitNone;
            searchPaletteHoverRow = -1;
            searchPalettePressedKind = kSearchPaletteHitNone;
            searchPalettePressedRow = -1;
            if (searchPaletteHwnd && GetCapture() == searchPaletteHwnd)
                ReleaseCapture();
            finalizeSearchPaletteClosed();
            if (restoreOwnerFocus && hwnd && IsWindow(hwnd)) {
                const bool keepFocusVisible = focusVisible;
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
                focusVisible = keepFocusVisible;
            }
            if (hwnd && IsWindow(hwnd))
                surface.invalidate();
            return;
        }
        if (!searchPaletteOpen || closingSearchPalette)
            return;
        closingSearchPalette = true;
        searchPaletteOpen = false;
        searchPaletteHoverKind = kSearchPaletteHitNone;
        searchPaletteHoverRow = -1;
        searchPalettePressedKind = kSearchPaletteHitNone;
        searchPalettePressedRow = -1;
        if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
            if (GetCapture() == searchPaletteHwnd)
                ReleaseCapture();
        }
        setWindowClickThrough(searchPaletteHwnd, true);
        setWindowClickThrough(searchScrimHwnd, true);
        BOOL clientAnimations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimations, 0);
        closingSearchPalette = false;
        if (restoreOwnerFocus && hwnd && IsWindow(hwnd)) {
            const bool keepFocusVisible = focusVisible;
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
            focusVisible = keepFocusVisible;
        }
        if (!clientAnimations || searchPaletteProgress <= 0.02f || !hwnd) {
            finalizeSearchPaletteClosed();
        } else {
            searchPaletteAnimating = true;
            searchPaletteAnimFrom = searchPaletteProgress;
            searchPaletteAnimTarget = 0.0f;
            searchPaletteAnimStartMs = GetTickCount64();
            searchPaletteAnimDurationMs =
                std::max(36.0f, kSearchPaletteCloseDurationMs * searchPaletteProgress);
            SetTimer(hwnd, kSearchPaletteAnimTimer, kNavIndicatorFrameMs, nullptr);
        }
        if (hwnd && IsWindow(hwnd))
            surface.invalidate();
    }

    void createSliderEditor() {
        if (!sliderValueEdit.create(hwnd, kIdSliderValueEdit, nullptr, false, 12.0f))
            return;
        sliderValueEdit.setEmbeddedNumberStyle();
        if (HWND edit = sliderValueEdit.editHwnd()) {
            LONG_PTR style = GetWindowLongPtrW(edit, GWL_STYLE);
            SetWindowLongPtrW(edit, GWL_STYLE, style | ES_NUMBER);
            SendMessageW(edit, EM_SETLIMITTEXT, 3, 0);
            SetWindowSubclass(edit, &Impl::sliderEditProc, kSliderEditSubclassId,
                              reinterpret_cast<DWORD_PTR>(this));
        }
        ShowWindow(sliderValueEdit.hwnd(), SW_HIDE);
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

    static bool isSegmentedChoice(const Row& row) {
        return row.kind == ControlKind::Radio && row.id != kIdCoverEffect &&
               row.id != kIdSpectrumStyle && row.id != kIdHoverControlStyle &&
               row.id != kIdSongToastPosition && row.id != kIdIdleQuoteBackground;
    }

    void updateHintHeights(fluent::FluentDialogSurface::Painter& painter) {
        bool changed = false;
        for (auto& row : rows[activePage]) {
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
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);

        searchTriggerRect = D2D1::RectF(16.0f, 14.0f, 12.0f + kNavW - 4.0f, 48.0f);
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
        auto& pageRows = rows[activePage];
        for (size_t i = 0; i < pageRows.size(); ++i) {
            auto& row = pageRows[i];
            // 同组设置连续排列，仅在标题和分组之间留白。
            row.gapAfter = row.kind == ControlKind::Header ? kRowGap : 0.0f;
            if (i + 1 < pageRows.size() && pageRows[i + 1].kind == ControlKind::Header)
                row.gapAfter = fluent::metrics::sectionGap;
            contentEnd += row.height + row.gapAfter;
        }
        if (!rows[activePage].empty())
            contentEnd -= rows[activePage].back().gapAfter;
        contentHeight = std::max(0.0f, contentEnd - contentTop);
        const float viewportHeight =
            std::max(0.0f, contentViewportRect.bottom - contentViewportRect.top);
        contentMaxScroll = std::max(0.0f, contentHeight - viewportHeight);
        contentScroll = std::clamp(contentScroll, 0.0f, contentMaxScroll);
        contentPaintScroll = std::round(contentScroll * surface.dipScale()) / surface.dipScale();
        updateScrollBarGeometry();

        float y = contentTop - contentPaintScroll;
        for (auto& page : rows) {
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

        for (auto& row : rows[activePage]) {
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
                const size_t count = state.idleApps.size();
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
                isSpectrumEffectRow(row.id)) {
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
                const float artworkH = isSpectrumEffectRow(row.id) || row.id == kIdSpectrumGradient
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
        if (searchPaletteOpen || searchPaletteAnimating)
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
        if (sliderEditOpen)
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
        const auto& p = fluent::palette();
        // 导航融入窗口底色，右侧设置卡片承担主要的表面层级。
        const bool triggerHovered = hoverId == kIdSettingsSearch;
        const bool triggerPressed = pressedId == kIdSettingsSearch;
        const bool triggerActive = triggerPressed || searchPaletteOpen;
        const D2D1_COLOR_F triggerFill =
            triggerActive
                ? p.listSelected
                : (triggerHovered
                       ? p.listHover
                       : D2D1::ColorF(p.listHover.r, p.listHover.g, p.listHover.b,
                                      p.listHover.a * 0.85f));
        painter.fillRoundRect(triggerFill, searchTriggerRect, 6.0f);
        if (focusedId == kIdSettingsSearch && focusVisible) {
            painter.strokeRoundRect(p.accent, searchTriggerRect, 1.5f, 6.0f);
        } else {
            painter.strokeRoundRect(
                triggerHovered || searchPaletteOpen ? p.controlStroke : p.cardStroke,
                searchTriggerRect, 1.0f, 6.0f);
        }
        const float triggerCenterY =
            (searchTriggerRect.top + searchTriggerRect.bottom) * 0.5f;
        const D2D1_RECT_F searchIconRect = D2D1::RectF(
            navRect.left + kNavIconLeft, triggerCenterY - kNavIconSize * 0.5f,
            navRect.left + kNavIconLeft + kNavIconSize, triggerCenterY + kNavIconSize * 0.5f);
        if (auto* triggerIconBrush = painter.brush(
                triggerHovered || searchPaletteOpen ? p.accent : p.textSecondary)) {
            settings_icon::draw(painter.target(), settings_icon::Kind::Search, searchIconRect,
                                triggerIconBrush, 1.3f);
        }
        const D2D1_RECT_F shortcutBadgeRect = D2D1::RectF(
            searchTriggerRect.right - 48.0f, triggerCenterY - 9.5f,
            searchTriggerRect.right - 7.0f, triggerCenterY + 9.5f);
        painter.fillRoundRect(p.listHover, shortcutBadgeRect, 4.0f);
        painter.strokeRoundRect(p.cardStroke, shortcutBadgeRect, 1.0f, 4.0f);
        painter.drawText(L"Ctrl+F", painter.textFormat(10.5f, 500, true, true),
                         shortcutBadgeRect, p.textSecondary);
        painter.drawText(
            L"搜索设置", painter.textFormat(13.5f, 400, false, true),
            D2D1::RectF(navRect.left + kNavIconLeft + kNavIconSize + kNavIconTextGap,
                        searchTriggerRect.top, shortcutBadgeRect.left - 6.0f,
                        searchTriggerRect.bottom),
            triggerHovered || searchPaletteOpen ? p.text : p.textSecondary);
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
        const auto& p = fluent::palette();
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
        const bool editing = sliderEditOpen && sliderEditRowId == row.id;
        const auto valueRect = sliderValueRect(row);
        const auto numberRect = sliderNumberRect(row);
        painter.fillRoundRect(row.enabled ? p.controlFill : p.listHover, valueRect);
        painter.strokeRoundRect(editing ? p.accent : p.controlStroke, valueRect);
        if (!editing) {
            painter.drawText(std::to_wstring(row.value),
                             painter.textFormat(12.0f, 600, true, true), numberRect,
                             row.enabled ? p.text : p.disabled);
        }
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

        if (style == 4) {
            drawLine(withOpacity(p.text, 0.65f),
                     D2D1::Point2F(left + 5.0f, top + height * 0.3f),
                     D2D1::Point2F(right - 5.0f, top + height * 0.3f), 1.3f);
            drawLine(withOpacity(p.textSecondary, 0.45f),
                     D2D1::Point2F(left + 5.0f, top + height * 0.56f),
                     D2D1::Point2F(left + width * 0.65f, top + height * 0.56f), 1.0f);
            for (int i = 0; i < 24; ++i) {
                const float x = left + i * width / 24;
                const float barH = 1.0f + height * 0.18f *
                    (0.5f + 0.5f * std::sin(i * 0.73f));
                painter.fillRoundRect(tone(withOpacity(p.accent, 0.8f)),
                    D2D1::RectF(x, bottom - barH, x + width / 40, bottom), 0.4f);
            }
            return;
        }

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

        if (style == 3) {
            constexpr int cells = 7;
            const float step = height / (cells + 1);
            for (int i = 0; i < kBars; ++i) {
                const int lit = static_cast<int>(std::ceil(kLevels[i] * cells));
                const float x = left + i * (barW + gap);
                for (int cell = 0; cell < cells; ++cell) {
                    const float y = bottom - cell * step;
                    painter.fillRoundRect(tone(withOpacity(p.accent,
                        cell < lit ? 0.85f : 0.10f)),
                        D2D1::RectF(x, y - step * 0.6f, x + barW, y), 0.2f);
                }
                const float peakY = bottom - std::min(cells - 1, lit) * step;
                drawLine(p.accent, D2D1::Point2F(x, peakY),
                         D2D1::Point2F(x + barW, peakY), 1.0f);
            }
            return;
        }

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

    void drawSpectrumEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                   const D2D1_RECT_F& bounds, SpectrumEffect effect, bool enabled,
                                   bool gradient = false) {
        auto* rt = painter.target();
        if (!rt || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return;
        const auto& p = fluent::palette();
        const auto tone = [enabled](D2D1_COLOR_F color, float opacity = 1.0f) {
            color.a *= opacity * (enabled ? 1.0f : 0.42f);
            return color;
        };
        const auto panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                      bounds.right - 1.0f, bounds.bottom - 1.0f);
        painter.fillRoundRect(tone(p.controlFill), panel, 7.0f);
        painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 7.0f);
        rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float left = panel.left + 8.0f;
        const float right = panel.right - 8.0f;
        const float baseY = panel.bottom - 6.0f;
        const float height = panel.bottom - panel.top;
        constexpr int count = 12;
        constexpr float levels[count] = {
            0.26f, 0.42f, 0.60f, 0.82f, 0.44f, 0.70f,
            0.52f, 0.90f, 0.66f, 0.86f, 0.48f, 0.30f};
        const float unit = std::max(0.0f, right - left) / (count * 5.0f + (count - 1) * 3.0f);
        const float barW = unit * 5.0f;
        const auto light = D2D1::ColorF(p.accent.r + (1.0f - p.accent.r) * 0.72f,
                                       p.accent.g + (1.0f - p.accent.g) * 0.72f,
                                       p.accent.b + (1.0f - p.accent.b) * 0.72f, p.accent.a);
        ID2D1LinearGradientBrush* sheen = nullptr;
        ID2D1LinearGradientBrush* barGradient = nullptr;
        if (gradient) {
            const auto dark = D2D1::ColorF(p.accent.r * kSpectrumBarGradientDarkFactor,
                p.accent.g * kSpectrumBarGradientDarkFactor,
                p.accent.b * kSpectrumBarGradientDarkFactor, p.accent.a);
            const auto bright = D2D1::ColorF(
                p.accent.r + (1.0f - p.accent.r) * kSpectrumBarGradientLightMix,
                p.accent.g + (1.0f - p.accent.g) * kSpectrumBarGradientLightMix,
                p.accent.b + (1.0f - p.accent.b) * kSpectrumBarGradientLightMix, p.accent.a);
            const D2D1_GRADIENT_STOP stops[] = {
                {0.0f, tone(dark)}, {1.0f, tone(bright)}};
            ID2D1GradientStopCollection* collection = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, _countof(stops), &collection))) {
                rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0, baseY), D2D1::Point2F(0, panel.top)), collection,
                    &barGradient);
                collection->Release();
            }
        }
        if (!gradient && effect == SpectrumEffect::Sheen) {
            const D2D1_GRADIENT_STOP stops[] = {
                {0.0f, tone(light, 0.0f)}, {0.38f, tone(light, 0.0f)},
                {0.48f, tone(light, 0.85f)}, {0.58f, tone(light, 0.0f)},
                {1.0f, tone(light, 0.0f)}};
            ID2D1GradientStopCollection* collection = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 5, &collection))) {
                rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(left, panel.top), D2D1::Point2F(right, panel.bottom)),
                    collection, &sheen);
                collection->Release();
            }
        }
        for (int i = 0; i < count; ++i) {
            const float x = left + i * unit * 8.0f;
            const float barH = 3.0f + levels[i] * height * 0.48f;
            const auto bar = D2D1::RectF(x, baseY - barH, x + barW, baseY);
            if (!gradient && effect == SpectrumEffect::Glow) {
                for (int layer = 4; layer >= 1; --layer) {
                    const float spread = layer * 0.85f;
                    painter.fillRoundRect(tone(p.accent, 0.035f + (4 - layer) * 0.015f),
                        D2D1::RectF(bar.left - spread, bar.top - spread,
                                     bar.right + spread, bar.bottom + spread),
                        barW * 0.5f + spread);
                }
            } else if (effect == SpectrumEffect::Trail) {
                // 两个较早的峰形从高处向当前柱顶收束，表现回落后的残留轮廓。
                const float previous = levels[(i + 3) % count];
                for (int layer = 2; layer >= 1; --layer) {
                    const float extra = (0.3f + previous * 0.7f) * layer * height * 0.10f;
                    painter.fillRoundRect(tone(p.accent, layer == 2 ? 0.12f : 0.24f),
                        D2D1::RectF(bar.left, bar.top - extra, bar.right, bar.bottom),
                        barW * 0.5f);
                }
            }
            if (barGradient) {
                const float cx = x + barW * 0.5f;
                barGradient->SetStartPoint(D2D1::Point2F(cx, bar.bottom));
                barGradient->SetEndPoint(D2D1::Point2F(cx, bar.top));
                rt->FillRoundedRectangle(D2D1::RoundedRect(bar, barW * 0.5f, barW * 0.5f),
                                         barGradient);
            } else {
                painter.fillRoundRect(tone(p.accent, 0.84f), bar, barW * 0.5f);
            }
            if (effect == SpectrumEffect::Sparks && i % 3 == 1) {
                const float cx = x + barW * 0.5f;
                if (auto* brush = painter.brush(tone(p.accent, 0.24f)))
                    rt->DrawLine(D2D1::Point2F(cx, bar.top - 1.0f),
                                 D2D1::Point2F(cx - 1.7f, bar.top - 4.0f), brush, 0.7f);
                if (auto* brush = painter.brush(tone(p.accent, 0.9f)))
                    rt->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(cx - 2.2f, bar.top - 5.2f), 1.0f, 1.0f), brush);
                if (auto* brush = painter.brush(tone(p.accent, 0.42f)))
                    rt->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(cx + 2.0f, bar.top - 8.0f), 0.7f, 0.7f), brush);
            } else if (sheen) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(bar, barW * 0.5f, barW * 0.5f), sheen);
            }
        }
        if (sheen)
            sheen->Release();
        if (barGradient)
            barGradient->Release();
        rt->PopAxisAlignedClip();
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

    void drawRadioCardBackground(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                                 const D2D1_RECT_F& card, bool selected, bool hovered,
                                 bool pressed) const {
        const auto& p = fluent::palette();
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
        const auto& p = fluent::palette();
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
        const auto& p = fluent::palette();
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
            drawSpectrumStyleArtwork(painter, artwork, static_cast<int>(i), row.enabled);

            painter.drawText(row.options[i], titleFormat,
                             D2D1::RectF(card.left + 6.0f, card.bottom - 25.0f,
                                         card.right - 6.0f, card.bottom - 6.0f),
                             row.enabled ? p.text : p.disabled);

            // 示意图和文字先画，单选框最后画，避免缩放时被内容盖住。
            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 14.0f, 14.0f);
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

        // 黑胶模式：固定圆角底座、中央唱片和右上方唱臂。
        const bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        const auto deckFill = light ? D2D1::ColorF(0.97f, 0.975f, 0.98f)
                                    : D2D1::ColorF(0.07f, 0.08f, 0.09f);
        const auto deckStroke = light ? D2D1::ColorF(0.77f, 0.79f, 0.81f) : vinylEdge;
        const auto platterRim = light ? D2D1::ColorF(0.82f, 0.84f, 0.86f)
                                      : D2D1::ColorF(0.37f, 0.39f, 0.42f);
        const auto metal = light ? D2D1::ColorF(0.86f, 0.88f, 0.90f)
                                 : D2D1::ColorF(0.65f, 0.67f, 0.69f);
        const auto armShadow = light ? D2D1::ColorF(0.45f, 0.47f, 0.49f, 0.60f) : ink;
        const auto cartridge = light ? D2D1::ColorF(0.95f, 0.96f, 0.97f) : ink;
        const auto grooveColor = light ? D2D1::ColorF(0.25f, 0.27f, 0.29f, 0.20f)
                                       : D2D1::ColorF(0.70f, 0.72f, 0.75f, 0.16f);
        const float size = std::max(0.0f, std::min(right - left, bottom - top) - 4.0f);
        const float deckLeft = (left + right - size) * 0.5f;
        const float deckTop = (top + bottom - size) * 0.5f;
        const auto point = [&](float x, float y) {
            return D2D1::Point2F(deckLeft + size * x, deckTop + size * y);
        };
        const auto deck = D2D1::RectF(deckLeft, deckTop, deckLeft + size, deckTop + size);
        painter.fillRoundRect(tone(deckFill), deck, 6.0f);
        painter.strokeRoundRect(tone(deckStroke), deck, 0.6f, 6.0f);
        const float cx = deckLeft + size * 0.5f;
        const float cy = deckTop + size * 0.5f;
        const float radius = size * 0.40f;
        const D2D1_POINT_2F center = D2D1::Point2F(cx, cy);
        fillEllipse(platterRim, center, radius);
        const float recordRadius = radius - 0.5f;
        fillEllipse(clay, center, recordRadius);
        const D2D1_GRADIENT_STOP stops[] = {
            {0.00f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f))},
            {0.22f, tone(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f))},
            {0.38f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f))},
            {0.52f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f))},
            {0.76f, tone(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f))},
            {1.00f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f))},
        };
        ID2D1GradientStopCollection* collection = nullptr;
        if (SUCCEEDED(painter.target()->CreateGradientStopCollection(stops, _countof(stops),
                D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection))) {
            ID2D1LinearGradientBrush* surface = nullptr;
            if (SUCCEEDED(painter.target()->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(cx - recordRadius * 0.8f, cy - recordRadius * 0.6f),
                        D2D1::Point2F(cx + recordRadius * 0.8f, cy + recordRadius * 0.6f)),
                    collection, &surface))) {
                painter.target()->FillEllipse(D2D1::Ellipse(center, recordRadius, recordRadius), surface);
                surface->Release();
            }
            collection->Release();
        }

        // 与真实渲染保持相同的中央封面比例。
        const float coverRadius = size * 0.25f;
        for (float groove = coverRadius + 0.9f; groove < recordRadius - 0.4f; groove += 1.1f)
            drawEllipse(grooveColor, center, groove, 0.35f);
        const D2D1_POINT_2F coverCenter = D2D1::Point2F(cx, cy);
        fillEllipse(paper, coverCenter, coverRadius);
        fillEllipse(clay,
                    D2D1::Point2F(cx - coverRadius * 0.18f, cy + coverRadius * 0.16f),
                    coverRadius * 0.44f);
        drawLine(ink, D2D1::Point2F(cx - coverRadius * 0.55f, cy - coverRadius * 0.12f),
                 D2D1::Point2F(cx + coverRadius * 0.50f, cy - coverRadius * 0.12f), 1.1f);
        drawLine(clay, D2D1::Point2F(cx - coverRadius * 0.52f, cy + coverRadius * 0.48f),
                 D2D1::Point2F(cx + coverRadius * 0.48f, cy - coverRadius * 0.34f), 1.4f);
        drawEllipse(grooveColor, coverCenter, coverRadius, 0.9f);
        fillEllipse(light ? D2D1::ColorF(0.78f, 0.80f, 0.82f) : ink, center, 1.7f);
        const auto pivot = point(0.85f, 0.17f);
        const auto elbow = point(0.81f, 0.55f);
        const auto stylus = point(0.73f, 0.75f);
        fillEllipse(armShadow, pivot, size * 0.075f);
        drawLine(armShadow, pivot, elbow, std::max(1.2f, size * 0.045f));
        drawLine(armShadow, elbow, stylus, std::max(1.2f, size * 0.045f));
        drawLine(metal, pivot, elbow, std::max(0.65f, size * 0.023f));
        drawLine(metal, elbow, stylus, std::max(0.65f, size * 0.023f));
        fillEllipse(light ? D2D1::ColorF(0.99f, 0.99f, 0.99f) : metal, pivot, size * 0.035f);
        drawLine(cartridge, point(0.75f, 0.70f), point(0.71f, 0.79f),
                 std::max(1.5f, size * 0.07f));
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
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 16.0f);

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

        const float toastW = std::min(72.0f, screen.right - screen.left - 18.0f);
        const float toastH = 20.0f;
        const float toastLeft = (screen.left + screen.right - toastW) * 0.5f;
        const float toastTop = top ? screen.top + 7.0f : screen.bottom - toastH - 7.0f;
        const D2D1_RECT_F toast =
            D2D1::RectF(toastLeft, toastTop, toastLeft + toastW, toastTop + toastH);
        const float toastRadius = 4.5f;
        painter.fillRoundRect(tone(p.listSelected), toast, toastRadius);
        painter.strokeRoundRect(tone(p.accent), toast, 1.0f, toastRadius);

        const float coverSize = toastH - 6.0f;
        const D2D1_RECT_F cover = D2D1::RectF(
            toast.left + 3.0f, toast.top + 3.0f, toast.left + 3.0f + coverSize,
            toast.bottom - 3.0f);
        painter.fillRoundRect(tone(p.accent), cover, 2.5f);
        drawLine(p.textOnAccent, D2D1::Point2F(cover.left + 2.0f, cover.top + 4.0f),
                 D2D1::Point2F(cover.right - 2.0f, cover.top + 4.0f), 0.8f);
        drawLine(p.text, D2D1::Point2F(cover.right + 4.0f, toast.top + 7.0f),
                 D2D1::Point2F(toast.right - 5.0f, toast.top + 7.0f), 1.2f);
        drawLine(p.textSecondary, D2D1::Point2F(cover.right + 4.0f, toast.top + 13.0f),
                 D2D1::Point2F(toast.right - 17.0f, toast.top + 13.0f), 1.0f);
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
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const float artworkW = std::min(100.0f, cardW - 20.0f);
            const D2D1_RECT_F artwork = D2D1::RectF(
                card.left + (cardW - artworkW) * 0.5f, card.top + 8.0f,
                card.left + (cardW + artworkW) * 0.5f, card.top + 58.0f);
            drawSongToastPositionArtwork(painter, artwork, i == 0, row.enabled);

            // 示意图先画，单选框最后画，避免位置指示被单选框遮住。
            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 16.0f);

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
                    rt->SetTransform(D2D1::Matrix3x2F::Rotation(leaves[i].angle, center) * previous);
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
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 10.0f,
                            card.bottom - 31.0f);
            drawIdleQuoteBackgroundArtwork(painter, artwork, optionIndex, row.enabled);

            drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 14.0f);

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
            drawRadioCardBackground(painter, row, card, selected, hovered, pressed);

            const D2D1_RECT_F artwork =
                D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 30.0f,
                            card.bottom - 27.0f);
            drawHoverControlStyleArtwork(painter, artwork, i == 1, row.enabled);

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

        const auto& p = fluent::palette();
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
        const auto& p = fluent::palette();
        const bool active = hoverId == kIdContentScrollBar ||
                            pressedId == kIdContentScrollBar || scrollDragging;
        painter.fillRoundRect(active ? p.accent : p.textSecondary, scrollThumbRect,
                              kScrollBarWidth * 0.5f);
    }

    void drawSettingGroups(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const auto& pageRows = rows[activePage];
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
        if (!navOnly && !contentOnly && !contentScrollAnimating)
            updateHintHeights(painter);
        const auto& p = fluent::palette();
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
        const auto& p = fluent::palette();
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
        for (auto& row : rows[activePage]) {
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
                drawSpectrumBackgroundArtwork(painter, row.artworkRect, row.enabled);
            else if (row.id == kIdSpectrumGradient)
                drawSpectrumEffectArtwork(painter, row.artworkRect, SpectrumEffect::Glow,
                                           row.enabled, true);
            else if (isSpectrumEffectRow(row.id))
                drawSpectrumEffectArtwork(painter, row.artworkRect,
                    kSpectrumEffects[row.id - kIdSpectrumEffectFirst], row.enabled);
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
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Translation(-left, contentPaintScroll - top));
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        {
            fluent::FluentDialogSurface::Painter painter(target, contentRenderer.dwrite());
            paintContent(painter);
        }
        const HRESULT result = target->EndDraw();
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
        if (!contentLayerReady)
            contentRenderer.discard();
    }

    int hitTest(float x, float y, int* option = nullptr) const {
        if (option)
            *option = -1;
        if (contains(searchTriggerRect, x, y))
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
                for (size_t i = 0; i < row.appItemRects.size(); ++i) {
                    if (contains(row.appItemRects[i], x, y)) {
                        if (option)
                            *option = kIdleAppDragOptionBase + static_cast<int>(i);
                        return row.id;
                    }
                }
                if (state.idleApps.size() < kMaxIdleApps && contains(row.addAppRect, x, y))
                    return row.id;
                continue;
            }
            bool inControl = contains(row.controlRect, x, y);
            if (row.id == kIdSpectrumBackground || row.id == kIdSpectrumGradient ||
                isSpectrumEffectRow(row.id))
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

    void onCommand(int id, int option = -1) {
        if (id >= kIdOpenDocs && id <= kIdOpenAbout) {
            const auto& action = id == kIdOpenDocs ? actions.onOpenDocs
                               : id == kIdOpenFeedback ? actions.onOpenFeedback
                                                      : actions.onOpenAbout;
            if (action)
                action();
            return;
        }
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
            state.idleQuoteEnabled = row->selected == 0;
            state.idleWelcomeEnabled = row->selected == 1;
            updateIdleRowsEnabled();
            if (actions.onIdleTextMode)
                actions.onIdleTextMode(row->selected);
            break;
        case kIdIdleCustomWelcome:
            if (actions.onEditIdleWelcome)
                actions.onEditIdleWelcome();
            break;
        case kIdTickTickEnabled: {
            const bool requested = !row->checked;
            state.tickTickEnabled = requested && state.tickTickApiTokenConfigured;
            row->checked = state.tickTickEnabled;
            updateTickTickRows();
            if (actions.onTickTickEnabled)
                actions.onTickTickEnabled(requested);
            break;
        }
        case kIdTickTickApiToken:
            if (actions.onEditTickTickApiToken)
                actions.onEditTickTickApiToken();
            break;
        case kIdTickTickConnect:
            if (actions.onTickTickConnect)
                actions.onTickTickConnect();
            break;
        case kIdTickTickRefresh:
            if (actions.onTickTickRefresh)
                actions.onTickTickRefresh();
            break;
        case kIdTickTickDisconnect:
            if (actions.onTickTickDisconnect)
                actions.onTickTickDisconnect();
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
            if (isIdleAppDragOption(option)) {
                // 应用条目的普通点击只用于启动拖动，不触发其他操作。
            } else if (option == kIdleAppNamesOption) {
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
        case kIdTaskbarContextMenu:
            row->checked = !row->checked;
            if (actions.onTaskbarContextMenu)
                actions.onTaskbarContextMenu(row->checked);
            break;
        case kIdTaskbarViewMode:
            if (row->selected == 1 &&
                (!state.mediaSessionAlive || state.verticalTaskbar)) {
                row->selected = std::clamp(state.taskbarViewMode, 0, 2);
                break;
            }
            state.taskbarViewMode = std::clamp(row->selected, 0, 2);
            updateImmersiveRowsEnabled();
            updateIdleRowsEnabled();
            if (actions.onTaskbarViewMode)
                actions.onTaskbarViewMode(state.taskbarViewMode);
            break;
        case kIdTaskbarMonitor:
            if (actions.onChooseTaskbarMonitor)
                actions.onChooseTaskbarMonitor();
            break;
        case kIdAppBarEdge:
            state.appBarEdge = std::clamp(row->selected, 0, 1);
            if (actions.onAppBarEdge)
                actions.onAppBarEdge(state.appBarEdge);
            break;
        case kIdImmersiveBackgroundAdjustment:
            state.immersiveBackgroundAdjustment = std::clamp(row->selected, 0, 1);
            updateBackgroundAdjustmentPresentation();
            if (actions.onImmersiveBackgroundAdjustment)
                actions.onImmersiveBackgroundAdjustment(
                    state.immersiveBackgroundAdjustment);
            break;
        case kIdDockBackgroundAdjustment:
            state.dockBackgroundAdjustment = std::clamp(row->selected, 0, 1);
            updateBackgroundAdjustmentPresentation();
            if (actions.onDockBackgroundAdjustment)
                actions.onDockBackgroundAdjustment(state.dockBackgroundAdjustment);
            break;
        case kIdImmersiveBackgroundBlur:
            if (actions.onImmersiveBackgroundBlur)
                actions.onImmersiveBackgroundBlur(row->value);
            break;
        case kIdDockBackgroundBlur:
            if (actions.onDockBackgroundBlur)
                actions.onDockBackgroundBlur(row->value);
            break;
        case kIdEmbeddedResourceMonitor:
            row->checked = !row->checked;
            state.embeddedResourceMonitor = row->checked;
            updateImmersiveRowsEnabled();
            updateIdleRowsEnabled();
            if (actions.onEmbeddedResourceMonitor)
                actions.onEmbeddedResourceMonitor(row->checked);
            break;
        case kIdDockResourceGpuUsage:
            row->checked = !row->checked;
            if (actions.onDockResourceGpuUsage)
                actions.onDockResourceGpuUsage(row->checked);
            break;
        case kIdDockResourceDownload:
            row->checked = !row->checked;
            state.dockResourceDownload = row->checked;
            if (actions.onDockResourceDownload)
                actions.onDockResourceDownload(row->checked);
            break;
        case kIdDockResourceUpload:
            row->checked = !row->checked;
            state.dockResourceUpload = row->checked;
            if (actions.onDockResourceUpload)
                actions.onDockResourceUpload(row->checked);
            break;
        case kIdDockResourceCpuFrequency:
            row->checked = !row->checked;
            if (actions.onDockResourceCpuFrequency)
                actions.onDockResourceCpuFrequency(row->checked);
            break;
        case kIdSpectrum:
            row->checked = !row->checked;
            for (int i = 0; i < kSpectrumEffectCount; ++i) {
                if (auto* effect = findRow(kIdSpectrumEffectFirst + i))
                    effect->enabled = row->checked;
            }
            if (auto* colorMode = findRow(kIdSpectrumColorMode))
                colorMode->enabled = row->checked;
            if (auto* color = findRow(kIdSpectrumColor))
                color->enabled = row->checked &&
                                 findRow(kIdSpectrumColorMode) &&
                                 findRow(kIdSpectrumColorMode)->selected == 2;
            if (auto* style = findRow(kIdSpectrumStyle))
                style->enabled = row->checked;
            if (auto* gradient = findRow(kIdSpectrumGradient))
                gradient->enabled = row->checked;
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
        case kIdSpectrumColorMode:
            row->selected = std::clamp(row->selected, 0, 2);
            state.spectrumCustomColor = row->selected == 2;
            state.spectrumFollowAlbum = row->selected == 1;
            if (auto* color = findRow(kIdSpectrumColor))
                color->enabled = state.spectrumCustomColor &&
                                 findRow(kIdSpectrum) &&
                                 findRow(kIdSpectrum)->checked;
            if (actions.onSpectrumColorMode)
                actions.onSpectrumColorMode(row->selected);
            break;
        case kIdSpectrumColor:
            openColorPicker(kIdSpectrumColor);
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
        case kIdSpectrumEffectFirst:
        case kIdSpectrumEffectFirst + 1:
        case kIdSpectrumEffectFirst + 2:
        case kIdSpectrumEffectFirst + 3:
            row->checked = !row->checked;
            if (actions.onSpectrumEffect)
                actions.onSpectrumEffect(row->id - kIdSpectrumEffectFirst, row->checked);
            break;
        case kIdSpectrumGradient:
            row->checked = !row->checked;
            if (actions.onSpectrumGradient)
                actions.onSpectrumGradient(row->checked);
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
            state.songToastEnabled = row->checked;
            if (auto* monitor = findRow(kIdSongToastMonitor))
                monitor->enabled = row->checked;
            if (auto* preview = findRow(kIdSongToastPreview))
                preview->enabled = row->checked && state.songToastPreviewAvailable;
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
        case kIdSongToastMonitor:
            state.songToastFollowMouseMonitor = row->selected == 1;
            if (actions.onSongToastFollowMouseMonitor)
                actions.onSongToastFollowMouseMonitor(state.songToastFollowMouseMonitor);
            break;
        case kIdSongToastPreview:
            if (actions.onPreviewSongToast)
                actions.onPreviewSongToast();
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
        if (sliderEditOpen)
            commitSliderEditor();
        state = s;
        const bool vertical = s.verticalTaskbar;
        resetIdleAppDrag();
        if (auto* row = findRow(kIdIdleEntry))
            row->checked = s.idleEntryEnabled;
        if (auto* row = findRow(kIdIdleQuote))
            row->selected = idleTextMode();
        if (auto* row = findRow(kIdIdleQuoteSource))
            row->selected = std::clamp(s.idleQuoteSource, 0, 2);
        if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
            row->selected = std::clamp(s.idleQuoteRefreshInterval, 0, 2);
        if (auto* row = findRow(kIdIdleQuoteAlignment))
            row->selected = vertical ? 0 : std::clamp(s.idleQuoteAlignment, 0, 2);
        if (auto* row = findRow(kIdIdleQuoteBackground))
            row->selected = std::clamp(s.idleQuoteBackground, 0, 4);
        if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
            row->selected = std::clamp(s.idleQuoteBackgroundScope, 0, 3);
        if (auto* row = findRow(kIdIdleApps))
            row->checked = s.idleAppNamesVisible;
        if (auto* row = findRow(kIdTickTickEnabled))
            row->checked = s.tickTickEnabled;
        updateIdleQuoteSourceRow();
        updateIdleAppsRowHeight();
        updateTickTickRows();
        const bool minimal = s.renderMode == kRenderModeMinimal;
        if (auto* row = findRow(kIdSongInfo)) {
            row->checked = vertical ? false : s.songInfoVisible;
            row->enabled = !vertical;
        }
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
        if (auto* row = findRow(kIdTaskbarContextMenu))
            row->checked = s.taskbarContextMenu;
        if (auto* row = findRow(kIdTaskbarViewMode)) {
            row->selected = std::clamp(s.taskbarViewMode, 0, 2);
            row->hint = taskbarViewModeHint(s.mediaSessionAlive, vertical);
            row->showHint = true;
        }
        if (auto* row = findRow(kIdTaskbarMonitor)) {
            row->controlText = s.taskbarMonitorLabel;
            row->enabled = s.taskbarViewMode != 0;
        }
        if (auto* row = findRow(kIdAppBarEdge)) {
            row->selected = std::clamp(s.appBarEdge, 0, 1);
            row->enabled = s.taskbarViewMode == 2;
        }
        if (auto* row = findRow(kIdImmersiveBackgroundAdjustment)) {
            row->selected = std::clamp(s.immersiveBackgroundAdjustment, 0, 1);
            row->enabled = !vertical && s.taskbarViewMode == 1 &&
                           s.backgroundBlurSupported;
        }
        if (auto* row = findRow(kIdDockBackgroundAdjustment)) {
            row->selected = std::clamp(s.dockBackgroundAdjustment, 0, 1);
            row->enabled = s.taskbarViewMode == 2 && s.backgroundBlurSupported;
        }
        if (auto* row = findRow(kIdImmersiveBackgroundBlur)) {
            row->value = std::clamp(s.immersiveBackgroundBlur, 0, 100);
            row->enabled = !vertical && s.taskbarViewMode == 1 &&
                           s.backgroundBlurSupported;
            row->hint = backgroundAdjustmentHint(
                s.backgroundBlurSupported, s.immersiveBackgroundAdjustment);
            row->showHint = true;
        }
        if (auto* row = findRow(kIdDockBackgroundBlur)) {
            row->value = std::clamp(s.dockBackgroundBlur, 0, 100);
            row->enabled = s.taskbarViewMode == 2 && s.backgroundBlurSupported;
            row->hint = backgroundAdjustmentHint(
                s.backgroundBlurSupported, s.dockBackgroundAdjustment);
            row->showHint = true;
        }
        updateBackgroundAdjustmentPresentation();
        if (auto* row = findRow(kIdEmbeddedResourceMonitor)) {
            row->checked = s.embeddedResourceMonitor;
            row->enabled = s.taskbarViewMode == 0 && !vertical;
        }
        const bool resourceExtrasEnabled = s.taskbarViewMode == 2 ||
            (s.taskbarViewMode == 0 && !vertical && s.embeddedResourceMonitor);
        if (auto* row = findRow(kIdDockResourceDownload)) {
            row->checked = s.dockResourceDownload;
            row->enabled = resourceExtrasEnabled;
        }
        if (auto* row = findRow(kIdDockResourceUpload)) {
            row->checked = s.dockResourceUpload;
            row->enabled = resourceExtrasEnabled;
        }
        if (auto* row = findRow(kIdDockResourceGpuUsage)) {
            row->checked = s.dockResourceGpuUsage;
            row->enabled = resourceExtrasEnabled;
        }
        if (auto* row = findRow(kIdDockResourceCpuFrequency)) {
            row->checked = s.dockResourceCpuFrequency;
            row->enabled = resourceExtrasEnabled;
        }
        if (auto* row = findRow(kIdSpectrum)) {
            row->checked = minimal || vertical ? false : s.spectrumOn;
            row->enabled = !minimal && !vertical;
        }
        if (auto* row = findRow(kIdSpectrumColorMode)) {
            row->selected = vertical ? 0
                                     : (s.spectrumCustomColor ? 2
                                                               : s.spectrumFollowAlbum ? 1 : 0);
            row->enabled = s.spectrumOn && !minimal && !vertical;
        }
        if (auto* row = findRow(kIdSpectrumColor)) {
            row->controlText = colorText(s.spectrumColor);
            row->enabled = s.spectrumOn && s.spectrumCustomColor && !minimal && !vertical;
        }
        if (auto* row = findRow(kIdSpectrumStyle)) {
            row->selected = vertical ? 0 : std::clamp(s.spectrumStyle, 0, 4);
            row->enabled = s.spectrumOn && !minimal && !vertical;
        }
        if (auto* row = findRow(kIdSpectrumGradient)) {
            row->checked = minimal || vertical ? false : s.spectrumGradient;
            row->enabled = s.spectrumOn && !minimal && !vertical;
        }
        for (int i = 0; i < kSpectrumEffectCount; ++i) {
            if (auto* row = findRow(kIdSpectrumEffectFirst + i)) {
                row->checked = (s.spectrumEffects & spectrumEffectBit(kSpectrumEffects[i])) != 0;
                row->enabled = s.spectrumOn && !minimal && !vertical;
            }
        }
        if (auto* row = findRow(kIdSpectrumBackground)) {
            row->checked = minimal || vertical ? false : s.spectrumBackground;
            row->enabled = s.spectrumOn && s.spectrumStyle == 2 && !minimal && !vertical;
        }
        if (auto* row = findRow(kIdSpectrumOpacity)) {
            row->value = std::clamp(s.spectrumOpacity, 0, 100);
            row->enabled = s.spectrumOn && s.spectrumStyle == 2 && s.spectrumBackground &&
                           !minimal && !vertical;
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
        if (auto* row = findRow(kIdSongToastMonitor)) {
            row->selected = s.songToastFollowMouseMonitor ? 1 : 0;
            row->enabled = !minimal && s.songToastEnabled;
        }
        if (auto* row = findRow(kIdSongToastPreview))
            row->enabled = !minimal && s.songToastEnabled && s.songToastPreviewAvailable;
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
        if (auto* row = findRow(kIdDoubleLine)) {
            row->checked = vertical ? false : s.doubleLineLyrics;
            row->enabled = !vertical;
        }
        if (auto* row = findRow(kIdAlignment)) {
            row->selected = vertical ? 0 : s.lyricAlignment;
            row->enabled = !vertical && s.taskbarViewMode == 0;
        }
        if (auto* row = findRow(kIdSecondaryOn)) {
            row->checked = vertical ? false : s.secondaryEnabled;
            row->enabled = !vertical;
        }
        if (auto* row = findRow(kIdSecondaryType)) {
            row->hint = s.secondaryAvailability == 1
                            ? L"正在检查翻译和罗马音…"
                            : s.secondaryAvailability == 2 ? L"当前歌曲无翻译或罗马音" : L"";
            row->showHint = !row->hint.empty();
            row->minHeight = row->showHint ? kRowTallH : kRowH;
            row->height = row->minHeight;
            row->selected = vertical ? 0 : (s.preferRomanization ? 1 : 0);
            row->enabled = !vertical && s.secondaryEnabled && s.secondaryAvailability == 0;
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
        updateImmersiveRowsEnabled();
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
        case WM_TIMER:
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
            searchQuery.clear();
            searchResults.clear();
            selectedSearchResult = -1;
            searchLocatedId = 0;
            searchLocatedStartMs = 0;
            searchLocateFading = false;
            searchLocateInitialCursorPt = {LONG_MIN, LONG_MIN};
            backdrop = fluent::styleDialogWindow(hwnd, false);
            surface.initialize(hwnd, backdrop, true);
            if (!createContentWindow())
                return -1;
            createControls();
            createSliderEditor();
            layout();
            return 0;
        case WM_MOVE:
            if (searchPaletteOpen || searchPaletteAnimating)
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
            sliderValueEdit.refreshTheme();
            searchScrimLastWidth = 0;
            searchScrimLastHeight = 0;
            if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
                styleSearchPaletteWindow();
                settingsSearch.refreshTheme();
                if (searchPaletteOpen)
                    layoutSearchPalette();
                searchPaletteSurface.invalidate();
            }
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
                    if (index >= 0 && static_cast<size_t>(index) < state.idleApps.size()) {
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
            const bool suppressSearchTrigger = suppressNextSearchTriggerPress;
            suppressNextSearchTriggerPress = false;
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
                if (index >= 0 && static_cast<size_t>(index) < state.idleApps.size()) {
                    idleAppDragIndex = index;
                    idleAppDragStartX = pointerX;
                    idleAppDragStartY = pointerY;
                }
            }
            if (pressedId != 0) {
                Row* row = findRow(pressedId);
                if (row && row->kind == ControlKind::Slider && row->enabled &&
                    !contains(sliderValueRect(*row), pointerX, pointerY))
                    updateSliderFromPointer(*row, pointerX);
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
            if (idleAppDragging) {
                const int fromIndex = idleAppDragIndex;
                const int toIndex = idleAppDropIndex;
                const bool validDrop = fromIndex >= 0 &&
                                       static_cast<size_t>(fromIndex) < state.idleApps.size() &&
                                       toIndex >= 0 &&
                                       toIndex <= static_cast<int>(state.idleApps.size());
                resetIdleAppDrag();
                pressedId = 0;
                pressedOption = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                if (validDrop && actions.onReorderIdleApps)
                    actions.onReorderIdleApps(fromIndex, toIndex);
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
        case WM_KEYDOWN:
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
            focusVisible = false;
            surface.invalidate();
            return 0;
        case kMsgColorPickerClosed:
            if (colorPicker && !colorPicker->isOpen())
                colorPicker.reset();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kIdSliderValueEdit) {
                if (HIWORD(wp) == EN_KILLFOCUS)
                    commitSliderEditor();
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == LBN_SELCHANGE)
                onCommand(LOWORD(wp));
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            stopContentScroll();
            stopNavIndicator();
            stopSearchLocateHighlight();
            KillTimer(hwnd, kSearchPaletteAnimTimer);
            searchPaletteAnimating = false;
            if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
                DestroyWindow(searchPaletteHwnd);
                searchPaletteHwnd = nullptr;
            }
            if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
                DestroyWindow(searchScrimHwnd);
                searchScrimHwnd = nullptr;
            }
            searchScrimRenderer.releaseAll();
            searchPaletteOpen = false;
            searchPaletteSurface.discard();
            contentRenderer.releaseAll();
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
        if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
            DestroyWindow(searchPaletteHwnd);
            searchPaletteHwnd = nullptr;
        }
        if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
            DestroyWindow(searchScrimHwnd);
            searchScrimHwnd = nullptr;
        }
        searchScrimRenderer.releaseAll();
        if (sliderEditOpen)
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
    impl_->state = state;
    impl_->actions = std::move(actions);

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
