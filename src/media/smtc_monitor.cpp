#include "smtc_monitor.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <chrono>
#include <mutex>
#include <utility>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;

namespace {

constexpr wchar_t kTargetAppId[] = L"QQMusic.exe";

int64_t nowUtcMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t timeSpanMs(TimeSpan ts) { return ts.count() / 10000; }

PlaybackStatus mapStatus(GlobalSystemMediaTransportControlsSessionPlaybackStatus s) {
    switch (s) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
        return PlaybackStatus::Playing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
        return PlaybackStatus::Paused;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
        return PlaybackStatus::Stopped;
    default:
        return PlaybackStatus::Other;
    }
}

} // namespace

// 成员顺序有意为之：revoker 最后声明、析构时最先退订，避免回调访问已销毁成员
struct SmtcMonitor::Impl {
    GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
    GlobalSystemMediaTransportControlsSession session{ nullptr };

    mutable std::mutex mtx;
    SmtcSnapshot snap;
    SmtcMonitor::ChangeCallback onChange;

    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessionsRevoker;
    GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker propsRevoker;
    GlobalSystemMediaTransportControlsSession::TimelinePropertiesChanged_revoker timelineRevoker;
    GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker playbackRevoker;

    void notify() {
        if (onChange) onChange();
    }

    void refreshAll() {
        SmtcSnapshot s;
        s.sessionAlive = session != nullptr;
        if (session) {
            try {
                auto props = session.TryGetMediaPropertiesAsync().get();
                s.title = props.Title().c_str();
                s.artist = props.Artist().c_str();
                s.album = props.AlbumTitle().c_str();
            } catch (...) {
            }
            try {
                auto tl = session.GetTimelineProperties();
                s.durationMs = timeSpanMs(tl.EndTime());
                s.positionMs = timeSpanMs(tl.Position());
                s.anchorUtcMs = nowUtcMs();
            } catch (...) {
            }
            try {
                s.status = mapStatus(session.GetPlaybackInfo().PlaybackStatus());
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            snap = std::move(s);
        }
        notify();
    }

    void refreshTimeline() {
        if (!session) return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            try {
                auto tl = session.GetTimelineProperties();
                snap.durationMs = timeSpanMs(tl.EndTime());
                snap.positionMs = timeSpanMs(tl.Position());
                snap.anchorUtcMs = nowUtcMs();
            } catch (...) {
                return;
            }
        }
        notify();
    }

    void refreshPlayback() {
        if (!session) return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            try {
                auto tl = session.GetTimelineProperties();
                snap.positionMs = timeSpanMs(tl.Position());
                snap.anchorUtcMs = nowUtcMs();
                snap.status = mapStatus(session.GetPlaybackInfo().PlaybackStatus());
            } catch (...) {
                return;
            }
        }
        notify();
    }

    void attach(const GlobalSystemMediaTransportControlsSession& s) {
        session = s;
        propsRevoker.revoke();
        timelineRevoker.revoke();
        playbackRevoker.revoke();
        if (!session) return;
        propsRevoker = session.MediaPropertiesChanged(winrt::auto_revoke,
            [this](auto&&, auto&&) { refreshAll(); });
        timelineRevoker = session.TimelinePropertiesChanged(winrt::auto_revoke,
            [this](auto&&, auto&&) { refreshTimeline(); });
        playbackRevoker = session.PlaybackInfoChanged(winrt::auto_revoke,
            [this](auto&&, auto&&) { refreshPlayback(); });
    }

    void updateSession() {
        GlobalSystemMediaTransportControlsSession found{ nullptr };
        if (manager) {
            try {
                for (auto const& s : manager.GetSessions()) {
                    if (s.SourceAppUserModelId() == kTargetAppId) {
                        found = s;
                        break;
                    }
                }
            } catch (...) {
            }
        }
        attach(found);
        refreshAll();
    }
};

SmtcMonitor::SmtcMonitor() : impl_(std::make_unique<Impl>()) {}
SmtcMonitor::~SmtcMonitor() = default;

void SmtcMonitor::start(ChangeCallback onChange) {
    impl_->onChange = std::move(onChange);
    impl_->manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    impl_->sessionsRevoker = impl_->manager.SessionsChanged(winrt::auto_revoke,
        [this](auto&&, auto&&) { impl_->updateSession(); });
    impl_->updateSession();
}

SmtcSnapshot SmtcMonitor::snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SmtcSnapshot s = impl_->snap;
    if (s.sessionAlive && s.status == PlaybackStatus::Playing) {
        int64_t elapsed = nowUtcMs() - s.anchorUtcMs;
        if (elapsed > 0) {
            s.positionMs += elapsed;
            if (s.durationMs > 0 && s.positionMs > s.durationMs)
                s.positionMs = s.durationMs;
        }
    }
    return s;
}
