#include "fluent_dialog_surface.h"

#include <dwmapi.h>

#include <algorithm>

#ifndef DWMWA_REDIRECTIONBITMAP_ALPHA
#define DWMWA_REDIRECTIONBITMAP_ALPHA 39
#endif

namespace fluent {

namespace {

void excludeVisibleChildren(HWND parent, HDC hdc) {
    if (!parent || !hdc)
        return;

    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child))
            continue;

        RECT childRect{};
        if (!GetWindowRect(child, &childRect))
            continue;

        POINT topLeft{childRect.left, childRect.top};
        POINT bottomRight{childRect.right, childRect.bottom};
        MapWindowPoints(nullptr, parent, &topLeft, 1);
        MapWindowPoints(nullptr, parent, &bottomRight, 1);
        ExcludeClipRect(hdc, topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
    }
}

} // namespace

FluentDialogSurface::Painter::Painter(ID2D1DCRenderTarget* target, IDWriteFactory* dwrite)
    : target_(target), dwrite_(dwrite) {}

FluentDialogSurface::Painter::~Painter() {
    for (auto* layout : layouts_) {
        if (layout)
            layout->Release();
    }
    for (auto* format : formats_) {
        if (format)
            format->Release();
    }
    for (auto* brush : brushes_) {
        if (brush)
            brush->Release();
    }
    if (brush_)
        brush_->Release();
}

ID2D1SolidColorBrush* FluentDialogSurface::Painter::brush(D2D1_COLOR_F color) {
    if (!brush_ && target_)
        target_->CreateSolidColorBrush(color, &brush_);
    if (brush_)
        brush_->SetColor(color);
    return brush_;
}

ID2D1SolidColorBrush* FluentDialogSurface::Painter::createBrush(D2D1_COLOR_F color) {
    if (!target_)
        return nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(target_->CreateSolidColorBrush(color, &brush)) && brush)
        brushes_.push_back(brush);
    return brush;
}

IDWriteTextFormat* FluentDialogSurface::Painter::textFormat(float dipSize, int weight,
                                                            bool center, bool noWrap,
                                                            const wchar_t* family) {
    if (!dwrite_)
        return nullptr;

    IDWriteTextFormat* format = nullptr;
    const wchar_t* familyName = family ? family : uiFontFamily();
    HRESULT hr = dwrite_->CreateTextFormat(
        familyName, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, dipSize, L"zh-cn", &format);
    if (FAILED(hr) || !format)
        return nullptr;

    applyUiFontFallback(format);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (center)
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (noWrap)
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats_.push_back(format);
    return format;
}

IDWriteTextLayout* FluentDialogSurface::Painter::textLayout(const std::wstring& text,
                                                             IDWriteTextFormat* format,
                                                             float width, float height) {
    if (!dwrite_ || !format)
        return nullptr;
    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                           std::max(1.0f, width), std::max(1.0f, height),
                                           &layout);
    if (FAILED(hr) || !layout)
        return nullptr;
    layouts_.push_back(layout);
    return layout;
}

float FluentDialogSurface::Painter::measureTextWidth(const std::wstring& text,
                                                     IDWriteTextFormat* format) {
    auto* layout = textLayout(text, format, 1000.0f, 100.0f);
    if (!layout)
        return 0.0f;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.width;
}

void FluentDialogSurface::Painter::fillRoundRect(D2D1_COLOR_F color, const D2D1_RECT_F& rect,
                                                  float radius) {
    if (auto* br = brush(color))
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), br);
}

void FluentDialogSurface::Painter::strokeRoundRect(D2D1_COLOR_F color,
                                                   const D2D1_RECT_F& rect, float width,
                                                   float radius) {
    if (auto* br = brush(color))
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), br, width);
}

void FluentDialogSurface::Painter::drawText(const std::wstring& text, IDWriteTextFormat* format,
                                            const D2D1_RECT_F& rect, D2D1_COLOR_F color,
                                            D2D1_DRAW_TEXT_OPTIONS options) {
    if (format) {
        if (auto* br = brush(color)) {
        target_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, br,
                           options);
        }
    }
}

void FluentDialogSurface::Painter::drawTextLayout(IDWriteTextLayout* layout,
                                                  D2D1_POINT_2F origin, D2D1_COLOR_F color,
                                                  D2D1_DRAW_TEXT_OPTIONS options) {
    if (layout) {
        if (auto* br = brush(color))
            target_->DrawTextLayout(origin, layout, br, options);
    }
}

void FluentDialogSurface::Painter::drawTrimmedText(const std::wstring& text,
                                                   IDWriteTextFormat* format,
                                                   const D2D1_RECT_F& rect,
                                                   D2D1_COLOR_F color) {
    auto* layout = textLayout(text, format, rect.right - rect.left, rect.bottom - rect.top);
    if (!layout)
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trimming, nullptr);
    drawTextLayout(layout, D2D1::Point2F(rect.left, rect.top), color);
}

FluentDialogSurface::~FluentDialogSurface() {
    discard();
    hwnd_ = nullptr;
}

bool FluentDialogSurface::initialize(HWND hwnd, bool backdrop) {
    hwnd_ = hwnd;
    dpi_ = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    if (dpi_ == 0)
        dpi_ = 96;
    alphaRedirection_ = false;
    if (hwnd_) {
        BOOL enabled = backdrop ? TRUE : FALSE;
        alphaRedirection_ = backdrop && SUCCEEDED(DwmSetWindowAttribute(
            hwnd_, DWMWA_REDIRECTIONBITMAP_ALPHA, &enabled, sizeof(enabled)));
    }
    return hwnd_ != nullptr;
}

void FluentDialogSurface::setBackdrop(bool backdrop) {
    if (!hwnd_)
        return;

    BOOL enabled = backdrop ? TRUE : FALSE;
    alphaRedirection_ = backdrop && SUCCEEDED(DwmSetWindowAttribute(
        hwnd_, DWMWA_REDIRECTIONBITMAP_ALPHA, &enabled, sizeof(enabled)));
}

bool FluentDialogSurface::paint(HDC hdc, bool backdrop, const PaintCallback& callback) {
    if (!hwnd_ || !hdc)
        return false;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return false;

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0)
        dpi_ = 96;
    if (!renderer_.initialize())
        return false;
    renderer_.setDpi(dpi_);
    auto* target = renderer_.renderTarget();
    if (!target)
        return false;

    // 对话框表面使用当前 WM_PAINT 的 DC，保留 DWM 材质和 Fluent 颜色的原有合成路径。
    // 父窗口本身带 WS_CLIPCHILDREN，且额外排除可见子窗口，原生控件由系统单独绘制。
    RECT renderRect{0, 0, width, height};
    excludeVisibleChildren(hwnd_, hdc);
    if (FAILED(target->BindDC(hdc, &renderRect)))
        return false;

    // 根背景由 D2D 在当前 WM_PAINT 内统一绘制；支持重定向 Alpha 时让 Mica 透出。
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    if (backdrop && alphaRedirection_)
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    else
        target->Clear(fluent::palette().windowBg);
    Painter painter(target, renderer_.dwrite());
    if (callback)
        callback(painter, static_cast<float>(width) / dipScale(),
                 static_cast<float>(height) / dipScale());

    HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderer_.discard();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    if (FAILED(hr))
        return false;

    return true;
}

void FluentDialogSurface::invalidate(const RECT* rect) const {
    if (hwnd_)
        InvalidateRect(hwnd_, rect, FALSE);
}

void FluentDialogSurface::discard() {
    renderer_.releaseAll();
}

} // namespace fluent
