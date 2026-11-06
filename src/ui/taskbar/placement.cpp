#include "ui/taskbar/placement.h"
#include <shellapi.h>
#include <algorithm>
namespace taskbar_detail {
std::vector<std::pair<int, int>> occupiedIntervals(const OccupiedRegions& input) {
    std::vector<std::pair<int, int>> v;
    const bool vertical = input.vertical;
    auto add = [&v, vertical](const RECT& r) {
        const int start = vertical ? r.top : r.left;
        const int end = vertical ? r.bottom : r.right;
        if (end > start)
            v.emplace_back(start, end);
    };
    for (const RECT& r : input.buttons)
        add(r);
    if (input.hasNotify)
        add(input.notifyRect);
    if (input.hasStart)
        add(input.startRect);
    add(input.trafficMonitor);
    for (const RECT& r : input.kugouWindows)
        add(r);
    std::sort(v.begin(), v.end());
    std::vector<std::pair<int, int>> merged;
    for (const auto& p : v) {
        if (!merged.empty() && p.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, p.second);
        else
            merged.push_back(p);
    }
    return merged;
}
int previewMajorPixels(const WindowPlacement& anchor, bool vertical, int desiredMajor) {
    const int anchorMajor = anchor.availableMajor > 0 ? anchor.availableMajor
                                                      : (vertical ? anchor.height : anchor.width);
    const int currentMajor =
        desiredMajor > 0 ? desiredMajor : (vertical ? anchor.height : anchor.width);
    return std::clamp(currentMajor, 1, std::max(1, anchorMajor));
}
WindowPlacement dragPreview(const WindowPlacement& anchor, int mode, bool vertical,
                            int desiredMajor) {
    WindowPlacement preview = anchor;
    const int previewMajor = previewMajorPixels(anchor, vertical, desiredMajor);
    if (vertical) {
        preview.height = previewMajor;
        if (mode == 0)
            preview.y = anchor.y + anchor.height - preview.height;
    } else {
        preview.width = previewMajor;
        if (mode == 0)
            preview.x = anchor.x + anchor.width - preview.width;
    }
    return preview;
}
TaskbarPlacementStatus calculatePlacement(const PlacementInput& input, WindowPlacement& placement) {
    // 空闲区间 = 任务栏主轴减去占用区间
    struct Span {
        int l, r;
    };
    std::vector<Span> spans;
    const int majorStart = input.vertical ? input.taskbar.top : input.taskbar.left;
    const int majorEnd = input.vertical ? input.taskbar.bottom : input.taskbar.right;
    int cursor = majorStart;
    const auto& occupied = input.occupied;
    for (const auto& o : occupied) {
        if (o.first > cursor)
            spans.push_back({cursor, std::min(o.first, majorEnd)});
        cursor = std::max(cursor, std::min(o.second, majorEnd));
    }
    if (cursor < majorEnd)
        spans.push_back({cursor, majorEnd});
    // 只有确实没有探测到任何占用区间时，才把整条任务栏视为可用；
    // 如果占用区间已经覆盖整个主轴，必须保留“无空闲位置”的结果。
    if (spans.empty() && occupied.empty())
        spans.push_back({majorStart, majorEnd});

    auto usableMajor = [gap = input.gap](const Span& s) { return s.r - s.l - gap * 2; };

    int pxMajor = 0;
    int availableMajor = 0;
    int x = 0;
    int y = 0;
    auto place = [&](const Span& s, int w) {
        availableMajor = std::max(1, usableMajor(s));
        pxMajor = std::min(w, std::max(1, majorEnd - majorStart));
        int major = input.positionMode == 1 ? s.l + input.gap : s.r - input.gap - pxMajor;
        if (pxMajor <= majorEnd - majorStart)
            major = std::clamp(major, majorStart, majorEnd - pxMajor);
        else
            major = majorStart;
        if (input.vertical) {
            x = input.edge == ABE_LEFT
                    ? input.taskbar.left + input.crossMargin
                    : input.taskbar.right - input.crossMargin - input.crossPixels;
            y = major;
        } else {
            x = major;
            y = input.taskbar.top + input.crossMargin;
        }
    };
    TaskbarPlacementStatus result = TaskbarPlacementStatus::Unavailable;
    if (spans.empty()) {
        if (!input.allowOverlap)
            return TaskbarPlacementStatus::NoSpace;
        const Span full{majorStart, majorEnd};
        // 用户明确选择继续开启时，也沿用压缩后的最小主轴尺寸，
        // 不再恢复为标准最小尺寸，尽量降低对任务栏的遮挡。
        place(full, input.compressedMinMajor);
        result = TaskbarPlacementStatus::ForcedOverlap;
    } else {
        // 原位优先：模式 0 锚定通知区域之前的主轴末端空闲区，模式 1 锚定
        // 任务栏起始端空闲区。横向对应右/左，纵向对应下/上。
        const Span& pref = input.positionMode == 1 ? spans.front() : spans.back();
        if (usableMajor(pref) >= input.compressedMinMajor) {
            place(pref, std::min(usableMajor(pref), input.maxMajor)); // 原位优先：被挤压先收缩长度
            result = usableMajor(pref) < input.minMajor ? TaskbarPlacementStatus::Compressed
                                                        : TaskbarPlacementStatus::Safe;
        } else {
            // 原位压到安全最小长度仍放不下：换到容得下的最大空闲区。
            const Span* best = nullptr;
            for (const auto& s : spans) {
                if (usableMajor(s) >= input.compressedMinMajor &&
                    (!best || s.r - s.l > best->r - best->l))
                    best = &s;
            }
            if (best) {
                place(*best, std::min(usableMajor(*best), input.maxMajor));
                result = TaskbarPlacementStatus::Relocated;
            } else if (input.allowOverlap) {
                // 用户已明确允许重叠时，仍把窗口限制在任务栏主轴范围内，
                // 只允许与已探测到的控件相交，不让窗口越出任务栏；尺寸使用
                // 压缩后的最小值，避免强制开启时又恢复为标准尺寸。
                place(pref, input.compressedMinMajor);
                result = TaskbarPlacementStatus::ForcedOverlap;
            } else {
                return TaskbarPlacementStatus::NoSpace;
            }
        }
    }
    if (input.vertical) {
        if (y < input.taskbar.top)
            y = input.taskbar.top;
        if (pxMajor <= majorEnd - majorStart && y + pxMajor > input.taskbar.bottom)
            y = input.taskbar.bottom - pxMajor;
    } else if (x < input.taskbar.left) {
        x = input.taskbar.left;
    }

    placement.x = x;
    placement.y = y;
    placement.width = input.vertical ? input.crossPixels : pxMajor;
    placement.height = input.vertical ? pxMajor : input.crossPixels;
    placement.availableMajor = availableMajor;
    if (placement.width <= 0 || placement.height <= 0)
        return TaskbarPlacementStatus::Unavailable;
    return result;
}
} // namespace taskbar_detail
