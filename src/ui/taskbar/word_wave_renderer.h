#pragma once

#include "lyric/lyric_types.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstddef>
#include <vector>

// 保存完整排版后的字形，只改变字形的升降偏移，不重新拆字排版。
class WordWaveRenderer {
public:
    static float offset(const LyricChar& token, int64_t lineMs, int64_t positionMs, float fontSize);
    static int64_t endTime(const LyricLine& line);
    static float lineOffset(int64_t lineMs, int64_t endMs, int64_t positionMs,
                            float fontSize, float along);
    static bool animating(const LyricLine& line, int64_t positionMs);
    bool prepare(IDWriteTextLayout* layout, int lineIndex, const LyricLine& line,
                 int64_t positionMs, float fontSize);
    void draw(ID2D1RenderTarget* target, D2D1_POINT_2F origin, ID2D1Brush* brush) const;
    void clear();

private:
    struct Run {
        Microsoft::WRL::ComPtr<IDWriteFontFace> face;
        float size{}, x{}, y{};
        BOOL sideways{};
        UINT32 bidi{};
        DWRITE_MEASURING_MODE measuring{};
        std::vector<UINT16> indices;
        std::vector<float> advances;
        std::vector<DWRITE_GLYPH_OFFSET> originalOffsets, offsets;
        std::vector<size_t> tokens;
        std::vector<float> positions;
    };
    class Collector;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout_;
    int lineIndex_ = -1;
    std::vector<Run> runs_;
    std::vector<float> offsets_;
};
