#include "fluent_dialog_surface.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fluent {

DialogContentLayout centeredDialogLayout(float clientWidth) {
    const float width = std::min(clientWidth, 1000.0f + metrics::pagePadding * 2.0f);
    return {(clientWidth - width) * 0.5f, width};
}

void offsetDialogRects(float offsetX, std::initializer_list<D2D1_RECT_F*> rects) {
    for (auto* rect : rects) {
        if (rect->right > rect->left && rect->bottom > rect->top) {
            rect->left += offsetX;
            rect->right += offsetX;
        }
    }
}

namespace {

void excludeVisibleChildren(HWND parent, HDC hdc) {
    if (!parent || !hdc)
        return;

    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child))
            continue;

        // 合成子窗口只提供视觉层，透明间隙仍依赖父窗口的根背景。
        // 排除它的整个矩形会使底面无法清理，缩放后透出旧画面。
        if (GetWindowLongPtrW(child, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP)
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

void setDialogMinimumTrackSize(HWND hwnd, MINMAXINFO* info, DWORD style, DWORD exStyle,
                               float minClientWidthDip, float minClientHeightDip) {
    if (!hwnd || !info || minClientWidthDip <= 0.0f || minClientHeightDip <= 0.0f)
        return;

    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi)
        dpi = GetDpiForSystem();
    const float scale = dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(minClientWidthDip * scale)),
            static_cast<LONG>(std::lround(minClientHeightDip * scale))};
    if (!AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi))
        return;

    info->ptMinTrackSize.x = std::max(info->ptMinTrackSize.x, rc.right - rc.left);
    info->ptMinTrackSize.y = std::max(info->ptMinTrackSize.y, rc.bottom - rc.top);
}

void applyDialogDpiChange(HWND hwnd, WPARAM dpiMessage, const RECT* suggestedRect, DWORD style,
                          DWORD exStyle, float minClientWidthDip, float minClientHeightDip) {
    if (!hwnd || !suggestedRect)
        return;

    UINT dpi = LOWORD(dpiMessage);
    if (!dpi)
        dpi = GetDpiForWindow(hwnd);
    if (!dpi)
        dpi = GetDpiForSystem();

    LONG minWidth = 0;
    LONG minHeight = 0;
    if (minClientWidthDip > 0.0f && minClientHeightDip > 0.0f) {
        const float scale = dipScale(dpi);
        RECT minRect{0, 0, static_cast<LONG>(std::lround(minClientWidthDip * scale)),
                     static_cast<LONG>(std::lround(minClientHeightDip * scale))};
        if (AdjustWindowRectExForDpi(&minRect, style, FALSE, exStyle, dpi)) {
            minWidth = std::max(1L, minRect.right - minRect.left);
            minHeight = std::max(1L, minRect.bottom - minRect.top);
        }
    }

    const LONG suggestedWidth = std::max(1L, suggestedRect->right - suggestedRect->left);
    const LONG suggestedHeight = std::max(1L, suggestedRect->bottom - suggestedRect->top);
    const LONG width = std::max(suggestedWidth, minWidth);
    const LONG height = std::max(suggestedHeight, minHeight);
    SetWindowPos(hwnd, nullptr, suggestedRect->left, suggestedRect->top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ensureDialogMinimumSize(HWND hwnd, DWORD style, DWORD exStyle, float minClientWidthDip,
                             float minClientHeightDip) {
    if (!hwnd || minClientWidthDip <= 0.0f || minClientHeightDip <= 0.0f)
        return;

    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi)
        dpi = GetDpiForSystem();
    const float scale = dipScale(dpi);
    RECT minRect{0, 0, static_cast<LONG>(std::lround(minClientWidthDip * scale)),
                 static_cast<LONG>(std::lround(minClientHeightDip * scale))};
    if (!AdjustWindowRectExForDpi(&minRect, style, FALSE, exStyle, dpi))
        return;

    RECT current{};
    if (!GetWindowRect(hwnd, &current))
        return;

    const LONG currentWidth = current.right - current.left;
    const LONG currentHeight = current.bottom - current.top;
    const LONG minWidth = minRect.right - minRect.left;
    const LONG minHeight = minRect.bottom - minRect.top;
    const LONG width = std::max(currentWidth, minWidth);
    const LONG height = std::max(currentHeight, minHeight);
    if (width == currentWidth && height == currentHeight)
        return;

    SetWindowPos(hwnd, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void enforceDialogMinimumAspectRatio(HWND hwnd, WPARAM sizingEdge, RECT* proposedRect,
                                     float minClientAspectRatio) {
    if (!hwnd || !proposedRect || minClientAspectRatio <= 0.0f)
        return;

    RECT client{};
    RECT window{};
    if (!GetClientRect(hwnd, &client) || !GetWindowRect(hwnd, &window))
        return;

    const int frameWidth = (window.right - window.left) - (client.right - client.left);
    const int frameHeight = (window.bottom - window.top) - (client.bottom - client.top);
    const int outerWidth = std::max(1L, proposedRect->right - proposedRect->left);
    const int outerHeight = std::max(1L, proposedRect->bottom - proposedRect->top);
    const int clientWidth = std::max(1, outerWidth - frameWidth);
    const int clientHeight = std::max(1, outerHeight - frameHeight);

    if (static_cast<double>(clientWidth) >=
        static_cast<double>(minClientAspectRatio) * clientHeight)
        return;

    const bool horizontalEdge = sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_RIGHT ||
                                sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_TOPRIGHT ||
                                sizingEdge == WMSZ_BOTTOMLEFT || sizingEdge == WMSZ_BOTTOMRIGHT;
    const bool topEdge = sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_TOPLEFT ||
                         sizingEdge == WMSZ_TOPRIGHT;
    const bool bottomEdge = sizingEdge == WMSZ_BOTTOM || sizingEdge == WMSZ_BOTTOMLEFT ||
                            sizingEdge == WMSZ_BOTTOMRIGHT;
    const bool verticalEdge = sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_BOTTOM;

    if (horizontalEdge) {
        // 拖动左右边或角点时保留用户给出的宽度，用高度收敛到最小比例。
        const int newClientHeight = std::max(
            1, static_cast<int>(std::lround(clientWidth / minClientAspectRatio)));
        const int newOuterHeight = newClientHeight + frameHeight;
        if (topEdge)
            proposedRect->top = proposedRect->bottom - newOuterHeight;
        else if (bottomEdge)
            proposedRect->bottom = proposedRect->top + newOuterHeight;
        else {
            const int center = (proposedRect->top + proposedRect->bottom) / 2;
            proposedRect->top = center - newOuterHeight / 2;
            proposedRect->bottom = proposedRect->top + newOuterHeight;
        }
    } else if (verticalEdge) {
        // 只拖动上下边时保持窗口中心不漂移，按高度补足所需宽度。
        const int newClientWidth = std::max(
            1, static_cast<int>(std::lround(clientHeight * minClientAspectRatio)));
        const int newOuterWidth = newClientWidth + frameWidth;
        const int center = (proposedRect->left + proposedRect->right) / 2;
        proposedRect->left = center - newOuterWidth / 2;
        proposedRect->right = proposedRect->left + newOuterWidth;
    }
}

FluentDialogSurface::Painter::Painter(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                                       TextCache* textCache)
    : target_(target), dwrite_(dwrite), textCache_(textCache) {}

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
    const TextCache::FormatKey key{dipSize, weight, center, noWrap, familyName};
    if (textCache_) {
        const auto found = textCache_->formats.find(key);
        if (found != textCache_->formats.end())
            return found->second.Get();
    }
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
    if (textCache_)
        textCache_->formats[key].Attach(format);
    else
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
    if (textCache_ && dwrite_ && format && !text.empty()) {
        const float width = std::max(1.0f, std::round((rect.right - rect.left) * 64.0f) / 64.0f);
        const float height = std::max(1.0f, std::round((rect.bottom - rect.top) * 64.0f) / 64.0f);
        const TextCache::LayoutKey key{text, reinterpret_cast<std::uintptr_t>(format),
                                       width, height};
        auto found = textCache_->layouts.find(key);
        if (found == textCache_->layouts.end()) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                format, width, height, layout.GetAddressOf())))
                return;
            found = textCache_->layouts.emplace(key, std::move(layout)).first;
        }
        drawTextLayout(found->second.Get(), D2D1::Point2F(rect.left, rect.top), color, options);
        return;
    }
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
    IDWriteInlineObject* trimmingSign = nullptr;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(format, &trimmingSign)) &&
        trimmingSign) {
        layout->SetTrimming(&trimming, trimmingSign);
    } else {
        layout->SetTrimming(&trimming, nullptr);
    }
    const auto options = static_cast<D2D1_DRAW_TEXT_OPTIONS>(
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
    drawTextLayout(layout, D2D1::Point2F(rect.left, rect.top), color, options);
    if (trimmingSign)
        trimmingSign->Release();
}

FluentDialogSurface::~FluentDialogSurface() {
    discard();
    hwnd_ = nullptr;
}

bool FluentDialogSurface::initialize(HWND hwnd, bool backdrop, bool cacheText) {
    textCache_.clear();
    cacheText_ = cacheText;
    hwnd_ = hwnd;
    dpi_ = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    if (dpi_ == 0)
        dpi_ = 96;
    (void)backdrop;
    hasPainted_ = false;
    return hwnd_ != nullptr;
}

void FluentDialogSurface::setBackdrop(bool backdrop) {
    textCache_.clear();
    (void)backdrop;
    hasPainted_ = false;
}

void FluentDialogSurface::eraseBackground(HDC hdc, bool backdrop) {
    if (!hasPainted_)
        paintDialogBackground(hwnd_, hdc, backdrop);
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

    const UINT previousDpi = dpi_;
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0)
        dpi_ = 96;
    if (dpi_ != previousDpi || width != textCacheWidth_ || height != textCacheHeight_) {
        textCache_.clear();
        textCacheWidth_ = width;
        textCacheHeight_ = height;
    }
    if (!renderer_.initialize())
        return false;
    renderer_.setDpi(dpi_);
    auto* target = renderer_.renderTarget();
    if (!target)
        return false;

    // 对话框表面使用当前 WM_PAINT 的 DC，材质仅位于顶层窗口根背景。
    // 仅排除有独立绘制表面的原生子控件，合成滚动层下方仍需绘制根背景。
    RECT renderRect{0, 0, width, height};
    excludeVisibleChildren(hwnd_, hdc);
    if (FAILED(target->BindDC(hdc, &renderRect)))
        return false;

    // Mica 窗口留出透明根背景；卡片和原生输入框仍使用各自的不透明表面。
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    RECT dirty{};
    GetClipBox(hdc, &dirty);
    const float scale = dipScale();
    target->PushAxisAlignedClip(
        D2D1::RectF(dirty.left / scale, dirty.top / scale,
                    dirty.right / scale, dirty.bottom / scale),
        D2D1_ANTIALIAS_MODE_ALIASED);
    target->Clear(backdrop ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)
                           : fluent::palette().windowBg);
    Painter painter(target, renderer_.dwrite(), cacheText_ ? &textCache_ : nullptr);
    if (callback)
        callback(painter, static_cast<float>(width) / dipScale(),
                 static_cast<float>(height) / dipScale());

    target->PopAxisAlignedClip();
    HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderer_.discard();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    if (FAILED(hr))
        return false;

    hasPainted_ = true;
    return true;
}

void FluentDialogSurface::invalidate(const RECT* rect) const {
    if (hwnd_)
        InvalidateRect(hwnd_, rect, FALSE);
}

void FluentDialogSurface::discard() {
    textCache_.clear();
    renderer_.releaseAll();
}

CompositedScrollView::~CompositedScrollView() { reset(); }

LRESULT CALLBACK CompositedScrollView::windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

bool CompositedScrollView::create(HWND parent, UINT_PTR timer, Changed changed,
                                  DCompRenderer* drawingDeviceSource) {
    reset();
    renderer_.setDrawingDeviceSource(drawingDeviceSource);
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    constexpr wchar_t className[] = L"QQMusicLyricScrollView";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    window_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, className, L"",
                              WS_CHILD | WS_DISABLED | WS_VISIBLE, 0, 0, 1, 1,
                              parent, nullptr, instance, nullptr);
    if (!window_)
        return false;
    parent_ = parent;
    timer_ = timer;
    changed_ = std::move(changed);
    return true;
}

void CompositedScrollView::layout(const D2D1_RECT_F& viewport, float contentHeight, UINT dpi,
                                 bool showWindow) {
    const float scale = static_cast<float>(dpi) / 96.0f;
    const bool changed = viewport.left != viewport_.left || viewport.top != viewport_.top ||
                         viewport.right != viewport_.right || viewport.bottom != viewport_.bottom ||
                         contentHeight != height_ || dpi != dpi_;
    if (!changed)
        return;
    stop();
    viewport_ = viewport;
    height_ = std::max(0.0f, contentHeight);
    dpi_ = dpi;
    maximum_ = std::max(0.0f, height_ - (viewport.bottom - viewport.top));
    from_ = target_ = std::clamp(target_, 0.0f, maximum_);
    ready_ = false;
    if (window_) {
        const int left = static_cast<int>(std::floor(viewport.left * scale));
        const int top = static_cast<int>(std::floor(viewport.top * scale));
        SetWindowPos(window_, nullptr, left, top,
                     std::max(1, static_cast<int>(std::ceil(viewport.right * scale)) - left),
                     std::max(1, static_cast<int>(std::ceil(viewport.bottom * scale)) - top),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(window_, showWindow && viewport.right > viewport.left && viewport.bottom > viewport.top
                                ? SW_SHOWNA : SW_HIDE);
    }
}

float CompositedScrollView::offset() const {
    if (!animating_)
        return target_;
    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - started_).count();
    const float displacement = from_ - target_;
    return std::clamp(target_ + (displacement + (speed_ + 28.0f * displacement) * elapsed) *
                                   std::exp(-28.0f * elapsed), 0.0f, maximum_);
}

float CompositedScrollView::velocity() const {
    if (!animating_)
        return 0.0f;
    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - started_).count();
    return (speed_ - 28.0f * (speed_ + 28.0f * (from_ - target_)) * elapsed) *
           std::exp(-28.0f * elapsed);
}

bool CompositedScrollView::needsCache(float position) const {
    const float viewHeight = viewport_.bottom - viewport_.top;
    return !ready_ ||
           (cacheTop_ > 0.0f && position < cacheTop_ + viewHeight * 0.5f) ||
           (cacheTop_ + cacheHeight_ < height_ &&
            position + viewHeight > cacheTop_ + cacheHeight_ - viewHeight * 0.5f);
}

void CompositedScrollView::applyAnimation() {
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const float position = offset();
    // 缓存按物理像素绘制；静止时也必须整像素放置，避免底部的小数偏移使整层发虚。
    const float destination = std::round((cacheTop_ - target_) * scale);
    if (animating_)
        renderer_.animateRootScroll((cacheTop_ - position) * scale,
                                    destination, -velocity() * scale, 28.0f);
    else
        renderer_.setRootOffsetY(destination);
}

void CompositedScrollView::paint(const Draw& draw, bool redraw) {
    if (!window_ || viewport_.bottom <= viewport_.top || viewport_.right <= viewport_.left)
        return;
    const float position = offset();
    if ((animating_ || !redraw) && !needsCache(position))
        return;
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const float viewHeight = viewport_.bottom - viewport_.top;
    cacheHeight_ = std::max(viewHeight, std::min(height_ + 2.0f,
                                               std::min(viewHeight * 3.0f, 8192.0f / scale)));
    cacheTop_ = std::round(std::clamp(position - viewHeight, 0.0f,
                                     std::max(0.0f, height_ - cacheHeight_)) * scale) / scale;
    const float left = std::floor(viewport_.left * scale) / scale;
    const float top = std::floor(viewport_.top * scale) / scale;
    const int width = std::max(1, static_cast<int>(std::ceil(viewport_.right * scale) -
                                                 std::floor(viewport_.left * scale)));
    const int height = std::max(1, static_cast<int>(std::ceil(cacheHeight_ * scale)));
    renderer_.setDpi(dpi_);
    if (!renderer_.bind(window_, width, height)) {
        ready_ = false;
        return;
    }
    auto* target = renderer_.renderTarget();
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Translation(-left, position - cacheTop_ - top));
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    {
        Painter painter(target, renderer_.dwrite());
        draw(painter, D2D1::RectF(viewport_.left, viewport_.top + cacheTop_ - position,
                                 viewport_.right, viewport_.top + cacheTop_ - position + cacheHeight_),
             position);
    }
    if (FAILED(target->EndDraw())) {
        renderer_.discard();
        ready_ = false;
        return;
    }
    applyAnimation();
    renderer_.setRootOpacity(contentVisible_ ? 1.0f : 0.0f);
    ready_ = renderer_.present();
    if (!ready_)
        renderer_.discard();
}

void CompositedScrollView::setOffset(float position) {
    animating_ = false;
    if (parent_)
        KillTimer(parent_, timer_);
    from_ = target_ = std::clamp(position, 0.0f, maximum_);
    speed_ = 0.0f;
    applyAnimation();
    renderer_.commit();
}

void CompositedScrollView::stop() { setOffset(offset()); }

void CompositedScrollView::setVisible(bool visible) {
    if (window_ && ((GetWindowLongPtrW(window_, GWL_STYLE) & WS_VISIBLE) != 0) != visible) {
        if (visible)
            renderer_.waitForCommitCompletion();
        ShowWindow(window_, visible ? SW_SHOWNA : SW_HIDE);
    }
}

void CompositedScrollView::setContentVisible(bool visible) {
    contentVisible_ = visible;
    renderer_.setRootOpacity(visible ? 1.0f : 0.0f);
}

void CompositedScrollView::scrollBy(float delta, bool smooth) {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    const float position = offset();
    const float next = std::clamp(
        (animating_ && delta * (target_ - position) > 0.0f ? target_ : position) + delta,
        0.0f, maximum_);
    if (std::fabs(next - position) < 0.01f && !animating_)
        return;
    if (!smooth || !enabled) {
        setOffset(next);
    } else {
        const float currentVelocity = velocity();
        from_ = position;
        target_ = next;
        speed_ = delta * currentVelocity < 0.0f ? 0.0f : currentVelocity;
        const float maximumSpeed = 28.0f * std::fabs(target_ - from_);
        speed_ = std::clamp(speed_, -maximumSpeed, maximumSpeed);
        started_ = std::chrono::steady_clock::now();
        if (!animating_)
            animating_ = SetTimer(parent_, timer_, USER_TIMER_MINIMUM, nullptr) != 0;
        if (!animating_)
            setOffset(next);
    }
    if (changed_)
        changed_(offset(), needsCache(offset()));
    if (parent_)
        UpdateWindow(parent_);
    applyAnimation();
    renderer_.commit();
}

bool CompositedScrollView::tick(WPARAM timer) {
    if (timer != timer_ || !animating_)
        return false;
    const float position = offset();
    if (std::fabs(position - target_) < 0.1f && std::fabs(velocity()) < 2.0f)
        setOffset(target_);
    if (changed_)
        changed_(offset(), needsCache(offset()) || !animating_);
    return true;
}

void CompositedScrollView::reset() {
    if (parent_ && IsWindow(parent_))
        KillTimer(parent_, timer_);
    renderer_.releaseAll();
    if (window_ && IsWindow(window_))
        DestroyWindow(window_);
    parent_ = nullptr;
    window_ = nullptr;
    timer_ = 0;
    dpi_ = 96;
    viewport_ = {};
    height_ = maximum_ = cacheTop_ = cacheHeight_ = 0.0f;
    changed_ = {};
    ready_ = animating_ = false;
    contentVisible_ = true;
    from_ = target_ = speed_ = 0.0f;
}

} // namespace fluent
