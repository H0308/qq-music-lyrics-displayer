#pragma once

#include <cstdint>

enum class LyricEffect { Stardust, FollowLight, Underline, Ripple };
constexpr int kLyricEffectCount = 4;
constexpr uint32_t kLyricEffectsMask = (1u << kLyricEffectCount) - 1u;
constexpr uint32_t lyricEffectBit(LyricEffect effect) {
    return 1u << static_cast<unsigned>(effect);
}
