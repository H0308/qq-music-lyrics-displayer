#include "ui/taskbar/layout.h"
#include <algorithm>

namespace taskbar_detail {
LayoutMetrics horizontalLayout(const HorizontalLayoutInput& input) {

    LayoutMetrics m;
    m.w = input.width;
    m.h = input.height;
    const bool immersiveControls = input.immersiveControls;
    m.immersiveControlsW = input.controlsWidth;
    if (input.scene == DisplayScene::Idle) {
        m.leftW = 0.0f;
        m.rightW = std::max(1.0f, m.w - m.immersiveControlsW - input.spectrumWidth);
        return m;
    }
    float effW = m.w - input.spectrumWidth;
    const float immersiveStart = input.songInfoVisible ? kSongInfoLyricGap : kTextPadding;
    const float contentW =
        std::max(1.0f, effW - m.immersiveControlsW - (immersiveControls ? immersiveStart : 0.0f));
    if (input.expanded && input.songInfoVisible) {
        // 沉浸模式的宽度来自整个任务栏，歌曲信息只保留稳定的左侧栏，
        // 把主要空间留给歌词和右侧频谱，避免 1920px 任务栏出现过宽信息区。
        const float available = contentW;
        const float minimum = input.albumCoverVisible
                                  ? kCoverPadding + input.coverSize + kTextPadding + 80.0f
                                  : 140.0f;
        const float preferred = std::clamp(available * 0.24f, 180.0f, 280.0f);
        m.leftW = std::min(available, std::max(minimum, preferred));
    } else if (input.expanded) {
        // 没有歌曲信息时，只有实际显示的封面需要占用左侧安全区。
        // 这样切换封面不会改变歌词的中心锚点，也不会给控件留下无意义的空洞。
        m.leftW = input.albumCoverVisible ? input.coverSlotWidth : 0.0f;
    } else {
        m.leftW = input.songInfoVisible ? contentW * kLeftRatio : input.coverSlotWidth;
    }
    m.rightW = std::max(1.0f, m.w - m.leftW - m.immersiveControlsW);
    return m;
}

VerticalLayout verticalLayout(float width, float height, DisplayScene scene, bool albumCoverVisible,
                              bool showControls) {

    VerticalLayout layout;
    layout.w = width;
    layout.h = height;
    if (layout.w <= 0.0f || layout.h <= 0.0f)
        return layout;

    const float pad = std::min(kVerticalRailPadding, layout.w * 0.18f);
    const float contentW = std::max(1.0f, layout.w - pad * 2.0f);
    const bool playbackScene = scene != DisplayScene::Idle && scene != DisplayScene::NoPlayback;
    if (albumCoverVisible && playbackScene) {
        layout.coverSize = contentW;
        layout.coverX = (layout.w - layout.coverSize) * 0.5f;
        layout.coverY = pad;
    }

    layout.lyricY =
        layout.coverSize > 0.0f ? layout.coverY + layout.coverSize + kVerticalCoverGap : pad;
    layout.lyricBottom = std::max(layout.lyricY + 1.0f, layout.h - pad);
    layout.showControls = showControls && playbackScene;
    if (layout.showControls) {
        layout.controlRadius = std::clamp(layout.w * 0.23f, 6.0f, 9.0f);
        const float diameter = layout.controlRadius * 2.0f;
        // 与横向内嵌控件复用同一套视觉节奏：按钮中心间距约为 2.8r。
        const float transportPitch = layout.controlRadius * 2.8f;
        const float transportGap = std::max(kVerticalControlsGap, transportPitch - diameter);
        // 音量是独立的辅助操作，与上一首/播放/下一首之间留出更明显的层级间距。
        const float volumeGap = transportGap + layout.controlRadius * 0.9f;
        const float blockH = diameter * 4.0f + transportGap * 2.0f + volumeGap;
        const float controlsTop = layout.lyricY;
        const float controlsBottom = layout.h - pad;
        const float freeSpace = controlsBottom - controlsTop - blockH;
        const float groupTop = controlsTop + std::max(0.0f, freeSpace) * 0.5f;
        const float firstY = groupTop + layout.controlRadius;
        if (freeSpace >= 0.0f && groupTop - kVerticalLyricGap > layout.lyricY) {
            for (int i = 0; i < 3; ++i)
                layout.controlCenters[i] =
                    D2D1::Point2F(layout.w * 0.5f, firstY + i * transportPitch);
            layout.controlCenters[3] = D2D1::Point2F(
                layout.w * 0.5f, firstY + transportPitch * 2.0f + diameter + volumeGap);
            layout.lyricBottom = groupTop - kVerticalLyricGap;
        } else {
            layout.showControls = false;
        }
    }
    return layout;
}

float expandedSpectrumWidth(float hostWidth) {
    return hostWidth <= 0.0f ? 0.0f : std::clamp(hostWidth * 0.22f, 220.0f, 440.0f);
}

float expandedClockWidth(float hostWidth) {
    return hostWidth <= 0.0f ? 0.0f : std::clamp(hostWidth * 0.045f, 80.0f, 96.0f);
}

ExpandedSideLayout expandedSideLayout(float w, bool dock, float resourceWidth) {
    ExpandedSideLayout result;
    result.zoneWidth = expandedSpectrumWidth(w);
    result.resourceWidth = dock ? resourceWidth : 0.0f;
    result.resourceX = w - result.resourceWidth - (dock ? 3.0f : kTextPadding);
    result.zoneX =
        dock ? result.resourceX - 10.0f - result.zoneWidth : w - result.zoneWidth - kTextPadding;
    result.clockWidth = dock ? 0.0f : std::min(result.zoneWidth, expandedClockWidth(w));
    result.clockX = w - result.clockWidth - kTextPadding;
    result.spectrumRight = dock ? result.resourceX - 10.0f : result.clockX - 10.0f;
    result.spectrumWidth = std::max(0.0f, result.spectrumRight - result.zoneX);
    return result;
}
} // namespace taskbar_detail
