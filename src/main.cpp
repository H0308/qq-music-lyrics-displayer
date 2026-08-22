#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "app_info.h"
#include "ui/font_style.h"
#include "ui/lyric_window.h"
#include "ui/taskbar_host.h"
#include "ui/about_dialog.h"
#include "ui/manual_search_dialog.h"
#include "ui/font_picker_dialog.h"
#include "ui/font_color_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_menu.h"
#include "ui/fluent_theme.h"
#include "ui/platform_icon.h"
#include "media/smtc_monitor.h"
#include "media/audio_spectrum.h"
#include "util/dominant_color.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <io.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kMsgSmtcChanged = WM_APP + 1;
constexpr UINT kMsgLyricReady = WM_APP + 2;
constexpr UINT kMsgCoverReady = WM_APP + 3;
constexpr UINT kMsgQqLocalFolderReady = WM_APP + 4;
// QQ 切歌时 SMTC 把媒体属性与时间线拆成多条事件投递，歌词请求延迟到这批事件
// 合并完成后发出，避免按不完整的标题/歌手/时长先失败一次（界面闪「暂无歌词」）。
constexpr UINT_PTR kTimerLyricDebounce = 3;
constexpr UINT kLyricDebounceMs = 300;
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
constexpr int64_t kLyricTransitionLeadMs = 100; // 提前准备下一句显示，逐字高亮仍按真实进度
constexpr int kUpdatePromptReleasePage = 1;
constexpr int kUpdatePromptDownload = 2;
constexpr int kUpdatePromptAbout = 3;

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

std::string utf8Of(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
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

std::wstring makeTrackKey(const SmtcSnapshot& snap) {
    if (snap.player == SmtcPlayerType::NetEase && !snap.neteaseSongId.empty())
        return L"netease|" + snap.neteaseSongId;
    return L"qq|" + LyricProvider::makeKey(snap.title, snap.artist, snap.durationMs);
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
    SmtcMonitor monitor;
    LyricProvider provider;
    CoverProvider coverProvider;
    std::unique_ptr<TaskbarHost> taskbarHost; // 具体类型：歌词描边光晕是任务栏独有接口
    std::unique_ptr<AboutDialog> aboutDialog;
    std::unique_ptr<ManualSearchDialog> manualSearchDialog;
    std::unique_ptr<FontPickerDialog> fontPickerDialog;
    std::unique_ptr<FontColorDialog> fontColorDialog;
    std::unique_ptr<SettingsDialog> settingsDialog;
    std::wstring currentKey;
    std::wstring currentTitle;
    std::wstring currentArtist;
    int64_t currentDurationMs = 0;
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    SmtcPlayerType lastPlayer_ = SmtcPlayerType::Unknown;
    bool lyricLoading_ = false;
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

    // 歌词外观（两宿主通用，新建宿主时应用）
    COLORREF lyricColor_ = RGB(49, 194, 124);        // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124); // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                  // 逐字未播放不透明度（%）
    bool lyricGlow_ = false;                          // 光晕开关（任务栏独有）
    bool lyricOutline_ = false;                       // 描边开关（任务栏独有）
    COLORREF lyricGlowColor_ = RGB(49, 194, 124);     // 光晕颜色
    COLORREF lyricOutlineColor_ = RGB(0, 0, 0);       // 描边颜色

    // 已播放颜色跟随专辑封面主色调：开启时覆盖 lyricColor_，关闭后恢复配置色
    bool lyricFollowAlbum_ = false;
    // 总开关与类型选择独立于当前歌曲能力；缺少所选内容时暂不显示，后续自动恢复。
    bool secondaryLyricEnabled_ = true;
    bool preferRomanization_ = false;
    bool doubleLineLyricsEnabled_ = false;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    bool currentHasTranslation_ = false;
    bool currentHasRomanization_ = false;
    bool hasAlbumColor_ = false; // 当前曲目是否已提取到主色调（切歌后失效）
    COLORREF albumColor_ = RGB(49, 194, 124);
    std::shared_ptr<const std::vector<uint8_t>> lastCover_; // 当前曲目有效封面（SMTC 优先，API 兜底）

    // 任务栏歌词锚定位置：0 = 通知区域左侧，1 = 任务栏最左侧
    int taskbarPosition_ = 0;
    bool hoverPlaybackControls_ = true;
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    MediaPopupBackground mediaPopupBackground_ = MediaPopupBackground::Solid;
    // 渲染模式：0 正常；1 低渲染（~30fps，降低 GPU/CPU 占用）；2 完全停止（仅驻留内存）
    int renderMode_ = 0;

    // 频谱（任务栏歌词独有）：开关持久化，开启时捕获线程跟随任务栏宿主启停
    AudioSpectrum spectrum_;
    bool spectrumOn_ = false;
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
    bool qqLocalLyricsEnabled_ = false;
    bool qqLocalLyricsPersistOrder_ = false;
    std::wstring qqLocalLyricsPath_;
    bool qqLocalLyricsPickerOpen_ = false;

    std::vector<ILyricHost*> hosts() {
        std::vector<ILyricHost*> v;
        if (taskbarHost) v.push_back(taskbarHost.get());
        return v;
    }

    const wchar_t* notRunningStatus() const {
        return lastPlayer_ == SmtcPlayerType::NetEase ? L"网易云音乐未运行" : L"QQ 音乐未运行";
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
        const bool showTranslation = secondaryLyricEnabled_ && !preferRomanization_;
        const bool showRomanization = secondaryLyricEnabled_ && preferRomanization_;
        for (auto* h : hosts())
            h->setSecondaryLyricMode(showTranslation, showRomanization);
    }

    // 设置项统一直接生效入口：右键菜单与设置页共用同一套应用逻辑
    void applySongInfoVisible(bool on) {
        songInfoVisible_ = on;
        if (taskbarHost)
            taskbarHost->setSongInfoVisible(on);
        saveSettings();
    }

    void applyAlbumCoverVisible(bool on) {
        albumCoverVisible_ = on;
        if (taskbarHost)
            taskbarHost->setAlbumCoverVisible(on);
        saveSettings();
    }

    void applyPlatformIconVisible(bool on) {
        platformIconVisible_ = on;
        if (taskbarHost)
            taskbarHost->setPlatformIconVisible(on);
        saveSettings();
    }

    void applyCoverEffect(bool vinyl) {
        albumCoverEffect_ = vinyl ? AlbumCoverEffect::Vinyl : AlbumCoverEffect::Default;
        if (taskbarHost)
            taskbarHost->setAlbumCoverEffect(albumCoverEffect_);
        saveSettings();
    }

    void applySpectrumOn(bool on) {
        spectrumOn_ = on;
        syncSpectrumWithMode();
        saveSettings();
    }

    // 频谱实际启停 = 用户开关 && 正常渲染模式 && 宿主存在；低渲染/完全停止模式下
    // 强制暂停捕获线程与绘制（频谱是持续动画源，开着就等于没省），切回正常模式自动恢复
    void syncSpectrumWithMode() {
        const bool active = spectrumOn_ && renderMode_ == 0 && taskbarHost != nullptr;
        if (active) {
            SmtcSnapshot snap = monitor.snapshot();
            const wchar_t* processName = spectrumProcessName(snap.player);
            if (*processName)
                spectrum_.setTargetProcessName(std::wstring(processName));
            spectrum_.start();
            taskbarHost->setSpectrumVisible(true);
        } else {
            spectrum_.stop();
            if (taskbarHost)
                taskbarHost->setSpectrumVisible(false);
        }
    }

    void applyRenderMode(int mode) {
        renderMode_ = std::clamp(mode, 0, 2);
        if (taskbarHost)
            taskbarHost->setRenderMode(renderMode_);
        syncSpectrumWithMode();
        saveSettings();
    }

    void applyHoverControls(bool on) {
        hoverPlaybackControls_ = on;
        if (taskbarHost)
            taskbarHost->setControlsOnHover(on);
        saveSettings();
    }

    void applyHoverControlStyle(int style) {
        hoverControlStyle_ = style == 1 ? HoverControlStyle::Popup : HoverControlStyle::Inline;
        if (taskbarHost)
            taskbarHost->setHoverControlStyle(hoverControlStyle_);
        saveSettings();
    }

    void applyMediaPopupBackground(int mode) {
        mediaPopupBackground_ = mode == 1 ? MediaPopupBackground::Frosted
                                          : MediaPopupBackground::Solid;
        if (taskbarHost)
            taskbarHost->setMediaPopupBackground(mediaPopupBackground_);
        saveSettings();
    }

    void applyFollowAlbum(bool on) {
        lyricFollowAlbum_ = on;
        if (on) {
            tryExtractAlbumColor(); // 立即用当前封面取色
        } else {
            hasAlbumColor_ = false; // 下次开启时重新提取
            applyFontColors();      // 恢复配置色
        }
        saveSettings();
    }

    void applyDoubleLineLyrics(bool on) {
        doubleLineLyricsEnabled_ = on;
        if (taskbarHost)
            taskbarHost->setDoubleLineLyrics(on);
        saveSettings();
    }

    void applyLyricAlignment(int alignment) {
        lyricAlignment_ = alignment == 1 ? LyricAlignment::Center
                          : alignment == 2 ? LyricAlignment::Right
                                           : LyricAlignment::Left;
        if (taskbarHost)
            taskbarHost->setLyricAlignment(lyricAlignment_);
        saveSettings();
    }

    void applySecondaryEnabled(bool on) {
        secondaryLyricEnabled_ = on;
        applySecondaryLyricMode();
        saveSettings();
    }

    void applyPreferRomanization(bool on) {
        preferRomanization_ = on;
        applySecondaryLyricMode();
        saveSettings();
    }

    void applyQqLocalLyricsEnabled(bool on) {
        qqLocalLyricsEnabled_ = on;
        provider.setQqLocalLyricsConfig(qqLocalLyricsEnabled_, qqLocalLyricsPath_);
        saveSettings();
        reloadCurrentQqLyrics();
    }

    void applyQqLocalLyricsPersistOrder(bool on) {
        qqLocalLyricsPersistOrder_ = on;
        saveSettings();
    }

    OverlayMediaInfo makeMediaInfo(const SmtcSnapshot& snap) const {
        OverlayMediaInfo mi;
        const bool retainPrevious = snap.title.empty() && snap.artist.empty() &&
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
            return DisplayScene::NoPlayback;
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
        frame.lyrics = currentLyrics_;
        frame.actualPositionMs = snap.positionMs;
        frame.lineSelectionPositionMs = snap.positionMs + kLyricTransitionLeadMs;
        frame.currentLine = LyricProvider::findLine(frame.lyrics, frame.lineSelectionPositionMs);
        frame.visible = snap.sessionAlive;
        frame.animateTransition = animateTransition;
        if (!snap.sessionAlive)
            frame.statusText = notRunningStatus();
        else if (lyricLoading_)
            frame.statusText = L"歌词加载中…";
        else if (currentLyrics_.empty())
            frame.statusText = currentKey.empty() ? L"等待播放…" : L"暂无歌词";
        return frame;
    }

    void publishPresentationFrame(const SmtcSnapshot& snap, bool animateTransition,
                                  bool lyricsChanged = false) {
        // SMTC 状态/控制/封面事件只刷新帧字段；完整歌词仅在内容事务变化时复制。
        if (frameRevision_ == 0 || lyricsChanged)
            currentFrame_.lyrics = currentLyrics_;
        currentFrame_.requestGeneration = requestGeneration_;
        currentFrame_.trackKey = currentKey;
        currentFrame_.scene = displaySceneFor(snap);
        currentFrame_.media = makeMediaInfo(snap);
        currentFrame_.actualPositionMs = snap.positionMs;
        currentFrame_.lineSelectionPositionMs = snap.positionMs + kLyricTransitionLeadMs;
        currentFrame_.currentLine =
            LyricProvider::findLine(currentFrame_.lyrics, currentFrame_.lineSelectionPositionMs);
        currentFrame_.visible = snap.sessionAlive;
        currentFrame_.animateTransition = animateTransition;
        if (!snap.sessionAlive)
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
    }

    bool createTaskbar(HINSTANCE inst) {
        if (taskbarHost) return true;
        auto host = std::make_unique<TaskbarHost>();
        if (!host->create(inst)) {
            std::wprintf(L"failed to create taskbar host\n");
            return false;
        }
        host->setTickCallback([this] { onFrame(); });
        host->setControlCallback([this](MediaControl c) { onControl(c); });
        host->setSourceOpenCallback([](const std::wstring& source) {
            if (!platform_icon::launchSourceApp(source))
                std::wprintf(L"[player] failed to activate source: %s\n", source.c_str());
        });
        taskbarHost = std::move(host);
        syncHost(taskbarHost.get());
        if (hasUserFont_)
            taskbarHost->setFont(fontFamily_, fontSize_, fontStyle_);
        taskbarHost->setFontColors(lyricColor_, lyricUnplayedColor_, lyricUnplayedAlphaPct_);
        taskbarHost->setFontGlow(lyricGlow_);
        taskbarHost->setFontOutline(lyricOutline_);
        taskbarHost->setFontGlowColors(lyricGlowColor_, lyricOutlineColor_);
        taskbarHost->setSecondaryLyricMode(secondaryLyricEnabled_ && !preferRomanization_,
                                           secondaryLyricEnabled_ && preferRomanization_);
        taskbarHost->setDoubleLineLyrics(doubleLineLyricsEnabled_);
        taskbarHost->setLyricAlignment(lyricAlignment_);
        taskbarHost->setControlsOnHover(hoverPlaybackControls_);
        taskbarHost->setHoverControlStyle(hoverControlStyle_);
        taskbarHost->setMediaPopupBackground(mediaPopupBackground_);
        taskbarHost->setSongInfoVisible(songInfoVisible_);
        taskbarHost->setAlbumCoverVisible(albumCoverVisible_);
        taskbarHost->setPlatformIconVisible(platformIconVisible_);
        taskbarHost->setAlbumCoverEffect(albumCoverEffect_);
        taskbarHost->setPositionMode(taskbarPosition_);
        taskbarHost->setRenderMode(renderMode_);
        taskbarHost->setSpectrumVisible(spectrumOn_ && renderMode_ == 0);
        if (spectrumOn_ && renderMode_ == 0)
            spectrum_.start();
        updateTrayIcon();
        return true;
    }

    void destroyTaskbar() {
        spectrum_.stop(); // 频谱只画在任务栏上，宿主销毁时捕获线程一并停
        taskbarHost.reset();
        updateTrayIcon();
    }

    void requestQuit() {
        if (shutdownRequested_)
            return;
        shutdownRequested_ = true;
        PostQuitMessage(0);
    }

    void toggleTaskbar() {
        if (taskbarHost) {
            destroyTaskbar();
        } else {
            createTaskbar(GetModuleHandleW(nullptr));
        }
    }

    void showManualSearch(HINSTANCE inst) {
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
            currentLyrics_ = provider.lines();
            currentLyricsFromLocal_ = false;
            currentLyricsFromManual_ = true;
            lyricLoading_ = false;
            updateLyricCapabilities(currentLyrics_);
            publishPresentationFrame(snap, true, true);
            std::wprintf(L"[lyric] manual override applied: %s\n", currentKey.c_str());
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
        switch (c) {
        case MediaControl::Prev: monitor.skipPrevious(); break;
        case MediaControl::PlayPause: monitor.playPause(); break;
        case MediaControl::Next: monitor.skipNext(); break;
        }
    }

    // 发起一次歌词请求：网易云逻辑保持不变（事件完整、可直接请求）；
    // QQ 由防抖定时器在切歌事件合并完成后调用。
    void startLyricRequest(const SmtcSnapshot& snap, bool forceOnline = false,
                           bool forceLocal = false, bool persistOrder = false) {
        lyricLoading_ = true;
        currentLyricsFromLocal_ = false;
        currentLyricsFromManual_ = false;
        lyricRequestStale_ = snap.timelineStale;
        ++requestGeneration_;
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
                                         snap.durationMs, postLyricResult);
        } else {
            provider.requestAsync(snap.title, snap.artist, snap.durationMs,
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
        if (!snap.sessionAlive) {
            if (spectrumSessionAlive_)
                spectrum_.requestReconnect();
            spectrumSessionAlive_ = false;
            spectrumSessionKey_.clear();
            if (!currentKey.empty() || lastStatus != PlaybackStatus::Stopped)
                std::wprintf(L"[smtc] %s session closed\n", playerName(lastPlayer_));
            currentKey.clear();
            currentTitle.clear();
            currentArtist.clear();
            currentDurationMs = 0;
            lastStatus = PlaybackStatus::Stopped;
            lyricLoading_ = false;
            cancelLyricDebounce();
            ++requestGeneration_;
            currentLyrics_.clear();
            currentLyricsFromLocal_ = false;
            currentLyricsFromManual_ = false;
            updateLyricCapabilities({});
            lastCover_.reset();
            lastSmtcThumbnail.reset();
            hasAlbumColor_ = false;
            publishPresentationFrame(snap, false, true);
            return;
        }
        lastPlayer_ = snap.player;
        const wchar_t* spectrumProcess = spectrumProcessName(snap.player);
        if (*spectrumProcess) {
            spectrum_.setTargetProcessName(std::wstring(spectrumProcess));
            if (spectrumOn_ && renderMode_ == 0)
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

        lastSmtcThumbnail = snap.thumbnail;
        const std::wstring key = makeTrackKey(snap);
        const bool trackChanged = key != currentKey && !snap.title.empty();
        if (snap.status != lastStatus) {
            std::wprintf(L"[smtc] %s status: %s\n", playerName(snap.player),
                         statusName(snap.status));
            lastStatus = snap.status;
        }
        if (trackChanged) {
            currentKey = key;
            currentTitle = snap.title;
            currentArtist = snap.artist;
            currentDurationMs = snap.durationMs;
            if (snap.player == SmtcPlayerType::NetEase) {
                std::wprintf(L"[smtc] %s track: %s - %s [%s] (%lld ms)\n",
                             playerName(snap.player), snap.title.c_str(), snap.artist.c_str(),
                             snap.neteaseSongId.c_str(), snap.durationMs);
            } else {
                std::wprintf(L"[smtc] %s track: %s - %s (%lld ms)\n",
                             playerName(snap.player), snap.title.c_str(), snap.artist.c_str(),
                             snap.durationMs);
            }
            lastCover_.reset();
            if (snap.thumbnail && !snap.thumbnail->empty())
                lastCover_ = snap.thumbnail;
            hasAlbumColor_ = false;
            currentLyrics_.clear();
            currentLyricsFromLocal_ = false;
            currentLyricsFromManual_ = false;
            updateLyricCapabilities({});
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
        } else if (snap.thumbnail && !snap.thumbnail->empty()) {
            lastCover_ = snap.thumbnail;
        }
        // 时间线从残留恢复可信（stale 1→0）：若当前歌词请求是在不可信期间发出的
        // （等待超时被迫发出），用可信时长立即重发，覆盖可能已被时长过滤淘汰的结果。
        if (!trackChanged && snap.player == SmtcPlayerType::QQMusic &&
            lyricRequestStale_ && !snap.timelineStale && !lyricDebounceArmed_ &&
            lyricLoading_ && currentLyrics_.empty()) {
            startLyricRequest(snap);
        }
        publishPresentationFrame(snap, !trackChanged, trackChanged);
        tryExtractAlbumColor();
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
            std::wprintf(L"[lyric] loaded %zu lines: %s\n", currentLyrics_.size(),
                         currentKey.c_str());
            // 仅 QQ 继续使用现有封面兜底；网易云阶段一不把 QQ albummid 接口当作其数据源。
            if (snap.player == SmtcPlayerType::QQMusic &&
                (!lastSmtcThumbnail || lastSmtcThumbnail->empty())) {
                const std::wstring albummid = provider.songInfo().albummid;
                if (albummid.empty()) {
                    std::wprintf(L"[cover] no albummid from search: %s\n", currentKey.c_str());
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
            currentLyricsFromLocal_ = false;
            currentLyricsFromManual_ = false;
            if (lyricRequestStale_) {
                // 时间线不可信期间发出的请求：失败大概率是旧时长触发候选的 15 秒
                // 时长过滤，不能据此判定「暂无歌词」。保持「加载中」，等新时间线
                // 可信后由 onSmtcChanged 的重试分支用正确时长重发。
                lyricLoading_ = true;
                publishPresentationFrame(snap, false);
                return;
            }
            currentLyrics_.clear();
            updateLyricCapabilities({});
            std::wprintf(L"[lyric] not found: %s\n", currentKey.c_str());
        }
        publishPresentationFrame(snap, true, true);
    }

    void onCoverReady(std::unique_ptr<CoverPayload> payload) {
        if (!payload || !payload->cover || payload->key != currentKey ||
            payload->requestGeneration != requestGeneration_)
            return; // 已切歌或重新加载，丢弃过期封面
        SmtcSnapshot snap = monitor.snapshot();
        if (!snapshotMatchesTrackKey(snap, currentKey))
            return;
        if (lastSmtcThumbnail && !lastSmtcThumbnail->empty()) return; // SMTC 已提供有效封面，优先使用
        std::wprintf(L"[cover] loaded from API: %s\n", currentKey.c_str());
        lastCover_ = payload->cover;
        publishPresentationFrame(snap, true);
        tryExtractAlbumColor();
    }

    // 30fps：插值进度 -> 二分定位当前行
    void onFrame() {
        SmtcSnapshot snap = monitor.snapshot();
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
        if (spectrumOn_ && taskbarHost) {
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
    void reloadCurrentQqLyrics(bool forceOnline = false, bool forceLocal = false,
                               bool persistOrder = false);
    void applyFontColors();
    COLORREF effectivePlayedColor() const;
    void tryExtractAlbumColor();
    void loadSettings();
    void saveSettings();
    static LRESULT CALLBACK trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK updatePromptWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

void App::loadSettings() {
    std::wstring dir = configDir();
    if (dir.empty())
        return;
    settingsPath_ = dir + L"\\settings.json";
    provider.setManualOverrideDir(dir + L"\\manual_lyrics");
    provider.setQqLyricOrderDir(dir + L"\\manual_lyrics_settings");
    try {
        std::ifstream f(std::filesystem::path(settingsPath_), std::ios::binary);
        if (!f)
            return;
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded())
            return;
        hasUserFont_ = j.value("hasUserFont", false);
        std::wstring fam = wideOf(j.value("fontFamily", std::string()));
        if (!fam.empty())
            fontFamily_ = fam;
        fontSize_ = (float)j.value("fontSize", 16.0);
        fontStyle_ = fontStyleOf(j.value("fontStyle", std::string("normal")));
        lyricColor_ = (COLORREF)j.value("lyricColor", (unsigned)RGB(49, 194, 124));
        // 未播放色默认跟随已播放色：老配置升级后视觉与之前完全一致
        lyricUnplayedColor_ = (COLORREF)j.value("lyricUnplayedColor", (unsigned)lyricColor_);
        lyricUnplayedAlphaPct_ = std::clamp(j.value("lyricUnplayedAlpha", 45), 5, 100);
        // 光晕色默认跟随已播放色、描边色默认纯黑：老配置升级后视觉与之前完全一致
        lyricGlowColor_ = (COLORREF)j.value("lyricGlowColor", (unsigned)lyricColor_);
        lyricOutlineColor_ = (COLORREF)j.value("lyricOutlineColor", (unsigned)RGB(0, 0, 0));
        lyricGlow_ = j.value("lyricGlow", false);
        // 老配置 lyricGlow 是描边+光晕总开关，升级时描边默认跟随它，视觉不变
        lyricOutline_ = j.value("lyricOutline", lyricGlow_);
        taskbarPosition_ = std::clamp(j.value("taskbarPosition", 0), 0, 1);
        renderMode_ = std::clamp(j.value("renderMode", 0), 0, 2);
        hoverPlaybackControls_ = j.value("hoverPlaybackControls", true);
        hoverControlStyle_ = j.value("hoverControlStyle", 0) == 1
                                 ? HoverControlStyle::Popup
                                 : HoverControlStyle::Inline;
        mediaPopupBackground_ = j.value("mediaPopupBackground", std::string("solid")) ==
                                        "frosted"
                                    ? MediaPopupBackground::Frosted
                                    : MediaPopupBackground::Solid;
        spectrumOn_ = j.value("spectrum", false);
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
        songInfoVisible_ = j.value("songInfoVisible", true);
        albumCoverVisible_ = j.value("albumCoverVisible", true);
        platformIconVisible_ = j.value("platformIconVisible", false);
        albumCoverEffect_ = j.value("albumCoverEffect", std::string("default")) == "vinyl"
                                ? AlbumCoverEffect::Vinyl
                                : AlbumCoverEffect::Default;
        qqLocalLyricsEnabled_ = j.value("qqLocalLyricsEnabled", false);
        qqLocalLyricsPersistOrder_ = j.value("qqLocalLyricsPersistOrder", false);
        qqLocalLyricsPath_ = wideOf(j.value("qqLocalLyricsPath", std::string()));
        provider.setQqLocalLyricsConfig(qqLocalLyricsEnabled_, qqLocalLyricsPath_);
    } catch (...) {
    }
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
        j["lyricColor"] = (unsigned)lyricColor_;
        j["lyricUnplayedColor"] = (unsigned)lyricUnplayedColor_;
        j["lyricUnplayedAlpha"] = lyricUnplayedAlphaPct_;
        j["lyricGlowColor"] = (unsigned)lyricGlowColor_;
        j["lyricOutlineColor"] = (unsigned)lyricOutlineColor_;
        j["lyricGlow"] = lyricGlow_;
        j["lyricOutline"] = lyricOutline_;
        j["taskbarPosition"] = taskbarPosition_;
        j["renderMode"] = renderMode_;
        j["hoverPlaybackControls"] = hoverPlaybackControls_;
        j["hoverControlStyle"] = hoverControlStyle_ == HoverControlStyle::Popup ? 1 : 0;
        j["mediaPopupBackground"] = mediaPopupBackground_ == MediaPopupBackground::Frosted
                                         ? "frosted"
                                         : "solid";
        j["spectrum"] = spectrumOn_;
        j["autoCheckOnStartup"] = autoCheckOnStartup_;
        j["updateSource"] = useGiteeUpdateSource_ ? "gitee" : "github";
        j["lyricFollowAlbum"] = lyricFollowAlbum_;
        j["secondaryLyricEnabled"] = secondaryLyricEnabled_;
        j["secondaryLyricType"] = preferRomanization_ ? "romanization" : "translation";
        j["doubleLineLyrics"] = doubleLineLyricsEnabled_;
        j["lyricAlignment"] = lyricAlignment_ == LyricAlignment::Center
                                   ? "center"
                               : lyricAlignment_ == LyricAlignment::Right ? "right" : "left";
        j["songInfoVisible"] = songInfoVisible_;
        j["albumCoverVisible"] = albumCoverVisible_;
        j["platformIconVisible"] = platformIconVisible_;
        j["albumCoverEffect"] = albumCoverEffect_ == AlbumCoverEffect::Vinyl ? "vinyl" : "default";
        j["qqLocalLyricsEnabled"] = qqLocalLyricsEnabled_;
        j["qqLocalLyricsPersistOrder"] = qqLocalLyricsPersistOrder_;
        j["qqLocalLyricsPath"] = utf8Of(qqLocalLyricsPath_);
        std::ofstream f(std::filesystem::path(settingsPath_), std::ios::binary | std::ios::trunc);
        f << j.dump();
    } catch (...) {
    }
}

void App::pickQqLocalLyricsPath() {
    if (qqLocalLyricsPickerOpen_)
        return;

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

        auto* payload = new QqLocalFolderPayload{std::move(selectedPath)};
        if (!PostThreadMessageW(mainThreadId, kMsgQqLocalFolderReady, 0,
                                reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void App::applyQqLocalLyricsPath(const std::wstring& selectedPath) {
    if (selectedPath.empty() || selectedPath == qqLocalLyricsPath_)
        return;
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

    cancelLyricDebounce();
    currentLyrics_.clear();
    currentLyricsFromLocal_ = false;
    currentLyricsFromManual_ = false;
    updateLyricCapabilities(currentLyrics_);
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
    return true;
}

void App::destroyTray() {
    if (trayHwnd) {
        HWND hwnd = trayHwnd;
        trayHwnd = nullptr;
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
    nid.hIcon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0,
                   LR_DEFAULTSIZE | LR_SHARED));
    lstrcpyW(nid.szTip, L"QQ 音乐歌词");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    // 首次创建时 MODIFY 不会生效，用 ADD
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void App::showUpdatePrompt(const std::wstring& latestVersion) {
    if (latestVersion.empty())
        return;
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
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
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
    switch (cmd) {
    case kCmdToggleTaskbar:
        toggleTaskbar();
        saveSettings();
        break;
    case kCmdTaskbarPosNotify:
    case kCmdTaskbarPosLeft:
        taskbarPosition_ = cmd == kCmdTaskbarPosLeft ? 1 : 0;
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
            std::wprintf(L"[autostart] %s\n", enable ? L"enabled" : L"disabled");
        else
            std::wprintf(L"[autostart] failed to %s\n", enable ? L"enable" : L"disable");
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
            if (taskbarHost)
                taskbarHost->setFont(fontFamily_, fontSize_, fontStyle_);
            saveSettings();
            if (settingsDialog)
                settingsDialog->updateFontDescription(currentSettingsState().fontDesc);
        });
    fontPickerDialog->show();
}

COLORREF App::effectivePlayedColor() const {
    return (lyricFollowAlbum_ && hasAlbumColor_) ? albumColor_ : lyricColor_;
}

// 开关开启且当前曲目还没提取过时，从有效封面提取主色调作为已播放颜色
void App::tryExtractAlbumColor() {
    if (!lyricFollowAlbum_ || hasAlbumColor_)
        return;
    if (!lastCover_ || lastCover_->empty())
        return;
    auto color = extractDominantColor(*lastCover_);
    if (!color)
        return;
    albumColor_ = *color;
    hasAlbumColor_ = true;
    std::wprintf(L"[color] album dominant: #%02X%02X%02X : %s\n", GetRValue(albumColor_),
                 GetGValue(albumColor_), GetBValue(albumColor_), currentKey.c_str());
    applyFontColors();
}

void App::applyFontColors() {
    for (auto* h : hosts())
        h->setFontColors(effectivePlayedColor(), lyricUnplayedColor_, lyricUnplayedAlphaPct_);
}

void App::showFontColorDialog() {
    if (!taskbarHost)
        return;
    if (fontColorDialog && fontColorDialog->isOpen()) {
        SetForegroundWindow(fontColorDialog->hwnd());
        return;
    }
    fontColorDialog = std::make_unique<FontColorDialog>();
    FontColorDialog::State st{};
    st.played = lyricColor_;
    st.unplayed = lyricUnplayedColor_;
    st.unplayedAlphaPct = lyricUnplayedAlphaPct_;
    st.glowColor = lyricGlowColor_;
    st.outlineColor = lyricOutlineColor_;
    st.glowOn = lyricGlow_;
    st.outlineOn = lyricOutline_;
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
        lyricColor_ = r.played;
        lyricUnplayedColor_ = r.unplayed;
        lyricUnplayedAlphaPct_ = r.unplayedAlphaPct;
        lyricGlowColor_ = r.glowColor;
        lyricOutlineColor_ = r.outlineColor;
        lyricGlow_ = r.glowOn;
        lyricOutline_ = r.outlineOn;
        applyFontColors();
        if (taskbarHost) {
            taskbarHost->setFontGlow(lyricGlow_);
            taskbarHost->setFontOutline(lyricOutline_);
            taskbarHost->setFontGlowColors(lyricGlowColor_, lyricOutlineColor_);
        }
        saveSettings();
    });
    fontColorDialog->show();
}

void App::setAutoCheckOnStartup(bool enabled) {
    if (autoCheckOnStartup_ == enabled)
        return;
    autoCheckOnStartup_ = enabled;
    saveSettings();
}

SettingsState App::currentSettingsState() const {
    SettingsState st;
    st.songInfoVisible = songInfoVisible_;
    st.albumCoverVisible = albumCoverVisible_;
    st.platformIconVisible = platformIconVisible_;
    st.coverEffectVinyl = albumCoverEffect_ == AlbumCoverEffect::Vinyl;
    st.spectrumOn = spectrumOn_;
    st.renderMode = renderMode_;
    st.hoverControls = hoverPlaybackControls_;
    st.hoverControlStyle = hoverControlStyle_ == HoverControlStyle::Popup ? 1 : 0;
    st.mediaPopupBackground = mediaPopupBackground_ == MediaPopupBackground::Frosted ? 1 : 0;
    st.followAlbum = lyricFollowAlbum_;
    st.doubleLineLyrics = doubleLineLyricsEnabled_;
    st.lyricAlignment = lyricAlignment_ == LyricAlignment::Center
                            ? 1
                            : lyricAlignment_ == LyricAlignment::Right ? 2 : 0;
    st.secondaryEnabled = secondaryLyricEnabled_;
    st.preferRomanization = preferRomanization_;
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
    act.onSongInfoVisible = [this](bool on) { applySongInfoVisible(on); };
    act.onAlbumCoverVisible = [this](bool on) { applyAlbumCoverVisible(on); };
    act.onPlatformIconVisible = [this](bool on) { applyPlatformIconVisible(on); };
    act.onCoverEffectVinyl = [this](bool vinyl) { applyCoverEffect(vinyl); };
    act.onSpectrum = [this](bool on) { applySpectrumOn(on); };
    act.onRenderMode = [this](int mode) { applyRenderMode(mode); };
    act.onHoverControls = [this](bool on) { applyHoverControls(on); };
    act.onHoverControlStyle = [this](int style) { applyHoverControlStyle(style); };
    act.onMediaPopupBackground = [this](int mode) { applyMediaPopupBackground(mode); };
    act.onPickFont = [this] { pickFont(); };
    act.onFontColorEffect = [this] { showFontColorDialog(); };
    act.onFollowAlbum = [this](bool on) { applyFollowAlbum(on); };
    act.onDoubleLineLyrics = [this](bool on) { applyDoubleLineLyrics(on); };
    act.onLyricAlignment = [this](int a) { applyLyricAlignment(a); };
    act.onSecondaryEnabled = [this](bool on) { applySecondaryEnabled(on); };
    act.onPreferRomanization = [this](bool on) { applyPreferRomanization(on); };
    act.onQqLocalLyricsEnabled = [this](bool on) { applyQqLocalLyricsEnabled(on); };
    act.onQqLocalLyricsPersistOrder = [this](bool on) { applyQqLocalLyricsPersistOrder(on); };
    act.onPickQqLocalLyricsPath = [this] { pickQqLocalLyricsPath(); };
    return act;
}

void App::showSettings() {
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
    }
}

void App::showAbout(bool downloadUpdate) {
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
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                     SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        return false;
    requestQuit();
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
    // 无命令行参数时始终开启任务栏歌词（沿用原"所有宿主都关闭则强制任务栏"的行为，
    // 避免用户关闭后找不到显示入口；运行中仍可通过托盘菜单随时关闭）
    if (!hasModeFlag)
        wantTaskbar = true;
    if (!app.createTrayWindow(inst)) {
        std::wprintf(L"failed to create tray window\n");
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
        std::wprintf(L"failed to create taskbar window\n");
        return 1;
    }
    if (!app.taskbarHost) {
        std::wprintf(L"no host enabled\n");
        return 1;
    }

    app.monitor.start([&app] { PostThreadMessageW(app.mainThread, kMsgSmtcChanged, 0, 0); });

    std::wprintf(L"QQMusicLyric started, waiting for QQ Music or NetEase Music...\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == nullptr) {
            if (msg.message == kMsgSmtcChanged) {
                app.onSmtcChanged();
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
            continue;
        }
        if (app.aboutDialog && app.aboutDialog->isOpen() &&
            IsDialogMessageW(app.aboutDialog->hwnd(), &msg))
            continue;
        if (app.updatePromptHwnd_ && IsDialogMessageW(app.updatePromptHwnd_, &msg))
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
    timeEndPeriod(1);
    return 0;
}
