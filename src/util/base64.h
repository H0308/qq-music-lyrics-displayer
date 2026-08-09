#pragma once

#include <string>

// 解码 base64 文本（QQ 歌词接口 lyric/trans 字段为 base64 编码的 UTF-8）
std::string base64Decode(const std::string& input);
