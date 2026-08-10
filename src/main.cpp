#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "ui/lyric_window.h"
#include "ui/taskbar_host.h"
#include "media/smtc_monitor.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <shellapi.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT kMsgSmtcChanged = WM_APP + 1;
constexpr UINT kMsgLyricReady = WM_APP + 2;
constexpr UINT kMsgCoverReady = WM_APP + 3;

struct CoverPayload {
    std::wstring key;
    std::shared_ptr<const std::vector<uint8_t>> cover;
};

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
    std::unique_ptr<ILyricHost> taskbarHost;
    std::wstring currentKey;
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    std::shared_ptr<const std::vector<uint8_t>> lastSmtcThumbnail;

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
        host->setHostToggleCallback([this] { toggleTaskbar(); });
        overlayHost = std::move(host);
        return true;
    }

    void destroyOverlay() {
        overlayHost.reset();
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
        host->setHostToggleCallback([this] { toggleOverlay(); });
        taskbarHost = std::move(host);
        return true;
    }

    void destroyTaskbar() {
        taskbarHost.reset();
    }

    void toggleOverlay() {
        if (overlayHost) {
            destroyOverlay();
            if (!taskbarHost) PostQuitMessage(0);
        } else {
            createOverlay(GetModuleHandleW(nullptr));
        }
    }

    void toggleTaskbar() {
        if (taskbarHost) {
            destroyTaskbar();
            if (!overlayHost) PostQuitMessage(0);
        } else {
            createTaskbar(GetModuleHandleW(nullptr));
        }
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
            std::wprintf(L"[smtc] track: %s - %s (%lld ms)\n", snap.title.c_str(),
                snap.artist.c_str(), snap.durationMs);
            for (auto* h : hs) {
                h->setLyrics({});
                h->setStatusText(L"歌词加载中…");
            }
            provider.requestAsync(snap.title, snap.artist, snap.durationMs, [this](bool ok) {
                PostThreadMessageW(mainThread, kMsgLyricReady, ok ? 1 : 0, 0);
            });
        }
    }

    void onLyricReady(bool ok) {
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
        }
    }
};

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT); // 否则 wprintf 中文输出为 '?'
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    if (!hasModeFlag)
        wantOverlay = true;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    App app;
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
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
