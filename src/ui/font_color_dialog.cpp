#include "font_color_dialog.h"

#include "ui/color_picker_dialog.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_theme.h"
#include "resource.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <dwrite.h>

namespace {

constexpr int kIdLabelUnplayed = 401;
constexpr int kIdLabelPlayed = 402;
constexpr int kIdLabelAlpha = 403;
constexpr int kIdLabelGlow = 404;
constexpr int kIdLabelOutline = 405;
constexpr int kIdSwatchUnplayed = 411;
constexpr int kIdSwatchPlayed = 412;
constexpr int kIdSwatchGlow = 413;
constexpr int kIdSwatchOutline = 414;
constexpr int kIdToggleGlow = 415;
constexpr int kIdToggleOutline = 416;
constexpr int kIdAlphaSlider = 417;
constexpr int kIdAlphaValue = 418;
constexpr int kIdPreview = 420;
constexpr int kIdOk = 421;
constexpr int kIdCancel = 422;
constexpr int kIdTitleLabel = 423;
constexpr int kIdSubtitleLabel = 424;
constexpr int kIdOptionsCard = 425;

// 未播放透明度范围（与设置项 lyricUnplayedAlpha 的钳制一致）
constexpr int kAlphaMin = 5;
constexpr int kAlphaMax = 100;

const wchar_t kSampleText[] = L"我是你爸爸，养你这么大";
// 与任务栏 drawScrollingText 一致的光晕/描边参数
constexpr float kGlowOffset = 2.4f;
constexpr float kOutlineOffset = 1.2f;
constexpr float kGlowAlpha = 0.28f;
constexpr float kOutlineAlpha = 0.50f;
// 预览中模拟逐字进度的已播放比例
constexpr float kPlayedRatio = 0.55f;

D2D1_COLOR_F colorOf(COLORREF c, float alpha = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f,
                        alpha);
}

// 颜色色块按钮：圆角卡片内嵌纯色块，点击向父窗口发送 WM_COMMAND/BN_CLICKED
class ColorSwatch : public fluent::LayeredChild {
public:
    bool create(HWND parent, int id, COLORREF color) {
        id_ = id;
        color_ = color;
        clearAlpha_ = 1.0f / 255.0f; // 整个色块面可点（防 alpha=0 穿透）
        return createLayered(parent, L"QQMusicLyricColorSwatch", wndProc, id, true, true);
    }

    void setColor(COLORREF c) {
        if (c == color_)
            return;
        color_ = c;
        renderNow();
    }

private:
    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        const fluent::Palette& p = fluent::palette();
        auto* br = brush(rt);
        if (!br)
            return;
        D2D1_RECT_F rect = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
        br->SetColor(hover_ ? p.controlHover : p.controlFill);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(rect, fluent::metrics::controlRadius, fluent::metrics::controlRadius),
            br);
        br->SetColor(hover_ ? p.accent : p.cardStroke);
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(rect, fluent::metrics::controlRadius, fluent::metrics::controlRadius),
            br, 1.0f);
        D2D1_RECT_F inner = D2D1::RectF(rect.left + 4.0f, rect.top + 4.0f, rect.right - 4.0f,
                                        rect.bottom - 4.0f);
        br->SetColor(colorOf(color_));
        rt->FillRoundedRectangle(D2D1::RoundedRect(inner, 2.5f, 2.5f), br);
        if (focused_) {
            br->SetColor(p.accent);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, wDip - 1.5f, hDip - 1.5f),
                                  fluent::metrics::controlRadius - 1.0f,
                                  fluent::metrics::controlRadius - 1.0f),
                br, 1.5f);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        ColorSwatch* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<ColorSwatch*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ColorSwatch*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_GETDLGCODE:
            return DLGC_BUTTON;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->renderNow();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (!self->hover_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                self->hover_ = true;
                self->renderNow();
            }
            return 0;
        case WM_MOUSELEAVE:
            self->hover_ = false;
            self->renderNow();
            return 0;
        case WM_SETFOCUS:
            self->focused_ = true;
            self->renderNow();
            return 0;
        case WM_KILLFOCUS:
            self->focused_ = false;
            self->renderNow();
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE || wp == VK_RETURN) {
                SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(self->id_, BN_CLICKED),
                             reinterpret_cast<LPARAM>(h));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(self->id_, BN_CLICKED),
                         reinterpret_cast<LPARAM>(h));
            return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    int id_ = 0;
    COLORREF color_ = 0;
    bool hover_ = false;
    bool focused_ = false;
};

// Win11 风格滑块：轨道 + 已填充段 + 圆形滑头，拖动改变取值（kAlphaMin..kAlphaMax），
// 取值变化向父窗口发送 WM_COMMAND/BN_CLICKED
class AlphaSlider : public fluent::LayeredChild {
public:
    bool create(HWND parent, int id, int pct) {
        id_ = id;
        pct_ = std::clamp(pct, kAlphaMin, kAlphaMax);
        clearAlpha_ = 1.0f / 255.0f; // 整个滑块面可点（防 alpha=0 穿透）
        return createLayered(parent, L"QQMusicLyricAlphaSlider", wndProc, id, true, true);
    }

    int value() const { return pct_; }

private:
    // 滑头圆心允许的活动范围（DIP），两端各留滑头半径
    float trackL(float wDip) const { return kThumbR; }
    float trackR(float wDip) const { return wDip - kThumbR; }
    float thumbX(float wDip) const {
        float t = static_cast<float>(pct_ - kAlphaMin) / (kAlphaMax - kAlphaMin);
        return trackL(wDip) + t * (trackR(wDip) - trackL(wDip));
    }

    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        const fluent::Palette& p = fluent::palette();
        auto* br = brush(rt);
        if (!br)
            return;
        float cy = hDip * 0.5f;
        float fx = thumbX(wDip);
        D2D1_RECT_F track = D2D1::RectF(trackL(wDip), cy - 2.0f, trackR(wDip), cy + 2.0f);
        br->SetColor(p.controlFill);
        rt->FillRoundedRectangle(D2D1::RoundedRect(track, 2.0f, 2.0f), br);
        br->SetColor(p.accent);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(track.left, track.top, fx, track.bottom), 2.0f, 2.0f),
            br);
        // 滑头：白芯 + 强调色描边
        br->SetColor(p.windowBg);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fx, cy), kThumbR, kThumbR), br);
        br->SetColor(p.accent);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fx, cy), kThumbR - 1.0f, kThumbR - 1.0f),
                        br, 2.0f);
        if (focused_) {
            br->SetColor(p.accent);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(1.5f, 2.0f, wDip - 1.5f, hDip - 2.0f), 6.0f, 6.0f),
                br, 1.5f);
        }
    }

    void updateFromMouse(LPARAM lp, bool notify) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        float s = fluent::dipScale(GetDpiForWindow(hwnd_));
        float wDip = (rc.right - rc.left) / s;
        float x = GET_X_LPARAM(lp) / s;
        float t = (x - trackL(wDip)) / (trackR(wDip) - trackL(wDip));
        int v = kAlphaMin +
                static_cast<int>(std::lround(std::clamp(t, 0.0f, 1.0f) * (kAlphaMax - kAlphaMin)));
        if (v == pct_)
            return;
        pct_ = v;
        renderNow();
        if (notify)
            SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, BN_CLICKED),
                         reinterpret_cast<LPARAM>(hwnd_));
    }

    void changeByKeyboard(int delta) {
        int next = std::clamp(pct_ + delta, kAlphaMin, kAlphaMax);
        if (next == pct_)
            return;
        pct_ = next;
        renderNow();
        SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, BN_CLICKED),
                     reinterpret_cast<LPARAM>(hwnd_));
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        AlphaSlider* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<AlphaSlider*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<AlphaSlider*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->renderNow();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETFOCUS:
            self->focused_ = true;
            self->renderNow();
            return 0;
        case WM_KILLFOCUS:
            self->focused_ = false;
            self->renderNow();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_LEFT || wp == VK_DOWN)
                self->changeByKeyboard(-1);
            else if (wp == VK_RIGHT || wp == VK_UP)
                self->changeByKeyboard(1);
            else if (wp == VK_HOME)
                self->changeByKeyboard(kAlphaMin - self->pct_);
            else if (wp == VK_END)
                self->changeByKeyboard(kAlphaMax - self->pct_);
            else
                break;
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            SetCapture(h);
            self->dragging_ = true;
            self->updateFromMouse(lp, true);
            return 0;
        case WM_MOUSEMOVE:
            if (self->dragging_)
                self->updateFromMouse(lp, true);
            return 0;
        case WM_LBUTTONUP:
            self->dragging_ = false;
            ReleaseCapture();
            return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    static constexpr float kThumbR = 8.0f;

    int id_ = 0;
    int pct_ = kAlphaMax;
    bool dragging_ = false;
    bool focused_ = false;
};

// Win11 风格开关：药丸轨道 + 圆形滑块，点击切换并向父窗口发送 WM_COMMAND/BN_CLICKED
class ToggleSwitch : public fluent::LayeredChild {
public:
    static constexpr float kWidth = 40.0f;
    static constexpr float kHeight = 20.0f;

    bool create(HWND parent, int id, bool on) {
        id_ = id;
        on_ = on;
        clearAlpha_ = 1.0f / 255.0f; // 整个开关面可点（防 alpha=0 穿透）
        return createLayered(parent, L"QQMusicLyricToggleSwitch", wndProc, id, true, true);
    }

    bool isOn() const { return on_; }

private:
    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        const fluent::Palette& p = fluent::palette();
        auto* br = brush(rt);
        if (!br)
            return;
        float cy = hDip * 0.5f;
        float trackH = std::min(kHeight, hDip);
        D2D1_RECT_F track = D2D1::RectF(0.5f, cy - trackH * 0.5f, wDip - 0.5f,
                                        cy + trackH * 0.5f);
        float radius = trackH * 0.5f;
        if (on_) {
            br->SetColor(hover_ ? p.accentHover : p.accent);
            rt->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), br);
        } else {
            br->SetColor(hover_ ? p.controlHover : p.controlFill);
            rt->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), br);
            br->SetColor(p.cardStroke);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(track, radius, radius), br, 1.0f);
        }
        float knobR = trackH * 0.5f - 3.5f;
        float knobX = on_ ? track.right - trackH * 0.5f : track.left + trackH * 0.5f;
        br->SetColor(on_ ? p.textOnAccent : p.textSecondary);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), knobR, knobR), br);
        if (focused_) {
            br->SetColor(p.accent);
            D2D1_RECT_F focusRect = D2D1::RectF(0.5f, cy - trackH * 0.5f - 3.0f,
                                                 wDip - 0.5f, cy + trackH * 0.5f + 3.0f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(focusRect, radius + 3.0f, radius + 3.0f),
                                     br, 1.5f);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        ToggleSwitch* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<ToggleSwitch*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ToggleSwitch*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_GETDLGCODE:
            return DLGC_BUTTON;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->renderNow();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (!self->hover_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                self->hover_ = true;
                self->renderNow();
            }
            return 0;
        case WM_MOUSELEAVE:
            self->hover_ = false;
            self->renderNow();
            return 0;
        case WM_SETFOCUS:
            self->focused_ = true;
            self->renderNow();
            return 0;
        case WM_KILLFOCUS:
            self->focused_ = false;
            self->renderNow();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE || wp == VK_RETURN) {
                self->on_ = !self->on_;
                self->renderNow();
                SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(self->id_, BN_CLICKED),
                             reinterpret_cast<LPARAM>(h));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            SetFocus(h);
            self->on_ = !self->on_;
            self->renderNow();
            SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(self->id_, BN_CLICKED),
                         reinterpret_cast<LPARAM>(h));
            return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    int id_ = 0;
    bool on_ = false;
    bool hover_ = false;
    bool focused_ = false;
};

// 示例歌词预览面板：复刻任务栏渲染（8 方向光晕/描边层 + 逐字已播放/未播放双色）
class StylePreviewPanel : public fluent::LayeredChild {
public:
    bool create(HWND parent, int id) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&dwrite_));
        return createLayered(parent, L"QQMusicLyricStylePreview", wndProc, id);
    }

    void setState(const FontColorDialog::State& s) {
        bool fontChanged = s.fontFamily != state_.fontFamily ||
                           s.lyricFontSize != state_.lyricFontSize;
        state_ = s;
        if (fontChanged)
            releaseLayout();
        renderNow();
    }

private:
    void releaseLayout() {
        if (layout_) {
            layout_->Release();
            layout_ = nullptr;
        }
        if (fmt_) {
            fmt_->Release();
            fmt_ = nullptr;
        }
    }

    void ensureLayout() {
        if (!dwrite_)
            return;
        if (!fmt_) {
            const wchar_t* family =
                state_.fontFamily.empty() ? fluent::uiFontFamily() : state_.fontFamily.c_str();
            if (FAILED(dwrite_->CreateTextFormat(
                    family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, state_.lyricFontSize, L"zh-cn", &fmt_)))
                return;
            if (state_.fontFamily.empty())
                fluent::applyUiFontFallback(fmt_);
        }
        if (fmt_ && !layout_) {
            if (FAILED(dwrite_->CreateTextLayout(kSampleText,
                                                 static_cast<UINT32>(wcslen(kSampleText)), fmt_,
                                                 4096.0f, 256.0f, &layout_)))
                return;
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(layout_->GetMetrics(&m))) {
                textW_ = m.width;
                textH_ = m.height;
            }
        }
    }

    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        const fluent::Palette& p = fluent::palette();
        auto* br = brush(rt);
        if (!br)
            return;
        D2D1_RECT_F rect = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
        br->SetColor(p.cardFill);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(rect, fluent::metrics::cardRadius, fluent::metrics::cardRadius), br);
        br->SetColor(p.cardStroke);
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(rect, fluent::metrics::cardRadius, fluent::metrics::cardRadius), br,
            1.0f);

        ensureLayout();
        if (!layout_ || textW_ <= 0.0f)
            return;
        float x = std::max((wDip - textW_) * 0.5f, 12.0f);
        float y = (hDip - textH_) * 0.5f;
        // 极端字号下也不画出卡片外
        rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        static constexpr float kDirs[8][2] = {{1.0f, 0.0f},
                                              {0.7071f, 0.7071f},
                                              {0.0f, 1.0f},
                                              {-0.7071f, 0.7071f},
                                              {-1.0f, 0.0f},
                                              {-0.7071f, -0.7071f},
                                              {0.0f, -1.0f},
                                              {0.7071f, -0.7071f}};
        D2D1_POINT_2F origin = D2D1::Point2F(x, y);
        // 与任务栏一致：先光晕层、再描边层、最后主文字
        if (state_.glowOn) {
            br->SetColor(colorOf(state_.glowColor, kGlowAlpha));
            for (auto& d : kDirs)
                rt->DrawTextLayout(
                    D2D1::Point2F(origin.x + d[0] * kGlowOffset, origin.y + d[1] * kGlowOffset),
                    layout_, br);
        }
        if (state_.outlineOn) {
            br->SetColor(colorOf(state_.outlineColor, kOutlineAlpha));
            for (auto& d : kDirs)
                rt->DrawTextLayout(D2D1::Point2F(origin.x + d[0] * kOutlineOffset,
                                                 origin.y + d[1] * kOutlineOffset),
                                   layout_, br);
        }
        br->SetColor(colorOf(state_.unplayed, state_.unplayedAlphaPct / 100.0f));
        rt->DrawTextLayout(origin, layout_, br);
        // 已播放部分：按像素裁剪出前 kPlayedRatio 区域，用已播放色再画一遍
        rt->PushAxisAlignedClip(D2D1::RectF(origin.x, origin.y - 8.0f,
                                            origin.x + textW_ * kPlayedRatio,
                                            origin.y + textH_ + 8.0f),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        br->SetColor(colorOf(state_.played));
        rt->DrawTextLayout(origin, layout_, br);
        rt->PopAxisAlignedClip();
        rt->PopAxisAlignedClip();
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        StylePreviewPanel* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<StylePreviewPanel*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<StylePreviewPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->renderNow();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            self->releaseLayout();
            if (self->dwrite_) {
                self->dwrite_->Release();
                self->dwrite_ = nullptr;
            }
            self->hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    FontColorDialog::State state_;
    IDWriteFactory* dwrite_ = nullptr;
    IDWriteTextFormat* fmt_ = nullptr;
    IDWriteTextLayout* layout_ = nullptr;
    float textW_ = 0;
    float textH_ = 0;
};

} // namespace

struct FontColorDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool backdrop = false;

    fluent::FluentLabel titleLabel;
    fluent::FluentLabel subtitleLabel;
    fluent::FluentCard optionsCard;
    State state; // 工作副本：确定前的一切修改只落在这里和预览上

    fluent::FluentLabel labelUnplayed;
    fluent::FluentLabel labelPlayed;
    fluent::FluentLabel labelAlpha;
    fluent::FluentLabel labelGlow;
    fluent::FluentLabel labelOutline;
    ColorSwatch swatchUnplayed;
    ColorSwatch swatchPlayed;
    ColorSwatch swatchGlow;
    ColorSwatch swatchOutline;
    ToggleSwitch toggleGlow;
    ToggleSwitch toggleOutline;
    AlphaSlider alphaSlider;
    fluent::FluentLabel alphaValue;
    StylePreviewPanel preview;
    fluent::FluentButton okBtn;
    fluent::FluentButton cancelBtn;

    // 内嵌取色器：同时最多打开一个，切换色块时丢弃未确认的上一个
    std::unique_ptr<ColorPickerDialog> picker;

    ApplyCallback onApply;

    void refreshTheme() {
        titleLabel.refreshTheme();
        subtitleLabel.refreshTheme();
        optionsCard.refreshTheme();
        labelUnplayed.refreshTheme();
        labelPlayed.refreshTheme();
        labelAlpha.refreshTheme();
        labelGlow.refreshTheme();
        labelOutline.refreshTheme();
        swatchUnplayed.refreshTheme();
        swatchPlayed.refreshTheme();
        swatchGlow.refreshTheme();
        swatchOutline.refreshTheme();
        toggleGlow.refreshTheme();
        toggleOutline.refreshTheme();
        alphaSlider.refreshTheme();
        alphaValue.refreshTheme();
        preview.refreshTheme();
        okBtn.refreshTheme();
        cancelBtn.refreshTheme();
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop);
            refreshTheme();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            fluent::paintDialogBackground(hwnd, hdc, backdrop);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            fluent::paintDialogBackground(hwnd, reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void createControls() {
        titleLabel.create(hwnd, kIdTitleLabel, L"字体颜色与效果", false, 20.0f, 600);
        subtitleLabel.create(hwnd, kIdSubtitleLabel, L"调整歌词颜色、透明度、光晕和描边", true, 13.0f,
                             400);
        optionsCard.create(hwnd, kIdOptionsCard);
        labelUnplayed.create(hwnd, kIdLabelUnplayed, L"未播放字体颜色");
        labelPlayed.create(hwnd, kIdLabelPlayed, L"已播放字体颜色");
        labelAlpha.create(hwnd, kIdLabelAlpha, L"未播放透明度");
        labelGlow.create(hwnd, kIdLabelGlow, L"光晕颜色");
        labelOutline.create(hwnd, kIdLabelOutline, L"描边颜色");
        swatchUnplayed.create(hwnd, kIdSwatchUnplayed, state.unplayed);
        swatchPlayed.create(hwnd, kIdSwatchPlayed, state.played);
        swatchGlow.create(hwnd, kIdSwatchGlow, state.glowColor);
        swatchOutline.create(hwnd, kIdSwatchOutline, state.outlineColor);
        toggleGlow.create(hwnd, kIdToggleGlow, state.glowOn);
        toggleOutline.create(hwnd, kIdToggleOutline, state.outlineOn);
        alphaSlider.create(hwnd, kIdAlphaSlider, state.unplayedAlphaPct);
        wchar_t buf[8];
        swprintf_s(buf, L"%d%%", state.unplayedAlphaPct);
        alphaValue.create(hwnd, kIdAlphaValue, buf);
        preview.create(hwnd, kIdPreview);
        preview.setState(state);
        okBtn.create(hwnd, kIdOk, L"确定", true);
        cancelBtn.create(hwnd, kIdCancel, L"取消", false);

        // 卡片只是设置区的背景表面，必须位于所有标签和控件下方，避免把内容冲淡。
        SetWindowPos(optionsCard.hwnd(), HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void layout() {
        RECT rc;
        GetClientRect(hwnd, &rc);
        float s = fluent::dipScale(GetDpiForWindow(hwnd));
        auto px = [&](float dip) { return static_cast<int>(dip * s); };
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int pad = px(fluent::metrics::pagePadding);
        int gap = px(fluent::metrics::controlGap);

        int titleH = px(28.0f);
        int subtitleH = px(20.0f);
        titleLabel.move(pad, pad, w - pad * 2, titleH);
        subtitleLabel.move(pad, pad + titleH, w - pad * 2, subtitleH);
        int optionsY = pad + titleH + subtitleH + px(fluent::metrics::sectionGap);
        int cardPad = px(16.0f);
        float optionsHDip = 16.0f + 5.0f * fluent::metrics::controlHeight +
                            4.0f * fluent::metrics::controlGap + 16.0f;
        int optionsH = px(optionsHDip);
        optionsCard.move(pad, optionsY, w - pad * 2, optionsH);

        int rowH = px(fluent::metrics::controlHeight);
        int rowGap = px(fluent::metrics::controlGap);
        int labelW = px(140), labelH = px(20);
        int swatchW = px(76), swatchH = px(26);
        int toggleW = px(ToggleSwitch::kWidth), toggleH = px(ToggleSwitch::kHeight);
        // 行序：0 未播放颜色 / 1 已播放颜色 / 2 未播放透明度 / 3 光晕 / 4 描边
        fluent::FluentLabel* labels[5] = {&labelUnplayed, &labelPlayed, &labelAlpha,
                                          &labelGlow, &labelOutline};
        ColorSwatch* swatches[5] = {&swatchUnplayed, &swatchPlayed, nullptr, &swatchGlow,
                                    &swatchOutline};
        for (int i = 0; i < 5; ++i) {
            int y = optionsY + cardPad + i * (rowH + rowGap);
            labels[i]->move(pad + cardPad, y + (rowH - labelH) / 2, labelW, labelH);
            if (swatches[i])
                swatches[i]->move(w - pad - cardPad - swatchW, y + (rowH - swatchH) / 2, swatchW,
                                  swatchH);
        }
        // 透明度行：滑块填满标签与百分比文本之间的区域
        int alphaRowY = optionsY + cardPad + 2 * (rowH + rowGap);
        int valueW = px(44);
        int sliderX = pad + cardPad + labelW + px(12);
        int valueX = w - pad - cardPad - valueW;
        int sliderW = valueX - px(12) - sliderX;
        alphaSlider.move(sliderX, alphaRowY, sliderW, rowH);
        alphaValue.move(valueX, alphaRowY + (rowH - labelH) / 2, valueW, labelH);
        // 光晕/描边行：开关放在色块左侧，色块位置与其他行保持对齐
        int glowRowY = optionsY + cardPad + 3 * (rowH + rowGap);
        int outlineRowY = optionsY + cardPad + 4 * (rowH + rowGap);
        int toggleX = w - pad - cardPad - swatchW - px(10) - toggleW;
        toggleGlow.move(toggleX, glowRowY + (rowH - toggleH) / 2, toggleW, toggleH);
        toggleOutline.move(toggleX, outlineRowY + (rowH - toggleH) / 2, toggleW, toggleH);
        int previewY = optionsY + optionsH + px(fluent::metrics::sectionGap);

        int btnH = px(fluent::metrics::controlHeight);
        int okW = px(96), cancelW = px(88);
        int btnY = h - pad - btnH;
        okBtn.move(w - pad - okW - cancelW - gap, btnY, okW, btnH);
        cancelBtn.move(w - pad - cancelW, btnY, cancelW, btnH);

        int previewH = btnY - px(fluent::metrics::sectionGap) - previewY;
        if (previewH > 0)
            preview.move(pad, previewY, w - pad * 2, previewH);
    }

    // 取色目标：色块控件 id -> 工作副本中的颜色
    COLORREF& colorRefOf(int swatchId) {
        switch (swatchId) {
        case kIdSwatchPlayed: return state.played;
        case kIdSwatchUnplayed: return state.unplayed;
        case kIdSwatchGlow: return state.glowColor;
        default: return state.outlineColor;
        }
    }

    ColorSwatch* swatchOf(int swatchId) {
        switch (swatchId) {
        case kIdSwatchPlayed: return &swatchPlayed;
        case kIdSwatchUnplayed: return &swatchUnplayed;
        case kIdSwatchGlow: return &swatchGlow;
        default: return &swatchOutline;
        }
    }

    const wchar_t* titleOf(int swatchId) {
        switch (swatchId) {
        case kIdSwatchPlayed: return L"已播放字体颜色";
        case kIdSwatchUnplayed: return L"未播放字体颜色";
        case kIdSwatchGlow: return L"光晕颜色";
        default: return L"描边颜色";
        }
    }

    void openPicker(int swatchId) {
        if (picker)
            picker->destroy();
        picker = std::make_unique<ColorPickerDialog>();
        if (!picker->create(inst, hwnd, colorRefOf(swatchId), titleOf(swatchId))) {
            picker.reset();
            return;
        }
        picker->setApplyCallback([this, swatchId](COLORREF c) {
            colorRefOf(swatchId) = c;
            swatchOf(swatchId)->setColor(c);
            preview.setState(state);
        });
        picker->show();
    }

    void onCommand(int id, int code) {
        if (code != BN_CLICKED)
            return;
        if (id == kIdCancel) {
            destroy();
        } else if (id == kIdOk) {
            if (onApply)
                onApply(Result{state.played, state.unplayed, state.unplayedAlphaPct,
                               state.glowColor, state.outlineColor, state.glowOn,
                               state.outlineOn});
            destroy();
        } else if (id == kIdAlphaSlider) {
            // 滑块取值已由控件自身更新，同步进工作副本并刷新预览
            state.unplayedAlphaPct = alphaSlider.value();
            wchar_t buf[8];
            swprintf_s(buf, L"%d%%", state.unplayedAlphaPct);
            alphaValue.setText(buf);
            preview.setState(state);
        } else if (id == kIdToggleGlow || id == kIdToggleOutline) {
            // 开关状态已由控件自身翻转，同步进工作副本并刷新预览
            state.glowOn = toggleGlow.isOn();
            state.outlineOn = toggleOutline.isOn();
            preview.setState(state);
        } else if (id >= kIdSwatchUnplayed && id <= kIdSwatchOutline) {
            openPicker(id);
        }
    }

    bool isDialogMessage(MSG* msg) {
        if (hwnd && IsDialogMessageW(hwnd, msg))
            return true;
        if (picker && picker->isOpen() && IsDialogMessageW(picker->hwnd(), msg))
            return true;
        return false;
    }

    void destroy() {
        if (picker)
            picker->destroy();
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

FontColorDialog::FontColorDialog() : impl_(std::make_unique<Impl>()) {}

FontColorDialog::~FontColorDialog() {
    destroy();
}

bool FontColorDialog::create(HINSTANCE inst, HWND parent, const State& initial) {
    (void)parent; // 托盘窗口是消息窗口，不能作为所有者；与其他对话框一致使用无所有者窗口
    impl_->inst = inst;
    impl_->state = initial;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricFontColor";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    // 期望的客户区尺寸（DIP），按标题栏等边框反推整个窗口尺寸，否则底部控件被裁掉
    float clientW = fluent::metrics::pagePadding + 340 + fluent::metrics::pagePadding;
    float optionsHDip = 16.0f + 5.0f * fluent::metrics::controlHeight +
                        4.0f * fluent::metrics::controlGap + 16.0f;
    float clientH = fluent::metrics::pagePadding + 28.0f + 20.0f +
                    fluent::metrics::sectionGap + optionsHDip + fluent::metrics::sectionGap + 96.0f +
                    fluent::metrics::sectionGap + fluent::metrics::controlHeight +
                    fluent::metrics::pagePadding;
    RECT rc{0, 0, static_cast<LONG>(std::lround(clientW * s)),
            static_cast<LONG>(std::lround(clientH * s))};
    AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE,
                             WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
                                  L"QQMusicLyricFontColor", L"字体颜色与效果",
                                  WS_CAPTION | WS_SYSMENU, x, y, w, h, nullptr, nullptr, inst,
                                  impl_.get());
    if (!impl_->hwnd)
        return false;
    return true;
}

void FontColorDialog::show() {
    if (impl_->hwnd)
        ShowWindow(impl_->hwnd, SW_SHOW);
}

void FontColorDialog::destroy() {
    impl_->destroy();
}

bool FontColorDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND FontColorDialog::hwnd() const {
    return impl_->hwnd;
}

bool FontColorDialog::isDialogMessage(MSG* msg) {
    return impl_->isDialogMessage(msg);
}

void FontColorDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}
