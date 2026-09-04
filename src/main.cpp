#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "idle/idle_quote_provider.h"
#include "idle/ticktick_provider.h"
#include "app_info.h"
#include "ui/font_style.h"
#include "ui/lyric_window.h"
#include "ui/taskbar_host.h"
#include "ui/app_icon.h"
#include "ui/about_dialog.h"
#include "ui/manual_search_dialog.h"
#include "ui/font_picker_dialog.h"
#include "ui/font_color_dialog.h"
#include "ui/idle_app_name_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/song_toast.h"
#include "ui/runtime_log_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_menu.h"
#include "ui/fluent_theme.h"
#include "ui/platform_icon.h"
#include "media/smtc_monitor.h"
#include "media/audio_spectrum.h"
#include "media/app_volume.h"
#include "util/dominant_color.h"
#include "logging/runtime_logger.h"

#include <windows.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cwctype>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <io.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kMsgSmtcChanged = WM_APP + 1;
constexpr UINT kMsgLyricReady = WM_APP + 2;
constexpr UINT kMsgCoverReady = WM_APP + 3;
constexpr UINT kMsgQqLocalFolderReady = WM_APP + 4;
// 应用音频会话的音量/静音/可用性变化（AppVolumeController 从 COM 回调线程投递）
constexpr UINT kMsgAppVolumeChanged = WM_APP + 5;
constexpr UINT kMsgIdleQuoteReady = WM_APP + 6;
constexpr UINT kMsgIdleAppReady = WM_APP + 7;
constexpr UINT kMsgHolidayReady = WM_APP + 8;
constexpr UINT kMsgTickTickTasksReady = WM_APP + 9;
constexpr UINT kMsgTickTickTaskCompleteReady = WM_APP + 10;
// QQ 切歌时 SMTC 把媒体属性与时间线拆成多条事件投递，歌词请求延迟到这批事件
// 合并完成后发出，避免按不完整的标题/歌手/时长先失败一次（界面闪「暂无歌词」）。
constexpr UINT_PTR kTimerLyricDebounce = 3;
// 切歌弹窗等待封面的超时：SMTC 把封面拆在切歌事件批的后续事件里投递，
// 短暂等待可以带封面弹出；超时则按无封面弹出，不再干等。
constexpr UINT_PTR kTimerSongToastCover = 4;
    // 每日一言只按本地周期检查；命中同一周期时不会重复请求网络。
constexpr UINT_PTR kTimerIdleQuote = 5;
constexpr UINT kSongToastCoverWaitMs = 350;
constexpr UINT kLyricDebounceMs = 300;
constexpr UINT kIdleQuoteCheckMs = 60 * 1000;
constexpr int64_t kHolidayCacheMaxAgeSec = 7 * 24 * 60 * 60;
constexpr UINT kTrayMsg = WM_APP + 200;
constexpr UINT kTrayIconId = 1;
constexpr UINT kCmdToggleTaskbar = 101;
constexpr UINT kCmdPickFont = 105;
constexpr UINT kCmdExit = 106;
constexpr UINT kCmdManualSearch = 107;
constexpr UINT kCmdFontColorEffect = 108;
constexpr UINT kCmdTaskbarPosNotify = 109;
constexpr UINT kCmdTaskbarPosLeft = 110;
constexpr UINT kCmdSpectrum = 111;
constexpr UINT kCmdAutoStart = 112;
constexpr UINT kCmdFollowAlbum = 113;
constexpr UINT kCmdSecondaryLyric = 114;
constexpr UINT kCmdSwitchSecondaryLyric = 115;
constexpr UINT kCmdSongInfo = 116;
constexpr UINT kCmdAlbumCover = 117;
constexpr UINT kCmdAlbumCoverEffectDefault = 118;
constexpr UINT kCmdAlbumCoverEffectVinyl = 119;
constexpr UINT kCmdDoubleLineLyrics = 120;
constexpr UINT kCmdPlatformIcon = 121;
constexpr UINT kCmdAbout = 122;
constexpr UINT kCmdLyricAlignLeft = 123;
constexpr UINT kCmdLyricAlignCenter = 124;
constexpr UINT kCmdLyricAlignRight = 125;
constexpr UINT kCmdHoverPlaybackControls = 126;
constexpr UINT kCmdSettings = 127;
constexpr UINT kCmdSwitchLyricSource = 128;
constexpr UINT kCmdRuntimeLog = 129;
constexpr int64_t kLyricTransitionLeadMs = 100; // 提前准备下一句显示，逐字高亮仍按真实进度
constexpr int kUpdatePromptReleasePage = 1;
constexpr int kUpdatePromptDownload = 2;
constexpr int kUpdatePromptAbout = 3;

const wchar_t* trayCommandName(int command) {
    switch (command) {
    case kCmdToggleTaskbar: return L"toggle-taskbar";
    case kCmdTaskbarPosNotify: return L"taskbar-position-notify";
    case kCmdTaskbarPosLeft: return L"taskbar-position-left";
    case kCmdSpectrum: return L"spectrum";
    case kCmdAutoStart: return L"autostart";
    case kCmdFollowAlbum: return L"follow-album";
    case kCmdSecondaryLyric: return L"secondary-lyric";
    case kCmdSwitchSecondaryLyric: return L"switch-secondary-lyric";
    case kCmdSongInfo: return L"song-info";
    case kCmdAlbumCover: return L"album-cover";
    case kCmdAlbumCoverEffectDefault: return L"album-cover-effect-default";
    case kCmdAlbumCoverEffectVinyl: return L"album-cover-effect-vinyl";
    case kCmdDoubleLineLyrics: return L"double-line-lyrics";
    case kCmdPlatformIcon: return L"platform-icon";
    case kCmdAbout: return L"about";
    case kCmdLyricAlignLeft: return L"lyric-align-left";
    case kCmdLyricAlignCenter: return L"lyric-align-center";
    case kCmdLyricAlignRight: return L"lyric-align-right";
    case kCmdHoverPlaybackControls: return L"hover-playback-controls";
    case kCmdSettings: return L"settings";
    case kCmdSwitchLyricSource: return L"switch-lyric-source";
    case kCmdRuntimeLog: return L"runtime-log";
    case kCmdPickFont: return L"pick-font";
    case kCmdFontColorEffect: return L"font-color-effect";
    case kCmdManualSearch: return L"manual-search";
    case kCmdExit: return L"exit";
    default: return L"unknown";
    }
}

struct CoverPayload {
    std::wstring key;
    uint64_t requestGeneration = 0;
    std::shared_ptr<const std::vector<uint8_t>> cover;
};

struct LyricPayload {
    std::wstring key;
    uint64_t requestGeneration = 0;
    bool ok = false;
};

struct QqLocalFolderPayload {
    std::wstring path;
};

struct IdleAppPayload {
    std::wstring path;
};

struct IdleQuotePayload {
    uint64_t generation = 0;
    IdleQuoteResult result;
};

struct HolidayPayload {
    uint64_t generation = 0;
    HolidayCalendarResult result;
};

struct TickTickTasksPayload {
    uint64_t generation = 0;
    TickTickTasksResult result;
};

struct TickTickTaskCompletePayload {
    uint64_t generation = 0;
    std::wstring taskId;
    TickTickTaskMutationResult result;
};

std::string utf8Of(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

COLORREF defaultFloatingCardBackgroundColor() {
    return fluent::isDarkMode(fluent::ThemeTarget::Window) ? RGB(0, 0, 0)
                                                            : RGB(255, 255, 255);
}

std::wstring wideOf(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
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
    case LyricFontStyle::Bold: return "bold";
    case LyricFontStyle::Italic: return "italic";
    case LyricFontStyle::BoldItalic: return "boldItalic";
    default: return "normal";
    }
}

const wchar_t* fontStyleLabel(LyricFontStyle style) {
    switch (style) {
    case LyricFontStyle::Bold: return L"加粗";
    case LyricFontStyle::Italic: return L"斜体";
    case LyricFontStyle::BoldItalic: return L"粗斜体";
    default: return L"常规";
    }
}

SpectrumStyle spectrumStyleFromConfig(const std::string& value) {
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
    case SpectrumStyle::Bars: return "bars";
    case SpectrumStyle::DreamyWave: return "dreamy-wave";
    case SpectrumStyle::Default:
    default: return "default";
    }
}

int spectrumStyleIndex(SpectrumStyle style) {
    switch (style) {
    case SpectrumStyle::Bars: return 1;
    case SpectrumStyle::DreamyWave: return 2;
    case SpectrumStyle::Default:
    default: return 0;
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
    case IdleQuoteBackground::FallingLeaves: return "leaves";
    case IdleQuoteBackground::TwinklingStars: return "stars";
    case IdleQuoteBackground::BinaryRain: return "binary";
    case IdleQuoteBackground::FloatingParticles: return "particles";
    case IdleQuoteBackground::None:
    default: return "none";
    }
}

int idleQuoteBackgroundIndex(IdleQuoteBackground background) {
    switch (background) {
    case IdleQuoteBackground::FallingLeaves: return 1;
    case IdleQuoteBackground::TwinklingStars: return 2;
    case IdleQuoteBackground::BinaryRain: return 3;
    case IdleQuoteBackground::FloatingParticles: return 4;
    case IdleQuoteBackground::None:
    default: return 0;
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
    case IdleQuoteBackgroundScope::None: return "none";
    case IdleQuoteBackgroundScope::Lyrics: return "lyrics";
    case IdleQuoteBackgroundScope::All: return "all";
    case IdleQuoteBackgroundScope::DailyQuote:
    default: return "daily-quote";
    }
}

int idleQuoteBackgroundScopeIndex(IdleQuoteBackgroundScope scope) {
    switch (scope) {
    case IdleQuoteBackgroundScope::Lyrics: return 2;
    case IdleQuoteBackgroundScope::All: return 3;
    case IdleQuoteBackgroundScope::None: return 0;
    case IdleQuoteBackgroundScope::DailyQuote:
    default: return 1;
    }
}

const wchar_t* idleQuoteSourceLabel(IdleQuoteSource source) {
    return source == IdleQuoteSource::Jinrishici ? L"今日诗词" : L"一言";
}

const char* idleQuoteSourceConfigName(IdleQuoteSource source) {
    return source == IdleQuoteSource::Jinrishici ? "jinrishici" : "hitokoto";
}

IdleQuoteSource idleQuoteSourceFromConfig(const std::string& value) {
    return value == "jinrishici" ? IdleQuoteSource::Jinrishici : IdleQuoteSource::Hitokoto;
}

constexpr size_t kIdleCustomWelcomeMaxLen = 20;

// 自定义欢迎语归一化：超长截断；纯空白等同于未设置（恢复默认欢迎语）。
std::wstring normalizeIdleCustomWelcome(std::wstring text) {
    if (text.size() > kIdleCustomWelcomeMaxLen)
        text.resize(kIdleCustomWelcomeMaxLen);
    const bool allSpace = std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return iswspace(c) != 0;
    });
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

std::string idleQuotePeriodKey(IdleQuoteRefreshInterval interval) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char key[64]{};
    if (interval == IdleQuoteRefreshInterval::Hourly) {
        sprintf_s(key, "%04d-%02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour);
    } else if (interval == IdleQuoteRefreshInterval::HalfDay) {
        sprintf_s(key, "%04d-%02d-%02d-%s", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour < 12 ? "0-12" : "12-24");
    } else {
        sprintf_s(key, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday);
    }
    return key;
}

std::tm localNow() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return local;
}

std::string localDateKey(const std::tm& local) {
    char key[32]{};
    sprintf_s(key, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
              local.tm_mday);
    return key;
}

bool validExePath(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".exe") == 0;
}

std::wstring fallbackExeName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
    std::wstring name = path.substr(start);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
        name.resize(dot);
    return name;
}

fluent::ThemeMode themeModeFromConfig(const std::string& value,
                                      fluent::ThemeMode fallback) {
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

// %APPDATA%\QQMusicLyric（不存在则创建）
std::wstring configDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        dir = p;
        CoTaskMemFree(p);
        dir += L"\\QQMusicLyric";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

// 开机自启动：HKCU\...\Run（仅当前用户，无需管理员权限；注册表即真源，菜单勾选态实时读取）
constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kAutoStartValueName = L"QQMusicLyric";

bool autoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG ret = RegQueryValueExW(key, kAutoStartValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return ret == ERROR_SUCCESS && type == REG_SZ;
}

bool setAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG ret;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\""; // 引号包裹，防止含空格路径解析失败
        cmd += path;
        cmd += L"\"";
        ret = RegSetValueExW(key, kAutoStartValueName, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(cmd.c_str()),
                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        ret = RegDeleteValueW(key, kAutoStartValueName);
    }
    RegCloseKey(key);
    return ret == ERROR_SUCCESS;
}

const wchar_t* statusName(PlaybackStatus s) {
    switch (s) {
    case PlaybackStatus::Playing: return L"Playing";
    case PlaybackStatus::Paused: return L"Paused";
    case PlaybackStatus::Stopped: return L"Stopped";
    default: return L"Other";
    }
}

const wchar_t* playerName(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic: return L"QQ Music";
    case SmtcPlayerType::NetEase: return L"NetEase Music";
    default: return L"Unknown";
    }
}

const wchar_t* spectrumProcessName(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic: return L"QQMusic.exe";
    case SmtcPlayerType::NetEase: return L"cloudmusic.exe";
    default: return L"";
    }
}

// 应用音量控制目标：网易云同时包含桥接插件进程 NeteaseBridge.exe——
// 它在音量合成器中独占一格，只调 cloudmusic.exe 会漏掉它。
std::vector<std::wstring> volumeProcessNames(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic: return {L"QQMusic.exe"};
    case SmtcPlayerType::NetEase: return {L"cloudmusic.exe", L"NeteaseBridge.exe"};
    default: return {};
    }
}

std::wstring makeTrackKey(const SmtcSnapshot& snap) {
    if (snap.player == SmtcPlayerType::NetEase && !snap.neteaseSongId.empty())
        return L"netease|" + snap.neteaseSongId;
    // QQ 切歌窗口可能把上一首的时间线残留带进当前快照；它对展示和歌词匹配
    // 都是不可信的，不能让这段时长参与曲目身份，否则真实时长到达时会被误判为换歌。
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    return L"qq|" + LyricProvider::makeKey(snap.title, snap.artist, durationMs);
}

bool snapshotMatchesTrackKey(const SmtcSnapshot& snap, const std::wstring& key) {
    if (!snap.sessionAlive)
        return false;
    // SMTC 的 Changing/Opened 过渡可能暂时没有完整文案；此时保留当前会话，
    // 待下一份完整快照再做曲目身份判断。
    if (snap.title.empty() && snap.neteaseSongId.empty())
        return true;
    return key.empty() || makeTrackKey(snap) == key;
}

struct App {
    DWORD mainThread = GetCurrentThreadId();
    runtime_log::RuntimeLogger runtimeLogger_;
    SmtcMonitor monitor;
    LyricProvider provider;
    CoverProvider coverProvider;
    IdleQuoteProvider idleQuoteProvider_;
    TickTickProvider tickTickProvider_;
    AppVolumeController appVolume_;   // 当前音乐应用的独立音量（音量合成器中该应用的一格）
    AppVolumeState appVolumeState_;   // 最近推送给宿主的音量状态（去重用）
    std::unique_ptr<TaskbarHost> taskbarHost; // 具体类型：歌词描边光晕是任务栏独有接口
    std::unique_ptr<AboutDialog> aboutDialog;
    std::unique_ptr<ManualSearchDialog> manualSearchDialog;
    std::unique_ptr<FontPickerDialog> fontPickerDialog;
    std::unique_ptr<FontColorDialog> fontColorDialog;
    std::unique_ptr<IdleAppNameDialog> idleAppNameDialog;
    std::unique_ptr<IdleAppNameDialog> idleWelcomeDialog;
    std::unique_ptr<IdleAppNameDialog> tickTickApiTokenDialog;
    std::unique_ptr<SettingsDialog> settingsDialog;
    std::unique_ptr<RuntimeLogDialog> runtimeLogDialog;
    std::wstring currentKey;
    std::wstring currentTitle;
    std::wstring currentArtist;
    int64_t currentDurationMs = 0;
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    SmtcPlayerType lastPlayer_ = SmtcPlayerType::Unknown;
    bool lyricLoading_ = false;
    bool songToastCoverWaitArmed_ = false; // 切歌弹窗正在等待封面补齐
    bool lyricDebounceArmed_ = false; // QQ 切歌请求防抖定时器已挂起
    ULONGLONG lyricDebounceDeadline_ = 0; // 等待新时间线的截止时刻（超时则按现有快照请求）
    bool lyricRequestStale_ = false; // 当前在途/最近一次歌词请求是否发于时间线不可信期间
    std::vector<LyricLine> currentLyrics_;
    bool currentLyricsFromLocal_ = false;
    bool currentLyricsFromManual_ = false;
    uint64_t requestGeneration_ = 0;
    uint64_t frameRevision_ = 0;
    PresentationFrame currentFrame_;
    std::shared_ptr<const std::vector<uint8_t>> lastSmtcThumbnail;

    HWND trayHwnd = nullptr;
    UINT taskbarCreatedMsg_ = 0; // Explorer 重启广播（只有顶层窗口收得到，托盘窗口不能用 HWND_MESSAGE）
    bool shutdownRequested_ = false;

    // 字体状态（作为字体选择器的记忆源）。hasUserFont_ 为 false 时各宿主使用各自的默认字体。
    bool hasUserFont_ = false;
    std::wstring fontFamily_ = fluent::uiFontFamily(); // 与普通窗口默认字体族一致
    float fontSize_ = 16.0f;
    LyricFontStyle fontStyle_ = LyricFontStyle::Normal;

    // 歌词外观可按任务栏深浅色模式分别保存，也可通过全局开关共用一套配置。
    bool lyricAppearanceGlobal_ = false;
    bool hasGlobalLyricAppearance_ = false;
    FontColorDialog::ThemeState lightLyricAppearance_;
    FontColorDialog::ThemeState darkLyricAppearance_;
    FontColorDialog::ThemeState globalLyricAppearance_;

    // 歌词已播放颜色跟随专辑封面主色调：开启时覆盖当前主题的已播放配置色。
    bool lyricFollowAlbum_ = false;
    // 总开关与类型选择独立于当前歌曲能力；缺少所选内容时暂不显示，后续自动恢复。
    bool secondaryLyricEnabled_ = true;
    bool preferRomanization_ = false;
    bool doubleLineLyricsEnabled_ = false;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    LyricAlignment idleQuoteAlignment_ = LyricAlignment::Left;
    IdleQuoteBackground idleQuoteBackground_ = IdleQuoteBackground::None;
    IdleQuoteBackgroundScope idleQuoteBackgroundScope_ = IdleQuoteBackgroundScope::DailyQuote;
    bool currentHasTranslation_ = false;
    bool currentHasRomanization_ = false;
    bool hasAlbumColor_ = false; // 当前曲目是否已提取到主色调（切歌后失效）
    COLORREF albumColor_ = RGB(49, 194, 124);
    std::shared_ptr<const std::vector<uint8_t>> lastCover_; // 当前曲目有效封面（SMTC 优先，API 兜底）

    // 每日一言任务栏入口：句子缓存与应用图标属于播放链路之外的独立状态。
    bool idleEntryEnabled_ = true;
    bool idleQuoteEnabled_ = true;
    bool idleAppNamesVisible_ = true;
    // 自定义欢迎语：非空时在关闭每日一言后始终显示，不再随时间和日期变化。
    std::wstring idleCustomWelcome_;
    IdleQuoteSource idleQuoteSource_ = IdleQuoteSource::Hitokoto;
    IdleQuoteRefreshInterval idleQuoteRefreshInterval_ = IdleQuoteRefreshInterval::Daily;
    std::vector<IdleAppInfo> idleApps_;
    std::wstring jinrishiciToken_;
    std::wstring idleQuoteText_;
    std::wstring idleQuoteOrigin_;
    std::wstring idleQuoteUuid_;
    std::string idleQuoteCacheSource_;
    std::string idleQuoteCachePeriod_;
    std::wstring idleQuoteCacheText_;
    std::wstring idleQuoteCacheOrigin_;
    std::wstring idleQuoteCacheUuid_;
    bool idleQuoteLoading_ = false;
    uint64_t idleQuoteRequestGeneration_ = 0;
    std::string idleQuoteAttemptKey_;
    std::string idleQuoteRequestKey_;

    // 滴答清单今日任务：普通用户 API 口令由 TickTickProvider 保存在 Windows
    // 凭据管理器；这里只保留连接状态和当前展示快照。
    TickTickService tickTickService_ = TickTickService::Dida365;
    std::wstring tickTickApiToken_;
    bool tickTickEnabled_ = false;
    bool tickTickEnableAfterTokenSave_ = false;
    bool tickTickConnected_ = false;
    bool tickTickConnecting_ = false;
    bool tickTickTasksLoading_ = false;
    std::wstring tickTickStatus_ = L"请在设置中连接滴答清单";
    std::vector<IdleTaskInfo> todayTasks_;
    uint64_t tickTickRequestGeneration_ = 0;
    std::wstring tickTickCompletingTaskId_;

    // 每次进程启动只决策一次：首次任务同步完成前显示加载状态；数据成功且
    // 没有媒体会话时播报一次任务概览，之后再回到普通空闲文案。
    bool startupTaskSummaryPending_ = false;
    bool startupTaskSummaryActive_ = false;

    // 当前年份节假日类型缓存：只在启动或缓存过期时请求全年数据，日常判断不访问网络。
    int holidayCalendarYear_ = 0;
    int64_t holidayCalendarUpdatedAt_ = 0;
    std::vector<HolidayDayInfo> holidayCalendar_;
    uint64_t holidayRequestGeneration_ = 0;
    bool holidayRequestInFlight_ = false;
    int holidayRequestYear_ = 0;
    int64_t holidayRequestAttemptAt_ = 0;

    // 任务栏歌词锚定位置：0 = 通知区域左侧，1 = 任务栏最左侧
    int taskbarPosition_ = 0;
    bool hoverPlaybackControls_ = true;
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    MediaPopupTrigger floatingCardTrigger_ = MediaPopupTrigger::Hover;
    MediaPopupBackground floatingCardBackground_ = MediaPopupBackground::Solid;
    COLORREF floatingCardBackgroundColor_ = RGB(255, 255, 255);
    bool floatingCardBackgroundColorCustomized_ = false;
    bool floatingCardFollowAlbum_ = false;
    bool floatingCardAutoTextContrast_ = true;
    // 切歌弹窗（灵动岛）：主屏幕中下方短暂弹出歌曲信息，固定鼠标穿透
    std::unique_ptr<SongToast> songToast_;
    bool songToastEnabled_ = false;
    int songToastDurationSec_ = 4;
    bool songToastSkipFullscreen_ = true; // 前台有全屏应用时不弹出
    bool songToastTop_ = false;           // true 中上，false 中下（默认）
    // 任务栏默认跟随 Windows 系统模式；普通窗口/悬浮窗默认跟随 Windows 应用模式。
    fluent::ThemeMode taskbarThemeMode_ = fluent::ThemeMode::FollowSystem;
    fluent::ThemeMode windowThemeMode_ = fluent::ThemeMode::FollowApp;
    // 渲染模式：0 正常；1 低渲染（~30fps，降低 GPU/CPU 占用）；2 完全停止；
    // 3 极简（不降低歌词刷新率，只关闭附加视觉与弹窗）
    int renderMode_ = 0;
    bool taskbarVertical_ = false; // 当前任务栏是否为左右侧的竖向布局

    // 频谱（任务栏歌词独有）：开关持久化，开启时捕获线程跟随任务栏宿主启停
    AudioSpectrum spectrum_;
    bool spectrumOn_ = false;
    SpectrumStyle spectrumStyle_ = SpectrumStyle::Default;
    bool spectrumBackground_ = false;
    int spectrumOpacity_ = 40;
    // 播放进度背景：与背景波浪互斥，不透明度默认 25% 保证文字可读
    bool progressBackground_ = false;
    int progressBackgroundOpacity_ = 25;
    // 任务栏歌词背景：0 无 1 封面模糊 2 纯色（画在最底层，可与进度背景/背景波浪叠加）
    int taskbarBackground_ = 0;
    int coverBackgroundOpacity_ = 60;
    bool spectrumSessionAlive_ = false;
    std::wstring spectrumSessionKey_;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;

    bool autoCheckOnStartup_ = true;
    bool useGiteeUpdateSource_ = false;
    HWND updatePromptHwnd_ = nullptr;
    bool updatePromptBackdrop_ = false;
    fluent::FluentDialogSurface updatePromptSurface_;
    std::wstring updatePromptLatestVersion_;
    bool updatePromptUseGiteeSource_ = false;
    D2D1_RECT_F updatePromptTitleRect_{};
    D2D1_RECT_F updatePromptSubtitleRect_{};
    D2D1_RECT_F updatePromptVersionCardRect_{};
    D2D1_RECT_F updatePromptCurrentVersionRect_{};
    D2D1_RECT_F updatePromptLatestVersionRect_{};
    D2D1_RECT_F updatePromptSourceRect_{};
    D2D1_RECT_F updatePromptReleasePageRect_{};
    D2D1_RECT_F updatePromptDownloadRect_{};
    D2D1_RECT_F updatePromptAboutRect_{};
    int updatePromptHoverId_ = 0;
    int updatePromptPressedId_ = 0;
    int updatePromptFocusedId_ = kUpdatePromptAbout;
    bool updatePromptFocusVisible_ = false;
    std::wstring settingsPath_;
    std::wstring logDirectory_;
    int logRetentionDays_ = 30;
    bool qqLocalLyricsEnabled_ = false;
    bool qqLocalLyricsPersistOrder_ = false;
    std::wstring qqLocalLyricsPath_;
    bool qqLocalLyricsPickerOpen_ = false;
    bool idleAppPickerOpen_ = false;

    std::vector<ILyricHost*> hosts() {
        std::vector<ILyricHost*> v;
        if (taskbarHost) v.push_back(taskbarHost.get());
        return v;
    }

    const wchar_t* notRunningStatus() const {
        return lastPlayer_ == SmtcPlayerType::NetEase ? L"网易云音乐未运行" : L"QQ 音乐未运行";
    }

    void updateRuntimeLogState(const SmtcSnapshot& snap) {
        if (!snap.sessionAlive) {
            runtimeLogger_.setPlayback({}, {}, 0, false);
            runtimeLogger_.setLyricSource(L"未加载");
            runtimeLogger_.setCoverImage(nullptr);
            return;
        }

        const std::wstring title = snap.title.empty() ? currentTitle : snap.title;
        const std::wstring artist = snap.artist.empty() ? currentArtist : snap.artist;
        const int64_t durationMs = !snap.timelineStale
                                       ? (snap.durationMs > 0 ? snap.durationMs
                                                              : currentDurationMs)
                                       : 0;
        runtimeLogger_.setPlayback(title, artist, durationMs, true);
        if (lyricLoading_)
            runtimeLogger_.setLyricSource(L"加载中…");
        else if (currentLyricsFromManual_)
            runtimeLogger_.setLyricSource(L"本地歌词（手动搜索）");
        else if (currentLyricsFromLocal_)
            runtimeLogger_.setLyricSource(L"本地歌词（QQ音乐本地）");
        else if (!currentLyrics_.empty())
            runtimeLogger_.setLyricSource(L"在线歌词");
        else
            runtimeLogger_.setLyricSource(L"暂无歌词");
        std::shared_ptr<const std::vector<uint8_t>> cover;
        if (lastSmtcThumbnail && !lastSmtcThumbnail->empty())
            cover = lastSmtcThumbnail;
        else if (lastCover_ && !lastCover_->empty())
            cover = lastCover_;
        runtimeLogger_.setCoverImage(cover);
    }

    void logLyricsCreated(const wchar_t* source) {
        if (!currentLyrics_.empty())
            runtime_log::writef(L"[resource][event] lyrics-created source=%s lines=%zu", source,
                                currentLyrics_.size());
    }

    void releaseCurrentLyrics() {
        if (!currentLyrics_.empty()) {
            const wchar_t* source = currentLyricsFromManual_
                                        ? L"manual"
                                        : currentLyricsFromLocal_ ? L"local" : L"online";
            runtime_log::writef(L"[resource][event] lyrics-released source=%s lines=%zu", source,
                                currentLyrics_.size());
        }
        currentLyrics_.clear();
        currentLyricsFromLocal_ = false;
        currentLyricsFromManual_ = false;
        updateLyricCapabilities({});
    }

    void logCoverCreated(const wchar_t* source,
                         const std::shared_ptr<const std::vector<uint8_t>>& cover) {
        if (cover && !cover->empty())
            runtime_log::writef(L"[resource][event] cover-created source=%s bytes=%zu", source,
                                cover->size());
    }

    void releaseCurrentCover() {
        const bool hasSmtcCover = lastSmtcThumbnail && !lastSmtcThumbnail->empty();
        const bool hasApiCover = lastCover_ && !lastCover_->empty() &&
                                 (!lastSmtcThumbnail || lastCover_ != lastSmtcThumbnail);
        if (hasSmtcCover)
            runtime_log::writef(L"[resource][event] cover-released source=smtc bytes=%zu",
                                lastSmtcThumbnail->size());
        if (hasApiCover)
            runtime_log::writef(L"[resource][event] cover-released source=api bytes=%zu",
                                lastCover_->size());
        lastCover_.reset();
        lastSmtcThumbnail.reset();
    }

    void releaseCurrentResources() {
        releaseCurrentLyrics();
        releaseCurrentCover();
    }

    void updateLyricCapabilities(const std::vector<LyricLine>& lines) {
        currentHasTranslation_ = false;
        currentHasRomanization_ = false;
        for (const auto& line : lines) {
            currentHasTranslation_ = currentHasTranslation_ || !line.translation.empty();
            currentHasRomanization_ = currentHasRomanization_ || !line.romanization.empty();
        }
    }

    void applySecondaryLyricMode() {
        const bool showTranslation = !taskbarVertical_ && secondaryLyricEnabled_ &&
                                     !preferRomanization_;
        const bool showRomanization = !taskbarVertical_ && secondaryLyricEnabled_ &&
                                      preferRomanization_;
        for (auto* h : hosts())
            h->setSecondaryLyricMode(showTranslation, showRomanization);
    }

    void logSettingBool(const wchar_t* name, bool value) {
        runtime_log::writef(L"[action][setting] %s=%s", name, value ? L"on" : L"off");
    }

    void logSettingInt(const wchar_t* name, int value) {
        runtime_log::writef(L"[action][setting] %s=%d", name, value);
    }

    // 设置项统一直接生效入口：右键菜单与设置页共用同一套应用逻辑
    void applySongInfoVisible(bool on) {
        songInfoVisible_ = on;
        if (taskbarHost)
            taskbarHost->setSongInfoVisible(taskbarVertical_ ? false : on);
        logSettingBool(L"song-info-visible", on);
        saveSettings();
    }

    void applyAlbumCoverVisible(bool on) {
        albumCoverVisible_ = on;
        if (taskbarHost)
            taskbarHost->setAlbumCoverVisible(on);
        logSettingBool(L"album-cover-visible", on);
        saveSettings();
    }

    void applyPlatformIconVisible(bool on) {
        platformIconVisible_ = on;
        if (taskbarHost)
            taskbarHost->setPlatformIconVisible(on);
        logSettingBool(L"platform-icon-visible", on);
        saveSettings();
    }

    void applyCoverEffect(bool vinyl) {
        albumCoverEffect_ = vinyl ? AlbumCoverEffect::Vinyl : AlbumCoverEffect::Default;
        if (taskbarHost)
            taskbarHost->setAlbumCoverEffect(
                isMinimalRenderMode() ? AlbumCoverEffect::Default : albumCoverEffect_);
        logSettingBool(L"album-cover-vinyl", vinyl);
        saveSettings();
    }

    void applySpectrumOn(bool on) {
        spectrumOn_ = taskbarVertical_ ? false : on;
        syncSpectrumWithMode();
        logSettingBool(L"spectrum", spectrumOn_);
        saveSettings();
    }

    void applySpectrumStyle(int style) {
        spectrumStyle_ = taskbarVertical_
                             ? SpectrumStyle::Default
                             : style == 1 ? SpectrumStyle::Bars
                                          : style == 2 ? SpectrumStyle::DreamyWave
                                                        : SpectrumStyle::Default;
        if (taskbarHost)
            taskbarHost->setSpectrumStyle(spectrumStyle_);
        logSettingInt(L"spectrum-style", spectrumStyle_ == SpectrumStyle::Bars
                                            ? 1
                                            : spectrumStyle_ == SpectrumStyle::DreamyWave ? 2 : 0);
        saveSettings();
    }

    void applySpectrumBackground(bool on) {
        spectrumBackground_ = taskbarVertical_ ? false : on;
        if (taskbarHost)
            taskbarHost->setSpectrumBackground(isMinimalRenderMode() ? false : spectrumBackground_);
        logSettingBool(L"spectrum-background", spectrumBackground_);
        saveSettings();
    }

    void applySpectrumOpacity(int percent) {
        spectrumOpacity_ = std::clamp(percent, 0, 100);
        if (taskbarHost)
            taskbarHost->setSpectrumOpacity(spectrumOpacity_);
        logSettingInt(L"spectrum-opacity", spectrumOpacity_);
        saveSettings();
    }

    void applyProgressBackground(bool on) {
        progressBackground_ = on;
        if (taskbarHost)
            taskbarHost->setProgressBackground(isMinimalRenderMode() ? false : on);
        logSettingBool(L"progress-background", on);
        saveSettings();
    }

    void applyProgressBackgroundOpacity(int percent) {
        progressBackgroundOpacity_ = std::clamp(percent, 0, 100);
        if (taskbarHost)
            taskbarHost->setProgressBackgroundOpacity(progressBackgroundOpacity_);
        logSettingInt(L"progress-background-opacity", progressBackgroundOpacity_);
        saveSettings();
    }

    void applyTaskbarBackground(int mode) {
        taskbarBackground_ = std::clamp(mode, 0, 2);
        if (taskbarHost)
            taskbarHost->setBackground(
                isMinimalRenderMode() ? TaskbarBackground::None
                                       : static_cast<TaskbarBackground>(taskbarBackground_));
        logSettingInt(L"taskbar-background", taskbarBackground_);
        saveSettings();
    }

    void applyCoverBackgroundOpacity(int percent) {
        coverBackgroundOpacity_ = std::clamp(percent, 0, 100);
        if (taskbarHost)
            taskbarHost->setCoverBackgroundOpacity(coverBackgroundOpacity_);
        logSettingInt(L"cover-background-opacity", coverBackgroundOpacity_);
        saveSettings();
    }

    bool isRenderMode(RenderMode mode) const {
        return renderMode_ == static_cast<int>(mode);
    }

    bool isMinimalRenderMode() const {
        return isRenderMode(RenderMode::Minimal);
    }

    bool isNormalRenderMode() const {
        return isRenderMode(RenderMode::Normal);
    }

    // 各设置字段保存用户的常规配置；极简模式只在推送到宿主时临时覆盖有效值，
    // 退出后重新推送原配置，避免用户切换性能模式时丢失显示偏好。竖向任务栏则
    // 额外关闭不适合窄栏的有效显示，但歌词相关配置仍保留，切回横向即可继续使用。
    void applyEffectiveTaskbarSettings() {
        if (!taskbarHost)
            return;
        const bool minimal = isMinimalRenderMode();
        const bool vertical = taskbarVertical_;
        taskbarHost->setSecondaryLyricMode(
            vertical ? false : (secondaryLyricEnabled_ && !preferRomanization_),
            vertical ? false : (secondaryLyricEnabled_ && preferRomanization_));
        taskbarHost->setDoubleLineLyrics(vertical ? false : doubleLineLyricsEnabled_);
        taskbarHost->setLyricAlignment(vertical ? LyricAlignment::Left : lyricAlignment_);
        taskbarHost->setIdleQuoteAlignment(vertical ? LyricAlignment::Left
                                                    : idleQuoteAlignment_);
        taskbarHost->setSongInfoVisible(vertical ? false : songInfoVisible_);
        taskbarHost->setControlsOnHover(hoverPlaybackControls_);
        taskbarHost->setHoverControlStyle(
            minimal ? HoverControlStyle::Inline : hoverControlStyle_);
        taskbarHost->setFloatingCardTrigger(floatingCardTrigger_);
        taskbarHost->setFloatingCardBackground(floatingCardBackground_);
        taskbarHost->setFloatingCardBackgroundColor(effectiveFloatingCardBackgroundColor(),
                                                     floatingCardBackgroundColorCustomized_);
        taskbarHost->setIdleQuoteBackground(
            minimal ? IdleQuoteBackground::None : idleQuoteBackground_);
        taskbarHost->setIdleQuoteBackgroundScope(
            minimal ? IdleQuoteBackgroundScope::None : idleQuoteBackgroundScope_);
        taskbarHost->setFloatingCardFollowAlbum(floatingCardFollowAlbum_);
        taskbarHost->setFloatingCardAutoTextContrast(floatingCardAutoTextContrast_);
        taskbarHost->setAlbumCoverEffect(
            minimal ? AlbumCoverEffect::Default : albumCoverEffect_);
        taskbarHost->setSpectrumStyle(spectrumStyle_);
        taskbarHost->setSpectrumBackground(minimal || vertical ? false : spectrumBackground_);
        taskbarHost->setSpectrumOpacity(spectrumOpacity_);
        taskbarHost->setProgressBackground(minimal ? false : progressBackground_);
        taskbarHost->setProgressBackgroundOpacity(progressBackgroundOpacity_);
        taskbarHost->setBackground(
            minimal ? TaskbarBackground::None
                    : static_cast<TaskbarBackground>(taskbarBackground_));
        taskbarHost->setCoverBackgroundOpacity(coverBackgroundOpacity_);
    }

    // 频谱实际启停 = 用户开关 && 横向任务栏 && 正常渲染模式 && 宿主存在；
    // 低渲染/完全停止/极简模式及竖向任务栏都强制暂停捕获线程。
    void syncSpectrumWithMode() {
        const bool active = spectrumOn_ && !taskbarVertical_ && isNormalRenderMode() &&
                            taskbarHost != nullptr;
        if (active) {
            SmtcSnapshot snap = monitor.snapshot();
            const wchar_t* processName = spectrumProcessName(snap.player);
            if (*processName)
                spectrum_.setTargetProcessName(std::wstring(processName));
            spectrum_.start();
            taskbarHost->setSpectrumVisible(spectrumOn_);
        } else {
            spectrum_.stop();
            if (taskbarHost)
                taskbarHost->setSpectrumVisible(false);
        }
    }

    void syncTaskbarOrientation() {
        if (!taskbarHost)
            return;
        const bool vertical = taskbarHost->isVerticalTaskbar();
        if (vertical == taskbarVertical_)
            return;

        taskbarVertical_ = vertical;
        if (vertical) {
            const bool spectrumChanged = spectrumOn_ || spectrumBackground_ ||
                                         spectrumStyle_ != SpectrumStyle::Default;
            // 频谱及其背景会侵占窄栏的可用空间；切换到竖向时固定关闭并持久化。
            spectrumOn_ = false;
            spectrumBackground_ = false;
            spectrumStyle_ = SpectrumStyle::Default;
            if (spectrumChanged) {
                logSettingBool(L"spectrum", false);
                logSettingBool(L"spectrum-background", false);
                logSettingInt(L"spectrum-style", 0);
                saveSettings();
            }
        }

        applyEffectiveTaskbarSettings();
        syncSpectrumWithMode();
        if (settingsDialog && settingsDialog->isOpen())
            settingsDialog->updateState(currentSettingsState());
    }

    void applyRenderMode(int mode) {
        renderMode_ = std::clamp(mode, 0, 3);
        if (taskbarHost) {
            taskbarHost->setRenderMode(renderMode_);
            applyEffectiveTaskbarSettings();
        }
        syncSpectrumWithMode();
        if (isMinimalRenderMode()) {
            cancelSongToastCoverWait();
            // 切歌弹窗是懒创建的；极简模式下直接销毁已创建的窗口和渲染资源，
            // 退出极简后下一次真正切歌时再按原配置懒创建。
            songToast_.reset();
        } else if (songToast_) {
            songToast_->setEnabled(songToastEnabled_);
        }
        if (settingsDialog && settingsDialog->isOpen())
            settingsDialog->updateState(currentSettingsState());
        logSettingInt(L"render-mode", renderMode_);
        saveSettings();
    }

    void applyHoverControls(bool on) {
        hoverPlaybackControls_ = on;
        if (taskbarHost)
            taskbarHost->setControlsOnHover(on);
        logSettingBool(L"hover-playback-controls", on);
        saveSettings();
    }

    void applyHoverControlStyle(int style) {
        hoverControlStyle_ = style == 1 ? HoverControlStyle::Popup : HoverControlStyle::Inline;
        if (taskbarHost)
            taskbarHost->setHoverControlStyle(
                isMinimalRenderMode() ? HoverControlStyle::Inline : hoverControlStyle_);
        logSettingInt(L"hover-control-style", style == 1 ? 1 : 0);
        saveSettings();
    }

    void applyFloatingCardTrigger(int mode) {
        floatingCardTrigger_ = mode == 1 ? MediaPopupTrigger::Click : MediaPopupTrigger::Hover;
        if (taskbarHost)
            taskbarHost->setFloatingCardTrigger(floatingCardTrigger_);
        logSettingInt(L"floating-card-trigger", mode == 1 ? 1 : 0);
        saveSettings();
    }

    void applyFloatingCardBackground(int mode) {
        floatingCardBackground_ = mode == 1 ? MediaPopupBackground::Frosted
                                             : MediaPopupBackground::Solid;
        if (taskbarHost)
            taskbarHost->setFloatingCardBackground(floatingCardBackground_);
        logSettingInt(L"floating-card-background", mode == 1 ? 1 : 0);
        saveSettings();
    }

    void applyFloatingCardFollowAlbum(bool on) {
        floatingCardFollowAlbum_ = on;
        if (taskbarHost)
            taskbarHost->setFloatingCardFollowAlbum(on);
        logSettingBool(L"floating-card-follow-album", on);
        saveSettings();
    }

    void applyFloatingCardAutoTextContrast(bool on) {
        floatingCardAutoTextContrast_ = on;
        if (taskbarHost)
            taskbarHost->setFloatingCardAutoTextContrast(on);
        logSettingBool(L"floating-card-auto-text-contrast", on);
        saveSettings();
    }

    void applySongToastEnabled(bool on) {
        songToastEnabled_ = on;
        if (isMinimalRenderMode()) {
            cancelSongToastCoverWait();
            songToast_.reset();
        } else if (songToast_) {
            songToast_->setEnabled(on);
        }
        logSettingBool(L"song-toast", on);
        saveSettings();
    }

    void applySongToastDuration(int seconds) {
        songToastDurationSec_ = std::clamp(seconds, 1, 10);
        if (songToast_)
            songToast_->setDurationSec(songToastDurationSec_);
        logSettingInt(L"song-toast-duration", songToastDurationSec_);
        saveSettings();
    }

    void applySongToastSkipFullscreen(bool on) {
        songToastSkipFullscreen_ = on;
        logSettingBool(L"song-toast-skip-fullscreen", on);
        saveSettings();
    }

    void applySongToastPosition(int position) {
        songToastTop_ = position == 0;
        if (songToast_)
            songToast_->setPlacementTop(songToastTop_);
        logSettingInt(L"song-toast-position", songToastTop_ ? 0 : 1);
        saveSettings();
    }

    // 前台窗口覆盖整块显示器（无边框全屏和独占全屏都命中）时视为全屏应用；
    // 桌面、任务栏等外壳窗口不算。
    bool foregroundFullscreen() {
        HWND foreground = GetForegroundWindow();
        if (!foreground || foreground == GetShellWindow())
            return false;
        wchar_t className[64]{};
        if (!GetClassNameW(foreground, className, static_cast<int>(std::size(className))))
            return false;
        static const wchar_t* shellClasses[] = {
            L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        };
        for (const wchar_t* shell : shellClasses) {
            if (wcscmp(className, shell) == 0)
                return false;
        }
        RECT rc{};
        if (!GetWindowRect(foreground, &rc))
            return false;
        HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return false;
        const RECT& mon = info.rcMonitor;
        return rc.left <= mon.left && rc.top <= mon.top && rc.right >= mon.right &&
               rc.bottom >= mon.bottom;
    }

    void cancelSongToastCoverWait() {
        if (songToastCoverWaitArmed_ && trayHwnd)
            KillTimer(trayHwnd, kTimerSongToastCover);
        songToastCoverWaitArmed_ = false;
    }

    // 切歌确认后弹出灵动岛；弹窗懒创建，关闭功能时不创建窗口。
    void notifySongToast() {
        if (isMinimalRenderMode() || !songToastEnabled_ || currentFrame_.media.title.empty())
            return;
        if (songToastSkipFullscreen_ && foregroundFullscreen())
            return;
        if (!songToast_) {
            auto toast = std::make_unique<SongToast>();
            if (!toast->create(GetModuleHandleW(nullptr)))
                return;
            toast->setEnabled(true);
            toast->setDurationSec(songToastDurationSec_);
            toast->setPlacementTop(songToastTop_);
            songToast_ = std::move(toast);
        }
        songToast_->showSong(currentFrame_.media);
        runtime_log::writef(L"[action][song-toast] shown title=%s artist=%s",
                            currentFrame_.media.title.c_str(), currentFrame_.media.artist.c_str());
    }


    void refreshThemeWindows() {
        auto refresh = [](HWND hwnd) {
            if (hwnd) {
                SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
                app_icon::applyWindowIcon(hwnd);
            }
        };
        refresh(settingsDialog ? settingsDialog->hwnd() : nullptr);
        refresh(aboutDialog ? aboutDialog->hwnd() : nullptr);
        refresh(manualSearchDialog ? manualSearchDialog->hwnd() : nullptr);
        refresh(fontPickerDialog ? fontPickerDialog->hwnd() : nullptr);
        if (fontColorDialog) {
            fontColorDialog->refreshTheme();
        }
        refresh(runtimeLogDialog ? runtimeLogDialog->hwnd() : nullptr);
        refresh(updatePromptHwnd_);
    }

    void refreshTheme() {
        fluent::setThemeModes(taskbarThemeMode_, windowThemeMode_);
        updateTrayIcon();
        if (taskbarHost) {
            taskbarHost->refreshTheme();
            applyFontAppearance();
            taskbarHost->setFloatingCardBackgroundColor(effectiveFloatingCardBackgroundColor(),
                                                         floatingCardBackgroundColorCustomized_);
        }
        refreshThemeWindows();
        if (settingsDialog && settingsDialog->isOpen())
            settingsDialog->updateState(currentSettingsState());
    }

    void applyTaskbarTheme(fluent::ThemeMode mode) {
        taskbarThemeMode_ = mode;
        refreshTheme();
        logSettingInt(L"taskbar-theme", static_cast<int>(mode));
        saveSettings();
    }

    void applyWindowTheme(fluent::ThemeMode mode) {
        windowThemeMode_ = mode;
        refreshTheme();
        logSettingInt(L"window-theme", static_cast<int>(mode));
        saveSettings();
    }

    void applyFollowAlbum(bool on) {
        lyricFollowAlbum_ = on;
        if (on) {
            tryExtractAlbumColor(); // 立即用当前封面取色
            applyFontColors();      // 当前曲目已有缓存主色时也要立即刷新歌词
        } else {
            applyFontColors();      // 恢复配置色
        }
        logSettingBool(L"follow-album-color", on);
        saveSettings();
    }

    void applyDoubleLineLyrics(bool on) {
        doubleLineLyricsEnabled_ = on;
        if (taskbarHost)
            taskbarHost->setDoubleLineLyrics(taskbarVertical_ ? false : on);
        logSettingBool(L"double-line-lyrics", on);
        saveSettings();
    }

    void applyLyricAlignment(int alignment) {
        lyricAlignment_ = alignment == 1 ? LyricAlignment::Center
                          : alignment == 2 ? LyricAlignment::Right
                                           : LyricAlignment::Left;
        if (taskbarHost)
            taskbarHost->setLyricAlignment(taskbarVertical_ ? LyricAlignment::Left
                                                             : lyricAlignment_);
        logSettingInt(L"lyric-alignment", alignment == 1 ? 1 : alignment == 2 ? 2 : 0);
        saveSettings();
    }

    void applyIdleQuoteAlignment(int alignment) {
        idleQuoteAlignment_ = alignment == 1 ? LyricAlignment::Center
                              : alignment == 2 ? LyricAlignment::Right
                                               : LyricAlignment::Left;
        if (taskbarHost)
            taskbarHost->setIdleQuoteAlignment(taskbarVertical_ ? LyricAlignment::Left
                                                                 : idleQuoteAlignment_);
        logSettingInt(L"daily-quote-alignment", alignment == 1 ? 1 : alignment == 2 ? 2 : 0);
        saveSettings();
    }

    void applyIdleQuoteBackground(int background) {
        const int normalized = std::clamp(background, 0, 4);
        idleQuoteBackground_ = static_cast<IdleQuoteBackground>(normalized);
        if (taskbarHost)
            taskbarHost->setIdleQuoteBackground(
                isMinimalRenderMode() ? IdleQuoteBackground::None : idleQuoteBackground_);
        logSettingInt(L"daily-quote-background", normalized);
        saveSettings();
    }

    void applyIdleQuoteBackgroundScope(int scope) {
        const int normalized = std::clamp(scope, 0, 3);
        idleQuoteBackgroundScope_ = static_cast<IdleQuoteBackgroundScope>(normalized);
        if (taskbarHost)
            taskbarHost->setIdleQuoteBackgroundScope(
                isMinimalRenderMode() ? IdleQuoteBackgroundScope::None
                                      : idleQuoteBackgroundScope_);
        logSettingInt(L"taskbar-dynamic-background-scope", normalized);
        saveSettings();
    }

    void applySecondaryEnabled(bool on) {
        secondaryLyricEnabled_ = on;
        applySecondaryLyricMode();
        logSettingBool(L"secondary-lyrics", on);
        saveSettings();
    }

    void applyPreferRomanization(bool on) {
        preferRomanization_ = on;
        applySecondaryLyricMode();
        logSettingBool(L"prefer-romanization", on);
        saveSettings();
    }

    void applyQqLocalLyricsEnabled(bool on) {
        qqLocalLyricsEnabled_ = on;
        provider.setQqLocalLyricsConfig(qqLocalLyricsEnabled_, qqLocalLyricsPath_);
        logSettingBool(L"qq-local-lyrics", on);
        saveSettings();
        reloadCurrentQqLyrics();
    }

    void applyQqLocalLyricsPersistOrder(bool on) {
        qqLocalLyricsPersistOrder_ = on;
        logSettingBool(L"qq-local-lyrics-persist-order", on);
        saveSettings();
    }

    void prepareIdleApps() {
        for (auto& app : idleApps_) {
            app.pathValid = validExePath(app.path);
            app.iconPixels.reset();
            app.iconWidth = 0;
            app.iconHeight = 0;
            app.name.clear();
            if (!app.customName.empty()) {
                app.name = app.customName;
            } else if (!app.pathValid) {
                app.name = fallbackExeName(app.path);
            } else if (!platform_icon::readExeDisplayName(app.path, app.name) ||
                       app.name.empty()) {
                app.name = fallbackExeName(app.path);
            }

            if (!app.pathValid)
                continue;

            std::vector<BYTE> pixels;
            UINT width = 0;
            UINT height = 0;
            if (platform_icon::readExeIconPixels(app.path, pixels, width, height) &&
                !pixels.empty() && width > 0 && height > 0) {
                app.iconPixels =
                    std::make_shared<const std::vector<BYTE>>(std::move(pixels));
                app.iconWidth = width;
                app.iconHeight = height;
            }
        }
    }

    bool holidayCalendarFresh(int year) const {
        if (holidayCalendarYear_ != year || holidayCalendarUpdatedAt_ <= 0)
            return false;
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        return now < holidayCalendarUpdatedAt_ ||
               now - holidayCalendarUpdatedAt_ <= kHolidayCacheMaxAgeSec;
    }

    void refreshHolidayCalendar(bool force) {
        const std::tm local = localNow();
        const int year = local.tm_year + 1900;
        if (!force && holidayCalendarFresh(year))
            return;
        if (!force && holidayRequestInFlight_)
            return;
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (!force && holidayRequestYear_ == year && holidayRequestAttemptAt_ > 0 &&
            (now < holidayRequestAttemptAt_ ||
             now - holidayRequestAttemptAt_ < kHolidayCacheMaxAgeSec))
            return;

        ++holidayRequestGeneration_;
        holidayRequestInFlight_ = true;
        holidayRequestYear_ = year;
        holidayRequestAttemptAt_ = now;
        const uint64_t generation = holidayRequestGeneration_;
        idleQuoteProvider_.requestHolidayYearAsync(
            year, [this, generation](HolidayCalendarResult result) {
                auto* payload = new HolidayPayload{generation, std::move(result)};
                if (!PostThreadMessageW(mainThread, kMsgHolidayReady, 0,
                                        reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            });
    }

    IdleDayType todayDayType() const {
        const std::tm local = localNow();
        const std::string today = localDateKey(local);
        if (holidayCalendarYear_ == local.tm_year + 1900) {
            for (const auto& day : holidayCalendar_) {
                if (day.date == today && day.type >= 0 && day.type <= 3)
                    return static_cast<IdleDayType>(day.type);
            }
        }
        // 年度数据尚未返回或接口没有覆盖该日时，先使用自然星期生成首屏文案；
        // 缓存到达后只刷新内容，不影响网络请求和入口状态。
        return local.tm_wday == 0 || local.tm_wday == 6 ? IdleDayType::Weekend
                                                         : IdleDayType::Workday;
    }

    std::wstring defaultIdleWelcome() const {
        const std::tm local = localNow();
        const wchar_t* greeting = local.tm_hour >= 5 && local.tm_hour < 12
                                       ? L"早上好"
                                       : local.tm_hour >= 12 && local.tm_hour < 18
                                             ? L"中午好"
                                             : L"晚上好";
        switch (todayDayType()) {
        case IdleDayType::MakeupWorkday:
            return L"放假最开心，调休伤身体，用一首歌的时间平衡一下吧~";
        case IdleDayType::Weekend:
        case IdleDayType::Holiday:
            return std::wstring(greeting) + L"，牛马快乐日用一首歌开启周末美好时光～";
        case IdleDayType::Workday:
        default:
            return std::wstring(greeting) + L"，致敬奋斗者，闲暇时光用一首歌的时间放松一下心情吧～";
        }
    }

    bool tickTickEffectiveEnabled() const {
        return tickTickEnabled_ && !tickTickApiToken_.empty();
    }

    IdlePresentation currentIdlePresentation() const {
        IdlePresentation presentation;
        presentation.showQuote = idleQuoteEnabled_;
        presentation.quickStartEnabled = idleEntryEnabled_;
        presentation.showAppNames = idleAppNamesVisible_;
        if (idleQuoteEnabled_) {
            presentation.loading = idleQuoteLoading_;
            presentation.sentence = idleQuoteText_;
            if (presentation.sentence.empty())
                presentation.sentence = idleQuoteLoading_ ? L"正在获取每日一言…" : L"暂时无法获取每日一言";
            presentation.source = idleQuoteSourceLabel(idleQuoteSource_);
            presentation.copyEnabled = !idleQuoteLoading_ && !idleQuoteText_.empty();
            if (!idleQuoteOrigin_.empty()) {
                presentation.source += L" · ";
                presentation.source += idleQuoteOrigin_;
            }
        } else {
            presentation.sentence = idleCustomWelcome_.empty() ? defaultIdleWelcome()
                                                               : idleCustomWelcome_;
        }
        presentation.apps = idleApps_;
        presentation.todayTasksEnabled = tickTickEffectiveEnabled();
        presentation.todayTasks = todayTasks_;
        presentation.todayTasksLoading = tickTickTasksLoading_;
        presentation.todayTasksConnected = tickTickConnected_;
        presentation.todayTasksStatus = tickTickStatus_;
        return presentation;
    }

    std::wstring startupTaskSummaryText() const {
        std::size_t remaining = 0;
        std::size_t high = 0;
        std::size_t medium = 0;
        std::size_t low = 0;
        std::size_t none = 0;
        for (const auto& task : todayTasks_) {
            if (task.completed)
                continue;
            ++remaining;
            switch (task.priority) {
            case IdleTaskPriority::High:
                ++high;
                break;
            case IdleTaskPriority::Medium:
                ++medium;
                break;
            case IdleTaskPriority::Low:
                ++low;
                break;
            case IdleTaskPriority::None:
            default:
                ++none;
                break;
            }
        }
        if (remaining == 0)
            return L"今日没有任务";

        std::wstring summary = L"今日剩余 ";
        summary += std::to_wstring(remaining);
        summary += L" 个任务";

        std::size_t emptyPriorityCount = 0;
        const auto appendPriority = [&](const wchar_t* label, std::size_t count) {
            if (count == 0) {
                ++emptyPriorityCount;
                return;
            }
            summary += L"，";
            summary += label;
            summary += L"有 ";
            summary += std::to_wstring(count);
            summary += L" 个";
        };
        appendPriority(L"高优先级", high);
        appendPriority(L"中优先级", medium);
        appendPriority(L"低优先级", low);
        appendPriority(L"无优先级", none);
        if (emptyPriorityCount > 0)
            summary += L"，其余优先级没有任务";
        return summary;
    }

    void refreshIdleWelcome() {
        const SmtcSnapshot snap = monitor.snapshot();
        if (!idleEntryEnabled_ || idleQuoteEnabled_ || snap.sessionAlive ||
            !idleCustomWelcome_.empty())
            return;
        if (currentFrame_.idle.sentence == defaultIdleWelcome())
            return;
        publishPresentationFrame(snap, false, true);
    }

    void onHolidayReady(std::unique_ptr<HolidayPayload> payload) {
        if (!payload || payload->generation != holidayRequestGeneration_)
            return;
        holidayRequestInFlight_ = false;
        const std::tm local = localNow();
        const int currentYear = local.tm_year + 1900;
        if (!payload->result.ok || payload->result.year != currentYear) {
            if (payload->result.year != currentYear)
                refreshHolidayCalendar(false);
            return;
        }

        holidayCalendarYear_ = payload->result.year;
        holidayCalendarUpdatedAt_ = static_cast<int64_t>(std::time(nullptr));
        holidayCalendar_ = std::move(payload->result.days);
        saveSettings();
        if (idleEntryEnabled_ && !idleQuoteEnabled_ && !monitor.snapshot().sessionAlive)
            publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void refreshIdleQuote(bool force) {
        // 每日一言属于空闲面板的独立内容状态；即使启动时已经存在播放器，
        // 也要先把内容准备好，用户切到“每日一言 + 快速打开”面板时才能直接显示。
        if (!idleEntryEnabled_ || !idleQuoteEnabled_)
            return;

        const std::string period = idleQuotePeriodKey(idleQuoteRefreshInterval_);
        const std::string key = std::string(idleQuoteSourceConfigName(idleQuoteSource_)) +
                                "|" + idleQuoteIntervalConfigName(idleQuoteRefreshInterval_) +
                                "|" + period;
        const bool cacheMatches =
            idleQuoteCacheSource_ == idleQuoteSourceConfigName(idleQuoteSource_) &&
            idleQuoteCachePeriod_ == period && !idleQuoteCacheText_.empty();
        if (cacheMatches && !force) {
            idleQuoteText_ = idleQuoteCacheText_;
            idleQuoteOrigin_ = idleQuoteCacheOrigin_;
            idleQuoteUuid_ = idleQuoteCacheUuid_;
            idleQuoteLoading_ = false;
            idleQuoteAttemptKey_ = key;
            publishPresentationFrame(monitor.snapshot(), false, true);
            return;
        }
        if (!force && (idleQuoteRequestKey_ == key || idleQuoteAttemptKey_ == key))
            return;

        ++idleQuoteRequestGeneration_;
        idleQuoteRequestKey_ = key;
        idleQuoteAttemptKey_ = key;
        idleQuoteText_.clear();
        idleQuoteOrigin_.clear();
        idleQuoteUuid_.clear();
        idleQuoteLoading_ = true;
        const uint64_t generation = idleQuoteRequestGeneration_;
        const IdleQuoteSource source = idleQuoteSource_;
        idleQuoteProvider_.requestAsync(
            source, jinrishiciToken_,
            [this, generation](IdleQuoteResult result) {
                auto* payload = new IdleQuotePayload{generation, std::move(result)};
                if (!PostThreadMessageW(mainThread, kMsgIdleQuoteReady, 0,
                                        reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            });
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void onIdleQuoteReady(std::unique_ptr<IdleQuotePayload> payload) {
        if (!idleEntryEnabled_ || !idleQuoteEnabled_ || !payload ||
            payload->generation != idleQuoteRequestGeneration_ ||
            idleQuoteRequestKey_.empty())
            return;

        if (!payload->result.token.empty()) {
            jinrishiciToken_ = payload->result.token;
            saveSettings();
        }
        idleQuoteLoading_ = false;
        if (payload->result.ok) {
            idleQuoteText_ = payload->result.content;
            idleQuoteOrigin_ = payload->result.origin;
            idleQuoteUuid_ = payload->result.uuid;
            idleQuoteCacheSource_ = idleQuoteSourceConfigName(idleQuoteSource_);
            idleQuoteCachePeriod_ = idleQuotePeriodKey(idleQuoteRefreshInterval_);
            idleQuoteCacheText_ = idleQuoteText_;
            idleQuoteCacheOrigin_ = idleQuoteOrigin_;
            idleQuoteCacheUuid_ = idleQuoteUuid_;
            saveSettings();
        }
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void applyIdleQuoteEnabled(bool enabled) {
        if (idleQuoteEnabled_ == enabled)
            return;
        idleQuoteEnabled_ = enabled;
        runtime_log::writef(L"[action][idle-quote] enabled=%s", enabled ? L"on" : L"off");
        if (!enabled) {
            ++idleQuoteRequestGeneration_;
            idleQuoteRequestKey_.clear();
            idleQuoteLoading_ = false;
        } else {
            refreshIdleQuote(false);
        }
        saveSettings();
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void applyIdleEntryEnabled(bool enabled) {
        if (idleEntryEnabled_ == enabled)
            return;
        idleEntryEnabled_ = enabled;
        runtime_log::writef(L"[action][idle-entry] enabled=%s", enabled ? L"on" : L"off");
        if (!enabled) {
            ++idleQuoteRequestGeneration_;
            idleQuoteRequestKey_.clear();
            idleQuoteLoading_ = false;
        } else if (idleQuoteEnabled_) {
            refreshIdleQuote(false);
        }
        saveSettings();
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void applyIdleQuoteSource(int source) {
        const IdleQuoteSource next = source == 1 ? IdleQuoteSource::Jinrishici
                                                  : IdleQuoteSource::Hitokoto;
        if (idleQuoteSource_ == next)
            return;
        idleQuoteSource_ = next;
        ++idleQuoteRequestGeneration_;
        idleQuoteRequestKey_.clear();
        idleQuoteAttemptKey_.clear();
        idleQuoteText_.clear();
        idleQuoteOrigin_.clear();
        idleQuoteUuid_.clear();
        idleQuoteLoading_ = false;
        saveSettings();
        refreshIdleQuote(false);
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void applyIdleQuoteRefreshInterval(int interval) {
        const IdleQuoteRefreshInterval next =
            interval == 2   ? IdleQuoteRefreshInterval::Hourly
            : interval == 1 ? IdleQuoteRefreshInterval::HalfDay
                            : IdleQuoteRefreshInterval::Daily;
        if (idleQuoteRefreshInterval_ == next)
            return;
        idleQuoteRefreshInterval_ = next;
        ++idleQuoteRequestGeneration_;
        idleQuoteRequestKey_.clear();
        idleQuoteAttemptKey_.clear();
        idleQuoteText_.clear();
        idleQuoteOrigin_.clear();
        idleQuoteUuid_.clear();
        idleQuoteLoading_ = false;
        saveSettings();
        refreshIdleQuote(false);
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void addIdleApp(const std::wstring& path) {
        if (idleApps_.size() >= kMaxIdleApps)
            return;
        if (!validExePath(path))
            return;
        for (const auto& app : idleApps_) {
            if (_wcsicmp(app.path.c_str(), path.c_str()) == 0)
                return;
        }
        IdleAppInfo app;
        app.path = path;
        idleApps_.push_back(std::move(app));
        prepareIdleApps();
        runtime_log::writef(L"[action][idle-entry] app-added path=%s", path.c_str());
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateState(currentSettingsState());
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void editIdleApp(int index) {
        if (index < 0 || static_cast<size_t>(index) >= idleApps_.size())
            return;
        if (idleAppNameDialog && idleAppNameDialog->isOpen()) {
            SetForegroundWindow(idleAppNameDialog->hwnd());
            return;
        }
        idleAppNameDialog.reset();

        auto dialog = std::make_unique<IdleAppNameDialog>();
        const auto& app = idleApps_[static_cast<size_t>(index)];
        const std::wstring initial = app.customName.empty() ? app.name : app.customName;
        HWND parent = settingsDialog ? settingsDialog->hwnd() : trayHwnd;
        if (!dialog->create(GetModuleHandleW(nullptr), parent, initial))
            return;
        dialog->setApplyCallback([this, index](const std::wstring& name) {
            if (index < 0 || static_cast<size_t>(index) >= idleApps_.size())
                return;
            auto& app = idleApps_[static_cast<size_t>(index)];
            if (app.customName == name || (app.customName.empty() && app.name == name))
                return;
            app.customName = name;
            prepareIdleApps();
            runtime_log::writef(L"[action][idle-entry] app-name-changed index=%d name=%s",
                                index, app.name.c_str());
            saveSettings();
            if (settingsDialog)
                settingsDialog->updateState(currentSettingsState());
            publishPresentationFrame(monitor.snapshot(), false, true);
        });
        idleAppNameDialog = std::move(dialog);
        idleAppNameDialog->show();
    }

    void editIdleWelcome() {
        if (idleWelcomeDialog && idleWelcomeDialog->isOpen()) {
            SetForegroundWindow(idleWelcomeDialog->hwnd());
            return;
        }
        idleWelcomeDialog.reset();

        auto dialog = std::make_unique<IdleAppNameDialog>();
        HWND parent = settingsDialog ? settingsDialog->hwnd() : trayHwnd;
        if (!dialog->create(GetModuleHandleW(nullptr), parent, idleCustomWelcome_,
                            L"自定义欢迎语",
                            L"最多 20 个字符，留空后恢复默认欢迎语",
                            L"输入欢迎语",
                            static_cast<int>(kIdleCustomWelcomeMaxLen)))
            return;
        dialog->setApplyCallback([this](const std::wstring& text) {
            const std::wstring normalized = normalizeIdleCustomWelcome(text);
            if (idleCustomWelcome_ == normalized)
                return;
            idleCustomWelcome_ = normalized;
            runtime_log::writef(L"[action][idle-quote] custom-welcome len=%llu",
                                static_cast<unsigned long long>(normalized.size()));
            saveSettings();
            publishPresentationFrame(monitor.snapshot(), false, true);
        });
        idleWelcomeDialog = std::move(dialog);
        idleWelcomeDialog->show();
    }

    void setIdleAppNamesVisible(bool show) {
        if (idleAppNamesVisible_ == show)
            return;
        idleAppNamesVisible_ = show;
        runtime_log::writef(L"[action][idle-entry] app-names-visibility show=%d",
                            show ? 1 : 0);
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateState(currentSettingsState());
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void removeIdleApp(int index) {
        if (index < 0 || static_cast<size_t>(index) >= idleApps_.size())
            return;
        runtime_log::writef(L"[action][idle-entry] app-removed path=%s",
                            idleApps_[static_cast<size_t>(index)].path.c_str());
        idleApps_.erase(idleApps_.begin() + index);
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateState(currentSettingsState());
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void reorderIdleApps(int fromIndex, int toIndex) {
        if (fromIndex < 0 || static_cast<size_t>(fromIndex) >= idleApps_.size() ||
            toIndex < 0 || toIndex > static_cast<int>(idleApps_.size()))
            return;
        if (toIndex == fromIndex || toIndex == fromIndex + 1)
            return;

        const std::wstring path = idleApps_[static_cast<size_t>(fromIndex)].path;
        auto begin = idleApps_.begin();
        if (toIndex > fromIndex) {
            std::rotate(begin + fromIndex, begin + fromIndex + 1, begin + toIndex);
        } else {
            std::rotate(begin + toIndex, begin + fromIndex, begin + fromIndex + 1);
        }
        runtime_log::writef(L"[action][idle-entry] app-reordered from=%d to=%d path=%s",
                            fromIndex, toIndex, path.c_str());
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateState(currentSettingsState());
        publishPresentationFrame(monitor.snapshot(), false, true);
    }

    void pickIdleApp() {
        if (idleAppPickerOpen_)
            return;

        idleAppPickerOpen_ = true;
        runtime_log::writef(L"[action][idle-entry] choose-app start");
        const DWORD mainThreadId = mainThread;

        // 主线程由 C++/WinRT 初始化为 MTA；IFileDialog 的模态界面放到独立 STA，
        // 避免在设置窗口的消息处理过程中同步 Show 导致整个 UI 无响应。
        std::thread([mainThreadId] {
            std::wstring selectedPath;
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                       COINIT_DISABLE_OLE1DDE);
            if (SUCCEEDED(init)) {
                IFileOpenDialog* dialog = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                               CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                    const COMDLG_FILTERSPEC filters[] = {{L"应用程序 (*.exe)", L"*.exe"}};
                    DWORD options = 0;
                    if (SUCCEEDED(dialog->GetOptions(&options)))
                        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                           FOS_PATHMUSTEXIST);
                    dialog->SetFileTypes(_countof(filters), filters);
                    dialog->SetTitle(L"添加可打开的应用");
                    if (SUCCEEDED(dialog->Show(nullptr))) {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                            PWSTR path = nullptr;
                            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                                selectedPath = path;
                                CoTaskMemFree(path);
                            }
                            item->Release();
                        }
                    }
                    dialog->Release();
                }
                CoUninitialize();
            }

            runtime_log::writef(L"[action][idle-entry] choose-app result=%s path=%s",
                                selectedPath.empty() ? L"cancelled" : L"selected",
                                selectedPath.c_str());
            auto* payload = new IdleAppPayload{std::move(selectedPath)};
            if (!PostThreadMessageW(mainThreadId, kMsgIdleAppReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        }).detach();
    }

    OverlayMediaInfo makeMediaInfo(const SmtcSnapshot& snap) const {
        OverlayMediaInfo mi;
        const bool retainPrevious = snap.sessionAlive && snap.title.empty() && snap.artist.empty() &&
                                    currentFrame_.frameRevision != 0 &&
                                    currentFrame_.trackKey == currentKey;
        if (retainPrevious) {
            // SMTC 的过渡事件可能暂时没有完整文案，保留当前曲目的媒体字段，
            // 避免用一份不完整快照拼出“旧歌词 + 空标题”的中间画面。
            mi = currentFrame_.media;
        } else {
            mi.title = snap.title.empty() ? currentTitle : snap.title;
            mi.artist = snap.artist.empty() ? currentArtist : snap.artist;
            if (!snap.album.empty()) {
                mi.artist += L" · ";
                mi.artist += snap.album;
            }
            mi.sourceAppUserModelId = snap.sourceAppUserModelId;
        }
        if (snap.thumbnail && !snap.thumbnail->empty())
            mi.thumbnail = snap.thumbnail;
        else if (lastSmtcThumbnail && !lastSmtcThumbnail->empty())
            mi.thumbnail = lastSmtcThumbnail;
        else
            mi.thumbnail = lastCover_;
        mi.hasDominantColor = hasAlbumColor_;
        mi.dominantColor = albumColor_;
        // QQ 切歌时旧时间线会暂时残留，不能把上一首的总时长带进媒体卡片。
        mi.durationMs = !snap.timelineStale && snap.durationMs > 0 ? snap.durationMs : 0;
        mi.canPrev = snap.canPrev;
        mi.canPlayPause = snap.canPlayPause;
        mi.canNext = snap.canNext;
        mi.playing = snap.status == PlaybackStatus::Playing;
        return mi;
    }

    DisplayScene displaySceneFor(const SmtcSnapshot& snap) const {
        if (!snap.sessionAlive)
            return idleEntryEnabled_ ? DisplayScene::Idle : DisplayScene::NoPlayback;
        if (lyricLoading_)
            return DisplayScene::Searching;
        if (!currentLyrics_.empty())
            return DisplayScene::Lyrics;
        return spectrumOn_ ? DisplayScene::Spectrum : DisplayScene::Message;
    }

    PresentationFrame buildPresentationFrame(const SmtcSnapshot& snap,
                                              bool animateTransition) const {
        PresentationFrame frame;
        frame.requestGeneration = requestGeneration_;
        frame.trackKey = currentKey;
        frame.scene = displaySceneFor(snap);
        frame.media = makeMediaInfo(snap);
        frame.idle = currentIdlePresentation();
        frame.lyrics = currentLyrics_;
        frame.actualPositionMs = snap.positionMs;
        frame.lineSelectionPositionMs = snap.positionMs + kLyricTransitionLeadMs;
        frame.currentLine = LyricProvider::findLine(frame.lyrics, frame.lineSelectionPositionMs);
        frame.visible = snap.sessionAlive || (idleEntryEnabled_ && !snap.sessionAlive);
        frame.animateTransition = animateTransition;
        const bool showStartupTaskSummary = startupTaskSummaryActive_ &&
                                             !snap.sessionAlive &&
                                             frame.scene == DisplayScene::Idle;
        const bool showStartupTaskLoading = startupTaskSummaryPending_ &&
                                             tickTickEffectiveEnabled() &&
                                             !snap.sessionAlive &&
                                             frame.scene == DisplayScene::Idle;
        if (showStartupTaskSummary) {
            frame.statusText = startupTaskSummaryText();
            frame.statusTextOneShot = true;
        } else if (showStartupTaskLoading) {
            frame.statusText = L"正在同步今日任务…";
        } else if (frame.scene == DisplayScene::Idle)
            frame.statusText = frame.idle.sentence;
        else if (!snap.sessionAlive)
            frame.statusText = notRunningStatus();
        else if (lyricLoading_)
            frame.statusText = L"歌词加载中…";
        else if (currentLyrics_.empty())
            frame.statusText = currentKey.empty() ? L"等待播放…" : L"暂无歌词";
        return frame;
    }

    void publishPresentationFrame(const SmtcSnapshot& snap, bool animateTransition,
                                  bool lyricsChanged = false,
                                  bool durationOnlyUpdate = false) {
        // SMTC 状态/控制/封面事件只刷新帧字段；完整歌词仅在内容事务变化时复制。
        if (frameRevision_ == 0 || lyricsChanged)
            currentFrame_.lyrics = currentLyrics_;
        currentFrame_.requestGeneration = requestGeneration_;
        currentFrame_.trackKey = currentKey;
        currentFrame_.scene = displaySceneFor(snap);
        currentFrame_.media = makeMediaInfo(snap);
        currentFrame_.idle = currentIdlePresentation();
        currentFrame_.actualPositionMs = snap.positionMs;
        currentFrame_.lineSelectionPositionMs = snap.positionMs + kLyricTransitionLeadMs;
        currentFrame_.currentLine =
            LyricProvider::findLine(currentFrame_.lyrics, currentFrame_.lineSelectionPositionMs);
        currentFrame_.visible =
            snap.sessionAlive || (idleEntryEnabled_ && !snap.sessionAlive);
        currentFrame_.animateTransition = animateTransition;
        currentFrame_.durationOnlyUpdate = durationOnlyUpdate;
        const bool showStartupTaskSummary = startupTaskSummaryActive_ &&
                                             !snap.sessionAlive &&
                                             currentFrame_.scene == DisplayScene::Idle;
        const bool showStartupTaskLoading = startupTaskSummaryPending_ &&
                                             tickTickEffectiveEnabled() &&
                                             !snap.sessionAlive &&
                                             currentFrame_.scene == DisplayScene::Idle;
        currentFrame_.statusTextOneShot = showStartupTaskSummary;
        if (showStartupTaskSummary)
            currentFrame_.statusText = startupTaskSummaryText();
        else if (showStartupTaskLoading)
            currentFrame_.statusText = L"正在同步今日任务…";
        else if (currentFrame_.scene == DisplayScene::Idle)
            currentFrame_.statusText = currentFrame_.idle.sentence;
        else if (!snap.sessionAlive)
            currentFrame_.statusText = notRunningStatus();
        else if (lyricLoading_)
            currentFrame_.statusText = L"歌词加载中…";
        else if (currentLyrics_.empty())
            currentFrame_.statusText = currentKey.empty() ? L"等待播放…" : L"暂无歌词";
        else
            currentFrame_.statusText.clear();
        currentFrame_.frameRevision = ++frameRevision_;
        for (auto* h : hosts())
            h->applyPresentationFrame(currentFrame_);
        // 弹窗可见期间异步补齐封面等字段；隐藏时只缓存不渲染
        if (songToast_)
            songToast_->setMedia(currentFrame_.media);
        // 切歌时封面未到会短暂等待；封面在这帧补齐则立即弹出
        if (songToastCoverWaitArmed_ && currentFrame_.media.thumbnail &&
            !currentFrame_.media.thumbnail->empty()) {
            cancelSongToastCoverWait();
            notifySongToast();
        }
    }

    bool createTaskbar(HINSTANCE inst) {
        if (taskbarHost) return true;
        auto host = std::make_unique<TaskbarHost>();
        if (!host->create(inst)) {
            runtime_log::writef(L"failed to create taskbar host");
            return false;
        }
        host->setTickCallback([this] { onFrame(); });
        host->setControlCallback([this](MediaControl c) { onControl(c); });
        host->setAppVolumeCallback([this](int percent) {
            const bool ok = appVolume_.setVolumePercent(percent);
            runtime_log::writef(L"[action][volume] set percent=%d result=%s", percent,
                                ok ? L"ok" : L"unavailable");
            pushAppVolume();
        });
        host->setSourceOpenCallback([](const std::wstring& source) {
            const bool ok = platform_icon::launchSourceApp(source);
            runtime_log::writef(L"[action][player] open-source source=%s result=%s", source.c_str(),
                                ok ? L"ok" : L"failed");
            if (!ok)
                runtime_log::writef(L"[player] failed to activate source: %s", source.c_str());
        });
        host->setIdleAppOpenCallback([this](const std::wstring& path) {
            const bool ok = platform_icon::launchConfiguredExe(path);
            runtime_log::writef(L"[action][idle-entry] app-open path=%s result=%s", path.c_str(),
                                ok ? L"ok" : L"failed");
            if (!ok) {
                for (auto& app : idleApps_) {
                    if (_wcsicmp(app.path.c_str(), path.c_str()) == 0) {
                        app.pathValid = validExePath(app.path);
                        break;
                    }
                }
                if (settingsDialog)
                    settingsDialog->updateState(currentSettingsState());
                publishPresentationFrame(monitor.snapshot(), false, true);
            }
        });
        host->setIdleTaskOpenCallback([this](const IdleTaskInfo& task) {
            openTickTickTask(task);
        });
        host->setIdleTaskCompleteCallback([this](const IdleTaskInfo& task) {
            completeTickTickTask(task);
        });
        host->setMediaPopupOpenedCallback([this] { refreshTickTickTasks(); });
        host->setStatusTextCycleCompletedCallback([this] {
            onStartupTaskSummaryCompleted();
        });
        taskbarHost = std::move(host);
        syncHost(taskbarHost.get());
        if (hasUserFont_)
            taskbarHost->setFont(fontFamily_, fontSize_, fontStyle_);
        applyFontAppearance();
        taskbarHost->setSecondaryLyricMode(secondaryLyricEnabled_ && !preferRomanization_,
                                           secondaryLyricEnabled_ && preferRomanization_);
        taskbarHost->setDoubleLineLyrics(doubleLineLyricsEnabled_);
        taskbarHost->setLyricAlignment(lyricAlignment_);
        taskbarHost->setIdleQuoteAlignment(idleQuoteAlignment_);
        taskbarHost->setSongInfoVisible(songInfoVisible_);
        taskbarHost->setAlbumCoverVisible(albumCoverVisible_);
        taskbarHost->setPlatformIconVisible(platformIconVisible_);
        taskbarHost->setPositionMode(taskbarPosition_);
        taskbarHost->setRenderMode(renderMode_);
        syncTaskbarOrientation();
        applyEffectiveTaskbarSettings();
        taskbarHost->setAppVolume(appVolumeState_); // 同步当前音量状态（可能早于宿主创建）
        syncSpectrumWithMode();
        updateTrayIcon();
        return true;
    }

    void destroyTaskbar() {
        spectrum_.stop(); // 频谱只画在任务栏上，宿主销毁时捕获线程一并停
        auto host = std::move(taskbarHost);
        host.reset();
        updateTrayIcon();
    }

    void requestQuit() {
        if (shutdownRequested_)
            return;
        shutdownRequested_ = true;
        runtimeLogger_.write(L"[lifecycle] quit requested");
        runtimeLogger_.flushSync();

        // 先销毁仍由 Explorer 承载的任务栏窗口和托盘窗口，再结束消息循环。
        // 仅投递 WM_QUIT 会把这部分清理推迟到 main() 返回后的析构阶段，
        // 任务栏歌词窗口或探测线程可能在此期间继续存活。
        destroyTaskbar();
        closeUpdatePrompt();
        destroyTray();
        PostQuitMessage(0);
    }

    void toggleTaskbar() {
        runtime_log::writef(L"[action][taskbar] toggle current=%s",
                            taskbarHost ? L"enabled" : L"disabled");
        if (taskbarHost) {
            destroyTaskbar();
        } else {
            createTaskbar(GetModuleHandleW(nullptr));
        }
        runtime_log::writef(L"[action][taskbar] toggle result=%s",
                            taskbarHost ? L"enabled" : L"disabled");
    }

    void showManualSearch(HINSTANCE inst) {
        runtime_log::writef(L"[action][dialog] open=manual-search title=%s artist=%s",
                            currentTitle.c_str(), currentArtist.c_str());
        if (manualSearchDialog && !manualSearchDialog->isOpen()) {
            manualSearchDialog.reset();
        }
        if (manualSearchDialog) {
            manualSearchDialog->show();
            return;
        }
        const SmtcSnapshot manualTarget = monitor.snapshot();
        const std::wstring targetNeteaseSongId =
            manualTarget.player == SmtcPlayerType::NetEase
                ? manualTarget.neteaseSongId
                : L"";
        const std::wstring manualKey = currentKey;
        manualSearchDialog = std::make_unique<ManualSearchDialog>();
        if (!manualSearchDialog->create(inst, trayHwnd, &provider, currentTitle, currentArtist,
                                        currentDurationMs, targetNeteaseSongId)) {
            manualSearchDialog.reset();
            return;
        }
        manualSearchDialog->setApplyCallback([this, manualKey] {
            SmtcSnapshot snap = monitor.snapshot();
            if (manualKey.empty() || !snap.sessionAlive || currentKey != manualKey ||
                makeTrackKey(snap) != manualKey)
                return;

            // 手动覆盖是新的内容事务，使仍在队列中的自动歌词/封面结果失效。
            ++requestGeneration_;
            cancelLyricDebounce(); // 防止挂起的切歌防抖请求到点后覆盖手动选择
            releaseCurrentLyrics();
            currentLyrics_ = provider.lines();
            currentLyricsFromLocal_ = false;
            currentLyricsFromManual_ = true;
            lyricLoading_ = false;
            updateLyricCapabilities(currentLyrics_);
            logLyricsCreated(L"manual");
            publishPresentationFrame(snap, true, true);
            runtime_log::writef(L"[lyric] manual override applied: %s", currentKey.c_str());
            updateRuntimeLogState(snap);
            // 手动选择后同样兜底封面
            if (!lastSmtcThumbnail || lastSmtcThumbnail->empty()) {
                const std::wstring albummid = provider.songInfo().albummid;
                if (!albummid.empty()) {
                    const std::wstring key = currentKey;
                    const uint64_t generation = requestGeneration_;
                    coverProvider.requestAsync(albummid,
                        [this, key, generation](std::shared_ptr<const std::vector<uint8_t>> cover) {
                            if (!cover || cover->empty()) return;
                            auto* payload = new CoverPayload{key, generation, std::move(cover)};
                            if (!PostThreadMessageW(mainThread, kMsgCoverReady, 1,
                                                    reinterpret_cast<LPARAM>(payload)))
                                delete payload;
                        });
                }
            }
        });
        manualSearchDialog->show();
    }

    // 把当前播放状态同步给某个宿主（新建宿主时避免显示“等待播放…”）
    void syncHost(ILyricHost* host) {
        if (!host)
            return;
        if (currentFrame_.frameRevision == 0) {
            currentFrame_ = buildPresentationFrame(monitor.snapshot(), false);
            currentFrame_.frameRevision = ++frameRevision_;
        }
        host->applyPresentationFrame(currentFrame_);
    }

    void onControl(MediaControl c) {
        const wchar_t* name = L"unknown";
        switch (c) {
        case MediaControl::Prev:
            name = L"previous";
            monitor.skipPrevious();
            break;
        case MediaControl::PlayPause:
            name = L"play-pause";
            monitor.playPause();
            break;
        case MediaControl::Next:
            name = L"next";
            monitor.skipNext();
            break;
        }
        runtime_log::writef(L"[action][media-control] command=%s", name);
    }

    // 读取当前音乐应用的会话音量并推送给各宿主；状态未变时跳过
    void pushAppVolume() {
        AppVolumeState state;
        state.available = appVolume_.query(state.percent, state.muted);
        if (state.available == appVolumeState_.available &&
            (!state.available || (state.percent == appVolumeState_.percent &&
                                  state.muted == appVolumeState_.muted)))
            return;
        appVolumeState_ = state;
        for (auto* h : hosts())
            h->setAppVolume(state);
    }

    // SMTC 来源播放器切换时更新音量控制目标进程；无会话时清空（音量不可用）
    void syncAppVolumeTarget(const SmtcSnapshot& snap) {
        appVolume_.setTargetProcessNames(
            volumeProcessNames(snap.sessionAlive ? snap.player : SmtcPlayerType::Unknown));
        pushAppVolume();
    }

    // 发起一次歌词请求：网易云逻辑保持不变（事件完整、可直接请求）；
    // QQ 由防抖定时器在切歌事件合并完成后调用。
    void startLyricRequest(const SmtcSnapshot& snap, bool forceOnline = false,
                           bool forceLocal = false, bool persistOrder = false) {
        lyricLoading_ = true;
        currentLyricsFromLocal_ = false;
        currentLyricsFromManual_ = false;
        lyricRequestStale_ = snap.timelineStale;
        const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
        ++requestGeneration_;
        updateRuntimeLogState(snap);
        const std::wstring requestKey = currentKey;
        const uint64_t generation = requestGeneration_;
        auto postLyricResult = [this, requestKey, generation](bool ok) {
            auto* payload = new LyricPayload{requestKey, generation, ok};
            if (!PostThreadMessageW(mainThread, kMsgLyricReady, ok ? 1 : 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        };
        if (snap.player == SmtcPlayerType::NetEase) {
            provider.requestNeteaseAsync(snap.neteaseSongId, snap.title, snap.artist,
                                         durationMs, postLyricResult);
        } else {
            provider.requestAsync(snap.title, snap.artist, durationMs,
                                  postLyricResult, forceOnline, forceLocal, persistOrder);
        }
    }

    void cancelLyricDebounce() {
        if (lyricDebounceArmed_ && trayHwnd)
            KillTimer(trayHwnd, kTimerLyricDebounce);
        lyricDebounceArmed_ = false;
    }

    // 防抖到点：此时切歌事件批已处理完（线程消息优先于 WM_TIMER 派发），
    // 快照身份必然已等于 currentKey；不等说明还有未处理事件，直接放弃本轮，
    // 由该事件的 onSmtcChanged 重新武装。
    void onLyricDebounce() {
        lyricDebounceArmed_ = false;
        SmtcSnapshot snap = monitor.snapshot();
        if (!snap.sessionAlive || snap.player != SmtcPlayerType::QQMusic ||
            snap.title.empty())
            return;
        if (makeTrackKey(snap) != currentKey)
            return;
        if (snap.timelineStale && GetTickCount64() < lyricDebounceDeadline_) {
            // 时间线仍是旧歌残留：durationMs 不可信， lyric 请求的时长过滤会
            // 把正确候选全部淘汰（失败会先闪「暂无歌词」）。继续等新时间线。
            if (trayHwnd)
                lyricDebounceArmed_ =
                    SetTimer(trayHwnd, kTimerLyricDebounce, kLyricDebounceMs, nullptr) != 0;
            return;
        }
        startLyricRequest(snap);
    }

    // 状态机：无会话(隐藏) -> 播放中(滚动渲染) <-> 暂停(静止显示)
    void onSmtcChanged() {
        SmtcSnapshot snap = monitor.snapshot();
        syncAppVolumeTarget(snap);
        if (snap.sessionAlive) {
            // 媒体会话优先于启动任务播报；即使任务请求先返回，也不能覆盖已经在播放的歌词。
            startupTaskSummaryPending_ = false;
            startupTaskSummaryActive_ = false;
        }
        if (!snap.sessionAlive) {
            if (spectrumSessionAlive_)
                spectrum_.requestReconnect();
            spectrumSessionAlive_ = false;
            spectrumSessionKey_.clear();
            if (!currentKey.empty() || lastStatus != PlaybackStatus::Stopped)
                runtime_log::writef(L"[smtc] %s session closed", playerName(lastPlayer_));
            currentKey.clear();
            currentTitle.clear();
            currentArtist.clear();
            currentDurationMs = 0;
            lastStatus = PlaybackStatus::Stopped;
            lyricLoading_ = false;
            cancelLyricDebounce();
            ++requestGeneration_;
            releaseCurrentResources();
            hasAlbumColor_ = false;
            refreshIdleQuote(false);
            publishPresentationFrame(snap, false, true);
            cancelSongToastCoverWait();
            if (songToast_)
                songToast_->hideImmediate();
            updateRuntimeLogState(snap);
            return;
        }
        const SmtcPlayerType previousPlayer = lastPlayer_;
        lastPlayer_ = snap.player;
        const wchar_t* spectrumProcess = spectrumProcessName(snap.player);
        if (*spectrumProcess) {
            spectrum_.setTargetProcessName(std::wstring(spectrumProcess));
            if (spectrumOn_ && isNormalRenderMode())
                spectrum_.start();
            // 把播放器源纳入频谱会话键：两个播放器播放同一首歌时也必须切换捕获目标。
            std::wstring spectrumKey = makeTrackKey(snap);
            if (!spectrumSessionAlive_ || spectrumSessionKey_ != spectrumKey)
                spectrum_.requestReconnect();
            spectrumSessionAlive_ = true;
            spectrumSessionKey_ = spectrumKey;
        } else {
            // 当前会话不是已适配的播放器时，不再保留旧播放器的捕获线程。
            spectrum_.stop();
            spectrumSessionAlive_ = false;
            spectrumSessionKey_.clear();
        }

        const std::wstring key = makeTrackKey(snap);
        const bool trackChanged = key != currentKey && !snap.title.empty();
        // QQ 的时间线可能晚于媒体属性到达：同一首歌先以未知时长（0）进入，
        // 随后补上真实时长会让包含时长的 trackKey 变化。这个补齐不是换歌，
        // 不能因此清掉当前封面/专辑色；歌词仍按新时长重新建立匹配键。
        const bool durationOnlyTrackUpdate =
            trackChanged && snap.player == previousPlayer && !snap.timelineStale &&
            currentDurationMs <= 0 && snap.durationMs > 0 &&
            snap.title == currentTitle && snap.artist == currentArtist &&
            snap.sourceAppUserModelId == currentFrame_.media.sourceAppUserModelId;
        // QQ 的曲目键包含时长，切歌后时间线更新会让 trackChanged 再触发一次；
        // 切歌弹窗只认标题/艺术家身份变化，避免同一首歌弹两次。
        const bool songIdentityChanged =
            !snap.title.empty() &&
            (snap.title != currentTitle || snap.artist != currentArtist);
        const bool newSmtcThumbnail = snap.thumbnail && !snap.thumbnail->empty() &&
                                      (!lastSmtcThumbnail || lastSmtcThumbnail != snap.thumbnail);
        if (snap.status != lastStatus) {
            runtime_log::writef(L"[smtc] %s status: %s", playerName(snap.player),
                                statusName(snap.status));
            lastStatus = snap.status;
        }
        if (trackChanged) {
            // 时长补齐仍需重建歌词请求，但媒体卡片继续沿用当前封面和专辑色，
            // 避免 0 → 有效时长的那一帧出现空卡片或背景闪回。
            releaseCurrentLyrics();
            if (!durationOnlyTrackUpdate)
                releaseCurrentCover();
            currentKey = key;
            currentTitle = snap.title;
            currentArtist = snap.artist;
            currentDurationMs = snap.timelineStale ? 0 : snap.durationMs;
            if (snap.player == SmtcPlayerType::NetEase) {
                runtime_log::writef(L"[smtc] %s track: %s - %s [%s] (%lld ms)",
                                    playerName(snap.player), snap.title.c_str(), snap.artist.c_str(),
                                    snap.neteaseSongId.c_str(), snap.durationMs);
            } else {
                runtime_log::writef(L"[smtc] %s track: %s - %s (%lld ms)",
                                    playerName(snap.player), snap.title.c_str(), snap.artist.c_str(),
                                    snap.durationMs);
            }
            if (snap.thumbnail && !snap.thumbnail->empty()) {
                lastCover_ = snap.thumbnail;
                logCoverCreated(L"smtc", lastCover_);
            }
            if (!durationOnlyTrackUpdate)
                hasAlbumColor_ = false;
            // 跟随专辑开启时不回退配置色：沿用上一首的专辑色直到新封面取色完成，
            // 避免歌词加载窗口期配置色闪一下再切回专辑色
            lyricLoading_ = true;
            if (snap.player == SmtcPlayerType::NetEase) {
                cancelLyricDebounce();
                startLyricRequest(snap);
            } else if (trayHwnd) {
                // QQ：等 SMTC 切歌事件批合并后再请求；期间 lyricLoading_ 保持 true，
                // 界面停留在「歌词加载中…」而不是闪「暂无歌词」。
                if (lyricDebounceArmed_)
                    KillTimer(trayHwnd, kTimerLyricDebounce);
                lyricDebounceDeadline_ = GetTickCount64() + 8000;
                lyricDebounceArmed_ =
                    SetTimer(trayHwnd, kTimerLyricDebounce, kLyricDebounceMs, nullptr) != 0;
                if (!lyricDebounceArmed_)
                    startLyricRequest(snap); // 定时器创建失败时按原逻辑立即请求
            } else {
                startLyricRequest(snap);
            }
        } else {
            if (!snap.timelineStale && snap.durationMs > 0)
                currentDurationMs = snap.durationMs;
            if (newSmtcThumbnail) {
                if (lastSmtcThumbnail && !lastSmtcThumbnail->empty()) {
                    runtime_log::writef(
                        L"[resource][event] cover-released source=smtc bytes=%zu",
                        lastSmtcThumbnail->size());
                } else if (lastCover_ && !lastCover_->empty() && lastCover_ != snap.thumbnail) {
                    runtime_log::writef(
                        L"[resource][event] cover-released source=api bytes=%zu",
                        lastCover_->size());
                }
                logCoverCreated(L"smtc", snap.thumbnail);
            }
            if (snap.thumbnail && !snap.thumbnail->empty())
                lastCover_ = snap.thumbnail;
        }
        lastSmtcThumbnail = snap.thumbnail;
        // 时间线从残留恢复可信（stale 1→0）：若当前歌词请求是在不可信期间发出的
        // （等待超时被迫发出），用可信时长立即重发，覆盖可能已被时长过滤淘汰的结果。
        if (!trackChanged && snap.player == SmtcPlayerType::QQMusic &&
            lyricRequestStale_ && !snap.timelineStale && !lyricDebounceArmed_ &&
            lyricLoading_ && currentLyrics_.empty()) {
            startLyricRequest(snap);
        }
        publishPresentationFrame(snap, !trackChanged, trackChanged, durationOnlyTrackUpdate);
        if (trackChanged && songIdentityChanged && songToastEnabled_ &&
            !isMinimalRenderMode()) {
            cancelSongToastCoverWait();
            if (currentFrame_.media.thumbnail && !currentFrame_.media.thumbnail->empty())
                notifySongToast();
            else if (trayHwnd)
                songToastCoverWaitArmed_ =
                    SetTimer(trayHwnd, kTimerSongToastCover, kSongToastCoverWaitMs, nullptr) != 0;
        }
        tryExtractAlbumColor();
        updateRuntimeLogState(snap);
    }

    void onLyricReady(std::unique_ptr<LyricPayload> payload) {
        if (!payload || payload->key != currentKey ||
            payload->requestGeneration != requestGeneration_)
            return;
        SmtcSnapshot snap = monitor.snapshot();
        if (!snapshotMatchesTrackKey(snap, currentKey))
            return;

        lyricLoading_ = false;
        if (payload->ok) {
            currentLyrics_ = provider.lines();
            currentLyricsFromLocal_ = snap.player == SmtcPlayerType::QQMusic &&
                                      provider.lastLoadWasLocal();
            currentLyricsFromManual_ = provider.lastLoadWasManual();
            updateLyricCapabilities(currentLyrics_);
            logLyricsCreated(currentLyricsFromManual_
                                 ? L"manual"
                                 : currentLyricsFromLocal_ ? L"local" : L"online");
            runtime_log::writef(L"[lyric] loaded %zu lines: %s", currentLyrics_.size(),
                                currentKey.c_str());
            // 仅 QQ 继续使用现有封面兜底；网易云阶段一不把 QQ albummid 接口当作其数据源。
            if (snap.player == SmtcPlayerType::QQMusic &&
                (!lastSmtcThumbnail || lastSmtcThumbnail->empty())) {
                const std::wstring albummid = provider.songInfo().albummid;
                if (albummid.empty()) {
                    runtime_log::writef(L"[cover] no albummid from search: %s",
                                        currentKey.c_str());
                } else {
                    const std::wstring key = currentKey;
                    const uint64_t generation = requestGeneration_;
                    coverProvider.requestAsync(albummid, [this, key, generation](
                                                   std::shared_ptr<const std::vector<uint8_t>> cover) {
                        if (!cover || cover->empty()) return;
                        auto* payload = new CoverPayload{key, generation, std::move(cover)};
                        if (!PostThreadMessageW(mainThread, kMsgCoverReady, 1,
                                                reinterpret_cast<LPARAM>(payload)))
                            delete payload;
                    });
                }
            }
        } else {
            if (lyricRequestStale_) {
                // 时间线不可信期间发出的请求：失败大概率是旧时长触发候选的 15 秒
                // 时长过滤，不能据此判定「暂无歌词」。保持「加载中」，等新时间线
                // 可信后由 onSmtcChanged 的重试分支用正确时长重发。
                lyricLoading_ = true;
                publishPresentationFrame(snap, false);
                updateRuntimeLogState(snap);
                return;
            }
            releaseCurrentLyrics();
            runtime_log::writef(L"[lyric] not found: %s", currentKey.c_str());
        }
        publishPresentationFrame(snap, true, true);
        updateRuntimeLogState(snap);
    }

    void onCoverReady(std::unique_ptr<CoverPayload> payload) {
        if (!payload || !payload->cover || payload->key != currentKey ||
            payload->requestGeneration != requestGeneration_)
            return; // 已切歌或重新加载，丢弃过期封面
        SmtcSnapshot snap = monitor.snapshot();
        if (!snapshotMatchesTrackKey(snap, currentKey))
            return;
        if (lastSmtcThumbnail && !lastSmtcThumbnail->empty()) return; // SMTC 已提供有效封面，优先使用
        runtime_log::writef(L"[cover] loaded from API: %s", currentKey.c_str());
        lastCover_ = payload->cover;
        logCoverCreated(L"api", lastCover_);
        publishPresentationFrame(snap, true);
        tryExtractAlbumColor();
        updateRuntimeLogState(snap);
    }

    // 30fps：插值进度 -> 二分定位当前行
    void onFrame() {
        syncTaskbarOrientation();
        SmtcSnapshot snap = monitor.snapshot();
        if (snap.sessionAlive && (startupTaskSummaryPending_ || startupTaskSummaryActive_)) {
            // 定时器也检查一次，避免媒体事件消息尚未出队时让启动播报多停留一帧。
            startupTaskSummaryPending_ = false;
            startupTaskSummaryActive_ = false;
            publishPresentationFrame(snap, false, true);
        }
        if (!snap.sessionAlive) return;
        if (!snapshotMatchesTrackKey(snap, currentKey))
            return;
        const int64_t lineSelectionPositionMs =
            snap.positionMs + kLyricTransitionLeadMs;
        const int idx = LyricProvider::findLine(currentLyrics_, lineSelectionPositionMs);
        currentFrame_.actualPositionMs = snap.positionMs;
        currentFrame_.lineSelectionPositionMs = lineSelectionPositionMs;
        currentFrame_.currentLine = idx;
        currentFrame_.media.playing = snap.status == PlaybackStatus::Playing;
        PlaybackPatch playback;
        playback.frameRevision = currentFrame_.frameRevision;
        playback.requestGeneration = currentFrame_.requestGeneration;
        playback.actualPositionMs = snap.positionMs;
        playback.lineSelectionPositionMs = lineSelectionPositionMs;
        playback.currentLine = idx;
        playback.playing = currentFrame_.media.playing;
        for (auto* h : hosts()) {
            // 提前切换显示行，让上下滚动动画在下一句真正开始前完成；
            // 补丁中的 actualPositionMs 仍是真实播放时间，避免逐字高亮跟着提前。
            h->applyPlaybackPatch(playback);
        }
        if (taskbarHost && spectrumOn_ && !taskbarVertical_) {
            SpectrumPatch spectrumPatch;
            spectrumPatch.frameRevision = currentFrame_.frameRevision;
            spectrumPatch.requestGeneration = currentFrame_.requestGeneration;
            spectrumPatch.bands = spectrum_.bands();
            taskbarHost->applySpectrumPatch(spectrumPatch);
        }
        if (manualSearchDialog && manualSearchDialog->isOpen()) {
            manualSearchDialog->setPlaybackPosition(snap.positionMs);
        }
    }

    // 统一托盘图标
    bool createTrayWindow(HINSTANCE inst);
    void destroyTray();
    void updateTrayIcon();
    void showTrayMenu();
    void onMenuCommand(int cmd);
    void showRuntimeLog();
    void initializeRuntimeLogger();
    void setLogDirectory(const std::wstring& path);
    void setLogRetentionDays(int days);
    void showSettings();
    SettingsState currentSettingsState() const;
    SettingsActions buildSettingsActions();
    void pickFont();
    void showFontColorDialog();
    void showAbout(bool downloadUpdate = false);
    void showUpdatePrompt(const std::wstring& latestVersion);
    void layoutUpdatePrompt();
    void paintUpdatePrompt(fluent::FluentDialogSurface::Painter& painter, float width,
                           float height);
    void drawUpdatePromptButton(fluent::FluentDialogSurface::Painter& painter,
                                const D2D1_RECT_F& rect, const wchar_t* text, bool accent,
                                int id);
    int hitTestUpdatePrompt(float x, float y) const;
    void focusStepUpdatePrompt(int direction);
    void closeUpdatePrompt();
    void onUpdatePromptCommand(int command);
    bool launchUpdateInstaller(const std::wstring& path);
    void onDialogClosed(DialogKind kind);
    void setAutoCheckOnStartup(bool enabled);
    void pickQqLocalLyricsPath();
    void applyQqLocalLyricsPath(const std::wstring& path);
    void applyTickTickEnabled(bool enabled);
    void editTickTickApiToken(bool enableAfterSave = false);
    void connectTickTick();
    void refreshTickTickTasks();
    void completeTickTickTask(const IdleTaskInfo& task);
    void disconnectTickTick();
    void onTickTickTasksReady(std::unique_ptr<TickTickTasksPayload> payload);
    void onTickTickTaskCompleteReady(
        std::unique_ptr<TickTickTaskCompletePayload> payload);
    void onStartupTaskSummaryCompleted();
    void openTickTickTask(const IdleTaskInfo& task);
    void reloadCurrentQqLyrics(bool forceOnline = false, bool forceLocal = false,
                               bool persistOrder = false);
    void applyFontAppearance();
    void applyFontColors();
    COLORREF effectivePlayedColor() const;
    COLORREF effectiveFloatingCardBackgroundColor() const;
    void applyFloatingCardBackgroundColor(COLORREF color);
    const FontColorDialog::ThemeState& currentLyricAppearance() const;
    void tryExtractAlbumColor();
    void loadSettings();
    void saveSettings();
    static LRESULT CALLBACK trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK updatePromptWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

void App::applyTickTickEnabled(bool enabled) {
    if (enabled) {
        if (tickTickApiToken_.empty()) {
            editTickTickApiToken(true);
            return;
        }
        if (tickTickEnabled_)
            return;
        tickTickEnabled_ = true;
        tickTickStatus_ = L"滴答清单已开启，正在同步今日任务…";
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateState(currentSettingsState());
        publishPresentationFrame(monitor.snapshot(), false, true);
        refreshTickTickTasks();
        return;
    }

    ++tickTickRequestGeneration_;
    tickTickEnabled_ = false;
    tickTickEnableAfterTokenSave_ = false;
    tickTickConnected_ = false;
    tickTickConnecting_ = false;
    tickTickTasksLoading_ = false;
    tickTickCompletingTaskId_.clear();
    todayTasks_.clear();
    startupTaskSummaryPending_ = false;
    startupTaskSummaryActive_ = false;
    tickTickStatus_ = L"滴答清单已关闭";
    saveSettings();
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    publishPresentationFrame(monitor.snapshot(), false, true);
}

void App::editTickTickApiToken(bool enableAfterSave) {
    if (tickTickApiTokenDialog && tickTickApiTokenDialog->isOpen()) {
        if (enableAfterSave)
            tickTickEnableAfterTokenSave_ = true;
        SetForegroundWindow(tickTickApiTokenDialog->hwnd());
        return;
    }
    tickTickApiTokenDialog.reset();
    tickTickEnableAfterTokenSave_ = enableAfterSave;

    auto dialog = std::make_unique<IdleAppNameDialog>();
    HWND parent = settingsDialog ? settingsDialog->hwnd() : trayHwnd;
    if (!dialog->create(GetModuleHandleW(nullptr), parent, std::wstring(),
                        L"设置滴答清单 API 口令",
                        L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建并复制；留空可清除",
                        L"输入 API 口令", 256)) {
        tickTickEnableAfterTokenSave_ = false;
        return;
    }
    dialog->setApplyCallback([this](const std::wstring& text) {
        const bool enableAfterSave = tickTickEnableAfterTokenSave_;
        tickTickEnableAfterTokenSave_ = false;
        std::wstring normalized = text;
        while (!normalized.empty() && iswspace(normalized.front()) != 0)
            normalized.erase(normalized.begin());
        while (!normalized.empty() && iswspace(normalized.back()) != 0)
            normalized.pop_back();

        ++tickTickRequestGeneration_;
        startupTaskSummaryPending_ = false;
        startupTaskSummaryActive_ = false;
        tickTickConnected_ = false;
        tickTickConnecting_ = false;
        tickTickTasksLoading_ = false;
        tickTickCompletingTaskId_.clear();
        todayTasks_.clear();
        bool shouldRefresh = false;
        if (normalized.empty()) {
            tickTickProvider_.clearApiToken(tickTickService_);
            tickTickApiToken_.clear();
            tickTickEnabled_ = false;
            tickTickStatus_ = L"请在设置中填写 API 口令";
        } else if (!tickTickProvider_.saveApiToken(tickTickService_, normalized)) {
            tickTickStatus_ = L"无法保存滴答清单 API 口令";
        } else {
            tickTickApiToken_ = std::move(normalized);
            if (enableAfterSave)
                tickTickEnabled_ = true;
            shouldRefresh = tickTickEffectiveEnabled();
            tickTickStatus_ = shouldRefresh ? L"API 口令已保存，正在同步今日任务…"
                                            : L"API 口令已保存，滴答清单已关闭";
        }
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateState(currentSettingsState());
        publishPresentationFrame(monitor.snapshot(), false, true);
        if (shouldRefresh)
            refreshTickTickTasks();
    });
    tickTickApiTokenDialog = std::move(dialog);
    tickTickApiTokenDialog->show();
}

void App::connectTickTick() {
    if (tickTickConnecting_ || tickTickTasksLoading_ ||
        !tickTickCompletingTaskId_.empty())
        return;
    if (!tickTickEffectiveEnabled())
        return;

    tickTickConnecting_ = true;
    refreshTickTickTasks();
}

void App::refreshTickTickTasks() {
    if (!tickTickEffectiveEnabled() || tickTickTasksLoading_ ||
        !tickTickCompletingTaskId_.empty())
        return;

    ++tickTickRequestGeneration_;
    const uint64_t generation = tickTickRequestGeneration_;
    tickTickTasksLoading_ = true;
    if (tickTickConnecting_)
        tickTickConnected_ = false;
    tickTickStatus_ = tickTickConnecting_ ? L"正在连接并同步今日任务…" : L"正在同步今日任务…";
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    publishPresentationFrame(monitor.snapshot(), false, true);
    tickTickProvider_.requestTodayTasksAsync(
        tickTickService_, tickTickApiToken_,
        [this, generation](TickTickTasksResult result) {
            auto* payload = new TickTickTasksPayload{generation, std::move(result)};
            if (!PostThreadMessageW(mainThread, kMsgTickTickTasksReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

void App::completeTickTickTask(const IdleTaskInfo& task) {
    if (!tickTickEffectiveEnabled() || task.id.empty() || task.projectId.empty() ||
        !tickTickCompletingTaskId_.empty())
        return;

    const auto it = std::find_if(
        todayTasks_.begin(), todayTasks_.end(),
        [&task](const IdleTaskInfo& item) { return item.id == task.id; });
    if (it == todayTasks_.end())
        return;

    ++tickTickRequestGeneration_;
    const uint64_t generation = tickTickRequestGeneration_;
    tickTickCompletingTaskId_ = task.id;
    tickTickStatus_ = L"正在完成任务…";
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    publishPresentationFrame(monitor.snapshot(), false, true);
    tickTickProvider_.completeTaskAsync(
        tickTickService_, tickTickApiToken_, task.projectId, task.id,
        [this, generation, taskId = task.id](TickTickTaskMutationResult result) {
            auto* payload = new TickTickTaskCompletePayload{
                generation, taskId, std::move(result)};
            if (!PostThreadMessageW(mainThread, kMsgTickTickTaskCompleteReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

void App::disconnectTickTick() {
    ++tickTickRequestGeneration_;
    tickTickProvider_.clearApiToken(tickTickService_);
    tickTickApiToken_.clear();
    tickTickEnabled_ = false;
    tickTickEnableAfterTokenSave_ = false;
    tickTickConnected_ = false;
    tickTickConnecting_ = false;
    tickTickTasksLoading_ = false;
    tickTickCompletingTaskId_.clear();
    todayTasks_.clear();
    tickTickStatus_ = L"已断开滴答清单连接";
    startupTaskSummaryPending_ = false;
    startupTaskSummaryActive_ = false;
    saveSettings();
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    publishPresentationFrame(monitor.snapshot(), false, true);
}

void App::onTickTickTasksReady(std::unique_ptr<TickTickTasksPayload> payload) {
    if (!payload || payload->generation != tickTickRequestGeneration_ ||
        !tickTickEffectiveEnabled())
        return;
    tickTickTasksLoading_ = false;
    tickTickConnecting_ = false;
    if (payload->result.ok) {
        tickTickConnected_ = true;
        todayTasks_ = std::move(payload->result.tasks);
        tickTickStatus_ = todayTasks_.empty()
                              ? L"已连接，今天没有待办任务"
                              : std::wstring(L"已连接 · ") +
                                    std::to_wstring(todayTasks_.size()) + L" 项今日任务";
    } else {
        tickTickConnected_ = !payload->result.authRequired && !tickTickApiToken_.empty();
        todayTasks_.clear();
        tickTickStatus_ = payload->result.error.empty() ? L"滴答清单任务同步失败"
                                                        : payload->result.error;
    }
    const SmtcSnapshot snap = monitor.snapshot();
    if (startupTaskSummaryPending_) {
        startupTaskSummaryPending_ = false;
        startupTaskSummaryActive_ = payload->result.ok && !snap.sessionAlive &&
                                    idleEntryEnabled_;
    } else if (startupTaskSummaryActive_ && !payload->result.ok) {
        // 播报期间刷新失败时不继续播报一份已经清空的任务统计，直接回到普通空闲文案。
        startupTaskSummaryActive_ = false;
    }
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    publishPresentationFrame(snap, false, true);
}

void App::onTickTickTaskCompleteReady(
    std::unique_ptr<TickTickTaskCompletePayload> payload) {
    if (!payload || payload->generation != tickTickRequestGeneration_ ||
        !tickTickEffectiveEnabled() ||
        payload->taskId != tickTickCompletingTaskId_)
        return;

    tickTickCompletingTaskId_.clear();
    if (payload->result.ok) {
        const std::wstring completedId = payload->taskId;
        std::erase_if(todayTasks_, [&completedId](const IdleTaskInfo& item) {
            return item.id == completedId;
        });
        tickTickConnected_ = true;
        tickTickStatus_ = todayTasks_.empty()
                              ? L"已连接，今天没有待办任务"
                              : std::wstring(L"已连接 · ") +
                                    std::to_wstring(todayTasks_.size()) + L" 项今日任务";
    } else {
        tickTickStatus_ = payload->result.error.empty()
                              ? L"滴答清单任务完成失败"
                              : payload->result.error;
        if (payload->result.authRequired)
            tickTickConnected_ = false;
    }
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    publishPresentationFrame(monitor.snapshot(), false, true);
}

void App::onStartupTaskSummaryCompleted() {
    if (!startupTaskSummaryActive_)
        return;
    startupTaskSummaryActive_ = false;
    const SmtcSnapshot snap = monitor.snapshot();
    publishPresentationFrame(snap, false, true);
}

void App::openTickTickTask(const IdleTaskInfo& task) {
    if (!tickTickEffectiveEnabled())
        return;
    if (task.id.empty() || task.projectId.empty()) {
        const bool opened = platform_icon::launchUri(L"https://dida365.com/webapp/");
        runtime_log::writef(L"[action][ticktick] open-task missing-id target=web result=%s",
                            opened ? L"ok" : L"failed");
        return;
    }

    // 滴答清单网页版的任务链接由清单 ID 和任务 ID 唯一定位。
    const std::wstring taskPath = task.projectId + L"/tasks/" + task.id;
    const std::wstring webUrl = L"https://dida365.com/webapp/#p/" + taskPath;
    const bool opened = platform_icon::launchUri(webUrl);
    const wchar_t* target = opened ? L"web" : L"failed";

    runtime_log::writef(L"[action][ticktick] open-task id=%s project=%s target=%s result=%s",
                        task.id.c_str(), task.projectId.c_str(),
                        target,
                        opened ? L"ok" : L"failed");
}

void App::loadSettings() {
    std::wstring dir = configDir();
    if (dir.empty())
        return;
    settingsPath_ = dir + L"\\settings.json";
    tickTickProvider_.loadApiToken(tickTickService_, tickTickApiToken_);
    tickTickEnabled_ = !tickTickApiToken_.empty();
    startupTaskSummaryPending_ = tickTickEffectiveEnabled();
    tickTickStatus_ = tickTickEffectiveEnabled()
                          ? L"API 口令已配置，正在同步今日任务…"
                          : L"请在设置中填写 API 口令";
    logDirectory_ = runtime_log::RuntimeLogger::defaultDirectory();
    bool migrateLegacyLogDirectory = false;
    provider.setManualOverrideDir(dir + L"\\manual_lyrics");
    provider.setQqLyricOrderDir(dir + L"\\manual_lyrics_settings");
    fluent::setThemeModes(taskbarThemeMode_, windowThemeMode_);
    try {
        std::ifstream f(std::filesystem::path(settingsPath_), std::ios::binary);
        if (!f)
            return;
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded())
            return;
        tickTickEnabled_ = j.value("tickTickEnabled", !tickTickApiToken_.empty());
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
        FontColorDialog::ThemeState legacyAppearance;
        legacyAppearance.played =
            (COLORREF)j.value("lyricColor", (unsigned)legacyAppearance.played);
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

        auto loadAppearance = [&j](const char* suffix,
                                   FontColorDialog::ThemeState fallback) {
            const std::string suffixValue = suffix;
            fallback.played = static_cast<COLORREF>(j.value(
                "lyricColor" + suffixValue, static_cast<unsigned>(fallback.played)));
            fallback.unplayed = static_cast<COLORREF>(j.value(
                "lyricUnplayedColor" + suffixValue,
                static_cast<unsigned>(fallback.unplayed)));
            fallback.unplayedAlphaPct = std::clamp(
                j.value("lyricUnplayedAlpha" + suffixValue, fallback.unplayedAlphaPct), 5, 100);
            fallback.glowColor = static_cast<COLORREF>(j.value(
                "lyricGlowColor" + suffixValue, static_cast<unsigned>(fallback.glowColor)));
            fallback.outlineColor = static_cast<COLORREF>(j.value(
                "lyricOutlineColor" + suffixValue,
                static_cast<unsigned>(fallback.outlineColor)));
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
        // 性能模式只对本次运行有效；忽略旧版本可能留下的持久化值，启动始终回到正常模式。
        renderMode_ = static_cast<int>(RenderMode::Normal);
        hoverPlaybackControls_ = j.value("hoverPlaybackControls", true);
        hoverControlStyle_ = j.value("hoverControlStyle", 0) == 1
                                 ? HoverControlStyle::Popup
                                 : HoverControlStyle::Inline;
        const int floatingCardTrigger =
            j.value("floatingCardTrigger", j.value("mediaPopupTrigger", 0));
        floatingCardTrigger_ = floatingCardTrigger == 1 ? MediaPopupTrigger::Click
                                                         : MediaPopupTrigger::Hover;
        const std::string floatingCardBackgroundValue = j.value(
            "floatingCardBackground",
            j.value("mediaPopupBackground", j.value("idleCardBackground", std::string("solid"))));
        floatingCardBackground_ = floatingCardBackgroundValue == "frosted"
                                      ? MediaPopupBackground::Frosted
                                      : MediaPopupBackground::Solid;
        floatingCardFollowAlbum_ = j.value(
            "floatingCardFollowAlbum",
            j.value("mediaPopupFollowAlbum", j.value("idleCardFollowAlbum", false)));
        floatingCardAutoTextContrast_ = j.value(
            "floatingCardAutoTextContrast", j.value("mediaPopupAutoTextContrast", true));
        songToastEnabled_ = j.value("songToast", false);
        songToastDurationSec_ = std::clamp(j.value("songToastDuration", 4), 1, 10);
        songToastSkipFullscreen_ = j.value("songToastSkipFullscreen", true);
        songToastTop_ = j.value("songToastPosition", std::string("bottom")) == "top";
        taskbarThemeMode_ = themeModeFromConfig(
            j.value("taskbarTheme", std::string("system")),
            fluent::ThemeMode::FollowSystem);
        windowThemeMode_ = themeModeFromConfig(
            j.value("windowTheme", std::string("app")),
            fluent::ThemeMode::FollowApp);
        fluent::setThemeModes(taskbarThemeMode_, windowThemeMode_);
        floatingCardBackgroundColorCustomized_ =
            j.contains("floatingCardBackgroundColor") ||
            j.contains("idleCardBackgroundColor") || j.contains("mediaPopupBackgroundColor");
        floatingCardBackgroundColor_ = static_cast<COLORREF>(j.value(
            "floatingCardBackgroundColor",
            j.value("idleCardBackgroundColor",
                    j.value("mediaPopupBackgroundColor",
                            static_cast<unsigned>(defaultFloatingCardBackgroundColor())))));
        spectrumOn_ = j.value("spectrum", false);
        const std::string spectrumStyleValue =
            j.value("spectrumStyle", std::string("default"));
        spectrumStyle_ = spectrumStyleFromConfig(spectrumStyleValue);
        spectrumBackground_ = j.value("spectrumBackground",
                                       spectrumStyleValue == "background-wave");
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
            preferRomanization_ = j.value("secondaryLyricType", std::string("translation")) ==
                                    "romanization";
        } else {
            // 从上一版的两个互斥开关迁移：两者都关时视为总开关关闭，类型保留翻译。
            bool oldTranslation = j.value("translationEnabled", true);
            bool oldRomanization = j.value("romanizationEnabled", false);
            secondaryLyricEnabled_ = oldTranslation || oldRomanization;
            preferRomanization_ = oldRomanization && !oldTranslation;
        }
        doubleLineLyricsEnabled_ = j.value("doubleLineLyrics", false);
        const std::string lyricAlignment = j.value("lyricAlignment", std::string("left"));
        if (lyricAlignment == "center")
            lyricAlignment_ = LyricAlignment::Center;
        else if (lyricAlignment == "right")
            lyricAlignment_ = LyricAlignment::Right;
        else
            lyricAlignment_ = LyricAlignment::Left;
        const std::string idleQuoteAlignment =
            j.value("idleQuoteAlignment", std::string("left"));
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
        idleAppNamesVisible_ = j.value("idleAppNamesVisible", true);
        idleCustomWelcome_ =
            normalizeIdleCustomWelcome(wideOf(j.value("idleCustomWelcome", std::string())));
        idleQuoteSource_ = idleQuoteSourceFromConfig(
            j.value("idleQuoteSource", std::string("hitokoto")));
        idleQuoteRefreshInterval_ = idleQuoteIntervalFromConfig(
            j.value("idleQuoteRefreshInterval", std::string("daily")));
        idleQuoteBackground_ = idleQuoteBackgroundFromConfig(
            j.value("idleQuoteBackground", std::string("none")));
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
        provider.setQqLocalLyricsConfig(qqLocalLyricsEnabled_, qqLocalLyricsPath_);
    } catch (...) {
    }
    tickTickConnected_ = false;
    tickTickConnecting_ = false;
    tickTickTasksLoading_ = false;
    if (tickTickApiToken_.empty()) {
        tickTickEnabled_ = false;
        tickTickStatus_ = L"请在设置中填写 API 口令";
    } else if (!tickTickEnabled_) {
        tickTickStatus_ = L"滴答清单已关闭";
    } else {
        tickTickStatus_ = L"API 口令已配置，正在同步今日任务…";
    }
    startupTaskSummaryPending_ = tickTickEffectiveEnabled();
    startupTaskSummaryActive_ = false;
    if (migrateLegacyLogDirectory)
        saveSettings();
}

void App::saveSettings() {
    if (settingsPath_.empty())
        return;
    try {
        nlohmann::json j;
        j["hasUserFont"] = hasUserFont_;
        j["fontFamily"] = utf8Of(fontFamily_);
        j["fontSize"] = fontSize_;
        j["fontStyle"] = fontStyleName(fontStyle_);
        auto saveAppearance = [&j](const char* suffix,
                                   const FontColorDialog::ThemeState& appearance) {
            const std::string suffixValue = suffix;
            j["lyricColor" + suffixValue] = static_cast<unsigned>(appearance.played);
            j["lyricUnplayedColor" + suffixValue] =
                static_cast<unsigned>(appearance.unplayed);
            j["lyricUnplayedAlpha" + suffixValue] = appearance.unplayedAlphaPct;
            j["lyricGlowColor" + suffixValue] = static_cast<unsigned>(appearance.glowColor);
            j["lyricOutlineColor" + suffixValue] =
                static_cast<unsigned>(appearance.outlineColor);
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
        j["idleAppNamesVisible"] = idleAppNamesVisible_;
        j["idleCustomWelcome"] = utf8Of(idleCustomWelcome_);
        j["idleQuoteSource"] = idleQuoteSourceConfigName(idleQuoteSource_);
        j["idleQuoteRefreshInterval"] =
            idleQuoteIntervalConfigName(idleQuoteRefreshInterval_);
        j["idleQuoteBackground"] = idleQuoteBackgroundConfigName(idleQuoteBackground_);
        j["idleQuoteBackgroundScope"] =
            idleQuoteBackgroundScopeConfigName(idleQuoteBackgroundScope_);
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
        j["idleQuoteCache"] = {
            {"source", idleQuoteCacheSource_},
            {"period", idleQuoteCachePeriod_},
            {"text", utf8Of(idleQuoteCacheText_)},
            {"origin", utf8Of(idleQuoteCacheOrigin_)},
            {"uuid", utf8Of(idleQuoteCacheUuid_)}};
        j["holidayCalendar"] = nlohmann::json{
            {"year", holidayCalendarYear_},
            {"updatedAt", holidayCalendarUpdatedAt_},
            {"days", nlohmann::json::array()}};
        for (const auto& day : holidayCalendar_)
            j["holidayCalendar"]["days"].push_back({
                {"date", day.date}, {"type", day.type}, {"name", utf8Of(day.name)}});
        j["taskbarPosition"] = taskbarPosition_;
        // 性能模式不写入配置，重启后由 loadSettings() 恢复正常模式。
        j["hoverPlaybackControls"] = hoverPlaybackControls_;
        j["hoverControlStyle"] = hoverControlStyle_ == HoverControlStyle::Popup ? 1 : 0;
        j["floatingCardTrigger"] = floatingCardTrigger_ == MediaPopupTrigger::Click ? 1 : 0;
        j["floatingCardBackground"] = floatingCardBackground_ == MediaPopupBackground::Frosted
                                           ? "frosted"
                                           : "solid";
        if (floatingCardBackgroundColorCustomized_)
            j["floatingCardBackgroundColor"] =
                static_cast<unsigned>(floatingCardBackgroundColor_);
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
        j["taskbarTheme"] = themeModeConfigName(taskbarThemeMode_);
        j["windowTheme"] = themeModeConfigName(windowThemeMode_);
        j["spectrum"] = spectrumOn_;
        j["spectrumStyle"] = spectrumStyleConfigName(spectrumStyle_);
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
        j["lyricAlignment"] = lyricAlignment_ == LyricAlignment::Center
                                   ? "center"
                               : lyricAlignment_ == LyricAlignment::Right ? "right" : "left";
        j["idleQuoteAlignment"] = idleQuoteAlignment_ == LyricAlignment::Center
                                       ? "center"
                                   : idleQuoteAlignment_ == LyricAlignment::Right ? "right" : "left";
        j["songInfoVisible"] = songInfoVisible_;
        j["albumCoverVisible"] = albumCoverVisible_;
        j["platformIconVisible"] = platformIconVisible_;
        j["albumCoverEffect"] = albumCoverEffect_ == AlbumCoverEffect::Vinyl ? "vinyl" : "default";
        j["qqLocalLyricsEnabled"] = qqLocalLyricsEnabled_;
        j["qqLocalLyricsPersistOrder"] = qqLocalLyricsPersistOrder_;
        j["qqLocalLyricsPath"] = utf8Of(qqLocalLyricsPath_);
        j["logDirectory"] = utf8Of(logDirectory_);
        j["logRetentionDays"] = logRetentionDays_;
        std::ofstream f(std::filesystem::path(settingsPath_), std::ios::binary | std::ios::trunc);
        f << j.dump();
    } catch (...) {
    }
}

void App::initializeRuntimeLogger() {
    if (logDirectory_.empty())
        logDirectory_ = runtime_log::RuntimeLogger::defaultDirectory();
    runtimeLogger_.start(logDirectory_, logRetentionDays_);
    logDirectory_ = runtimeLogger_.directory();
    logRetentionDays_ = runtimeLogger_.retentionDays();
    updateRuntimeLogState(monitor.snapshot());
}

void App::setLogDirectory(const std::wstring& path) {
    if (path.empty())
        return;
    runtimeLogger_.setDirectory(path);
    logDirectory_ = runtimeLogger_.directory();
    saveSettings();
}

void App::setLogRetentionDays(int days) {
    logRetentionDays_ = std::clamp(days, 0, 3650);
    runtimeLogger_.setRetentionDays(logRetentionDays_);
    saveSettings();
}

void App::showRuntimeLog() {
    runtime_log::writef(L"[action][dialog] open=runtime-log");
    if (runtimeLogDialog && !runtimeLogDialog->isOpen())
        runtimeLogDialog.reset();
    if (!runtimeLogDialog) {
        runtimeLogDialog = std::make_unique<RuntimeLogDialog>();
        if (!runtimeLogDialog->create(
                GetModuleHandleW(nullptr), trayHwnd, &runtimeLogger_,
                [this](const std::wstring& path) { setLogDirectory(path); },
                [this](int days) { setLogRetentionDays(days); })) {
            runtimeLogDialog.reset();
            return;
        }
    }
    runtimeLogDialog->show();
}

void App::pickQqLocalLyricsPath() {
    if (qqLocalLyricsPickerOpen_)
        return;

    runtime_log::writef(L"[action][qq-local-lyrics] choose-directory start current=%s",
                        qqLocalLyricsPath_.c_str());
    qqLocalLyricsPickerOpen_ = true;
    const DWORD mainThreadId = mainThread;
    const std::wstring initialPath = qqLocalLyricsPath_;

    // IFileDialog 要求 STA；主线程由 C++/WinRT 初始化为 MTA，单独在线程 STA 中打开。
    std::thread([mainThreadId, initialPath] {
        std::wstring selectedPath;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                       COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(init)) {
            IFileDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                DWORD options = 0;
                if (SUCCEEDED(dialog->GetOptions(&options)))
                    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                dialog->SetTitle(L"选择 QQ 音乐本地歌词目录");

                if (!initialPath.empty()) {
                    IShellItem* folder = nullptr;
                    if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.c_str(), nullptr,
                                                              IID_PPV_ARGS(&folder)))) {
                        dialog->SetFolder(folder);
                        folder->Release();
                    }
                }

                if (SUCCEEDED(dialog->Show(nullptr))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            selectedPath = path;
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            CoUninitialize();
        }

        runtime_log::writef(L"[action][qq-local-lyrics] choose-directory result=%s path=%s",
                            selectedPath.empty() ? L"cancelled" : L"selected",
                            selectedPath.c_str());
        auto* payload = new QqLocalFolderPayload{std::move(selectedPath)};
        if (!PostThreadMessageW(mainThreadId, kMsgQqLocalFolderReady, 0,
                                reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void App::applyQqLocalLyricsPath(const std::wstring& selectedPath) {
    if (selectedPath.empty() || selectedPath == qqLocalLyricsPath_)
        return;
    runtime_log::writef(L"[action][qq-local-lyrics] directory-changed path=%s",
                        selectedPath.c_str());
    qqLocalLyricsPath_ = selectedPath;
    saveSettings();
    if (settingsDialog)
        settingsDialog->updateState(currentSettingsState());
    if (qqLocalLyricsEnabled_) {
        provider.setQqLocalLyricsConfig(true, qqLocalLyricsPath_);
        reloadCurrentQqLyrics();
    }
}

void App::reloadCurrentQqLyrics(bool forceOnline, bool forceLocal, bool persistOrder) {
    const SmtcSnapshot snap = monitor.snapshot();
    if (!snap.sessionAlive || snap.player != SmtcPlayerType::QQMusic || currentKey.empty() ||
        snap.title.empty() || snap.timelineStale || makeTrackKey(snap) != currentKey ||
        (forceOnline && !currentLyricsFromLocal_) ||
        (forceLocal && (currentLyricsFromLocal_ || currentLyricsFromManual_)))
        return;

    runtime_log::writef(L"[action][lyrics] reload force-online=%d force-local=%d persist-order=%d",
                        forceOnline ? 1 : 0, forceLocal ? 1 : 0, persistOrder ? 1 : 0);
    cancelLyricDebounce();
    releaseCurrentLyrics();
    lyricLoading_ = true;
    startLyricRequest(snap, forceOnline, forceLocal, persistOrder);
    publishPresentationFrame(snap, false, true);
}

bool App::createTrayWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = App::trayWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricTray";
    RegisterClassExW(&wc);

    // 必须是不可见的顶层窗口（不能用 HWND_MESSAGE）：系统广播（如 Explorer 重启时的
    // TaskbarCreated）只投递给顶层窗口，message-only 窗口收不到，任务栏歌词将无法恢复
    trayHwnd = CreateWindowExW(0, L"QQMusicLyricTray", L"QQMusicLyricTray", 0, 0, 0, 0, 0,
                               nullptr, nullptr, inst, this);
    if (!trayHwnd)
        return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    updateTrayIcon();
    SetTimer(trayHwnd, kTimerIdleQuote, kIdleQuoteCheckMs, nullptr);
    return true;
}

void App::destroyTray() {
    if (trayHwnd) {
        HWND hwnd = trayHwnd;
        trayHwnd = nullptr;
        KillTimer(hwnd, kTimerIdleQuote);
        // 先关闭可能打开的 Fluent 菜单（其窗口由托盘窗口所有）
        fluent::FluentMenu::dismiss();
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
    }
}

void App::updateTrayIcon() {
    if (!trayHwnd)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = app_icon::taskbarIcon();
    lstrcpyW(nid.szTip, L"QQ 音乐歌词");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    // 首次创建时 MODIFY 不会生效，用 ADD
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void App::showUpdatePrompt(const std::wstring& latestVersion) {
    if (latestVersion.empty())
        return;
    runtime_log::writef(L"[action][update] prompt version=%s source=%s", latestVersion.c_str(),
                        useGiteeUpdateSource_ ? L"gitee" : L"github");
    if (updatePromptHwnd_) {
        SetForegroundWindow(updatePromptHwnd_);
        return;
    }

    updatePromptLatestVersion_ = latestVersion;
    updatePromptUseGiteeSource_ = useGiteeUpdateSource_;
    updatePromptHoverId_ = 0;
    updatePromptPressedId_ = 0;
    updatePromptFocusedId_ = kUpdatePromptAbout;
    updatePromptFocusVisible_ = false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = App::updatePromptWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QQMusicLyricUpdatePrompt";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(500.0f * s)),
            static_cast<LONG>(std::lround(280.0f * s))};
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU;
    constexpr DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"发现新版本", style, x, y, w, h,
                                nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) {
        updatePromptLatestVersion_.clear();
        return;
    }
    app_icon::applyWindowIcon(hwnd);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void App::layoutUpdatePrompt() {
    if (!updatePromptHwnd_)
        return;

    RECT rc{};
    GetClientRect(updatePromptHwnd_, &rc);
    const float s = updatePromptSurface_.dipScale();
    const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
    const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);
    const float pad = fluent::metrics::pagePadding;
    const float gap = fluent::metrics::controlGap;
    const float titleH = 30.0f;
    const float subtitleH = 20.0f;
    const float titleW = 112.0f;
    const float sourceX = pad + titleW + fluent::metrics::compactGap;
    const float sourceY = pad + 8.0f;
    updatePromptTitleRect_ = D2D1::RectF(pad, pad, pad + titleW, pad + titleH);
    updatePromptSourceRect_ = D2D1::RectF(sourceX, sourceY, std::max(sourceX, w - pad), pad + titleH);
    updatePromptSubtitleRect_ = D2D1::RectF(pad, pad + titleH,
                                             std::max(pad, w - pad), pad + titleH + subtitleH);

    const float cardY = pad + titleH + subtitleH + fluent::metrics::compactGap;
    const float cardH = 102.0f;
    updatePromptVersionCardRect_ = D2D1::RectF(pad, cardY, std::max(pad, w - pad), cardY + cardH);
    const float contentX = pad + 16.0f;
    const float contentW = std::max(20.0f, w - contentX - pad - 16.0f);
    updatePromptCurrentVersionRect_ = D2D1::RectF(contentX, cardY + 17.0f,
                                                  contentX + contentW, cardY + 41.0f);
    updatePromptLatestVersionRect_ = D2D1::RectF(contentX, cardY + 57.0f,
                                                 contentX + contentW, cardY + 81.0f);

    const float buttonH = fluent::metrics::controlHeight;
    const float buttonY = h - pad - buttonH;
    const float releaseW = 128.0f;
    const float downloadW = 112.0f;
    const float aboutW = 128.0f;
    const float totalW = releaseW + downloadW + aboutW + gap * 2.0f;
    const float buttonX = std::max(pad, (w - totalW) * 0.5f);
    updatePromptReleasePageRect_ = D2D1::RectF(buttonX, buttonY, buttonX + releaseW,
                                               buttonY + buttonH);
    updatePromptDownloadRect_ = D2D1::RectF(updatePromptReleasePageRect_.right + gap, buttonY,
                                            updatePromptReleasePageRect_.right + gap + downloadW,
                                            buttonY + buttonH);
    updatePromptAboutRect_ = D2D1::RectF(updatePromptDownloadRect_.right + gap, buttonY,
                                         updatePromptDownloadRect_.right + gap + aboutW,
                                         buttonY + buttonH);
}

void App::drawUpdatePromptButton(fluent::FluentDialogSurface::Painter& painter,
                                 const D2D1_RECT_F& rect, const wchar_t* text, bool accent,
                                 int id) {
    const auto& p = fluent::palette();
    const bool hovered = updatePromptHoverId_ == id;
    const bool pressed = updatePromptPressedId_ == id;
    const D2D1_COLOR_F fill = accent
                                  ? pressed ? p.accentPressed : hovered ? p.accentHover : p.accent
                                  : pressed ? p.controlPressed
                                            : hovered ? p.controlHover
                                                      : p.controlFill;
    const D2D1_COLOR_F textColor = accent ? p.textOnAccent : p.text;
    painter.fillRoundRect(fill, rect);
    if (!accent)
        painter.strokeRoundRect(p.cardStroke, rect);
    if (updatePromptFocusedId_ == id && updatePromptFocusVisible_) {
        painter.strokeRoundRect(
            accent ? p.textOnAccent : p.accent,
            D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                        rect.bottom - 1.5f),
            1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
    }
    painter.drawText(std::wstring(text), painter.textFormat(14.0f, 400, true, true),
                     D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f, rect.bottom),
                     textColor);
}

void App::paintUpdatePrompt(fluent::FluentDialogSurface::Painter& painter, float,
                            float) {
    const auto& p = fluent::palette();
    painter.drawText(L"发现新版本", painter.textFormat(21.0f, 600), updatePromptTitleRect_, p.text);
    painter.drawText(std::wstring(L"更新源：") +
                         (updatePromptUseGiteeSource_ ? L"Gitee" : L"GitHub"),
                     painter.textFormat(13.0f, 400), updatePromptSourceRect_, p.textSecondary);
    painter.drawText(L"检测到新的正式版本，可选择查看详情或立即更新",
                     painter.textFormat(13.0f, 400), updatePromptSubtitleRect_, p.textSecondary);

    painter.fillRoundRect(p.cardFill, updatePromptVersionCardRect_, fluent::metrics::cardRadius);
    painter.strokeRoundRect(p.cardStroke, updatePromptVersionCardRect_, 1.0f,
                            fluent::metrics::cardRadius);
    painter.drawText(std::wstring(L"当前版本：") + app_info::kVersion,
                     painter.textFormat(14.0f, 400), updatePromptCurrentVersionRect_, p.text);
    painter.drawText(std::wstring(L"最新版本：") + updatePromptLatestVersion_,
                     painter.textFormat(14.0f, 600), updatePromptLatestVersionRect_, p.text);

    drawUpdatePromptButton(painter, updatePromptReleasePageRect_, L"跳转到发布页", false,
                           kUpdatePromptReleasePage);
    drawUpdatePromptButton(painter, updatePromptDownloadRect_, L"下载更新", true,
                           kUpdatePromptDownload);
    drawUpdatePromptButton(painter, updatePromptAboutRect_, L"进入关于页面", false,
                           kUpdatePromptAbout);
}

int App::hitTestUpdatePrompt(float x, float y) const {
    auto contains = [x, y](const D2D1_RECT_F& rect) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    };
    if (contains(updatePromptReleasePageRect_))
        return kUpdatePromptReleasePage;
    if (contains(updatePromptDownloadRect_))
        return kUpdatePromptDownload;
    if (contains(updatePromptAboutRect_))
        return kUpdatePromptAbout;
    return 0;
}

void App::focusStepUpdatePrompt(int direction) {
    constexpr int ids[] = {kUpdatePromptReleasePage, kUpdatePromptDownload, kUpdatePromptAbout};
    int index = 0;
    for (int i = 0; i < static_cast<int>(std::size(ids)); ++i) {
        if (ids[i] == updatePromptFocusedId_) {
            index = i;
            break;
        }
    }
    index = (index + direction + static_cast<int>(std::size(ids))) % static_cast<int>(std::size(ids));
    updatePromptFocusedId_ = ids[index];
    updatePromptFocusVisible_ = true;
    updatePromptSurface_.invalidate();
}

void App::closeUpdatePrompt() {
    if (updatePromptHwnd_)
        DestroyWindow(updatePromptHwnd_);
}

void App::onUpdatePromptCommand(int command) {
    if (command != kUpdatePromptReleasePage && command != kUpdatePromptDownload &&
        command != kUpdatePromptAbout)
        return;

    const wchar_t* action = command == kUpdatePromptReleasePage
                                ? L"open-release-page"
                                : command == kUpdatePromptDownload ? L"download" : L"open-about";
    runtime_log::writef(L"[action][update-prompt] command=%s", action);
    const bool useGiteeSource = updatePromptUseGiteeSource_;
    closeUpdatePrompt();
    if (command == kUpdatePromptReleasePage) {
        const wchar_t* releasePage = useGiteeSource ? app_info::kGiteeLatestReleasePage
                                                    : app_info::kLatestReleasePage;
        ShellExecuteW(nullptr, L"open", releasePage, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (command == kUpdatePromptDownload) {
        showAbout(true);
    } else {
        showAbout();
    }
}

LRESULT CALLBACK App::updatePromptWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app)
            app->updatePromptHwnd_ = h;
    }

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
    case WM_CREATE:
        app->updatePromptBackdrop_ = fluent::styleDialogWindow(h);
        app->updatePromptSurface_.initialize(h, app->updatePromptBackdrop_);
        app->layoutUpdatePrompt();
        return 0;
    case WM_SIZE:
        app->layoutUpdatePrompt();
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_DPICHANGED: {
        auto* suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        app->layoutUpdatePrompt();
        app->updatePromptSurface_.invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        app->updatePromptBackdrop_ =
            fluent::restyleDialogWindow(h, app->updatePromptBackdrop_);
        app->updatePromptSurface_.setBackdrop(app->updatePromptBackdrop_);
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(h, &ps);
        app->updatePromptSurface_.paint(
            hdc, app->updatePromptBackdrop_,
            [app](fluent::FluentDialogSurface::Painter& painter, float width, float height) {
                app->paintUpdatePrompt(painter, width, height);
            });
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        app->updatePromptSurface_.eraseBackground(reinterpret_cast<HDC>(wp),
                                                   app->updatePromptBackdrop_);
        return 1;
    case WM_MOUSEMOVE: {
        if (!GetCapture()) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        const float s = app->updatePromptSurface_.dipScale();
        const int id = app->hitTestUpdatePrompt(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        if (id != app->updatePromptHoverId_) {
            app->updatePromptHoverId_ = id;
            app->updatePromptSurface_.invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        app->updatePromptHoverId_ = 0;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        app->updatePromptFocusVisible_ = false;
        const float s = app->updatePromptSurface_.dipScale();
        app->updatePromptPressedId_ =
            app->hitTestUpdatePrompt(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        if (app->updatePromptPressedId_ != 0) {
            app->updatePromptFocusedId_ = app->updatePromptPressedId_;
            SetCapture(h);
        }
        app->updatePromptSurface_.invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float s = app->updatePromptSurface_.dipScale();
        const int hit = app->hitTestUpdatePrompt(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        const int pressed = app->updatePromptPressedId_;
        app->updatePromptPressedId_ = 0;
        if (GetCapture() == h)
            ReleaseCapture();
        if (pressed != 0 && pressed == hit)
            app->onUpdatePromptCommand(pressed);
        if (app->updatePromptHwnd_ == h)
            app->updatePromptSurface_.invalidate();
        return 0;
    }
    case WM_CAPTURECHANGED:
        app->updatePromptPressedId_ = 0;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTTAB;
    case WM_KEYDOWN:
        if (wp == VK_TAB) {
            app->focusStepUpdatePrompt((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            app->closeUpdatePrompt();
            return 0;
        }
        if (wp == VK_SPACE || wp == VK_RETURN) {
            app->onUpdatePromptCommand(app->updatePromptFocusedId_);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        app->updatePromptFocusVisible_ = true;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_KILLFOCUS:
        app->updatePromptFocusVisible_ = false;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_CLOSE:
        app->closeUpdatePrompt();
        return 0;
    case WM_DESTROY:
        app->updatePromptSurface_.discard();
        app->updatePromptHwnd_ = nullptr;
        app->updatePromptHoverId_ = 0;
        app->updatePromptPressedId_ = 0;
        app->updatePromptLatestVersion_.clear();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void App::showTrayMenu() {
    std::vector<fluent::FluentMenuItem> items;
    auto addItem = [&items](int id, const wchar_t* text, bool checked = false,
                            bool enabled = true) {
        fluent::FluentMenuItem it;
        it.id = id;
        it.text = text;
        it.checked = checked;
        it.enabled = enabled;
        items.push_back(std::move(it));
    };
    auto addSeparator = [&items] {
        fluent::FluentMenuItem it;
        it.separator = true;
        items.push_back(std::move(it));
    };

    addItem(kCmdToggleTaskbar, taskbarHost ? L"关闭任务栏歌词" : L"开启任务栏歌词");
    if (taskbarHost) {
        fluent::FluentMenuItem pos;
        pos.text = L"任务栏位置";
        fluent::FluentMenuItem sub;
        sub.id = kCmdTaskbarPosNotify;
        sub.text = L"通知区域左侧";
        sub.checked = taskbarPosition_ == 0;
        pos.submenu.push_back(sub);
        sub.id = kCmdTaskbarPosLeft;
        sub.text = L"任务栏最左侧";
        sub.checked = taskbarPosition_ == 1;
        pos.submenu.push_back(sub);
        items.push_back(std::move(pos));
    }
    const SmtcSnapshot snap = monitor.snapshot();
    const bool canSwitchLyricSource =
        snap.sessionAlive && snap.player == SmtcPlayerType::QQMusic && !lyricLoading_ &&
        !currentLyrics_.empty() && qqLocalLyricsEnabled_ && !qqLocalLyricsPath_.empty() &&
        !currentLyricsFromManual_;
    addItem(kCmdManualSearch, L"手动搜索歌词");
    if (canSwitchLyricSource) {
        addItem(kCmdSwitchLyricSource,
                currentLyricsFromLocal_ ? L"切换到在线版歌词" : L"切换到本地版歌词");
    }
    addSeparator();
    addItem(kCmdRuntimeLog, L"运行日志");
    // 其余设置项集中到设置页
    addItem(kCmdSettings, L"设置…");
    addItem(kCmdAutoStart, L"开机自启动", autoStartEnabled());
    addSeparator();
    addItem(kCmdAbout, L"关于");
    addItem(kCmdExit, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    fluent::FluentMenu::show(trayHwnd, pt, std::move(items),
                             [this](int cmd) { onMenuCommand(cmd); });
}

void App::onMenuCommand(int cmd) {
    runtime_log::writef(L"[action][tray] command=%s id=%d", trayCommandName(cmd), cmd);
    switch (cmd) {
    case kCmdToggleTaskbar:
        toggleTaskbar();
        saveSettings();
        break;
    case kCmdTaskbarPosNotify:
    case kCmdTaskbarPosLeft:
        taskbarPosition_ = cmd == kCmdTaskbarPosLeft ? 1 : 0;
        logSettingInt(L"taskbar-position", taskbarPosition_);
        if (taskbarHost)
            taskbarHost->setPositionMode(taskbarPosition_);
        saveSettings();
        break;
    case kCmdSpectrum:
        applySpectrumOn(!spectrumOn_);
        break;
    case kCmdHoverPlaybackControls:
        applyHoverControls(!hoverPlaybackControls_);
        break;
    case kCmdPickFont:
        pickFont();
        break;
    case kCmdFontColorEffect:
        showFontColorDialog();
        break;
    case kCmdFollowAlbum:
        applyFollowAlbum(!lyricFollowAlbum_);
        break;
    case kCmdManualSearch:
        showManualSearch(GetModuleHandleW(nullptr));
        break;
    case kCmdSwitchLyricSource:
        if (currentLyricsFromLocal_)
            reloadCurrentQqLyrics(true, false, qqLocalLyricsPersistOrder_);
        else if (!currentLyricsFromManual_)
            reloadCurrentQqLyrics(false, true, qqLocalLyricsPersistOrder_);
        break;
    case kCmdRuntimeLog:
        showRuntimeLog();
        break;
    case kCmdSecondaryLyric:
        applySecondaryEnabled(!secondaryLyricEnabled_);
        break;
    case kCmdDoubleLineLyrics:
        applyDoubleLineLyrics(!doubleLineLyricsEnabled_);
        break;
    case kCmdLyricAlignLeft:
    case kCmdLyricAlignCenter:
    case kCmdLyricAlignRight:
        applyLyricAlignment(cmd == kCmdLyricAlignCenter ? 1 : cmd == kCmdLyricAlignRight ? 2 : 0);
        break;
    case kCmdSongInfo:
        applySongInfoVisible(!songInfoVisible_);
        break;
    case kCmdAlbumCover:
        applyAlbumCoverVisible(!albumCoverVisible_);
        break;
    case kCmdPlatformIcon:
        applyPlatformIconVisible(!platformIconVisible_);
        break;
    case kCmdAlbumCoverEffectDefault:
    case kCmdAlbumCoverEffectVinyl:
        applyCoverEffect(cmd == kCmdAlbumCoverEffectVinyl);
        break;
    case kCmdSwitchSecondaryLyric:
        applyPreferRomanization(!preferRomanization_);
        break;
    case kCmdSettings:
        showSettings();
        break;
    case kCmdAutoStart: {
        bool enable = !autoStartEnabled();
        if (setAutoStart(enable))
            runtime_log::writef(L"[autostart] %s", enable ? L"enabled" : L"disabled");
        else
            runtime_log::writef(L"[autostart] failed to %s", enable ? L"enable" : L"disable");
        break;
    }
    case kCmdAbout:
        showAbout();
        break;
    case kCmdExit:
        requestQuit();
        break;
    }
}

void App::pickFont() {
    runtime_log::writef(L"[action][dialog] open=font-picker current-family=%s size=%.1f style=%s",
                        fontFamily_.c_str(), static_cast<double>(fontSize_),
                        fontStyleLabel(fontStyle_));
    if (!taskbarHost)
        return;
    if (fontPickerDialog && fontPickerDialog->isOpen()) {
        SetForegroundWindow(fontPickerDialog->hwnd());
        return;
    }
    fontPickerDialog = std::make_unique<FontPickerDialog>();
    if (!fontPickerDialog->create(GetModuleHandleW(nullptr), trayHwnd, fontFamily_, fontSize_,
                                  fontStyle_)) {
        fontPickerDialog.reset();
        return;
    }
    fontPickerDialog->setApplyCallback(
        [this](const std::wstring& family, float size, LyricFontStyle style) {
            fontFamily_ = family;
            fontSize_ = std::clamp(size, 4.0f, 96.0f);
            fontStyle_ = style;
            hasUserFont_ = true;
            runtime_log::writef(L"[action][font] applied family=%s size=%.1f style=%s",
                                fontFamily_.c_str(), static_cast<double>(fontSize_),
                                fontStyleLabel(fontStyle_));
            if (taskbarHost)
                taskbarHost->setFont(fontFamily_, fontSize_, fontStyle_);
            saveSettings();
            if (settingsDialog)
                settingsDialog->updateFontDescription(currentSettingsState().fontDesc);
        });
    fontPickerDialog->show();
}

const FontColorDialog::ThemeState& App::currentLyricAppearance() const {
    if (lyricAppearanceGlobal_)
        return globalLyricAppearance_;
    return fluent::isDarkMode(fluent::ThemeTarget::Taskbar) ? darkLyricAppearance_
                                                             : lightLyricAppearance_;
}

COLORREF App::effectivePlayedColor() const {
    return (lyricFollowAlbum_ && hasAlbumColor_) ? albumColor_ : currentLyricAppearance().played;
}

COLORREF App::effectiveFloatingCardBackgroundColor() const {
    return floatingCardBackgroundColorCustomized_
               ? floatingCardBackgroundColor_
               : defaultFloatingCardBackgroundColor();
}

void App::applyFloatingCardBackgroundColor(COLORREF color) {
    floatingCardBackgroundColor_ = color;
    floatingCardBackgroundColorCustomized_ = true;
    if (taskbarHost)
        taskbarHost->setFloatingCardBackgroundColor(floatingCardBackgroundColor_, true);
    runtime_log::writef(L"[action][floating-card] background-color=#%02X%02X%02X",
                        GetRValue(color), GetGValue(color), GetBValue(color));
    saveSettings();
}

// 当前曲目还没提取过时，从有效封面提取主色调；它同时供歌词颜色和媒体卡片背景使用。
void App::tryExtractAlbumColor() {
    if (hasAlbumColor_)
        return;
    if (!lastCover_ || lastCover_->empty())
        return;
    auto color = extractDominantColor(*lastCover_);
    if (!color)
        return;
    albumColor_ = *color;
    hasAlbumColor_ = true;
    runtime_log::writef(L"[color] album dominant: #%02X%02X%02X : %s", GetRValue(albumColor_),
                        GetGValue(albumColor_), GetBValue(albumColor_), currentKey.c_str());
    currentFrame_.media.hasDominantColor = true;
    currentFrame_.media.dominantColor = albumColor_;
    if (taskbarHost)
        taskbarHost->setMediaInfo(currentFrame_.media);
    applyFontColors();
}

void App::applyFontColors() {
    const auto& appearance = currentLyricAppearance();
    for (auto* h : hosts())
        h->setFontColors(effectivePlayedColor(), appearance.unplayed,
                         appearance.unplayedAlphaPct);
}

void App::applyFontAppearance() {
    const auto& appearance = currentLyricAppearance();
    applyFontColors();
    if (taskbarHost) {
        taskbarHost->setFontGlow(appearance.glowOn);
        taskbarHost->setFontOutline(appearance.outlineOn);
        taskbarHost->setFontGlowColors(appearance.glowColor, appearance.outlineColor);
    }
}

void App::showFontColorDialog() {
    runtime_log::writef(L"[action][dialog] open=font-color");
    if (!taskbarHost)
        return;
    if (fontColorDialog && fontColorDialog->isOpen()) {
        SetForegroundWindow(fontColorDialog->hwnd());
        return;
    }
    fontColorDialog = std::make_unique<FontColorDialog>();
    FontColorDialog::State st{};
    st.global = lyricAppearanceGlobal_;
    st.hasGlobalTheme = hasGlobalLyricAppearance_;
    st.light = lightLyricAppearance_;
    st.dark = darkLyricAppearance_;
    st.globalTheme = globalLyricAppearance_;
    st.fontFamily = fontFamily_;
    st.fontStyle = fontStyle_;
    // 与任务栏实际渲染字号一致：setFont 钳制 9..18，绘制时放大 1.18 倍
    float baseSize = hasUserFont_ ? fontSize_ : 12.0f;
    st.lyricFontSize = std::clamp(baseSize, 9.0f, 18.0f) * 1.18f;
    if (!fontColorDialog->create(GetModuleHandleW(nullptr), trayHwnd, st)) {
        fontColorDialog.reset();
        return;
    }
    fontColorDialog->setApplyCallback([this](const FontColorDialog::Result& r) {
        lyricAppearanceGlobal_ = r.global;
        hasGlobalLyricAppearance_ = r.hasGlobalTheme;
        lightLyricAppearance_ = r.light;
        darkLyricAppearance_ = r.dark;
        globalLyricAppearance_ = r.globalTheme;
        runtime_log::writef(L"[action][font-color] applied global=%d has-global=%d",
                            r.global ? 1 : 0, r.hasGlobalTheme ? 1 : 0);
        applyFontAppearance();
        saveSettings();
    });
    fontColorDialog->show();
}

void App::setAutoCheckOnStartup(bool enabled) {
    if (autoCheckOnStartup_ == enabled)
        return;
    autoCheckOnStartup_ = enabled;
    runtime_log::writef(L"[action][update] auto-check-on-startup=%s",
                        enabled ? L"on" : L"off");
    saveSettings();
}

SettingsState App::currentSettingsState() const {
    SettingsState st;
    const bool vertical = taskbarVertical_;
    st.idleEntryEnabled = idleEntryEnabled_;
    st.idleQuoteEnabled = idleQuoteEnabled_;
    st.idleQuoteSource = idleQuoteSource_ == IdleQuoteSource::Jinrishici ? 1 : 0;
    st.idleQuoteRefreshInterval =
        idleQuoteRefreshInterval_ == IdleQuoteRefreshInterval::Hourly
            ? 2
            : idleQuoteRefreshInterval_ == IdleQuoteRefreshInterval::HalfDay ? 1 : 0;
    st.idleQuoteBackground = idleQuoteBackgroundIndex(idleQuoteBackground_);
    st.idleQuoteBackgroundScope = idleQuoteBackgroundScopeIndex(idleQuoteBackgroundScope_);
    st.idleAppNamesVisible = idleAppNamesVisible_;
    st.idleApps = idleApps_;
    st.tickTickEnabled = tickTickEffectiveEnabled();
    st.tickTickApiTokenConfigured = !tickTickApiToken_.empty();
    st.tickTickConnected = tickTickConnected_;
    st.tickTickConnecting = tickTickConnecting_;
    st.tickTickSyncing = tickTickTasksLoading_ || !tickTickCompletingTaskId_.empty();
    st.tickTickStatus = tickTickStatus_;
    st.verticalTaskbar = vertical;
    st.songInfoVisible = vertical ? false : songInfoVisible_;
    st.albumCoverVisible = albumCoverVisible_;
    st.platformIconVisible = platformIconVisible_;
    st.coverEffectVinyl = albumCoverEffect_ == AlbumCoverEffect::Vinyl;
    st.spectrumOn = vertical ? false : spectrumOn_;
    st.spectrumStyle = spectrumStyleIndex(vertical ? SpectrumStyle::Default : spectrumStyle_);
    st.spectrumBackground = vertical ? false : spectrumBackground_;
    st.spectrumOpacity = spectrumOpacity_;
    st.progressBackground = progressBackground_;
    st.progressBackgroundOpacity = progressBackgroundOpacity_;
    st.taskbarBackground = taskbarBackground_;
    st.coverBackgroundOpacity = coverBackgroundOpacity_;
    st.renderMode = renderMode_;
    st.hoverControls = hoverPlaybackControls_;
    st.hoverControlStyle = hoverControlStyle_ == HoverControlStyle::Popup ? 1 : 0;
    st.floatingCardTrigger = floatingCardTrigger_ == MediaPopupTrigger::Click ? 1 : 0;
    st.floatingCardBackground = floatingCardBackground_ == MediaPopupBackground::Frosted ? 1 : 0;
    st.floatingCardBackgroundColor = effectiveFloatingCardBackgroundColor();
    st.floatingCardFollowAlbum = floatingCardFollowAlbum_;
    st.floatingCardAutoTextContrast = floatingCardAutoTextContrast_;
    st.songToastEnabled = songToastEnabled_;
    st.songToastDurationSec = songToastDurationSec_;
    st.songToastSkipFullscreen = songToastSkipFullscreen_;
    st.songToastPosition = songToastTop_ ? 0 : 1;
    st.taskbarThemeMode = taskbarThemeMode_;
    st.windowThemeMode = windowThemeMode_;
    st.followAlbum = lyricFollowAlbum_;
    st.doubleLineLyrics = vertical ? false : doubleLineLyricsEnabled_;
    st.lyricAlignment = vertical
                            ? 0
                            : lyricAlignment_ == LyricAlignment::Center
                                  ? 1
                                  : lyricAlignment_ == LyricAlignment::Right ? 2 : 0;
    st.idleQuoteAlignment = vertical
                                ? 0
                                : idleQuoteAlignment_ == LyricAlignment::Center
                                      ? 1
                                      : idleQuoteAlignment_ == LyricAlignment::Right ? 2 : 0;
    st.secondaryEnabled = vertical ? false : secondaryLyricEnabled_;
    st.preferRomanization = vertical ? false : preferRomanization_;
    st.secondaryAvailability = lyricLoading_ ? 1
                               : (!currentHasTranslation_ && !currentHasRomanization_) ? 2 : 0;
    st.qqLocalLyricsEnabled = qqLocalLyricsEnabled_;
    st.qqLocalLyricsPersistOrder = qqLocalLyricsPersistOrder_;
    st.qqLocalLyricsPath = qqLocalLyricsPath_;
    st.fontDesc = fontFamily_ + L", " +
                  std::to_wstring(static_cast<int>(std::lround(fontSize_))) + L"px, " +
                  fontStyleLabel(fontStyle_);
    return st;
}

SettingsActions App::buildSettingsActions() {
    SettingsActions act;
    act.onIdleEntryEnabled = [this](bool on) { applyIdleEntryEnabled(on); };
    act.onIdleQuoteEnabled = [this](bool on) { applyIdleQuoteEnabled(on); };
    act.onIdleQuoteSource = [this](int source) { applyIdleQuoteSource(source); };
    act.onIdleQuoteRefreshInterval =
        [this](int interval) { applyIdleQuoteRefreshInterval(interval); };
    act.onIdleQuoteBackground =
        [this](int background) { applyIdleQuoteBackground(background); };
    act.onIdleQuoteBackgroundScope =
        [this](int scope) { applyIdleQuoteBackgroundScope(scope); };
    act.onEditIdleWelcome = [this] { editIdleWelcome(); };
    act.onAddIdleApp = [this] { pickIdleApp(); };
    act.onEditIdleApp = [this](int index) { editIdleApp(index); };
    act.onIdleAppNamesVisible = [this](bool show) { setIdleAppNamesVisible(show); };
    act.onRemoveIdleApp = [this](int index) { removeIdleApp(index); };
    act.onReorderIdleApps = [this](int fromIndex, int toIndex) {
        reorderIdleApps(fromIndex, toIndex);
    };
    act.onSongInfoVisible = [this](bool on) { applySongInfoVisible(on); };
    act.onAlbumCoverVisible = [this](bool on) { applyAlbumCoverVisible(on); };
    act.onPlatformIconVisible = [this](bool on) { applyPlatformIconVisible(on); };
    act.onCoverEffectVinyl = [this](bool vinyl) { applyCoverEffect(vinyl); };
    act.onSpectrum = [this](bool on) { applySpectrumOn(on); };
    act.onSpectrumStyle = [this](int style) { applySpectrumStyle(style); };
    act.onSpectrumBackground = [this](bool on) { applySpectrumBackground(on); };
    act.onSpectrumOpacity = [this](int percent) { applySpectrumOpacity(percent); };
    act.onProgressBackground = [this](bool on) { applyProgressBackground(on); };
    act.onProgressBackgroundOpacity = [this](int percent) {
        applyProgressBackgroundOpacity(percent);
    };
    act.onTaskbarBackground = [this](int mode) { applyTaskbarBackground(mode); };
    act.onCoverBackgroundOpacity = [this](int percent) {
        applyCoverBackgroundOpacity(percent);
    };
    act.onRenderMode = [this](int mode) { applyRenderMode(mode); };
    act.onHoverControls = [this](bool on) { applyHoverControls(on); };
    act.onHoverControlStyle = [this](int style) { applyHoverControlStyle(style); };
    act.onFloatingCardTrigger = [this](int mode) { applyFloatingCardTrigger(mode); };
    act.onFloatingCardBackground = [this](int mode) { applyFloatingCardBackground(mode); };
    act.onFloatingCardBackgroundColor =
        [this](COLORREF color) { applyFloatingCardBackgroundColor(color); };
    act.onFloatingCardFollowAlbum = [this](bool on) { applyFloatingCardFollowAlbum(on); };
    act.onFloatingCardAutoTextContrast =
        [this](bool on) { applyFloatingCardAutoTextContrast(on); };
    act.onSongToastEnabled = [this](bool on) { applySongToastEnabled(on); };
    act.onSongToastDuration = [this](int seconds) { applySongToastDuration(seconds); };
    act.onSongToastSkipFullscreen = [this](bool on) { applySongToastSkipFullscreen(on); };
    act.onSongToastPosition = [this](int position) { applySongToastPosition(position); };
    act.onTaskbarTheme = [this](fluent::ThemeMode mode) { applyTaskbarTheme(mode); };
    act.onWindowTheme = [this](fluent::ThemeMode mode) { applyWindowTheme(mode); };
    act.onPickFont = [this] { pickFont(); };
    act.onFontColorEffect = [this] { showFontColorDialog(); };
    act.onFollowAlbum = [this](bool on) { applyFollowAlbum(on); };
    act.onDoubleLineLyrics = [this](bool on) { applyDoubleLineLyrics(on); };
    act.onLyricAlignment = [this](int a) { applyLyricAlignment(a); };
    act.onIdleQuoteAlignment = [this](int a) { applyIdleQuoteAlignment(a); };
    act.onSecondaryEnabled = [this](bool on) { applySecondaryEnabled(on); };
    act.onPreferRomanization = [this](bool on) { applyPreferRomanization(on); };
    act.onTickTickEnabled = [this](bool on) { applyTickTickEnabled(on); };
    act.onEditTickTickApiToken = [this] { editTickTickApiToken(); };
    act.onTickTickConnect = [this] { connectTickTick(); };
    act.onTickTickRefresh = [this] { refreshTickTickTasks(); };
    act.onTickTickDisconnect = [this] { disconnectTickTick(); };
    act.onQqLocalLyricsEnabled = [this](bool on) { applyQqLocalLyricsEnabled(on); };
    act.onQqLocalLyricsPersistOrder = [this](bool on) { applyQqLocalLyricsPersistOrder(on); };
    act.onPickQqLocalLyricsPath = [this] { pickQqLocalLyricsPath(); };
    return act;
}

void App::showSettings() {
    runtime_log::writef(L"[action][dialog] open=settings");
    syncTaskbarOrientation();
    if (settingsDialog && !settingsDialog->isOpen())
        settingsDialog.reset();

    if (!settingsDialog) {
        settingsDialog = std::make_unique<SettingsDialog>();
        if (!settingsDialog->create(GetModuleHandleW(nullptr), trayHwnd,
                                    currentSettingsState(), buildSettingsActions())) {
            settingsDialog.reset();
            return;
        }
    } else if (IsWindowVisible(settingsDialog->hwnd())) {
        // 窗口已打开：仅激活到前台。再走 updateState 会触发全量 layout 重排，
        // 十几个分层子控件同时 SetWindowPos/Z-order 调整会造成可见闪烁。
        if (IsIconic(settingsDialog->hwnd()))
            ShowWindow(settingsDialog->hwnd(), SW_RESTORE);
        SetForegroundWindow(settingsDialog->hwnd());
        return;
    } else {
        // 启动时窗口已预创建但仍隐藏，打开前同步最新状态快照。
        settingsDialog->updateState(currentSettingsState());
    }
    settingsDialog->show();
}

void App::onDialogClosed(DialogKind kind) {
    runtime_log::writef(L"[action][dialog] closed kind=%d", static_cast<int>(kind));
    auto resetIfClosed = [](auto& dialog) {
        if (dialog && !dialog->isOpen())
            dialog.reset();
    };

    switch (kind) {
    case DialogKind::Settings:
        resetIfClosed(settingsDialog);
        break;
    case DialogKind::About:
        resetIfClosed(aboutDialog);
        break;
    case DialogKind::ManualSearch:
        resetIfClosed(manualSearchDialog);
        break;
    case DialogKind::FontPicker:
        resetIfClosed(fontPickerDialog);
        break;
    case DialogKind::FontColor:
        resetIfClosed(fontColorDialog);
        break;
    case DialogKind::RuntimeLog:
        resetIfClosed(runtimeLogDialog);
        break;
    }
}

void App::showAbout(bool downloadUpdate) {
    runtime_log::writef(L"[action][dialog] open=about download-update=%d",
                        downloadUpdate ? 1 : 0);
    if (aboutDialog && aboutDialog->isOpen()) {
        aboutDialog->show(downloadUpdate);
        return;
    }
    aboutDialog = std::make_unique<AboutDialog>();
    if (!aboutDialog->create(
            GetModuleHandleW(nullptr), trayHwnd, autoCheckOnStartup_, useGiteeUpdateSource_,
            [this](bool enabled) { setAutoCheckOnStartup(enabled); },
            [this](bool useGitee) {
                if (useGiteeUpdateSource_ == useGitee)
                    return;
                useGiteeUpdateSource_ = useGitee;
                runtime_log::writef(L"[action][update] source=%s",
                                    useGitee ? L"gitee" : L"github");
                saveSettings();
            },
            [this](const std::wstring& path) { return launchUpdateInstaller(path); },
            [this](const std::wstring& version) { showUpdatePrompt(version); })) {
        aboutDialog.reset();
        return;
    }
    aboutDialog->show(downloadUpdate);
}

bool App::launchUpdateInstaller(const std::wstring& path) {
    if (path.empty())
        return false;
    runtime_log::writef(L"[action][update] installer-start path=%s", path.c_str());
    // 软件内更新使用 Inno Setup 的标准参数，让安装器在安装阶段接管仍存活的旧进程；
    // 外部直接运行安装包不带该参数，仍保持手动退出策略。
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(),
                                     L"/CLOSEAPPLICATIONS", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        runtime_log::writef(L"[action][update] installer-start result=failed");
        return false;
    }
    runtime_log::writef(L"[action][update] installer-start result=ok");
    // 更新安装器已经接管后，不能再等待任务栏探测、网络请求等退出析构路径。
    // 这些同步清理在部分电脑上可能长时间阻塞，导致前台窗口和托盘已消失，
    // 但 QQMusicLyric.exe 仍存活，使 Restart Manager 无法再次关闭它。
    // ExitProcess 会立即释放本进程持有的文件句柄，交由已启动的安装器继续覆盖文件。
    ExitProcess(0);
    return true;
}

LRESULT CALLBACK App::trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_SETTINGCHANGE || msg == WM_THEMECHANGED) {
        app->refreshTheme();
        return 0;
    }
    if (msg == WM_QUERYENDSESSION)
        return TRUE;
    if (msg == WM_ENDSESSION) {
        if (wp)
            app->requestQuit();
        return 0;
    }
    if (msg == WM_CLOSE) {
        app->requestQuit();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerLyricDebounce) {
        KillTimer(h, kTimerLyricDebounce);
        app->onLyricDebounce();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerSongToastCover) {
        KillTimer(h, kTimerSongToastCover);
        app->songToastCoverWaitArmed_ = false;
        // 等待封面超时：按当前媒体信息弹出（可能无封面）
        app->notifySongToast();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerIdleQuote) {
        app->refreshHolidayCalendar(false);
        app->refreshIdleQuote(false);
        app->refreshIdleWelcome();
        return 0;
    }
    if (msg == kMsgDialogClosed) {
        app->onDialogClosed(static_cast<DialogKind>(wp));
        return 0;
    }
    if (msg == kTrayMsg && LOWORD(lp) == WM_RBUTTONUP) {
        app->showTrayMenu();
        return 0;
    }
    if (msg == WM_DESTROY) {
        app->destroyTray();
        return 0;
    }
    if (app->taskbarCreatedMsg_ && msg == app->taskbarCreatedMsg_) {
        // Explorer 重启：通知区域被清空，托盘图标必须重新添加；
        // 任务栏歌词窗口可能已随 Shell_TrayWnd 一起被销毁，交给宿主重建/重附着
        app->updateTrayIcon();
        if (app->taskbarHost)
            app->taskbarHost->onTaskbarCreated();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int main() {
    // 安装脚本用这个无界面参数确认候选文件确实来自 Release 配置；
    // 必须在单实例检查之前处理，否则已有程序运行时会掩盖构建类型。
    int verifyArgc = 0;
    LPWSTR* verifyArgv = CommandLineToArgvW(GetCommandLineW(), &verifyArgc);
    bool verifyRelease = false;
    if (verifyArgv) {
        for (int i = 1; i < verifyArgc; ++i) {
            if (wcscmp(verifyArgv[i], L"--verify-release") == 0) {
                verifyRelease = true;
                break;
            }
        }
        LocalFree(verifyArgv);
    }
    if (verifyRelease) {
#if defined(QQMUSICLYRIC_RELEASE_BUILD) && QQMUSICLYRIC_RELEASE_BUILD
        return 0;
#else
        return 1;
#endif
    }

    // 单实例限制：命名互斥体（Local\ 会话级）。进程退出（含崩溃）时内核自动销毁，
    // 无需手动释放；句柄保持到进程结束即可
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE,
        L"Local\\QQMusicLyric.SingleInstance.{7E3A9C41-2B5D-4F1E-9A6C-0D8B3E5F2A74}");
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"QQ 音乐任务栏歌词已在运行中，请勿重复启动。",
            L"QQ 音乐任务栏歌词", MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleInstance);
        return 0;
    }

    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT); // 否则 wprintf 中文输出为 '?'
    // DPI 感知由 app.manifest 声明 PerMonitorV2（运行时 API 请求 V2 会被静默降级为 V1）
    // 提高计时器粒度，否则 SetTimer 的实际触发间隔可能远大于设定值，滚动动画卡顿
    timeBeginPeriod(1);
    winrt::init_apartment();

    bool wantTaskbar = false;
    bool hasModeFlag = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--taskbar") == 0) {
                wantTaskbar = true;
                hasModeFlag = true;
            }
        }
        LocalFree(argv);
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    App app;
    app.loadSettings();
    app.prepareIdleApps();
    app.initializeRuntimeLogger();
    // 无命令行参数时始终开启任务栏歌词（沿用原"所有宿主都关闭则强制任务栏"的行为，
    // 避免用户关闭后找不到显示入口；运行中仍可通过托盘菜单随时关闭）
    if (!hasModeFlag)
        wantTaskbar = true;
    if (!app.createTrayWindow(inst)) {
        runtime_log::writef(L"failed to create tray window");
        return 1;
    }
    // 关于窗口保持隐藏，用于在程序启动后自动检查更新；用户打开关于时会再次检查。
    app.aboutDialog = std::make_unique<AboutDialog>();
    if (!app.aboutDialog->create(
            inst, app.trayHwnd, app.autoCheckOnStartup_, app.useGiteeUpdateSource_,
            [&app](bool enabled) { app.setAutoCheckOnStartup(enabled); },
            [&app](bool useGitee) {
                if (app.useGiteeUpdateSource_ == useGitee)
                    return;
                app.useGiteeUpdateSource_ = useGitee;
                app.saveSettings();
            },
            [&app](const std::wstring& path) { return app.launchUpdateInstaller(path); },
            [&app](const std::wstring& version) { app.showUpdatePrompt(version); }))
        app.aboutDialog.reset();
    // 设置窗口预创建（保持隐藏）：把首次打开时的控件构建成本挪到启动阶段；
    // 关闭后仍会销毁，下一次打开重新创建。
    app.settingsDialog = std::make_unique<SettingsDialog>();
    if (!app.settingsDialog->create(inst, app.trayHwnd, app.currentSettingsState(),
                                    app.buildSettingsActions()))
        app.settingsDialog.reset();
    if (wantTaskbar && !app.createTaskbar(inst)) {
        runtime_log::writef(L"failed to create taskbar window");
        return 1;
    }
    if (!app.taskbarHost) {
        runtime_log::writef(L"no host enabled");
        return 1;
    }

    app.monitor.start([&app] { PostThreadMessageW(app.mainThread, kMsgSmtcChanged, 0, 0); });
    app.refreshHolidayCalendar(false);
    app.refreshIdleQuote(false);
    app.refreshTickTickTasks();
    app.refreshIdleWelcome();
    app.appVolume_.setChangedCallback(
        [&app] { PostThreadMessageW(app.mainThread, kMsgAppVolumeChanged, 0, 0); });

    runtime_log::writef(L"QQMusicLyric started, waiting for QQ Music or NetEase Music...");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == nullptr) {
            if (msg.message == kMsgSmtcChanged) {
                app.onSmtcChanged();
            }
            else if (msg.message == kMsgAppVolumeChanged) {
                app.pushAppVolume();
            }
            else if (msg.message == kMsgLyricReady) {
                app.onLyricReady(std::unique_ptr<LyricPayload>(
                    reinterpret_cast<LyricPayload*>(msg.lParam)));
            }
            else if (msg.message == kMsgCoverReady) {
                app.onCoverReady(std::unique_ptr<CoverPayload>(
                    reinterpret_cast<CoverPayload*>(msg.lParam)));
            }
            else if (msg.message == kMsgQqLocalFolderReady) {
                auto payload = std::unique_ptr<QqLocalFolderPayload>(
                    reinterpret_cast<QqLocalFolderPayload*>(msg.lParam));
                app.qqLocalLyricsPickerOpen_ = false;
                if (payload)
                    app.applyQqLocalLyricsPath(payload->path);
            }
            else if (msg.message == kMsgIdleAppReady) {
                auto payload = std::unique_ptr<IdleAppPayload>(
                    reinterpret_cast<IdleAppPayload*>(msg.lParam));
                app.idleAppPickerOpen_ = false;
                if (payload && !payload->path.empty())
                    app.addIdleApp(payload->path);
            }
            else if (msg.message == kMsgIdleQuoteReady) {
                app.onIdleQuoteReady(std::unique_ptr<IdleQuotePayload>(
                    reinterpret_cast<IdleQuotePayload*>(msg.lParam)));
            }
            else if (msg.message == kMsgHolidayReady) {
                app.onHolidayReady(std::unique_ptr<HolidayPayload>(
                    reinterpret_cast<HolidayPayload*>(msg.lParam)));
            }
            else if (msg.message == kMsgTickTickTasksReady) {
                app.onTickTickTasksReady(std::unique_ptr<TickTickTasksPayload>(
                    reinterpret_cast<TickTickTasksPayload*>(msg.lParam)));
            }
            else if (msg.message == kMsgTickTickTaskCompleteReady) {
                app.onTickTickTaskCompleteReady(
                    std::unique_ptr<TickTickTaskCompletePayload>(
                        reinterpret_cast<TickTickTaskCompletePayload*>(msg.lParam)));
            }
            continue;
        }
        if (app.aboutDialog && app.aboutDialog->isOpen() &&
            IsDialogMessageW(app.aboutDialog->hwnd(), &msg))
            continue;
        if (app.updatePromptHwnd_ && IsDialogMessageW(app.updatePromptHwnd_, &msg))
            continue;
        if (app.idleAppNameDialog && app.idleAppNameDialog->isOpen() &&
            IsDialogMessageW(app.idleAppNameDialog->hwnd(), &msg))
            continue;
        if (app.tickTickApiTokenDialog && app.tickTickApiTokenDialog->isOpen() &&
            IsDialogMessageW(app.tickTickApiTokenDialog->hwnd(), &msg))
            continue;
        if (app.settingsDialog && app.settingsDialog->isOpen() &&
            IsDialogMessageW(app.settingsDialog->hwnd(), &msg))
            continue;
        // 手动搜索和选择字体使用窗口级自绘输入框，字符输入必须经过
        // TranslateMessage 生成 WM_CHAR，再由各自窗口处理；不能交给
        // IsDialogMessageW 抢先消费键盘消息。
        if (app.fontColorDialog && app.fontColorDialog->isOpen() &&
            app.fontColorDialog->isDialogMessage(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app.closeUpdatePrompt();
    app.destroyTray();
    app.runtimeLogger_.write(L"[lifecycle] message loop ended");
    app.runtimeLogger_.flushSync();
    timeEndPeriod(1);
    return 0;
}
