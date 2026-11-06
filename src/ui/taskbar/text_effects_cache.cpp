#include "ui/taskbar/text_effects_cache.h"

void LyricTextEffectsCache::clear() {
    for (auto& entry : textFxCaches_) {
        if (entry.target)
            entry.target->Release();
        entry = {};
    }
    textFxUse_ = 0;
}

ID2D1Bitmap* LyricTextEffectsCache::bitmap(ID2D1RenderTarget* rt, UINT dpi,
                                           IDWriteTextLayout* layout, float textW, float textH,
                                           ID2D1Brush* brush, ID2D1Brush* outline,
                                           ID2D1Brush* glow) {
    if (!rt || !layout || !brush)
        return nullptr;
    constexpr float pad = 3.0f; // 覆盖 2.4 DIP 的光晕外扩和边缘抗锯齿
    const float wDip = textW + pad * 2.0f;
    const float hDip = textH + pad * 2.0f;
    TextFxCacheEntry* entry = nullptr;
    for (auto& candidate : textFxCaches_) {
        if (candidate.target && candidate.layout == layout && candidate.brush == brush &&
            candidate.outline == outline && candidate.glow == glow &&
            candidate.generation == textFxGen_ && candidate.dpi == dpi && candidate.w == wDip &&
            candidate.h == hDip) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        entry = &textFxCaches_[0];
        if (textFxCaches_[1].lastUse < entry->lastUse)
            entry = &textFxCaches_[1];
        if (entry->target)
            entry->target->Release();
        *entry = {};
        if (FAILED(rt->CreateCompatibleRenderTarget(D2D1::SizeF(wDip, hDip), &entry->target)))
            return nullptr;
        entry->target->SetAntialiasMode(rt->GetAntialiasMode());
        entry->target->SetTextAntialiasMode(rt->GetTextAntialiasMode());
        static constexpr float kDirs[8][2] = {
            {1.0f, 0.0f},  {0.7071f, 0.7071f},   {0.0f, 1.0f},  {-0.7071f, 0.7071f},
            {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}};
        entry->target->BeginDraw();
        entry->target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        if (glow) {
            for (auto& d : kDirs)
                entry->target->DrawTextLayout(D2D1::Point2F(pad + d[0] * 2.4f, pad + d[1] * 2.4f),
                                              layout, glow);
        }
        if (outline) {
            for (auto& d : kDirs)
                entry->target->DrawTextLayout(D2D1::Point2F(pad + d[0] * 1.2f, pad + d[1] * 1.2f),
                                              layout, outline);
        }
        entry->target->DrawTextLayout(D2D1::Point2F(pad, pad), layout, brush);
        if (FAILED(entry->target->EndDraw())) {
            entry->target->Release();
            *entry = {};
            return nullptr;
        }
        entry->layout = layout;
        entry->brush = brush;
        entry->outline = outline;
        entry->glow = glow;
        entry->generation = textFxGen_;
        entry->dpi = dpi;
        entry->w = wDip;
        entry->h = hDip;
        entry->pad = pad;
    }
    entry->lastUse = ++textFxUse_;
    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(entry->target->GetBitmap(&bmp)) || !bmp)
        return nullptr;
    return bmp;
}
