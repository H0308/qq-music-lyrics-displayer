#pragma once
#include "ui/lyric_window.h"
#include <d2d1.h>

namespace taskbar_detail {
inline constexpr float kCoverPadding = 4.0f;
inline constexpr float kTextPadding = 8.0f;
inline constexpr float kSongInfoLyricGap = 8.0f;
inline constexpr float kLeftRatio = 0.38f;
inline constexpr float kVerticalRailPadding = 4.0f;
inline constexpr float kVerticalCoverGap = 6.0f;
inline constexpr float kVerticalControlsGap = 4.0f;
inline constexpr float kVerticalLyricGap = 6.0f;
struct LayoutMetrics {
    float w = 0.0f;
    float h = 0.0f;
    float leftW = 0.0f;
    float rightW = 0.0f;
    float immersiveControlsW = 0.0f;
};
struct VerticalLayout {
    float w = 0.0f;
    float h = 0.0f;
    float coverX = 0.0f;
    float coverY = 0.0f;
    float coverSize = 0.0f;
    float lyricY = 0.0f;
    float lyricBottom = 0.0f;
    bool showControls = false;
    float controlRadius = 0.0f;
    D2D1_POINT_2F controlCenters[4]{};
};

struct HorizontalLayoutInput {
    float width;
    float height;
    DisplayScene scene;
    bool expanded;
    bool immersiveControls;
    bool songInfoVisible;
    bool albumCoverVisible;
    float controlsWidth;
    float spectrumWidth;
    float coverSize;
    float coverSlotWidth;
};

// 纯几何计算。渲染与命中检测读取同一份布局，窗口句柄和设备资源不进入计算。
LayoutMetrics horizontalLayout(const HorizontalLayoutInput& input);
VerticalLayout verticalLayout(float width, float height, DisplayScene scene, bool albumCoverVisible,
                              bool showControls);
} // namespace taskbar_detail
