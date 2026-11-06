#pragma once
#include "ui/taskbar/lyric_typesetter.h"
#include "ui/taskbar/lyric_effects_renderer.h"
#include "ui/taskbar/text_effects_cache.h"
#include <d2d1_1.h>
#include <wrl/client.h>

struct LyricTextDrawContext {
    ID2D1DeviceContext* target;
    const LyricTypesetter::Resources* resources;
    LyricEffectsRenderer& effects;
    UINT dpi;
    float fontSize, width, starTextShift;
    int currentLine, outgoingLine;
    uint32_t effectsMask;
};

// 独占文字离屏缓存、渐隐画刷和裁剪层；输入只在当前同步绘制期间借用。
class LyricTextRenderer {
    struct VerticalEdgeFades {
        float outBand = 0.0f;     // 滚出侧区域内渐隐带宽，0 表示不渐隐
        bool outAtTop = false;    // 滚出侧是否在可视区顶部
        bool borrow = false;      // 滚入侧是否启用借位渐隐
        bool borrowAtTop = false; // 滚入侧是否在可视区顶部
    };

public:
    ID2D1Bitmap* textFxBitmap(const LyricTextDrawContext& context, IDWriteTextLayout* layout,
                              float textW, float textH, ID2D1Brush* brush, ID2D1Brush* outline,
                              ID2D1Brush* glow);
    void drawScrollingText(const LyricTextDrawContext& context, IDWriteTextLayout* layout,
                           float textW, float textH, float areaW, float x, float y, float offset,
                           ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
                           ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
                           float karaokeX = 0.0f, float opacity = 1.0f,
                           LyricAlignment alignment = LyricAlignment::Center,
                           bool singleCopy = false, float rightExtend = 0.0f,
                           bool constantEdgeFade = false, float leftFadeDip = 0.0f,
                           float centerAnchor = -1.0f);
    void drawVerticalScrollingText(const LyricTextDrawContext& context, IDWriteTextLayout* layout,
                                   float textW, float textH, float areaH, float x, float y,
                                   float offset, ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
                                   ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
                                   float karaokeX = 0.0f, float opacity = 1.0f,
                                   LyricAlignment alignment = LyricAlignment::Center,
                                   bool singleCopy = false, bool rotated = false,
                                   float topExtend = 0.0f, float bottomExtend = 0.0f,
                                   bool constantEdgeFade = false, bool skipSlotClip = false,
                                   float slotY = -1.0f, int effectLine = -1,
                                   float effectRangeStart = 0.0f, float effectRangeSize = 1.0f,
                                   float effectProgress = -1.0f);
    void drawVerticalLyricParts(const LyricTextDrawContext& context,
                                const std::vector<VerticalLyricPart>& parts, float blockH,
                                float areaH, float railW, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeProgress = 0.0f,
                                float opacity = 1.0f, bool singleCopy = false,
                                float topExtend = 0.0f, float bottomExtend = 0.0f,
                                bool constantEdgeFade = false, float slotY = -1.0f);

    void createDeviceResources(ID2D1DeviceContext* target);
    void discardDeviceResources();
    void invalidate() { textEffectsCache_.invalidate(); }

private:
    ID2D1LinearGradientBrush* ensureLyricEdgeFadeBrush(const LyricTextDrawContext& context);
    ID2D1LinearGradientBrush* lyricEdgeFadeBrush(const LyricTextDrawContext& context, float x,
                                                 float fadeW, float offset);
    ID2D1LinearGradientBrush* ensureLyricRightFadeBrush(const LyricTextDrawContext& context);
    ID2D1LinearGradientBrush* lyricRightFadeBrush(const LyricTextDrawContext& context,
                                                  float rightEdge, float extend);
    VerticalEdgeFades verticalEdgeFades(const LyricTextDrawContext& context, float offset,
                                        float contentH, float areaH, bool karaoke,
                                        bool constantEdgeFade) const;
    ID2D1LinearGradientBrush* verticalOutFadeBrush(const LyricTextDrawContext& context, float edgeY,
                                                   float band, bool atTop);
    ID2D1LinearGradientBrush* verticalBorrowFadeBrush(const LyricTextDrawContext& context,
                                                      float edgeY, float extend, bool atTop);
    LyricTextEffectsCache textEffectsCache_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> lyricEdgeFadeBrush_, lyricRightFadeBrush_;
    Microsoft::WRL::ComPtr<ID2D1Layer> lyricEdgeFadeLayer_, lyricRightFadeLayer_;
};
