#include "volume_popup.h"

#include "fluent_theme.h"
#include "lyric_renderer.h"
#include "media_control_icons.h"
#include "logging/runtime_logger.h"

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

constexpr float kWidthDip = 196.0f;
constexpr float kHeightDip = 44.0f;
constexpr float kCornerDip = 10.0f;
constexpr float kIconLeftDip = 14.0f;
constexpr float kTrackLeftDip = 46.0f;
constexpr float kTrackRightDip = 148.0f;
constexpr float kTextRightDip = 186.0f;
constexpr float kKnobRadiusDip = 6.5f;
constexpr float kTrackHalfHitDip = 9.0f;

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
    std::function<void(int)> onChange;

    LyricRenderer renderer;
    ID2D1SolidColorBrush* brushBg = nullptr;
    ID2D1SolidColorBrush* brushStroke = nullptr;
    ID2D1SolidColorBrush* brushText = nullptr;
    ID2D1SolidColorBrush* brushSecondary = nullptr;
    ID2D1SolidColorBrush* brushTrack = nullptr;
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* brushKnob = nullptr;
    IDWriteTextFormat* fmtValue = nullptr;

    float scale() const { return static_cast<float>(dpi) / 96.0f; }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    void releaseResources() {
        auto releaseBrush = [](ID2D1SolidColorBrush*& brush) {
            if (brush) {
                brush->Release();
                brush = nullptr;
            }
        };
        releaseBrush(brushBg);
        releaseBrush(brushStroke);
        releaseBrush(brushText);
        releaseBrush(brushSecondary);
        releaseBrush(brushTrack);
        releaseBrush(brushAccent);
        releaseBrush(brushKnob);
        if (fmtValue) {
            fmtValue->Release();
            fmtValue = nullptr;
        }
        renderer.discard();
    }

    bool ensureResources() {
        if (brushBg)
            return true;
        auto* rt = renderer.renderTarget();
        if (!rt || !renderer.dwrite())
            return false;
        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        if (FAILED(rt->CreateSolidColorBrush(p.cardFillSolid, &brushBg)) ||
            FAILED(rt->CreateSolidColorBrush(p.cardStroke, &brushStroke)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.separator, &brushTrack)) ||
            FAILED(rt->CreateSolidColorBrush(p.accent, &brushAccent)) ||
            FAILED(rt->CreateSolidColorBrush(p.textOnAccent, &brushKnob)))
            return false;
        if (FAILED(renderer.dwrite()->CreateTextFormat(
                fluent::uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"",
                &fmtValue)))
            return false;
        fmtValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        fmtValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fluent::applyUiFontFallback(fmtValue);
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
        return xDip >= kTrackLeftDip - kKnobRadiusDip &&
               xDip <= kTrackRightDip + kKnobRadiusDip &&
               std::fabs(yDip - cy) <= kTrackHalfHitDip;
    }

    void applyPercent(int next) {
        next = std::clamp(next, 0, 100);
        if (next == percent && !muted)
            return;
        percent = next;
        muted = false; // 调整音量即解除静音（控制器侧同样处理）
        render();
        if (onChange)
            onChange(percent);
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
        if (!renderer.bindDC(pxW, pxH))
            return false;
        if (!ensureResources())
            return false;

        auto* rt = renderer.renderTarget();
        const float w = dip(pxW);
        const float h = dip(pxH);
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const auto card = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, w, h), kCornerDip,
                                            kCornerDip);
        rt->FillRoundedRectangle(card, brushBg);
        rt->DrawRoundedRectangle(card, brushStroke, 1.0f);

        // 音量图标
        const int level = !available || muted ? 0
                                              : percent == 0 ? 1
                                                             : percent < 50 ? 2 : 3;
        media_control::drawVolume(rt, D2D1::Point2F(kIconLeftDip + 8.0f, h * 0.5f), 9.0f,
                                  available ? brushText : brushSecondary, level);

        // 滑块轨道 + 已填部分 + 旋钮
        const float cy = h * 0.5f;
        const float trackTop = cy - 2.0f;
        const float trackBottom = cy + 2.0f;
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(kTrackLeftDip, trackTop, kTrackRightDip,
                                          trackBottom),
                              2.0f, 2.0f),
            brushTrack);
        const float fillRight =
            kTrackLeftDip + (kTrackRightDip - kTrackLeftDip) * valueFraction();
        if (available && fillRight > kTrackLeftDip) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(kTrackLeftDip, trackTop, fillRight,
                                              trackBottom),
                                  2.0f, 2.0f),
                brushAccent);
        }
        if (available) {
            const float knobX = std::clamp(fillRight, kTrackLeftDip, kTrackRightDip);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), kKnobRadiusDip,
                                          kKnobRadiusDip),
                            brushAccent);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), 2.6f, 2.6f), brushKnob);
        }

        // 百分比数值
        std::wstring text = available ? std::to_wstring(percent) + L"%" : L"--";
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmtValue,
                      D2D1::RectF(kTrackRightDip + 4.0f, 0.0f, kTextRightDip, h),
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
        return renderer.present(hwnd);
    }

    void scheduleHide() {
        if (!hwnd || !visible || anchorHover || popupHover)
            return;
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void showNear(const RECT& anchorRect) {
        if (!hwnd)
            return;
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

        const int anchorCenterX = (anchorRect.left + anchorRect.right) / 2;
        int x = anchorCenterX - popupW / 2;
        x = std::clamp(x, static_cast<int>(work.left),
                       std::max(static_cast<int>(work.left),
                                static_cast<int>(work.right) - popupW));
        int y = anchorRect.top - gap - popupH;
        if (y < work.top)
            y = anchorRect.bottom + gap;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, popupW, popupH,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (!visible) {
            render();
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
    }

    void trackMouseLeave() {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            popupHover = true;
            KillTimer(hwnd, kHideTimer);
            trackMouseLeave();
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (dragging) {
                applyPercent(percentAt(x));
            } else {
                const bool hover = available && hitSlider(x, y);
                if (hover != hoverSlider) {
                    hoverSlider = hover;
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            popupHover = false;
            hoverSlider = false;
            scheduleHide();
            return 0;
        case WM_LBUTTONDOWN: {
            if (!available)
                return 0;
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (hitSlider(x, y)) {
                dragging = true;
                SetCapture(hwnd);
                applyPercent(percentAt(x));
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            dragging = false;
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
                if (!anchorHover && !popupHover)
                    hide();
            }
            return 0;
        case WM_DPICHANGED:
            dpi = GetDpiForWindow(hwnd);
            releaseResources();
            if (visible)
                render();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
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

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    impl_->hwnd = CreateWindowExW(exStyle, kWndClassName, L"QQMusicLyricVolumePopup",
                                  WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst,
                                  impl_.get());
    if (!impl_->hwnd)
        return false;
    impl_->dpi = GetDpiForWindow(impl_->hwnd);
    if (impl_->dpi == 0)
        impl_->dpi = GetDpiForSystem();
    return impl_->renderer.initialize();
}

void VolumePopup::destroy() {
    if (!impl_)
        return;
    impl_->hide();
    if (impl_->hwnd)
        DestroyWindow(impl_->hwnd);
    impl_->renderer.releaseAll();
}

void VolumePopup::setVolume(int percent, bool muted, bool available) {
    const bool changed = percent != impl_->percent || muted != impl_->muted ||
                         available != impl_->available;
    impl_->percent = std::clamp(percent, 0, 100);
    impl_->muted = muted;
    impl_->available = available;
    if (changed && impl_->visible)
        impl_->render();
}

void VolumePopup::setCallback(std::function<void(int)> cb) {
    impl_->onChange = std::move(cb);
}

void VolumePopup::showNear(const RECT& anchorRect) {
    impl_->showNear(anchorRect);
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
