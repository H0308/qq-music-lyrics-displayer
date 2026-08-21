#pragma once

#include "lyric_provider.h"

#include <filesystem>
#include <string>
#include <vector>

// 解密 QQ 音乐 lyric_download.fcg 返回的十六进制 QRC，并解析为现有逐字时间轴。
bool decodeQrcLyrics(const std::string& encryptedHex, std::vector<LyricLine>& out);

// 解密 QQ 音乐客户端本地缓存的二进制 QRC，返回 UTF-8 歌词文本（可能是 QRC 或 LRC）。
bool decodeLocalQrcText(const std::filesystem::path& path, std::string& out);

// 解析已经解包的 UTF-8 QRC 文本。
bool parseQrcLyricsText(const std::string& content, std::vector<LyricLine>& out);

