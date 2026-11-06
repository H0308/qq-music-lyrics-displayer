#pragma once
#include <windows.h>

// 独占 WS_CHILD / WS_POPUP 与父窗口关系的变更及验证。
class TaskbarWindowAttachment {
public:
    bool attach(HWND window, HWND taskbar);
    bool detach(HWND window);
    bool embedded() const { return embedded_; }
    void reset() { embedded_ = false; }

private:
    bool embedded_ = false;
};
