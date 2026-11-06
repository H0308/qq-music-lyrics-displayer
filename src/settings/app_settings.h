#pragma once

#include "idle/idle_types.h"
#include "ui/font_color_dialog.h"
#include "ui/fluent_theme.h"
#include "ui/taskbar_host.h"
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <string>
#include <vector>

// 仅包含持久化的偏好与内容缓存；异步请求、窗口和播放状态不属于配置。
struct AppSettings {
    // 字体状态（作为字体选择器的记忆源）。hasUserFont_ 为 false 时各宿主使用各自的默认字体。
    bool hasUserFont_ = false;
    std::wstring fontFamily_ = fluent::uiFontFamily(); // 与普通窗口默认字体族一致
    float fontSize_ = 16.0f;
    LyricFontStyle fontStyle_ = LyricFontStyle::Normal;
    bool lyricAppearanceGlobal_ = false;
    FontColorDialog::ThemeState lightLyricAppearance_;
    FontColorDialog::ThemeState darkLyricAppearance_;
    bool hasGlobalLyricAppearance_ = false;
    FontColorDialog::ThemeState globalLyricAppearance_;
    bool idleEntryEnabled_ = true;
    bool idleQuoteEnabled_ = true;
    bool idleWelcomeEnabled_ = true;
    bool idleAppNamesVisible_ = true;
    std::wstring idleCustomWelcome_;
    IdleQuoteSource idleQuoteSource_ = IdleQuoteSource::Hitokoto;
    IdleQuoteRefreshInterval idleQuoteRefreshInterval_ = IdleQuoteRefreshInterval::Daily;
    IdleQuoteBackground idleQuoteBackground_ = IdleQuoteBackground::None;
    IdleQuoteBackgroundScope idleQuoteBackgroundScope_ = IdleQuoteBackgroundScope::DailyQuote;
    std::wstring jinrishiciToken_;
    bool tickTickEnabled_ = false;
    std::vector<IdleAppInfo> idleApps_;
    std::string idleQuoteCacheSource_;
    std::string idleQuoteCachePeriod_;
    std::wstring idleQuoteCacheText_;
    std::wstring idleQuoteCacheOrigin_;
    std::wstring idleQuoteCacheUuid_;
    int holidayCalendarYear_ = 0;
    int64_t holidayCalendarUpdatedAt_ = 0;
    std::vector<HolidayDayInfo> holidayCalendar_;
    int taskbarPosition_ = 0;
    TaskbarViewMode taskbarViewMode_ = TaskbarViewMode::Embedded;
    std::wstring taskbarMonitorId_;
    AppBarEdge appBarEdge_ = AppBarEdge::Top;
    int immersiveBackgroundBlur_ = 88;
    int dockBackgroundBlur_ = 88;
    int immersiveBackgroundAdjustment_ = 0;
    int dockBackgroundAdjustment_ = 0;
    bool embeddedResourceMonitor_ = false;
    bool dockResourceGpuUsage_ = false;
    bool dockResourceDownload_ = true;
    bool dockResourceUpload_ = true;
    bool dockResourceCpuFrequency_ = false;
    bool taskbarContextMenuEnabled_ = true;
    bool hoverPlaybackControls_ = true;
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    MediaPopupTrigger floatingCardTrigger_ = MediaPopupTrigger::Hover;
    MediaPopupBackground floatingCardBackground_ = MediaPopupBackground::Solid;
    bool floatingCardBackgroundColorCustomized_ = false;
    COLORREF floatingCardBackgroundColor_ = RGB(255, 255, 255);
    bool floatingCardFollowAlbum_ = false;
    bool floatingCardAutoTextContrast_ = true;
    bool songToastEnabled_ = false;
    int songToastDurationSec_ = 4;
    bool songToastSkipFullscreen_ = true; // 前台有全屏应用时不弹出
    bool songToastTop_ = false;           // true 中上，false 中下（默认）
    bool songToastFollowMouseMonitor_ = false;
    fluent::ThemeMode taskbarThemeMode_ = fluent::ThemeMode::FollowSystem;
    fluent::ThemeMode windowThemeMode_ = fluent::ThemeMode::FollowApp;
    bool spectrumOn_ = false;
    SpectrumStyle spectrumStyle_ = SpectrumStyle::Default;
    COLORREF spectrumColor_ = RGB(49, 194, 124);
    bool spectrumCustomColor_ = false;
    bool spectrumFollowAlbum_ = false;
    bool spectrumGradient_ = false;
    uint32_t spectrumEffects_ = 0;
    bool spectrumBackground_ = false;
    int spectrumOpacity_ = 40;
    bool progressBackground_ = false;
    int progressBackgroundOpacity_ = 25;
    int taskbarBackground_ = 0;
    int coverBackgroundOpacity_ = 60;
    bool autoCheckOnStartup_ = true;
    bool useGiteeUpdateSource_ = false;
    bool lyricFollowAlbum_ = false;
    bool secondaryLyricEnabled_ = true;
    bool preferRomanization_ = false;
    bool doubleLineLyricsEnabled_ = false;
    uint32_t lyricEffects_ = 0;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    LyricAlignment idleQuoteAlignment_ = LyricAlignment::Left;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;
    bool qqLocalLyricsEnabled_ = false;
    bool qqLocalLyricsPersistOrder_ = false;
    std::wstring qqLocalLyricsPath_;
    std::wstring logDirectory_;
    int logRetentionDays_ = 30;

    // 保留既有字段读取规则及主题初始化顺序；迁移标志在部分字段异常时仍保留。
    void readJson(const nlohmann::json& document, bool hasTickTickToken,
                  bool& migrateLegacyLogDirectory);
    nlohmann::json toJson() const;
};
