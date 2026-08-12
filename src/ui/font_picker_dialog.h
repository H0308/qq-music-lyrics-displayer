#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// Win11 风格字体选择对话框（替代系统 ChooseFont）：
// 字体族筛选/列表（各行以该字体渲染预览）、字号、示例预览。
class FontPickerDialog {
public:
    using ApplyCallback = std::function<void(const std::wstring& family, float sizePt)>;

    FontPickerDialog();
    ~FontPickerDialog();

    FontPickerDialog(const FontPickerDialog&) = delete;
    FontPickerDialog& operator=(const FontPickerDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, const std::wstring& family, float sizePt);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

    void setApplyCallback(ApplyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
