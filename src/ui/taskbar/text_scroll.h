#pragma once
#include "lyric/lyric_types.h"
#include <windows.h>
#include <utility>
#include <vector>

struct TextScrollInput {
    ULONGLONG now{};
    bool animations{}, wasTransitioning{}, hold{}, vertical{}, showControls{};
    bool playing{}, idle{}, karaoke{}, songInfoVisible{}, showInlineControls{}, areaValid{};
    float area{}, infoWidth{}, titleWidth{}, artistWidth{}, lyricWidth{}, secondaryWidth{};
    float verticalExtent{}, karaokeProgress{};
};

// 保留各文本独立的偏移和一次性文案计数；几何只从本帧输入读取。
class TextScroll {
public:
    void advance(const TextScrollInput& input);
    void setLineTiming(const std::vector<LyricLine>& lines, int line, float extent, float gap);
    void resetClock() { lastTickMs_ = 0; }
    void resetTitle() { titleScrollOffset_ = 0.0f; }
    void resetArtist() { artistScrollOffset_ = 0.0f; }
    void resetLyric() { lyricScrollOffset_ = 0.0f; }
    void resetSecondary() { secondaryScrollOffset_ = 0.0f; }
    float title() const { return titleScrollOffset_; }
    float artist() const { return artistScrollOffset_; }
    float lyric() const { return lyricScrollOffset_; }
    float secondary() const { return secondaryScrollOffset_; }
    bool animating() const { return scrollAnimating_; }
    bool oneShot() const { return statusTextOneShot_; }
    void setOneShot(bool enabled, ULONGLONG now) {
        statusTextOneShot_ = enabled;
        statusTextOneShotStartMs_ = enabled ? now : 0;
        statusTextOneShotRounds_ = 0;
    }
    bool consumeCompleted() { return std::exchange(statusTextCycleCallbackPending_, false); }

private:
    float titleScrollOffset_ = 0.0f, artistScrollOffset_ = 0.0f;
    float lyricScrollOffset_ = 0.0f, secondaryScrollOffset_ = 0.0f;
    float lyricScrollSpeed_ = 15.0f;
    ULONGLONG lastTickMs_ = 0;
    bool scrollAnimating_ = true;
    bool statusTextOneShot_ = false;
    ULONGLONG statusTextOneShotStartMs_ = 0;
    int statusTextOneShotRounds_ = 0;
    bool statusTextCycleCallbackPending_ = false;
};
