#pragma once

#include "model/display_types.h"
#include <windows.h>
#include <dwrite.h>

// 用户选择的歌词字体样式。粗斜体同时启用粗体和斜体。

// 字号建议使用任务栏的实际逻辑尺寸，与字体窗口的显示器缩放无关。
struct LyricFontSizeContext {
    float availableDip = 0.0f;
    float mainScale = 1.0f;
    float extraScale = 0.0f;
    float gapDip = 0.0f;
    float minSize = 9.0f;
    float maxSize = 18.0f;
    bool star = false;
};

inline constexpr bool isBoldFontStyle(LyricFontStyle style) {
    return style == LyricFontStyle::Bold || style == LyricFontStyle::BoldItalic;
}

inline constexpr DWRITE_FONT_STYLE dwriteStyleOf(LyricFontStyle style) {
    return style == LyricFontStyle::Italic || style == LyricFontStyle::BoldItalic
               ? DWRITE_FONT_STYLE_ITALIC
               : DWRITE_FONT_STYLE_NORMAL;
}

inline constexpr DWRITE_FONT_WEIGHT dwriteWeightOf(LyricFontStyle style) {
    return isBoldFontStyle(style) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
}
