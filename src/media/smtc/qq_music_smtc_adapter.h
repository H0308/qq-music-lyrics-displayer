#pragma once

#include "media/smtc/smtc_player_adapter.h"

#include <chrono>
#include <string>

namespace smtc {

class QqMusicSmtcAdapter final : public SmtcPlayerAdapter {
public:
    SmtcPlayerType playerType() const noexcept override;
    SmtcSessionIdentity identifySession(const Session& session) const override;
    void prepareInitialSnapshot(SmtcSnapshot& snapshot) const override;
    void refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                         int64_t eventNowMs) override;
    void refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                         int64_t eventNowMs) override;
    SmtcSnapshot snapshot(const SmtcSnapshot& source,
                          int64_t nowUtcMs) const override;
    void reset() override;

private:
    bool isStaleTimelineUpdate(const SmtcSnapshot& snapshot, int64_t newPosMs,
                               int64_t newAnchorMs, PlaybackStatus status,
                               int64_t eventNowMs) const;

    mutable double smoothPos_ = 0.0;
    mutable std::chrono::steady_clock::time_point posTick_{};
    mutable bool posInit_ = false;
    mutable std::wstring posTrackKey_;
    mutable int64_t prevRawMs_ = 0;
    mutable bool prevPaused_ = false;
    int64_t lastStatusChangeMs_ = 0;
};

} // namespace smtc
