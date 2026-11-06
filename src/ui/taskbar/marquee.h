#pragma once
#include <algorithm>
#include <cmath>

namespace taskbar_detail {
// 时间差由各自的调用时钟提供；返回本次是否需要继续绘制。
inline bool advanceMarquee(float textWidth, float areaWidth, float speed, float elapsedSeconds,
                           float loopGap, bool enabled, float& offset) {
    if (!enabled || textWidth <= areaWidth || areaWidth <= 0.0f) {
        const bool changed = offset != 0.0f;
        offset = 0.0f;
        return changed;
    }
    const float width = textWidth + loopGap;
    offset = std::fmod(offset + speed * std::max(elapsedSeconds, 0.0f), width);
    if (offset < 0.0f)
        offset += width;
    return true;
}
} // namespace taskbar_detail
