#pragma once

#include "ui/fluent_theme.h"
#include "ui/lyric_renderer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <functional>
#include <cstdint>
#include <chrono>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <wrl/client.h>

namespace fluent {

struct DialogContentLayout {
    float offsetX;
    float width;
};

// 内容宽度最多 1000 DIP（不含页面内边距），宽屏下整体居中。
DialogContentLayout centeredDialogLayout(float clientWidth);
void offsetDialogRects(float offsetX, std::initializer_list<D2D1_RECT_F*> rects);

// 为普通顶层窗口设置按客户区 DIP 计算的最小外框尺寸。
void setDialogMinimumTrackSize(HWND hwnd, MINMAXINFO* info, DWORD style, DWORD exStyle,
                               float minClientWidthDip, float minClientHeightDip);

// 应用 WM_DPICHANGED 的建议外框，同时按新 DPI 钳制最小客户区尺寸。
// SetWindowPos 不会自动执行 WM_GETMINMAXINFO，因此不能直接使用 suggestedRect。
void applyDialogDpiChange(HWND hwnd, WPARAM dpiMessage, const RECT* suggestedRect, DWORD style,
                          DWORD exStyle, float minClientWidthDip, float minClientHeightDip);

// 对话框创建后按实际所在显示器 DPI 校正最小外框尺寸。
// 这会覆盖创建时 GetDpiForSystem() 取到的过期尺寸，但不会缩小用户指定的更大尺寸。
void ensureDialogMinimumSize(HWND hwnd, DWORD style, DWORD exStyle, float minClientWidthDip,
                             float minClientHeightDip);

// 在 WM_SIZING 中保持客户区不低于指定的宽高比，避免可调整窗口被拉成长条。
void enforceDialogMinimumAspectRatio(HWND hwnd, WPARAM sizingEdge, RECT* proposedRect,
                                     float minClientAspectRatio);

// 普通顶层 Fluent 窗口共用的单表面绘制宿主。
// 它只负责把一次 WM_PAINT 绑定到 D2D、处理 DIP 换算和资源生命周期；
// 页面布局、命中测试和业务命令仍由各自的窗口实现。
class FluentDialogSurface {
public:
    struct TextCache {
        using FormatKey = std::tuple<float, int, bool, bool, std::wstring>;
        using LayoutKey = std::tuple<std::wstring, std::uintptr_t, float, float>;
        std::map<FormatKey, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats;
        std::map<LayoutKey, Microsoft::WRL::ComPtr<IDWriteTextLayout>> layouts;

        void clear() {
            layouts.clear();
            formats.clear();
        }
    };

    class Painter {
    public:
        Painter(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                TextCache* textCache = nullptr);
        ~Painter();

        Painter(const Painter&) = delete;
        Painter& operator=(const Painter&) = delete;

        ID2D1RenderTarget* target() const { return target_; }
        IDWriteFactory* dwrite() const { return dwrite_; }
        const Palette& colors() const { return colors_; }

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
        ID2D1RenderTarget* target_ = nullptr;
        IDWriteFactory* dwrite_ = nullptr;
        const Palette colors_;
        TextCache* textCache_ = nullptr;
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

    bool initialize(HWND hwnd, bool backdrop, bool cacheText = false);
    void setBackdrop(bool backdrop);
    void eraseBackground(HDC hdc, bool backdrop);
    // 在一次 WM_PAINT 内完成背景准备、D2D 绘制和当前 DC 提交。
    bool paint(HDC hdc, bool backdrop, const PaintCallback& callback);
    void invalidate(const RECT* rect = nullptr) const;
    void discard();

    HWND hwnd() const { return hwnd_; }
    // 布局发生在 WM_PAINT 之前，必须查询窗口当前 DPI，不能沿用上一帧。
    UINT dpi() const;
    float dipScale() const { return fluent::dipScale(dpi()); }

private:
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96; // 已绘制帧的 DPI，用于判断绘制缓存是否需要失效。
    bool hasPainted_ = false;
    bool cacheText_ = false;
    TextCache textCache_;
    int textCacheWidth_ = 0;
    int textCacheHeight_ = 0;
    LyricRenderer renderer_;
};

class CompositedScrollView {
public:
    using Painter = FluentDialogSurface::Painter;
    using Draw = std::function<void(Painter&, const D2D1_RECT_F&, float)>;
    using Changed = std::function<void(float, bool)>;

    CompositedScrollView() = default;
    CompositedScrollView(const CompositedScrollView&) = delete;
    CompositedScrollView& operator=(const CompositedScrollView&) = delete;
    ~CompositedScrollView();
    bool create(HWND parent, UINT_PTR timer, Changed changed,
                DCompRenderer* drawingDeviceSource = nullptr);
    void layout(const D2D1_RECT_F& viewport, float contentHeight, UINT dpi,
                bool showWindow = true);
    void paint(const Draw& draw, bool redraw = true, bool forceRedraw = false);
    void scrollBy(float delta, bool smooth = true);
    void scrollTo(float targetPosition, bool smooth = true, float frequency = 28.0f,
                  bool notifyParent = true);
    void setOffset(float offset);
    void stop();
    void setVisible(bool visible);
    // 只更新合成层透明度，调用方可与父层变更一起提交。
    void setContentVisible(bool visible);
    bool tick(WPARAM timer);
    float offset() const;
    float targetOffset() const { return target_; }
    bool animating() const { return animating_; }
    void reset();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp);
    float velocity() const;
    void applyAnimation();
    bool needsCache(float offset) const;

    HWND parent_ = nullptr;
    HWND window_ = nullptr;
    UINT_PTR timer_ = 0;
    UINT dpi_ = 96;
    D2D1_RECT_F viewport_{};
    float height_ = 0.0f;
    float maximum_ = 0.0f;
    float from_ = 0.0f;
    float target_ = 0.0f;
    float speed_ = 0.0f;
    float frequency_ = 28.0f;
    float cacheTop_ = 0.0f;
    float cacheHeight_ = 0.0f;
    bool ready_ = false;
    bool contentVisible_ = true;
    bool animating_ = false;
    std::chrono::steady_clock::time_point started_{};
    Changed changed_;
    DCompRenderer renderer_;
};

} // namespace fluent
