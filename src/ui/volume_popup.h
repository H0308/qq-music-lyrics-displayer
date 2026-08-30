#pragma once

#include <functional>
#include <memory>

#include <windows.h>

// 任务栏内嵌控件音量按钮悬停时弹出的小型音量滑块浮窗。
// 独立的非激活置顶弹出窗，只消费宿主推送的音量状态并通过回调上报拖动结果。
class VolumePopup {
public:
    VolumePopup();
    ~VolumePopup();

    VolumePopup(const VolumePopup&) = delete;
    VolumePopup& operator=(const VolumePopup&) = delete;

    bool create(HINSTANCE inst);
    void destroy();

    void setVolume(int percent, bool muted, bool available);
    // 拖动/滚轮调整音量（percent 0-100）
    void setCallback(std::function<void(int percent)> cb);

    // 横向任务栏显示在锚点上方/下方；侧边任务栏贴着任务栏左右显示，并按锚点垂直居中
    void showNear(const RECT& anchorRect, bool verticalTaskbar, bool popupOnRight);
    void hide();
    bool isVisible() const;

    // 锚点（音量按钮）悬停状态，配合浮窗自身悬停决定延迟隐藏
    void onAnchorEnter();
    void onAnchorLeave();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
