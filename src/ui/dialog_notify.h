#pragma once

#include <windows.h>

// 对话框窗口销毁后向托盘窗口投递此消息，由 App 释放对应的 C++ 对象。
// 不能在对话框自身窗口过程里同步 delete（窗口过程还在调用栈上）。
constexpr UINT kMsgDialogClosed = WM_APP + 210;

// kMsgDialogClosed 的 wParam：被销毁的对话框
enum class DialogKind : WPARAM {
    Settings = 1,
    About = 2,
    ManualSearch = 3,
    FontPicker = 4,
    FontColor = 5,
    RuntimeLog = 6,
};
