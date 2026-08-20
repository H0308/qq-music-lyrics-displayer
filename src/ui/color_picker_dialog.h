#pragma once

#include <windows.h>

#include <functional>
#include <memory>

// 取色器窗口销毁后通知其父级，由父级释放 ColorPickerDialog 对象。
constexpr UINT kMsgColorPickerClosed = WM_APP + 220;

// Win11 风格颜色选择对话框（替代系统 ChooseColor）：
// SV 取色板 + 色相滑条 + HEX 输入 + 新旧颜色对比。
class ColorPickerDialog {
public:
    using ApplyCallback = std::function<void(COLORREF)>;

    ColorPickerDialog();
    ~ColorPickerDialog();

    ColorPickerDialog(const ColorPickerDialog&) = delete;
    ColorPickerDialog& operator=(const ColorPickerDialog&) = delete;

    // cascadeIndex：同时打开多个时按序号级联偏移，避免完全重叠
    bool create(HINSTANCE inst, HWND parent, COLORREF initial, const wchar_t* title,
                int cascadeIndex = 0);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

    void setApplyCallback(ApplyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
