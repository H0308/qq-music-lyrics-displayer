#include "media/smtc/qq_music_smtc_adapter.h"

#include "media/smtc/smtc_common.h"

#include <algorithm>
#include <cmath>

namespace smtc {

namespace {

constexpr wchar_t kQqMusicAppId[] = L"QQMusic.exe";

// 残留时间线窗口：切歌后 3 秒内、与残留位置相差 2 秒内的时间线视为旧歌残留
constexpr int64_t kResidualWindowMs = 3000;
constexpr int64_t kResidualToleranceMs = 2000;

std::wstring positionTrackKey(const SmtcSnapshot& snapshot) {
    return L"qq|" + snapshot.title + L'|' + snapshot.artist;
}

} // namespace

bool QqMusicSmtcAdapter::isResidualTimeline(int64_t newPosMs, int64_t nowMs) const {
    if (residualPosMs_ < 0 || nowMs - residualAtMs_ >= kResidualWindowMs)
        return false;
    const int64_t diff = newPosMs - residualPosMs_;
    return diff >= -kResidualToleranceMs && diff <= kResidualToleranceMs;
}

SmtcPlayerType QqMusicSmtcAdapter::playerType() const noexcept {
    return SmtcPlayerType::QQMusic;
}

SmtcSessionIdentity QqMusicSmtcAdapter::identifySession(const Session& session) const {
    SmtcSessionIdentity identity;
    if (!session)
        return identity;
    try {
        identity.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
        if (identity.sourceAppUserModelId == kQqMusicAppId)
            identity.player = SmtcPlayerType::QQMusic;
    } catch (...) {
    }
    return identity;
}

void QqMusicSmtcAdapter::prepareInitialSnapshot(SmtcSnapshot& snapshot) const {
    const int64_t now = nowUtcMs();
    if (snapshot.anchorUtcMs == 0)
        snapshot.anchorUtcMs = now;
    if (now - snapshot.anchorUtcMs > 2000)
        snapshot.anchorUtcMs = now;

    // 切歌时媒体属性先于时间线到达：此刻 snapshot 里的 positionMs/anchorUtcMs 仍是
    // 上一首歌的时间线残留，按它插值会把进度推到旧歌曲位置，新歌词就绪瞬间会
    // 高亮到中间某行。切歌后进入 awaitingTimeline_ 状态：后续 refreshAll 重读到的
    // 残留进度一律压回 0，直到新曲目的首份时间线到达。
    const std::wstring key = positionTrackKey(snapshot);
    if (posInit_ && posTrackKey_ != key) {
        residualPosMs_ = snapshot.positionMs; // 记录残留位置，供识别旧时间线
        residualAtMs_ = now;
        awaitingTimeline_ = true;
    }
    if (awaitingTimeline_) {
        if (isResidualTimeline(snapshot.positionMs, now)) {
            snapshot.positionMs = 0;
            snapshot.anchorUtcMs = now;
            snapshot.timelineStale = true;
        } else {
            // refreshAll 已经读到新曲目的时间线，恢复常轨
            awaitingTimeline_ = false;
            residualPosMs_ = -1;
            snapshot.timelineStale = false;
        }
    }
}

bool QqMusicSmtcAdapter::isStaleTimelineUpdate(
    const SmtcSnapshot& snapshot, int64_t newPosMs, int64_t newAnchorMs,
    PlaybackStatus status, int64_t eventNowMs) const {
    if (status != PlaybackStatus::Playing || snapshot.durationMs <= 0)
        return false;
    int64_t current = snapshot.positionMs +
                      std::max<int64_t>(0, eventNowMs - snapshot.anchorUtcMs);
    int64_t incoming = newPosMs +
                       std::max<int64_t>(0, eventNowMs - newAnchorMs);
    int64_t backward = current - incoming;
    return backward > 250 && backward < 2000;
}

void QqMusicSmtcAdapter::refreshTimeline(const Session& session,
                                         SmtcSnapshot& snapshot,
                                         int64_t eventNowMs) {
    auto timeline = session.GetTimelineProperties();
    if (!timeline)
        return;

    int64_t newPos = timeSpanMs(timeline.Position());
    // 旧歌残留的迟到事件：duration/position 整体属于上一首歌，全部丢弃。
    if (isResidualTimeline(newPos, eventNowMs)) {
        snapshot.timelineStale = true;
        return;
    }

    snapshot.durationMs = timeSpanMs(timeline.EndTime());
    int64_t newAnchor = lastUpdatedMs(timeline.LastUpdatedTime());
    if (newAnchor == 0)
        newAnchor = eventNowMs;

    // QQ 可能用当前时间包装旧位置，先将过旧锚点按事件时刻重新锚定。
    if (eventNowMs - newAnchor > 2000)
        newAnchor = eventNowMs;

    if (awaitingTimeline_) {
        // 新曲目的第一份时间线：这是等待已久的真实锚点，无条件接受。
        // 不能走下面的回退守卫——从旧歌位置回到 0 附近的大幅回退会被误杀，
        // 导致进度在旧歌位置多停两秒才跳回（歌词先高亮中间行再跳回首行）。
        awaitingTimeline_ = false;
        residualPosMs_ = -1;
        snapshot.positionMs = newPos;
        snapshot.anchorUtcMs = newAnchor;
        snapshot.timelineStale = false;
        return;
    }

    int64_t currentPosNow = snapshot.positionMs +
                            std::max<int64_t>(0, eventNowMs - snapshot.anchorUtcMs);
    int64_t incomingPosNow = newPos +
                             std::max<int64_t>(0, eventNowMs - newAnchor);
    int64_t backwardMs = currentPosNow - incomingPosNow;
    int64_t statusAge = lastStatusChangeMs_ > 0
                            ? eventNowMs - lastStatusChangeMs_
                            : -1;
    bool dropResidual = snapshot.status == PlaybackStatus::Playing &&
                        statusAge >= 0 && statusAge < 2000 && backwardMs > 250;
    if (!dropResidual &&
        !isStaleTimelineUpdate(snapshot, newPos, newAnchor, snapshot.status,
                               eventNowMs)) {
        snapshot.positionMs = newPos;
        snapshot.anchorUtcMs = newAnchor;
    }
}

void QqMusicSmtcAdapter::refreshPlayback(const Session& session,
                                         SmtcSnapshot& snapshot,
                                         int64_t eventNowMs) {
    auto info = session.GetPlaybackInfo();
    if (!info)
        return;

    auto newStatus = mapStatus(info.PlaybackStatus());
    if (newStatus != snapshot.status)
        lastStatusChangeMs_ = eventNowMs;
    const PlaybackStatus previousStatus = snapshot.status;
    bool resumed = newStatus == PlaybackStatus::Playing &&
                   previousStatus != PlaybackStatus::Playing;

    // 播放状态独立于时间线；时间线暂时为空时也必须提交暂停状态，
    // 否则 snapshot() 会继续按 Playing 插值。
    snapshot.status = newStatus;
    bool staleUpdate = false;
    bool smoothUsed = false;
    int64_t newPos = 0;
    auto timeline = session.GetTimelineProperties();
    if (timeline && isResidualTimeline(timeSpanMs(timeline.Position()), eventNowMs)) {
        timeline = nullptr; // 残留事件只更新播放状态/控件，不采用其位置
        snapshot.timelineStale = true;
    }
    if (timeline) {
        newPos = timeSpanMs(timeline.Position());
        int64_t newAnchor = lastUpdatedMs(timeline.LastUpdatedTime());
        if (newAnchor == 0)
            newAnchor = eventNowMs;
        if (eventNowMs - newAnchor > 2000)
            newAnchor = eventNowMs;

        if (awaitingTimeline_) {
            // 与 refreshTimeline 相同：新曲目首份时间线无条件接受
            awaitingTimeline_ = false;
            residualPosMs_ = -1;
            snapshot.positionMs = newPos;
            snapshot.anchorUtcMs = newAnchor;
            snapshot.timelineStale = false;
        } else if (resumed) {
            std::wstring key = positionTrackKey(snapshot);
            smoothUsed = posInit_ && posTrackKey_ == key;
            snapshot.positionMs = smoothUsed ? (int64_t)(smoothPos_ + 0.5) : newPos;
            if (snapshot.durationMs > 0 && snapshot.positionMs > snapshot.durationMs)
                snapshot.positionMs = snapshot.durationMs;
            snapshot.anchorUtcMs = eventNowMs;
        } else {
            staleUpdate = isStaleTimelineUpdate(snapshot, newPos, newAnchor,
                                                previousStatus, eventNowMs);
            if (!staleUpdate) {
                snapshot.positionMs = newPos;
                snapshot.anchorUtcMs = newAnchor;
            }
        }
    } else if (resumed) {
        std::wstring key = positionTrackKey(snapshot);
        smoothUsed = posInit_ && posTrackKey_ == key;
        if (smoothUsed)
            snapshot.positionMs = (int64_t)(smoothPos_ + 0.5);
        if (snapshot.durationMs > 0 && snapshot.positionMs > snapshot.durationMs)
            snapshot.positionMs = snapshot.durationMs;
        snapshot.anchorUtcMs = eventNowMs;
    }

    auto controls = info.Controls();
    if (controls) {
        snapshot.canPrev = controls.IsPreviousEnabled();
        snapshot.canPlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
        snapshot.canNext = controls.IsNextEnabled();
    }
}

SmtcSnapshot QqMusicSmtcAdapter::snapshot(const SmtcSnapshot& source,
                                          int64_t nowMs) const {
    SmtcSnapshot result = source;
    const int64_t rawPos = result.positionMs;
    if (result.status == PlaybackStatus::Playing) {
        int64_t elapsed = nowMs - result.anchorUtcMs;
        if (elapsed > 0) {
            result.positionMs += elapsed;
            if (result.durationMs > 0 && result.positionMs > result.durationMs)
                result.positionMs = result.durationMs;
        }
    }

    std::wstring key = positionTrackKey(result);
    auto now = std::chrono::steady_clock::now();
    float dt = 16.7f;
    if (posTick_ != std::chrono::steady_clock::time_point{})
        dt = std::chrono::duration<float, std::milli>(now - posTick_).count();
    posTick_ = now;

    bool smoothReset = false;
    bool seekWhilePaused = false;
    if (result.status != PlaybackStatus::Playing) {
        double rawJump = (double)(rawPos - prevRawMs_);
        seekWhilePaused = prevPaused_ && std::fabs(rawJump) > 250.0;
        if (!posInit_ || posTrackKey_ != key || seekWhilePaused)
            smoothReset = true;
        if (smoothReset)
            smoothPos_ = (double)result.positionMs;
        posTrackKey_ = key;
        posInit_ = true;
        prevRawMs_ = rawPos;
        prevPaused_ = true;
        int64_t paused = (int64_t)(smoothPos_ + 0.5);
        if (result.durationMs > 0 && paused > result.durationMs)
            paused = result.durationMs;
        result.positionMs = std::max<int64_t>(paused, 0);
        return result;
    }

    if (!posInit_ || posTrackKey_ != key) {
        smoothReset = true;
        smoothPos_ = (double)result.positionMs;
        posTrackKey_ = key;
        posInit_ = true;
    } else {
        double error = (double)result.positionMs - smoothPos_;
        if (std::fabs(error) > 800.0) {
            smoothPos_ = (double)result.positionMs;
        } else {
            double previous = smoothPos_;
            smoothPos_ += dt;
            smoothPos_ += error * (1.0 - std::exp(-dt / 2000.0));
            if (smoothPos_ < previous)
                smoothPos_ = previous;
        }
    }

    int64_t smoothed = (int64_t)(smoothPos_ + 0.5);
    if (result.durationMs > 0 && smoothed > result.durationMs)
        smoothed = result.durationMs;
    if (smoothed < 0)
        smoothed = 0;
    result.positionMs = smoothed;
    prevRawMs_ = rawPos;
    prevPaused_ = false;
    return result;
}

void QqMusicSmtcAdapter::reset() {
    smoothPos_ = 0.0;
    posTick_ = {};
    posInit_ = false;
    posTrackKey_.clear();
    prevRawMs_ = 0;
    prevPaused_ = false;
    awaitingTimeline_ = false;
    residualPosMs_ = -1;
    residualAtMs_ = 0;
    lastStatusChangeMs_ = 0;
}

} // namespace smtc
