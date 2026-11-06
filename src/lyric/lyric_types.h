#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 逐字时间轴（QRC/KRC/YRC 解析结果）；text 为歌词时间轴 token，英文常为整词
struct LyricChar {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::wstring text;
};

struct LyricLine {
    int64_t ms = 0;
    std::wstring text;
    std::vector<LyricChar> chars; // 非空表示有逐字时间轴；text 为 chars 拼接
    std::wstring translation;     // 与本行时间轴对齐的翻译（可空）
    std::wstring romanization;    // 与本行时间轴对齐的罗马音（可空）
};
