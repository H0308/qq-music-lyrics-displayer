#pragma once
#include "model/display_types.h"
#include <windows.h>
#include <vector>
#include <span>
#include <utility>
namespace taskbar_detail {
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int availableMajor = 0;
};
struct OccupiedRegions {
    bool vertical = false;
    bool hasNotify = false;
    bool hasStart = false;
    RECT notifyRect{};
    RECT startRect{};
    RECT trafficMonitor{};
    std::span<const RECT> buttons;
    std::span<const RECT> kugouWindows;
};
struct PlacementInput {
    RECT taskbar{};
    UINT edge = 0;
    bool vertical = false;
    bool allowOverlap = false;
    int positionMode = 0;
    int gap = 0;
    int minMajor = 0;
    int compressedMinMajor = 0;
    int maxMajor = 0;
    int crossPixels = 0;
    int crossMargin = 0;
    std::vector<std::pair<int, int>> occupied;
};
std::vector<std::pair<int, int>> occupiedIntervals(const OccupiedRegions& input);
TaskbarPlacementStatus calculatePlacement(const PlacementInput& input, WindowPlacement& placement);
WindowPlacement dragPreview(const WindowPlacement& anchor, int mode, bool vertical,
                            int desiredMajor);
} // namespace taskbar_detail
