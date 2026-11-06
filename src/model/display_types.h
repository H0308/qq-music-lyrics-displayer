#pragma once
#include <windows.h>
#include <cstdint>

// 设置与显示共同使用的值类型，不依赖窗口或渲染器。
inline constexpr wchar_t kDefaultUiFontFamily[] = L"Noto Sans SC";
enum class SecondaryLyricAvailability { Available, Loading, Unavailable };
enum class LyricFontStyle {
    Normal,
    Bold,
    Italic,
    BoldItalic,
};
namespace fluent {
enum class ThemeMode {
    FollowSystem,
    FollowApp,
    Light,
    Dark,
};
}
enum class AlbumCoverEffect {
    Default,
    Vinyl,
};

enum class LyricAlignment {
    Left,
    Center,
    Right,
};

enum class HoverControlStyle {
    Inline,
    Popup,
};

// 媒体卡片展开方式：悬浮停留片刻后展开，或点击歌词区域任意位置立即展开
enum class MediaPopupTrigger {
    Hover,
    Click,
};

enum class SpectrumStyle {
    Default,
    Bars,
    DreamyWave,
    Segmented,
    BottomBand,
};

// 任务栏歌词背景：无 / 封面模糊 / 纯色（跟随任务栏深浅色）；
// 画在最底层，可与播放进度背景、频谱背景波浪叠加
enum class TaskbarBackground {
    None,
    CoverBlur,
    Solid,
};

enum class TaskbarPlacementStatus {
    Unavailable,
    Safe,
    Compressed,
    Relocated,
    NoSpace,
    ForcedOverlap,
};

// 复用 Dock 资源区；CPU、内存为基础指标，其余指标按开关紧凑排列。
struct DockResourceVisibility {
    bool embeddedEnabled = false;
    bool download = true;
    bool upload = true;
    bool gpuUsage = false;
    bool cpuFrequency = false;
    bool operator==(const DockResourceVisibility&) const = default;
};

// 任务栏内容动态背景；按作用范围覆盖实际显示的内容区域，不用于悬浮卡片。
enum class IdleQuoteBackground {
    None,
    FallingLeaves,
    TwinklingStars,
    BinaryRain,
    FloatingParticles,
};

// 作用范围：不启用 / 每日一言和资源监视 / 封面、歌曲信息、歌词和频谱 / 整体。
enum class IdleQuoteBackgroundScope {
    None,
    DailyQuote,
    Lyrics,
    All,
};

// 任务栏歌词的承载方式。嵌入模式只占用任务栏中的一段空闲区，沉浸模式
// 覆盖当前任务栏客户区，AppBar 作为独立顶栏/底栏保留桌面工作区。
enum class TaskbarViewMode {
    Embedded,
    Immersive,
    AppBar,
};

enum class AppBarEdge {
    Top,
    Bottom,
};

enum class RenderMode {
    Normal = 0,
    Low = 1,
    Stopped = 2,
    Minimal = 3,
};
enum class MediaPopupBackground {
    Solid,
    Frosted,
};

// 正式展示的语义场景，由展示帧、任务栏宿主与绘制组件共同使用。
enum class DisplayScene {
    NoPlayback,
    Idle,
    Searching,
    Lyrics,
    Message,
    Spectrum,
};

struct LyricAppearance {
    COLORREF played = RGB(49, 194, 124);
    COLORREF unplayed = RGB(49, 194, 124);
    int unplayedAlphaPct = 45;
    COLORREF glowColor = RGB(49, 194, 124);
    COLORREF outlineColor = RGB(0, 0, 0);
    bool glowOn = false;
    bool outlineOn = false;
};
