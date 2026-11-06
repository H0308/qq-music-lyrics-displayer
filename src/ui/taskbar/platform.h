#pragma once
#include "ui/taskbar_host.h"
#include <uiautomation.h>

namespace taskbar_detail {

// Windows 任务栏发现与占用区域探测，不持有渲染器或歌词状态。
bool isTaskbarCenterAlign();
bool isValidTaskbarEdge(UINT edge);
UINT queryTaskbarEdge(HWND taskbar, const RECT& taskbarRect);
bool isVerticalTaskbarEdge(UINT edge);
int taskbarCrossPixels(const RECT& rect, UINT edge);
HWND findTrafficMonitorWnd(HWND taskbar);
void queryKugouTaskbarWindows(HWND taskbar, std::vector<RECT>& out);
void queryTaskbarButtonsUia(IUIAutomation* uia, HWND taskbar, std::vector<RECT>& out);

} // namespace taskbar_detail
