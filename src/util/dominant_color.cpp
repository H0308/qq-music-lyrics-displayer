#include "util/dominant_color.h"

// WIN32_LEAN_AND_MEAN 下 windows.h 不含 COM 声明，gdiplus.h 依赖的 IStream/PROPID 需要 objidl.h 提供
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// RGB 各通道量化到 4 bit，共 4096 桶
constexpr int kBucketCount = 4096;

// 作为文字颜色的可读亮度区间（HSL 的 L）：上限压低，提取到浅色时调成同色相稍深的颜色；
// 过暗的颜色也会适当提亮，避免在深/浅任务栏上看不清
constexpr float kMinLightness = 0.35f;
constexpr float kMaxLightness = 0.45f;

void rgbToHsl(int ri, int gi, int bi, float& h, float& s, float& l) {
    const float r = ri / 255.0f, g = gi / 255.0f, b = bi / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) * 0.5f;
    if (mx == mn) {
        h = 0.0f;
        s = 0.0f;
        return;
    }
    const float d = mx - mn;
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r)
        h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g)
        h = (b - r) / d + 2.0f;
    else
        h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

float hueToRgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

COLORREF hslToRgb(float h, float s, float l) {
    float r, g, b;
    if (s == 0.0f) {
        r = g = b = l;
    } else {
        const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        r = hueToRgb(p, q, h + 1.0f / 3.0f);
        g = hueToRgb(p, q, h);
        b = hueToRgb(p, q, h - 1.0f / 3.0f);
    }
    return RGB(static_cast<int>(r * 255.0f + 0.5f), static_cast<int>(g * 255.0f + 0.5f),
               static_cast<int>(b * 255.0f + 0.5f));
}

} // namespace

std::optional<COLORREF> extractDominantColor(const std::vector<uint8_t>& imageBytes) {
    if (imageBytes.empty())
        return std::nullopt;

    HGLOBAL hglobal = GlobalAlloc(GHND, imageBytes.size());
    if (!hglobal)
        return std::nullopt;
    void* ptr = GlobalLock(hglobal);
    if (ptr) {
        memcpy(ptr, imageBytes.data(), imageBytes.size());
        GlobalUnlock(hglobal);
    }
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hglobal, TRUE, &stream)) || !stream) {
        GlobalFree(hglobal);
        return std::nullopt;
    }

    Gdiplus::Bitmap bitmap(stream);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        stream->Release();
        return std::nullopt;
    }
    const UINT w = bitmap.GetWidth(), h = bitmap.GetHeight();
    if (w == 0 || h == 0) {
        stream->Release();
        return std::nullopt;
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok) {
        stream->Release();
        return std::nullopt;
    }

    // 降采样：总量控制在约 64x64，大图也能瞬间统计完
    const UINT stepX = std::max(1u, w / 64);
    const UINT stepY = std::max(1u, h / 64);
    uint32_t buckets[kBucketCount] = {};
    const auto* base = static_cast<const uint8_t*>(data.Scan0);
    for (UINT y = 0; y < h; y += stepY) {
        const uint8_t* row = base + y * data.Stride;
        for (UINT x = 0; x < w; x += stepX) {
            const uint8_t* px = row + x * 4; // BGRA
            const uint8_t b = px[0], g = px[1], r = px[2], a = px[3];
            if (a < 32) // 跳过近透明像素
                continue;
            const int idx = (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
            ++buckets[idx];
        }
    }
    bitmap.UnlockBits(&data);
    stream->Release();

    int best = -1;
    uint32_t bestCount = 0;
    int chromaticBest = -1;
    uint32_t chromaticBestCount = 0;
    int darkChromaticBest = -1;
    uint32_t darkChromaticBestCount = 0;
    for (int i = 0; i < kBucketCount; ++i) {
        if (buckets[i] > bestCount) {
            bestCount = buckets[i];
            best = i;
        }

        // 浅色封面常有大面积白色/灰色背景。优先从有明显色相的桶中
        // 选主色，避免背景数量更多时把粉色、蓝色等封面主色冲掉。
        const int r = (((i >> 8) & 0xF) << 4) + 8;
        const int g = (((i >> 4) & 0xF) << 4) + 8;
        const int b = ((i & 0xF) << 4) + 8;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b});
        if (chroma >= 24 && buckets[i] > chromaticBestCount) {
            chromaticBestCount = buckets[i];
            chromaticBest = i;
        }
        if (chroma >= 24 && buckets[i] > darkChromaticBestCount) {
            float hh = 0.0f, ss = 0.0f, ll = 0.0f;
            rgbToHsl(r, g, b, hh, ss, ll);
            if (ll <= kMaxLightness) {
                darkChromaticBestCount = buckets[i];
                darkChromaticBest = i;
            }
        }
    }
    // 明亮主色不直接压成同色的亮色版本：优先换用封面中更暗的其他有色桶，
    // 这样能避开黄色、荧光绿等刺眼颜色，同时保留专辑的色彩倾向。
    if (darkChromaticBest >= 0)
        best = darkChromaticBest;
    else if (chromaticBest >= 0)
        best = chromaticBest;
    if (best < 0)
        return std::nullopt;

    // 桶中心颜色
    const int r = (((best >> 8) & 0xF) << 4) + 8;
    const int g = (((best >> 4) & 0xF) << 4) + 8;
    const int b = ((best & 0xF) << 4) + 8;

    // 只钳制亮度：色相/饱和度保持封面原色（灰黑封面不会被强行染成彩色）
    float hh, ss, ll;
    rgbToHsl(r, g, b, hh, ss, ll);
    ll = std::clamp(ll, kMinLightness, kMaxLightness);
    return hslToRgb(hh, ss, ll);
}
