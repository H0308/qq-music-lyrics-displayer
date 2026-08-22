#pragma once

#include "lyric_window.h"

#include <functional>
#include <memory>

#include <windows.h>

// 任务栏歌词悬浮时展开的媒体卡片。它是独立的非激活窗口，
// 只消费宿主已经整理好的媒体快照，不直接访问播放器或歌词提供器。
class MediaPopup {
public:
    MediaPopup();
    ~MediaPopup();

    MediaPopup(const MediaPopup&) = delete;
    MediaPopup& operator=(const MediaPopup&) = delete;

    bool create(HINSTANCE inst, HWND anchor);
    void destroy();

    void setControlCallback(std::function<void(MediaControl)> cb);
    void setEnabled(bool enabled);
    void setBackgroundMode(MediaPopupBackground mode);
    void setMedia(const OverlayMediaInfo& info, bool available);
    void setAnchor(HWND anchor);

    void onAnchorEnter();
    void onAnchorLeave();
    void hideImmediate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
