#pragma once

#include <windows.h>

#include <memory>

// Dock 和沉浸模式共用的背景模糊层。该层由 Windows.UI.Composition
// 采样窗口后方的桌面，并放在歌词窗口的 DirectComposition 内容下方。
class DockBackdrop {
public:
    DockBackdrop();
    ~DockBackdrop();

    DockBackdrop(const DockBackdrop&) = delete;
    DockBackdrop& operator=(const DockBackdrop&) = delete;

    bool initialize(HWND hwnd);
    void reset() noexcept;
    void setAdjustmentMode(int mode) noexcept; // 0 模糊程度 1 不透明度
    void setSolidColor(COLORREF color) noexcept;
    void setBlurPercent(int percent) noexcept;
    void setVisible(bool visible) noexcept;
    // 整层显隐过渡（沉浸模式让出任务栏时使用），与 blur/不透明度设置相互独立。
    // durationMs 为 0 时直接落到 to。
    void animatePresence(float from, float to, unsigned durationMs) noexcept;
    bool available() const noexcept;
    // 所有本线程背景实例释放后调用；完成 DispatcherQueue 关闭再退出 WinRT。
    static void shutdownThreadResources();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
