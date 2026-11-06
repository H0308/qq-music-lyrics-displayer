#include "graphics/pixel_luminance.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace graphics {
namespace {
float srgbChannelToLinear(uint8_t value) {
    const float channel = static_cast<float>(value) / 255.0f;
    return channel <= 0.04045f ? channel / 12.92f
                              : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}
} // namespace

float backdropLuminance(const void* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0)
        return 0.0f;
    const auto* bytes = static_cast<const uint8_t*>(pixels);
    const int step = std::max(1, std::min(width, height) / 96);
    double total = 0.0;
    int samples = 0;
    for (int y = 0; y < height; y += step) {
        for (int x = 0; x < width; x += step) {
            const auto* pixel = bytes + (static_cast<size_t>(y) * width + x) * 4;
            const float blue = srgbChannelToLinear(pixel[0]);
            const float green = srgbChannelToLinear(pixel[1]);
            const float red = srgbChannelToLinear(pixel[2]);
            total += 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            ++samples;
        }
    }
    return samples > 0 ? static_cast<float>(total / samples) : 0.0f;
}
} // namespace graphics
