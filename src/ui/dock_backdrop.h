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
    bool available() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
