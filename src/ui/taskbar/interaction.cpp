#include "ui/taskbar/interaction.h"

float TaskbarPresence::opacity(ULONGLONG now) const {
    if (phase_ == Phase::Shown)
        return 1.0f;
    if (phase_ == Phase::Hidden)
        return 0.0f;
    if (startMs_ == 0)
        return from_;
    const bool hiding = phase_ == Phase::Hiding;
    const float duration = static_cast<float>(hiding ? kHideMs : kShowMs);
    const float t = std::clamp(static_cast<float>(now - startMs_) / duration, 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);
    const float target = hiding ? 0.0f : 1.0f;
    return from_ + (target - from_) * eased;
}

TaskbarPresence::Action TaskbarPresence::advance(ULONGLONG now, bool cursorAway) {
    switch (phase_) {
    case Phase::Hiding:
        return now - startMs_ >= kHideMs ? Action::Hide : Action::None;
    case Phase::Hidden:
        if (!cursorAway)
            awaySince_ = 0;
        else if (awaySince_ == 0)
            awaySince_ = now;
        else if (now - awaySince_ >= 180)
            return Action::Show;
        return Action::None;
    case Phase::Showing:
        if (now - startMs_ < kShowMs)
            return Action::None;
        phase_ = Phase::Shown;
        return Action::StopTimer;
    case Phase::Shown:
        return Action::StopTimer;
    }
    return Action::None;
}
