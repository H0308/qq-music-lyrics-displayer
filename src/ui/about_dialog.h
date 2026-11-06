#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// 启动检查只接收网络结果，不创建关于窗口或绘图资源。
class StartupUpdateCheck {
public:
    StartupUpdateCheck(bool useGiteeUpdateSource,
                       std::function<void(const std::wstring&)> onAvailable);
    ~StartupUpdateCheck();
    StartupUpdateCheck(const StartupUpdateCheck&) = delete;
    StartupUpdateCheck& operator=(const StartupUpdateCheck&) = delete;

private:
    friend class AboutDialog;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 软件关于窗口：显示应用信息，并从用户选择的 GitHub/Gitee Release 检查正式版本更新。
class AboutDialog {
public:
    AboutDialog();
    ~AboutDialog();

    AboutDialog(const AboutDialog&) = delete;
    AboutDialog& operator=(const AboutDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, bool autoCheckOnStartup = true,
                bool useGiteeUpdateSource = false,
                std::function<void(bool)> onAutoCheckChanged = {},
                std::function<void(bool)> onUpdateSourceChanged = {},
                std::function<bool(const std::wstring&)> onInstallUpdate = {},
                StartupUpdateCheck* startupCheck = nullptr);
    void show(bool downloadUpdate = false);
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
