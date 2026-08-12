#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <windows.h>

// 从封面图片字节流（PNG/JPEG）提取主色调，用作已播放歌词颜色。
// 量化直方图取最高频色后只做亮度钳制，保持封面原本的色相与饱和度。
std::optional<COLORREF> extractDominantColor(const std::vector<uint8_t>& imageBytes);
