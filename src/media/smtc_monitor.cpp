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

    // 控制按钮运行在任务栏窗口线程上，不能同步等待远程 WinRT 操作。
    // 失败状态在完成回调中直接忽略，避免在切歌期间对已失效会话调用
    // GetResults()，也避免阻塞任务栏消息循环。
    template <typename StartOperation>
    void startControlOperation(StartOperation&& start) {
        try {
            auto operation = start();
            if (!operation)
                return;
            operation.Completed([](auto&& completedOperation, auto status) {
                if (status != winrt::Windows::Foundation::AsyncStatus::Completed)
                    return;
                try {
                    (void)completedOperation.GetResults();
                } catch (...) {
                }
            });
        } catch (...) {
        }
    }

    void refreshAll() {
        Session currentSession{ nullptr };
        SmtcSnapshot previous;
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
            previous = snap;
        }

        SmtcSnapshot next;
        smtc::SmtcSessionIdentity identity = identifySession(currentSession);
        if (identity.player == SmtcPlayerType::Unknown && previous.sessionAlive &&
            currentSession) {
            try {
                // 网易云的身份来自 Genres 中的 NCM-{ID}，播放/暂停/切歌的过渡窗口里
                // 状态或属性会暂时读不全。只要仍是同一来源的会话，就保留上一份身份，
                // 避免 sessionAlive 短暂翻转为 false 让任务栏窗口整体闪隐。
                const std::wstring source = currentSession.SourceAppUserModelId().c_str();
                if (!source.empty() && source == previous.sourceAppUserModelId) {
                    identity.player = previous.player;
                    identity.neteaseSongId = previous.neteaseSongId;
                    identity.sourceAppUserModelId = previous.sourceAppUserModelId;
                    identity.enhancedSmtc = previous.enhancedSmtc;
                }
            } catch (...) {
            }
        }
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
        try {
            timelineRevoker.revoke();
        } catch (...) {
            // 旧会话刚结束时撤销事件也可能返回 HRESULT；旧会话已经失效，
            // 不能让异常穿过 SMTC 事件回调。
        }
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

        try {
            timelineRevoker = selected.TimelinePropertiesChanged(winrt::auto_revoke,
                [this, watched = selected](auto&&, auto&&) { refreshTimeline(watched); });
        } catch (...) {
            // 会话可能在绑定监听前已经被播放器撤销；下一次 SessionsChanged
            // 或 CurrentSessionChanged 会重新尝试绑定。
        }
    }

    void watchSessions(const auto& sessions) {
        std::lock_guard<std::mutex> watchersLock(watchersMtx);
        sessionWatchers.clear();
        for (auto const& watched : sessions) {
            if (!watched)
                continue;
            try {
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
            } catch (...) {
                // 单个已失效会话绑定失败不应阻断其他会话的监听。
            }
        }
    }

    void updateSession(bool rebuildWatchers = false) {
        Session found{ nullptr };
        Session selectedSession{ nullptr };
        bool selectedSessionAlive = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            selectedSession = session;
            selectedSessionAlive = snap.sessionAlive;
        }
        if (manager) {
            try {
                // Windows 已经维护了“用户当前最可能想控制”的媒体会话。
                auto current = manager.GetCurrentSession();
                auto sessions = manager.GetSessions();
                // GetCurrentSession() 在播放器退出的边界上可能短暂返回已经断开的
                // 旧会话。先用会话列表确认它仍然存在，再读取媒体属性，避免对
                // 已失效的网易云会话继续调用 TryGetMediaPropertiesAsync()。
                bool currentListed = false;
                if (current && sessions) {
                    for (auto const& candidate : sessions) {
                        if (smtc::sameSession(current, candidate)) {
                            currentListed = true;
                            break;
                        }
                    }
                }

                smtc::SmtcSessionIdentity currentIdentity;
                if (current && currentListed) {
                    currentIdentity = identifySession(current);
                    if (currentIdentity.player != SmtcPlayerType::Unknown) {
                        try {
                            auto info = current.GetPlaybackInfo();
                            if (info &&
                                smtc::mapStatus(info.PlaybackStatus()) == PlaybackStatus::Playing)
                                found = current;
                        } catch (...) {
                        }
                    }
                }

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

                // 暂停不是切换理由：已选中的会话仍存在时保持选中。否则网易云暂停后，
                // Windows 当前会话若指向另一个暂停的播放器（如 QQ 音乐），显示会跳走，
                // 后续控制按钮也会作用到错误的会话上。
                if (!found && selectedSessionAlive && sessions) {
                    for (auto const& candidate : sessions) {
                        if (smtc::sameSession(selectedSession, candidate)) {
                            found = candidate;
                            break;
                        }
                    }
                }

                if (!found && currentListed &&
                    currentIdentity.player != SmtcPlayerType::Unknown)
                    found = current;

                // 网易云暂停/恢复时可能短暂上报 Changing/Opened 状态。适配器会把这类
                // 非稳定状态映射为 Other，识别结果暂时变成 Unknown；但只要仍是此前
                // 已选中的同一会话，就不能在这个过渡窗口把会话解绑，否则主程序会先
                // 收到空快照并隐藏任务栏窗口，下一条 Playing 事件到达时再整体显示。
                // 保留条件必须仅限 Opened/Changing 这种真正的过渡态：Playing/Paused
                // 是稳定态，此时识别失败只可能是属性 RPC 已随播放器退出而失败
                // （RPC_E_SERVERUNAVAILABLE）。若把这类已失效会话保留下来，
                // sameSession 判等会让下面跳过 attach/refreshAll，且死会话不会再
                // 产生任何事件，歌词就永久停在网易云退出的前一刻。
                if (!found && selectedSessionAlive && currentListed &&
                    smtc::sameSession(selectedSession, current) && current) {
                    bool transient = false;
                    try {
                        auto info = current.GetPlaybackInfo();
                        if (info) {
                            const auto status = info.PlaybackStatus();
                            transient =
                                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened ||
                                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing;
                        }
                    } catch (...) {
                        // 会话已经无法读取时不能继续保留旧会话；否则网易云退出后
                        // 会一直显示退出前的最后一帧歌词。
                    }
                    if (transient)
                        found = current;
                }

                // 已经有选中会话时，如果它在退出/切换边界上失效，不能再从
                // 会话列表中随便挑一个同播放器的暂停会话把旧状态续回来；否则
                // 网易云退出后会重新选中旧会话，歌词就会停在最后一刻。只有在
                // 尚未选中过任何会话时，才用已识别的会话作为初始候选。
                if (!found && !selectedSessionAlive && sessions) {
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
            try {
                attach(found);
                refreshAll();
            } catch (...) {
                // 会话切换与属性读取发生在播放器生命周期边界，失败时保持
                // 当前快照，等待下一次系统媒体会话事件重试。
            }
        }
    }
};

SmtcMonitor::SmtcMonitor() : impl_(std::make_unique<Impl>()) {}
SmtcMonitor::~SmtcMonitor() = default;

void SmtcMonitor::start(ChangeCallback onChange) {
    impl_->onChange = std::move(onChange);
    try {
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
    } catch (...) {
        // 没有可用媒体会话或系统媒体控制能力时，监控器保持空快照，
        // 不让启动阶段的 HRESULT 终止主程序。
        impl_->manager = nullptr;
    }
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
    PlaybackStatus status = PlaybackStatus::Other;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
        status = impl_->snap.status;
    }
    if (!current)
        return;
    if (status == PlaybackStatus::Playing)
        impl_->startControlOperation([current] { return current.TryPauseAsync(); });
    else
        impl_->startControlOperation([current] { return current.TryPlayAsync(); });
}

void SmtcMonitor::skipNext() {
    Session current{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
    }
    if (!current)
        return;
    impl_->startControlOperation([current] { return current.TrySkipNextAsync(); });
}

void SmtcMonitor::skipPrevious() {
    Session current{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
    }
    if (!current)
        return;
    impl_->startControlOperation([current] { return current.TrySkipPreviousAsync(); });
}
