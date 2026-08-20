#pragma once

#include "ui/fluent_theme.h"
#include "ui/lyric_renderer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace fluent {

// 普通顶层 Fluent 窗口共用的单表面绘制宿主。
// 它只负责把一次 WM_PAINT 绑定到 D2D、处理 DIP 换算和资源生命周期；
// 页面布局、命中测试和业务命令仍由各自的窗口实现。
class FluentDialogSurface {
public:
    class Painter {
    public:
        Painter(ID2D1DCRenderTarget* target, IDWriteFactory* dwrite);
        ~Painter();

        Painter(const Painter&) = delete;
        Painter& operator=(const Painter&) = delete;

        ID2D1DCRenderTarget* target() const { return target_; }
        IDWriteFactory* dwrite() const { return dwrite_; }

        ID2D1SolidColorBrush* brush(D2D1_COLOR_F color);
        // 为 DirectWrite 的 DrawingEffect 等需要独立颜色的场景创建额外画刷。
        // 画刷由当前 Painter 在一帧结束时统一释放。
        ID2D1SolidColorBrush* createBrush(D2D1_COLOR_F color);
        IDWriteTextFormat* textFormat(float dipSize, int weight = 400,
                                      bool center = false, bool noWrap = false,
                                      const wchar_t* family = nullptr);
        IDWriteTextLayout* textLayout(const std::wstring& text, IDWriteTextFormat* format,
                                      float width, float height);
        float measureTextWidth(const std::wstring& text, IDWriteTextFormat* format);

        void fillRoundRect(D2D1_COLOR_F color, const D2D1_RECT_F& rect,
                           float radius = metrics::controlRadius);
        void strokeRoundRect(D2D1_COLOR_F color, const D2D1_RECT_F& rect, float width = 1.0f,
                             float radius = metrics::controlRadius);
        void drawText(const std::wstring& text, IDWriteTextFormat* format,
                      const D2D1_RECT_F& rect, D2D1_COLOR_F color,
                      D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        void drawTextLayout(IDWriteTextLayout* layout, D2D1_POINT_2F origin,
                            D2D1_COLOR_F color,
                            D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        void drawTrimmedText(const std::wstring& text, IDWriteTextFormat* format,
                             const D2D1_RECT_F& rect, D2D1_COLOR_F color);

    private:
        ID2D1DCRenderTarget* target_ = nullptr;
        IDWriteFactory* dwrite_ = nullptr;
        ID2D1SolidColorBrush* brush_ = nullptr;
        std::vector<ID2D1SolidColorBrush*> brushes_;
        std::vector<IDWriteTextFormat*> formats_;
        std::vector<IDWriteTextLayout*> layouts_;
    };

    using PaintCallback = std::function<void(Painter&, float, float)>;

    FluentDialogSurface() = default;
    ~FluentDialogSurface();

    FluentDialogSurface(const FluentDialogSurface&) = delete;
    FluentDialogSurface& operator=(const FluentDialogSurface&) = delete;

    bool initialize(HWND hwnd);
    // 在一次 WM_PAINT 内完成背景准备、D2D 绘制和当前 DC 提交。
    bool paint(HDC hdc, bool backdrop, const PaintCallback& callback);
    void invalidate(const RECT* rect = nullptr) const;
    void discard();

    HWND hwnd() const { return hwnd_; }
    UINT dpi() const { return dpi_; }
    float dipScale() const { return fluent::dipScale(dpi_); }

private:
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool alphaRedirection_ = false;
    LyricRenderer renderer_;
};

} // namespace fluent
