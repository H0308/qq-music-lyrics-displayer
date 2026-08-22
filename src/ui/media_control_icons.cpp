#include "media_control_icons.h"

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
    const float barWidth = 0.16f * scale;
    const float barHeight = 1.0f * scale;
    const float barRadius = barWidth * 0.45f;
    const float barX = index == 0 ? center.x - 0.72f * scale : center.x + 0.56f * scale;
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(barX, center.y - barHeight * 0.5f, barX + barWidth,
                                       center.y + barHeight * 0.5f),
                          barRadius, barRadius),
        brush);
    if (triangle) {
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x, center.y));
        target->FillGeometry(triangle, brush);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

} // namespace media_control
