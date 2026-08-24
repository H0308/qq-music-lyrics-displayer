#include "fluent_theme.h"

#include <dwmapi.h>
#include <dwrite_2.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <cmath>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace fluent {

// 部分 SDK 尚未定义的新版 DWM 属性
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

constexpr int kDwmWcpRound = 2;      // DWMWCP_ROUND
constexpr int kDwmWcpRoundSmall = 3; // DWMWCP_ROUNDSMALL
constexpr int kDwmsbtNone = 1;       // DWMSBT_NONE
constexpr int kDwmsbtMainWindow = 2;      // DWMSBT_MAINWINDOW (Mica)
constexpr int kDwmsbtTransientWindow = 3; // DWMSBT_TRANSIENTWINDOW (Acrylic)

ThemeMode gTaskbarThemeMode = ThemeMode::FollowSystem;
ThemeMode gWindowThemeMode = ThemeMode::FollowApp;

IDWriteFontFallback* createUiFontFallback() {
    IDWriteFactory* factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&factory))) ||
        !factory)
        return nullptr;

    IDWriteFactory2* factory2 = nullptr;
    IDWriteFontFallbackBuilder* builder = nullptr;
    IDWriteFontFallback* systemFallback = nullptr;
    IDWriteFontFallback* result = nullptr;

    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDWriteFactory2),
                                          reinterpret_cast<void**>(&factory2))) &&
        factory2 && SUCCEEDED(factory2->CreateFontFallbackBuilder(&builder)) && builder) {
        // 只覆盖中日韩相关区段；其他字符继续沿用 Windows 的完整系统回退表，
        // 这样不会因为自定义中文列表而影响符号、表情或其他文字脚本。
        const DWRITE_UNICODE_RANGE ranges[] = {
            {0x2E80, 0x2FFF}, {0x3000, 0x303F}, {0x3100, 0x312F}, {0x31C0, 0x31EF},
            {0x3200, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFAFF},
            {0xFE30, 0xFE4F}, {0xFF00, 0xFFEF}, {0x20000, 0x2FA1F},
        };
        const wchar_t* targetFamilies[] = {
            L"Noto Sans SC",       L"Noto Sans CJK SC", L"Microsoft YaHei UI",
            L"Microsoft YaHei",    L"DengXian",         L"SimHei", L"SimSun",
        };

        // 应用字体优先，系统字体补齐列表中未覆盖的字符。
        builder->AddMapping(ranges, static_cast<UINT32>(_countof(ranges)), targetFamilies,
                            static_cast<UINT32>(_countof(targetFamilies)), nullptr, L"zh-cn",
                            uiFontFamily(), 1.0f);
        if (SUCCEEDED(factory2->GetSystemFontFallback(&systemFallback)) && systemFallback)
            builder->AddMappings(systemFallback);
        builder->CreateFontFallback(&result);
    }

    if (systemFallback)
        systemFallback->Release();
    if (builder)
        builder->Release();
    if (factory2)
        factory2->Release();
    factory->Release();
    return result;
}

struct UiFontFallbackCache {
    IDWriteFontFallback* value = createUiFontFallback();

    ~UiFontFallbackCache() {
        if (value)
            value->Release();
    }
};

bool readLightThemeValue(const wchar_t* name, bool& light) {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    light = value != 0;
    return true;
}

bool detectAppDarkMode() {
    bool light = true;
    if (readLightThemeValue(L"AppsUseLightTheme", light))
        return !light;

    // 注册表不可用时，UISettings 是 Windows 对 Win32 应用推荐的主题来源。
    try {
        winrt::Windows::UI::ViewManagement::UISettings settings;
        auto foreground = settings.GetColorValue(
            winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
        int brightness = 5 * static_cast<int>(foreground.G) +
                         2 * static_cast<int>(foreground.R) +
                         static_cast<int>(foreground.B);
        return brightness > 8 * 128;
    } catch (...) {
        // 旧系统或 COM 初始化异常时保留注册表回退。
        DWORD value = 1; // 默认浅色
        DWORD size = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
            return value == 0;
        return false;
    }
}

bool detectSystemDarkMode() {
    bool light = true;
    if (readLightThemeValue(L"SystemUsesLightTheme", light))
        return !light;
    return detectAppDarkMode();
}

bool isDarkForMode(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::FollowSystem:
        return detectSystemDarkMode();
    case ThemeMode::FollowApp:
        return detectAppDarkMode();
    case ThemeMode::Dark:
        return true;
    case ThemeMode::Light:
    default:
        return false;
    }
}

// 半透明色叠在底色上的等效不透明色
D2D1_COLOR_F blendOver(D2D1_COLOR_F top, D2D1_COLOR_F base) {
    return D2D1::ColorF(top.r * top.a + base.r * (1.0f - top.a),
                        top.g * top.a + base.g * (1.0f - top.a),
                        top.b * top.a + base.b * (1.0f - top.a), 1.0f);
}

Palette makeLight() {
    Palette p{};
    p.text = toD2D(RGB(26, 26, 26));
    p.textSecondary = toD2D(RGB(96, 96, 96));
    p.disabled = toD2D(RGB(160, 160, 160));
    // 提高表面层级对比，让 Mica 背景、卡片和控件在桌面窗口中清晰分层。
    p.cardFill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f);
    p.cardStroke = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f);
    p.windowBg = toD2D(RGB(243, 243, 243));
    p.cardFillSolid = blendOver(p.cardFill, p.windowBg);
    p.controlFill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f);
    p.controlHover = D2D1::ColorF(0.976f, 0.976f, 0.976f, 0.88f);
    p.controlPressed = D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.92f);
    p.listHover = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f);
    p.listSelected = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f);
    p.menuFill = D2D1::ColorF(0.976f, 0.976f, 0.976f, 1.0f);
    p.menuStroke = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f);
    p.separator = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
    p.editText = toD2D(RGB(26, 26, 26));
    return p;
}

Palette makeDark() {
    Palette p{};
    p.text = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    p.textSecondary = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.6f);
    p.disabled = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.36f);
    p.cardFill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    p.cardStroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
    p.windowBg = toD2D(RGB(32, 32, 32));
    p.cardFillSolid = blendOver(p.cardFill, p.windowBg);
    p.controlFill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
    p.controlHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);
    p.controlPressed = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
    p.listHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
    p.listSelected = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
    p.menuFill = D2D1::ColorF(0.17f, 0.17f, 0.17f, 1.0f);
    p.menuStroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f);
    p.separator = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f);
    p.editText = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    return p;
}

void applyAccent(Palette& p) {
    COLORREF a = accentColor();
    p.accent = toD2D(a);
    // 悬停变亮、按下变暗
    auto scale = [](D2D1_COLOR_F c, float f) {
        return D2D1::ColorF(std::min(1.0f, c.r * f), std::min(1.0f, c.g * f),
                            std::min(1.0f, c.b * f), c.a);
    };
    auto mixWhite = [](D2D1_COLOR_F c, float t) {
        return D2D1::ColorF(c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t,
                            c.b + (1.0f - c.b) * t, c.a);
    };
    p.accentHover = mixWhite(p.accent, 0.1f);
    p.accentPressed = scale(p.accent, 0.85f);
    // 强调色亮度决定其上文字用白还是黑
    float lum = 0.299f * p.accent.r + 0.587f * p.accent.g + 0.114f * p.accent.b;
    p.textOnAccent = lum > 0.55f ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)
                                 : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

void setThemeModes(ThemeMode taskbarMode, ThemeMode windowMode) {
    gTaskbarThemeMode = taskbarMode;
    gWindowThemeMode = windowMode;
}

bool isDarkMode() {
    return isDarkMode(ThemeTarget::Window);
}

bool isDarkMode(ThemeTarget target) {
    return isDarkForMode(target == ThemeTarget::Taskbar ? gTaskbarThemeMode
                                                         : gWindowThemeMode);
}

COLORREF accentColor() {
    try {
        winrt::Windows::UI::ViewManagement::UISettings settings;
        auto c = settings.GetColorValue(
            winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        return RGB(c.R, c.G, c.B);
    } catch (...) {
        return RGB(0, 120, 212);
    }
}

D2D1_COLOR_F toD2D(COLORREF c, float alpha) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f,
                        alpha);
}

const Palette& palette() {
    return palette(ThemeTarget::Window);
}

const Palette& palette(ThemeTarget target) {
    static Palette light = [] {
        Palette p = makeLight();
        applyAccent(p);
        return p;
    }();
    static Palette dark = [] {
        Palette p = makeDark();
        applyAccent(p);
        return p;
    }();
    return isDarkMode(target) ? dark : light;
}

void applyRoundCorners(HWND hwnd, bool smallCorners) {
    int pref = smallCorners ? kDwmWcpRoundSmall : kDwmWcpRound;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

bool applyBackdrop(HWND hwnd, bool transientWindow) {
    int type = transientWindow ? kDwmsbtTransientWindow : kDwmsbtMainWindow;
    return SUCCEEDED(
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type)));
}

void clearBackdrop(HWND hwnd) {
    int type = kDwmsbtNone;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
}

void applyDarkCaption(HWND hwnd, bool dark) {
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
}

void applyBorderColor(HWND hwnd, COLORREF color) {
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
}

void suppressBorder(HWND hwnd) {
    // 0xFFFFFFFE = DWM 约定值，不绘制边框
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &none, sizeof(none));
}

bool styleDialogWindow(HWND hwnd, bool transientWindow) {
    applyRoundCorners(hwnd, false);
    const bool dark = isDarkMode(ThemeTarget::Window);
    // 深浅色都使用系统材质；先同步标题栏主题，
    // 这样 Mica/Acrylic 在用户固定深色且 Windows 当前为浅色时仍能保持一致。
    applyDarkCaption(hwnd, dark);
    bool applied = applyBackdrop(hwnd, transientWindow);

    if (!applied)
        clearBackdrop(hwnd);

    InvalidateRect(hwnd, nullptr, applied ? FALSE : TRUE);
    return applied;
}

bool restyleDialogWindow(HWND hwnd, bool oldBackdrop, bool transientWindow) {
    bool applied = styleDialogWindow(hwnd, transientWindow);
    // 从不透明自绘切换到 DWM 材质：GDI 画过的像素不会因不再绘制而消失，
    // 隐藏再显示让 DWM 重建透明表面，材质才能透出。
    if (applied && !oldBackdrop && IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
        ShowWindow(hwnd, SW_SHOW);
    }
    return applied;
}

COLORREF fallbackBgColor() {
    return isDarkMode(ThemeTarget::Window) ? RGB(32, 32, 32) : RGB(243, 243, 243);
}

void paintDialogBackground(HWND hwnd, HDC hdc, bool backdrop) {
    // 只要系统材质已应用，就不能用 GDI 纯色覆盖 DWM 背景；
    // 无材质时才绘制首帧回退底色。
    if (backdrop)
        return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH brush = CreateSolidBrush(fallbackBgColor());
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);
}

const wchar_t* uiFontFamily() {
    return L"Noto Sans SC";
}

void applyUiFontFallback(IDWriteTextFormat* format) {
    if (!format)
        return;

    IDWriteTextFormat1* format1 = nullptr;
    if (FAILED(format->QueryInterface(__uuidof(IDWriteTextFormat1),
                                      reinterpret_cast<void**>(&format1))) ||
        !format1)
        return;

    static UiFontFallbackCache cache;
    if (cache.value)
        format1->SetFontFallback(cache.value);
    format1->Release();
}

HFONT createUiFont(UINT dpi, float dipSize, int weight) {
    // DIP -> 像素：px = dip * dpi / 96（之前误按磅换算除以 72，字体偏大约 33%）
    int px = -MulDiv(static_cast<int>(std::lround(dipSize)), static_cast<int>(dpi), 96);
    HFONT font = CreateFontW(px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, uiFontFamily());
    return font;
}

} // namespace fluent
