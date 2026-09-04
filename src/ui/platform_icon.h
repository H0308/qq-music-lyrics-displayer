#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace platform_icon {

// 从媒体来源标识对应的播放器进程读取预乘 BGRA 图标像素。
// sourceAppUserModelId 可以是进程名、可访问的图标路径，或网易云桥接器名称。
bool readSourceIconPixels(const std::wstring& sourceAppUserModelId,
                          std::vector<BYTE>& pixels, UINT& width, UINT& height);

// 从用户配置的 EXE 路径读取正常大小的图标与文件描述。
bool readExeIconPixels(const std::wstring& path, std::vector<BYTE>& pixels, UINT& width,
                       UINT& height);
bool readExeDisplayName(const std::wstring& path, std::wstring& name);

// 激活媒体来源对应的播放器窗口；没有可见实例时再按来源标识启动它。
bool launchSourceApp(const std::wstring& sourceAppUserModelId);

// 按配置的 EXE 路径激活已运行实例，未运行时启动该路径。
bool launchConfiguredExe(const std::wstring& path);

// 使用 Windows Shell 打开 URL 或已注册的自定义 URI 协议。
bool launchUri(const std::wstring& uri);

} // namespace platform_icon
