#pragma once

#include <cstdint>

enum class LyricEffect { Stardust, FollowLight, Underline, Ripple, WordWave };
constexpr int kLyricEffectCount = 5;
constexpr uint32_t kLyricEffectsMask = (1u << kLyricEffectCount) - 1u;
constexpr uint32_t lyricEffectBit(LyricEffect effect) {
    return 1u << static_cast<unsigned>(effect);
}

// 只有显式开启互斥效果时才修改另一项；临时显示限制不改变偏好。
constexpr uint32_t withLyricEffect(uint32_t effects, LyricEffect effect, bool enabled) {
    const auto bit = lyricEffectBit(effect);
    if (!enabled)
        return effects & ~bit;
    if (effect == LyricEffect::Stardust)
        effects &= ~lyricEffectBit(LyricEffect::WordWave);
    else if (effect == LyricEffect::WordWave)
        effects &= ~lyricEffectBit(LyricEffect::Stardust);
    return (effects | bit) & kLyricEffectsMask;
}
