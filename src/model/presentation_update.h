#pragma once

// 低频展示更新：明确区分内容重建、转场和设置窗口同步；高频进度使用 PlaybackPatch。
struct PresentationUpdate {
    bool animateTransition = false;
    bool lyricsChanged = false;
    bool durationOnlyUpdate = false;
    bool refreshSettings = false;
};
