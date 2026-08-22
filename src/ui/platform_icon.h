#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace platform_icon {

// 从媒体来源标识对应的播放器进程读取预乘 BGRA 图标像素。
// sourceAppUserModelId 可以是进程名、可访问的图标路径，或网易云桥接器名称。
bool readSourceIconPixels(const std::wstring& sourceAppUserModelId,
                          std::vector<BYTE>& pixels, UINT& width, UINT& height);

// 激活媒体来源对应的播放器窗口；没有可见实例时再按来源标识启动它。
bool launchSourceApp(const std::wstring& sourceAppUserModelId);

} // namespace platform_icon
