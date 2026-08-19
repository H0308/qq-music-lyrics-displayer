#pragma once

#include <windows.h>
#include <dwrite.h>

// 用户选择的歌词字体样式。粗斜体同时启用粗体和斜体。
enum class LyricFontStyle {
    Normal,
    Bold,
    Italic,
    BoldItalic,
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
