#include "ui/settings/artwork.h"
#include "ui/fluent_theme.h"
#include "ui/media_control_icons.h"
#include <algorithm>
#include <cmath>
void SettingsArtwork::drawSpectrumStyleArtwork(fluent::FluentDialogSurface::Painter& painter,
                                               const D2D1_RECT_F& bounds, int style, bool enabled) {
    const auto& p = painter.colors();
    const auto tone = [enabled](D2D1_COLOR_F color) {
        if (!enabled)
            color.a *= 0.42f;
        return color;
    };
    const auto withOpacity = [](D2D1_COLOR_F color, float opacity) {
        color.a *= opacity;
        return color;
    };
    const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                              float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->DrawLine(from, to, brush, stroke);
    };

    const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                          bounds.right - 1.0f, bounds.bottom - 1.0f);
    painter.fillRoundRect(tone(p.controlFill), panel, 7.0f);
    painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 7.0f);

    const float left = panel.left + 8.0f;
    const float right = panel.right - 8.0f;
    const float top = panel.top + 8.0f;
    const float bottom = panel.bottom - 8.0f;
    const float width = std::max(0.0f, right - left);
    const float height = std::max(0.0f, bottom - top);
    if (width <= 0.0f || height <= 0.0f)
        return;

    if (style == 4) {
        drawLine(withOpacity(p.text, 0.65f), D2D1::Point2F(left + 5.0f, top + height * 0.3f),
                 D2D1::Point2F(right - 5.0f, top + height * 0.3f), 1.3f);
        drawLine(withOpacity(p.textSecondary, 0.45f),
                 D2D1::Point2F(left + 5.0f, top + height * 0.56f),
                 D2D1::Point2F(left + width * 0.65f, top + height * 0.56f), 1.0f);
        for (int i = 0; i < 24; ++i) {
            const float x = left + i * width / 24;
            const float barH = 1.0f + height * 0.18f * (0.5f + 0.5f * std::sin(i * 0.73f));
            painter.fillRoundRect(tone(withOpacity(p.accent, 0.8f)),
                                  D2D1::RectF(x, bottom - barH, x + width / 40, bottom), 0.4f);
        }
        return;
    }

    if (style == 2) {
        // 梦幻波浪：三层半透明波面向同一底线收束，避免画成几条装饰线。
        if (!painter.target())
            return;
        ID2D1Factory* factory = nullptr;
        painter.target()->GetFactory(&factory);
        if (!factory)
            return;

        const float baseY = bottom - 2.0f;
        constexpr int kSamples = 24;
        constexpr float kTwoPi = 6.28318530718f;
        const auto waveY = [&](float x, float amplitude, float offset, float phase) {
            const float t = (x - left) / width;
            const float profile =
                0.18f + 0.82f * (0.5f + 0.5f * std::sin(kTwoPi * (t * 1.12f) + phase));
            return baseY - offset - amplitude * profile;
        };
        const auto drawFilledWave = [&](float amplitude, float offset, float phase, float opacity) {
            ID2D1PathGeometry* geometry = nullptr;
            if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
                return;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED(geometry->Open(&sink)) || !sink) {
                geometry->Release();
                return;
            }

            sink->BeginFigure(D2D1::Point2F(left, waveY(left, amplitude, offset, phase)),
                              D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i <= kSamples; ++i) {
                const float t = static_cast<float>(i) / kSamples;
                const float x = left + width * t;
                sink->AddLine(D2D1::Point2F(x, waveY(x, amplitude, offset, phase)));
            }
            sink->AddLine(D2D1::Point2F(right, baseY));
            sink->AddLine(D2D1::Point2F(left, baseY));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            const HRESULT closeHr = sink->Close();
            sink->Release();
            if (SUCCEEDED(closeHr)) {
                if (auto* brush = painter.brush(tone(withOpacity(p.accent, opacity))))
                    painter.target()->FillGeometry(geometry, brush);
            }
            geometry->Release();
        };

        drawFilledWave(height * 0.25f, height * 0.08f, 1.05f, 0.24f);
        drawFilledWave(height * 0.34f, height * 0.04f, 0.35f, 0.36f);
        drawFilledWave(height * 0.44f, 0.0f, -0.35f, 0.58f);
        drawLine(p.separator, D2D1::Point2F(left, baseY), D2D1::Point2F(right, baseY), 0.8f);

        D2D1_POINT_2F previous = D2D1::Point2F(left, waveY(left, height * 0.44f, 0.0f, -0.35f));
        for (int i = 1; i <= kSamples; ++i) {
            const float t = static_cast<float>(i) / kSamples;
            const float x = left + width * t;
            const D2D1_POINT_2F current = D2D1::Point2F(x, waveY(x, height * 0.44f, 0.0f, -0.35f));
            drawLine(withOpacity(p.accent, 0.62f), previous, current, 1.0f);
            previous = current;
        }
        factory->Release();
        return;
    }

    // 预览按实际频谱的 12 个频段绘制，并保持任务栏中 5:3 的柱宽/间隙比例。
    // 这样缩小到设置卡片后仍是窄柱，不会因为用少量柱体铺满宽度而显得粗重。
    constexpr int kBars = 12;
    constexpr float kLevels[kBars] = {0.26f, 0.42f, 0.60f, 0.82f, 0.44f, 0.70f,
                                      0.52f, 0.90f, 0.66f, 0.86f, 0.48f, 0.30f};
    constexpr float kBarUnitDenominator = kBars * 5.0f + (kBars - 1) * 3.0f;
    const float barUnit = width / kBarUnitDenominator;
    const float barW = barUnit * 5.0f;
    const float gap = barUnit * 3.0f;
    if (barW <= 0.0f)
        return;

    if (style == 3) {
        constexpr int cells = 7;
        const float step = height / (cells + 1);
        for (int i = 0; i < kBars; ++i) {
            const int lit = static_cast<int>(std::ceil(kLevels[i] * cells));
            const float x = left + i * (barW + gap);
            for (int cell = 0; cell < cells; ++cell) {
                const float y = bottom - cell * step;
                painter.fillRoundRect(tone(withOpacity(p.accent, cell < lit ? 0.85f : 0.10f)),
                                      D2D1::RectF(x, y - step * 0.6f, x + barW, y), 0.2f);
            }
            const float peakY = bottom - std::min(cells - 1, lit) * step;
            drawLine(p.accent, D2D1::Point2F(x, peakY), D2D1::Point2F(x + barW, peakY), 1.0f);
        }
        return;
    }

    if (style == 1) {
        // 柱状图：柱底统一贴近底线，电平只向上增长。
        const float baseY = bottom - 2.0f;
        drawLine(p.separator, D2D1::Point2F(left, baseY), D2D1::Point2F(right, baseY), 0.8f);
        for (int i = 0; i < kBars; ++i) {
            const float barH = std::max(3.0f, height * (0.16f + kLevels[i] * 0.72f));
            const float x = left + i * (barW + gap);
            painter.fillRoundRect(tone(p.accent), D2D1::RectF(x, baseY - barH, x + barW, baseY),
                                  barW * 0.5f);
        }
        return;
    }

    // 默认：柱体以中线为基准上下对称，保留原样式的视觉特征。
    const float centerY = (top + bottom) * 0.5f;
    for (int i = 0; i < kBars; ++i) {
        const float barH = std::max(3.0f, height * (0.16f + kLevels[i] * 0.68f));
        const float x = left + i * (barW + gap);
        painter.fillRoundRect(
            tone(p.accent), D2D1::RectF(x, centerY - barH * 0.5f, x + barW, centerY + barH * 0.5f),
            barW * 0.5f);
    }
}

void SettingsArtwork::drawSpectrumEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                                const D2D1_RECT_F& bounds, SpectrumEffect effect,
                                                bool enabled, bool gradient) {
    auto* rt = painter.target();
    if (!rt || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return;
    const auto& p = painter.colors();
    const auto tone = [enabled](D2D1_COLOR_F color, float opacity = 1.0f) {
        color.a *= opacity * (enabled ? 1.0f : 0.42f);
        return color;
    };
    const auto panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f, bounds.right - 1.0f,
                                   bounds.bottom - 1.0f);
    painter.fillRoundRect(tone(p.controlFill), panel, 7.0f);
    painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 7.0f);
    rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float left = panel.left + 8.0f;
    const float right = panel.right - 8.0f;
    const float baseY = panel.bottom - 6.0f;
    const float height = panel.bottom - panel.top;
    constexpr int count = 12;
    constexpr float levels[count] = {0.26f, 0.42f, 0.60f, 0.82f, 0.44f, 0.70f,
                                     0.52f, 0.90f, 0.66f, 0.86f, 0.48f, 0.30f};
    const float unit = std::max(0.0f, right - left) / (count * 5.0f + (count - 1) * 3.0f);
    const float barW = unit * 5.0f;
    const auto light = D2D1::ColorF(p.accent.r + (1.0f - p.accent.r) * 0.72f,
                                    p.accent.g + (1.0f - p.accent.g) * 0.72f,
                                    p.accent.b + (1.0f - p.accent.b) * 0.72f, p.accent.a);
    ID2D1LinearGradientBrush* sheen = nullptr;
    ID2D1LinearGradientBrush* barGradient = nullptr;
    if (gradient) {
        const auto dark = D2D1::ColorF(p.accent.r * kSpectrumBarGradientDarkFactor,
                                       p.accent.g * kSpectrumBarGradientDarkFactor,
                                       p.accent.b * kSpectrumBarGradientDarkFactor, p.accent.a);
        const auto bright = D2D1::ColorF(
            p.accent.r + (1.0f - p.accent.r) * kSpectrumBarGradientLightMix,
            p.accent.g + (1.0f - p.accent.g) * kSpectrumBarGradientLightMix,
            p.accent.b + (1.0f - p.accent.b) * kSpectrumBarGradientLightMix, p.accent.a);
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, tone(dark)}, {1.0f, tone(bright)}};
        ID2D1GradientStopCollection* collection = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, _countof(stops), &collection))) {
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                              D2D1::Point2F(0, baseY), D2D1::Point2F(0, panel.top)),
                                          collection, &barGradient);
            collection->Release();
        }
    }
    if (!gradient && effect == SpectrumEffect::Sheen) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, tone(light, 0.0f)},
                                            {0.38f, tone(light, 0.0f)},
                                            {0.48f, tone(light, 0.85f)},
                                            {0.58f, tone(light, 0.0f)},
                                            {1.0f, tone(light, 0.0f)}};
        ID2D1GradientStopCollection* collection = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 5, &collection))) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, panel.top),
                                                    D2D1::Point2F(right, panel.bottom)),
                collection, &sheen);
            collection->Release();
        }
    }
    for (int i = 0; i < count; ++i) {
        const float x = left + i * unit * 8.0f;
        const float barH = 3.0f + levels[i] * height * 0.48f;
        const auto bar = D2D1::RectF(x, baseY - barH, x + barW, baseY);
        if (!gradient && effect == SpectrumEffect::Glow) {
            for (int layer = 4; layer >= 1; --layer) {
                const float spread = layer * 0.85f;
                painter.fillRoundRect(tone(p.accent, 0.035f + (4 - layer) * 0.015f),
                                      D2D1::RectF(bar.left - spread, bar.top - spread,
                                                  bar.right + spread, bar.bottom + spread),
                                      barW * 0.5f + spread);
            }
        } else if (effect == SpectrumEffect::Trail) {
            // 两个较早的峰形从高处向当前柱顶收束，表现回落后的残留轮廓。
            const float previous = levels[(i + 3) % count];
            for (int layer = 2; layer >= 1; --layer) {
                const float extra = (0.3f + previous * 0.7f) * layer * height * 0.10f;
                painter.fillRoundRect(tone(p.accent, layer == 2 ? 0.12f : 0.24f),
                                      D2D1::RectF(bar.left, bar.top - extra, bar.right, bar.bottom),
                                      barW * 0.5f);
            }
        }
        if (barGradient) {
            const float cx = x + barW * 0.5f;
            barGradient->SetStartPoint(D2D1::Point2F(cx, bar.bottom));
            barGradient->SetEndPoint(D2D1::Point2F(cx, bar.top));
            rt->FillRoundedRectangle(D2D1::RoundedRect(bar, barW * 0.5f, barW * 0.5f), barGradient);
        } else {
            painter.fillRoundRect(tone(p.accent, 0.84f), bar, barW * 0.5f);
        }
        if (effect == SpectrumEffect::Sparks && i % 3 == 1) {
            const float cx = x + barW * 0.5f;
            if (auto* brush = painter.brush(tone(p.accent, 0.24f)))
                rt->DrawLine(D2D1::Point2F(cx, bar.top - 1.0f),
                             D2D1::Point2F(cx - 1.7f, bar.top - 4.0f), brush, 0.7f);
            if (auto* brush = painter.brush(tone(p.accent, 0.9f)))
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 2.2f, bar.top - 5.2f), 1.0f, 1.0f),
                                brush);
            if (auto* brush = painter.brush(tone(p.accent, 0.42f)))
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 2.0f, bar.top - 8.0f), 0.7f, 0.7f),
                                brush);
        } else if (sheen) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(bar, barW * 0.5f, barW * 0.5f), sheen);
        }
    }
    if (sheen)
        sheen->Release();
    if (barGradient)
        barGradient->Release();
    rt->PopAxisAlignedClip();
}

void SettingsArtwork::drawLyricEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                             const D2D1_RECT_F& bounds, LyricEffect effect,
                                             bool enabled) {
    auto* rt = painter.target();
    if (!rt || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return;
    const auto& p = painter.colors();
    auto tone = [&](D2D1_COLOR_F color, float alpha) {
        color.a *= alpha * (enabled ? 1.0f : 0.42f);
        return color;
    };
    const auto panel =
        D2D1::RectF(bounds.left + 1, bounds.top + 1, bounds.right - 1, bounds.bottom - 1);
    painter.fillRoundRect(tone(p.controlFill, 1), panel, 7);
    painter.strokeRoundRect(tone(p.cardStroke, 1), panel, 1, 7);
    rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float cx = (panel.left + panel.right) * 0.5f;
    const float cy = (panel.top + panel.bottom) * 0.5f;
    const float textShift = effect == LyricEffect::Stardust ? 7.0f : 0.0f;
    const auto textRect = D2D1::RectF(panel.left + 8, panel.top + 6 + textShift, panel.right - 8,
                                      panel.bottom - 6 + textShift);
    auto* format = painter.textFormat(14.0f, 500, true, true);
    if (!format) {
        rt->PopAxisAlignedClip();
        return;
    }
    if (effect == LyricEffect::Ripple) {
        if (auto* brush = painter.brush(tone(p.accent, 0.65f))) {
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8, 8), brush, 1.35f);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 12, 12), brush, 1.0f);
        }
    }
    if (effect == LyricEffect::WordWave) {
        const wchar_t* letters[] = {L"听", L"见", L"音", L"乐"};
        const float offsets[] = {0.2f, 1.0f, 1.5f, 0.5f};
        for (int i = 0; i < 4; ++i) {
            const float left = cx - 28.0f + i * 14.0f;
            painter.drawText(letters[i], format,
                D2D1::RectF(left, textRect.top + offsets[i], left + 14.0f,
                            textRect.bottom + offsets[i]), tone(p.accent, 0.82f));
        }
    } else {
        painter.drawText(L"听见音乐", format, textRect, tone(p.accent, 0.82f));
    }
    if (effect == LyricEffect::FollowLight) {
        rt->PushAxisAlignedClip(D2D1::RectF(cx - 8, panel.top, cx + 8, panel.bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        painter.drawText(L"听见音乐", format, textRect,
                         tone(D2D1::ColorF(p.accent.r * 0.30f, p.accent.g * 0.30f,
                                           p.accent.b * 0.30f, p.accent.a),
                              0.85f));
        rt->PopAxisAlignedClip();
        rt->PushAxisAlignedClip(D2D1::RectF(cx - 3, panel.top, cx + 3, panel.bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto light =
            D2D1::ColorF(p.accent.r + (1 - p.accent.r) * 0.7f, p.accent.g + (1 - p.accent.g) * 0.7f,
                         p.accent.b + (1 - p.accent.b) * 0.7f, p.accent.a);
        painter.drawText(L"听见音乐", format, textRect, tone(light, 1));
        rt->PopAxisAlignedClip();
    } else if (effect == LyricEffect::Underline) {
        for (int i = 0; i < 10; ++i) {
            if (auto* brush = painter.brush(tone(p.accent, 0.15f + i * 0.08f)))
                rt->DrawLine(D2D1::Point2F(cx - 27 + i * 4, cy + 10),
                             D2D1::Point2F(cx - 23 + i * 4, cy + 10), brush, 1);
        }
    } else if (effect == LyricEffect::Stardust) {
        for (int i = 15; i >= 1; --i) {
            const float age = i / 16.0f;
            const float fade = 1.0f - age;
            const float r = 0.55f + fade * (1.0f + (i % 3) * 0.2f);
            const float scatter = std::sin(i * 2.4f) * age * 5.5f - age;
            if (auto* brush = painter.brush(tone(p.accent, fade * fade * 0.85f)))
                rt->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cx - 7 - 5.0f - age * 30.0f, cy - 7.5f + scatter),
                                  r, r),
                    brush);
        }
        lyricStarArtwork_.draw(rt, D2D1::Point2F(cx - 7, cy - 7.5f), 7.2f, 0.35f,
                               painter.brush(tone(p.accent, 1.0f)));
    }
    rt->PopAxisAlignedClip();
}

void SettingsArtwork::drawSpectrumBackgroundArtwork(fluent::FluentDialogSurface::Painter& painter,
                                                    const D2D1_RECT_F& bounds, bool enabled) {
    const auto& p = painter.colors();
    const auto tone = [enabled](D2D1_COLOR_F color) {
        if (!enabled)
            color.a *= 0.42f;
        return color;
    };
    const auto withOpacity = [](D2D1_COLOR_F color, float opacity) {
        color.a *= opacity;
        return color;
    };
    const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                              float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->DrawLine(from, to, brush, stroke);
    };

    const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                          bounds.right - 1.0f, bounds.bottom - 1.0f);
    painter.fillRoundRect(tone(p.controlFill), panel, 7.0f);
    painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 7.0f);

    const float left = panel.left + 7.0f;
    const float right = panel.right - 7.0f;
    const float top = panel.top + 5.0f;
    const float bottom = panel.bottom - 5.0f;
    const float width = std::max(0.0f, right - left);
    const float height = std::max(0.0f, bottom - top);
    if (width <= 0.0f || height <= 0.0f || !painter.target())
        return;

    // 背景波浪：波面先铺在歌词横线下面，明确表达“歌词在波浪前景之上”。
    ID2D1Factory* factory = nullptr;
    painter.target()->GetFactory(&factory);
    if (!factory)
        return;

    const float baseY = bottom;
    constexpr int kSamples = 24;
    constexpr float kTwoPi = 6.28318530718f;
    const auto waveY = [&](float x, float amplitude, float offset, float phase) {
        const float t = (x - left) / width;
        const float profile =
            0.18f + 0.82f * (0.5f + 0.5f * std::sin(kTwoPi * (t * 1.05f) + phase));
        return baseY - offset - amplitude * profile;
    };
    const auto drawFilledWave = [&](float amplitude, float offset, float phase, float opacity) {
        ID2D1PathGeometry* geometry = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
            return;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geometry->Open(&sink)) || !sink) {
            geometry->Release();
            return;
        }

        sink->BeginFigure(D2D1::Point2F(left, waveY(left, amplitude, offset, phase)),
                          D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i <= kSamples; ++i) {
            const float t = static_cast<float>(i) / kSamples;
            const float x = left + width * t;
            sink->AddLine(D2D1::Point2F(x, waveY(x, amplitude, offset, phase)));
        }
        sink->AddLine(D2D1::Point2F(right, baseY));
        sink->AddLine(D2D1::Point2F(left, baseY));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        const HRESULT closeHr = sink->Close();
        sink->Release();
        if (SUCCEEDED(closeHr)) {
            if (auto* brush = painter.brush(tone(withOpacity(p.accent, opacity))))
                painter.target()->FillGeometry(geometry, brush);
        }
        geometry->Release();
    };

    drawFilledWave(height * 0.54f, 0.0f, -0.35f, 0.24f);
    drawFilledWave(height * 0.38f, height * 0.10f, 0.55f, 0.16f);

    // 前景歌词：横线覆盖在波面上，和“梦幻波浪”独立展示波形的预览区分开。
    drawLine(withOpacity(p.text, 0.78f), D2D1::Point2F(left + 12.0f, top + height * 0.36f),
             D2D1::Point2F(right - 12.0f, top + height * 0.36f), 1.4f);
    drawLine(withOpacity(p.textSecondary, 0.72f), D2D1::Point2F(left + 12.0f, top + height * 0.66f),
             D2D1::Point2F(left + width * 0.62f, top + height * 0.66f), 1.1f);
    factory->Release();
}

void SettingsArtwork::drawCoverEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                             const D2D1_RECT_F& bounds, bool vinyl, bool enabled) {
    const auto tone = [enabled](D2D1_COLOR_F color) {
        if (!enabled)
            color.a *= 0.42f;
        return color;
    };
    const auto fillEllipse = [&](D2D1_COLOR_F color, D2D1_POINT_2F center, float radius) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->FillEllipse(D2D1::Ellipse(center, radius, radius), brush);
    };
    const auto drawEllipse = [&](D2D1_COLOR_F color, D2D1_POINT_2F center, float radius,
                                 float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush, stroke);
    };
    const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                              float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->DrawLine(from, to, brush, stroke);
    };

    const float left = bounds.left;
    const float top = bounds.top;
    const float right = bounds.right;
    const float bottom = bounds.bottom;
    const D2D1_COLOR_F clay = D2D1::ColorF(0.73f, 0.39f, 0.31f);
    const D2D1_COLOR_F paper = D2D1::ColorF(0.95f, 0.84f, 0.68f);
    const D2D1_COLOR_F ink = D2D1::ColorF(0.13f, 0.14f, 0.16f);
    const D2D1_COLOR_F vinylEdge = D2D1::ColorF(0.25f, 0.26f, 0.28f);

    if (!vinyl) {
        // 默认模式：歌曲封面会占满这块圆角方形，不再画成“封面套封面”。
        const D2D1_RECT_F cover = D2D1::RectF(left + 2.0f, top + 2.0f, right - 2.0f, bottom - 2.0f);
        const D2D1_RECT_F artwork =
            D2D1::RectF(left + 5.0f, top + 5.0f, right - 5.0f, bottom - 5.0f);
        painter.fillRoundRect(tone(clay), cover, 6.0f);
        painter.fillRoundRect(tone(paper), artwork, 4.0f);
        drawLine(ink, D2D1::Point2F(artwork.left + 3.0f, artwork.top + 8.0f),
                 D2D1::Point2F(artwork.right - 4.0f, artwork.top + 8.0f), 1.5f);
        drawLine(ink, D2D1::Point2F(artwork.left + 3.0f, artwork.top + 12.0f),
                 D2D1::Point2F(artwork.left + 14.0f, artwork.top + 12.0f), 1.5f);
        drawLine(clay, D2D1::Point2F(artwork.left + 4.0f, artwork.bottom - 6.0f),
                 D2D1::Point2F(artwork.right - 4.0f, artwork.top + 16.0f), 1.8f);
        return;
    }

    // 黑胶模式：固定圆角底座、中央唱片和右上方唱臂。
    const bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
    const auto deckFill =
        light ? D2D1::ColorF(0.97f, 0.975f, 0.98f) : D2D1::ColorF(0.07f, 0.08f, 0.09f);
    const auto deckStroke = light ? D2D1::ColorF(0.77f, 0.79f, 0.81f) : vinylEdge;
    const auto platterRim =
        light ? D2D1::ColorF(0.82f, 0.84f, 0.86f) : D2D1::ColorF(0.37f, 0.39f, 0.42f);
    const auto metal =
        light ? D2D1::ColorF(0.86f, 0.88f, 0.90f) : D2D1::ColorF(0.65f, 0.67f, 0.69f);
    const auto armShadow = light ? D2D1::ColorF(0.45f, 0.47f, 0.49f, 0.60f) : ink;
    const auto cartridge = light ? D2D1::ColorF(0.95f, 0.96f, 0.97f) : ink;
    const auto grooveColor =
        light ? D2D1::ColorF(0.25f, 0.27f, 0.29f, 0.20f) : D2D1::ColorF(0.70f, 0.72f, 0.75f, 0.16f);
    const float size = std::max(0.0f, std::min(right - left, bottom - top) - 4.0f);
    const float deckLeft = (left + right - size) * 0.5f;
    const float deckTop = (top + bottom - size) * 0.5f;
    const auto point = [&](float x, float y) {
        return D2D1::Point2F(deckLeft + size * x, deckTop + size * y);
    };
    const auto deck = D2D1::RectF(deckLeft, deckTop, deckLeft + size, deckTop + size);
    painter.fillRoundRect(tone(deckFill), deck, 6.0f);
    painter.strokeRoundRect(tone(deckStroke), deck, 0.6f, 6.0f);
    const float cx = deckLeft + size * 0.5f;
    const float cy = deckTop + size * 0.5f;
    const float radius = size * 0.40f;
    const D2D1_POINT_2F center = D2D1::Point2F(cx, cy);
    fillEllipse(platterRim, center, radius);
    const float recordRadius = radius - 0.5f;
    fillEllipse(clay, center, recordRadius);
    const D2D1_GRADIENT_STOP stops[] = {
        {0.00f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f))},
        {0.22f, tone(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f))},
        {0.38f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f))},
        {0.52f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f))},
        {0.76f, tone(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f))},
        {1.00f, tone(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f))},
    };
    ID2D1GradientStopCollection* collection = nullptr;
    if (SUCCEEDED(painter.target()->CreateGradientStopCollection(
            stops, _countof(stops), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection))) {
        ID2D1LinearGradientBrush* surface = nullptr;
        if (SUCCEEDED(painter.target()->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(cx - recordRadius * 0.8f, cy - recordRadius * 0.6f),
                    D2D1::Point2F(cx + recordRadius * 0.8f, cy + recordRadius * 0.6f)),
                collection, &surface))) {
            painter.target()->FillEllipse(D2D1::Ellipse(center, recordRadius, recordRadius),
                                          surface);
            surface->Release();
        }
        collection->Release();
    }

    // 与真实渲染保持相同的中央封面比例。
    const float coverRadius = size * 0.25f;
    for (float groove = coverRadius + 0.9f; groove < recordRadius - 0.4f; groove += 1.1f)
        drawEllipse(grooveColor, center, groove, 0.35f);
    const D2D1_POINT_2F coverCenter = D2D1::Point2F(cx, cy);
    fillEllipse(paper, coverCenter, coverRadius);
    fillEllipse(clay, D2D1::Point2F(cx - coverRadius * 0.18f, cy + coverRadius * 0.16f),
                coverRadius * 0.44f);
    drawLine(ink, D2D1::Point2F(cx - coverRadius * 0.55f, cy - coverRadius * 0.12f),
             D2D1::Point2F(cx + coverRadius * 0.50f, cy - coverRadius * 0.12f), 1.1f);
    drawLine(clay, D2D1::Point2F(cx - coverRadius * 0.52f, cy + coverRadius * 0.48f),
             D2D1::Point2F(cx + coverRadius * 0.48f, cy - coverRadius * 0.34f), 1.4f);
    drawEllipse(grooveColor, coverCenter, coverRadius, 0.9f);
    fillEllipse(light ? D2D1::ColorF(0.78f, 0.80f, 0.82f) : ink, center, 1.7f);
    const auto pivot = point(0.85f, 0.17f);
    const auto elbow = point(0.81f, 0.55f);
    const auto stylus = point(0.73f, 0.75f);
    fillEllipse(armShadow, pivot, size * 0.075f);
    drawLine(armShadow, pivot, elbow, std::max(1.2f, size * 0.045f));
    drawLine(armShadow, elbow, stylus, std::max(1.2f, size * 0.045f));
    drawLine(metal, pivot, elbow, std::max(0.65f, size * 0.023f));
    drawLine(metal, elbow, stylus, std::max(0.65f, size * 0.023f));
    fillEllipse(light ? D2D1::ColorF(0.99f, 0.99f, 0.99f) : metal, pivot, size * 0.035f);
    drawLine(cartridge, point(0.75f, 0.70f), point(0.71f, 0.79f), std::max(1.5f, size * 0.07f));
}

void SettingsArtwork::drawSongToastPositionArtwork(fluent::FluentDialogSurface::Painter& painter,
                                                   const D2D1_RECT_F& bounds, bool top,
                                                   bool enabled) {
    const auto& p = painter.colors();
    const auto tone = [enabled](D2D1_COLOR_F color) {
        if (!enabled)
            color.a *= 0.42f;
        return color;
    };
    const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                              float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->DrawLine(from, to, brush, stroke);
    };

    // 用一个简化的屏幕框表示位置参照，弹窗本身保留封面和文字两块信息。
    const D2D1_RECT_F screen = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                           bounds.right - 1.0f, bounds.bottom - 1.0f);
    painter.fillRoundRect(tone(p.controlFill), screen, 7.0f);
    painter.strokeRoundRect(tone(p.cardStroke), screen, 1.0f, 7.0f);

    const float centerY = (screen.top + screen.bottom) * 0.5f;
    drawLine(p.separator, D2D1::Point2F(screen.left + 12.0f, centerY),
             D2D1::Point2F(screen.right - 12.0f, centerY), 1.0f);

    const float toastW = std::min(72.0f, screen.right - screen.left - 18.0f);
    const float toastH = 20.0f;
    const float toastLeft = (screen.left + screen.right - toastW) * 0.5f;
    const float toastTop = top ? screen.top + 7.0f : screen.bottom - toastH - 7.0f;
    const D2D1_RECT_F toast =
        D2D1::RectF(toastLeft, toastTop, toastLeft + toastW, toastTop + toastH);
    const float toastRadius = 4.5f;
    painter.fillRoundRect(tone(p.listSelected), toast, toastRadius);
    painter.strokeRoundRect(tone(p.accent), toast, 1.0f, toastRadius);

    const float coverSize = toastH - 6.0f;
    const D2D1_RECT_F cover = D2D1::RectF(toast.left + 3.0f, toast.top + 3.0f,
                                          toast.left + 3.0f + coverSize, toast.bottom - 3.0f);
    painter.fillRoundRect(tone(p.accent), cover, 2.5f);
    drawLine(p.textOnAccent, D2D1::Point2F(cover.left + 2.0f, cover.top + 4.0f),
             D2D1::Point2F(cover.right - 2.0f, cover.top + 4.0f), 0.8f);
    drawLine(p.text, D2D1::Point2F(cover.right + 4.0f, toast.top + 7.0f),
             D2D1::Point2F(toast.right - 5.0f, toast.top + 7.0f), 1.2f);
    drawLine(p.textSecondary, D2D1::Point2F(cover.right + 4.0f, toast.top + 13.0f),
             D2D1::Point2F(toast.right - 17.0f, toast.top + 13.0f), 1.0f);
}

void SettingsArtwork::drawHoverControlStyleArtwork(fluent::FluentDialogSurface::Painter& painter,
                                                   const D2D1_RECT_F& bounds, bool mediaCard,
                                                   bool enabled) {
    const auto& p = painter.colors();
    const auto tone = [enabled](D2D1_COLOR_F color) {
        if (!enabled)
            color.a *= 0.42f;
        return color;
    };
    const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                              float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            painter.target()->DrawLine(from, to, brush, stroke);
    };

    const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                          bounds.right - 1.0f, bounds.bottom - 1.0f);
    painter.fillRoundRect(tone(p.controlFill), panel, 9.0f);
    painter.strokeRoundRect(tone(p.cardStroke), panel, 1.0f, 9.0f);

    // 设置页直接复用任务栏和媒体卡片使用的播放控件几何，避免预览和真实图标各画一套。
    media_control::Geometry controls;
    ID2D1Factory* factory = nullptr;
    bool hasControls = false;
    if (painter.target()) {
        painter.target()->GetFactory(&factory);
        if (factory) {
            hasControls = media_control::create(factory, controls);
            factory->Release();
        }
    }
    const auto drawControl = [&](int index, bool playing, D2D1_POINT_2F center, float radius,
                                 D2D1_COLOR_F color) {
        if (!hasControls)
            return;
        if (auto* brush = painter.brush(tone(color)))
            media_control::draw(painter.target(), controls, index, playing, center, radius, brush);
    };
    const auto fillButton = [&](D2D1_COLOR_F color, D2D1_POINT_2F center, float size) {
        painter.fillRoundRect(tone(color),
                              D2D1::RectF(center.x - size * 0.5f, center.y - size * 0.5f,
                                          center.x + size * 0.5f, center.y + size * 0.5f),
                              size * 0.5f);
    };

    if (!mediaCard) {
        // 内嵌控件：保留两条等长歌词基线，四个控件统一使用任务栏里的普通单色字形。
        const float panelH = panel.bottom - panel.top;
        const float cx = (panel.left + panel.right) * 0.5f;
        const float cy = (panel.top + panel.bottom) * 0.5f;
        const float lineLeft = panel.left + 18.0f;
        const float lineRight = panel.right - 18.0f;
        drawLine(p.separator, D2D1::Point2F(lineLeft, panel.top + 20.0f),
                 D2D1::Point2F(lineRight, panel.top + 20.0f), 1.2f);
        drawLine(p.separator, D2D1::Point2F(lineLeft, panel.bottom - 20.0f),
                 D2D1::Point2F(lineRight, panel.bottom - 20.0f), 1.2f);

        const float preferredControlSize = std::clamp(panelH * 0.30f, 28.0f, 34.0f);
        constexpr float kControlGap = 10.0f;
        constexpr float kVolumeGap = 14.0f;
        const float preferredGroupW = preferredControlSize * 4.0f + kControlGap * 2.0f + kVolumeGap;
        // 卡片变窄时按同一比例缩放控件组，保留 4 DIP 安全边距，避免图标越界。
        const float availableW = std::max(0.0f, panel.right - panel.left - 8.0f);
        const float groupScale =
            preferredGroupW > 0.0f ? std::clamp(availableW / preferredGroupW, 0.0f, 1.0f) : 1.0f;
        const float controlSize = preferredControlSize * groupScale;
        const float controlGap = kControlGap * groupScale;
        const float volumeGap = kVolumeGap * groupScale;
        const float groupW = controlSize * 4.0f + controlGap * 2.0f + volumeGap;
        const float groupLeft = cx - groupW * 0.5f;
        const float previousX = groupLeft + controlSize * 0.5f;
        const float playX = previousX + controlSize + controlGap;
        const float nextX = playX + controlSize + controlGap;
        const float volumeX = nextX + controlSize + volumeGap;
        const D2D1_POINT_2F previous = D2D1::Point2F(previousX, cy);
        const D2D1_POINT_2F play = D2D1::Point2F(playX, cy);
        const D2D1_POINT_2F next = D2D1::Point2F(nextX, cy);
        const D2D1_POINT_2F volume = D2D1::Point2F(volumeX, cy);
        const float iconRadius = controlSize * 0.25f;
        const D2D1_COLOR_F iconColor = enabled ? p.text : p.disabled;
        drawControl(0, false, previous, iconRadius, iconColor);
        drawControl(1, false, play, iconRadius, iconColor);
        drawControl(2, false, next, iconRadius, iconColor);
        if (auto* brush = painter.brush(tone(iconColor)))
            media_control::drawVolume(painter.target(), volume, iconRadius * 0.8f, brush,
                                      enabled ? 3 : 0);
        media_control::release(controls);
        return;
    }

    // 媒体卡片按实际弹窗 384x208 DIP 等比缩放，结构顺序也与真实弹窗一致。
    const float panelW = panel.right - panel.left;
    const float panelH = panel.bottom - panel.top;
    const float popupScale = std::min(panelW / 384.0f, panelH / 208.0f);
    const float popupW = 384.0f * popupScale;
    const float popupH = 208.0f * popupScale;
    const float originX = panel.left + (panelW - popupW) * 0.5f;
    const float originY = panel.top + (panelH - popupH) * 0.5f;
    const auto popupPoint = [=](float x, float y) {
        return D2D1::Point2F(originX + x * popupScale, originY + y * popupScale);
    };
    const auto popupRect = [=](float left, float top, float right, float bottom) {
        return D2D1::RectF(originX + left * popupScale, originY + top * popupScale,
                           originX + right * popupScale, originY + bottom * popupScale);
    };

    const D2D1_COLOR_F primary = enabled ? p.text : p.disabled;
    const D2D1_COLOR_F secondary = enabled ? p.textSecondary : p.disabled;

    // 来源行：左侧是应用图标，右侧留出真实媒体卡片的音量入口。
    const D2D1_RECT_F sourceIcon = popupRect(16.0f, 14.0f, 34.0f, 32.0f);
    painter.fillRoundRect(tone(p.accent), sourceIcon, 4.0f);
    drawLine(p.textOnAccent, popupPoint(21.0f, 20.0f), popupPoint(29.0f, 20.0f), 1.3f);
    drawLine(p.textOnAccent, popupPoint(21.0f, 24.0f), popupPoint(27.0f, 24.0f), 1.3f);
    drawLine(primary, popupPoint(42.0f, 23.0f), popupPoint(126.0f, 23.0f), 1.5f);
    const D2D1_RECT_F volumeButton = popupRect(344.0f, 8.0f, 376.0f, 40.0f);
    painter.fillRoundRect(tone(p.controlHover), volumeButton, 16.0f);
    if (auto* brush = painter.brush(tone(secondary)))
        media_control::drawVolume(painter.target(), popupPoint(360.0f, 24.0f), 7.0f * popupScale,
                                  brush, 3);

    // 封面与歌曲信息：封面比例保持 80x80，右侧用不同长度的线表现标题/歌手。
    drawCoverEffectArtwork(painter, popupRect(16.0f, 44.0f, 96.0f, 124.0f), false, enabled);
    drawLine(primary, popupPoint(112.0f, 57.0f), popupPoint(286.0f, 57.0f), 2.0f);
    drawLine(secondary, popupPoint(112.0f, 86.0f), popupPoint(236.0f, 86.0f), 1.6f);
    drawLine(secondary, popupPoint(112.0f, 97.0f), popupPoint(198.0f, 97.0f), 1.2f);

    // 进度：上方两段短时间标记，下面是实际弹窗的 4 DIP 进度轨道。
    drawLine(secondary, popupPoint(16.0f, 136.0f), popupPoint(42.0f, 136.0f), 1.4f);
    drawLine(secondary, popupPoint(342.0f, 136.0f), popupPoint(368.0f, 136.0f), 1.4f);
    const D2D1_RECT_F progressTrack = popupRect(16.0f, 148.0f, 368.0f, 152.0f);
    painter.fillRoundRect(tone(p.separator), progressTrack, 2.0f);
    painter.fillRoundRect(tone(p.accent), popupRect(16.0f, 148.0f, 178.0f, 152.0f), 2.0f);

    // 底部三键使用真实控件尺寸关系：两侧 36 DIP，中间 40 DIP。
    const float controlY = originY + 182.0f * popupScale;
    const float centerX = originX + 192.0f * popupScale;
    const float sideSize = 36.0f * popupScale;
    const float playSize = 40.0f * popupScale;
    const D2D1_POINT_2F previous = D2D1::Point2F(centerX - 84.0f * popupScale, controlY);
    const D2D1_POINT_2F play = D2D1::Point2F(centerX, controlY);
    const D2D1_POINT_2F next = D2D1::Point2F(centerX + 84.0f * popupScale, controlY);
    fillButton(p.controlHover, previous, sideSize);
    fillButton(p.accent, play, playSize);
    fillButton(p.controlHover, next, sideSize);
    drawControl(0, false, previous, 9.0f * popupScale, primary);
    drawControl(1, false, play, 11.0f * popupScale, p.textOnAccent);
    drawControl(2, false, next, 9.0f * popupScale, primary);
    media_control::release(controls);
}

void SettingsArtwork::drawIdleQuoteBackgroundArtwork(fluent::FluentDialogSurface::Painter& painter,
                                                     const D2D1_RECT_F& bounds, int style,
                                                     bool enabled) {
    auto* rt = painter.target();
    if (!rt)
        return;

    const auto& p = painter.colors();
    const auto tone = [enabled](D2D1_COLOR_F color) {
        if (!enabled)
            color.a *= 0.42f;
        return color;
    };
    const auto withOpacity = [](D2D1_COLOR_F color, float opacity) {
        color.a *= opacity;
        return color;
    };
    const auto drawLine = [&](D2D1_COLOR_F color, D2D1_POINT_2F from, D2D1_POINT_2F to,
                              float stroke) {
        if (auto* brush = painter.brush(tone(color)))
            rt->DrawLine(from, to, brush, stroke);
    };

    const D2D1_RECT_F panel = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f,
                                          bounds.right - 1.0f, bounds.bottom - 1.0f);
    painter.fillRoundRect(tone(p.controlFill), panel, 8.0f);
    painter.fillRoundRect(tone(withOpacity(p.accent, 0.055f)), panel, 8.0f);
    painter.strokeRoundRect(tone(withOpacity(p.cardStroke, 0.72f)), panel, 1.0f, 8.0f);

    const float left = panel.left + 8.0f;
    const float right = panel.right - 8.0f;
    const float top = panel.top + 7.0f;
    const float bottom = panel.bottom - 7.0f;
    const float width = std::max(0.0f, right - left);
    const float height = std::max(0.0f, bottom - top);
    if (width <= 0.0f || height <= 0.0f)
        return;

    rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    switch (style) {
    case 1: { // 落叶
        struct Leaf {
            float x;
            float y;
            float size;
            float angle;
        };
        static constexpr Leaf leaves[] = {
            {0.04f, 0.30f, 5.0f, -28.0f}, {0.14f, 0.76f, 3.6f, 22.0f},
            {0.23f, 0.16f, 4.2f, 48.0f},  {0.34f, 0.58f, 5.6f, -12.0f},
            {0.46f, 0.86f, 3.8f, 34.0f},  {0.57f, 0.28f, 4.8f, -46.0f},
            {0.69f, 0.70f, 4.0f, 16.0f},  {0.80f, 0.12f, 5.2f, -34.0f},
            {0.92f, 0.48f, 4.0f, 42.0f},
        };
        for (size_t i = 0; i < _countof(leaves); ++i) {
            const float x = left + leaves[i].x * width;
            const float y = top + leaves[i].y * height;
            const D2D1_POINT_2F center = D2D1::Point2F(x, y);
            const D2D1_COLOR_F color = withOpacity(p.accent, i % 3 == 0 ? 0.90f : 0.68f);
            if (auto* brush = painter.brush(tone(color))) {
                D2D1_MATRIX_3X2_F previous{};
                rt->GetTransform(&previous);
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(leaves[i].angle, center) * previous);
                rt->FillEllipse(D2D1::Ellipse(center, leaves[i].size, leaves[i].size * 0.44f),
                                brush);
                rt->DrawLine(D2D1::Point2F(x - leaves[i].size * 0.72f, y),
                             D2D1::Point2F(x + leaves[i].size * 0.72f, y), brush, 0.8f);
                rt->SetTransform(previous);
            }
        }
        drawLine(withOpacity(p.textSecondary, 0.55f), D2D1::Point2F(left, bottom - 2.0f),
                 D2D1::Point2F(right, bottom - 2.0f), 0.8f);
        break;
    }
    case 2: { // 闪烁星星
        struct Star {
            float x;
            float y;
            float radius;
            bool cross;
        };
        static constexpr Star stars[] = {
            {0.04f, 0.22f, 1.5f, true},  {0.11f, 0.72f, 1.0f, false}, {0.18f, 0.44f, 1.2f, false},
            {0.25f, 0.14f, 1.0f, false}, {0.31f, 0.84f, 1.4f, true},  {0.38f, 0.36f, 0.9f, false},
            {0.45f, 0.64f, 1.1f, false}, {0.52f, 0.16f, 1.3f, true},  {0.59f, 0.48f, 0.9f, false},
            {0.66f, 0.78f, 1.2f, false}, {0.73f, 0.28f, 1.0f, true},  {0.80f, 0.58f, 1.4f, false},
            {0.88f, 0.12f, 0.9f, false}, {0.95f, 0.82f, 1.2f, true},
        };
        for (size_t i = 0; i < _countof(stars); ++i) {
            const D2D1_POINT_2F center =
                D2D1::Point2F(left + stars[i].x * width, top + stars[i].y * height);
            const D2D1_COLOR_F color = withOpacity(p.accent, stars[i].cross ? 0.90f : 0.64f);
            if (auto* brush = painter.brush(tone(color))) {
                rt->FillEllipse(D2D1::Ellipse(center, stars[i].radius, stars[i].radius), brush);
                if (stars[i].cross) {
                    const float ray = stars[i].radius * 2.8f;
                    rt->DrawLine(D2D1::Point2F(center.x - ray, center.y),
                                 D2D1::Point2F(center.x + ray, center.y), brush, 0.8f);
                    rt->DrawLine(D2D1::Point2F(center.x, center.y - ray),
                                 D2D1::Point2F(center.x, center.y + ray), brush, 0.8f);
                }
            }
        }
        break;
    }
    case 3: { // 二进制
        auto* format = painter.textFormat(11.0f, 600, true, true);
        if (format) {
            constexpr wchar_t digits[] = L"101100101011010010110101";
            constexpr int kColumns = 8;
            constexpr int kRows = 3;
            const float columnW = width / static_cast<float>(kColumns);
            const float rowH = height / static_cast<float>(kRows);
            for (int col = 0; col < kColumns; ++col) {
                for (int row = 0; row < kRows; ++row) {
                    const int index =
                        (col * 3 + row * 5) % (static_cast<int>(_countof(digits)) - 1);
                    const wchar_t digit[2] = {digits[index], L'\0'};
                    const D2D1_RECT_F rect =
                        D2D1::RectF(left + col * columnW, top + row * rowH,
                                    left + (col + 1) * columnW, top + (row + 1) * rowH);
                    painter.drawText(std::wstring(digit), format, rect,
                                     tone(withOpacity(p.accent, 0.52f + ((col + row) % 3) * 0.16f)),
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
        break;
    }
    case 4: { // 流光粒子
        constexpr int kFlowLines = 3;
        constexpr int kFlowSamples = 28;
        constexpr float kTwoPi = 6.28318530718f;
        const auto flowY = [&](float x, float lane, float phase) {
            const float t = std::clamp((x - left) / width, 0.0f, 1.0f);
            const float center = 0.28f + lane * 0.22f;
            const float wave = std::sin(t * kTwoPi * 1.15f + phase) * 0.07f +
                               std::sin(t * kTwoPi * 2.25f + phase * 0.65f) * 0.035f;
            return top + (center + wave) * height;
        };
        for (int lane = 0; lane < kFlowLines; ++lane) {
            const float phase = 0.45f + lane * 1.12f;
            D2D1_POINT_2F previous =
                D2D1::Point2F(left, flowY(left, static_cast<float>(lane), phase));
            for (int sample = 1; sample <= kFlowSamples; ++sample) {
                const float t = static_cast<float>(sample) / kFlowSamples;
                const float x = left + width * t;
                const D2D1_POINT_2F current =
                    D2D1::Point2F(x, flowY(x, static_cast<float>(lane), phase));
                drawLine(
                    withOpacity(lane == 1 ? p.accent : p.textSecondary, lane == 1 ? 0.34f : 0.20f),
                    previous, current, lane == 1 ? 1.4f : 0.9f);
                previous = current;
            }
        }

        struct Particle {
            float x;
            float lane;
            float radius;
            float phase;
            bool accent;
        };
        static constexpr Particle particles[] = {
            {0.04f, 0.0f, 1.5f, 0.3f, true}, {0.10f, 1.0f, 1.1f, 1.8f, false},
            {0.17f, 2.0f, 1.8f, 3.4f, true}, {0.25f, 0.0f, 1.0f, 4.6f, false},
            {0.32f, 1.0f, 2.0f, 5.5f, true}, {0.39f, 2.0f, 1.2f, 0.8f, false},
            {0.47f, 0.0f, 1.4f, 2.6f, true}, {0.54f, 1.0f, 1.0f, 4.0f, false},
            {0.61f, 2.0f, 1.9f, 5.1f, true}, {0.68f, 0.0f, 1.1f, 1.2f, false},
            {0.75f, 1.0f, 1.7f, 2.2f, true}, {0.82f, 2.0f, 1.0f, 3.8f, false},
            {0.89f, 0.0f, 1.8f, 5.0f, true}, {0.96f, 1.0f, 1.2f, 0.6f, false},
        };
        for (size_t i = 0; i < _countof(particles); ++i) {
            const float x = left + particles[i].x * width;
            const float y = flowY(x, particles[i].lane, particles[i].phase);
            const D2D1_POINT_2F center = D2D1::Point2F(x, y);
            const D2D1_COLOR_F base = particles[i].accent ? p.accent : p.textSecondary;
            if (auto* brush = painter.brush(tone(withOpacity(base, 0.14f))))
                rt->FillEllipse(
                    D2D1::Ellipse(center, particles[i].radius * 2.3f, particles[i].radius * 2.3f),
                    brush);
            if (auto* brush =
                    painter.brush(tone(withOpacity(base, particles[i].accent ? 0.92f : 0.72f)))) {
                const float trail = 5.0f + particles[i].radius * 2.8f;
                rt->DrawLine(D2D1::Point2F(x - trail, y + 0.5f),
                             D2D1::Point2F(x - particles[i].radius, y), brush, 0.9f);
                rt->FillEllipse(D2D1::Ellipse(center, particles[i].radius, particles[i].radius),
                                brush);
                if (particles[i].accent && i % 3 == 0) {
                    const float ray = particles[i].radius * 2.1f;
                    rt->DrawLine(D2D1::Point2F(x - ray, y), D2D1::Point2F(x + ray, y), brush,
                                 0.65f);
                    rt->DrawLine(D2D1::Point2F(x, y - ray), D2D1::Point2F(x, y + ray), brush,
                                 0.65f);
                }
            }
        }
        break;
    }
    default:
        break;
    }
    rt->PopAxisAlignedClip();
}
