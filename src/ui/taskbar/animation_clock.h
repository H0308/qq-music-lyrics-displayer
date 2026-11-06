#pragma once
#include <windows.h>
namespace taskbar_detail {
// 同一单调时间来源；各动画分别保留暂停、恢复和推进状态。
inline ULONGLONG monotonicNowMs() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const LONGLONG wholeMs = counter.QuadPart / frequency.QuadPart * 1000;
    const LONGLONG remainderMs =
        (counter.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart;
    return static_cast<ULONGLONG>(wholeMs + remainderMs);
}
} // namespace taskbar_detail
