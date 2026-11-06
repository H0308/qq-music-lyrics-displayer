#include "ui/taskbar/lyric_text_renderer.h"
#include "ui/taskbar/layout.h"
#include <algorithm>
#include <cmath>
#include <array>

using taskbar_detail::kTextPadding;
using taskbar_detail::kVerticalLyricGap;
namespace {
constexpr float kLyricEdgeFadeDip = 18.0f;
}
ID2D1Bitmap* LyricTextRenderer::textFxBitmap(const LyricTextDrawContext& context,
                                             IDWriteTextLayout* layout, float textW, float textH,
                                             ID2D1Brush* brush, ID2D1Brush* outline,
                                             ID2D1Brush* glow) {
    return textEffectsCache_.bitmap(context.target, context.dpi, layout, textW, textH, brush,
                                    outline, glow);
}

ID2D1LinearGradientBrush*
LyricTextRenderer::ensureLyricEdgeFadeBrush(const LyricTextDrawContext& context) {
    if (lyricEdgeFadeBrush_.Get())
        return lyricEdgeFadeBrush_.Get();
    auto* rt = context.target;
    if (!rt)
        return nullptr;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)},
    };
    ID2D1GradientStopCollection* stopCollection = nullptr;
    if (FAILED(rt->CreateGradientStopCollection(stops, 2, &stopCollection)))
        return nullptr;
    const HRESULT hr = rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 0.0f)),
        D2D1::BrushProperties(), stopCollection, lyricEdgeFadeBrush_.ReleaseAndGetAddressOf());
    stopCollection->Release();
    return FAILED(hr) ? nullptr : lyricEdgeFadeBrush_.Get();
}

ID2D1LinearGradientBrush* LyricTextRenderer::lyricEdgeFadeBrush(const LyricTextDrawContext& context,
                                                                float x, float fadeW,
                                                                float offset) {
    ID2D1LinearGradientBrush* brush = ensureLyricEdgeFadeBrush(context);
    if (!brush || fadeW <= 0.0f)
        return nullptr;
    const float band = std::min(offset, fadeW);
    if (band <= 0.0f)
        return nullptr;
    brush->SetStartPoint(D2D1::Point2F(x, 0.0f));
    brush->SetEndPoint(D2D1::Point2F(x + band, 0.0f));
    return brush;
}

ID2D1LinearGradientBrush*
LyricTextRenderer::ensureLyricRightFadeBrush(const LyricTextDrawContext& context) {
    if (lyricRightFadeBrush_.Get())
        return lyricRightFadeBrush_.Get();
    auto* rt = context.target;
    if (!rt)
        return nullptr;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)},
        {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
    };
    ID2D1GradientStopCollection* stopCollection = nullptr;
    if (FAILED(rt->CreateGradientStopCollection(stops, 2, &stopCollection)))
        return nullptr;
    const HRESULT hr = rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 0.0f)),
        D2D1::BrushProperties(), stopCollection, lyricRightFadeBrush_.ReleaseAndGetAddressOf());
    stopCollection->Release();
    return FAILED(hr) ? nullptr : lyricRightFadeBrush_.Get();
}

ID2D1LinearGradientBrush*
LyricTextRenderer::lyricRightFadeBrush(const LyricTextDrawContext& context, float rightEdge,
                                       float extend) {
    ID2D1LinearGradientBrush* brush = ensureLyricRightFadeBrush(context);
    if (!brush || extend <= 0.0f)
        return nullptr;
    brush->SetStartPoint(D2D1::Point2F(rightEdge, 0.0f));
    brush->SetEndPoint(D2D1::Point2F(rightEdge + extend, 0.0f));
    return brush;
}

LyricTextRenderer::VerticalEdgeFades
LyricTextRenderer::verticalEdgeFades(const LyricTextDrawContext& context, float offset,
                                     float contentH, float areaH, bool karaoke,
                                     bool constantEdgeFade) const {
    VerticalEdgeFades fades;
    if (contentH <= areaH || areaH <= 0.0f)
        return fades;
    const float fadeH = std::min(kLyricEdgeFadeDip, areaH * 0.25f);
    fades.outAtTop = offset < 0.0f;
    fades.outBand = constantEdgeFade ? fadeH : std::min(std::fabs(offset), fadeH);
    fades.borrow = true;
    fades.borrowAtTop = !karaoke && offset >= 0.0f;
    return fades;
}

ID2D1LinearGradientBrush*
LyricTextRenderer::verticalOutFadeBrush(const LyricTextDrawContext& context, float edgeY,
                                        float band, bool atTop) {
    ID2D1LinearGradientBrush* brush = ensureLyricEdgeFadeBrush(context);
    if (!brush || band <= 0.0f)
        return nullptr;
    brush->SetStartPoint(D2D1::Point2F(0.0f, edgeY));
    brush->SetEndPoint(D2D1::Point2F(0.0f, atTop ? edgeY + band : edgeY - band));
    return brush;
}

ID2D1LinearGradientBrush*
LyricTextRenderer::verticalBorrowFadeBrush(const LyricTextDrawContext& context, float edgeY,
                                           float extend, bool atTop) {
    ID2D1LinearGradientBrush* brush = ensureLyricRightFadeBrush(context);
    if (!brush || extend <= 0.0f)
        return nullptr;
    brush->SetStartPoint(D2D1::Point2F(0.0f, edgeY));
    brush->SetEndPoint(D2D1::Point2F(0.0f, atTop ? edgeY - extend : edgeY + extend));
    return brush;
}

void LyricTextRenderer::drawScrollingText(const LyricTextDrawContext& context,
                                          IDWriteTextLayout* layout, float textW, float textH,
                                          float areaW, float x, float y, float offset,
                                          ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow,
                                          ID2D1Brush* karaokeBrush, float karaokeX, float opacity,
                                          LyricAlignment alignment, bool singleCopy,
                                          float rightExtend, bool constantEdgeFade,
                                          float leftFadeDip, float centerAnchor) {
    auto* rt = context.target;
    if (!rt || !layout || areaW <= 0.0f)
        return;
    y += context.starTextShift;
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity >= 0.999f)
        opacity = 1.0f;
    // 光晕/描边走离屏缓存：三层合成一次，之后滚动/淡变只 DrawImage；
    // 失败时 fxBmp 为空，回落到逐层直接绘制
    ID2D1Bitmap* fxBmp = nullptr;
    if (glow || outline)
        fxBmp = textFxBitmap(context, layout, textW, textH, brush, outline, glow);
    ID2D1Brush* brushes[4] = {fxBmp ? nullptr : brush, fxBmp ? nullptr : outline,
                              fxBmp ? nullptr : glow, karaokeBrush};
    ID2D1Brush* changed[4] = {};
    float previousOpacity[4] = {};
    int changedCount = 0;
    for (ID2D1Brush* candidate : brushes) {
        if (!candidate || opacity >= 0.999f)
            continue;
        bool alreadyChanged = false;
        for (int i = 0; i < changedCount; ++i)
            alreadyChanged = alreadyChanged || changed[i] == candidate;
        if (alreadyChanged)
            continue;
        previousOpacity[changedCount] = candidate->GetOpacity();
        candidate->SetOpacity(previousOpacity[changedCount] * opacity);
        changed[changedCount++] = candidate;
    }
    // 右缘（滚入侧）渐隐带借用到可视区之外的留白，可读区域内文字保持清晰
    ID2D1LinearGradientBrush* rightFade =
        (textW > areaW && rightExtend > 0.0f && lyricRightFadeLayer_.Get())
            ? lyricRightFadeBrush(context, x + areaW, rightExtend)
            : nullptr;
    // 左缘（滚出侧）在区域内渐隐，文字不会越过区域边界：歌词渐隐带随滚出量
    // 从 0 建立（新行贴边界时不渐隐，与第二行左缘严格对齐）；歌曲信息是
    // 无限循环跑马灯，是否滚动在内容确定时就已知，渐隐恒定保持，
    // 避免每轮循环绕回时渐隐消失再出现。leftFadeDip > 0 时覆盖默认渐隐宽度
    const float fadeDip = leftFadeDip > 0.0f ? leftFadeDip : kLyricEdgeFadeDip;
    const float leftFadeW = std::min(fadeDip, areaW * 0.25f);
    const float leftScrolled = constantEdgeFade ? leftFadeW : offset;
    ID2D1LinearGradientBrush* fadeBrush =
        (textW > areaW && leftScrolled > 0.0f && lyricEdgeFadeLayer_.Get())
            ? lyricEdgeFadeBrush(context, x, leftFadeW, leftScrolled)
            : nullptr;
    const int effectLine = layout == (*context.resources).lyricLayout_.Get() ? context.currentLine
                           : layout == (*context.resources).outgoingLyricLayout_.Get()
                               ? context.outgoingLine
                               : -1;
    const float effectTopPad =
        context.effects.lyricStarEnabled(effectLine)
            ? LyricEffectsRenderer::starRadius(context.fontSize) * 2.0f + 10.0f
            : 0.0f;
    D2D1_RECT_F clip{x, y - effectTopPad, x + areaW + (rightFade ? rightExtend : 0.0f), y + textH};
    if (rightFade) {
        rt->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Identity(), 1.0f, rightFade),
                      lyricRightFadeLayer_.Get());
    }
    if (fadeBrush) {
        rt->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Identity(), 1.0f, fadeBrush),
                      lyricEdgeFadeLayer_.Get());
    } else {
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    }
    static constexpr float kDirs[8][2] = {
        {1.0f, 0.0f},  {0.7071f, 0.7071f},   {0.0f, 1.0f},  {-0.7071f, 0.7071f},
        {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}};
    // 绘制起点：居中一个，或跑马灯首尾相接两个；逐字/转场入场时只有一份、不循环
    float bases[2];
    int n = 0;
    auto alignedBase = [&]() {
        float freeW = std::max(0.0f, areaW - textW);
        switch (alignment) {
        case LyricAlignment::Left:
            return x;
        case LyricAlignment::Right:
            return x + freeW;
        case LyricAlignment::Center:
        default:
            if (centerAnchor >= 0.0f)
                return std::clamp(centerAnchor - textW * 0.5f, x, x + freeW);
            return x + freeW * 0.5f;
        }
    };
    if (karaokeBrush || singleCopy) {
        bases[n++] = (textW <= areaW) ? alignedBase() : x - offset;
    } else if (textW <= areaW) {
        bases[n++] = alignedBase();
    } else {
        float loopW = textW + kTextPadding * 2.0f;
        bases[n++] = x - offset;
        bases[n++] = x - offset + loopW;
    }
    const float effectProgress = context.effectsMask == 0 ? 0.0f
                                 : karaokeBrush && textW > 0
                                     ? std::clamp(karaokeX / textW, 0.0f, 1.0f)
                                     : context.effects.lyricEffectProgress(effectLine);
    auto drawOverlay = [&](bool foreground) {
        if (effectLine < 0 || context.effectsMask == 0)
            return;
        for (int i = 0; i < n; ++i)
            context.effects.drawLyricOverlay(layout, effectLine, bases[i], y, textW, textH, false,
                                             opacity, effectProgress, foreground);
    };
    drawOverlay(false);
    if (fxBmp) {
        constexpr float textFxPad = 3.0f;
        const float textFxW = textW + textFxPad * 2.0f;
        const float textFxH = textH + textFxPad * 2.0f;
        for (int i = 0; i < n; ++i)
            rt->DrawBitmap(fxBmp,
                           D2D1::RectF(bases[i] - textFxPad, y - textFxPad,
                                       bases[i] - textFxPad + textFxW, y - textFxPad + textFxH),
                           opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        if (glow || outline) {
            for (int i = 0; i < n; ++i) {
                if (glow) {
                    for (auto& d : kDirs)
                        rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 2.4f, y + d[1] * 2.4f),
                                           layout, glow);
                }
                if (outline) {
                    for (auto& d : kDirs)
                        rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 1.2f, y + d[1] * 1.2f),
                                           layout, outline);
                }
            }
        }
        for (int i = 0; i < n; ++i)
            rt->DrawTextLayout(D2D1::Point2F(bases[i], y), layout, brush);
    }
    // 逐字高亮层：裁剪到填充进度
    if (karaokeBrush && karaokeX > 0.0f) {
        rt->PushAxisAlignedClip(D2D1::RectF(bases[0], y, bases[0] + karaokeX, y + textH),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->DrawTextLayout(D2D1::Point2F(bases[0], y), layout, karaokeBrush);
        rt->PopAxisAlignedClip();
    }
    drawOverlay(true);
    if (fadeBrush)
        rt->PopLayer();
    else
        rt->PopAxisAlignedClip();
    if (rightFade)
        rt->PopLayer();
    for (int i = 0; i < changedCount; ++i)
        changed[i]->SetOpacity(previousOpacity[i]);
    if (fxBmp)
        fxBmp->Release();
}

void LyricTextRenderer::drawVerticalScrollingText(
    const LyricTextDrawContext& context, IDWriteTextLayout* layout, float textW, float textH,
    float areaH, float x, float y, float offset, ID2D1Brush* brush, ID2D1Brush* outline,
    ID2D1Brush* glow, ID2D1Brush* karaokeBrush, float karaokeX, float opacity,
    LyricAlignment alignment, bool singleCopy, bool rotated, float topExtend, float bottomExtend,
    bool constantEdgeFade, bool skipSlotClip, float slotY, int effectLine, float effectRangeStart,
    float effectRangeSize, float effectProgress) {
    auto* rt = context.target;
    if (!rt || !layout || textW <= 0.0f || textH <= 0.0f || areaH <= 0.0f)
        return;
    if (effectLine < 0)
        effectLine = layout == (*context.resources).verticalLyricLayout_.Get() ? context.currentLine
                     : layout == (*context.resources).outgoingVerticalLyricLayout_.Get()
                         ? context.outgoingLine
                         : -1;
    const bool star = context.effects.lyricStarEnabled(effectLine);
    // 竖排沿字列右侧预留星星通道，旋转英文与中文保持同一侧。
    const float starPad =
        star ? LyricEffectsRenderer::starRadius(context.fontSize) * 2.0f + 10.0f : 0.0f;
    if (star)
        x -= std::min(8.0f, std::max(0.0f, (context.width - context.fontSize) * 0.5f - 2.0f));
    (void)alignment;
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity >= 0.999f)
        opacity = 1.0f;

    // rotated 布局保存的是整句横向文字：左右侧共用截图中的固定角度，
    // 再映射到同一个竖向滚动槽；中文逐字布局则保持字形正向。
    ID2D1Bitmap* fxBmp = nullptr;
    // 离屏缓存按横向布局的原始尺寸创建；旋转文字直接绘制，避免把
    // “横向宽高”交换后再交给未旋转的缓存目标而截断英文句子。
    if (!rotated && (glow || outline))
        fxBmp = textFxBitmap(context, layout, textW, textH, brush, outline, glow);
    ID2D1Brush* brushes[4] = {fxBmp ? nullptr : brush, fxBmp ? nullptr : outline,
                              fxBmp ? nullptr : glow, karaokeBrush};
    ID2D1Brush* changed[4] = {};
    float previousOpacity[4] = {};
    int changedCount = 0;
    for (ID2D1Brush* candidate : brushes) {
        if (!candidate || opacity >= 0.999f)
            continue;
        bool alreadyChanged = false;
        for (int i = 0; i < changedCount; ++i)
            alreadyChanged = alreadyChanged || changed[i] == candidate;
        if (alreadyChanged)
            continue;
        previousOpacity[changedCount] = candidate->GetOpacity();
        candidate->SetOpacity(previousOpacity[changedCount] * opacity);
        changed[changedCount++] = candidate;
    }

    // 两侧渐隐：滚出侧在区域内按滚出量建立渐隐带，滚入侧借位到可视区外。
    // skipSlotClip 时由调用方（多段竖排歌词）统一裁剪与渐隐。
    // 裁剪与渐隐锚定歌词槽（slotY，转场期间文字随 oldY/newY 移动但槽不动），
    // 避免滑出/滑入的文字飘到封面等区域
    const float slotTop = slotY >= 0.0f ? slotY : y;
    const VerticalEdgeFades fades =
        skipSlotClip ? VerticalEdgeFades{}
                     : verticalEdgeFades(context, offset, textH, areaH, karaokeBrush != nullptr,
                                         constantEdgeFade);
    const float topExt = fades.borrow && fades.borrowAtTop ? topExtend : 0.0f;
    const float bottomExt = fades.borrow && !fades.borrowAtTop ? bottomExtend : 0.0f;
    ID2D1LinearGradientBrush* borrowFade =
        (topExt > 0.0f || bottomExt > 0.0f) && lyricRightFadeLayer_.Get()
            ? verticalBorrowFadeBrush(context, fades.borrowAtTop ? slotTop : slotTop + areaH,
                                      fades.borrowAtTop ? topExt : bottomExt, fades.borrowAtTop)
            : nullptr;
    ID2D1LinearGradientBrush* outFade =
        fades.outBand > 0.0f && lyricEdgeFadeLayer_.Get()
            ? verticalOutFadeBrush(context, fades.outAtTop ? slotTop : slotTop + areaH,
                                   fades.outBand, fades.outAtTop)
            : nullptr;
    const D2D1_RECT_F clip{x, slotTop - topExt, x + textW + starPad, slotTop + areaH + bottomExt};
    enum class ClipMode { kNone, kClip, kOutLayer, kBorrowLayer, kBothLayers };
    ClipMode clipMode = ClipMode::kNone;
    if (!skipSlotClip) {
        if (borrowFade) {
            rt->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Identity(), 1.0f, borrowFade),
                          lyricRightFadeLayer_.Get());
            clipMode = ClipMode::kBorrowLayer;
        }
        if (outFade) {
            rt->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Identity(), 1.0f, outFade),
                          lyricEdgeFadeLayer_.Get());
            clipMode =
                clipMode == ClipMode::kBorrowLayer ? ClipMode::kBothLayers : ClipMode::kOutLayer;
        }
        if (clipMode == ClipMode::kNone) {
            rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            clipMode = ClipMode::kClip;
        }
    }
    // 两侧统一从上向下滚动。第二份副本放在前一份上方，确保内容离开底部
    // 后从顶部无缝接回，而不会因为右侧任务栏的文字顺序而反向上跑。
    const float base = y + offset;
    float bases[2]{};
    int count = 0;
    if (karaokeBrush || singleCopy || textH <= areaH) {
        bases[count++] = base;
    } else {
        const float loopH = textH + kVerticalLyricGap;
        bases[count++] = base;
        bases[count++] = base - loopH;
    }

    auto rotatedTransform = [&](float drawX, float top) {
        // D2D 坐标系的 Y 轴向下，统一使用顺时针 90°，匹配截图中的固定角度。
        return D2D1::Matrix3x2F(0.0f, 1.0f, -1.0f, 0.0f, drawX + textW, top);
    };

    auto drawRotatedLayout = [&](float drawX, float top, ID2D1Brush* drawBrush) {
        if (!drawBrush)
            return;
        D2D1_MATRIX_3X2_F previous{};
        rt->GetTransform(&previous);
        rt->SetTransform(rotatedTransform(drawX, top) * previous);
        rt->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), layout, drawBrush);
        rt->SetTransform(previous);
    };

    if (effectLine < 0)
        effectLine = layout == (*context.resources).verticalLyricLayout_.Get() ? context.currentLine
                     : layout == (*context.resources).outgoingVerticalLyricLayout_.Get()
                         ? context.outgoingLine
                         : -1;
    if (effectProgress < 0)
        effectProgress = context.effectsMask == 0 ? 0.0f
                         : karaokeBrush           ? karaokeX
                                                  : context.effects.lyricEffectProgress(effectLine);
    auto drawOverlay = [&](bool foreground) {
        if (effectLine < 0 || context.effectsMask == 0)
            return;
        for (int i = 0; i < count; ++i) {
            if (rotated) {
                D2D1_MATRIX_3X2_F previous{};
                rt->GetTransform(&previous);
                rt->SetTransform(rotatedTransform(x, bases[i]) * previous);
                context.effects.drawLyricOverlay(layout, effectLine, 0, 0, textH, textW, false,
                                                 opacity, effectProgress, foreground,
                                                 effectRangeStart, effectRangeSize);
                rt->SetTransform(previous);
            } else {
                context.effects.drawLyricOverlay(layout, effectLine, x, bases[i], textW, textH,
                                                 true, opacity, effectProgress, foreground,
                                                 effectRangeStart, effectRangeSize);
            }
        }
    };
    drawOverlay(false);
    if (fxBmp) {
        constexpr float textFxPad = 3.0f;
        const float textFxW = textW + textFxPad * 2.0f;
        const float textFxH = textH + textFxPad * 2.0f;
        for (int i = 0; i < count; ++i)
            rt->DrawBitmap(fxBmp,
                           D2D1::RectF(x - textFxPad, bases[i] - textFxPad, x - textFxPad + textFxW,
                                       bases[i] - textFxPad + textFxH),
                           opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        static constexpr float kDirs[8][2] = {
            {1.0f, 0.0f},  {0.7071f, 0.7071f},   {0.0f, 1.0f},  {-0.7071f, 0.7071f},
            {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}};
        if (glow || outline) {
            for (int i = 0; i < count; ++i) {
                if (glow) {
                    for (auto& d : kDirs)
                        if (rotated)
                            drawRotatedLayout(x + d[0] * 2.4f, bases[i] + d[1] * 2.4f, glow);
                        else
                            rt->DrawTextLayout(
                                D2D1::Point2F(x + d[0] * 2.4f, bases[i] + d[1] * 2.4f), layout,
                                glow);
                }
                if (outline) {
                    for (auto& d : kDirs)
                        if (rotated)
                            drawRotatedLayout(x + d[0] * 1.2f, bases[i] + d[1] * 1.2f, outline);
                        else
                            rt->DrawTextLayout(
                                D2D1::Point2F(x + d[0] * 1.2f, bases[i] + d[1] * 1.2f), layout,
                                outline);
                }
            }
        }
        for (int i = 0; i < count; ++i) {
            if (rotated)
                drawRotatedLayout(x, bases[i], brush);
            else
                rt->DrawTextLayout(D2D1::Point2F(x, bases[i]), layout, brush);
        }
    }

    // karaokeX 在竖排路径中统一从上向下填充；旋转英文也按最终可见的
    // 竖向顺序裁剪，避免左右任务栏出现相反的高亮方向。
    if (karaokeBrush && karaokeX > 0.0f) {
        const float progress = std::clamp(karaokeX, 0.0f, 1.0f);
        const float top = bases[0];
        const float bottom = bases[0] + textH * progress;
        if (bottom > top) {
            rt->PushAxisAlignedClip(D2D1::RectF(x, top, x + textW, bottom),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (rotated)
                drawRotatedLayout(x, bases[0], karaokeBrush);
            else
                rt->DrawTextLayout(D2D1::Point2F(x, bases[0]), layout, karaokeBrush);
            rt->PopAxisAlignedClip();
        }
    }
    drawOverlay(true);
    if (clipMode == ClipMode::kOutLayer || clipMode == ClipMode::kBothLayers)
        rt->PopLayer();
    if (clipMode == ClipMode::kBorrowLayer || clipMode == ClipMode::kBothLayers)
        rt->PopLayer();
    if (clipMode == ClipMode::kClip)
        rt->PopAxisAlignedClip();
    for (int i = 0; i < changedCount; ++i)
        changed[i]->SetOpacity(previousOpacity[i]);
    if (fxBmp)
        fxBmp->Release();
}

void LyricTextRenderer::drawVerticalLyricParts(
    const LyricTextDrawContext& context, const std::vector<VerticalLyricPart>& parts, float blockH,
    float areaH, float railW, float y, float offset, ID2D1Brush* brush, ID2D1Brush* outline,
    ID2D1Brush* glow, ID2D1Brush* karaokeBrush, float karaokeProgress, float opacity,
    bool singleCopy, float topExtend, float bottomExtend, bool constantEdgeFade, float slotY) {
    auto* rt = context.target;
    if (!rt || parts.empty() || blockH <= 0.0f || areaH <= 0.0f)
        return;

    // 多段布局的两侧渐隐在整组内容上统一施加，避免每个分段各自渐隐；
    // 分段内部不再单独裁剪，裁剪由此处的图层/裁剪范围承担。
    // 裁剪与渐隐锚定歌词槽（slotY），转场期间文字随动画移动但槽不动
    const float slotTop = slotY >= 0.0f ? slotY : y;
    const VerticalEdgeFades fades = verticalEdgeFades(context, offset, blockH, areaH,
                                                      karaokeBrush != nullptr, constantEdgeFade);
    const float topExt = fades.borrow && fades.borrowAtTop ? topExtend : 0.0f;
    const float bottomExt = fades.borrow && !fades.borrowAtTop ? bottomExtend : 0.0f;
    ID2D1LinearGradientBrush* borrowFade =
        (topExt > 0.0f || bottomExt > 0.0f) && lyricRightFadeLayer_.Get()
            ? verticalBorrowFadeBrush(context, fades.borrowAtTop ? slotTop : slotTop + areaH,
                                      fades.borrowAtTop ? topExt : bottomExt, fades.borrowAtTop)
            : nullptr;
    ID2D1LinearGradientBrush* outFade =
        fades.outBand > 0.0f && lyricEdgeFadeLayer_.Get()
            ? verticalOutFadeBrush(context, fades.outAtTop ? slotTop : slotTop + areaH,
                                   fades.outBand, fades.outAtTop)
            : nullptr;
    const D2D1_RECT_F clip{0.0f, slotTop - topExt, railW, slotTop + areaH + bottomExt};
    bool pushedBorrow = false;
    bool pushedOut = false;
    bool pushedClip = false;
    if (borrowFade) {
        rt->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Identity(), 1.0f, borrowFade),
                      lyricRightFadeLayer_.Get());
        pushedBorrow = true;
    }
    if (outFade) {
        rt->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Identity(), 1.0f, outFade),
                      lyricEdgeFadeLayer_.Get());
        pushedOut = true;
    }
    if (!pushedBorrow && !pushedOut) {
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        pushedClip = true;
    }

    const float progress = std::clamp(karaokeProgress, 0.0f, 1.0f);
    const float sungExtent = blockH * progress;
    const float loopH = blockH + kVerticalLyricGap;
    const int copyCount = karaokeBrush || singleCopy || blockH <= areaH ? 1 : 2;
    const float base = y + offset;
    const int effectLine = &parts == &(*context.resources).verticalLyricParts_ ? context.currentLine
                           : &parts == &(*context.resources).outgoingVerticalLyricParts_
                               ? context.outgoingLine
                               : -1;
    for (int copy = 0; copy < copyCount; ++copy) {
        float partTop = base - copy * loopH;
        float partOffset = 0.0f;
        for (const auto& part : parts) {
            if (!part.layout.Get() || part.width <= 0.0f || part.height <= 0.0f)
                continue;

            float partProgress = 0.0f;
            if (karaokeBrush) {
                partProgress = std::clamp((sungExtent - partOffset) / part.height, 0.0f, 1.0f);
            }
            ID2D1Brush* partKaraoke = karaokeBrush && partProgress > 0.0f ? karaokeBrush : nullptr;
            const float partX = part.rotated ? (railW - part.width) * 0.5f : 0.0f;
            drawVerticalScrollingText(
                context, part.layout.Get(), part.width, part.height, areaH, partX, partTop, 0.0f,
                brush, outline, glow, partKaraoke, partProgress, opacity, LyricAlignment::Center,
                true, part.rotated, 0.0f, 0.0f, false, true, -1.0f, effectLine, partOffset / blockH,
                part.height / blockH,
                context.effectsMask == 0 ? 0.0f
                : karaokeBrush           ? karaokeProgress
                                         : context.effects.lyricEffectProgress(effectLine));
            partTop += part.height;
            partOffset += part.height;
        }
    }

    if (pushedOut)
        rt->PopLayer();
    if (pushedBorrow)
        rt->PopLayer();
    if (pushedClip)
        rt->PopAxisAlignedClip();
}

void LyricTextRenderer::createDeviceResources(ID2D1DeviceContext* target) {
    target->CreateLayer(lyricEdgeFadeLayer_.ReleaseAndGetAddressOf());
    target->CreateLayer(lyricRightFadeLayer_.ReleaseAndGetAddressOf());
}
void LyricTextRenderer::discardDeviceResources() {
    textEffectsCache_.clear();
    lyricEdgeFadeBrush_.Reset();
    lyricRightFadeBrush_.Reset();
    lyricEdgeFadeLayer_.Reset();
    lyricRightFadeLayer_.Reset();
}
