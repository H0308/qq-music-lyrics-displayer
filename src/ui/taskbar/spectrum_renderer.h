#pragma once
#include "model/display_types.h"
#include "ui/lyric_window.h"
#include "ui/spectrum_effects.h"
#include "ui/taskbar/animation_clock.h"
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace taskbar_detail {
inline constexpr float kSpectrumBarW = 5.0f; // 频谱柱宽
inline constexpr float kSpectrumGap = 3.0f;  // 频谱柱间隙
inline constexpr float kImmersiveSpectrumWideZoneThreshold = 340.0f;
inline constexpr int kImmersiveSpectrumWideBarCount = 32;
inline constexpr int kImmersiveSpectrumBarCount = 24;
inline constexpr float kSpectrumBottomPadding = 1.0f;
inline constexpr float kSpectrumBarRadius = 2.0f; // 轻微圆角，保持柱状感
} // namespace taskbar_detail
struct SpectrumContext {
    ID2D1DeviceContext* target = nullptr; // 仅在当前同步调用期间借用
    bool visible{};
    bool animations{};
    DisplayScene scene{};
    ULONGLONG nowMs{};
    SpectrumStyle style{};
    bool customColor{};
    COLORREF color{};
    bool followAlbum{};
    COLORREF albumColor{};
    bool hasAlbumColor{};
    bool gradient{};
    uint32_t effects{};
    bool background{};
    int opacity{};
    COLORREF playedColor{};
    float compactHeight{};
    bool playing = false;
    bool minimal = false;
    bool stopped = false;
    bool horizontalImmersive = false;
};

// 频谱独占电平、峰值、火花、历史帧和全部 GPU 效果资源。
class SpectrumRenderer {
    struct SpectrumSpark;

public:
    ~SpectrumRenderer() { discardDeviceResources(); }
    void setContext(const SpectrumContext& context) { context_ = context; }
    void setBands(const std::array<float, kPresentationSpectrumBands>& bands) {
        spectrumBands_ = bands;
    }
    void clearBands() { spectrumBands_.fill(0.0f); }
    const auto& bands() const { return spectrumBands_; }
    bool consumeRedraw() { return std::exchange(redraw_, false); }
    void createBrushes();
    void discardDeviceResources();
    void drawCluster(float x, float height, float width, int count);
    bool backgroundWaveEnabled() const;
    bool integratedSpectrum() const;
    void resetSpectrumVisuals();
    bool spectrumEffectEnabled(SpectrumEffect effect) const;
    void resetSpectrumEffectHistory();
    void releaseSpectrumEffectResources();
    bool spectrumEffectsAnimating() const;
    void updateSpectrumEffects(ULONGLONG now);
    bool spectrumVisualsAnimating() const;
    void updateSpectrumVisuals(ULONGLONG now);
    bool backgroundWaveAnimating() const;
    float spectrumClusterW() const;
    float spectrumVisualClusterW(int barCount) const;
    int immersiveSpectrumBarCount(float visualW) const;
    float spectrumLevel(int index) const;
    void fillSpectrumBar(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& bar);
    D2D1_COLOR_F spectrumColorForLevel(float level, float alphaScale = 1.0f) const;
    float spectrumVisualLevel(int index, int visualCount) const;
    void drawDefaultSpectrum(float x, float h, int visualCount);
    void drawBarSpectrum(float x, float h, int visualCount);
    void drawSegmentedSpectrum(float x, float h, int visualCount);
    bool ensureSpectrumEffectResources(float width, float height);
    float spectrumSparkOriginY(const SpectrumSpark& spark, float h) const;
    void drawBottomSpectrum(float x, float h, float width);
    void drawBottomSpectrumShape(float x, float h, float width);
    void drawWaveSpectrum(float x, float h, float width, float opacityScale);
    void drawDreamyWaveSpectrum(float x, float h, float width);
    void drawBackgroundWaveSpectrum(float x, float h, float width);
    void drawSpectrum(float x, float h, float width);
    void drawSpectrumShape(float x, float h, float width);

private:
    SpectrumContext context_;
    bool redraw_ = false;
    ID2D1DeviceContext* drawTargetOverride_ = nullptr;
    ID2D1DeviceContext* drawTarget() const {
        return drawTargetOverride_ ? drawTargetOverride_ : context_.target;
    }
    void requestFrame() { redraw_ = true; }
    static ULONGLONG monotonicNowMs() { return taskbar_detail::monotonicNowMs(); }
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brushSpectrumBarGradient_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSpectrum_;
    std::array<float, kPresentationSpectrumBands> spectrumBands_{};
    std::array<float, kPresentationSpectrumBands> spectrumPeaks_{};
    std::array<ULONGLONG, kPresentationSpectrumBands> spectrumPeakHoldUntil_{};
    ULONGLONG spectrumVisualTickMs_ = 0;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> spectrumFxContext_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> spectrumFxFrame_;
    std::array<Microsoft::WRL::ComPtr<ID2D1Bitmap1>, 4> spectrumFxHistory_{};
    std::array<ULONGLONG, 4> spectrumFxHistoryMs_{};
    size_t spectrumFxHistoryNext_ = 0;
    ULONGLONG spectrumFxCaptureMs_ = 0;
    Microsoft::WRL::ComPtr<ID2D1Effect> spectrumGlowFx_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> spectrumSheenBrush_;
    struct SpectrumSpark {
        ULONGLONG bornMs = 0;
        float level = 0.0f;
        int band = 0;
        float drift = 0.0f;
    };
    std::array<SpectrumSpark, 24> spectrumSparks_{};
    std::array<float, kPresentationSpectrumBands> spectrumPreviousLevels_{};
    std::array<ULONGLONG, kPresentationSpectrumBands> spectrumLastSparkMs_{};
    size_t spectrumSparkNext_ = 0;

    template <typename DrawShape>
    void drawSpectrumWithEffects(float x, float h, float width, DrawShape drawShape) {
        using namespace taskbar_detail;
        auto* rt = drawTarget();
        if (!rt || width <= 0.0f || h <= 0.0f)
            return;
        if (context_.effects == 0 || context_.minimal || context_.stopped) {
            drawShape();
            return;
        }
        if (!ensureSpectrumEffectResources(width, h)) {
            drawShape();
            return;
        }
        // 独立上下文不改动外层歌词和转场的 target、变换与裁剪栈。
        spectrumFxContext_->SetTarget(spectrumFxFrame_.Get());
        spectrumFxContext_->SetTransform(D2D1::Matrix3x2F::Translation(-x, 0.0f));
        spectrumFxContext_->BeginDraw();
        spectrumFxContext_->Clear(D2D1::ColorF(0, 0, 0, 0));
        auto* previousTarget = drawTargetOverride_;
        drawTargetOverride_ = spectrumFxContext_.Get();
        drawShape();
        drawTargetOverride_ = previousTarget;
        const HRESULT hr = spectrumFxContext_->EndDraw();
        spectrumFxContext_->SetTarget(nullptr);
        if (FAILED(hr)) {
            releaseSpectrumEffectResources();
            requestFrame();
            return;
        }
        const auto destination = D2D1::RectF(x, 0.0f, x + width, h);
        const auto source = D2D1::RectF(0.0f, 0.0f, width, h);
        const auto offset = D2D1::Point2F(x, 0.0f);
        const ULONGLONG now = context_.nowMs != 0 ? context_.nowMs : monotonicNowMs();
        const bool audioActive = std::any_of(spectrumBands_.begin(), spectrumBands_.end(),
                                             [](float level) { return level > 0.01f; });
        rt->PushAxisAlignedClip(destination, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (context_.animations && spectrumEffectEnabled(SpectrumEffect::Trail)) {
            for (size_t n = 0; n < spectrumFxHistory_.size(); ++n) {
                const size_t i = (spectrumFxHistoryNext_ + n) % spectrumFxHistory_.size();
                const ULONGLONG captured = spectrumFxHistoryMs_[i];
                if (captured == 0 || now - captured >= 220)
                    continue;
                const float age = static_cast<float>(now - captured) / 220.0f;
                rt->DrawBitmap(spectrumFxHistory_[i].Get(), &destination, (1.0f - age) * 0.18f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
            }
        }
        if (spectrumGlowFx_.Get()) {
            float energy = 0.0f;
            for (float level : spectrumBands_)
                energy = std::max(energy, level);
            spectrumGlowFx_->SetInput(0, spectrumFxFrame_.Get());
            spectrumGlowFx_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                                      1.6f + energy * 1.6f);
            rt->DrawImage(spectrumGlowFx_.Get(), offset, source);
        }
        rt->DrawBitmap(spectrumFxFrame_.Get(), &destination, 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
        if (spectrumSheenBrush_.Get() && context_.animations && context_.playing && audioActive) {
            const float phase = static_cast<float>(now % 2600ULL) / 2600.0f;
            const float bandW = std::clamp(width * 0.18f, 12.0f, 60.0f);
            const float center = x - bandW + phase * (width + 2.0f * bandW);
            spectrumSheenBrush_->SetStartPoint(D2D1::Point2F(center - bandW * 0.5f, 0));
            spectrumSheenBrush_->SetEndPoint(D2D1::Point2F(center + bandW * 0.5f, 0));
            const auto antialias = rt->GetAntialiasMode();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            rt->FillOpacityMask(spectrumFxFrame_.Get(), spectrumSheenBrush_.Get(), &destination,
                                &source);
            rt->SetAntialiasMode(antialias);
        }
        if (context_.animations && spectrumEffectEnabled(SpectrumEffect::Sparks) &&
            brushSpectrum_.Get()) {
            const auto original = brushSpectrum_->GetColor();
            const float opacity = backgroundWaveEnabled() ? context_.opacity / 100.0f : 1.0f;
            for (const auto& spark : spectrumSparks_) {
                if (spark.bornMs == 0 || now - spark.bornMs >= 420)
                    continue;
                const float age = static_cast<float>(now - spark.bornMs) / 420.0f;
                const float t = static_cast<float>(spark.band) / (kPresentationSpectrumBands - 1);
                float originX = t * width;
                if (context_.style == SpectrumStyle::BottomBand) {
                    const int count = std::max(1, static_cast<int>(width / 4.0f));
                    const float pitch = width / count;
                    originX = std::round(t * (count - 1)) * pitch + pitch * 0.3f;
                } else if (context_.style != SpectrumStyle::DreamyWave) {
                    const int count =
                        std::max(1, static_cast<int>(std::round((width + kSpectrumGap) /
                                                                (kSpectrumBarW + kSpectrumGap))));
                    originX = kSpectrumBarW * 0.5f +
                              std::round(t * (count - 1)) * (kSpectrumBarW + kSpectrumGap);
                }
                const float px = x + originX + spark.drift * age * 5.0f;
                const float py = spectrumSparkOriginY(spark, h) - age * 8.0f + age * age * 3.0f;
                brushSpectrum_->SetColor(original);
                brushSpectrum_->SetColor(
                    spectrumColorForLevel(spark.level, (1.0f - age) * 0.85f * opacity));
                rt->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(px, py), 0.9f - age * 0.45f, 0.9f - age * 0.45f),
                    brushSpectrum_.Get());
            }
            brushSpectrum_->SetColor(original);
        }
        rt->PopAxisAlignedClip();
        // 每 45ms 采一张，最多四张；静音后停止写入，让残影自然衰减。
        if (context_.animations && context_.playing &&
            spectrumEffectEnabled(SpectrumEffect::Trail) && audioActive &&
            (spectrumFxCaptureMs_ == 0 || now - spectrumFxCaptureMs_ >= 45)) {
            auto* bitmap = spectrumFxHistory_[spectrumFxHistoryNext_].Get();
            if (SUCCEEDED(bitmap->CopyFromBitmap(nullptr, spectrumFxFrame_.Get(), nullptr))) {
                spectrumFxHistoryMs_[spectrumFxHistoryNext_] = now;
                spectrumFxHistoryNext_ = (spectrumFxHistoryNext_ + 1) % spectrumFxHistory_.size();
                spectrumFxCaptureMs_ = now;
            }
        }
    }
};
