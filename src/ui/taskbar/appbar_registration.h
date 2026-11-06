#pragma once
#include "model/display_types.h"
#include <windows.h>

// 仅管理 Shell AppBar 协议；DPI 和渲染失效仍由宿主处理。
class AppBarRegistration {
public:
    bool registered() const { return registered_; }
    bool fullscreen() const { return fullscreen_; }
    void setFullscreen(bool value) { fullscreen_ = value; }
    void forget() { registered_ = false; }
    bool registerWindow(HWND window, UINT callbackMessage);
    void unregisterWindow(HWND window);
    bool positionWindow(HWND window, HMONITOR monitor, AppBarEdge edge, LONG height);
    void updateZOrder(HWND window);

private:
    bool registered_ = false;
    bool fullscreen_ = false;
    bool positioning_ = false;
};
