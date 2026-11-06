#include "ui/taskbar/spectrum_renderer.h"
#include "logging/runtime_logger.h"
using namespace taskbar_detail;

void SpectrumRenderer::createBrushes() {
    auto* rt = drawTarget();
    if (!rt)
        return;
    brushSpectrum_.Reset();
    brushSpectrumBarGradient_.Reset();
    const auto rgb = [](COLORREF c, float a) {
        return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
    };
    const COLORREF spectrumBaseColor = context_.customColor ? context_.color
                                       : context_.followAlbum && context_.hasAlbumColor
                                           ? context_.albumColor
                                           : context_.playedColor;
    const D2D1_COLOR_F spectrumBase = rgb(spectrumBaseColor, 0.60f);
    rt->CreateSolidColorBrush(spectrumBase, brushSpectrum_.ReleaseAndGetAddressOf());

    // 每根柱子由底部深色连续过渡到顶部浅色，不插入分层色带。
    const D2D1_COLOR_F spectrumDark =
        D2D1::ColorF(spectrumBase.r * kSpectrumBarGradientDarkFactor,
                     spectrumBase.g * kSpectrumBarGradientDarkFactor,
                     spectrumBase.b * kSpectrumBarGradientDarkFactor, spectrumBase.a);
    const D2D1_COLOR_F spectrumLight = D2D1::ColorF(
        spectrumBase.r + (1.0f - spectrumBase.r) * kSpectrumBarGradientLightMix,
        spectrumBase.g + (1.0f - spectrumBase.g) * kSpectrumBarGradientLightMix,
        spectrumBase.b + (1.0f - spectrumBase.b) * kSpectrumBarGradientLightMix, spectrumBase.a);
    const D2D1_GRADIENT_STOP barGradientStops[] = {
        {0.0f, spectrumDark},
        {1.0f, spectrumLight},
    };
    ID2D1GradientStopCollection* barGradientStopCollection = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(barGradientStops, _countof(barGradientStops),
                                                   &barGradientStopCollection)) &&
        barGradientStopCollection) {
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                          D2D1::Point2F(0.0f, 1.0f), D2D1::Point2F(0.0f, 0.0f)),
                                      barGradientStopCollection,
                                      brushSpectrumBarGradient_.ReleaseAndGetAddressOf());
        barGradientStopCollection->Release();
    }
}
void SpectrumRenderer::discardDeviceResources() {
    releaseSpectrumEffectResources();
    brushSpectrum_.Reset();
    brushSpectrumBarGradient_.Reset();
    context_ = {};
    drawTargetOverride_ = nullptr;
}
void SpectrumRenderer::drawCluster(float x, float height, float width, int count) {
    drawSpectrumWithEffects(x, height, width, [&] {
        if (context_.style == SpectrumStyle::Segmented)
            drawSegmentedSpectrum(x, height, count);
        else if (context_.style == SpectrumStyle::Bars)
            drawBarSpectrum(x, height, count);
        else
            drawDefaultSpectrum(x, height, count);
    });
}
bool SpectrumRenderer::backgroundWaveEnabled() const {
    return context_.background && context_.style == SpectrumStyle::DreamyWave;
}

bool SpectrumRenderer::integratedSpectrum() const {
    return context_.style == SpectrumStyle::BottomBand;
}

void SpectrumRenderer::resetSpectrumVisuals() {
    spectrumPeaks_.fill(0.0f);
    spectrumPeakHoldUntil_.fill(0);
    spectrumVisualTickMs_ = 0;
    resetSpectrumEffectHistory();
}

bool SpectrumRenderer::spectrumEffectEnabled(SpectrumEffect effect) const {
    return (context_.effects & spectrumEffectBit(effect)) != 0;
}

void SpectrumRenderer::resetSpectrumEffectHistory() {
    spectrumFxHistoryMs_.fill(0);
    spectrumFxHistoryNext_ = 0;
    spectrumFxCaptureMs_ = 0;
    spectrumSparks_.fill({});
    spectrumPreviousLevels_.fill(0.0f);
    spectrumLastSparkMs_.fill(0);
    spectrumSparkNext_ = 0;
}

void SpectrumRenderer::releaseSpectrumEffectResources() {
    if (spectrumFxContext_)
        spectrumFxContext_->SetTarget(nullptr);
    spectrumGlowFx_.Reset();
    spectrumSheenBrush_.Reset();
    spectrumFxFrame_.Reset();
    for (auto& bitmap : spectrumFxHistory_)
        bitmap.Reset();
    spectrumFxContext_.Reset();
    resetSpectrumEffectHistory();
}

bool SpectrumRenderer::spectrumEffectsAnimating() const {
    if (!context_.visible || !context_.animations || context_.scene == DisplayScene::Idle ||
        context_.minimal || context_.stopped)
        return false;
    const ULONGLONG now = context_.nowMs;
    for (const auto& spark : spectrumSparks_)
        if (spark.bornMs != 0 && now - spark.bornMs < 420)
            return true;
    for (ULONGLONG captured : spectrumFxHistoryMs_)
        if (captured != 0 && now - captured < 220)
            return true;
    if (spectrumEffectEnabled(SpectrumEffect::Sheen) && context_.playing) {
        for (float level : spectrumBands_)
            if (level > 0.01f)
                return true;
    }
    return false;
}

void SpectrumRenderer::updateSpectrumEffects(ULONGLONG now) {
    if (!context_.visible || !context_.animations || context_.scene == DisplayScene::Idle ||
        context_.minimal || context_.stopped) {
        resetSpectrumEffectHistory();
        return;
    }
    bool expired = false;
    for (auto& spark : spectrumSparks_) {
        if (spark.bornMs != 0 && now - spark.bornMs >= 420) {
            spark.bornMs = 0;
            expired = true;
        }
    }
    for (auto& captured : spectrumFxHistoryMs_) {
        if (captured != 0 && now - captured >= 220) {
            captured = 0;
            expired = true;
        }
    }
    for (int band = 0; band < kPresentationSpectrumBands; ++band) {
        const float level = spectrumLevel(band);
        if (spectrumEffectEnabled(SpectrumEffect::Sparks) && context_.playing && level > 0.35f &&
            level - spectrumPreviousLevels_[band] > 0.12f &&
            now - spectrumLastSparkMs_[band] >= 90) {
            spectrumLastSparkMs_[band] = now;
            for (int i = 0; i < 2; ++i) {
                spectrumSparks_[spectrumSparkNext_] = {now, level, band, i == 0 ? -1.0f : 1.0f};
                spectrumSparkNext_ = (spectrumSparkNext_ + 1) % spectrumSparks_.size();
            }
        }
        spectrumPreviousLevels_[band] = level;
    }
    if (expired || spectrumEffectsAnimating())
        requestFrame();
}

bool SpectrumRenderer::spectrumVisualsAnimating() const {
    if (!context_.visible || context_.style != SpectrumStyle::Segmented)
        return false;
    for (int i = 0; i < kPresentationSpectrumBands; ++i) {
        if (spectrumPeaks_[i] > 0.01f)
            return true;
    }
    return false;
}

void SpectrumRenderer::updateSpectrumVisuals(ULONGLONG now) {
    if (!context_.visible || context_.style != SpectrumStyle::Segmented)
        return;
    const float dt = spectrumVisualTickMs_ == 0
                         ? 0.0f
                         : static_cast<float>(now - spectrumVisualTickMs_) / 1000.0f;
    spectrumVisualTickMs_ = now;
    bool changed = false;
    for (int i = 0; i < kPresentationSpectrumBands; ++i) {
        const float level = spectrumLevel(i);
        const float previous = spectrumPeaks_[i];
        if (!context_.animations || level >= previous) {
            spectrumPeaks_[i] = level;
            spectrumPeakHoldUntil_[i] = now + 180;
        } else if (now > spectrumPeakHoldUntil_[i]) {
            const float decaySeconds =
                std::min(dt, static_cast<float>(now - spectrumPeakHoldUntil_[i]) / 1000.0f);
            spectrumPeaks_[i] = std::max(level, previous - decaySeconds * 0.9f);
        }
        changed = changed || spectrumPeaks_[i] != previous;
    }
    if (changed)
        requestFrame();
}

bool SpectrumRenderer::backgroundWaveAnimating() const {
    // 波浪的相位独立于播放状态和音频电平，暂停后仍需要连续刷新。
    return context_.animations && context_.visible && backgroundWaveEnabled() &&
           context_.scene != DisplayScene::Idle && !context_.minimal && !context_.stopped;
}

float SpectrumRenderer::spectrumClusterW() const {
    return kPresentationSpectrumBands * kSpectrumBarW +
           (kPresentationSpectrumBands - 1) * kSpectrumGap;
}

float SpectrumRenderer::spectrumVisualClusterW(int barCount) const {
    const int count = std::max(1, barCount);
    return count * kSpectrumBarW + (count - 1) * kSpectrumGap;
}

int SpectrumRenderer::immersiveSpectrumBarCount(float visualW) const {
    const int target = visualW >= kImmersiveSpectrumWideZoneThreshold
                           ? kImmersiveSpectrumWideBarCount
                           : kImmersiveSpectrumBarCount;
    const int capacity = std::max(
        1, static_cast<int>(std::floor((visualW + kSpectrumGap) / (kSpectrumBarW + kSpectrumGap))));
    return std::min(target, capacity);
}

float SpectrumRenderer::spectrumLevel(int index) const {
    return std::clamp(spectrumBands_[index], 0.0f, 1.0f);
}

void SpectrumRenderer::fillSpectrumBar(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& bar) {
    if (!rt || !brushSpectrum_.Get())
        return;
    if (context_.gradient && brushSpectrumBarGradient_.Get()) {
        const float centerX = (bar.rect.left + bar.rect.right) * 0.5f;
        brushSpectrumBarGradient_->SetStartPoint(D2D1::Point2F(centerX, bar.rect.bottom));
        brushSpectrumBarGradient_->SetEndPoint(D2D1::Point2F(centerX, bar.rect.top));
        rt->FillRoundedRectangle(bar, brushSpectrumBarGradient_.Get());
        return;
    }
    rt->FillRoundedRectangle(bar, brushSpectrum_.Get());
}

D2D1_COLOR_F SpectrumRenderer::spectrumColorForLevel(float level, float alphaScale) const {
    const D2D1_COLOR_F base = brushSpectrum_->GetColor();
    if (!context_.gradient)
        return D2D1::ColorF(base.r, base.g, base.b, base.a * alphaScale);

    // 只在当前频谱基色的基础上向白色轻微提亮，波动越高提亮越多，保持同色系关系。
    const float lift = std::clamp(level, 0.0f, 1.0f) * 0.30f;
    return D2D1::ColorF(base.r + (1.0f - base.r) * lift, base.g + (1.0f - base.g) * lift,
                        base.b + (1.0f - base.b) * lift, base.a * alphaScale);
}

float SpectrumRenderer::spectrumVisualLevel(int index, int visualCount) const {
    if (visualCount <= 1)
        return spectrumLevel(0);
    const float position = static_cast<float>(index) / static_cast<float>(visualCount - 1) *
                           static_cast<float>(kPresentationSpectrumBands - 1);
    const int leftBand = std::min(kPresentationSpectrumBands - 2, static_cast<int>(position));
    const float local = position - static_cast<float>(leftBand);
    const float smoothLocal = local * local * (3.0f - 2.0f * local);
    return spectrumLevel(leftBand) +
           (spectrumLevel(leftBand + 1) - spectrumLevel(leftBand)) * smoothLocal;
}

void SpectrumRenderer::drawDefaultSpectrum(float x, float h, int visualCount) {
    auto* rt = drawTarget();
    if (!rt || !brushSpectrum_.Get())
        return;
    const int n = std::max(1, visualCount);
    const float cy = h * 0.5f;
    const float maxH = h * 0.74f;
    constexpr float minH = 4.0f; // 静音时也保留小柱，不消失
    for (int i = 0; i < n; ++i) {
        const float level = spectrumVisualLevel(i, n);
        const float bh = minH + level * (maxH - minH);
        D2D1_ROUNDED_RECT rr{D2D1::RectF(x, cy - bh * 0.5f, x + kSpectrumBarW, cy + bh * 0.5f),
                             kSpectrumBarW * 0.5f, kSpectrumBarW * 0.5f};
        fillSpectrumBar(rt, rr);
        x += kSpectrumBarW + kSpectrumGap;
    }
}

void SpectrumRenderer::drawBarSpectrum(float x, float h, int visualCount) {
    auto* rt = drawTarget();
    if (!rt || !brushSpectrum_.Get())
        return;
    const int n = std::max(1, visualCount);
    const float baseY = h - kSpectrumBottomPadding;
    const float maxH = h * 0.82f;
    constexpr float minH = 3.0f;
    for (int i = 0; i < n; ++i) {
        const float level = spectrumVisualLevel(i, n);
        const float bh = minH + level * (maxH - minH);
        const D2D1_ROUNDED_RECT bar{D2D1::RectF(x, baseY - bh, x + kSpectrumBarW, baseY),
                                    kSpectrumBarRadius, kSpectrumBarRadius};
        fillSpectrumBar(rt, bar);
        x += kSpectrumBarW + kSpectrumGap;
    }
}

void SpectrumRenderer::drawSegmentedSpectrum(float x, float h, int visualCount) {
    auto* rt = drawTarget();
    if (!rt || !brushSpectrum_.Get())
        return;
    const D2D1_COLOR_F original = brushSpectrum_->GetColor();
    const float baseY = h - kSpectrumBottomPadding;
    constexpr float cellH = 2.0f;
    constexpr float step = 3.5f;
    const int cells = std::max(1, static_cast<int>(h * 0.78f / step));
    for (int i = 0; i < visualCount; ++i) {
        const float level = spectrumVisualLevel(i, visualCount);
        const int lit = static_cast<int>(std::ceil(level * cells));
        const float position = visualCount <= 1 ? 0.0f
                                                : static_cast<float>(i) / (visualCount - 1) *
                                                      (kPresentationSpectrumBands - 1);
        const int band = std::min(kPresentationSpectrumBands - 2, static_cast<int>(position));
        const float peak = spectrumPeaks_[band] +
                           (spectrumPeaks_[band + 1] - spectrumPeaks_[band]) * (position - band);
        for (int cell = 0; cell < cells; ++cell) {
            brushSpectrum_->SetColor(original);
            brushSpectrum_->SetColor(spectrumColorForLevel(static_cast<float>(cell + 1) / cells,
                                                           cell < lit ? 0.88f : 0.09f));
            const float y = baseY - cell * step;
            rt->FillRectangle(D2D1::RectF(x, y - cellH, x + kSpectrumBarW, y),
                              brushSpectrum_.Get());
        }
        if (peak > 0.01f) {
            brushSpectrum_->SetColor(original);
            brushSpectrum_->SetColor(spectrumColorForLevel(peak));
            const int peakCell =
                std::clamp(static_cast<int>(std::ceil(peak * cells)) - 1, 0, cells - 1);
            const float y = baseY - peakCell * step;
            rt->FillRectangle(D2D1::RectF(x, y - 1.0f, x + kSpectrumBarW, y), brushSpectrum_.Get());
        }
        x += kSpectrumBarW + kSpectrumGap;
    }
    brushSpectrum_->SetColor(original);
}

bool SpectrumRenderer::ensureSpectrumEffectResources(float width, float height) {
    auto* rt = drawTarget();
    if (!rt)
        return false;
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    rt->GetDpi(&dpiX, &dpiY);
    const auto size =
        D2D1::SizeU(std::max(1u, static_cast<UINT>(std::ceil(width * dpiX / 96.0f))),
                    std::max(1u, static_cast<UINT>(std::ceil(height * dpiY / 96.0f))));
    if (spectrumFxFrame_.Get()) {
        const auto previous = spectrumFxFrame_->GetPixelSize();
        float previousDpiX = 0.0f;
        float previousDpiY = 0.0f;
        spectrumFxFrame_->GetDpi(&previousDpiX, &previousDpiY);
        if (previous.width == size.width && previous.height == size.height &&
            previousDpiX == dpiX && previousDpiY == dpiY)
            return true;
        releaseSpectrumEffectResources();
    }
    ID2D1Device* device = nullptr;
    rt->GetDevice(&device);
    if (!device)
        return false;
    HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                             spectrumFxContext_.ReleaseAndGetAddressOf());
    device->Release();
    if (FAILED(hr))
        return false;
    spectrumFxContext_->SetDpi(dpiX, dpiY);
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
    hr = spectrumFxContext_->CreateBitmap(size, nullptr, 0, &properties,
                                          spectrumFxFrame_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr) && spectrumEffectEnabled(SpectrumEffect::Trail)) {
        for (auto& bitmap : spectrumFxHistory_) {
            hr = spectrumFxContext_->CreateBitmap(size, nullptr, 0, &properties,
                                                  bitmap.ReleaseAndGetAddressOf());
            if (FAILED(hr))
                break;
        }
    }
    if (SUCCEEDED(hr) && spectrumEffectEnabled(SpectrumEffect::Glow))
        hr = rt->CreateEffect(CLSID_D2D1GaussianBlur, spectrumGlowFx_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr) && spectrumEffectEnabled(SpectrumEffect::Sheen)) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(1, 1, 1, 0)},
                                            {0.5f, D2D1::ColorF(1, 1, 1, 0.5f)},
                                            {1.0f, D2D1::ColorF(1, 1, 1, 0)}};
        ID2D1GradientStopCollection* collection = nullptr;
        hr = rt->CreateGradientStopCollection(stops, 3, &collection);
        if (SUCCEEDED(hr)) {
            hr = rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)),
                collection, spectrumSheenBrush_.ReleaseAndGetAddressOf());
            collection->Release();
        }
    }
    if (FAILED(hr)) {
        runtime_log::writef(L"[spectrum] effect-resource-failed hr=0x%08X", hr);
        releaseSpectrumEffectResources();
        return false;
    }
    return true;
}

float SpectrumRenderer::spectrumSparkOriginY(const SpectrumSpark& spark, float h) const {
    const float level = spark.level;
    switch (context_.style) {
    case SpectrumStyle::Default:
        return h * 0.5f - (4.0f + level * (h * 0.74f - 4.0f)) * 0.5f;
    case SpectrumStyle::Segmented: {
        const int cells = std::max(1, static_cast<int>(h * 0.78f / 3.5f));
        return h - kSpectrumBottomPadding - std::max(0.0f, std::ceil(level * cells) - 1.0f) * 3.5f -
               2.0f;
    }
    case SpectrumStyle::BottomBand:
        return h - 1.1f - level * std::min(5.0f, h * 0.12f);
    case SpectrumStyle::DreamyWave: {
        const float t = static_cast<float>(spark.band) / (kPresentationSpectrumBands - 1);
        const float phase = static_cast<float>(spark.bornMs % 60000ULL) / 1000.0f;
        const float tide =
            std::clamp(0.5f + 0.5f * std::sin(6.2831853f * t * 1.24f + phase * 0.46f - 0.55f) +
                           0.08f * std::sin(6.2831853f * t * 0.60f + phase * 0.24f + 0.7f),
                       0.0f, 1.0f);
        const float profile = std::clamp(0.06f + level * 0.62f + tide * 0.34f, 0.0f, 1.0f);
        return h - kSpectrumBottomPadding - (2.0f + profile * (std::max(3.0f, h * 0.76f) - 2.0f));
    }
    case SpectrumStyle::Bars:
    default:
        return h - kSpectrumBottomPadding - (3.0f + level * (h * 0.82f - 3.0f));
    }
}

void SpectrumRenderer::drawBottomSpectrum(float x, float h, float width) {
    drawSpectrumWithEffects(x, h, width, [&] { drawBottomSpectrumShape(x, h, width); });
}

void SpectrumRenderer::drawBottomSpectrumShape(float x, float h, float width) {
    auto* rt = drawTarget();
    if (!rt || !brushSpectrum_.Get() || width <= 0.0f)
        return;
    const D2D1_COLOR_F original = brushSpectrum_->GetColor();
    const int count = std::max(1, static_cast<int>(width / 4.0f));
    const float pitch = width / count;
    const float maxH = std::min(5.0f, h * 0.12f);
    const float baseY = h - 0.5f;
    for (int i = 0; i < count; ++i) {
        const float level = spectrumVisualLevel(i, count);
        const float t = (i + 0.5f) / count;
        const float edge = std::min(1.0f, std::min(t, 1.0f - t) * 16.0f);
        brushSpectrum_->SetColor(original);
        brushSpectrum_->SetColor(spectrumColorForLevel(level, edge * 0.78f));
        rt->FillRectangle(D2D1::RectF(x + i * pitch, baseY - 0.6f - level * maxH,
                                      x + i * pitch + pitch * 0.6f, baseY),
                          brushSpectrum_.Get());
    }
    brushSpectrum_->SetColor(original);
}

void SpectrumRenderer::drawWaveSpectrum(float x, float h, float width, float opacityScale) {
    auto* rt = drawTarget();
    if (!rt || !brushSpectrum_.Get())
        return;

    constexpr int n = kPresentationSpectrumBands;
    constexpr int samplesPerBand = 6;
    constexpr int sampleCount = (n - 1) * samplesPerBand + 1;
    constexpr float twoPi = 6.28318530718f;
    const float baseY = h - kSpectrumBottomPadding;
    const float minH = 2.0f;
    const float maxH = std::max(minH + 1.0f, h * 0.76f);
    const float phase =
        context_.animations ? static_cast<float>(context_.nowMs % 60000ULL) / 1000.0f : 0.0f;

    std::array<D2D1_POINT_2F, sampleCount> backWave{};
    std::array<D2D1_POINT_2F, sampleCount> middleWave{};
    std::array<D2D1_POINT_2F, sampleCount> frontWave{};
    for (int i = 0; i < sampleCount; ++i) {
        const float normalized = static_cast<float>(i) / (sampleCount - 1);
        const float bandPosition = normalized * static_cast<float>(n - 1);
        const int leftBand = std::min(n - 2, static_cast<int>(bandPosition));
        const float local = bandPosition - static_cast<float>(leftBand);
        const float smoothLocal = local * local * (3.0f - 2.0f * local);
        const float leftLevel = spectrumLevel(leftBand);
        const float rightLevel = spectrumLevel(leftBand + 1);
        const float level = leftLevel + (rightLevel - leftLevel) * smoothLocal;

        // 频段电平控制浪高，低频正弦只负责制造宽阔的海浪峰谷；每层使用
        // 不同波长和相位，避免三层变成同一条线的缩放副本。
        const float audio = std::clamp(level, 0.0f, 1.0f);
        const float backTide =
            std::clamp(0.5f + 0.5f * std::sin(twoPi * normalized * 0.92f + phase * 0.30f + 1.15f) +
                           0.12f * std::sin(twoPi * normalized * 0.44f + phase * 0.16f - 0.4f),
                       0.0f, 1.0f);
        const float middleTide =
            std::clamp(0.5f + 0.5f * std::sin(twoPi * normalized * 1.08f + phase * 0.38f + 0.35f) +
                           0.10f * std::sin(twoPi * normalized * 0.52f + phase * 0.20f + 1.0f),
                       0.0f, 1.0f);
        const float frontTide =
            std::clamp(0.5f + 0.5f * std::sin(twoPi * normalized * 1.24f + phase * 0.46f - 0.55f) +
                           0.08f * std::sin(twoPi * normalized * 0.60f + phase * 0.24f + 0.7f),
                       0.0f, 1.0f);
        const float backProfile = std::clamp(0.16f + audio * 0.34f + backTide * 0.50f, 0.0f, 1.0f);
        const float middleProfile =
            std::clamp(0.10f + audio * 0.46f + middleTide * 0.42f, 0.0f, 1.0f);
        const float frontProfile =
            std::clamp(0.06f + audio * 0.62f + frontTide * 0.34f, 0.0f, 1.0f);
        const float backAmplitude = minH + backProfile * (maxH * 0.88f - minH);
        const float middleAmplitude = minH + middleProfile * (maxH * 0.76f - minH);
        const float frontAmplitude = minH + frontProfile * (maxH - minH);
        const float pointX = x + width * normalized;
        backWave[i] = D2D1::Point2F(pointX, baseY - 1.6f - backAmplitude);
        middleWave[i] = D2D1::Point2F(pointX, baseY - 0.8f - middleAmplitude);
        frontWave[i] = D2D1::Point2F(pointX, baseY - frontAmplitude);
    }

    const float originalOpacity = brushSpectrum_->GetOpacity();
    std::array<D2D1_GRADIENT_STOP, kPresentationSpectrumBands + 6> fadeStops{};
    size_t fadeStopCount = 0;
    auto addFadeStop = [&](float position, float level, float alphaScale) {
        if (fadeStopCount >= fadeStops.size())
            return;
        fadeStops[fadeStopCount++] = {position, spectrumColorForLevel(level, alphaScale)};
    };
    auto levelAt = [&](float normalized) {
        const float bandPosition = std::clamp(normalized, 0.0f, 1.0f) * static_cast<float>(n - 1);
        const int leftBand = std::min(n - 2, static_cast<int>(bandPosition));
        const float local = bandPosition - static_cast<float>(leftBand);
        const float smoothLocal = local * local * (3.0f - 2.0f * local);
        return spectrumLevel(leftBand) +
               (spectrumLevel(leftBand + 1) - spectrumLevel(leftBand)) * smoothLocal;
    };
    if (context_.gradient) {
        addFadeStop(0.00f, levelAt(0.0f), 0.00f);
        addFadeStop(0.017f, levelAt(0.0f), 0.38f);
        addFadeStop(0.033f, levelAt(0.0f), 0.78f);
        for (int i = 0; i < n; ++i) {
            const float position =
                0.05f + 0.90f * static_cast<float>(i) / static_cast<float>(n - 1);
            addFadeStop(position, spectrumLevel(i), 1.0f);
        }
        addFadeStop(0.967f, levelAt(1.0f), 0.78f);
        addFadeStop(0.983f, levelAt(1.0f), 0.38f);
        addFadeStop(1.00f, levelAt(1.0f), 0.00f);
    } else {
        addFadeStop(0.00f, 0.0f, 0.00f);
        addFadeStop(0.017f, 0.0f, 0.38f);
        addFadeStop(0.033f, 0.0f, 0.78f);
        addFadeStop(0.05f, 0.0f, 1.0f);
        addFadeStop(0.95f, 0.0f, 1.0f);
        addFadeStop(0.967f, 0.0f, 0.78f);
        addFadeStop(0.983f, 0.0f, 0.38f);
        addFadeStop(1.00f, 0.0f, 0.00f);
    }
    ID2D1GradientStopCollection* fadeStopCollection = nullptr;
    ID2D1LinearGradientBrush* fadeBrush = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(
            fadeStops.data(), static_cast<UINT32>(fadeStopCount), &fadeStopCollection)) &&
        fadeStopCollection) {
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                          D2D1::Point2F(x, 0.0f), D2D1::Point2F(x + width, 0.0f)),
                                      fadeStopCollection, &fadeBrush);
    }
    if (fadeStopCollection)
        fadeStopCollection->Release();

    ID2D1Brush* waveBrush = fadeBrush ? static_cast<ID2D1Brush*>(fadeBrush)
                                      : static_cast<ID2D1Brush*>(brushSpectrum_.Get());
    auto setWaveOpacity = [&](float opacity) {
        waveBrush->SetOpacity(originalOpacity * opacityScale * opacity);
    };
    auto drawFilledWave = [&](const auto& points, float opacity) {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        rt->GetFactory(factory.GetAddressOf());
        auto* d2d = factory.Get();
        if (!d2d)
            return;

        ID2D1PathGeometry* geometry = nullptr;
        if (FAILED(d2d->CreatePathGeometry(&geometry)) || !geometry)
            return;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geometry->Open(&sink)) || !sink) {
            geometry->Release();
            return;
        }

        sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < sampleCount; ++i)
            sink->AddLine(points[i]);
        sink->AddLine(D2D1::Point2F(points[sampleCount - 1].x, baseY));
        sink->AddLine(D2D1::Point2F(points[0].x, baseY));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        const HRESULT closeHr = sink->Close();
        sink->Release();
        if (SUCCEEDED(closeHr)) {
            setWaveOpacity(opacity);
            rt->FillGeometry(geometry, waveBrush);
        }
        geometry->Release();
    };
    auto drawCurve = [&](const auto& points, float opacity, float strokeWidth) {
        setWaveOpacity(opacity);
        for (int i = 1; i < sampleCount; ++i)
            rt->DrawLine(points[i - 1], points[i], waveBrush, strokeWidth);
    };

    // 先铺后景到前景的连续波面，避免波峰下方出现大片空白。
    drawFilledWave(backWave, 0.32f);
    drawFilledWave(middleWave, 0.46f);
    drawFilledWave(frontWave, 0.76f);

    // 这条线与柱状图的柱底严格共用 baseY，切换样式时视觉基准不跳动。
    setWaveOpacity(0.16f);
    rt->DrawLine(D2D1::Point2F(x, baseY), D2D1::Point2F(x + width, baseY), waveBrush, 0.8f);
    drawCurve(frontWave, 0.07f, 3.8f);
    drawCurve(frontWave, 0.36f, 1.0f);
    brushSpectrum_->SetOpacity(originalOpacity);
    if (fadeBrush)
        fadeBrush->Release();
}

void SpectrumRenderer::drawDreamyWaveSpectrum(float x, float h, float width) {
    drawWaveSpectrum(x, h, width, 1.0f);
}

void SpectrumRenderer::drawBackgroundWaveSpectrum(float x, float h, float width) {
    drawSpectrumWithEffects(x, h, width, [&] {
        drawWaveSpectrum(x, h, width, static_cast<float>(context_.opacity) / 100.0f);
    });
}

void SpectrumRenderer::drawSpectrum(float x, float h, float width) {
    auto* rt = drawTarget();
    if (!rt || width <= 0.0f || h <= 0.0f)
        return;
    D2D1_MATRIX_3X2_F previous{};
    const bool compact = context_.compactHeight > 0.0f && h > 6.0f;
    if (compact) {
        rt->GetTransform(&previous);
        rt->PushAxisAlignedClip(D2D1::RectF(x, 2.0f, x + width, h - 2.0f),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, (h - 6.0f) / h) *
                         D2D1::Matrix3x2F::Translation(0.0f, 3.0f) * previous);
    }
    drawSpectrumWithEffects(x, h, width, [&] { drawSpectrumShape(x, h, width); });
    if (compact) {
        rt->SetTransform(previous);
        rt->PopAxisAlignedClip();
    }
}

void SpectrumRenderer::drawSpectrumShape(float x, float h, float width) {
    if (width <= 0.0f)
        return;
    switch (context_.style) {
    case SpectrumStyle::BottomBand:
        break; // 与歌曲内容整合的样式不在右侧重复绘制。
    case SpectrumStyle::Segmented:
    case SpectrumStyle::Bars: {
        const int visualCount = context_.horizontalImmersive ? immersiveSpectrumBarCount(width)
                                                             : kPresentationSpectrumBands;
        const float clusterW = spectrumVisualClusterW(visualCount);
        const float clusterX = x + std::max(0.0f, (width - clusterW) * 0.5f);
        if (context_.style == SpectrumStyle::Segmented)
            drawSegmentedSpectrum(clusterX, h, visualCount);
        else
            drawBarSpectrum(clusterX, h, visualCount);
        break;
    }
    case SpectrumStyle::DreamyWave:
        drawDreamyWaveSpectrum(x, h, width);
        break;
    case SpectrumStyle::Default:
    default: {
        const int visualCount = context_.horizontalImmersive ? immersiveSpectrumBarCount(width)
                                                             : kPresentationSpectrumBands;
        const float clusterW = spectrumVisualClusterW(visualCount);
        const float clusterX = x + std::max(0.0f, (width - clusterW) * 0.5f);
        drawDefaultSpectrum(clusterX, h, visualCount);
        break;
    }
    }
}
