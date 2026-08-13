#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// Win11 风格自绘弹出菜单：圆角、阴影、悬停高亮、勾选标记、一级子菜单。
namespace fluent {

struct FluentMenuItem {
    int id = 0;
    std::wstring text;
    bool checked = false;
    bool enabled = true;
    bool separator = false;
    std::vector<FluentMenuItem> submenu;
};

class FluentMenu {
public:
    using Callback = std::function<void(int)>;

    // 在屏幕坐标 pt 处弹出菜单；选中有效项时回调其 id，取消不回调。
    // 菜单为非阻塞式，由主消息循环驱动。
    static void show(HWND owner, POINT screenPt, std::vector<FluentMenuItem> items, Callback cb);

    // 关闭当前打开的菜单（如果有）
    static void dismiss();
};

} // namespace fluent
