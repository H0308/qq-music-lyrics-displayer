#pragma once

#include "media/smtc_monitor.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

namespace smtc {

using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;

int64_t nowUtcMs();
int64_t timeSpanMs(winrt::Windows::Foundation::TimeSpan value);
int64_t lastUpdatedMs(winrt::Windows::Foundation::DateTime value);

bool sameSession(const Session& left, const Session& right) noexcept;

PlaybackStatus mapStatus(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status);

std::shared_ptr<const std::vector<uint8_t>> readThumbnail(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& props);

template <typename PlaybackInfo>
inline void applyPlaybackControls(const PlaybackInfo& info, SmtcSnapshot& snapshot) {
    auto controls = info.Controls();
    if (!controls)
        return;
    snapshot.canPrev = controls.IsPreviousEnabled();
    snapshot.canPlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
    snapshot.canNext = controls.IsNextEnabled();
}

inline void advancePlayingPosition(SmtcSnapshot& snapshot, int64_t nowMs,
                                   bool requireAnchor) {
    if (snapshot.status != PlaybackStatus::Playing ||
        (requireAnchor && snapshot.anchorUtcMs <= 0))
        return;

    const int64_t elapsed = nowMs - snapshot.anchorUtcMs;
    if (elapsed <= 0)
        return;

    snapshot.positionMs += elapsed;
    if (snapshot.durationMs > 0)
        snapshot.positionMs = std::min(snapshot.positionMs, snapshot.durationMs);
}

} // namespace smtc
