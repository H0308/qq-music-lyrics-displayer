#pragma once

#include <cstdint>

// 实际频谱与设置示意图共用同一组连续渐变端点。
constexpr float kSpectrumBarGradientDarkFactor = 0.55f;
constexpr float kSpectrumBarGradientLightMix = 0.45f;

// 位号用于持久化，删除特效时保留其他特效的原有位号。
enum class SpectrumEffect { Glow = 0, Trail = 1, Sparks = 3, Sheen = 4 };

constexpr int kSpectrumEffectCount = 4;
constexpr SpectrumEffect kSpectrumEffects[kSpectrumEffectCount] = {
    SpectrumEffect::Glow, SpectrumEffect::Trail, SpectrumEffect::Sparks, SpectrumEffect::Sheen};
constexpr uint32_t spectrumEffectBit(SpectrumEffect effect) {
    return 1u << static_cast<unsigned>(effect);
}
constexpr uint32_t kSpectrumEffectsMask = spectrumEffectBit(SpectrumEffect::Glow) |
    spectrumEffectBit(SpectrumEffect::Trail) | spectrumEffectBit(SpectrumEffect::Sparks) |
    spectrumEffectBit(SpectrumEffect::Sheen);
