#include "ui/taskbar/karaoke_tracker.h"
#include <algorithm>
#include <cmath>

void KaraokeTracker::build(const std::vector<LyricLine>& lines, int lineIndex,
                           IDWriteTextLayout* layout) {
    spans_.clear();
    geometryLine_ = -1;
    geometryLayout_ = nullptr;
    if (lineIndex < 0 || (size_t)lineIndex >= lines.size() || !layout)
        return;

    const LyricLine& line = lines[(size_t)lineIndex];
    if (line.chars.empty())
        return;

    UINT32 textOffset = 0;
    spans_.reserve(line.chars.size());
    for (const LyricChar& c : line.chars) {
        const UINT32 textLength = static_cast<UINT32>(c.text.size());
        if (textLength == 0)
            continue;

        DWRITE_HIT_TEST_METRICS metrics{};
        float startX = 0.0f;
        float y = 0.0f;
        if (FAILED(layout->HitTestTextPosition(textOffset, FALSE, &startX, &y, &metrics))) {
            textOffset += textLength;
            continue;
        }

        float endX = startX;
        if (FAILED(layout->HitTestTextPosition(textOffset + textLength - 1, TRUE, &endX, &y,
                                               &metrics)))
            endX = startX;
        spans_.push_back({c.startMs, c.endMs, startX, endX});
        textOffset += textLength;
    }
    geometryLine_ = lineIndex;
    geometryLayout_ = layout;
}

float KaraokeTracker::target(int currentLine, IDWriteTextLayout* layout, int64_t positionMs,
                             int64_t& durOut) const {
    durOut = 0;
    if (geometryLine_ != currentLine || geometryLayout_ != layout || spans_.empty())
        return 0.0f;

    auto it = std::upper_bound(
        spans_.begin(), spans_.end(), positionMs,
        [](int64_t position, const Span& span) { return position < span.startMs; });
    if (it == spans_.begin())
        return 0.0f;
    --it;

    durOut = std::max<int64_t>(it->endMs - it->startMs, 1);
    float frac = (float)std::clamp((double)(positionMs - it->startMs) / (double)durOut, 0.0, 1.0);
    return it->startX + (it->endX - it->startX) * frac;
}

float KaraokeTracker::advance(int currentLine, IDWriteTextLayout* layout, int64_t positionMs,
                              bool animations, bool transitioning, ULONGLONG now) {
    int64_t charDur = 0;
    float target = this->target(currentLine, layout, positionMs, charDur);
    if (!animations) {
        tick_ = now;
        line_ = currentLine;
        smooth_ = target;
        progress_ = target;
        settled_ = true;
        entering_ = false;
        return target;
    }
    float dt = tick_ ? (float)(now - tick_) : 16.7f;
    tick_ = now;
    float gap = target - smooth_;
    const bool lineChanged = line_ != currentLine;
    if (lineChanged) {
        line_ = currentLine;
        entering_ = transitioning;
        smooth_ = entering_ ? 0.0f : target;
    } else if (std::fabs(gap) > 100.0f && !entering_) {
        smooth_ = target;
    } else {
        float tau = std::clamp((float)charDur * 0.25f, 40.0f, 200.0f);
        float alpha = 1.0f - std::exp(-dt / tau);
        smooth_ += gap * alpha;
    }
    progress_ = smooth_;
    settled_ = std::fabs(target - smooth_) < 0.5f;
    if (settled_ && !transitioning)
        entering_ = false;
    return progress_;
}

void KaraokeTracker::clearGeometry() {
    spans_.clear();
    geometryLine_ = -1;
    geometryLayout_ = nullptr;
}
void KaraokeTracker::reset() {
    clearGeometry();
    progress_ = smooth_ = 0.0f;
    line_ = -1;
    entering_ = false;
    tick_ = 0;
    settled_ = true;
}
void KaraokeTracker::retarget(ULONGLONG now) {
    line_ = -1;
    settled_ = false;
    tick_ = now;
    entering_ = false;
}
void KaraokeTracker::finishTransition(int line, IDWriteTextLayout* layout, int64_t position,
                                      bool hasKaraoke, ULONGLONG now) {
    tick_ = now;
    const bool preserve = entering_ && line_ == line;
    line_ = line;
    if (hasKaraoke) {
        int64_t duration = 0;
        const float next = target(line, layout, position, duration);
        if (preserve) {
            progress_ = smooth_;
            settled_ = std::fabs(next - smooth_) < 0.5f;
        } else {
            smooth_ = progress_ = next;
            settled_ = true;
            entering_ = false;
        }
    } else {
        smooth_ = progress_ = 0.0f;
        settled_ = true;
        entering_ = false;
    }
}
