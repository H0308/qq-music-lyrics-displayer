#pragma once

#include "lyric_provider.h"

#include <string>
#include <vector>

// 解析 UTF-8 LRC 文本；支持一行多个时间戳，并忽略 [ti:]/[ar:] 等元数据行。
std::vector<LyricLine> parseLrcText(const std::string& content);

// 解析单个 LRC 时间戳，供 YRC 的普通 LRC 回退行复用。
bool parseLrcTimestamp(const std::string& text, int64_t& milliseconds);
