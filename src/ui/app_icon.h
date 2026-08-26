#pragma once

#include <windows.h>

namespace app_icon {

// 按“任务栏歌词主题”选择托盘和任务栏窗口图标。
HICON taskbarIcon();

// 按“任务栏歌词主题”选择普通窗口标题栏和任务栏按钮图标。
HICON windowIcon();

// 更新已经创建的普通窗口的大小图标。
void applyWindowIcon(HWND hwnd);

// 更新已经创建的任务栏窗口的大小图标。
void applyTaskbarIcon(HWND hwnd);

} // namespace app_icon
