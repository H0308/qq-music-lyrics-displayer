#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace graphics {
// 进程内共享同一会话；首次调用初始化，应用窗口和工作线程退出后随进程清理。
Gdiplus::Status gdiplusStatus();
} // namespace graphics
