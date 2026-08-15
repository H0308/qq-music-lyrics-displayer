#include "smtc_monitor.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <utility>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;

namespace {

constexpr wchar_t kTargetAppId[] = L"QQMusic.exe";
constexpr wchar_t kNeteaseGenrePrefix[] = L"NCM-";

struct SessionIdentity {
    SmtcPlayerType player = SmtcPlayerType::Unknown;
    std::wstring neteaseSongId;
    std::wstring sourceAppUserModelId;
    bool enhancedSmtc = false;
};

int64_t nowUtcMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t timeSpanMs(TimeSpan ts) { return ts.count() / 10000; }

// SMTC LastUpdatedTime 是 DateTime（1601 年起 100ns），换算为 Unix 毫秒；
// 部分应用不上报（为 0），返回 0 由调用方回退
int64_t lastUpdatedMs(DateTime dt) {
    constexpr int64_t kFileTimeEpochOffsetMs = 11644473600000LL;
    int64_t ms = dt.time_since_epoch().count() / 10000 - kFileTimeEpochOffsetMs;
    return ms > 0 ? ms : 0;
}

bool parseNeteaseGenre(const std::wstring& genre, std::wstring& songId) {
    if (genre.rfind(kNeteaseGenrePrefix, 0) != 0)
        return false;
    std::wstring value = genre.substr(std::wcslen(kNeteaseGenrePrefix));
    if (value.empty())
        return false;
    for (wchar_t ch : value) {
        if (ch < L'0' || ch > L'9')
            return false;
    }
    songId = std::move(value);
    return true;
}

SessionIdentity identifySession(
    const GlobalSystemMediaTransportControlsSession& session) {
    SessionIdentity identity;
    if (!session)
        return identity;

    try {
        identity.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
        if (identity.sourceAppUserModelId == kTargetAppId) {
            identity.player = SmtcPlayerType::QQMusic;
            return identity;
        }

        // InfLink-rs and the自研桥接器约定把网易云歌曲 ID 放在 SMTC Genres 中，
        // 这样主程序无需依赖 BetterNCM/Chromium 的内部 API。
        auto propsOp = session.TryGetMediaPropertiesAsync();
        if (!propsOp)
            return identity;
        auto props = propsOp.get();
        if (!props)
            return identity;
        auto genres = props.Genres();
        if (!genres)
            return identity;
        for (uint32_t i = 0; i < genres.Size(); ++i) {
            std::wstring songId;
            if (parseNeteaseGenre(genres.GetAt(i).c_str(), songId)) {
                identity.player = SmtcPlayerType::NetEase;
                identity.neteaseSongId = std::move(songId);
                identity.enhancedSmtc = true;
                break;
            }
        }
    } catch (...) {
        // 媒体会话可能在枚举期间退出；保留 Unknown，让调用方忽略它。
    }
    return identity;
}

std::wstring positionTrackKey(const SmtcSnapshot& snapshot) {
    if (snapshot.player == SmtcPlayerType::NetEase && !snapshot.neteaseSongId.empty())
        return L"netease|" + snapshot.neteaseSongId;
    return L"qq|" + snapshot.title + L'|' + snapshot.artist;
}

bool sameSession(const GlobalSystemMediaTransportControlsSession& left,
                 const GlobalSystemMediaTransportControlsSession& right) noexcept {
    return winrt::get_abi(left) == winrt::get_abi(right);
}

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

// 读取封面缩略图为字节数组（失败/过大返回 nullptr）
std::shared_ptr<const std::vector<uint8_t>> readThumbnail(
    const GlobalSystemMediaTransportControlsSessionMediaProperties& props) {
    try {
        auto ref = props.Thumbnail();
        if (!ref) return nullptr;
        auto streamOp = ref.OpenReadAsync();
        if (!streamOp) return nullptr;
        auto stream = streamOp.get();
        if (!stream) return nullptr;
        uint64_t size = stream.Size();
        if (size == 0 || size > 4 * 1024 * 1024) return nullptr;
        auto buf = std::make_shared<std::vector<uint8_t>>((size_t)size);
        Windows::Storage::Streams::DataReader reader(stream);
        auto loadOp = reader.LoadAsync((uint32_t)size);
        if (!loadOp) return nullptr;
        loadOp.get();
        reader.ReadBytes(winrt::array_view<uint8_t>(buf->data(), (uint32_t)buf->size()));
        return buf;
    } catch (...) {
        return nullptr;
    }
}

} // namespace

// 成员顺序有意为之：revoker 最后声明、析构时最先退订，避免回调访问已销毁成员
struct SmtcMonitor::Impl {
    struct SessionWatcher {
        GlobalSystemMediaTransportControlsSession session{ nullptr };
        GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker playbackRevoker;
        GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker propsRevoker;
    };

    GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
    GlobalSystemMediaTransportControlsSession session{ nullptr };

    mutable std::mutex mtx;
    std::mutex attachMtx;
    SmtcSnapshot snap;
    SmtcMonitor::ChangeCallback onChange;

    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessionsRevoker;
    GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker
        currentSessionRevoker;
    GlobalSystemMediaTransportControlsSession::TimelinePropertiesChanged_revoker timelineRevoker;
    // 所有媒体会话都保留轻量监听，避免只监听当前选中会话导致切换丢失。
    std::mutex watchersMtx;
    std::vector<SessionWatcher> sessionWatchers;

    // 进度平滑状态（snapshot() 内使用）。
    // QQ 的 TimelineProperties 播放中会频繁阶跃更新（Position 可能是量化值），
    // 逐字歌词对进度抖动极其敏感：本地时钟按播放速率自走，
    // 原始位置仅作慢速校正（τ=2s 低通），大偏差（seek/切歌）立即对齐
    mutable double smoothPos_ = 0.0;
    mutable std::chrono::steady_clock::time_point posTick_{};
    mutable bool posInit_ = false;
    mutable std::wstring posTrackKey_;
    mutable int64_t prevRawMs_ = 0; // 上次 snapshot 读到的 SMTC 原始位置（未插值）
    mutable bool prevPaused_ = false; // 上一帧 snapshot 是否处于非播放分支
    int64_t lastStatusChangeMs_ = 0;  // 最近一次播放状态切换（UTC ms），识别过渡期残留事件

    // 播放中 QQ 可能推送过期/量化的 Timeline（Position 与 LastUpdatedTime 不成对，
    // 或 LastUpdatedTime 缺失回退为当前时刻），换算到当前时刻后若比现有锚点
    // 落后 250ms~2s，视为噪声丢弃本次更新、保留旧锚点；
    // 落后超过 2s 才认为是真实 seek 回退，予以接受
    bool isStaleTimelineUpdate(int64_t newPosMs, int64_t newAnchorMs) const {
        if (snap.status != PlaybackStatus::Playing || snap.durationMs <= 0)
            return false;
        int64_t now = nowUtcMs();
        int64_t cur = snap.positionMs + std::max<int64_t>(0, now - snap.anchorUtcMs);
        int64_t nxt = newPosMs + std::max<int64_t>(0, now - newAnchorMs);
        int64_t back = cur - nxt;
        return back > 250 && back < 2000;
    }

    void notify() {
        if (onChange) onChange();
    }

    void refreshAll() {
        GlobalSystemMediaTransportControlsSession currentSession{ nullptr };
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
        }

        SmtcSnapshot s;
        const SessionIdentity identity = identifySession(currentSession);
        s.player = identity.player;
        s.neteaseSongId = identity.neteaseSongId;
        s.sourceAppUserModelId = identity.sourceAppUserModelId;
        s.enhancedSmtc = identity.enhancedSmtc;
        s.sessionAlive = identity.player != SmtcPlayerType::Unknown;
        if (currentSession) {
            try {
                auto propsOp = currentSession.TryGetMediaPropertiesAsync();
                if (propsOp) {
                    auto props = propsOp.get();
                    if (props) {
                        s.title = props.Title().c_str();
                        s.artist = props.Artist().c_str();
                        s.album = props.AlbumTitle().c_str();
                        s.thumbnail = readThumbnail(props);
                    }
                }
            } catch (...) {
            }
            try {
                auto tl = currentSession.GetTimelineProperties();
                if (tl) {
                    s.durationMs = timeSpanMs(tl.EndTime());
                    s.positionMs = timeSpanMs(tl.Position());
                    // 锚点用 QQ 采样位置的时刻（而不是我们读取的时刻），补偿事件送达延迟
                    s.anchorUtcMs = lastUpdatedMs(tl.LastUpdatedTime());
                    if (s.anchorUtcMs == 0)
                        s.anchorUtcMs = nowUtcMs();
                    // 同 refreshTimeline：过旧锚点按此刻锚定，避免插值前跳
                    if (nowUtcMs() - s.anchorUtcMs > 2000)
                        s.anchorUtcMs = nowUtcMs();
                }
            } catch (...) {
            }
            try {
                auto info = currentSession.GetPlaybackInfo();
                if (info) {
                    s.status = mapStatus(info.PlaybackStatus());
                    auto c = info.Controls();
                    if (c) {
                        s.canPrev = c.IsPreviousEnabled();
                        s.canPlayPause = c.IsPlayEnabled() || c.IsPauseEnabled();
                        s.canNext = c.IsNextEnabled();
                    }
                }
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!sameSession(session, currentSession))
                return;
            snap = std::move(s);
        }
        notify();
    }

    void refreshTimeline(const GlobalSystemMediaTransportControlsSession& changedSession) {
        if (!changedSession) return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            try {
                if (!sameSession(session, changedSession))
                    return;
                auto tl = changedSession.GetTimelineProperties();
                if (!tl)
                    return;
                snap.durationMs = timeSpanMs(tl.EndTime());
                int64_t newPos = timeSpanMs(tl.Position());
                int64_t newAnchor = lastUpdatedMs(tl.LastUpdatedTime());
                if (newAnchor == 0)
                    newAnchor = nowUtcMs();
                bool anchorStale = nowUtcMs() - newAnchor > 2000;
                // 播放中且刚经历状态切换（2s 内）时，锚点过旧的 Timeline 是快速暂停/播放
                // 残留的延迟事件——真实 seek 的 LastUpdatedTime 必为 seek 当下，不会过旧。
                // 若重锚定后接受，>2s 的倒退会被当成真实 seek，造成高亮整段回退，故丢弃
                // （只更新时长，位置/锚点保持，等下一条新鲜事件）
                bool dropResidual = anchorStale && snap.status == PlaybackStatus::Playing &&
                                    nowUtcMs() - lastStatusChangeMs_ < 2000;
                if (!dropResidual) {
                    // 锚点过旧（如恢复播放瞬间读到暂停前的残留 Timeline）：按此刻锚定，
                    // 否则 snapshot 插值会把整段暂停时长计入，位置瞬间前跳再被真实更新拉回
                    if (anchorStale)
                        newAnchor = nowUtcMs();
                    if (!isStaleTimelineUpdate(newPos, newAnchor)) {
                        snap.positionMs = newPos;
                        snap.anchorUtcMs = newAnchor;
                    }
                }
            } catch (...) {
                return;
            }
        }
        notify();
    }

    void refreshPlayback(const GlobalSystemMediaTransportControlsSession& changedSession) {
        if (!changedSession) return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            try {
                if (!sameSession(session, changedSession))
                    return;
                auto info = changedSession.GetPlaybackInfo();
                if (!info)
                    return;
                auto newStatus = mapStatus(info.PlaybackStatus());
                if (newStatus != snap.status)
                    lastStatusChangeMs_ = nowUtcMs();
                // 暂停->播放过渡：QQ 此刻常上报暂停前的残留 Timeline，直接采用会把滞后
                // 位置烘焙进锚点，随后被播放分支 >800 对齐造成整段高亮回退。
                // 而暂停期间位置本就不会移动（暂停中拖进度条已实时跟随进 smoothPos_），
                // 本地平滑值就是最佳恢复点：以它重建锚点，不信此刻的上报值
                bool resumed = newStatus == PlaybackStatus::Playing &&
                               snap.status != PlaybackStatus::Playing;

                auto tl = changedSession.GetTimelineProperties();
                if (!tl)
                    return;
                int64_t newPos = timeSpanMs(tl.Position());
                int64_t newAnchor = lastUpdatedMs(tl.LastUpdatedTime());
                if (newAnchor == 0)
                    newAnchor = nowUtcMs();
                // 过旧锚点按此刻锚定（残留 Timeline）
                if (nowUtcMs() - newAnchor > 2000)
                    newAnchor = nowUtcMs();
                if (resumed) {
                    std::wstring key = positionTrackKey(snap);
                    snap.positionMs = (posInit_ && posTrackKey_ == key)
                                          ? (int64_t)(smoothPos_ + 0.5)
                                          : newPos; // 无平滑状态（如刚启动）才退回上报值
                    if (snap.durationMs > 0 && snap.positionMs > snap.durationMs)
                        snap.positionMs = snap.durationMs;
                    snap.anchorUtcMs = nowUtcMs();
                } else if (!isStaleTimelineUpdate(newPos, newAnchor)) {
                    snap.positionMs = newPos;
                    snap.anchorUtcMs = newAnchor;
                }
                snap.status = newStatus;
                auto c = info.Controls();
                if (c) {
                    snap.canPrev = c.IsPreviousEnabled();
                    snap.canPlayPause = c.IsPlayEnabled() || c.IsPauseEnabled();
                    snap.canNext = c.IsNextEnabled();
                }
            } catch (...) {
                return;
            }
        }
        notify();
    }

    void attach(const GlobalSystemMediaTransportControlsSession& s) {
        std::lock_guard<std::mutex> attachLock(attachMtx);
        timelineRevoker.revoke();
        {
            std::lock_guard<std::mutex> lk(mtx);
            session = s;
        }
        if (!s) return;
        timelineRevoker = s.TimelinePropertiesChanged(winrt::auto_revoke,
            [this, watched = s](auto&&, auto&&) { refreshTimeline(watched); });
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
                    // 先保留当前选中会话已有的暂停/恢复进度处理，
                    // 再根据所有会话的最新状态重新选择来源。
                    refreshPlayback(watched);
                    updateSession();
                });
            watcher.propsRevoker = watched.MediaPropertiesChanged(winrt::auto_revoke,
                [this](auto&&, auto&&) {
                    // 网易云可能先建立会话、后写入 NCM-{ID}，属性变化也要触发重新识别。
                    updateSession();
                });
            sessionWatchers.push_back(std::move(watcher));
        }
    }

    void updateSession(bool rebuildWatchers = false) {
        GlobalSystemMediaTransportControlsSession found{ nullptr };
        if (manager) {
            try {
                // Windows 已经维护了“用户当前最可能想控制”的媒体会话。
                // 同时运行 QQ 和网易云时，优先使用这个会话，而不是依赖
                // GetSessions() 的枚举顺序或固定的 QQ 优先级。
                auto current = manager.GetCurrentSession();
                const SessionIdentity currentIdentity = identifySession(current);
                if (currentIdentity.player != SmtcPlayerType::Unknown) {
                    try {
                        auto info = current.GetPlaybackInfo();
                        if (info &&
                            mapStatus(info.PlaybackStatus()) == PlaybackStatus::Playing)
                            found = current;
                    } catch (...) {
                    }
                }

                auto sessions = manager.GetSessions();
                if (sessions) {
                    if (rebuildWatchers)
                        watchSessions(sessions);
                    if (!found) {
                        // 当前会话可能是暂停的，而另一个受支持会话正在播放；
                        // 此时选择真正处于 Playing 的来源。
                        for (auto const& s : sessions) {
                            const SessionIdentity identity = identifySession(s);
                            if (identity.player == SmtcPlayerType::Unknown)
                                continue;
                            try {
                                auto info = s.GetPlaybackInfo();
                                if (info &&
                                    mapStatus(info.PlaybackStatus()) == PlaybackStatus::Playing) {
                                    found = s;
                                    break;
                                }
                            } catch (...) {
                            }
                        }
                    }
                }

                if (!found && currentIdentity.player != SmtcPlayerType::Unknown) {
                    // 没有播放中的会话时，保留 Windows 当前会话（例如暂停状态），
                    // 这样恢复播放和拖动进度仍然由同一来源接收。
                    found = current;
                }

                if (!found && sessions) {
                    // 当前会话不可用时，保留一个可识别会话作为启动/恢复兜底。
                    for (auto const& s : sessions) {
                        if (identifySession(s).player != SmtcPlayerType::Unknown) {
                            found = s;
                            break;
                        }
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
    SmtcSnapshot s = impl_->snap;
    const int64_t rawPos = s.positionMs; // SMTC 原始上报值（下方 Playing 分支才会插值）
    if (s.sessionAlive && s.status == PlaybackStatus::Playing) {
        int64_t elapsed = nowUtcMs() - s.anchorUtcMs;
        if (elapsed > 0) {
            s.positionMs += elapsed;
            if (s.durationMs > 0 && s.positionMs > s.durationMs)
                s.positionMs = s.durationMs;
        }
    }
    if (!s.sessionAlive) {
        impl_->posInit_ = false;
        impl_->prevPaused_ = false;
        return s;
    }
    // 进度平滑：本地时钟自走 + 慢速校正，滤掉锚点阶跃
    std::wstring key = positionTrackKey(s);
    auto now = std::chrono::steady_clock::now();
    float dt = 16.7f;
    if (impl_->posTick_ != std::chrono::steady_clock::time_point{})
        dt = std::chrono::duration<float, std::milli>(now - impl_->posTick_).count();
    impl_->posTick_ = now;
    if (s.status != PlaybackStatus::Playing) {
        // 非播放态冻结在平滑值，不高亮回退也不抢跑。
        // 进入暂停的第一帧（上一帧还在播放）无条件冻结：快速连续暂停/播放时 QQ 此刻
        // 上报的 Timeline 可能是被合并/滞后的残留值（甚至可能以 >2s 倒退穿过噪声过滤），
        // 跟随会回退逐字高亮；只有持续暂停中原始值自身跳变（>250ms，用户拖进度条，
        // 250 与 isStaleTimelineUpdate 的噪声窗口一致）才跟随
        double rawJump = (double)(rawPos - impl_->prevRawMs_);
        bool seekWhilePaused = impl_->prevPaused_ && std::fabs(rawJump) > 250.0;
        if (!impl_->posInit_ || impl_->posTrackKey_ != key || seekWhilePaused)
            impl_->smoothPos_ = (double)s.positionMs;
        impl_->posTrackKey_ = key;
        impl_->posInit_ = true;
        impl_->prevRawMs_ = rawPos;
        impl_->prevPaused_ = true;
        int64_t paused = (int64_t)(impl_->smoothPos_ + 0.5);
        if (s.durationMs > 0 && paused > s.durationMs)
            paused = s.durationMs;
        s.positionMs = std::max<int64_t>(paused, 0);
        return s;
    }
    if (!impl_->posInit_ || impl_->posTrackKey_ != key) {
        impl_->smoothPos_ = (double)s.positionMs;
        impl_->posTrackKey_ = key;
        impl_->posInit_ = true;
    } else {
        double err = (double)s.positionMs - impl_->smoothPos_;
        if (std::fabs(err) > 800.0) {
            impl_->smoothPos_ = (double)s.positionMs; // seek/跳变立即对齐（允许倒退）
        } else {
            double prev = impl_->smoothPos_;
            impl_->smoothPos_ += dt;                              // 按播放速率自走
            impl_->smoothPos_ += err * (1.0 - std::exp(-dt / 2000.0)); // 慢速校正
            // 播放中不倒退：原始位置小幅落后只是 SMTC 上报滞后/量化，
            // 原地等待它追上来，避免逐字填充退回重放
            if (impl_->smoothPos_ < prev)
                impl_->smoothPos_ = prev;
        }
    }
    int64_t smoothed = (int64_t)(impl_->smoothPos_ + 0.5);
    if (s.durationMs > 0 && smoothed > s.durationMs)
        smoothed = s.durationMs;
    if (smoothed < 0)
        smoothed = 0;
    s.positionMs = smoothed;
    impl_->prevRawMs_ = rawPos; // 播放中也要保持最新，暂停瞬间的 rawJump 才测得准
    impl_->prevPaused_ = false;
    return s;
}

void SmtcMonitor::playPause() {
    GlobalSystemMediaTransportControlsSession s{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        s = impl_->session;
    }
    if (!s) return;
    try {
        auto info = s.GetPlaybackInfo();
        if (!info) return;
        auto st = info.PlaybackStatus();
        if (st == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
            auto op = s.TryPauseAsync();
            if (op) op.get();
        } else {
            auto op = s.TryPlayAsync();
            if (op) op.get();
        }
    } catch (...) {
    }
}

void SmtcMonitor::skipNext() {
    GlobalSystemMediaTransportControlsSession s{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        s = impl_->session;
    }
    if (!s) return;
    try {
        auto op = s.TrySkipNextAsync();
        if (op) op.get();
    } catch (...) {
    }
}

void SmtcMonitor::skipPrevious() {
    GlobalSystemMediaTransportControlsSession s{ nullptr };
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        s = impl_->session;
    }
    if (!s) return;
    try {
        auto op = s.TrySkipPreviousAsync();
        if (op) op.get();
    } catch (...) {
    }
}
