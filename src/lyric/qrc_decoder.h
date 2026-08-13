#pragma once

#include "lyric_provider.h"

#include <string>
#include <vector>

// 解密 QQ 音乐 lyric_download.fcg 返回的十六进制 QRC，并解析为现有逐字时间轴。
bool decodeQrcLyrics(const std::string& encryptedHex, std::vector<LyricLine>& out);

