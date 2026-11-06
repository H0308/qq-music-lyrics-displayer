#pragma once
#include "ui/settings_dialog.h"
#include "ui/settings_icons.h"
#include "ui/settings/ids.h"

namespace settings_ui {
inline fluent::ThemeMode themeModeFromIndex(int index) {
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

inline int themeModeIndex(fluent::ThemeMode mode) {
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

inline const wchar_t* idleQuoteSourceHint(int source) {
    if (source == 1)
        return L"今日诗词会根据客户端网络 IP、时间等信息进行推荐，并在本地保存接口 Token。";
    if (source == 2)
        return L"通过一言的网易云分类获取每日一句";
    return L"默认使用一言获取每日一言。";
}

inline const wchar_t* taskbarViewModeHint(bool mediaSessionAlive, bool verticalTaskbar) {
    if (!mediaSessionAlive)
        return L"当前没有音乐播放，沉浸模式暂不可用；开始播放后即可选择。Dock 模式仍可使用。";
    if (verticalTaskbar)
        return L"侧边任务栏不支持沉浸模式；Dock 模式以独立顶栏或底栏占用桌面工作区。";
    return L"沉浸模式覆盖 Windows 任务栏；Dock 模式以独立顶栏或底栏占用桌面工作区。";
}

inline const wchar_t* backgroundAdjustmentHint(bool supported, int mode) {
    if (!supported)
        return L"背景模糊层初始化失败，请查看运行日志中的 dock-backdrop 信息。";
    return mode == 1
               ? L"0% 完全透明，100% 显示随 Windows 应用主题变化的纯底色；中间值为底色半透明。"
               : L"0% 完全透明，100% 完全模糊；中间值混合清晰桌面与模糊背景。";
}

inline const wchar_t* backgroundAdjustmentLabel(bool immersive, int mode) {
    if (immersive)
        return mode == 1 ? L"沉浸模式背景不透明度" : L"沉浸模式背景模糊程度";
    return mode == 1 ? L"背景不透明度" : L"背景模糊程度";
}

inline constexpr const wchar_t* kFontSettingNotice =
    L"字体修改不会影响到界面字体，只会影响到任务栏歌词、每日一言、歌曲信息、沉浸模式时间/日期和 Dock、内嵌模式资源监视";
inline settings_icon::Kind iconForRow(int id) {
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
    case kIdLyricEffectFirst:
    case kIdLyricEffectFirst + 1:
    case kIdLyricEffectFirst + 2:
    case kIdLyricEffectFirst + 3:
        return settings_icon::Kind::Lyrics;
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

} // namespace settings_ui
