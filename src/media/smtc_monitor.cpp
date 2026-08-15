#include "media/smtc_monitor.h"

#include "media/smtc/netease_smtc_adapter.h"
#include "media/smtc/qq_music_smtc_adapter.h"
#include "media/smtc/smtc_common.h"

#include <mutex>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

using Session = smtc::Session;
using namespace winrt;
using namespace winrt::Windows::Media::Control;

// 成员顺序有意为之：revoker 最后声明、析构时最先退订，避免回调访问已销毁成员。
struct SmtcMonitor::Impl {
    struct SessionWatcher {
        Session session{ nullptr };
        Session::PlaybackInfoChanged_revoker playbackRevoker;
        Session::MediaPropertiesChanged_revoker propsRevoker;
    };

    GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
    Session session{ nullptr };

    mutable std::mutex mtx;
    std::mutex attachMtx;
    SmtcSnapshot snap;
    SmtcMonitor::ChangeCallback onChange;

    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessionsRevoker;
    GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker
        currentSessionRevoker;
    Session::TimelinePropertiesChanged_revoker timelineRevoker;

    // 所有媒体会话都保留轻量监听，避免只监听当前选中会话导致切换丢失。
    std::mutex watchersMtx;
    std::vector<SessionWatcher> sessionWatchers;

    // 新播放器只需实现一个适配器并在这里注册，监控器本身不再增加播放器分支。
    std::vector<std::unique_ptr<smtc::SmtcPlayerAdapter>> adapters;

    Impl() {
        adapters.emplace_back(std::make_unique<smtc::QqMusicSmtcAdapter>());
        adapters.emplace_back(std::make_unique<smtc::NeteaseSmtcAdapter>());
    }

    smtc::SmtcPlayerAdapter* adapterFor(SmtcPlayerType player) const {
        for (const auto& adapter : adapters) {
            if (adapter && adapter->playerType() == player)
                return adapter.get();
        }
        return nullptr;
    }

    smtc::SmtcSessionIdentity identifySession(const Session& candidate) const {
        for (const auto& adapter : adapters) {
            if (!adapter)
                continue;
            auto identity = adapter->identifySession(candidate);
            if (identity.player != SmtcPlayerType::Unknown)
                return identity;
        }
        return {};
    }

    void notify() {
        if (onChange)
            onChange();
    }

    void refreshAll() {
        Session currentSession{ nullptr };
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
        }

        SmtcSnapshot next;
        const smtc::SmtcSessionIdentity identity = identifySession(currentSession);
        next.player = identity.player;
        next.neteaseSongId = identity.neteaseSongId;
        next.sourceAppUserModelId = identity.sourceAppUserModelId;
        next.enhancedSmtc = identity.enhancedSmtc;
        next.sessionAlive = identity.player != SmtcPlayerType::Unknown;

        if (currentSession) {
            try {
                auto propsOp = currentSession.TryGetMediaPropertiesAsync();
                if (propsOp) {
                    auto props = propsOp.get();
                    if (props) {
                        next.title = props.Title().c_str();
                        next.artist = props.Artist().c_str();
                        next.album = props.AlbumTitle().c_str();
                        next.thumbnail = smtc::readThumbnail(props);
                    }
                }
            } catch (...) {
            }

            try {
                auto timeline = currentSession.GetTimelineProperties();
                if (timeline) {
                    next.durationMs = smtc::timeSpanMs(timeline.EndTime());
                    next.positionMs = smtc::timeSpanMs(timeline.Position());
                    next.anchorUtcMs = smtc::lastUpdatedMs(timeline.LastUpdatedTime());
                    if (next.anchorUtcMs == 0)
                        next.anchorUtcMs = smtc::nowUtcMs();
                }
            } catch (...) {
            }

            if (next.anchorUtcMs != 0) {
                if (auto* adapter = adapterFor(next.player))
                    adapter->prepareInitialSnapshot(next);
            }

            try {
                auto info = currentSession.GetPlaybackInfo();
                if (info) {
                    next.status = smtc::mapStatus(info.PlaybackStatus());
                    auto controls = info.Controls();
                    if (controls) {
                        next.canPrev = controls.IsPreviousEnabled();
                        next.canPlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
                        next.canNext = controls.IsNextEnabled();
                    }
                }
            } catch (...) {
            }
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!smtc::sameSession(session, currentSession))
                return;
            snap = std::move(next);
        }
        notify();
    }

    void refreshCurrentSession(const Session& changedSession) {
        if (!changedSession)
            return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!smtc::sameSession(session, changedSession))
                return;
        }
        // MediaPropertiesChanged 表示歌曲或播放器识别属性可能已经切换，
        // 因此重新读取完整的当前快照。
        refreshAll();
    }

    void refreshTimeline(const Session& changedSession) {
        if (!changedSession)
            return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            try {
                if (!smtc::sameSession(session, changedSession))
                    return;
                if (auto* adapter = adapterFor(snap.player))
                    adapter->refreshTimeline(changedSession, snap, smtc::nowUtcMs());
            } catch (...) {
                return;
            }
        }
        notify();
    }

    void refreshPlayback(const Session& changedSession) {
        if (!changedSession)
            return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            try {
                if (!smtc::sameSession(session, changedSession))
                    return;
                if (auto* adapter = adapterFor(snap.player))
                    adapter->refreshPlayback(changedSession, snap, smtc::nowUtcMs());
            } catch (...) {
                return;
            }
        }
        notify();
    }

    void attach(const Session& selected) {
        std::lock_guard<std::mutex> attachLock(attachMtx);
        timelineRevoker.revoke();
        {
            std::lock_guard<std::mutex> lk(mtx);
            session = selected;
            for (auto& adapter : adapters) {
                if (adapter)
                    adapter->reset();
            }
        }
        if (!selected)
            return;

        timelineRevoker = selected.TimelinePropertiesChanged(winrt::auto_revoke,
            [this, watched = selected](auto&&, auto&&) { refreshTimeline(watched); });
    }

    void watchSessions(const auto& sessions) {
        std::lock_guard<std::mutex> watchersLock(watchersMtx);
        sessionWatchers.clear();
        for (auto const& watched : sessions) {
            if (!watched)
                continue;
            SessionWatcher watcher;
            watcher.session = watched;
            watcher.playbackRevoker = watched.PlaybackInfoChanged(winrt::auto_revoke,
                [this, watched](auto&&, auto&&) {
                    // 先处理当前选中会话的播放器专属进度，再重新选择活动来源。
                    refreshPlayback(watched);
                    updateSession();
                });
            watcher.propsRevoker = watched.MediaPropertiesChanged(winrt::auto_revoke,
                [this, watched](auto&&, auto&&) {
                    // 网易云可能先建立会话、后写入 NCM-{ID}，属性变化也要触发重新识别。
                    refreshCurrentSession(watched);
                    updateSession();
                });
            sessionWatchers.push_back(std::move(watcher));
        }
    }

    void updateSession(bool rebuildWatchers = false) {
        Session found{ nullptr };
        if (manager) {
            try {
                // Windows 已经维护了“用户当前最可能想控制”的媒体会话。
                auto current = manager.GetCurrentSession();
                const auto currentIdentity = identifySession(current);
                if (currentIdentity.player != SmtcPlayerType::Unknown) {
                    try {
                        auto info = current.GetPlaybackInfo();
                        if (info &&
                            smtc::mapStatus(info.PlaybackStatus()) == PlaybackStatus::Playing)
                            found = current;
                    } catch (...) {
                    }
                }

                auto sessions = manager.GetSessions();
                if (sessions) {
                    if (rebuildWatchers)
                        watchSessions(sessions);
                    if (!found) {
                        // 当前会话可能暂停，而另一个受支持会话正在播放。
                        for (auto const& candidate : sessions) {
                            const auto identity = identifySession(candidate);
                            if (identity.player == SmtcPlayerType::Unknown)
                                continue;
                            try {
                                auto info = candidate.GetPlaybackInfo();
                                if (info &&
                                    smtc::mapStatus(info.PlaybackStatus()) == PlaybackStatus::Playing) {
                                    found = candidate;
                                    break;
                                }
                            } catch (...) {
                            }
                        }
                    }
                }

                if (!found && currentIdentity.player != SmtcPlayerType::Unknown)
                    found = current;

                if (!found && sessions) {
                    for (auto const& candidate : sessions) {
                        if (identifySession(candidate).player != SmtcPlayerType::Unknown) {
                            found = candidate;
                            break;
                        }
                    }
                }
            } catch (...) {
            }
        }

        Session currentSession{ nullptr };
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
        }
        if (!smtc::sameSession(currentSession, found)) {
            attach(found);
            refreshAll();
        }
    }
};

SmtcMonitor::SmtcMonitor() : impl_(std::make_unique<Impl>()) {}
SmtcMonitor::~SmtcMonitor() = default;

void SmtcMonitor::start(ChangeCallback onChange) {
    impl_->onChange = std::move(onChange);
    auto managerOp = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
    if (!managerOp)
        return;
    impl_->manager = managerOp.get();
    if (!impl_->manager)
        return;
    impl_->sessionsRevoker = impl_->manager.SessionsChanged(winrt::auto_revoke,
        [this](auto&&, auto&&) { impl_->updateSession(true); });
    impl_->currentSessionRevoker = impl_->manager.CurrentSessionChanged(winrt::auto_revoke,
        [this](auto&&, auto&&) { impl_->updateSession(); });
    impl_->updateSession(true);
}

SmtcSnapshot SmtcMonitor::snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SmtcSnapshot source = impl_->snap;
    if (!source.sessionAlive)
        return source;
    if (auto* adapter = impl_->adapterFor(source.player))
        return adapter->snapshot(source, smtc::nowUtcMs());
    return source;
}

void SmtcMonitor::playPause() {
    Session current{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
    }
    if (!current)
        return;
    try {
        auto info = current.GetPlaybackInfo();
        if (!info)
            return;
        auto status = info.PlaybackStatus();
        if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
            auto op = current.TryPauseAsync();
            if (op)
                op.get();
        } else {
            auto op = current.TryPlayAsync();
            if (op)
                op.get();
        }
    } catch (...) {
    }
}

void SmtcMonitor::skipNext() {
    Session current{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
    }
    if (!current)
        return;
    try {
        auto op = current.TrySkipNextAsync();
        if (op)
            op.get();
    } catch (...) {
    }
}

void SmtcMonitor::skipPrevious() {
    Session current{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
    }
    if (!current)
        return;
    try {
        auto op = current.TrySkipPreviousAsync();
        if (op)
            op.get();
    } catch (...) {
    }
}
