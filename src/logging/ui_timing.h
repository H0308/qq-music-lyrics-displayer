#pragma once
#include <windows.h>
#include <cstdint>

namespace runtime_log {
// UI 阶段耗时使用性能计数器；GetTickCount64 的时钟粒度不足以区分单帧开销。
inline uint64_t uiTimeMs() {
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    const auto ticks = static_cast<uint64_t>(value.QuadPart);
    return ticks / frequency * 1000 + (ticks % frequency) * 1000 / frequency;
}
} // namespace runtime_log
