#include "fluent_theme.h"

#include <dwmapi.h>
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
constexpr int kDwmsbtMainWindow = 2;      // DWMSBT_MAINWINDOW (Mica)
constexpr int kDwmsbtTransientWindow = 3; // DWMSBT_TRANSIENTWINDOW (Acrylic)

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

bool isDarkMode() {
    DWORD value = 1; // 默认浅色
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
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
    return isDarkMode() ? dark : light;
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
    applyDarkCaption(hwnd, isDarkMode());
    bool applied = applyBackdrop(hwnd, transientWindow);
    if (applied)
        InvalidateRect(hwnd, nullptr, FALSE);
    return applied;
}

COLORREF fallbackBgColor() {
    return isDarkMode() ? RGB(32, 32, 32) : RGB(243, 243, 243);
}

const wchar_t* uiFontFamily() {
    return L"Segoe UI Variable Text";
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
