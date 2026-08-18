#pragma once

#include <windows.h>

#include <functional>
#include <memory>

// 软件关于窗口：显示应用信息，并从 GitHub Releases 检查正式版本更新。
class AboutDialog {
public:
    AboutDialog();
    ~AboutDialog();

    AboutDialog(const AboutDialog&) = delete;
    AboutDialog& operator=(const AboutDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, bool autoCheckOnStartup = true,
                std::function<void(bool)> onAutoCheckChanged = {});
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
