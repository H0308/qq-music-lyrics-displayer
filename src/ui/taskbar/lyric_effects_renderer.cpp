#include "ui/taskbar/lyric_effects_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
namespace {}
float LyricEffectsRenderer::starRadius(float fontSize) {
    return std::clamp(fontSize * 0.40f, 5.5f, 8.5f);
}
void LyricEffectsRenderer::createBrushes() {
    auto* rt = drawTarget();
    if (!rt)
        return;
    brushLyricFx_.Reset();
    brushLyricFollow_.Reset();
    const auto rgb = [](COLORREF c, float alpha) {
        return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f,
                            alpha);
    };
    rt->CreateSolidColorBrush(rgb(context_.color, 1.00f), brushLyricFx_.ReleaseAndGetAddressOf());
    const auto base = rgb(context_.color, 1.0f);
    const auto dark = D2D1::ColorF(base.r * 0.30f, base.g * 0.30f, base.b * 0.30f, 0.85f);
    const auto bright = D2D1::ColorF(base.r + (1 - base.r) * 0.85f, base.g + (1 - base.g) * 0.85f,
                                     base.b + (1 - base.b) * 0.85f, 1.0f);
    const D2D1_GRADIENT_STOP followStops[] = {{0.0f, D2D1::ColorF(base.r, base.g, base.b, 0)},
                                              {0.25f, dark},
                                              {0.5f, bright},
                                              {0.75f, dark},
                                              {1.0f, D2D1::ColorF(base.r, base.g, base.b, 0)}};
    ID2D1GradientStopCollection* followCollection = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(followStops, _countof(followStops),
                                                   &followCollection))) {
        rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)),
            followCollection, brushLyricFollow_.ReleaseAndGetAddressOf());
        followCollection->Release();
    }
}
void LyricEffectsRenderer::discardDeviceResources() {
    clearLyricEffectGeometry();
    brushLyricFx_.Reset();
    brushLyricFollow_.Reset();
    lyricStar_.reset();
    context_.target = nullptr;
}
bool LyricEffectsRenderer::lyricEffectEnabled(LyricEffect effect) const {
    return (context_.effects & lyricEffectBit(effect)) != 0;
}

void LyricEffectsRenderer::syncLyricEffectClock(int64_t position, bool playing, bool reset) {
    const ULONGLONG now = monotonicNowMs();
    const int64_t elapsed =
        lyricFxLastClockMs_ == 0 ? 0 : static_cast<int64_t>(now - lyricFxLastClockMs_);
    const int64_t expected = lyricFxPlaying_ ? elapsed : 0;
    if (reset || lyricFxLastPositionMs_ < 0 ||
        std::llabs(position - lyricFxLastPositionMs_ - expected) > 350 ||
        (!playing && !lyricFxPlaying_ && position != lyricFxLastPositionMs_) ||
        (playing && !lyricFxPlaying_)) {
        lyricFxEmitFromMs_ = position;
        if (context_.effects != 0)
            requestFrame();
    }
    if (!playing && (lyricFxPlaying_ || lyricFxPauseMs_ == 0))
        lyricFxPauseMs_ = now;
    if (playing)
        lyricFxPauseMs_ = 0;
    lyricFxPlaying_ = playing;
    lyricFxLastPositionMs_ = position;
    lyricFxLastClockMs_ = now;
}

bool LyricEffectsRenderer::lyricEffectsAnimating() const {
    if (context_.effects == 0 || !context_.animations || context_.minimal || context_.stopped ||
        context_.scene != DisplayScene::Lyrics || context_.currentLine < 0)
        return false;
    // 暂停后继续画到所有短尾迹消散，末帧由 tick 主动失效刷新。
    if (!context_.playing)
        return lyricFxPauseMs_ != 0 && monotonicNowMs() - lyricFxPauseMs_ <= 700;
    if (static_cast<size_t>(context_.currentLine) >= (*context_.lines).size())
        return false;
    const auto& chars = (*context_.lines)[context_.currentLine].chars;
    if (lyricStarEnabled(context_.currentLine) && !chars.empty() &&
        context_.positionMs >= chars.front().startMs &&
        context_.positionMs < chars.back().endMs + 300)
        return true;
    for (const auto& token : (*context_.lines)[context_.currentLine].chars)
        if (context_.positionMs >= token.startMs && context_.positionMs - token.endMs < 700)
            return true;
    return false;
}

void LyricEffectsRenderer::clearLyricEffectGeometry() {
    lyricFxGeometry_.clear();
    for (auto& source : lyricFxSources_)
        source = {};
}

bool LyricEffectsRenderer::lyricStarEnabled(int lineIndex) const {
    const bool singleLine = lineIndex == context_.outgoingLine ? !context_.outgoingDoubleLine
                                                               : !context_.currentDoubleLine;
    return lineIndex >= 0 && static_cast<size_t>(lineIndex) < (*context_.lines).size() &&
           !(*context_.lines)[lineIndex].chars.empty() && singleLine &&
           context_.compactHeight <= 0.0f && (!context_.doubleLineEnabled || context_.vertical) &&
           context_.animations && !context_.minimal && !context_.stopped &&
           context_.scene == DisplayScene::Lyrics && lyricEffectEnabled(LyricEffect::Stardust);
}

const std::vector<LyricEffectsRenderer::LyricFxToken>&
LyricEffectsRenderer::lyricEffectTokens(int lineIndex) {
    const bool current = lineIndex == context_.currentLine;
    auto& cache = lyricFxSources_[current ? 0 : 1];
    IDWriteTextLayout* layout = current                              ? context_.currentLayout
                                : lineIndex == context_.outgoingLine ? context_.outgoingLayout
                                                                     : nullptr;
    const float width = current ? context_.width : context_.outgoingWidth;
    if (cache.layout == layout && cache.line == lineIndex)
        return cache.tokens;
    cache = {};
    cache.layout = layout;
    cache.line = lineIndex;
    if (!layout || width <= 0 || lineIndex < 0 ||
        static_cast<size_t>(lineIndex) >= (*context_.lines).size())
        return cache.tokens;
    UINT32 offset = 0;
    cache.tokens.reserve((*context_.lines)[lineIndex].chars.size());
    for (const auto& token : (*context_.lines)[lineIndex].chars) {
        const UINT32 length = static_cast<UINT32>(token.text.size());
        if (length == 0)
            continue;
        DWRITE_HIT_TEST_METRICS hit{};
        float first = 0, last = 0, y = 0;
        const bool valid =
            SUCCEEDED(layout->HitTestTextPosition(offset, FALSE, &first, &y, &hit)) &&
            SUCCEEDED(layout->HitTestTextPosition(offset + length - 1, TRUE, &last, &y, &hit));
        const bool visible =
            valid && token.text.find_first_not_of(L" \t\r\n") != std::wstring::npos;
        cache.tokens.push_back({token.startMs, token.endMs, std::clamp(first / width, 0.0f, 1.0f),
                                std::clamp(last / width, 0.0f, 1.0f), visible});
        offset += length;
    }
    return cache.tokens;
}

float LyricEffectsRenderer::lyricEffectProgress(int lineIndex) {
    // 预览、翻译和歌曲信息没有演唱行，不得驱逐旧行的时间轴缓存。
    if (lineIndex < 0 || static_cast<size_t>(lineIndex) >= (*context_.lines).size())
        return 0.0f;
    const auto& tokens = lyricEffectTokens(lineIndex);
    auto it = std::upper_bound(
        tokens.begin(), tokens.end(), context_.positionMs,
        [](int64_t time, const LyricFxToken& token) { return time < token.startMs; });
    if (it == tokens.begin())
        return 0;
    --it;
    const float t =
        static_cast<float>(std::clamp(static_cast<double>(context_.positionMs - it->startMs) /
                                          std::max<int64_t>(1, it->endMs - it->startMs),
                                      0.0, 1.0));
    return it->first + (it->last - it->first) * t;
}

LyricEffectsRenderer::LyricFxGeometry&
LyricEffectsRenderer::lyricEffectGeometry(IDWriteTextLayout* layout, int lineIndex, float width,
                                          float height, bool vertical, float rangeStart,
                                          float rangeSize) {
    auto found = lyricFxGeometry_.find(layout);
    if (found != lyricFxGeometry_.end()) {
        const auto& value = *found->second;
        if (value.line == lineIndex && value.width == width && value.height == height &&
            value.vertical == vertical && value.rangeStart == rangeStart &&
            value.rangeSize == rangeSize)
            return *found->second;
        lyricFxGeometry_.erase(found);
    }
    auto geometry = std::make_unique<LyricFxGeometry>();
    geometry->line = lineIndex;
    geometry->width = width;
    geometry->height = height;
    geometry->vertical = vertical;
    geometry->rangeStart = rangeStart;
    geometry->rangeSize = rangeSize;
    auto locate = [&](float u) {
        LyricFxPoint point{};
        const float local = (u - rangeStart) / rangeSize;
        if (local < 0 || local > 1)
            return point;
        const float px = vertical ? width * 0.5f : local * width;
        const float py = vertical ? local * height : height * 0.5f;
        DWRITE_HIT_TEST_METRICS hit{};
        BOOL trailing = FALSE, inside = FALSE;
        if (FAILED(layout->HitTestPoint(px, py, &trailing, &inside, &hit)) || !hit.isText ||
            hit.isTrimmed || hit.width <= 0 || hit.height <= 0)
            return point;
        // 采用最近字框，不用整行高度猜测字心，也不因字体行间留白丢弃定位。
        point.center = D2D1::Point2F(hit.left + hit.width * 0.5f, hit.top + hit.height * 0.5f);
        point.edge =
            vertical ? D2D1::Point2F(hit.left + hit.width, py) : D2D1::Point2F(px, hit.top);
        point.valid = true;
        return point;
    };
    const auto& tokens = lyricEffectTokens(lineIndex);
    geometry->tokens.reserve(tokens.size());
    for (const auto& token : tokens) {
        const float first = std::max(token.first, rangeStart);
        const float last = std::min(token.last, rangeStart + rangeSize);
        const auto point =
            token.visible && last > first ? locate((first + last) * 0.5f) : LyricFxPoint{};
        geometry->tokens.push_back(point);
        // 星星沿整行字形的外缘运动；空格有进度，但不应决定星星是否可见。
        if (point.valid && (!geometry->starAnchor.valid ||
                            (vertical ? point.edge.x > geometry->starAnchor.edge.x
                                      : point.edge.y < geometry->starAnchor.edge.y)))
            geometry->starAnchor = point;
    }
    auto* result = geometry.get();
    lyricFxGeometry_.emplace(layout, std::move(geometry));
    return *result;
}

void LyricEffectsRenderer::ensureLyricGlyphMask(IDWriteTextLayout* layout,
                                                LyricFxGeometry& geometry) {
    if (geometry.glyphMask.Get() || !brushLyricFx_.Get())
        return;
    ID2D1BitmapRenderTarget* target = nullptr;
    auto* rt = drawTarget();
    if (!rt || FAILED(rt->CreateCompatibleRenderTarget(D2D1::SizeF(geometry.width, geometry.height),
                                                       &target)))
        return;
    const auto oldColor = brushLyricFx_->GetColor();
    brushLyricFx_->SetColor(D2D1::ColorF(1, 1, 1, 1));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    target->DrawTextLayout(D2D1::Point2F(0, 0), layout, brushLyricFx_.Get());
    if (SUCCEEDED(target->EndDraw()))
        target->GetBitmap(geometry.glyphMask.ReleaseAndGetAddressOf());
    brushLyricFx_->SetColor(oldColor);
    if (geometry.glyphMask.Get())
        geometry.glyphTarget.Attach(target);
    else
        target->Release();
}

void LyricEffectsRenderer::drawLyricOverlay(IDWriteTextLayout* layout, int lineIndex, float x,
                                            float y, float width, float height, bool vertical,
                                            float opacity, float progress, bool foreground,
                                            float rangeStart, float rangeSize) {
    auto* rt = drawTarget();
    if (!foreground || !rt || !layout || !brushLyricFx_.Get() || context_.effects == 0 ||
        !context_.animations || context_.minimal || context_.stopped ||
        context_.scene != DisplayScene::Lyrics || lineIndex < 0 ||
        static_cast<size_t>(lineIndex) >= (*context_.lines).size() || width <= 0 || height <= 0 ||
        rangeSize <= 0 || opacity <= 0.0f)
        return;
    const auto& line = (*context_.lines)[lineIndex];
    auto& geometry =
        lyricEffectGeometry(layout, lineIndex, width, height, vertical, rangeStart, rangeSize);
    const auto& tokens = lyricEffectTokens(lineIndex);
    const bool stardust = lyricStarEnabled(lineIndex);
    const ULONGLONG now = monotonicNowMs();
    const float pauseAge = lyricFxPauseMs_ == 0 ? 0.0f : static_cast<float>(now - lyricFxPauseMs_);
    const float liveAlpha = context_.playing ? 1.0f : std::max(0.0f, 1.0f - pauseAge / 300.0f);
    const auto baseColor =
        D2D1::ColorF(GetRValue(context_.color) / 255.0f, GetGValue(context_.color) / 255.0f,
                     GetBValue(context_.color) / 255.0f, 1.0f);
    auto color = [&](float alpha, bool light = false) {
        auto value = baseColor;
        if (light) {
            value.r += (1.0f - value.r) * 0.60f;
            value.g += (1.0f - value.g) * 0.60f;
            value.b += (1.0f - value.b) * 0.60f;
        }
        value.a *= std::clamp(alpha * opacity, 0.0f, 1.0f);
        brushLyricFx_->SetColor(value);
    };
    auto upper = std::upper_bound(
        tokens.begin(), tokens.end(), context_.positionMs,
        [](int64_t time, const LyricFxToken& token) { return time < token.startMs; });
    const float localProgress = std::clamp((progress - rangeStart) / rangeSize, 0.0f, 1.0f);
    if (stardust && upper != tokens.begin() && liveAlpha > 0.0f && progress >= rangeStart &&
        progress <= rangeStart + rangeSize) {
        const auto active = std::prev(upper);
        const size_t index = static_cast<size_t>(active - tokens.begin());
        const auto& origin = geometry.starAnchor;
        if (origin.valid) {
            const float t = static_cast<float>(std::clamp(
                static_cast<double>(context_.positionMs - active->startMs) /
                    static_cast<double>(std::max<int64_t>(1, active->endMs - active->startMs)),
                0.0, 1.0));
            constexpr float pi = 3.14159265358979323846f;
            const float hop = std::sin(pi * t) * 3.0f;
            const float radius = lyricStarRadius();
            const float gap = 1.25f + (context_.glow ? 1.0f : context_.outline ? 0.75f : 0.0f);
            const float extent = vertical ? height : width;
            const float inset = std::min(radius, extent * 0.5f);
            const float along = std::clamp(localProgress * extent, inset, extent - inset);
            const auto center =
                vertical ? D2D1::Point2F(x + origin.edge.x + gap + radius + hop, y + along)
                         : D2D1::Point2F(x + along, y + origin.edge.y - gap - radius - hop);
            // 角度只依赖播放时间：暂停冻结，seek 直接对齐，不积累动画状态。
            const float angle =
                static_cast<float>(context_.positionMs % 3000) / 3000.0f * 2.0f * pi;
            const float endAlpha =
                index + 1 == tokens.size()
                    ? std::clamp(1.0f - static_cast<float>(context_.positionMs - active->endMs) /
                                            300.0f,
                                 0.0f, 1.0f)
                    : 1.0f;
            // 每 24ms 沿缓存轨迹发射一粒，最多 18 粒；固定出生时间保证粒子连续漂移。
            const float rawProgress = active->first + (active->last - active->first) * t;
            const float rawAlong =
                std::clamp((rawProgress - rangeStart) / rangeSize * extent, inset, extent - inset);
            const float smoothOffset = along - rawAlong;
            for (int i = 17; i >= 0; --i) {
                const int64_t sampleMs = (context_.positionMs / 24 - i) * 24;
                if (sampleMs < lyricFxEmitFromMs_)
                    continue;
                auto sample = std::upper_bound(
                    tokens.begin(), tokens.end(), sampleMs,
                    [](int64_t time, const LyricFxToken& token) { return time < token.startMs; });
                if (sample == tokens.begin())
                    continue;
                --sample;
                const float sampleT = static_cast<float>(std::clamp(
                    static_cast<double>(sampleMs - sample->startMs) /
                        static_cast<double>(std::max<int64_t>(1, sample->endMs - sample->startMs)),
                    0.0, 1.0));
                const float sampleProgress =
                    sample->first + (sample->last - sample->first) * sampleT;
                if (sampleProgress < rangeStart || sampleProgress > rangeStart + rangeSize)
                    continue;
                const float sampleAlong =
                    std::clamp((sampleProgress - rangeStart) / rangeSize * extent, inset,
                               extent - inset) +
                    smoothOffset;
                const float sampleHop = std::sin(pi * sampleT) * 3.0f;
                uint32_t seed = static_cast<uint32_t>(sampleMs / 24) ^
                                (static_cast<uint32_t>(lineIndex) + 1u) * 0x9e3779b9u;
                seed ^= seed >> 16;
                seed *= 0x7feb352du;
                seed ^= seed >> 15;
                const float side = static_cast<float>(seed & 1023u) / 1023.0f;
                const float speed = static_cast<float>((seed >> 10) & 1023u) / 1023.0f;
                const float age = static_cast<float>(context_.positionMs - sampleMs) / 432.0f;
                // 越老越向后、向外散开；朝文字的一侧少扩散，保持字形周围留白。
                const float lag = age * (radius + 9.0f * speed);
                const float scatter = (side * 12.0f - 7.5f) * age;
                const auto dot =
                    vertical
                        ? D2D1::Point2F(x + origin.edge.x + gap + radius + sampleHop - scatter,
                                        y + sampleAlong - lag)
                        : D2D1::Point2F(x + sampleAlong - lag,
                                        y + origin.edge.y - gap - radius - sampleHop + scatter);
                const float dx = dot.x - center.x;
                const float dy = dot.y - center.y;
                if (dx * dx + dy * dy < radius * radius * 0.49f)
                    continue;
                const float fade = 1.0f - age;
                const float dotRadius = 0.55f + fade * (0.8f + speed * 0.6f);
                color(liveAlpha * endAlpha * fade * fade * 0.85f);
                rt->FillEllipse(D2D1::Ellipse(dot, dotRadius, dotRadius), brushLyricFx_.Get());
            }
            color(liveAlpha * endAlpha);
            const float previousOutline =
                context_.outlineBrush ? context_.outlineBrush->GetOpacity() : 1.0f;
            if (context_.outline && context_.outlineBrush)
                context_.outlineBrush->SetOpacity(opacity * liveAlpha * endAlpha);
            lyricStar_.draw(rt, center, radius, angle, brushLyricFx_.Get(),
                            context_.outline ? context_.outlineBrush : nullptr);
            if (context_.outlineBrush)
                context_.outlineBrush->SetOpacity(previousOutline);
        }
    }
    int checked = 0;
    for (auto it = upper;
         lyricEffectEnabled(LyricEffect::Ripple) && it != tokens.begin() && checked++ < 24;) {
        --it;
        const auto& token = *it;
        if (context_.positionMs - token.endMs > 700)
            break;
        const size_t tokenIndex = static_cast<size_t>(it - tokens.begin());
        const auto& origin = geometry.tokens[tokenIndex];
        if (!origin.valid || token.endMs <= token.startMs)
            continue;
        const float elapsed = static_cast<float>(context_.positionMs - token.startMs);
        if (lyricEffectEnabled(LyricEffect::Ripple) && token.startMs >= lyricFxEmitFromMs_ &&
            elapsed + pauseAge < 420) {
            const auto center = D2D1::Point2F(x + origin.center.x, y + origin.center.y);
            const float t = (elapsed + pauseAge) / 420.0f;
            color((1.0f - t) * 0.78f);
            const float radius = 2.0f + t * std::min(8.0f, context_.fontSize * 0.36f);
            rt->DrawEllipse(D2D1::Ellipse(center, radius, radius), brushLyricFx_.Get(), 1.35f);
            color((1.0f - t) * 0.32f);
            rt->DrawEllipse(D2D1::Ellipse(center, radius * 0.64f, radius * 0.64f),
                            brushLyricFx_.Get(), 0.85f);
        }
    }
    const bool hasTiming = !line.chars.empty() && context_.positionMs >= line.ms;
    if (hasTiming && lyricEffectEnabled(LyricEffect::Underline) && liveAlpha > 0 &&
        localProgress > 0) {
        for (int i = 0; i < 12; ++i) {
            const float a = localProgress * i / 12.0f;
            const float b = localProgress * (i + 1) / 12.0f;
            color((0.15f + i * 0.055f) * liveAlpha);
            rt->DrawLine(vertical ? D2D1::Point2F(x + width - 1.2f, y + a * height)
                                  : D2D1::Point2F(x + a * width, y + height - 1.2f),
                         vertical ? D2D1::Point2F(x + width - 1.2f, y + b * height)
                                  : D2D1::Point2F(x + b * width, y + height - 1.2f),
                         brushLyricFx_.Get(), 0.9f);
        }
    }
    if (hasTiming && lyricEffectEnabled(LyricEffect::FollowLight) &&
        context_.positionMs < line.chars.back().endMs && progress > rangeStart &&
        progress <= rangeStart + rangeSize && liveAlpha > 0 && brushLyricFollow_.Get()) {
        ensureLyricGlyphMask(layout, geometry);
        const float axis = vertical ? y + localProgress * height : x + localProgress * width;
        const float halfWidth = std::clamp(context_.fontSize * 0.65f, 7.0f, 18.0f);
        brushLyricFollow_->SetStartPoint(vertical ? D2D1::Point2F(x, axis - halfWidth)
                                                  : D2D1::Point2F(axis - halfWidth, y));
        brushLyricFollow_->SetEndPoint(vertical ? D2D1::Point2F(x, axis + halfWidth)
                                                : D2D1::Point2F(axis + halfWidth, y));
        const float previous = brushLyricFollow_->GetOpacity();
        brushLyricFollow_->SetOpacity(liveAlpha * opacity);
        if (geometry.glyphMask.Get()) {
            const auto destination = D2D1::RectF(x, y, x + width, y + height);
            const auto source = D2D1::RectF(0, 0, width, height);
            const auto antialias = rt->GetAntialiasMode();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            rt->FillOpacityMask(geometry.glyphMask.Get(), brushLyricFollow_.Get(), &destination,
                                &source);
            rt->SetAntialiasMode(antialias);
        }
        brushLyricFollow_->SetOpacity(previous);
    }
}
