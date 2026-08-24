#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

// Win11 Fluent 风格主题支持：
// 系统深/浅色检测、DWM 圆角/Mica/深色标题栏、统一调色板与字体。
namespace fluent {

// 主题跟随来源：系统模式读取 SystemUsesLightTheme，应用模式读取
// AppsUseLightTheme；Light/Dark 用于用户固定选择。
enum class ThemeMode {
    FollowSystem,
    FollowApp,
    Light,
    Dark,
};

// 需要独立主题来源的渲染面。
enum class ThemeTarget {
    Taskbar,
    Window,
};

// 普通桌面窗口统一使用的紧凑密度尺寸（单位：DIP）。
// 这些尺寸对应文档中的 4/8/12/16/24 间距等级，避免各窗口各自漂移。
namespace metrics {
inline constexpr float pagePadding = 24.0f;
inline constexpr float sectionGap = 16.0f;
inline constexpr float controlGap = 12.0f;
inline constexpr float compactGap = 8.0f;
inline constexpr float controlHeight = 32.0f;
inline constexpr float controlRadius = 6.0f;
inline constexpr float cardRadius = 8.0f;
} // namespace metrics

// 设置任务栏歌词和普通窗口各自的主题模式。
void setThemeModes(ThemeMode taskbarMode, ThemeMode windowMode);

// 解析后的主题：无参数版本兼容普通窗口，任务栏使用显式 target。
bool isDarkMode();
bool isDarkMode(ThemeTarget target);

// 系统强调色（取不到时回退 Win11 默认蓝）
COLORREF accentColor();

D2D1_COLOR_F toD2D(COLORREF c, float alpha = 1.0f);

// Fluent 调色板（按普通窗口主题返回）；任务栏宿主使用显式 target。
struct Palette {
    D2D1_COLOR_F text;           // 主文字
    D2D1_COLOR_F textSecondary;  // 次要文字（提示/状态）
    D2D1_COLOR_F disabled;       // 禁用文字
    D2D1_COLOR_F cardFill;       // 输入框/卡片填充（半透明，叠在 Mica 上）
    D2D1_COLOR_F cardStroke;     // 卡片描边
    D2D1_COLOR_F windowBg;       // Mica 底色近似（不透明，用于非分层控件铺底）
    D2D1_COLOR_F cardFillSolid;  // cardFill 叠在 Mica 底色后的等效不透明色
    D2D1_COLOR_F controlFill;    // 普通按钮填充
    D2D1_COLOR_F controlHover;   // 普通按钮悬停
    D2D1_COLOR_F controlPressed; // 普通按钮按下
    D2D1_COLOR_F accent;         // 强调色按钮
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F accentPressed;
    D2D1_COLOR_F textOnAccent;   // 强调色按钮上的文字
    D2D1_COLOR_F listHover;      // 列表行悬停
    D2D1_COLOR_F listSelected;   // 列表行选中
    D2D1_COLOR_F menuFill;       // 菜单背景（不透明）
    D2D1_COLOR_F menuStroke;     // 菜单边框
    D2D1_COLOR_F separator;      // 分隔线
    D2D1_COLOR_F editText;       // EDIT 真控件文字色
};
const Palette& palette();
const Palette& palette(ThemeTarget target);

// ---- DWM Win11 窗口元素 ----
void applyRoundCorners(HWND hwnd, bool smallCorners = false);
bool applyBackdrop(HWND hwnd, bool transientWindow); // Mica / 亚克力；返回是否应用成功
void clearBackdrop(HWND hwnd); // 关闭背景材质（恢复不透明客户区）
void applyDarkCaption(HWND hwnd, bool dark);
void applyBorderColor(HWND hwnd, COLORREF color);
// 不绘制窗口边框（用于弹出菜单等无边框窗口）
void suppressBorder(HWND hwnd);
// 普通窗口一键套用：圆角 + 背景材质 + 标题栏配色；返回背景材质是否生效。
// 普通窗口默认使用 Mica，短暂的取色弹窗等场景传 true 使用 Acrylic。
bool styleDialogWindow(HWND hwnd, bool transientWindow = false);
// 主题变化时重新套用窗口样式，oldBackdrop 传调用方保存的 styleDialogWindow 旧返回值。
// 旧状态在客户区画过不透明底色（oldBackdrop=false）而新状态启用了 DWM 材质时，
// 残留像素会盖住材质（深色→浅色切换后背景残留深色），通过隐藏再显示强制 DWM 丢弃旧表面。
bool restyleDialogWindow(HWND hwnd, bool oldBackdrop, bool transientWindow = false);
// 背景材质未生效时的实心回退背景色
COLORREF fallbackBgColor();
// 绘制普通对话框根背景；深色模式不依赖 DWM 材质。
void paintDialogBackground(HWND hwnd, HDC hdc, bool backdrop);

// ---- 字体 ----
// DWrite 渲染用的 UI 字体族名
const wchar_t* uiFontFamily();
// 为缺失字形设置 UI 专用字体回退链；调用方继续负责释放文本格式。
void applyUiFontFallback(IDWriteTextFormat* format);
// GDI 真控件（EDIT）用字体；调用方负责 DeleteObject
HFONT createUiFont(UINT dpi, float dipSize = 14.0f, int weight = FW_NORMAL);

inline float dipScale(UINT dpi) {
    return static_cast<float>(dpi) / 96.0f;
}

} // namespace fluent
