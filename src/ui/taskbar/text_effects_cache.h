#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <array>
#include <cstdint>

// 歌词光晕/描边的双槽 GPU 缓存，独立管理资源与失效代次。
class LyricTextEffectsCache {
public:
    LyricTextEffectsCache() = default;
    ~LyricTextEffectsCache() { clear(); }
    LyricTextEffectsCache(const LyricTextEffectsCache&) = delete;
    LyricTextEffectsCache& operator=(const LyricTextEffectsCache&) = delete;
    void clear();
    void invalidate() { ++textFxGen_; }
    // 成功时返回持有一份引用的 bitmap，调用方负责 Release。
    ID2D1Bitmap* bitmap(ID2D1RenderTarget* target, UINT dpi, IDWriteTextLayout* layout, float textW,
                        float textH, ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow);

private:
    struct TextFxCacheEntry {
        ID2D1BitmapRenderTarget* target = nullptr;
        const IDWriteTextLayout* layout = nullptr;
        ID2D1Brush* brush = nullptr;
        ID2D1Brush* outline = nullptr;
        ID2D1Brush* glow = nullptr;
        uint64_t generation = 0;
        uint64_t lastUse = 0;
        UINT dpi = 0;
        float w = 0.0f;
        float h = 0.0f;
        float pad = 0.0f;
    };
    std::array<TextFxCacheEntry, 2> textFxCaches_{};
    uint64_t textFxUse_ = 0;
    uint64_t textFxGen_ = 0;
};
