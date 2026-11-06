#pragma once
#include "ui/lyric_window.h"
#include <optional>
#include <cstdint>
#include <utility>
enum class LyricTransitionKind {
    None,
    Line,
    Scene,
};
struct LyricTransitionTarget {
    int lineIndex = -1;
    int64_t actualPositionMs = 0;
    int direction = 1;
    uint64_t frameRevision = 0;
};

// 目标行只保留最新请求；布局持有者和帧调度器分别消费状态，不拥有彼此。
class LyricTransition {
public:
    enum class Phase { Idle, Pending, Running };
    enum class Decision { Reset, Queued, Rebuild };
    Decision request(int previous, int next, int64_t position, uint64_t revision, bool animate) {
        if (!animate || previous < 0 || next < 0 || static_cast<int64_t>(next) - previous > 1 ||
            static_cast<int64_t>(previous) - next > 1)
            return Decision::Reset;
        LyricTransitionTarget target{next, position, next > previous ? 1 : -1, revision};
        if (phase_ == Phase::Running) {
            if (target.direction != lyricTransitionDirection_)
                return Decision::Reset;
            pending_ = target;
            lyricTransitionRevision_ = revision;
            return Decision::Queued;
        }
        target_ = target;
        lyricTransitionKind_ = LyricTransitionKind::Line;
        lyricTransitionDirection_ = target.direction;
        lyricTransitionRevision_ = revision;
        phase_ = Phase::Pending;
        return Decision::Rebuild;
    }
    bool advancePending(uint64_t revision) {
        if (!pending_)
            return false;
        target_ = pending_;
        pending_.reset();
        lyricTransitionDirection_ = target_->direction;
        lyricTransitionRevision_ = revision;
        phase_ = Phase::Pending;
        return true;
    }
    Phase phase() const { return phase_; }
    void setPhase(Phase value) { phase_ = value; }
    LyricTransitionKind kind() const { return lyricTransitionKind_; }
    void setKind(LyricTransitionKind value) { lyricTransitionKind_ = value; }
    ULONGLONG startMs() const { return lyricTransitionStartMs_; }
    void setStartMs(ULONGLONG value) { lyricTransitionStartMs_ = value; }
    uint64_t revision() const { return lyricTransitionRevision_; }
    void setRevision(uint64_t value) { lyricTransitionRevision_ = value; }
    int direction() const { return lyricTransitionDirection_; }
    void setDirection(int value) { lyricTransitionDirection_ = value; }
    bool outgoingDoubleLine() const { return outgoingDoubleLine_; }
    void setOutgoingDoubleLine(bool value) { outgoingDoubleLine_ = value; }
    const std::optional<LyricTransitionTarget>& target() const { return target_; }
    const std::optional<LyricTransitionTarget>& pending() const { return pending_; }
    void setTarget(std::optional<LyricTransitionTarget> target) { target_ = std::move(target); }
    void setPending(std::optional<LyricTransitionTarget> target) { pending_ = std::move(target); }
    void clearTarget() { target_.reset(); }
    void clearPending() { pending_.reset(); }

private:
    Phase phase_ = Phase::Idle;
    LyricTransitionKind lyricTransitionKind_ = LyricTransitionKind::None;
    ULONGLONG lyricTransitionStartMs_ = 0;
    uint64_t lyricTransitionRevision_ = 0;
    int lyricTransitionDirection_ = 1;
    bool outgoingDoubleLine_ = false;
    std::optional<LyricTransitionTarget> target_;
    std::optional<LyricTransitionTarget> pending_;
};
