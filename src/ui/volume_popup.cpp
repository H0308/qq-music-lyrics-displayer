#include "volume_popup.h"

#include "fluent_theme.h"
#include "graphics/composition_renderer.h"
#include "media_control_icons.h"
#include "logging/runtime_logger.h"
#include "util/com_release.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr wchar_t kWndClassName[] = L"QQMusicLyricVolumePopup";
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kHideDelayMs = 160;

// 主体为 220 × 44 DIP；圆角、阴影与背景材质统一由 DWM 绘制。
constexpr float kWidthDip = 220.0f;
constexpr float kHeightDip = 44.0f;
constexpr float kIconCenterDip = 22.0f;
constexpr float kTrackLeftDip = 48.0f;
constexpr float kTrackRightDip = 164.0f;
constexpr float kTextLeftDip = 180.0f;
constexpr float kTextRightDip = 208.0f;
constexpr float kKnobRadiusDip = 7.5f;
constexpr float kTrackHitPaddingDip = kKnobRadiusDip;
constexpr float kTrackHalfHitDip = 14.0f;

} // namespace

struct VolumePopup::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT dpi = 96;

    bool visible = false;
    bool anchorHover = false;
    bool popupHover = false;
    bool dragging = false;
    bool hoverSlider = false;

    int percent = 0;
    bool muted = false;
    bool available = false;
    uint64_t requestRevision = 0;
    std::function<void(AppVolumeRequest)> onChange;

    CompositionRenderer renderer;
    uint64_t resourceGeneration = 0;
    ID2D1SolidColorBrush* brushText = nullptr;
    ID2D1SolidColorBrush* brushSecondary = nullptr;
    ID2D1SolidColorBrush* brushTrack = nullptr;
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* brushKnob = nullptr;
    IDWriteTextFormat* fmtValue = nullptr;

    float scale() const { return static_cast<float>(dpi) / 96.0f; }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    void releaseDrawingResources() {
        releaseCom(brushText);
        releaseCom(brushSecondary);
        releaseCom(brushTrack);
        releaseCom(brushAccent);
        releaseCom(brushKnob);
        releaseCom(fmtValue);
        resourceGeneration = 0;
    }

    void releaseResources() {
        releaseDrawingResources();
        renderer.discard();
    }

    bool ensureResources() {
        if (resourceGeneration == renderer.deviceGeneration() && fmtValue)
            return true;
        releaseDrawingResources();
        auto* rt = renderer.renderTarget();
        if (!rt || !renderer.dwrite())
            return false;
        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        if (FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushTrack)) ||
            FAILED(rt->CreateSolidColorBrush(p.accent, &brushAccent)) ||
            FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushKnob)))
            return false;
        if (FAILED(renderer.dwrite()->CreateTextFormat(
                fluent::uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"",
                &fmtValue)))
            return false;
        fmtValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        fmtValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fluent::applyUiFontFallback(fmtValue);
        resourceGeneration = renderer.deviceGeneration();
        return true;
    }

    float valueFraction() const {
        return std::clamp(percent, 0, 100) / 100.0f;
    }

    int percentAt(float xDip) const {
        const float fraction = std::clamp(
            (xDip - kTrackLeftDip) / (kTrackRightDip - kTrackLeftDip), 0.0f, 1.0f);
        return static_cast<int>(std::lround(fraction * 100.0f));
    }

    bool hitSlider(float xDip, float yDip) const {
        const float cy = kHeightDip * 0.5f;
        return xDip >= kTrackLeftDip - kTrackHitPaddingDip &&
               xDip <= kTrackRightDip + kTrackHitPaddingDip &&
               std::fabs(yDip - cy) <= kTrackHalfHitDip;
    }

    bool applyPercent(int next) {
        next = std::clamp(next, 0, 100);
        if (next == percent && !muted)
            return false;
        percent = next;
        muted = false; // 调整音量即解除静音（控制器侧同样处理）
        const auto request = AppVolumeRequest::create(percent);
        requestRevision = request.revision;
        render();
        if (onChange)
            onChange(request);
        return true;
    }

    bool render() {
        if (!hwnd)
            return false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int pxW = rc.right - rc.left;
        const int pxH = rc.bottom - rc.top;
        if (pxW <= 0 || pxH <= 0)
            return false;
        renderer.setDpi(dpi);
        if (!renderer.bind(hwnd, pxW, pxH))
            return false;
        if (!ensureResources()) {
            releaseResources();
            return false;
        }

        auto* rt = renderer.renderTarget();
        const float h = dip(pxH);
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        // 只绘制前景，透明底面透出 DWM 的实时 Acrylic。
        const bool dark = fluent::isDarkMode(fluent::ThemeTarget::Window);

        // 音量图标
        const int level = !available || muted ? 0
                                              : percent == 0 ? 1
                                                             : percent < 50 ? 2 : 3;
        media_control::drawVolume(rt, D2D1::Point2F(kIconCenterDip, h * 0.5f), 7.0f,
                                  available ? brushText : brushSecondary, level);

        // 细轨道与圆形滑块共用同一音量位置，交互热区覆盖两端的圆钮。
        const float cy = h * 0.5f;
        const float trackTop = cy - 2.0f;
        const float trackBottom = cy + 2.0f;
        brushTrack->SetOpacity(!available ? 0.18f :
            dark ? (hoverSlider || dragging ? 0.65f : 0.55f) :
                   (hoverSlider || dragging ? 0.38f : 0.30f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(kTrackLeftDip, trackTop, kTrackRightDip,
                                          trackBottom),
                              2.0f, 2.0f),
            brushTrack);
        const float fillRight =
            kTrackLeftDip + (kTrackRightDip - kTrackLeftDip) * valueFraction();
        if (available && !muted && fillRight > kTrackLeftDip) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(kTrackLeftDip, trackTop, fillRight,
                                              trackBottom),
                                  2.0f, 2.0f),
                brushAccent);
        }
        if (available) {
            const auto center = D2D1::Point2F(fillRight, cy);
            rt->FillEllipse(D2D1::Ellipse(center, kKnobRadiusDip, kKnobRadiusDip), brushKnob);
            const float innerRadius = dragging ? 3.0f : hoverSlider ? 4.5f : 3.8f;
            rt->FillEllipse(D2D1::Ellipse(center, innerRadius, innerRadius), brushAccent);
        }
        // 固定宽度数值区，只显示音量值，不附加百分号。
        std::wstring text = available ? std::to_wstring(percent) : L"--";
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmtValue,
                      D2D1::RectF(kTextLeftDip, 0.0f, kTextRightDip, h),
                      available ? brushText : brushSecondary, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            releaseResources();
            return false;
        }
        if (FAILED(hr)) {
            releaseResources();
            return false;
        }
        if (!renderer.present()) {
            releaseResources();
            return false;
        }
        return true;
    }

    void scheduleHide() {
        if (!hwnd || !visible || anchorHover || popupHover || dragging)
            return;
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void showNear(const RECT& anchorRect, bool verticalTaskbar, bool popupOnRight) {
        if (!hwnd)
            return;
        if (!visible && !fluent::applyAcrylicBackdrop(hwnd)) {
            runtime_log::writef(L"[volume-popup] native Acrylic unavailable on show");
            return;
        }
        KillTimer(hwnd, kHideTimer);
        const float s = scale();
        const int popupW = static_cast<int>(std::lround(kWidthDip * s));
        const int popupH = static_cast<int>(std::lround(kHeightDip * s));
        const int gap = static_cast<int>(std::lround(6.0f * s));

        HMONITOR monitor = MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        const RECT work = info.rcWork;

        int x = 0;
        int y = 0;
        if (verticalTaskbar) {
            x = popupOnRight ? anchorRect.right + gap : anchorRect.left - gap - popupW;
            if (popupOnRight && x + popupW > work.right) {
                popupOnRight = false;
                x = anchorRect.left - gap - popupW;
            } else if (!popupOnRight && x < work.left) {
                popupOnRight = true;
                x = anchorRect.right + gap;
            }
            x = std::clamp(x, static_cast<int>(work.left),
                           std::max(static_cast<int>(work.left),
                                    static_cast<int>(work.right) - popupW));
            const int anchorCenterY = (anchorRect.top + anchorRect.bottom) / 2;
            y = anchorCenterY - popupH / 2;
            y = std::clamp(y, static_cast<int>(work.top),
                           std::max(static_cast<int>(work.top),
                                    static_cast<int>(work.bottom) - popupH));
        } else {
            const int anchorCenterX = (anchorRect.left + anchorRect.right) / 2;
            x = anchorCenterX - popupW / 2;
            x = std::clamp(x, static_cast<int>(work.left),
                           std::max(static_cast<int>(work.left),
                                    static_cast<int>(work.right) - popupW));
            y = anchorRect.top - gap - popupH;
            if (y < work.top)
                y = anchorRect.bottom + gap;
        }
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, popupW, popupH,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (!visible) {
            if (!render())
                return;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            visible = true;
            runtime_log::writef(L"[action][volume-popup] shown percent=%d available=%d",
                                percent, available ? 1 : 0);
        }
    }

    void hide() {
        if (!hwnd)
            return;
        KillTimer(hwnd, kHideTimer);
        dragging = false;
        hoverSlider = false;
        if (visible) {
            visible = false;
            ShowWindow(hwnd, SW_HIDE);
            runtime_log::writef(L"[action][volume-popup] hidden");
        }
        if (GetCapture() == hwnd)
            ReleaseCapture();
    }

    void trackMouseLeave() {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCCALCSIZE:
            if (wp)
                return 0; // 保留系统边框样式供 DWM 绘制圆角和阴影，客户区占满窗口。
            break;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT client{};
            GetClientRect(hwnd, &client);
            popupHover = PtInRect(&client, point) != FALSE;
            if (popupHover || dragging)
                KillTimer(hwnd, kHideTimer);
            if (popupHover)
                trackMouseLeave();
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (dragging) {
                applyPercent(percentAt(x));
            } else {
                const bool hover = available && hitSlider(x, y);
                if (hover != hoverSlider) {
                    hoverSlider = hover;
                    render();
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            popupHover = false;
            hoverSlider = false;
            if (visible)
                render();
            scheduleHide();
            return 0;
        case WM_LBUTTONDOWN: {
            if (!available)
                return 0;
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (hitSlider(x, y)) {
                KillTimer(hwnd, kHideTimer);
                dragging = true;
                SetCapture(hwnd);
                if (!applyPercent(percentAt(x)))
                    render();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                if (!applyPercent(percentAt(dip(GET_X_LPARAM(lp)))))
                    render();
                scheduleHide();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging) {
                dragging = false;
                if (visible)
                    render();
                scheduleHide();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (!available)
                return 0;
            const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (steps != 0)
                applyPercent(percent + steps * 2);
            return 0;
        }
        case WM_TIMER:
            if (wp == kHideTimer) {
                KillTimer(hwnd, kHideTimer);
                if (!anchorHover && !popupHover && !dragging)
                    hide();
            }
            return 0;
        case WM_DPICHANGED: {
            dpi = GetDpiForWindow(hwnd);
            releaseResources();
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                static_cast<int>(std::lround(kWidthDip * scale())),
                static_cast<int>(std::lround(kHeightDip * scale())),
                SWP_NOACTIVATE | SWP_NOZORDER);
            if (visible)
                render();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DWMCOMPOSITIONCHANGED:
            if (!fluent::applyAcrylicBackdrop(hwnd)) {
                runtime_log::writef(L"[volume-popup] native Acrylic unavailable");
                hide();
            }
            releaseResources();
            if (visible)
                render();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            hide();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kHideTimer);
            releaseResources();
            renderer.releaseAll();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(cs->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
};

VolumePopup::VolumePopup() : impl_(std::make_unique<Impl>()) {}

VolumePopup::~VolumePopup() {
    destroy();
}

bool VolumePopup::create(HINSTANCE inst) {
    if (impl_->hwnd)
        return true;
    impl_->inst = inst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW |
                          WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    impl_->hwnd = CreateWindowExW(exStyle, kWndClassName, L"QQMusicLyricVolumePopup",
                                  WS_POPUP | WS_THICKFRAME, 0, 0, 1, 1, nullptr, nullptr, inst,
                                  impl_.get());
    if (!impl_->hwnd)
        return false;
    impl_->dpi = GetDpiForWindow(impl_->hwnd);
    if (impl_->dpi == 0)
        impl_->dpi = GetDpiForSystem();
    if (!fluent::applyAcrylicBackdrop(impl_->hwnd) || !impl_->renderer.initialize()) {
        runtime_log::writef(L"[volume-popup] native Acrylic initialization failed");
        destroy();
        return false;
    }
    return true;
}

void VolumePopup::destroy() {
    if (!impl_)
        return;
    impl_->hide();
    if (impl_->hwnd)
        DestroyWindow(impl_->hwnd);
    impl_->renderer.releaseAll();
}

void VolumePopup::setVolume(const AppVolumeState& state) {
    if (state.available && state.requestRevision < impl_->requestRevision)
        return;
    const bool changed = state.percent != impl_->percent || state.muted != impl_->muted ||
                         state.available != impl_->available;
    impl_->percent = std::clamp(state.percent, 0, 100);
    impl_->muted = state.muted;
    impl_->available = state.available;
    impl_->requestRevision = state.requestRevision;
    if (!state.available) {
        impl_->dragging = false;
        if (impl_->hwnd && GetCapture() == impl_->hwnd)
            ReleaseCapture();
    }
    if (changed && impl_->visible)
        impl_->render();
}

void VolumePopup::setCallback(std::function<void(AppVolumeRequest)> cb) {
    impl_->onChange = std::move(cb);
}

void VolumePopup::showNear(const RECT& anchorRect, bool verticalTaskbar, bool popupOnRight) {
    impl_->showNear(anchorRect, verticalTaskbar, popupOnRight);
}

void VolumePopup::hide() {
    impl_->hide();
}

bool VolumePopup::isVisible() const {
    return impl_->visible;
}

void VolumePopup::onAnchorEnter() {
    impl_->anchorHover = true;
    if (impl_->hwnd)
        KillTimer(impl_->hwnd, kHideTimer);
}

void VolumePopup::onAnchorLeave() {
    impl_->anchorHover = false;
    impl_->scheduleHide();
}
