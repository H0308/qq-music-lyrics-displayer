#include "ui/app_icon.h"

#include "resource.h"
#include "ui/fluent_theme.h"

#include <array>
#include <mutex>

namespace {

HICON loadIconFromResource(int resourceId) {
    return static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
}

struct IconCache {
    std::array<HICON, 2> icons{};
};

IconCache g_iconCache;
std::once_flag g_iconCacheOnce;

void initializeIcons() {
    g_iconCache.icons[0] = loadIconFromResource(IDI_APPICON_DARK);
    g_iconCache.icons[1] = loadIconFromResource(IDI_APPICON_LIGHT);
}

HICON iconForTheme(bool dark) {
    std::call_once(g_iconCacheOnce, initializeIcons);
    return g_iconCache.icons[dark ? 0 : 1];
}

} // namespace

namespace app_icon {

HICON taskbarIcon() {
    return iconForTheme(fluent::isDarkMode(fluent::ThemeTarget::Taskbar));
}

HICON windowIcon() {
    // 普通顶层窗口的任务栏按钮也应与“任务栏歌词主题”保持一致。
    return taskbarIcon();
}

namespace {

void applyIcon(HWND hwnd, HICON icon) {
    if (!hwnd || !IsWindow(hwnd))
        return;
    if (!icon)
        return;

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

} // namespace

void applyWindowIcon(HWND hwnd) {
    applyIcon(hwnd, windowIcon());
}

void applyTaskbarIcon(HWND hwnd) {
    applyIcon(hwnd, taskbarIcon());
}

} // namespace app_icon
