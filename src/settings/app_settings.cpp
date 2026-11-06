#include "settings/app_settings.h"
#include "ui/fluent_theme.h"
#include "ui/spectrum_effects.h"
#include "ui/lyric_effects.h"
#include "logging/runtime_logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cwctype>
#include <utility>

namespace {
constexpr size_t kIdleCustomWelcomeMaxLen = 20;
std::string utf8Of(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wideOf(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

COLORREF defaultFloatingCardBackgroundColor() {
    return fluent::isDarkMode(fluent::ThemeTarget::Window) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

LyricFontStyle fontStyleOf(const std::string& value) {
    if (value == "bold")
        return LyricFontStyle::Bold;
    if (value == "italic")
        return LyricFontStyle::Italic;
    if (value == "boldItalic")
        return LyricFontStyle::BoldItalic;
    return LyricFontStyle::Normal;
}

const char* fontStyleName(LyricFontStyle style) {
    switch (style) {
    case LyricFontStyle::Bold:
        return "bold";
    case LyricFontStyle::Italic:
        return "italic";
    case LyricFontStyle::BoldItalic:
        return "boldItalic";
    default:
        return "normal";
    }
}

SpectrumStyle spectrumStyleFromConfig(const std::string& value) {
    if (value == "segmented")
        return SpectrumStyle::Segmented;
    if (value == "bottom-band")
        return SpectrumStyle::BottomBand;
    if (value == "bars" || value == "rounded-bars") {
        return SpectrumStyle::Bars;
    }
    if (value == "dreamy-wave") {
        return SpectrumStyle::DreamyWave;
    }
    if (value == "background-wave") {
        // 旧版本的背景波浪是“梦幻波浪 + 背景开关”的组合。
        return SpectrumStyle::DreamyWave;
    }
    return SpectrumStyle::Default;
}

const char* spectrumStyleConfigName(SpectrumStyle style) {
    switch (style) {
    case SpectrumStyle::Bars:
        return "bars";
    case SpectrumStyle::DreamyWave:
        return "dreamy-wave";
    case SpectrumStyle::Segmented:
        return "segmented";
    case SpectrumStyle::BottomBand:
        return "bottom-band";
    case SpectrumStyle::Default:
    default:
        return "default";
    }
}

IdleQuoteBackground idleQuoteBackgroundFromConfig(const std::string& value) {
    if (value == "leaves")
        return IdleQuoteBackground::FallingLeaves;
    if (value == "stars")
        return IdleQuoteBackground::TwinklingStars;
    if (value == "binary")
        return IdleQuoteBackground::BinaryRain;
    if (value == "particles")
        return IdleQuoteBackground::FloatingParticles;
    return IdleQuoteBackground::None;
}

const char* idleQuoteBackgroundConfigName(IdleQuoteBackground background) {
    switch (background) {
    case IdleQuoteBackground::FallingLeaves:
        return "leaves";
    case IdleQuoteBackground::TwinklingStars:
        return "stars";
    case IdleQuoteBackground::BinaryRain:
        return "binary";
    case IdleQuoteBackground::FloatingParticles:
        return "particles";
    case IdleQuoteBackground::None:
    default:
        return "none";
    }
}

IdleQuoteBackgroundScope idleQuoteBackgroundScopeFromConfig(const std::string& value) {
    if (value == "none")
        return IdleQuoteBackgroundScope::None;
    if (value == "lyrics")
        return IdleQuoteBackgroundScope::Lyrics;
    if (value == "all")
        return IdleQuoteBackgroundScope::All;
    return IdleQuoteBackgroundScope::DailyQuote;
}

const char* idleQuoteBackgroundScopeConfigName(IdleQuoteBackgroundScope scope) {
    switch (scope) {
    case IdleQuoteBackgroundScope::None:
        return "none";
    case IdleQuoteBackgroundScope::Lyrics:
        return "lyrics";
    case IdleQuoteBackgroundScope::All:
        return "all";
    case IdleQuoteBackgroundScope::DailyQuote:
    default:
        return "daily-quote";
    }
}

const char* idleQuoteSourceConfigName(IdleQuoteSource source) {
    return source == IdleQuoteSource::Jinrishici ? "jinrishici"
           : source == IdleQuoteSource::Netease  ? "netease"
                                                 : "hitokoto";
}

IdleQuoteSource idleQuoteSourceFromConfig(const std::string& value) {
    return value == "jinrishici" ? IdleQuoteSource::Jinrishici
           : value == "netease"  ? IdleQuoteSource::Netease
                                 : IdleQuoteSource::Hitokoto;
}

std::wstring normalizeIdleCustomWelcome(std::wstring text) {
    if (text.size() > kIdleCustomWelcomeMaxLen)
        text.resize(kIdleCustomWelcomeMaxLen);
    const bool allSpace =
        std::all_of(text.begin(), text.end(), [](wchar_t c) { return iswspace(c) != 0; });
    if (allSpace)
        text.clear();
    return text;
}

const char* idleQuoteIntervalConfigName(IdleQuoteRefreshInterval interval) {
    switch (interval) {
    case IdleQuoteRefreshInterval::HalfDay:
        return "half-day";
    case IdleQuoteRefreshInterval::Hourly:
        return "hourly";
    case IdleQuoteRefreshInterval::Daily:
    default:
        return "daily";
    }
}

IdleQuoteRefreshInterval idleQuoteIntervalFromConfig(const std::string& value) {
    if (value == "half-day")
        return IdleQuoteRefreshInterval::HalfDay;
    if (value == "hourly")
        return IdleQuoteRefreshInterval::Hourly;
    return IdleQuoteRefreshInterval::Daily;
}

fluent::ThemeMode themeModeFromConfig(const std::string& value, fluent::ThemeMode fallback) {
    if (value == "system")
        return fluent::ThemeMode::FollowSystem;
    if (value == "app")
        return fluent::ThemeMode::FollowApp;
    if (value == "light")
        return fluent::ThemeMode::Light;
    if (value == "dark")
        return fluent::ThemeMode::Dark;
    return fallback;
}

const char* themeModeConfigName(fluent::ThemeMode mode) {
    switch (mode) {
    case fluent::ThemeMode::FollowApp:
        return "app";
    case fluent::ThemeMode::Light:
        return "light";
    case fluent::ThemeMode::Dark:
        return "dark";
    case fluent::ThemeMode::FollowSystem:
    default:
        return "system";
    }
}
} // namespace

void AppSettings::readJson(const nlohmann::json& j, bool hasTickTickToken,
                           bool& migrateLegacyLogDirectory) {
    tickTickEnabled_ = j.value("tickTickEnabled", hasTickTickToken);
    const std::wstring configuredLogDirectory =
        wideOf(j.value("logDirectory", utf8Of(logDirectory_)));
    if (configuredLogDirectory == runtime_log::RuntimeLogger::legacyDefaultDirectory())
        migrateLegacyLogDirectory = true;
    else if (!configuredLogDirectory.empty())
        logDirectory_ = configuredLogDirectory;
    logRetentionDays_ = std::clamp(j.value("logRetentionDays", 30), 0, 3650);
    hasUserFont_ = j.value("hasUserFont", false);
    std::wstring fam = wideOf(j.value("fontFamily", std::string()));
    if (!fam.empty())
        fontFamily_ = fam;
    fontSize_ = (float)j.value("fontSize", 16.0);
    fontStyle_ = fontStyleOf(j.value("fontStyle", std::string("normal")));
    LyricAppearance legacyAppearance;
    legacyAppearance.played = (COLORREF)j.value("lyricColor", (unsigned)legacyAppearance.played);
    // 未播放色默认跟随已播放色：旧配置进入双主题后视觉保持一致。
    legacyAppearance.unplayed =
        (COLORREF)j.value("lyricUnplayedColor", (unsigned)legacyAppearance.played);
    legacyAppearance.unplayedAlphaPct =
        std::clamp(j.value("lyricUnplayedAlpha", legacyAppearance.unplayedAlphaPct), 5, 100);
    // 光晕色默认跟随已播放色、描边色默认纯黑：旧配置进入双主题后视觉保持一致。
    legacyAppearance.glowColor =
        (COLORREF)j.value("lyricGlowColor", (unsigned)legacyAppearance.played);
    legacyAppearance.outlineColor =
        (COLORREF)j.value("lyricOutlineColor", (unsigned)legacyAppearance.outlineColor);
    legacyAppearance.glowOn = j.value("lyricGlow", legacyAppearance.glowOn);
    // 旧配置 lyricGlow 是描边+光晕总开关，升级时描边默认跟随它。
    legacyAppearance.outlineOn = j.value("lyricOutline", legacyAppearance.glowOn);

    auto loadAppearance = [&j](const char* suffix, LyricAppearance fallback) {
        const std::string suffixValue = suffix;
        fallback.played = static_cast<COLORREF>(
            j.value("lyricColor" + suffixValue, static_cast<unsigned>(fallback.played)));
        fallback.unplayed = static_cast<COLORREF>(
            j.value("lyricUnplayedColor" + suffixValue, static_cast<unsigned>(fallback.unplayed)));
        fallback.unplayedAlphaPct = std::clamp(
            j.value("lyricUnplayedAlpha" + suffixValue, fallback.unplayedAlphaPct), 5, 100);
        fallback.glowColor = static_cast<COLORREF>(
            j.value("lyricGlowColor" + suffixValue, static_cast<unsigned>(fallback.glowColor)));
        fallback.outlineColor = static_cast<COLORREF>(j.value(
            "lyricOutlineColor" + suffixValue, static_cast<unsigned>(fallback.outlineColor)));
        fallback.glowOn = j.value("lyricGlow" + suffixValue, fallback.glowOn);
        fallback.outlineOn = j.value("lyricOutline" + suffixValue, fallback.outlineOn);
        return fallback;
    };
    lightLyricAppearance_ = loadAppearance("Light", legacyAppearance);
    darkLyricAppearance_ = loadAppearance("Dark", legacyAppearance);
    hasGlobalLyricAppearance_ = j.contains("lyricColorGlobal");
    globalLyricAppearance_ = loadAppearance("Global", lightLyricAppearance_);
    lyricAppearanceGlobal_ = j.value("lyricAppearanceGlobal", false);
    if (lyricAppearanceGlobal_ && !hasGlobalLyricAppearance_) {
        globalLyricAppearance_ = fluent::isDarkMode(fluent::ThemeTarget::Taskbar)
                                     ? darkLyricAppearance_
                                     : lightLyricAppearance_;
        hasGlobalLyricAppearance_ = true;
    }
    taskbarPosition_ = std::clamp(j.value("taskbarPosition", 0), 0, 1);
    const int savedViewMode = j.contains("taskbarViewMode")
                                  ? j.value("taskbarViewMode", 0)
                                  : (j.value("taskbarImmersive", false) ? 1 : 0);
    taskbarViewMode_ = static_cast<TaskbarViewMode>(std::clamp(savedViewMode, 0, 2));
    taskbarMonitorId_ = wideOf(j.value("taskbarMonitor", std::string()));
    appBarEdge_ = j.value("appBarEdge", 0) == 1 ? AppBarEdge::Bottom : AppBarEdge::Top;
    const int legacyImmersiveMaskOpacity = std::clamp(j.value("immersiveMaskOpacity", 88), 0, 100);
    const int legacyDockMaskOpacity =
        std::clamp(j.value("dockMaskOpacity", legacyImmersiveMaskOpacity), 0, 100);
    const int legacyDockBlur = std::clamp(j.value("dockBackgroundBlur", 100), 0, 100);
    const int legacyDockBlurLevel = (legacyDockMaskOpacity * legacyDockBlur + 50) / 100;
    immersiveBackgroundBlur_ =
        std::clamp(j.value("immersiveBackgroundBlur", legacyImmersiveMaskOpacity), 0, 100);
    dockBackgroundBlur_ =
        std::clamp(j.value("dockBackgroundBlurLevel", legacyDockBlurLevel), 0, 100);
    immersiveBackgroundAdjustment_ = std::clamp(j.value("immersiveBackgroundAdjustment", 0), 0, 1);
    dockBackgroundAdjustment_ = std::clamp(j.value("dockBackgroundAdjustment", 0), 0, 1);
    embeddedResourceMonitor_ = j.value("embeddedResourceMonitor", false);
    dockResourceGpuUsage_ = j.value("dockResourceGpu", false);
    dockResourceDownload_ = j.value("dockResourceDownload", true);
    dockResourceUpload_ = j.value("dockResourceUpload", true);
    dockResourceCpuFrequency_ = j.value("dockResourceCpuFrequency", false);
    taskbarContextMenuEnabled_ = j.value("taskbarContextMenu", true);
    // 性能模式只对本次运行有效；忽略旧版本可能留下的持久化值，启动始终回到正常模式。
    hoverPlaybackControls_ = j.value("hoverPlaybackControls", true);
    hoverControlStyle_ =
        j.value("hoverControlStyle", 0) == 1 ? HoverControlStyle::Popup : HoverControlStyle::Inline;
    const int floatingCardTrigger = j.value("floatingCardTrigger", j.value("mediaPopupTrigger", 0));
    floatingCardTrigger_ =
        floatingCardTrigger == 1 ? MediaPopupTrigger::Click : MediaPopupTrigger::Hover;
    const std::string floatingCardBackgroundValue = j.value(
        "floatingCardBackground",
        j.value("mediaPopupBackground", j.value("idleCardBackground", std::string("solid"))));
    floatingCardBackground_ = floatingCardBackgroundValue == "frosted"
                                  ? MediaPopupBackground::Frosted
                                  : MediaPopupBackground::Solid;
    floatingCardFollowAlbum_ =
        j.value("floatingCardFollowAlbum",
                j.value("mediaPopupFollowAlbum", j.value("idleCardFollowAlbum", false)));
    floatingCardAutoTextContrast_ =
        j.value("floatingCardAutoTextContrast", j.value("mediaPopupAutoTextContrast", true));
    songToastEnabled_ = j.value("songToast", false);
    songToastDurationSec_ = std::clamp(j.value("songToastDuration", 4), 1, 10);
    songToastSkipFullscreen_ = j.value("songToastSkipFullscreen", true);
    songToastTop_ = j.value("songToastPosition", std::string("bottom")) == "top";
    songToastFollowMouseMonitor_ = j.value("songToastFollowMouseMonitor", false);
    taskbarThemeMode_ = themeModeFromConfig(j.value("taskbarTheme", std::string("system")),
                                            fluent::ThemeMode::FollowSystem);
    windowThemeMode_ = themeModeFromConfig(j.value("windowTheme", std::string("app")),
                                           fluent::ThemeMode::FollowApp);
    fluent::setThemeModes(taskbarThemeMode_, windowThemeMode_);
    floatingCardBackgroundColorCustomized_ = j.contains("floatingCardBackgroundColor") ||
                                             j.contains("idleCardBackgroundColor") ||
                                             j.contains("mediaPopupBackgroundColor");
    floatingCardBackgroundColor_ = static_cast<COLORREF>(
        j.value("floatingCardBackgroundColor",
                j.value("idleCardBackgroundColor",
                        j.value("mediaPopupBackgroundColor",
                                static_cast<unsigned>(defaultFloatingCardBackgroundColor())))));
    spectrumOn_ = j.value("spectrum", false);
    const std::string spectrumStyleValue = j.value("spectrumStyle", std::string("default"));
    spectrumStyle_ = spectrumStyleFromConfig(spectrumStyleValue);
    spectrumColor_ =
        static_cast<COLORREF>(j.value("spectrumColor", static_cast<unsigned>(spectrumColor_)));
    spectrumCustomColor_ = j.value("spectrumCustomColor", false);
    spectrumFollowAlbum_ = j.value("spectrumFollowAlbum", false);
    if (spectrumCustomColor_)
        spectrumFollowAlbum_ = false;
    spectrumGradient_ = j.value("spectrumGradient", false);
    spectrumEffects_ = j.value("spectrumEffects", 0u) & kSpectrumEffectsMask;
    spectrumBackground_ = j.value("spectrumBackground", spectrumStyleValue == "background-wave");
    spectrumOpacity_ = std::clamp(j.value("spectrumOpacity", 40), 0, 100);
    progressBackground_ = j.value("progressBackground", false);
    progressBackgroundOpacity_ = std::clamp(j.value("progressBackgroundOpacity", 25), 0, 100);
    taskbarBackground_ = std::clamp(j.value("taskbarBackground", 0), 0, 2);
    coverBackgroundOpacity_ = std::clamp(j.value("coverBackgroundOpacity", 60), 0, 100);
    autoCheckOnStartup_ = j.value("autoCheckOnStartup", true);
    useGiteeUpdateSource_ = j.value("updateSource", std::string("github")) == "gitee";
    lyricFollowAlbum_ = j.value("lyricFollowAlbum", false);
    if (j.contains("secondaryLyricEnabled") || j.contains("secondaryLyricType")) {
        secondaryLyricEnabled_ = j.value("secondaryLyricEnabled", true);
        preferRomanization_ =
            j.value("secondaryLyricType", std::string("translation")) == "romanization";
    } else {
        // 从上一版的两个互斥开关迁移：两者都关时视为总开关关闭，类型保留翻译。
        bool oldTranslation = j.value("translationEnabled", true);
        bool oldRomanization = j.value("romanizationEnabled", false);
        secondaryLyricEnabled_ = oldTranslation || oldRomanization;
        preferRomanization_ = oldRomanization && !oldTranslation;
    }
    doubleLineLyricsEnabled_ = j.value("doubleLineLyrics", false);
    lyricEffects_ = j.value("lyricEffects", 0u) & kLyricEffectsMask;
    if ((lyricEffects_ & lyricEffectBit(LyricEffect::Stardust)) != 0)
        secondaryLyricEnabled_ = false;
    const std::string lyricAlignment = j.value("lyricAlignment", std::string("left"));
    if (lyricAlignment == "center")
        lyricAlignment_ = LyricAlignment::Center;
    else if (lyricAlignment == "right")
        lyricAlignment_ = LyricAlignment::Right;
    else
        lyricAlignment_ = LyricAlignment::Left;
    const std::string idleQuoteAlignment = j.value("idleQuoteAlignment", std::string("left"));
    if (idleQuoteAlignment == "center")
        idleQuoteAlignment_ = LyricAlignment::Center;
    else if (idleQuoteAlignment == "right")
        idleQuoteAlignment_ = LyricAlignment::Right;
    else
        idleQuoteAlignment_ = LyricAlignment::Left;
    songInfoVisible_ = j.value("songInfoVisible", true);
    albumCoverVisible_ = j.value("albumCoverVisible", true);
    platformIconVisible_ = j.value("platformIconVisible", false);
    albumCoverEffect_ = j.value("albumCoverEffect", std::string("default")) == "vinyl"
                            ? AlbumCoverEffect::Vinyl
                            : AlbumCoverEffect::Default;
    idleEntryEnabled_ = j.value("idleEntryEnabled", true);
    idleQuoteEnabled_ = j.value("idleQuoteEnabled", true);
    idleWelcomeEnabled_ = j.value("idleWelcomeEnabled", !idleQuoteEnabled_);
    idleAppNamesVisible_ = j.value("idleAppNamesVisible", true);
    idleCustomWelcome_ =
        normalizeIdleCustomWelcome(wideOf(j.value("idleCustomWelcome", std::string())));
    idleQuoteSource_ =
        idleQuoteSourceFromConfig(j.value("idleQuoteSource", std::string("hitokoto")));
    idleQuoteRefreshInterval_ =
        idleQuoteIntervalFromConfig(j.value("idleQuoteRefreshInterval", std::string("daily")));
    idleQuoteBackground_ =
        idleQuoteBackgroundFromConfig(j.value("idleQuoteBackground", std::string("none")));
    idleQuoteBackgroundScope_ = idleQuoteBackgroundScopeFromConfig(
        j.value("idleQuoteBackgroundScope", std::string("daily-quote")));
    jinrishiciToken_ = wideOf(j.value("jinrishiciToken", std::string()));
    idleApps_.clear();
    if (j.contains("idleApps") && j["idleApps"].is_array()) {
        for (const auto& value : j["idleApps"]) {
            IdleAppInfo app;
            if (value.is_string()) {
                // 兼容旧版本仅保存 EXE 路径的格式。
                app.path = wideOf(value.get<std::string>());
            } else if (value.is_object()) {
                app.path = wideOf(value.value("path", std::string()));
                app.customName = wideOf(value.value("customName", std::string()));
            }
            if (!app.path.empty())
                idleApps_.push_back(std::move(app));
            if (idleApps_.size() >= kMaxIdleApps)
                break;
        }
    }
    if (j.contains("idleQuoteCache") && j["idleQuoteCache"].is_object()) {
        const auto& cache = j["idleQuoteCache"];
        idleQuoteCacheSource_ = cache.value("source", std::string());
        idleQuoteCachePeriod_ = cache.value("period", std::string());
        idleQuoteCacheText_ = wideOf(cache.value("text", std::string()));
        idleQuoteCacheOrigin_ = wideOf(cache.value("origin", std::string()));
        idleQuoteCacheUuid_ = wideOf(cache.value("uuid", std::string()));
    }
    holidayCalendarYear_ = 0;
    holidayCalendarUpdatedAt_ = 0;
    holidayCalendar_.clear();
    if (j.contains("holidayCalendar") && j["holidayCalendar"].is_object()) {
        const auto& cache = j["holidayCalendar"];
        holidayCalendarYear_ = cache.value("year", 0);
        holidayCalendarUpdatedAt_ = cache.value("updatedAt", int64_t{0});
        if (cache.contains("days") && cache["days"].is_array()) {
            for (const auto& value : cache["days"]) {
                if (!value.is_object())
                    continue;
                HolidayDayInfo day;
                day.date = value.value("date", std::string());
                day.type = value.value("type", -1);
                day.name = wideOf(value.value("name", std::string()));
                if (!day.date.empty() && day.type >= 0 && day.type <= 3)
                    holidayCalendar_.push_back(std::move(day));
            }
        }
    }
    qqLocalLyricsEnabled_ = j.value("qqLocalLyricsEnabled", false);
    qqLocalLyricsPersistOrder_ = j.value("qqLocalLyricsPersistOrder", false);
    qqLocalLyricsPath_ = wideOf(j.value("qqLocalLyricsPath", std::string()));
}

nlohmann::json AppSettings::toJson() const {
    nlohmann::json j;
    j["hasUserFont"] = hasUserFont_;
    j["fontFamily"] = utf8Of(fontFamily_);
    j["fontSize"] = fontSize_;
    j["fontStyle"] = fontStyleName(fontStyle_);
    auto saveAppearance = [&j](const char* suffix, const LyricAppearance& appearance) {
        const std::string suffixValue = suffix;
        j["lyricColor" + suffixValue] = static_cast<unsigned>(appearance.played);
        j["lyricUnplayedColor" + suffixValue] = static_cast<unsigned>(appearance.unplayed);
        j["lyricUnplayedAlpha" + suffixValue] = appearance.unplayedAlphaPct;
        j["lyricGlowColor" + suffixValue] = static_cast<unsigned>(appearance.glowColor);
        j["lyricOutlineColor" + suffixValue] = static_cast<unsigned>(appearance.outlineColor);
        j["lyricGlow" + suffixValue] = appearance.glowOn;
        j["lyricOutline" + suffixValue] = appearance.outlineOn;
    };
    j["lyricAppearanceGlobal"] = lyricAppearanceGlobal_;
    saveAppearance("Light", lightLyricAppearance_);
    saveAppearance("Dark", darkLyricAppearance_);
    if (hasGlobalLyricAppearance_)
        saveAppearance("Global", globalLyricAppearance_);
    j["idleEntryEnabled"] = idleEntryEnabled_;
    j["idleQuoteEnabled"] = idleQuoteEnabled_;
    j["idleWelcomeEnabled"] = idleWelcomeEnabled_;
    j["idleAppNamesVisible"] = idleAppNamesVisible_;
    j["idleCustomWelcome"] = utf8Of(idleCustomWelcome_);
    j["idleQuoteSource"] = idleQuoteSourceConfigName(idleQuoteSource_);
    j["idleQuoteRefreshInterval"] = idleQuoteIntervalConfigName(idleQuoteRefreshInterval_);
    j["idleQuoteBackground"] = idleQuoteBackgroundConfigName(idleQuoteBackground_);
    j["idleQuoteBackgroundScope"] = idleQuoteBackgroundScopeConfigName(idleQuoteBackgroundScope_);
    j["jinrishiciToken"] = utf8Of(jinrishiciToken_);
    j["tickTickEnabled"] = tickTickEnabled_;
    j.erase("tickTickAppPath");
    j.erase("tickTickClientId");
    j["idleApps"] = nlohmann::json::array();
    for (const auto& app : idleApps_) {
        j["idleApps"].push_back({
            {"path", utf8Of(app.path)},
            {"customName", utf8Of(app.customName)},
        });
    }
    j["idleQuoteCache"] = {{"source", idleQuoteCacheSource_},
                           {"period", idleQuoteCachePeriod_},
                           {"text", utf8Of(idleQuoteCacheText_)},
                           {"origin", utf8Of(idleQuoteCacheOrigin_)},
                           {"uuid", utf8Of(idleQuoteCacheUuid_)}};
    j["holidayCalendar"] = nlohmann::json{{"year", holidayCalendarYear_},
                                          {"updatedAt", holidayCalendarUpdatedAt_},
                                          {"days", nlohmann::json::array()}};
    for (const auto& day : holidayCalendar_)
        j["holidayCalendar"]["days"].push_back(
            {{"date", day.date}, {"type", day.type}, {"name", utf8Of(day.name)}});
    j["taskbarPosition"] = taskbarPosition_;
    j["taskbarViewMode"] = static_cast<int>(taskbarViewMode_);
    j["taskbarMonitor"] = utf8Of(taskbarMonitorId_);
    j["appBarEdge"] = appBarEdge_ == AppBarEdge::Bottom ? 1 : 0;
    j.erase("taskbarImmersive");
    j["immersiveBackgroundBlur"] = immersiveBackgroundBlur_;
    j["dockBackgroundBlurLevel"] = dockBackgroundBlur_;
    j["immersiveBackgroundAdjustment"] = immersiveBackgroundAdjustment_;
    j["dockBackgroundAdjustment"] = dockBackgroundAdjustment_;
    j.erase("immersiveMaskOpacity");
    j.erase("dockMaskOpacity");
    j.erase("dockBackgroundBlur");
    j["embeddedResourceMonitor"] = embeddedResourceMonitor_;
    j["dockResourceGpu"] = dockResourceGpuUsage_;
    j["dockResourceDownload"] = dockResourceDownload_;
    j["dockResourceUpload"] = dockResourceUpload_;
    j.erase("dockResourceDisk");
    j.erase("dockResourceBattery");
    j.erase("dockResourceTemperature");
    j["dockResourceCpuFrequency"] = dockResourceCpuFrequency_;
    j.erase("immersiveMaskCustomColor");
    j.erase("immersiveMaskColor");
    j["taskbarContextMenu"] = taskbarContextMenuEnabled_;
    // 性能模式不写入配置，重启后由 loadSettings() 恢复正常模式。
    j["hoverPlaybackControls"] = hoverPlaybackControls_;
    j["hoverControlStyle"] = hoverControlStyle_ == HoverControlStyle::Popup ? 1 : 0;
    j["floatingCardTrigger"] = floatingCardTrigger_ == MediaPopupTrigger::Click ? 1 : 0;
    j["floatingCardBackground"] =
        floatingCardBackground_ == MediaPopupBackground::Frosted ? "frosted" : "solid";
    if (floatingCardBackgroundColorCustomized_)
        j["floatingCardBackgroundColor"] = static_cast<unsigned>(floatingCardBackgroundColor_);
    j["floatingCardFollowAlbum"] = floatingCardFollowAlbum_;
    j["floatingCardAutoTextContrast"] = floatingCardAutoTextContrast_;
    j.erase("mediaPopupTrigger");
    j.erase("mediaPopupBackground");
    j.erase("mediaPopupBackgroundColor");
    j.erase("idleCardBackground");
    j.erase("idleCardBackgroundColor");
    j.erase("idleCardFollowAlbum");
    j.erase("idleCardTriggerSync");
    j.erase("idleCardTrigger");
    j.erase("mediaPopupFollowAlbum");
    j.erase("mediaPopupAutoTextContrast");
    j["songToast"] = songToastEnabled_;
    j["songToastDuration"] = songToastDurationSec_;
    j["songToastSkipFullscreen"] = songToastSkipFullscreen_;
    j["songToastPosition"] = songToastTop_ ? "top" : "bottom";
    j["songToastFollowMouseMonitor"] = songToastFollowMouseMonitor_;
    j["taskbarTheme"] = themeModeConfigName(taskbarThemeMode_);
    j["windowTheme"] = themeModeConfigName(windowThemeMode_);
    j["spectrum"] = spectrumOn_;
    j["spectrumStyle"] = spectrumStyleConfigName(spectrumStyle_);
    j["spectrumColor"] = static_cast<unsigned>(spectrumColor_);
    j["spectrumCustomColor"] = spectrumCustomColor_;
    j["spectrumFollowAlbum"] = spectrumFollowAlbum_;
    j["spectrumGradient"] = spectrumGradient_;
    j["spectrumEffects"] = spectrumEffects_;
    j["spectrumBackground"] = spectrumBackground_;
    j["spectrumOpacity"] = spectrumOpacity_;
    j["progressBackground"] = progressBackground_;
    j["progressBackgroundOpacity"] = progressBackgroundOpacity_;
    j["taskbarBackground"] = taskbarBackground_;
    j["coverBackgroundOpacity"] = coverBackgroundOpacity_;
    j["autoCheckOnStartup"] = autoCheckOnStartup_;
    j["updateSource"] = useGiteeUpdateSource_ ? "gitee" : "github";
    j["lyricFollowAlbum"] = lyricFollowAlbum_;
    j["secondaryLyricEnabled"] = secondaryLyricEnabled_;
    j["secondaryLyricType"] = preferRomanization_ ? "romanization" : "translation";
    j["doubleLineLyrics"] = doubleLineLyricsEnabled_;
    j["lyricEffects"] = lyricEffects_;
    j["lyricAlignment"] = lyricAlignment_ == LyricAlignment::Center  ? "center"
                          : lyricAlignment_ == LyricAlignment::Right ? "right"
                                                                     : "left";
    j["idleQuoteAlignment"] = idleQuoteAlignment_ == LyricAlignment::Center  ? "center"
                              : idleQuoteAlignment_ == LyricAlignment::Right ? "right"
                                                                             : "left";
    j["songInfoVisible"] = songInfoVisible_;
    j["albumCoverVisible"] = albumCoverVisible_;
    j["platformIconVisible"] = platformIconVisible_;
    j["albumCoverEffect"] = albumCoverEffect_ == AlbumCoverEffect::Vinyl ? "vinyl" : "default";
    j["qqLocalLyricsEnabled"] = qqLocalLyricsEnabled_;
    j["qqLocalLyricsPersistOrder"] = qqLocalLyricsPersistOrder_;
    j["qqLocalLyricsPath"] = utf8Of(qqLocalLyricsPath_);
    j["logDirectory"] = utf8Of(logDirectory_);
    j["logRetentionDays"] = logRetentionDays_;

    return j;
}
