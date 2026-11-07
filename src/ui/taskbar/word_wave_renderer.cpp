#include "ui/taskbar/word_wave_renderer.h"
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    // 位移、速度与加速度在分段端点连续，减少沉浮衔接处的停顿感。
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
float leadMs(const LyricChar& token) {
    return std::clamp(static_cast<float>(token.endMs - token.startMs) * 0.35f, 160.0f, 240.0f);
}
float floatMs(const LyricChar& token) {
    return std::clamp(static_cast<float>(token.endMs - token.startMs) * 1.2f, 520.0f, 780.0f);
}
constexpr float kTroughAfterStartMs = 70.0f;
constexpr int64_t kLineTailMs = 600;
}

class WordWaveRenderer::Collector final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                         IDWriteTextRenderer> {
public:
    Collector(std::vector<Run>& runs, const std::vector<UINT32>& ends,
              IDWriteTextLayout* layout, float width)
        : runs_(runs), ends_(ends), layout_(layout), width_(width) {}
    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        *transform = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* pixels) override {
        *pixels = 1.0f;
        return S_OK;
    }
    IFACEMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE measuring,
        const DWRITE_GLYPH_RUN* glyphs, const DWRITE_GLYPH_RUN_DESCRIPTION* description,
        IUnknown*) override {
        if (!glyphs || !description || !description->clusterMap)
            return E_INVALIDARG;
        if (glyphs->glyphCount == 0)
            return S_OK;
        if (!glyphs->fontFace || !glyphs->glyphIndices || !glyphs->glyphAdvances)
            return E_INVALIDARG;
        Run run;
        run.face = glyphs->fontFace;
        run.size = glyphs->fontEmSize;
        run.x = x;
        run.y = y;
        run.sideways = glyphs->isSideways;
        run.bidi = glyphs->bidiLevel;
        run.measuring = measuring;
        run.indices.assign(glyphs->glyphIndices, glyphs->glyphIndices + glyphs->glyphCount);
        run.advances.assign(glyphs->glyphAdvances, glyphs->glyphAdvances + glyphs->glyphCount);
        run.originalOffsets.resize(glyphs->glyphCount);
        if (glyphs->glyphOffsets)
            std::copy_n(glyphs->glyphOffsets, glyphs->glyphCount, run.originalOffsets.begin());
        run.offsets = run.originalOffsets;
        constexpr UINT32 missing = std::numeric_limits<UINT32>::max();
        std::vector<UINT32> clusters(glyphs->glyphCount, missing);
        for (UINT32 i = 0; i < description->stringLength; ++i) {
            const UINT16 glyph = description->clusterMap[i];
            if (glyph < glyphs->glyphCount)
                clusters[glyph] = std::min(clusters[glyph], description->textPosition + i);
        }
        // 同一字形簇只采用一个时间单元，连字、组合字符不会被分开拉动。
        UINT32 text = missing;
        float position = 0.0f;
        for (UINT32 glyph = 0; glyph < glyphs->glyphCount; ++glyph) {
            if (clusters[glyph] != missing) {
                text = clusters[glyph];
                DWRITE_HIT_TEST_METRICS hit{};
                float hitX = 0.0f, hitY = 0.0f;
                const HRESULT hr = layout_->HitTestTextPosition(text, FALSE, &hitX, &hitY, &hit);
                if (FAILED(hr))
                    return hr;
                position = std::clamp((hit.left + hit.width * 0.5f) / width_, 0.0f, 1.0f);
            }
            const auto token = std::upper_bound(ends_.begin(), ends_.end(), text);
            run.tokens.push_back(static_cast<size_t>(token - ends_.begin()));
            run.positions.push_back(position);
        }
        runs_.push_back(std::move(run));
        return S_OK;
    }
    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
        return E_NOTIMPL;
    }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return E_NOTIMPL;
    }
    IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                  IUnknown*) override { return E_NOTIMPL; }
private:
    std::vector<Run>& runs_;
    const std::vector<UINT32>& ends_;
    IDWriteTextLayout* layout_;
    float width_;
};

float WordWaveRenderer::offset(const LyricChar& token, int64_t lineMs, int64_t positionMs,
                              float fontSize) {
    if (token.endMs <= token.startMs || positionMs < lineMs)
        return 0.0f;
    const float depth = std::clamp(fontSize * 0.06f, 0.6f, 1.2f);
    const float elapsed = static_cast<float>(positionMs - token.startMs);
    const float lead = leadMs(token);
    // 下沉略先于唱词开始，之后用更长时间浮回基线；相邻时间单元的波形自然重叠。
    const float phase = elapsed < kTroughAfterStartMs
                            ? smooth((elapsed + lead) / (lead + kTroughAfterStartMs))
                            : 1.0f - smooth((elapsed - kTroughAfterStartMs) / floatMs(token));
    // 当前行刚接入时从原位连续进入波形，首字同样经历下沉与浮起。
    const float entrance = smooth(static_cast<float>(positionMs - lineMs) / 320.0f);
    return depth * phase * entrance;
}

int64_t WordWaveRenderer::endTime(const LyricLine& line) {
    int64_t end = line.ms;
    for (const auto& token : line.chars)
        if (token.endMs > token.startMs)
            end = std::max(end, token.endMs);
    return end;
}

float WordWaveRenderer::lineOffset(int64_t lineMs, int64_t endMs, int64_t positionMs,
                                  float fontSize, float along) {
    if (endMs <= lineMs || positionMs < lineMs)
        return 0.0f;
    const float age = static_cast<float>(positionMs - lineMs);
    const float entrance = smooth(age / 420.0f);
    const float exit = 1.0f - smooth(static_cast<float>(positionMs - endMs) / kLineTailMs);
    constexpr float tau = 6.28318530718f;
    // 两列缓慢前进的长波叠加，波峰沿文字实际位置传播，不依赖逐字分段宽度。
    const float swell = std::sin(tau * (along * 1.15f - age / 2800.0f));
    const float wake = std::sin(tau * (along * 2.3f - age / 2300.0f) + 0.8f);
    const float amplitude = std::clamp(fontSize * 0.04f, 0.35f, 0.65f);
    return amplitude * (0.78f * swell + 0.22f * wake) * entrance * exit;
}

bool WordWaveRenderer::animating(const LyricLine& line, int64_t positionMs) {
    if (positionMs < line.ms)
        return false;
    const int64_t end = endTime(line);
    if (end > line.ms && positionMs < end + kLineTailMs)
        return true;
    // 极短句最后一个字的浮起可能晚于整行长波的渐隐，继续画完它的尾段。
    for (const auto& token : line.chars)
        if (token.endMs > token.startMs &&
            positionMs - token.startMs >= -leadMs(token) &&
            positionMs - token.startMs < kTroughAfterStartMs + floatMs(token))
            return true;
    return false;
}

bool WordWaveRenderer::prepare(IDWriteTextLayout* layout, int lineIndex, const LyricLine& line,
                               int64_t positionMs, float fontSize) {
    if (!layout || line.chars.empty())
        return false;
    if (layout_.Get() != layout || lineIndex_ != lineIndex || offsets_.size() != line.chars.size()) {
        clear();
        std::vector<UINT32> ends;
        UINT32 end = 0;
        for (const auto& token : line.chars) {
            end += static_cast<UINT32>(token.text.size());
            ends.push_back(end);
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)))
            return false;
        auto collector = Microsoft::WRL::Make<Collector>(runs_, ends, layout,
            std::max(1.0f, metrics.widthIncludingTrailingWhitespace));
        if (!collector || FAILED(layout->Draw(nullptr, collector.Get(), 0, 0))) {
            clear();
            return false;
        }
        layout_ = layout;
        lineIndex_ = lineIndex;
        offsets_.resize(line.chars.size());
    }
    for (size_t i = 0; i < offsets_.size(); ++i)
        offsets_[i] = offset(line.chars[i], line.ms, positionMs, fontSize);
    const int64_t end = endTime(line);
    for (auto& run : runs_)
        for (size_t i = 0; i < run.tokens.size(); ++i) {
            run.offsets[i] = run.originalOffsets[i];
            if (run.tokens[i] < offsets_.size())
                run.offsets[i].ascenderOffset -= offsets_[run.tokens[i]];
            run.offsets[i].ascenderOffset -=
                lineOffset(line.ms, end, positionMs, fontSize, run.positions[i]);
        }
    return true;
}

void WordWaveRenderer::draw(ID2D1RenderTarget* target, D2D1_POINT_2F origin, ID2D1Brush* brush) const {
    if (!target || !brush)
        return;
    for (const auto& run : runs_) {
        const DWRITE_GLYPH_RUN glyphs{run.face.Get(), run.size,
            static_cast<UINT32>(run.indices.size()), run.indices.data(), run.advances.data(),
            run.offsets.data(), run.sideways, run.bidi};
        target->DrawGlyphRun(D2D1::Point2F(origin.x + run.x, origin.y + run.y), &glyphs,
                             brush, run.measuring);
    }
}

void WordWaveRenderer::clear() {
    layout_.Reset();
    lineIndex_ = -1;
    runs_.clear();
    offsets_.clear();
}
