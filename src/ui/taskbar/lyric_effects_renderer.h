#pragma once
#include "model/display_types.h"
#include "lyric/lyric_types.h"
#include "ui/lyric_effects.h"
#include "ui/lyric_star.h"
#include "ui/taskbar/animation_clock.h"
#include "ui/taskbar/word_wave_renderer.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include <utility>

struct LyricEffectsContext {
    ID2D1DeviceContext* target = nullptr;
    const std::vector<LyricLine>* lines = nullptr;
    uint32_t effects{};
    bool animations{};
    DisplayScene scene{};
    int currentLine{};
    int outgoingLine{};
    IDWriteTextLayout* currentLayout{};
    IDWriteTextLayout* outgoingLayout{};
    float width{};
    float outgoingWidth{};
    int64_t positionMs{};
    bool outgoingDoubleLine{};
    float compactHeight{};
    bool doubleLineEnabled{};
    COLORREF color{};
    bool glow{};
    bool outline{};
    float fontSize{};
    ID2D1Brush* outlineBrush{};
    bool playing = false;
    bool minimal = false;
    bool stopped = false;
    bool vertical = false;
    bool currentDoubleLine = false;
};

// Layout 仅借用；布局重建前清理缓存，特效拥有自己的几何和离屏资源。
class LyricEffectsRenderer {
    struct LyricFxToken {
        int64_t startMs = 0;
        int64_t endMs = 0;
        float first = 0.0f;
        float last = 0.0f;
        bool visible = false;
    };
    struct LyricFxSource {
        const IDWriteTextLayout* layout = nullptr;
        int line = -1;
        std::vector<LyricFxToken> tokens;
    };
    struct LyricFxPoint {
        D2D1_POINT_2F center{};
        D2D1_POINT_2F edge{};
        bool valid = false;
    };
    struct LyricFxGeometry {
        int line = -1;
        float width = 0;
        float height = 0;
        float rangeStart = 0;
        float rangeSize = 1;
        bool vertical = false;
        std::vector<LyricFxPoint> tokens;
        LyricFxPoint starAnchor{};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> glyphMask;
        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> glyphTarget;
    };

public:
    static float starRadius(float fontSize);
    void setContext(const LyricEffectsContext& context) { context_ = context; }
    bool consumeRedraw() { return std::exchange(redraw_, false); }
    void createBrushes();
    void discardDeviceResources();
    bool lyricEffectEnabled(LyricEffect effect) const;
    void syncLyricEffectClock(int64_t position, bool playing, bool reset = false);
    bool lyricEffectsAnimating() const;
    void clearLyricEffectGeometry();
    bool lyricStarEnabled(int lineIndex) const;
    bool wordWaveEnabled(int lineIndex) const;
    bool prepareWordWave(IDWriteTextLayout* layout, int lineIndex);
    void drawWordWaveText(D2D1_POINT_2F origin, ID2D1Brush* brush) const;
    const std::vector<LyricFxToken>& lyricEffectTokens(int lineIndex);
    float lyricEffectProgress(int lineIndex);
    LyricFxGeometry& lyricEffectGeometry(IDWriteTextLayout* layout, int lineIndex, float width,
                                         float height, bool vertical, float rangeStart,
                                         float rangeSize);
    void ensureLyricGlyphMask(IDWriteTextLayout* layout, LyricFxGeometry& geometry);
    void drawLyricOverlay(IDWriteTextLayout* layout, int lineIndex, float x, float y, float width,
                          float height, bool vertical, float opacity, float progress,
                          bool foreground, float rangeStart = 0.0f, float rangeSize = 1.0f);

private:
    float lyricStarRadius() const { return starRadius(context_.fontSize); }
    LyricEffectsContext context_;
    bool redraw_ = false;
    void requestFrame() { redraw_ = true; }
    static ULONGLONG monotonicNowMs() { return taskbar_detail::monotonicNowMs(); }
    ID2D1DeviceContext* drawTarget() const { return context_.target; }
    int64_t lyricFxEmitFromMs_ = 0;
    int64_t lyricFxLastPositionMs_ = -1;
    ULONGLONG lyricFxLastClockMs_ = 0;
    ULONGLONG lyricFxPauseMs_ = 0;
    bool lyricFxPlaying_ = false;
    std::array<LyricFxSource, 2> lyricFxSources_{};
    std::unordered_map<const IDWriteTextLayout*, std::unique_ptr<LyricFxGeometry>> lyricFxGeometry_;
    LyricStarShape lyricStar_;
    WordWaveRenderer wordWave_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricFx_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brushLyricFollow_;
};
