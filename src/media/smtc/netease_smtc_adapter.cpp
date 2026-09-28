#include "media/smtc/netease_smtc_adapter.h"

#include "media/smtc/smtc_common.h"

#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cwchar>
#include <utility>

namespace smtc {

namespace {

constexpr wchar_t kGenrePrefix[] = L"NCM-";
constexpr wchar_t kCloudMusicAppId[] = L"cloudmusic.exe";
constexpr wchar_t kNeteaseBridgeAppId[] = L"NeteaseBridge.exe";
constexpr int64_t kTimelinePositionToleranceMs = 250;
constexpr int64_t kRecentStatusChangeWindowMs = 2000;
constexpr int64_t kMaxStatusTransitionTimelineLagMs = 2000;

bool isNeteaseSource(const std::wstring& sourceAppUserModelId) {
    return _wcsicmp(sourceAppUserModelId.c_str(), kCloudMusicAppId) == 0 ||
           _wcsicmp(sourceAppUserModelId.c_str(), kNeteaseBridgeAppId) == 0;
}

bool isStalePositionRegression(int64_t positionMs, int64_t anchorUtcMs,
                               const SmtcSnapshot& snapshot) {
    return positionMs < snapshot.positionMs && snapshot.anchorUtcMs > 0 &&
           anchorUtcMs > 0 && anchorUtcMs <= snapshot.anchorUtcMs;
}

int64_t positionAtEventMs(int64_t positionMs, int64_t anchorUtcMs,
                          int64_t eventNowMs, PlaybackStatus status,
                          int64_t durationMs) {
    if (status == PlaybackStatus::Playing && anchorUtcMs > 0)
        positionMs += std::max<int64_t>(0, eventNowMs - anchorUtcMs);
    if (durationMs > 0)
        positionMs = std::min(positionMs, durationMs);
    return std::max<int64_t>(positionMs, 0);
}

} // namespace

SmtcPlayerType NeteaseSmtcAdapter::playerType() const noexcept {
    return SmtcPlayerType::NetEase;
}

bool NeteaseSmtcAdapter::parseSongId(const std::wstring& genre,
                                     std::wstring& songId) {
    if (genre.rfind(kGenrePrefix, 0) != 0)
        return false;
    std::wstring value = genre.substr(std::wcslen(kGenrePrefix));
    if (value.empty())
        return false;
    for (wchar_t ch : value) {
        if (ch < L'0' || ch > L'9')
            return false;
    }
    songId = std::move(value);
    return true;
}

SmtcSessionIdentity NeteaseSmtcAdapter::identifySession(const Session& session) const {
    SmtcSessionIdentity identity;
    if (!session)
        return identity;

    try {
        identity.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
        // 只对网易云自身/桥接器会话读取 Genres。对所有 SMTC 会话都调用
        // TryGetMediaPropertiesAsync() 会触发 QQ、浏览器或已失效会话的
        // HRESULT，尤其是在当前没有播放歌曲时更容易出现。
        if (!isNeteaseSource(identity.sourceAppUserModelId))
            return identity;
        auto info = session.GetPlaybackInfo();
        if (!info)
            return identity;
        const PlaybackStatus status = mapStatus(info.PlaybackStatus());
        if (status == PlaybackStatus::Stopped || status == PlaybackStatus::Other)
            return identity;
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
            if (parseSongId(genres.GetAt(i).c_str(), songId)) {
                identity.player = SmtcPlayerType::NetEase;
                identity.neteaseSongId = std::move(songId);
                identity.enhancedSmtc = true;
                break;
            }
        }
    } catch (...) {
    }
    return identity;
}

void NeteaseSmtcAdapter::prepareInitialSnapshot(SmtcSnapshot&) const {}

void NeteaseSmtcAdapter::refreshTimeline(const Session& session,
                                         SmtcSnapshot& snapshot,
                                         int64_t eventNowMs) {
    auto timeline = session.GetTimelineProperties();
    if (!timeline)
        return;

    const int64_t durationMs = timeSpanMs(timeline.EndTime());
    const int64_t positionMs = timeSpanMs(timeline.Position());
    const int64_t reportedAnchorMs = lastUpdatedMs(timeline.LastUpdatedTime());
    const bool placeholderZero = reportedAnchorMs == 0 && positionMs == 0 &&
                                 snapshot.positionMs > 0;
    const int64_t incomingAnchorMs = reportedAnchorMs > 0 ? reportedAnchorMs : eventNowMs;
    const int64_t currentAtEventMs = positionAtEventMs(
        snapshot.positionMs, snapshot.anchorUtcMs, eventNowMs, snapshot.status,
        snapshot.durationMs);
    const int64_t incomingAtEventMs = positionAtEventMs(
        positionMs, incomingAnchorMs, eventNowMs, snapshot.status, durationMs);
    const int64_t positionDeltaMs = incomingAtEventMs - currentAtEventMs;
    const bool positionDiscontinuity =
        positionDeltaMs > kTimelinePositionToleranceMs ||
        positionDeltaMs < -kTimelinePositionToleranceMs;
    const int64_t statusAgeMs = lastStatusChangeMs_ > 0
                                    ? eventNowMs - lastStatusChangeMs_
                                    : -1;
    const int64_t rawBackwardMs = snapshot.positionMs - positionMs;
    const bool staleTransitionTimeline =
        statusAgeMs >= 0 && statusAgeMs < kRecentStatusChangeWindowMs &&
        reportedAnchorMs > 0 && reportedAnchorMs < lastStatusChangeMs_ &&
        rawBackwardMs > kTimelinePositionToleranceMs &&
        rawBackwardMs <= kMaxStatusTransitionTimelineLagMs &&
        !positionDiscontinuity;
    const bool stalePositionRegression =
        isStalePositionRegression(positionMs, reportedAnchorMs, snapshot);
    const bool playingSeek = positionDiscontinuity &&
                             snapshot.status == PlaybackStatus::Playing;
    // 状态切换后的旧采样继续丢弃；播放中的 seek 不能沿用跳变前的锚点。
    if (placeholderZero || staleTransitionTimeline ||
        (stalePositionRegression && !playingSeek))
        return;

    snapshot.durationMs = durationMs;
    snapshot.positionMs = positionMs;
    snapshot.anchorUtcMs = positionDiscontinuity ? eventNowMs : reportedAnchorMs;
    if (snapshot.anchorUtcMs == 0)
        snapshot.anchorUtcMs = eventNowMs;
}

void NeteaseSmtcAdapter::refreshPlayback(const Session& session,
                                         SmtcSnapshot& snapshot,
                                         int64_t eventNowMs) {
    auto info = session.GetPlaybackInfo();
    if (!info)
        return;

    const PlaybackStatus previousStatus = snapshot.status;
    const PlaybackStatus newStatus = mapStatus(info.PlaybackStatus());
    const bool wasPlaying = previousStatus == PlaybackStatus::Playing;
    const bool nowPlaying = newStatus == PlaybackStatus::Playing;
    const bool leavingPlaying = wasPlaying && !nowPlaying;
    const bool repeatedPlaying = wasPlaying && nowPlaying;
    if (newStatus != previousStatus)
        lastStatusChangeMs_ = eventNowMs;

    // PlaybackInfoChanged 到达时，Position 可能还是上一条时间线的采样值；
    // 暂停时先按旧锚点换算到事件时刻，避免歌词回退到旧采样点。
    int64_t effectiveOldPos = snapshot.positionMs;
    if (wasPlaying && snapshot.anchorUtcMs > 0)
        effectiveOldPos += std::max<int64_t>(0, eventNowMs - snapshot.anchorUtcMs);
    if (snapshot.durationMs > 0 && effectiveOldPos > snapshot.durationMs)
        effectiveOldPos = snapshot.durationMs;
    effectiveOldPos = std::max<int64_t>(effectiveOldPos, 0);
    snapshot.status = newStatus;

    bool anchorMissing = false;
    bool placeholderZero = false;
    bool stalePositionRegression = false;
    int64_t newPos = snapshot.positionMs;
    int64_t rawAnchorAge = -1;
    auto timeline = session.GetTimelineProperties();
    if (timeline) {
        snapshot.durationMs = timeSpanMs(timeline.EndTime());
        newPos = timeSpanMs(timeline.Position());
        int64_t reportedAnchor = lastUpdatedMs(timeline.LastUpdatedTime());
        anchorMissing = reportedAnchor == 0;
        rawAnchorAge = anchorMissing ? -1 : eventNowMs - reportedAnchor;

        // 网易云在状态切换通知中会先返回 Position=0、LastUpdatedTime=0 的占位数据，
        // 下一条通知才是实际位置；该数据不能让歌词回到开头。
        placeholderZero = anchorMissing && newPos == 0 && snapshot.positionMs > 0;

        // 状态切换附近的重复通知可能携带旧采样值。时间锚点未推进且位置倒退，或暂停采样明显过期
        // 且位置倒退时保留已有位置；恢复播放时从当前事件时刻重新开始插值。
        stalePositionRegression = !anchorMissing && newPos < snapshot.positionMs &&
                                  (isStalePositionRegression(newPos, reportedAnchor,
                                                             snapshot) ||
                                   (!nowPlaying && rawAnchorAge > 250));
        if (repeatedPlaying) {
            // 播放中的进度只由独立 TimelinePropertiesChanged 推进。
        } else if (leavingPlaying) {
            snapshot.positionMs = effectiveOldPos;
            snapshot.anchorUtcMs = eventNowMs;
        } else if (!placeholderZero && !stalePositionRegression) {
            snapshot.positionMs = newPos;
            // 状态事件携带的 Position 可能早于事件到达，使用事件时刻重新锚定。
            snapshot.anchorUtcMs = eventNowMs;
        } else if (placeholderZero || stalePositionRegression) {
            snapshot.anchorUtcMs = eventNowMs;
        }
    } else if (leavingPlaying) {
        snapshot.positionMs = effectiveOldPos;
        snapshot.anchorUtcMs = eventNowMs;
    }

    applyPlaybackControls(info, snapshot);
}

SmtcSnapshot NeteaseSmtcAdapter::snapshot(const SmtcSnapshot& source,
                                          int64_t nowMs) const {
    SmtcSnapshot result = source;
    advancePlayingPosition(result, nowMs, true);
    return result;
}

void NeteaseSmtcAdapter::reset() {
    lastStatusChangeMs_ = 0;
}

} // namespace smtc
