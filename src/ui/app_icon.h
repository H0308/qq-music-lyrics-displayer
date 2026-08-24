#pragma once

#include <windows.h>

namespace app_icon {

// 按当前任务栏主题选择托盘图标。
HICON taskbarIcon();

// 按当前普通窗口主题选择标题栏图标。
HICON windowIcon();

// 更新已经创建的普通窗口的大小图标。
void applyWindowIcon(HWND hwnd);

} // namespace app_icon
