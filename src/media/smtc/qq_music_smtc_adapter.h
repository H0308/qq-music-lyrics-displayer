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

    // 切歌后的短暂窗口内，QQ 会补发携带上一首歌时间线的迟到事件（Position 仍停在
    // 旧歌播放点附近），refreshAll 重读媒体属性时也会把这份残留时间线重新写回快照。
    // 切歌时记录被丢弃的残留位置并进入 awaitingTimeline_ 状态：窗口内到达且位置仍
    // 贴着残留点的时间线判定为旧歌残留（压回 0），第一份非残留时间线才是新曲目的
    // 真实锚点，无条件接受并退出该状态。
    bool isResidualTimeline(int64_t newPosMs, int64_t nowMs) const;

    mutable double smoothPos_ = 0.0;
    mutable std::chrono::steady_clock::time_point posTick_{};
    mutable bool posInit_ = false;
    mutable std::wstring posTrackKey_;
    mutable int64_t prevRawMs_ = 0;
    mutable bool prevPaused_ = false;
    mutable bool awaitingTimeline_ = false; // 已切歌、尚未收到新曲目的首份时间线
    mutable int64_t residualPosMs_ = -1; // 最近一次切歌丢弃的旧时间线位置（-1 无残留）
    mutable int64_t residualAtMs_ = 0;   // 残留记录时刻（超出窗口后失效）
    int64_t lastStatusChangeMs_ = 0;
};

} // namespace smtc
