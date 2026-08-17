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

bool isNeteaseSource(const std::wstring& sourceAppUserModelId) {
    return _wcsicmp(sourceAppUserModelId.c_str(), kCloudMusicAppId) == 0 ||
           _wcsicmp(sourceAppUserModelId.c_str(), kNeteaseBridgeAppId) == 0;
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

    snapshot.durationMs = timeSpanMs(timeline.EndTime());
    snapshot.positionMs = timeSpanMs(timeline.Position());
    snapshot.anchorUtcMs = lastUpdatedMs(timeline.LastUpdatedTime());
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
    bool stalePausedPosition = false;
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

        // 暂停后的重复状态事件可能携带暂停前的旧采样值，不能覆盖刚冻结的位置。
        stalePausedPosition = !nowPlaying && !anchorMissing &&
                              rawAnchorAge > 250 && newPos < snapshot.positionMs;
        if (repeatedPlaying) {
            // 播放中的进度只由独立 TimelinePropertiesChanged 推进。
        } else if (leavingPlaying) {
            snapshot.positionMs = effectiveOldPos;
            snapshot.anchorUtcMs = eventNowMs;
        } else if (!placeholderZero && !stalePausedPosition) {
            snapshot.positionMs = newPos;
            // 状态事件携带的 Position 可能早于事件到达，使用事件时刻重新锚定。
            snapshot.anchorUtcMs = eventNowMs;
        } else if (placeholderZero) {
            snapshot.anchorUtcMs = eventNowMs;
        }
    } else if (leavingPlaying) {
        snapshot.positionMs = effectiveOldPos;
        snapshot.anchorUtcMs = eventNowMs;
    }

    auto controls = info.Controls();
    if (controls) {
        snapshot.canPrev = controls.IsPreviousEnabled();
        snapshot.canPlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
        snapshot.canNext = controls.IsNextEnabled();
    }
}

SmtcSnapshot NeteaseSmtcAdapter::snapshot(const SmtcSnapshot& source,
                                          int64_t nowMs) const {
    SmtcSnapshot result = source;
    if (result.status == PlaybackStatus::Playing && result.anchorUtcMs > 0) {
        int64_t elapsed = nowMs - result.anchorUtcMs;
        if (elapsed > 0) {
            result.positionMs += elapsed;
            if (result.durationMs > 0 && result.positionMs > result.durationMs)
                result.positionMs = result.durationMs;
        }
    }
    return result;
}

void NeteaseSmtcAdapter::reset() {}

} // namespace smtc
