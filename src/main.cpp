#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "ui/lyric_window.h"
#include "ui/taskbar_host.h"
#include "ui/about_dialog.h"
#include "ui/manual_search_dialog.h"
#include "ui/font_picker_dialog.h"
#include "ui/font_color_dialog.h"
#include "ui/fluent_menu.h"
#include "media/smtc_monitor.h"
#include "media/audio_spectrum.h"
#include "util/dominant_color.h"
#include "resource.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <shellapi.h>
#include <shlobj.h>
#include <timeapi.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <io.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT kMsgSmtcChanged = WM_APP + 1;
constexpr UINT kMsgLyricReady = WM_APP + 2;
constexpr UINT kMsgCoverReady = WM_APP + 3;
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
constexpr int64_t kLyricTransitionLeadMs = 100; // 提前准备下一句显示，逐字高亮仍按真实进度

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
    std::wstring currentKey;
    std::wstring currentTitle;
    std::wstring currentArtist;
    int64_t currentDurationMs = 0;
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    SmtcPlayerType lastPlayer_ = SmtcPlayerType::Unknown;
    bool lyricLoading_ = false;
    std::vector<LyricLine> currentLyrics_;
    uint64_t requestGeneration_ = 0;
    uint64_t frameRevision_ = 0;
    PresentationFrame currentFrame_;
    std::shared_ptr<const std::vector<uint8_t>> lastSmtcThumbnail;

    HWND trayHwnd = nullptr;
    UINT taskbarCreatedMsg_ = 0; // Explorer 重启广播（只有顶层窗口收得到，托盘窗口不能用 HWND_MESSAGE）

    // 字体状态（作为字体选择器的记忆源）。hasUserFont_ 为 false 时各宿主使用各自的默认字体。
    bool hasUserFont_ = false;
    std::wstring fontFamily_ = L"Microsoft YaHei UI";
    float fontSize_ = 16.0f;

    // 歌词外观（两宿主通用，新建宿主时应用）
    COLORREF lyricColor_ = RGB(49, 194, 124);        // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124); // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                  // 逐字未播放透明度（%）
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
    std::wstring settingsPath_;

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
        taskbarHost = std::move(host);
        syncHost(taskbarHost.get());
        if (hasUserFont_)
            taskbarHost->setFont(fontFamily_, fontSize_);
        taskbarHost->setFontColors(lyricColor_, lyricUnplayedColor_, lyricUnplayedAlphaPct_);
        taskbarHost->setFontGlow(lyricGlow_);
        taskbarHost->setFontOutline(lyricOutline_);
        taskbarHost->setFontGlowColors(lyricGlowColor_, lyricOutlineColor_);
        taskbarHost->setSecondaryLyricMode(secondaryLyricEnabled_ && !preferRomanization_,
                                           secondaryLyricEnabled_ && preferRomanization_);
        taskbarHost->setDoubleLineLyrics(doubleLineLyricsEnabled_);
        taskbarHost->setLyricAlignment(lyricAlignment_);
        taskbarHost->setControlsOnHover(hoverPlaybackControls_);
        taskbarHost->setSongInfoVisible(songInfoVisible_);
        taskbarHost->setAlbumCoverVisible(albumCoverVisible_);
        taskbarHost->setPlatformIconVisible(platformIconVisible_);
        taskbarHost->setAlbumCoverEffect(albumCoverEffect_);
        taskbarHost->setPositionMode(taskbarPosition_);
        taskbarHost->setSpectrumVisible(spectrumOn_);
        if (spectrumOn_)
            spectrum_.start();
        updateTrayIcon();
        return true;
    }

    void destroyTaskbar() {
        spectrum_.stop(); // 频谱只画在任务栏上，宿主销毁时捕获线程一并停
        taskbarHost.reset();
        updateTrayIcon();
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
            currentLyrics_ = provider.lines();
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

    // 状态机：无会话(隐藏) -> 播放中(滚动渲染) <-> 暂停(静止显示)
    void onSmtcChanged() {
        SmtcSnapshot snap = monitor.snapshot();
        if (!snap.sessionAlive) {
            if (spectrumSessionAlive_)
                spectrum_.requestReconnect();
            spectrumSessionAlive_ = false;
            spectrumSessionKey_.clear();
            if (!currentKey.empty() || lastStatus != PlaybackStatus::Stopped)
                std::wprintf(L"[smtc] QQ Music session closed\n");
            currentKey.clear();
            currentTitle.clear();
            currentArtist.clear();
            currentDurationMs = 0;
            lastStatus = PlaybackStatus::Stopped;
            lyricLoading_ = false;
            ++requestGeneration_;
            currentLyrics_.clear();
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
            if (spectrumOn_)
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
            updateLyricCapabilities({});
            if (lyricFollowAlbum_)
                applyFontColors(); // 新封面就绪前先回到配置色
            lyricLoading_ = true;
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
                                      postLyricResult);
            }
        } else if (snap.thumbnail && !snap.thumbnail->empty()) {
            lastCover_ = snap.thumbnail;
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
    void pickFont();
    void showFontColorDialog();
    void showAbout();
    void setAutoCheckOnStartup(bool enabled);
    void applyFontColors();
    COLORREF effectivePlayedColor() const;
    void tryExtractAlbumColor();
    void loadSettings();
    void saveSettings();
    static LRESULT CALLBACK trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

void App::loadSettings() {
    std::wstring dir = configDir();
    if (dir.empty())
        return;
    settingsPath_ = dir + L"\\settings.json";
    provider.setManualOverrideDir(dir + L"\\manual_lyrics");
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
        hoverPlaybackControls_ = j.value("hoverPlaybackControls", true);
        spectrumOn_ = j.value("spectrum", false);
        autoCheckOnStartup_ = j.value("autoCheckOnStartup", true);
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
        j["lyricColor"] = (unsigned)lyricColor_;
        j["lyricUnplayedColor"] = (unsigned)lyricUnplayedColor_;
        j["lyricUnplayedAlpha"] = lyricUnplayedAlphaPct_;
        j["lyricGlowColor"] = (unsigned)lyricGlowColor_;
        j["lyricOutlineColor"] = (unsigned)lyricOutlineColor_;
        j["lyricGlow"] = lyricGlow_;
        j["lyricOutline"] = lyricOutline_;
        j["taskbarPosition"] = taskbarPosition_;
        j["hoverPlaybackControls"] = hoverPlaybackControls_;
        j["spectrum"] = spectrumOn_;
        j["autoCheckOnStartup"] = autoCheckOnStartup_;
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
        std::ofstream f(std::filesystem::path(settingsPath_), std::ios::binary | std::ios::trunc);
        f << j.dump();
    } catch (...) {
    }
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
        // 先关闭可能打开的 Fluent 菜单（其窗口由托盘窗口所有）
        fluent::FluentMenu::dismiss();
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = trayHwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(trayHwnd);
        trayHwnd = nullptr;
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
        addItem(kCmdSongInfo, L"显示歌曲信息", songInfoVisible_);
        addItem(kCmdAlbumCover, L"显示专辑封面", albumCoverVisible_);
        addItem(kCmdPlatformIcon, L"显示平台图标", platformIconVisible_);
        fluent::FluentMenuItem coverEffect;
        coverEffect.text = L"专辑封面效果";
        fluent::FluentMenuItem effectItem;
        effectItem.id = kCmdAlbumCoverEffectDefault;
        effectItem.text = L"默认";
        effectItem.checked = albumCoverEffect_ == AlbumCoverEffect::Default;
        coverEffect.submenu.push_back(effectItem);
        effectItem.id = kCmdAlbumCoverEffectVinyl;
        effectItem.text = L"黑胶唱片";
        effectItem.checked = albumCoverEffect_ == AlbumCoverEffect::Vinyl;
        coverEffect.submenu.push_back(effectItem);
        items.push_back(std::move(coverEffect));
        addItem(kCmdSpectrum, L"频谱", spectrumOn_);
        addItem(kCmdHoverPlaybackControls, L"悬浮时显示播放控件", hoverPlaybackControls_);
    }
    addSeparator();
    addItem(kCmdPickFont, L"字体…");
    if (taskbarHost) {
        addItem(kCmdFontColorEffect, L"字体颜色与效果…");
        addItem(kCmdFollowAlbum, L"已播放颜色跟随专辑", lyricFollowAlbum_);
    }
    addItem(kCmdManualSearch, L"手动搜索歌词");
    addItem(kCmdDoubleLineLyrics, L"双行歌词", doubleLineLyricsEnabled_);
    fluent::FluentMenuItem lyricAlignment;
    lyricAlignment.text = L"歌词对齐";
    fluent::FluentMenuItem alignmentItem;
    alignmentItem.id = kCmdLyricAlignLeft;
    alignmentItem.text = L"左对齐";
    alignmentItem.checked = lyricAlignment_ == LyricAlignment::Left;
    lyricAlignment.submenu.push_back(alignmentItem);
    alignmentItem.id = kCmdLyricAlignCenter;
    alignmentItem.text = L"居中对齐";
    alignmentItem.checked = lyricAlignment_ == LyricAlignment::Center;
    lyricAlignment.submenu.push_back(alignmentItem);
    alignmentItem.id = kCmdLyricAlignRight;
    alignmentItem.text = L"右对齐";
    alignmentItem.checked = lyricAlignment_ == LyricAlignment::Right;
    lyricAlignment.submenu.push_back(alignmentItem);
    items.push_back(std::move(lyricAlignment));
    addItem(kCmdSecondaryLyric, L"开启翻译/罗马音", secondaryLyricEnabled_);
    if (lyricLoading_) {
        addItem(kCmdSwitchSecondaryLyric, L"正在检查翻译和罗马音", false, false);
    } else if (!currentHasTranslation_ && !currentHasRomanization_) {
        addItem(kCmdSwitchSecondaryLyric, L"无罗马音和翻译", false, false);
    } else if (preferRomanization_) {
        // 只要当前歌曲存在任意一种辅助歌词，切换按钮就保持可用。
        // 目标类型不存在时由歌词渲染层显示空的辅助行，保留原文歌词。
        addItem(kCmdSwitchSecondaryLyric, L"切换到翻译", false, true);
    } else {
        addItem(kCmdSwitchSecondaryLyric, L"切换到罗马音", false, true);
    }
    addSeparator();
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
        spectrumOn_ = !spectrumOn_;
        if (spectrumOn_ && taskbarHost) {
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
        saveSettings();
        break;
    case kCmdHoverPlaybackControls:
        hoverPlaybackControls_ = !hoverPlaybackControls_;
        if (taskbarHost)
            taskbarHost->setControlsOnHover(hoverPlaybackControls_);
        saveSettings();
        break;
    case kCmdPickFont:
        pickFont();
        break;
    case kCmdFontColorEffect:
        showFontColorDialog();
        break;
    case kCmdFollowAlbum:
        lyricFollowAlbum_ = !lyricFollowAlbum_;
        if (lyricFollowAlbum_) {
            tryExtractAlbumColor(); // 立即用当前封面取色
        } else {
            hasAlbumColor_ = false; // 下次开启时重新提取
            applyFontColors();      // 恢复配置色
        }
        saveSettings();
        break;
    case kCmdManualSearch:
        showManualSearch(GetModuleHandleW(nullptr));
        break;
    case kCmdSecondaryLyric:
        secondaryLyricEnabled_ = !secondaryLyricEnabled_;
        applySecondaryLyricMode();
        saveSettings();
        break;
    case kCmdDoubleLineLyrics:
        doubleLineLyricsEnabled_ = !doubleLineLyricsEnabled_;
        if (taskbarHost)
            taskbarHost->setDoubleLineLyrics(doubleLineLyricsEnabled_);
        saveSettings();
        break;
    case kCmdLyricAlignLeft:
    case kCmdLyricAlignCenter:
    case kCmdLyricAlignRight:
        lyricAlignment_ = cmd == kCmdLyricAlignCenter
                              ? LyricAlignment::Center
                              : cmd == kCmdLyricAlignRight ? LyricAlignment::Right
                                                           : LyricAlignment::Left;
        if (taskbarHost)
            taskbarHost->setLyricAlignment(lyricAlignment_);
        saveSettings();
        break;
    case kCmdSongInfo:
        songInfoVisible_ = !songInfoVisible_;
        if (taskbarHost)
            taskbarHost->setSongInfoVisible(songInfoVisible_);
        saveSettings();
        break;
    case kCmdAlbumCover:
        albumCoverVisible_ = !albumCoverVisible_;
        if (taskbarHost)
            taskbarHost->setAlbumCoverVisible(albumCoverVisible_);
        saveSettings();
        break;
    case kCmdPlatformIcon:
        platformIconVisible_ = !platformIconVisible_;
        if (taskbarHost)
            taskbarHost->setPlatformIconVisible(platformIconVisible_);
        saveSettings();
        break;
    case kCmdAlbumCoverEffectDefault:
    case kCmdAlbumCoverEffectVinyl:
        albumCoverEffect_ = cmd == kCmdAlbumCoverEffectVinyl ? AlbumCoverEffect::Vinyl
                                                               : AlbumCoverEffect::Default;
        if (taskbarHost)
            taskbarHost->setAlbumCoverEffect(albumCoverEffect_);
        saveSettings();
        break;
    case kCmdSwitchSecondaryLyric:
        preferRomanization_ = !preferRomanization_;
        applySecondaryLyricMode();
        saveSettings();
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
        PostQuitMessage(0);
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
    if (!fontPickerDialog->create(GetModuleHandleW(nullptr), trayHwnd, fontFamily_, fontSize_)) {
        fontPickerDialog.reset();
        return;
    }
    fontPickerDialog->setApplyCallback([this](const std::wstring& family, float size) {
        fontFamily_ = family;
        fontSize_ = std::clamp(size, 4.0f, 96.0f);
        hasUserFont_ = true;
        if (taskbarHost)
            taskbarHost->setFont(fontFamily_, fontSize_);
        saveSettings();
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

void App::showAbout() {
    if (aboutDialog && aboutDialog->isOpen()) {
        aboutDialog->show();
        return;
    }
    aboutDialog = std::make_unique<AboutDialog>();
    if (!aboutDialog->create(
            GetModuleHandleW(nullptr), trayHwnd, autoCheckOnStartup_,
            [this](bool enabled) { setAutoCheckOnStartup(enabled); }))
        return;
    aboutDialog->show();
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
            inst, app.trayHwnd, app.autoCheckOnStartup_,
            [&app](bool enabled) { app.setAutoCheckOnStartup(enabled); }))
        app.aboutDialog.reset();
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
            continue;
        }
        if (app.aboutDialog && app.aboutDialog->isOpen() &&
            IsDialogMessageW(app.aboutDialog->hwnd(), &msg))
            continue;
        if (app.manualSearchDialog && app.manualSearchDialog->isOpen() &&
            IsDialogMessageW(app.manualSearchDialog->hwnd(), &msg))
            continue;
        if (app.fontPickerDialog && app.fontPickerDialog->isOpen() &&
            IsDialogMessageW(app.fontPickerDialog->hwnd(), &msg))
            continue;
        if (app.fontColorDialog && app.fontColorDialog->isOpen() &&
            app.fontColorDialog->isDialogMessage(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app.destroyTray();
    timeEndPeriod(1);
    return 0;
}
