#pragma once

#include "media/smtc_monitor.h"

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

} // namespace smtc
