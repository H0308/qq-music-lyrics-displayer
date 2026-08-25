#pragma once

#include "lyric_window.h"

#include <memory>

#include <windows.h>

// 切歌时在主屏幕中下方短暂弹出的歌曲信息条（灵动岛式）。
// 独立置顶窗口：封面 + 标题 + 艺术家单行，磨砂半透明背景，
// 始终依据背后内容的明暗自动切换文字颜色；窗口固定鼠标穿透、不可交互。
class SongToast {
public:
    SongToast();
    ~SongToast();

    SongToast(const SongToast&) = delete;
    SongToast& operator=(const SongToast&) = delete;

    bool create(HINSTANCE inst);
    void destroy();

    void setEnabled(bool enabled);
    // 自动收起前的停留秒数（1~10）
    void setDurationSec(int seconds);
    // true 显示在主屏幕中上方（与中下对称，出入场动画方向相反），false 中下方
    void setPlacementTop(bool top);
    // 切歌触发：更新内容并从下方滑入，停留设定时长后自动收起
    void showSong(const OverlayMediaInfo& info);
    // 可见期间异步补齐媒体字段（封面等）；空标题视为无播放，立即收起
    void setMedia(const OverlayMediaInfo& info);
    void hideImmediate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
