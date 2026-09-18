#pragma once

#include <d2d1.h>

namespace settings_icon {

// 设置窗口使用的轻量线性矢量图标。图标坐标按 bounds 自动缩放，
// 不依赖外部位图或 SVG 资源，因此可以跟随窗口 DPI 和主题实时重绘。
enum class Kind {
    None,
    Display,
    Performance,
    Card,
    Media,
    Spectrum,
    Lyrics,
    Toast,
    Idle,
    Theme,
    Font,
    SongInfo,
    AlbumCover,
    Platform,
    Vinyl,
    RenderMode,
    Hover,
    Control,
    Trigger,
    Background,
    Color,
    Follow,
    Contrast,
    Progress,
    Opacity,
    DynamicBackground,
    Scope,
    Quote,
    Language,
    Refresh,
    Apps,
    Tray,
    Api,
    Connect,
    Sync,
    Disconnect,
    Duration,
    Fullscreen,
    Position,
    DoubleLine,
    Alignment,
    LocalLyrics,
    Search,
    Log,
    Settings,
    Info,
    Exit,
    Folder,
    Persist,
    Gradient,
    Wave,
};

// 以 brush 的当前颜色绘制一个小型矢量图标；bounds 建议使用 16~20 DIP。
void draw(ID2D1RenderTarget* target, Kind kind, const D2D1_RECT_F& bounds,
          ID2D1Brush* brush, float strokeWidth = 1.25f);

} // namespace settings_icon
