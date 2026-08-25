#include "media_control_icons.h"

#include <cmath>

namespace media_control {
namespace {

bool createRoundedTriangle(ID2D1Factory* factory, int direction,
                           ID2D1PathGeometry** out) {
    if (!factory || !out)
        return false;
    *out = nullptr;
    if (FAILED(factory->CreatePathGeometry(out)) || !*out)
        return false;

    ID2D1GeometrySink* sink = nullptr;
    if (FAILED((*out)->Open(&sink)) || !sink) {
        (*out)->Release();
        *out = nullptr;
        return false;
    }

    const float tipX = direction * 0.55f;
    const float baseX = -direction * 0.35f;
    constexpr float radius = 0.10f;
    const float ux = direction * 0.8739f;
    constexpr float uy = 0.4856f;
    const D2D1_POINT_2F tip = {tipX, 0.0f};
    const D2D1_POINT_2F baseTop = {baseX, -0.5f};
    const D2D1_POINT_2F baseBottom = {baseX, 0.5f};

    sink->BeginFigure(D2D1::Point2F(baseTop.x + radius * ux, baseTop.y + radius * uy),
                      D2D1_FIGURE_BEGIN_FILLED);
    sink->AddQuadraticBezier(
        D2D1::QuadraticBezierSegment(baseTop, {baseTop.x, baseTop.y + radius}));
    sink->AddLine({baseBottom.x, baseBottom.y - radius});
    sink->AddQuadraticBezier(
        D2D1::QuadraticBezierSegment(baseBottom,
                                     {baseBottom.x + radius * ux,
                                      baseBottom.y - radius * uy}));
    sink->AddLine({tip.x - radius * ux, tip.y + radius * uy});
    sink->AddQuadraticBezier(
        D2D1::QuadraticBezierSegment(tip, {tip.x - radius * ux, tip.y - radius * uy}));
    sink->AddLine({baseTop.x + radius * ux, baseTop.y + radius * uy});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    const HRESULT closeHr = sink->Close();
    sink->Release();
    if (FAILED(closeHr)) {
        (*out)->Release();
        *out = nullptr;
        return false;
    }
    return true;
}

} // namespace

void release(Geometry& geometry) {
    auto releaseGeometry = [](ID2D1PathGeometry*& value) {
        if (value) {
            value->Release();
            value = nullptr;
        }
    };
    releaseGeometry(geometry.play);
    releaseGeometry(geometry.previous);
    releaseGeometry(geometry.next);
}

bool create(ID2D1Factory* factory, Geometry& geometry) {
    release(geometry);
    if (!factory || !createRoundedTriangle(factory, 1, &geometry.play) ||
        !createRoundedTriangle(factory, -1, &geometry.previous) ||
        !createRoundedTriangle(factory, 1, &geometry.next)) {
        release(geometry);
        return false;
    }
    return true;
}

void draw(ID2D1DeviceContext* target, const Geometry& geometry, int index, bool playing,
          const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush) {
    if (!target || !brush || radius <= 0.0f || index < 0 || index > 2)
        return;

    if (index == 1) {
        if (playing) {
            const float width = radius * 0.22f;
            const float gap = radius * 0.20f;
            const float height = radius * 0.55f;
            const float barRadius = width * 0.45f;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(center.x - gap - width, center.y - height,
                                               center.x - gap, center.y + height),
                                  barRadius, barRadius),
                brush);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(center.x + gap, center.y - height,
                                               center.x + gap + width, center.y + height),
                                  barRadius, barRadius),
                brush);
        } else if (geometry.play) {
            target->SetTransform(D2D1::Matrix3x2F::Scale(radius * 1.4f, radius * 1.4f) *
                                  D2D1::Matrix3x2F::Translation(center.x + radius * 0.05f,
                                                                 center.y));
            target->FillGeometry(geometry.play, brush);
            target->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        return;
    }

    ID2D1PathGeometry* triangle = index == 0 ? geometry.previous : geometry.next;
    const float scale = radius * 1.4f;
    // 竖条+三角的组合字形边界为 ∓0.72 ~ ±0.55（单位），整体平移 0.085 使视觉中心对齐
    const float centerShift = (index == 0 ? 0.085f : -0.085f) * scale;
    const float barWidth = 0.16f * scale;
    const float barHeight = 1.0f * scale;
    const float barRadius = barWidth * 0.45f;
    const float barX = (index == 0 ? center.x - 0.72f * scale : center.x + 0.56f * scale) +
                       centerShift;
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(barX, center.y - barHeight * 0.5f, barX + barWidth,
                                       center.y + barHeight * 0.5f),
                          barRadius, barRadius),
        brush);
    if (triangle) {
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x + centerShift, center.y));
        target->FillGeometry(triangle, brush);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

} // namespace media_control

namespace media_control {
namespace {

// 以 center 为原点、scale 缩放的单位坐标系下填充喇叭多边形。
void fillSpeaker(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float scale,
                 ID2D1Brush* brush) {
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory)
        return;
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || !sink) {
        geometry->Release();
        return;
    }
    // 喇叭 = 左侧矩形箱体 + 右侧外张梯形
    sink->BeginFigure(D2D1::Point2F(-0.62f, -0.22f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(-0.28f, -0.22f));
    sink->AddLine(D2D1::Point2F(0.06f, -0.52f));
    sink->AddLine(D2D1::Point2F(0.06f, 0.52f));
    sink->AddLine(D2D1::Point2F(-0.28f, 0.22f));
    sink->AddLine(D2D1::Point2F(-0.62f, 0.22f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    const HRESULT hr = sink->Close();
    sink->Release();
    if (SUCCEEDED(hr)) {
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x, center.y));
        target->FillGeometry(geometry, brush);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    geometry->Release();
}

// 喇叭右侧的声波圆弧：radius 为弧半径（单位坐标），stroke 为线宽（DIP）。
void strokeWaveArc(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float scale,
                   float radius, float stroke, ID2D1Brush* brush) {
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory)
        return;
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || !sink) {
        geometry->Release();
        return;
    }
    // 以喇叭口 (0.06, 0) 为圆心，向右张开 ±42°（D2D 坐标系 y 向下）
    constexpr float kAngle = 42.0f * 3.14159265f / 180.0f;
    const float cx = 0.06f;
    const float cosA = std::cos(kAngle);
    const float sinA = std::sin(kAngle);
    sink->BeginFigure(D2D1::Point2F(cx + radius * cosA, -radius * sinA),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    D2D1_ARC_SEGMENT arc{};
    arc.point = D2D1::Point2F(cx + radius * cosA, radius * sinA);
    arc.size = D2D1::SizeF(radius, radius);
    arc.rotationAngle = 0.0f;
    arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    arc.arcSize = D2D1_ARC_SIZE_SMALL;
    sink->AddArc(arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    const HRESULT hr = sink->Close();
    sink->Release();
    if (SUCCEEDED(hr)) {
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x, center.y));
        target->DrawGeometry(geometry, brush, stroke / scale);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    geometry->Release();
}

} // namespace

void drawVolume(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float radius,
                ID2D1Brush* brush, int level) {
    if (!target || !brush || radius <= 0.0f)
        return;
    const float scale = radius * 1.4f;
    // 各档字形边界不同（喇叭 [-0.62,0.06]，一道波到 0.58，两道波/叉到 0.94），
    // 按档位平移使视觉中心对齐 center
    static constexpr float kCenterShift[4] = {-0.16f, 0.28f, 0.02f, -0.16f};
    const int lvl = level < 0 ? 0 : level > 3 ? 3 : level;
    const D2D1_POINT_2F c{center.x + kCenterShift[lvl] * scale, center.y};
    fillSpeaker(target, c, scale, brush);
    if (lvl <= 0) {
        // 静音：右侧画叉
        const float x0 = c.x + scale * 0.42f;
        const float y0 = c.y - scale * 0.30f;
        const float x1 = c.x + scale * 0.94f;
        const float y1 = c.y + scale * 0.30f;
        const float stroke = radius * 0.22f;
        target->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, stroke);
        target->DrawLine(D2D1::Point2F(x0, y1), D2D1::Point2F(x1, y0), brush, stroke);
        return;
    }
    if (lvl >= 2)
        strokeWaveArc(target, c, scale, 0.52f, radius * 0.20f, brush);
    if (lvl >= 3)
        strokeWaveArc(target, c, scale, 0.88f, radius * 0.20f, brush);
}

} // namespace media_control
