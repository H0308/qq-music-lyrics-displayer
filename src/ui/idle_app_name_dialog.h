#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// 快速启动应用名称编辑窗口。名称留空时由调用方恢复为 EXE 默认名称。
class IdleAppNameDialog {
public:
    using ApplyCallback = std::function<void(const std::wstring&)>;

    IdleAppNameDialog();
    ~IdleAppNameDialog();

    IdleAppNameDialog(const IdleAppNameDialog&) = delete;
    IdleAppNameDialog& operator=(const IdleAppNameDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, const std::wstring& initial);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

    void setApplyCallback(ApplyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
