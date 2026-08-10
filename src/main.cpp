#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "ui/lyric_window.h"
#include "media/smtc_monitor.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>

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
        OverlayHost host;
        std::wstring currentKey;
        PlaybackStatus lastStatus = PlaybackStatus::Stopped;
        std::shared_ptr<const std::vector<uint8_t>> lastSmtcThumbnail;

        // 状态机：无会话(隐藏) -> 播放中(滚动渲染) <-> 暂停(静止显示)
        void onSmtcChanged() {
            SmtcSnapshot snap = monitor.snapshot();
            if (!snap.sessionAlive) {
                if (!currentKey.empty() || lastStatus != PlaybackStatus::Stopped)
                    std::wprintf(L"[smtc] QQ Music session closed\n");
                currentKey.clear();
                lastStatus = PlaybackStatus::Stopped;
                host.setLyrics({});
                host.setMediaInfo({});
                host.setStatusText(L"QQ 音乐未运行");
                host.hide();
                return;
            }
            host.show();
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
            host.setMediaInfo(mi);
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
                host.setLyrics({});
                host.setStatusText(L"暂无歌词");
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
            host.setMediaInfo(mi);
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
    app.host.setControlCallback([&app](MediaControl c) {
        switch (c) {
        case MediaControl::Prev: app.monitor.skipPrevious(); break;
        case MediaControl::PlayPause: app.monitor.playPause(); break;
        case MediaControl::Next: app.monitor.skipNext(); break;
        }
    });
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
