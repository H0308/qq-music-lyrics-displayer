#pragma once

#include <cstdint>
#include <vector>

// QQ 音乐 QRC 使用的非标准 3DES（保留客户端算法中的 S 盒差异）。
// Based on WXRIW/Lyricify-Lyrics-Helper, Apache-2.0.
bool decryptQrcDes(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

