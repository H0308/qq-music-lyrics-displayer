#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// 单行文本编辑小窗。默认文案对应快速启动应用名称编辑（留空恢复 EXE 默认名称），
// 调用方可通过 title/subtitle/placeholder 复用于其他文本设置（如自定义欢迎语）。
class IdleAppNameDialog {
public:
    using ApplyCallback = std::function<void(const std::wstring&)>;

    IdleAppNameDialog();
    ~IdleAppNameDialog();

    IdleAppNameDialog(const IdleAppNameDialog&) = delete;
    IdleAppNameDialog& operator=(const IdleAppNameDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, const std::wstring& initial,
                const wchar_t* title = L"修改应用名称",
                const wchar_t* subtitle = L"留空后将恢复 EXE 的默认名称",
                const wchar_t* placeholder = L"输入应用名称",
                int maxLength = 0);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

    void setApplyCallback(ApplyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
