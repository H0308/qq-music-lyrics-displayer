#pragma once
#include "lyric/lyric_types.h"
#include <dwrite.h>
#include <vector>

// 逐字几何与平滑进度同属一行；排版替换前清除几何，不持有 Layout 引用。
class KaraokeTracker {
public:
    void build(const std::vector<LyricLine>& lines, int lineIndex, IDWriteTextLayout* layout);
    float target(int line, IDWriteTextLayout* layout, int64_t position, int64_t& duration) const;
    float advance(int line, IDWriteTextLayout* layout, int64_t position, bool animations,
                  bool transitioning, ULONGLONG now);
    void clearGeometry();
    void reset();
    void retarget(ULONGLONG now);
    void finishTransition(int line, IDWriteTextLayout* layout, int64_t position, bool hasKaraoke,
                          ULONGLONG now);
    void cancelEntry() { entering_ = false; }
    void invalidateProgress() { settled_ = false; }
    bool settled() const { return settled_; }
    float progressForLine(int line) const { return line_ == line ? progress_ : 0.0f; }

private:
    struct Span {
        int64_t startMs{}, endMs{};
        float startX{}, endX{};
    };
    std::vector<Span> spans_;
    int geometryLine_ = -1;
    const IDWriteTextLayout* geometryLayout_ = nullptr;
    float progress_ = 0.0f, smooth_ = 0.0f;
    int line_ = -1;
    bool entering_ = false, settled_ = true;
    ULONGLONG tick_ = 0;
};
