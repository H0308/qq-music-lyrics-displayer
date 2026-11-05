#pragma once

#include <d2d1.h>
#include <wrl/client.h>
#include <cmath>

// 单位五角星只创建一次，跟唱时仅改变绘制矩阵。
class LyricStarShape {
public:
    void reset() { geometry_.Reset(); }

    void draw(ID2D1RenderTarget* target, D2D1_POINT_2F center, float radius,
              float angle, ID2D1Brush* fill, ID2D1Brush* outline = nullptr) {
        if (!target || !fill || radius <= 0.0f)
            return;
        if (!geometry_) {
            Microsoft::WRL::ComPtr<ID2D1Factory> factory;
            target->GetFactory(factory.GetAddressOf());
            if (!factory || FAILED(factory->CreatePathGeometry(geometry_.GetAddressOf())))
                return;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geometry_->Open(sink.GetAddressOf()))) {
                geometry_.Reset();
                return;
            }
            constexpr float pi = 3.14159265358979323846f;
            for (int i = 0; i < 10; ++i) {
                const float a = -pi * 0.5f + i * pi * 0.2f;
                const float r = i % 2 == 0 ? 1.0f : 0.45f;
                const auto p = D2D1::Point2F(std::cos(a) * r, std::sin(a) * r);
                if (i == 0)
                    sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                else
                    sink->AddLine(p);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) {
                geometry_.Reset();
                return;
            }
        }
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        const float c = std::cos(angle) * radius;
        const float s = std::sin(angle) * radius;
        target->SetTransform(D2D1::Matrix3x2F(c, s, -s, c, center.x, center.y) * previous);
        target->FillGeometry(geometry_.Get(), fill);
        if (outline)
            target->DrawGeometry(geometry_.Get(), outline, 0.65f / radius);
        target->SetTransform(previous);
    }

private:
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry_;
};
