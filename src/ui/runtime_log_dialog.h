#pragma once

#include "logging/runtime_logger.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// 运行日志窗口：展示低频运行状态，并提供日志目录与清理周期设置。
class RuntimeLogDialog {
public:
    using DirectoryCallback = std::function<void(const std::wstring&)>;
    using RetentionCallback = std::function<void(int)>;

    RuntimeLogDialog();
    ~RuntimeLogDialog();

    RuntimeLogDialog(const RuntimeLogDialog&) = delete;
    RuntimeLogDialog& operator=(const RuntimeLogDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, runtime_log::RuntimeLogger* logger,
                DirectoryCallback onDirectoryChanged, RetentionCallback onRetentionChanged);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
