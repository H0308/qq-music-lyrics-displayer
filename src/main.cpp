#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "ui/lyric_window.h"
#include "ui/taskbar_host.h"
#include "ui/manual_search_dialog.h"
#include "media/smtc_monitor.h"
#include "resource.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <timeapi.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
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
constexpr UINT kCmdToggleOverlay = 100;
constexpr UINT kCmdToggleTaskbar = 101;
constexpr UINT kCmdClickThrough = 102;
constexpr UINT kCmdFontUp = 103;
constexpr UINT kCmdFontDown = 104;
constexpr UINT kCmdPickFont = 105;
constexpr UINT kCmdExit = 106;
constexpr UINT kCmdManualSearch = 107;
constexpr UINT kCmdPlayedColor = 108;
constexpr UINT kCmdToggleGlow = 109;
constexpr UINT kCmdUnplayedColor = 110;
// 111..115：未播放透明度预设（kUnplayedAlphaPresets 下标偏移）
constexpr UINT kCmdUnplayedAlphaBase = 111;
constexpr int kUnplayedAlphaPresets[] = {25, 45, 65, 85, 100};
constexpr int kUnplayedAlphaPresetCount = 5;
constexpr UINT kCmdGlowColor = 116;
constexpr UINT kCmdOutlineColor = 117;
constexpr UINT kCmdToggleOutline = 118;

struct CoverPayload {
    std::wstring key;
    std::shared_ptr<const std::vector<uint8_t>> cover;
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

const wchar_t* statusName(PlaybackStatus s) {
    switch (s) {
    case PlaybackStatus::Playing: return L"Playing";
    case PlaybackStatus::Paused: return L"Paused";
    case PlaybackStatus::Stopped: return L"Stopped";
    default: return L"Other";
    }
}

struct App {
    DWORD mainThread = GetCurrentThreadId();
    SmtcMonitor monitor;
    LyricProvider provider;
    CoverProvider coverProvider;
    std::unique_ptr<ILyricHost> overlayHost;
    std::unique_ptr<TaskbarHost> taskbarHost; // 具体类型：歌词描边光晕是任务栏独有接口
    std::unique_ptr<ManualSearchDialog> manualSearchDialog;
    std::wstring currentKey;
    std::wstring currentTitle;
    std::wstring currentArtist;
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    bool lyricLoading_ = false;
    std::shared_ptr<const std::vector<uint8_t>> lastSmtcThumbnail;

    HWND trayHwnd = nullptr;

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

    // 窗口状态（持久化；无命令行参数时按此启动）
    bool cfgOverlay_ = false;
    bool cfgTaskbar_ = true;
    bool cfgClickThrough_ = false;

    std::wstring settingsPath_;

    std::vector<ILyricHost*> hosts() {
        std::vector<ILyricHost*> v;
        if (overlayHost) v.push_back(overlayHost.get());
        if (taskbarHost) v.push_back(taskbarHost.get());
        return v;
    }

    bool createOverlay(HINSTANCE inst) {
        if (overlayHost) return true;
        auto host = std::make_unique<OverlayHost>();
        if (!host->create(inst)) {
            std::wprintf(L"failed to create overlay host\n");
            return false;
        }
        host->setTickCallback([this] { onFrame(); });
        host->setControlCallback([this](MediaControl c) { onControl(c); });
        overlayHost = std::move(host);
        syncHost(overlayHost.get());
        if (hasUserFont_)
            overlayHost->setFont(fontFamily_, fontSize_);
        if (cfgClickThrough_)
            overlayHost->setClickThrough(true);
        overlayHost->setFontColors(lyricColor_, lyricUnplayedColor_, lyricUnplayedAlphaPct_);
        updateTrayIcon();
        return true;
    }

    void destroyOverlay() {
        overlayHost.reset();
        updateTrayIcon();
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
        updateTrayIcon();
        return true;
    }

    void destroyTaskbar() {
        taskbarHost.reset();
        updateTrayIcon();
    }

    void toggleOverlay() {
        if (overlayHost) {
            destroyOverlay();
        } else {
            createOverlay(GetModuleHandleW(nullptr));
        }
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
        manualSearchDialog = std::make_unique<ManualSearchDialog>();
        if (!manualSearchDialog->create(inst, trayHwnd, &provider, currentTitle, currentArtist)) {
            manualSearchDialog.reset();
            return;
        }
        manualSearchDialog->setApplyCallback([this] {
            auto hs = hosts();
            for (auto* h : hs) {
                h->setLyrics(provider.lines());
                h->setStatusText(L"");
            }
            std::wprintf(L"[lyric] manual override applied: %s\n", currentKey.c_str());
            // 手动选择后同样兜底封面
            if (!lastSmtcThumbnail || lastSmtcThumbnail->empty()) {
                const std::wstring albummid = provider.songInfo().albummid;
                if (!albummid.empty()) {
                    const std::wstring key = currentKey;
                    coverProvider.requestAsync(albummid,
                        [this, key](std::shared_ptr<const std::vector<uint8_t>> cover) {
                            if (!cover || cover->empty()) return;
                            auto* payload = new CoverPayload{key, std::move(cover)};
                            PostThreadMessageW(mainThread, kMsgCoverReady, 1,
                                               reinterpret_cast<LPARAM>(payload));
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
        SmtcSnapshot snap = monitor.snapshot();
        if (!snap.sessionAlive) {
            host->setLyrics({});
            host->setMediaInfo({});
            host->setStatusText(L"QQ 音乐未运行");
            host->hide();
            return;
        }
        host->show();
        OverlayMediaInfo mi;
        mi.title = snap.title;
        mi.artist = snap.artist;
        if (!snap.album.empty()) {
            mi.artist += L" · ";
            mi.artist += snap.album;
        }
        mi.thumbnail = snap.thumbnail;
        mi.canPrev = snap.canPrev;
        mi.canPlayPause = snap.canPlayPause;
        mi.canNext = snap.canNext;
        mi.playing = snap.status == PlaybackStatus::Playing;
        host->setMediaInfo(mi);
        host->setLyrics(provider.lines());
        if (provider.lines().empty()) {
            if (lyricLoading_)
                host->setStatusText(L"歌词加载中…");
            else
                host->setStatusText(currentKey.empty() ? L"等待播放…" : L"暂无歌词");
        } else {
            host->setStatusText(L"");
        }
        int idx = LyricProvider::findLine(host->lyrics(), snap.positionMs);
        host->setCurrentLine(idx);
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
        auto hs = hosts();
        SmtcSnapshot snap = monitor.snapshot();
        if (!snap.sessionAlive) {
            if (!currentKey.empty() || lastStatus != PlaybackStatus::Stopped)
                std::wprintf(L"[smtc] QQ Music session closed\n");
            currentKey.clear();
            lastStatus = PlaybackStatus::Stopped;
            lyricLoading_ = false;
            for (auto* h : hs) {
                h->setLyrics({});
                h->setMediaInfo({});
                h->setStatusText(L"QQ 音乐未运行");
                h->hide();
            }
            return;
        }
        for (auto* h : hs) h->show();
        OverlayMediaInfo mi;
        mi.title = snap.title;
        mi.artist = snap.artist;
        if (!snap.album.empty()) {
            mi.artist += L" · ";
            mi.artist += snap.album;
        }
        mi.thumbnail = snap.thumbnail;
        mi.canPrev = snap.canPrev;
        mi.canPlayPause = snap.canPlayPause;
        mi.canNext = snap.canNext;
        mi.playing = snap.status == PlaybackStatus::Playing;
        for (auto* h : hs) h->setMediaInfo(mi);
        lastSmtcThumbnail = snap.thumbnail;
        if (snap.status != lastStatus) {
            std::wprintf(L"[smtc] status: %s\n", statusName(snap.status));
            lastStatus = snap.status;
        }
        std::wstring key = LyricProvider::makeKey(snap.title, snap.artist);
        if (key != currentKey && !snap.title.empty()) {
            currentKey = key;
            currentTitle = snap.title;
            currentArtist = snap.artist;
            std::wprintf(L"[smtc] track: %s - %s (%lld ms)\n", snap.title.c_str(),
                snap.artist.c_str(), snap.durationMs);
            for (auto* h : hs) {
                h->setLyrics({});
                h->setStatusText(L"歌词加载中…");
            }
            lyricLoading_ = true;
            provider.requestAsync(snap.title, snap.artist, snap.durationMs, [this](bool ok) {
                PostThreadMessageW(mainThread, kMsgLyricReady, ok ? 1 : 0, 0);
            });
        }
    }

    void onLyricReady(bool ok) {
        lyricLoading_ = false;
        auto hs = hosts();
        if (ok) {
            for (auto* h : hs) {
                h->setLyrics(provider.lines());
                std::wprintf(L"[lyric] loaded %zu lines: %s\n", h->lyrics().size(),
                    currentKey.c_str());
            }

            // SMTC 没给封面时，用 QQ 音乐接口兜底
            if (!lastSmtcThumbnail || lastSmtcThumbnail->empty()) {
                const std::wstring albummid = provider.songInfo().albummid;
                if (albummid.empty()) {
                    std::wprintf(L"[cover] no albummid from search: %s\n", currentKey.c_str());
                } else {
                    const std::wstring key = currentKey;
                    coverProvider.requestAsync(albummid, [this, key](std::shared_ptr<const std::vector<uint8_t>> cover) {
                        if (!cover || cover->empty()) return;
                        auto* payload = new CoverPayload{key, std::move(cover)};
                        PostThreadMessageW(mainThread, kMsgCoverReady, 1,
                                           reinterpret_cast<LPARAM>(payload));
                    });
                }
            }
        }
        else {
            for (auto* h : hs) {
                h->setLyrics({});
                h->setStatusText(L"暂无歌词");
            }
            std::wprintf(L"[lyric] not found: %s\n", currentKey.c_str());
        }
    }

    void onCoverReady(std::unique_ptr<CoverPayload> payload) {
        if (!payload || !payload->cover) return;
        if (payload->key != currentKey) return; // 已切歌，丢弃过期封面
        if (lastSmtcThumbnail && !lastSmtcThumbnail->empty()) return; // SMTC 已提供有效封面，优先使用
        std::wprintf(L"[cover] loaded from API: %s\n", currentKey.c_str());
        OverlayMediaInfo mi;
        SmtcSnapshot snap = monitor.snapshot();
        mi.title = snap.title;
        mi.artist = snap.artist;
        if (!snap.album.empty()) {
            mi.artist += L" · ";
            mi.artist += snap.album;
        }
        mi.thumbnail = payload->cover;
        mi.canPrev = snap.canPrev;
        mi.canPlayPause = snap.canPlayPause;
        mi.canNext = snap.canNext;
        mi.playing = snap.status == PlaybackStatus::Playing;
        for (auto* h : hosts()) h->setMediaInfo(mi);
    }

    // 30fps：插值进度 -> 二分定位当前行
    void onFrame() {
        SmtcSnapshot snap = monitor.snapshot();
        if (!snap.sessionAlive) return;
        for (auto* h : hosts()) {
            int idx = LyricProvider::findLine(h->lyrics(), snap.positionMs);
            h->setCurrentLine(idx);
            h->setPosition(snap.positionMs);
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
    void pickFont();
    bool pickColor(COLORREF& target);
    void pickPlayedColor();
    void pickUnplayedColor();
    void pickGlowColor();
    void pickOutlineColor();
    void applyFontColors();
    void loadSettings();
    void saveSettings();
    static LRESULT CALLBACK trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

void App::loadSettings() {
    std::wstring dir = configDir();
    if (dir.empty())
        return;
    settingsPath_ = dir + L"\\settings.json";
    provider.setManualOverridePath(dir + L"\\manual_lyrics.json");
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
        cfgOverlay_ = j.value("overlay", false);
        cfgTaskbar_ = j.value("taskbar", true);
        cfgClickThrough_ = j.value("clickThrough", false);
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
        j["overlay"] = overlayHost != nullptr;
        j["taskbar"] = taskbarHost != nullptr;
        j["clickThrough"] = overlayHost && overlayHost->clickThrough();
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

    trayHwnd = CreateWindowExW(0, L"QQMusicLyricTray", L"QQMusicLyricTray", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, inst, this);
    if (!trayHwnd)
        return false;
    updateTrayIcon();
    return true;
}

void App::destroyTray() {
    if (trayHwnd) {
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
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (overlayHost ? MF_CHECKED : 0), kCmdToggleOverlay, L"桌面歌词");
    AppendMenuW(menu, MF_STRING | (taskbarHost ? MF_CHECKED : 0), kCmdToggleTaskbar, L"任务栏歌词");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (overlayHost) {
        AppendMenuW(menu, MF_STRING | (overlayHost->clickThrough() ? MF_CHECKED : 0),
                    kCmdClickThrough, L"鼠标穿透");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kCmdFontUp, L"增大字号");
    AppendMenuW(menu, MF_STRING, kCmdFontDown, L"减小字号");
    AppendMenuW(menu, MF_STRING, kCmdPickFont, L"字体…");
    if (taskbarHost || overlayHost) {
        AppendMenuW(menu, MF_STRING, kCmdPlayedColor, L"已播放颜色…");
        AppendMenuW(menu, MF_STRING, kCmdUnplayedColor, L"未播放颜色…");
        HMENU alphaMenu = CreatePopupMenu();
        for (int i = 0; i < kUnplayedAlphaPresetCount; ++i) {
            wchar_t label[16];
            swprintf_s(label, L"%d%%", kUnplayedAlphaPresets[i]);
            AppendMenuW(alphaMenu,
                        MF_STRING |
                            (lyricUnplayedAlphaPct_ == kUnplayedAlphaPresets[i] ? MF_CHECKED : 0),
                        kCmdUnplayedAlphaBase + i, label);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(alphaMenu), L"未播放透明度");
    }
    if (taskbarHost) {
        AppendMenuW(menu, MF_STRING | (lyricGlow_ ? MF_CHECKED : 0), kCmdToggleGlow,
                    L"歌词光晕");
        AppendMenuW(menu, MF_STRING | (lyricOutline_ ? MF_CHECKED : 0), kCmdToggleOutline,
                    L"歌词描边");
        AppendMenuW(menu, MF_STRING, kCmdGlowColor, L"光晕颜色…");
        AppendMenuW(menu, MF_STRING, kCmdOutlineColor, L"描边颜色…");
    }
    AppendMenuW(menu, MF_STRING, kCmdManualSearch, L"手动搜索歌词");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");

    SetForegroundWindow(trayHwnd);
    POINT pt{};
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                              trayHwnd, nullptr);
    DestroyMenu(menu);

    // 未播放透明度子菜单：命令 ID = 基数 + 预设下标
    if (cmd >= kCmdUnplayedAlphaBase &&
        cmd < kCmdUnplayedAlphaBase + kUnplayedAlphaPresetCount) {
        lyricUnplayedAlphaPct_ = kUnplayedAlphaPresets[cmd - kCmdUnplayedAlphaBase];
        applyFontColors();
        saveSettings();
        return;
    }

    switch (cmd) {
    case kCmdToggleOverlay:
        toggleOverlay();
        saveSettings();
        break;
    case kCmdToggleTaskbar:
        toggleTaskbar();
        saveSettings();
        break;
    case kCmdClickThrough:
        if (overlayHost)
            overlayHost->setClickThrough(!overlayHost->clickThrough());
        saveSettings();
        break;
    case kCmdFontUp:
        hasUserFont_ = true;
        fontSize_ = std::clamp(fontSize_ + 2.0f, 8.0f, 48.0f);
        if (overlayHost) overlayHost->setFont(fontFamily_, fontSize_);
        if (taskbarHost) taskbarHost->setFont(fontFamily_, fontSize_);
        saveSettings();
        break;
    case kCmdFontDown:
        hasUserFont_ = true;
        fontSize_ = std::clamp(fontSize_ - 2.0f, 8.0f, 48.0f);
        if (overlayHost) overlayHost->setFont(fontFamily_, fontSize_);
        if (taskbarHost) taskbarHost->setFont(fontFamily_, fontSize_);
        saveSettings();
        break;
    case kCmdPickFont:
        pickFont();
        break;
    case kCmdPlayedColor:
        pickPlayedColor();
        break;
    case kCmdUnplayedColor:
        pickUnplayedColor();
        break;
    case kCmdToggleGlow:
        lyricGlow_ = !lyricGlow_;
        if (taskbarHost)
            taskbarHost->setFontGlow(lyricGlow_);
        saveSettings();
        break;
    case kCmdToggleOutline:
        lyricOutline_ = !lyricOutline_;
        if (taskbarHost)
            taskbarHost->setFontOutline(lyricOutline_);
        saveSettings();
        break;
    case kCmdGlowColor:
        pickGlowColor();
        break;
    case kCmdOutlineColor:
        pickOutlineColor();
        break;
    case kCmdManualSearch:
        showManualSearch(GetModuleHandleW(nullptr));
        break;
    case kCmdExit:
        PostQuitMessage(0);
        break;
    }
}

void App::pickFont() {
    if (!overlayHost && !taskbarHost)
        return;

    // CHOOSEFONT 的 lfHeight 单位是像素，fontSize_ 是磅，需要按屏幕 DPI 换算
    HDC screen = GetDC(nullptr);
    int dpiY = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen)
        ReleaseDC(nullptr, screen);

    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, fontFamily_.c_str(), LF_FACESIZE);
    lf.lfHeight = -MulDiv((int)std::lround(fontSize_), dpiY, 72);
    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = trayHwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_FORCEFONTEXIST;
    if (!ChooseFontW(&cf))
        return;

    fontFamily_ = lf.lfFaceName;
    if (cf.iPointSize > 0)
        fontSize_ = (float)cf.iPointSize / 10.0f;

    hasUserFont_ = true;
    if (overlayHost)
        overlayHost->setFont(fontFamily_, fontSize_);
    if (taskbarHost)
        taskbarHost->setFont(fontFamily_, fontSize_);
    saveSettings();
}

bool App::pickColor(COLORREF& target) {
    static COLORREF custom[16] = {}; // 记住用户自定义颜色
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = trayHwnd;
    cc.rgbResult = target;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc))
        return false;
    target = cc.rgbResult;
    return true;
}

void App::applyFontColors() {
    for (auto* h : hosts())
        h->setFontColors(lyricColor_, lyricUnplayedColor_, lyricUnplayedAlphaPct_);
}

void App::pickPlayedColor() {
    if (!pickColor(lyricColor_))
        return;
    applyFontColors();
    saveSettings();
}

void App::pickUnplayedColor() {
    if (!pickColor(lyricUnplayedColor_))
        return;
    applyFontColors();
    saveSettings();
}

void App::pickGlowColor() {
    if (!taskbarHost || !pickColor(lyricGlowColor_))
        return;
    taskbarHost->setFontGlowColors(lyricGlowColor_, lyricOutlineColor_);
    saveSettings();
}

void App::pickOutlineColor() {
    if (!taskbarHost || !pickColor(lyricOutlineColor_))
        return;
    taskbarHost->setFontGlowColors(lyricGlowColor_, lyricOutlineColor_);
    saveSettings();
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
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT); // 否则 wprintf 中文输出为 '?'
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 提高计时器粒度，否则 SetTimer 的实际触发间隔可能远大于设定值，滚动动画卡顿
    timeBeginPeriod(1);
    winrt::init_apartment();

    bool wantOverlay = false;
    bool wantTaskbar = false;
    bool hasModeFlag = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--taskbar") == 0) {
                wantTaskbar = true;
                hasModeFlag = true;
            } else if (wcscmp(argv[i], L"--overlay") == 0) {
                wantOverlay = true;
                hasModeFlag = true;
            }
        }
        LocalFree(argv);
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    App app;
    app.loadSettings();
    // 无命令行参数时按上次保存的窗口状态启动；两个都关了则强制任务栏，否则无法找回设置入口
    if (!hasModeFlag) {
        wantOverlay = app.cfgOverlay_;
        wantTaskbar = app.cfgTaskbar_;
        if (!wantOverlay && !wantTaskbar)
            wantTaskbar = true;
    }
    if (!app.createTrayWindow(inst)) {
        std::wprintf(L"failed to create tray window\n");
        return 1;
    }
    if (wantOverlay && !app.createOverlay(inst)) {
        std::wprintf(L"failed to create overlay window\n");
        return 1;
    }
    if (wantTaskbar && !app.createTaskbar(inst)) {
        std::wprintf(L"failed to create taskbar window\n");
        return 1;
    }
    if (!app.overlayHost && !app.taskbarHost) {
        std::wprintf(L"no host enabled\n");
        return 1;
    }

    app.monitor.start([&app] { PostThreadMessageW(app.mainThread, kMsgSmtcChanged, 0, 0); });

    const wchar_t* modeDesc = app.overlayHost && app.taskbarHost ? L"overlay + taskbar"
                              : app.overlayHost                ? L"overlay"
                                                               : L"taskbar";
    std::wprintf(L"QQMusicLyric started (%s mode), waiting for QQ Music...\n", modeDesc);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == nullptr) {
            if (msg.message == kMsgSmtcChanged) {
                app.onSmtcChanged();
            }
            else if (msg.message == kMsgLyricReady) {
                app.onLyricReady(msg.wParam != 0);
            }
            else if (msg.message == kMsgCoverReady) {
                app.onCoverReady(std::unique_ptr<CoverPayload>(
                    reinterpret_cast<CoverPayload*>(msg.lParam)));
            }
            continue;
        }
        if (app.manualSearchDialog && app.manualSearchDialog->isOpen() &&
            IsDialogMessageW(app.manualSearchDialog->hwnd(), &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app.destroyTray();
    timeEndPeriod(1);
    return 0;
}
