#pragma once
#include "ui/lyric_window.h"
#include <algorithm>

namespace media_popup_detail {
enum class Page { Media, Idle };

// 终点层会在交换链提交期间暂留，因此 Retained 与“仍在动画”明确分开。
class LayerTransition {
public:
    enum class Phase { None, Pending, Composited, Retained };
    bool active() const { return phase_ == Phase::Pending || phase_ == Phase::Composited; }
    bool hasLayers() const { return phase_ == Phase::Composited || phase_ == Phase::Retained; }
    void begin() { phase_ = Phase::Pending; }
    void markComposited() { phase_ = Phase::Composited; }
    void retainFinal() { phase_ = hasLayers() ? Phase::Retained : Phase::None; }
    void reset() { phase_ = Phase::None; }

private:
    Phase phase_ = Phase::None;
};

struct PageSnapshot {
    Page from = Page::Media;
    IdlePresentation idle;
    OverlayMediaInfo media;
    float titleScrollOffset = 0.0f;
    float artistScrollOffset = 0.0f;
    float idleQuoteScrollOffset = 0.0f;
    float idleQuoteSourceScrollOffset = 0.0f;
    float idleScrollOffset = 0.0f;
    int direction = 1;
};

struct IdleContentTransition {
    bool active = false;
    IdlePresentation from;
    ULONGLONG startMs = 0;
    float progress(ULONGLONG now, float durationMs) const {
        if (!active || startMs == 0)
            return 1.0f;
        const float linear = std::clamp(
            static_cast<float>(now >= startMs ? now - startMs : 0) / durationMs, 0.0f, 1.0f);
        return linear * linear * (3.0f - 2.0f * linear);
    }
};

struct QuickExpansion {
    bool expanded = false;
    bool opening = false;
    float progress = 0.0f;
};
} // namespace media_popup_detail
