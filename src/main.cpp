#include "lyric/lyric_provider.h"
#include "ui/lyric_window.h"
#include "media/smtc_monitor.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>

namespace {

    constexpr UINT kMsgSmtcChanged = WM_APP + 1;
    constexpr UINT kMsgLyricReady = WM_APP + 2;

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
        OverlayHost host;
        std::wstring currentKey;
        PlaybackStatus lastStatus = PlaybackStatus::Stopped;

        // 状态机：无会话(隐藏) -> 播放中(滚动渲染) <-> 暂停(静止显示)
        void onSmtcChanged() {
            SmtcSnapshot snap = monitor.snapshot();
            if (!snap.sessionAlive) {
                if (!currentKey.empty() || lastStatus != PlaybackStatus::Stopped)
                    std::wprintf(L"[smtc] QQ Music session closed\n");
                currentKey.clear();
                lastStatus = PlaybackStatus::Stopped;
                host.setLyrics({});
                host.setStatusText(L"QQ 音乐未运行");
                host.hide();
                return;
            }
            host.show();
            if (snap.status != lastStatus) {
                std::wprintf(L"[smtc] status: %s\n", statusName(snap.status));
                lastStatus = snap.status;
            }
            std::wstring key = LyricProvider::makeKey(snap.title, snap.artist);
            if (key != currentKey && !snap.title.empty()) {
                currentKey = key;
                std::wprintf(L"[smtc] track: %s - %s (%lld ms)\n", snap.title.c_str(),
                    snap.artist.c_str(), snap.durationMs);
                host.setLyrics({});
                host.setStatusText(L"歌词加载中…");
                provider.requestAsync(snap.title, snap.artist, snap.durationMs, [this](bool ok) {
                    PostThreadMessageW(mainThread, kMsgLyricReady, ok ? 1 : 0, 0);
                    });
            }
        }

        void onLyricReady(bool ok) {
            if (ok) {
                host.setLyrics(provider.lines());
                std::wprintf(L"[lyric] loaded %zu lines: %s\n", host.lyrics().size(),
                    currentKey.c_str());
            }
            else {
                host.setLyrics({});
                host.setStatusText(L"暂无歌词");
                std::wprintf(L"[lyric] not found: %s\n", currentKey.c_str());
            }
        }

        // 30fps：插值进度 -> 二分定位当前行
        void onFrame() {
            SmtcSnapshot snap = monitor.snapshot();
            if (!snap.sessionAlive) return;
            int idx = LyricProvider::findLine(host.lyrics(), snap.positionMs);
            host.setCurrentLine(idx);
        }
    };

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT); // 否则 wprintf 中文输出为 '?'
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment();

    App app;
    if (!app.host.create(GetModuleHandleW(nullptr))) {
        std::wprintf(L"failed to create overlay window\n");
        return 1;
    }
    app.host.setTickCallback([&app] { app.onFrame(); });
    app.monitor.start([&app] { PostThreadMessageW(app.mainThread, kMsgSmtcChanged, 0, 0); });
    std::wprintf(L"QQMusicLyric started, waiting for QQ Music...\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == nullptr) {
            if (msg.message == kMsgSmtcChanged) {
                app.onSmtcChanged();
            }
            else if (msg.message == kMsgLyricReady) {
                app.onLyricReady(msg.wParam != 0);
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
