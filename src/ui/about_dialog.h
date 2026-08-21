#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

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
                std::function<void(const std::wstring&)> onStartupUpdateAvailable = {});
    void show(bool downloadUpdate = false);
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
