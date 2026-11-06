#pragma once
#include <windows.h>
namespace settings_ui {
inline constexpr float kSearchPaletteHeaderH = 52.0f;
inline constexpr float kSearchPaletteFooterH = 32.0f;
inline constexpr float kSearchPaletteRowH = 56.0f;
inline constexpr float kSearchPaletteListPadY = 6.0f;
inline constexpr int kSearchPaletteMaxVisibleRows = 6;
inline constexpr float kSearchPaletteEmptyPromptH = 54.0f;
inline constexpr float kSearchPaletteNoResultH = 70.0f;
inline constexpr int kSearchPaletteHitNone = 0;
inline constexpr int kSearchPaletteHitClear = 1;
inline constexpr int kSearchPaletteHitClose = 2;
inline constexpr int kSearchPaletteHitRow = 3;
inline constexpr UINT_PTR kSearchPaletteAnimTimer = 4;
inline constexpr float kSearchPaletteOpenDurationMs = 165.0f;
inline constexpr float kSearchPaletteCloseDurationMs = 120.0f;
inline constexpr float kSearchPaletteOpenSlideDip = -10.0f;
inline constexpr float kSearchPaletteCloseSlideDip = -8.0f;
} // namespace settings_ui
