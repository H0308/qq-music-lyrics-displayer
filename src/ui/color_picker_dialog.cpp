#include "color_picker_dialog.h"

#include "ui/fluent_controls.h"
#include "ui/fluent_theme.h"
#include "resource.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kIdCanvas = 301;
constexpr int kIdHexLabel = 302;
constexpr int kIdHexEdit = 303;
constexpr int kIdOk = 304;
constexpr int kIdCancel = 305;
constexpr int kIdTitleLabel = 306;
constexpr int kIdSubtitleLabel = 307;

// HSV(h: 0-360, s/v: 0-1) <-> RGB
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    r = rr + m;
    g = gg + m;
    b = bb + m;
}

void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r, g, b});
    float mn = std::min({r, g, b});
    float d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) {
        h = 0;
    } else if (mx == r) {
        h = 60.0f * std::fmod((g - b) / d, 6.0f);
    } else if (mx == g) {
        h = 60.0f * ((b - r) / d + 2.0f);
    } else {
        h = 60.0f * ((r - g) / d + 4.0f);
    }
    if (h < 0)
        h += 360.0f;
}

COLORREF hsvToColorRef(float h, float s, float v) {
    float r, g, b;
    hsvToRgb(h, s, v, r, g, b);
    return RGB(static_cast<BYTE>(r * 255.0f), static_cast<BYTE>(g * 255.0f),
               static_cast<BYTE>(b * 255.0f));
}

bool parseHex(const std::wstring& t, COLORREF& out) {
    std::wstring s = t;
    if (!s.empty() && s[0] == L'#')
        s.erase(s.begin());
    if (s.size() != 6)
        return false;
    unsigned value = 0;
    for (wchar_t c : s) {
        value <<= 4;
        if (c >= L'0' && c <= L'9')
            value |= c - L'0';
        else if (c >= L'a' && c <= L'f')
            value |= c - L'a' + 10;
        else if (c >= L'A' && c <= L'F')
            value |= c - L'A' + 10;
        else
            return false;
    }
    out = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

// 取色画布：左侧 SV 方块 + 右侧色相条 + 底部新旧色对比条
class ColorCanvas : public fluent::LayeredChild {
public:
    static constexpr float kSvSize = 232.0f;
    static constexpr float kHueW = 18.0f;
    static constexpr float kGap = 12.0f;
    static constexpr float kSwatchH = 28.0f;

    std::function<void()> onChanged;

    bool create(HWND parent, int id) {
        clearAlpha_ = 1.0f / 255.0f; // 整个画布可拖拽（防 alpha=0 穿透）
        return createLayered(parent, L"QQMusicLyricColorCanvas", wndProc, id, true, true);
    }

    void setColor(COLORREF c) {
        float h, s, v;
        rgbToHsv(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, h, s, v);
        hue_ = h;
        sat_ = s;
        val_ = v;
        renderNow();
    }

    void setOldColor(COLORREF c) {
        old_ = c;
    }

    COLORREF color() const { return hsvToColorRef(hue_, sat_, val_); }

private:
    // 布局（DIP）：SV [0, kSvSize]，色相 [kSvSize+kGap, +kHueW]，对比条 [0, kSvSize+kGap+kHueW] 底部
    D2D1_RECT_F svRect() const { return D2D1::RectF(0, 0, kSvSize, kSvSize); }
    D2D1_RECT_F hueRect() const {
        return D2D1::RectF(kSvSize + kGap, 0, kSvSize + kGap + kHueW, kSvSize);
    }

    void render(ID2D1DCRenderTarget* rt, float wDip, float hDip) override {
        auto* br = brush(rt);
        if (!br)
            return;

        // SV 方块：白 -> 纯色（横向），透明 -> 黑（纵向）
        float hr, hg, hb;
        hsvToRgb(hue_, 1.0f, 1.0f, hr, hg, hb);
        D2D1_COLOR_F pure = D2D1::ColorF(hr, hg, hb, 1.0f);
        ID2D1LinearGradientBrush* g1 = nullptr;
        ID2D1GradientStopCollection* stops = nullptr;
        D2D1_GRADIENT_STOP gs1[2] = {{0.0f, D2D1::ColorF(1, 1, 1, 1)}, {1.0f, pure}};
        rt->CreateGradientStopCollection(gs1, 2, &stops);
        if (stops) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(kSvSize, 0)),
                stops, &g1);
            stops->Release();
            stops = nullptr;
        }
        if (g1) {
            rt->FillRectangle(svRect(), g1);
            g1->Release();
        }
        D2D1_GRADIENT_STOP gs2[2] = {{0.0f, D2D1::ColorF(0, 0, 0, 0)}, {1.0f, D2D1::ColorF(0, 0, 0, 1)}};
        rt->CreateGradientStopCollection(gs2, 2, &stops);
        ID2D1LinearGradientBrush* g2 = nullptr;
        if (stops) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kSvSize)),
                stops, &g2);
            stops->Release();
        }
        if (g2) {
            rt->FillRectangle(svRect(), g2);
            g2->Release();
        }

        // 色相条：7 段彩虹渐变
        ID2D1LinearGradientBrush* gh = nullptr;
        D2D1_GRADIENT_STOP gsh[7];
        for (int i = 0; i < 7; ++i) {
            float r, g, b;
            hsvToRgb(i * 60.0f, 1.0f, 1.0f, r, g, b);
            gsh[i].position = i / 6.0f;
            gsh[i].color = D2D1::ColorF(r, g, b, 1.0f);
        }
        rt->CreateGradientStopCollection(gsh, 7, &stops);
        D2D1_RECT_F hrct = hueRect();
        if (stops) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(hrct.left, hrct.top),
                                                    D2D1::Point2F(hrct.left, hrct.bottom)),
                stops, &gh);
            stops->Release();
        }
        if (gh) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(hrct, 3.0f, 3.0f), gh);
            gh->Release();
        }

        // SV 十字准星
        float px = sat_ * kSvSize;
        float py = (1.0f - val_) * kSvSize;
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.7f));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 7.5f, 7.5f), br, 2.5f);
        br->SetColor(D2D1::ColorF(1, 1, 1, 1.0f));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 6.0f, 6.0f), br, 1.5f);

        // 色相指示条
        float hy = hue_ / 360.0f * kSvSize;
        br->SetColor(D2D1::ColorF(1, 1, 1, 1.0f));
        rt->DrawRectangle(D2D1::RectF(hrct.left - 2.0f, hy - 2.0f, hrct.right + 2.0f, hy + 2.0f),
                          br, 1.5f);

        // 底部：旧色 | 新色 对比
        float swY = kSvSize + kGap;
        D2D1_RECT_F oldR = D2D1::RectF(0, swY, kSvSize / 2.0f, swY + kSwatchH);
        D2D1_RECT_F newR = D2D1::RectF(kSvSize / 2.0f, swY, kSvSize + kGap + kHueW, swY + kSwatchH);
        br->SetColor(fluent::toD2D(old_));
        rt->FillRoundedRectangle(D2D1::RoundedRect(oldR, 4.0f, 4.0f), br);
        br->SetColor(fluent::toD2D(color()));
        rt->FillRoundedRectangle(D2D1::RoundedRect(newR, 4.0f, 4.0f), br);

        // 文字：旧 / 新（按色块亮度选黑/白字保证可读）
        if (auto* fmt = textFormat(12.0f)) {
            auto lumText = [](COLORREF c) {
                float lum = 0.299f * GetRValue(c) + 0.587f * GetGValue(c) + 0.114f * GetBValue(c);
                return lum > 140.0f ? D2D1::ColorF(0, 0, 0, 0.8f) : D2D1::ColorF(1, 1, 1, 0.9f);
            };
            D2D1_RECT_F oldText = D2D1::RectF(oldR.left + 8, oldR.top, oldR.right, oldR.bottom);
            D2D1_RECT_F newText = D2D1::RectF(newR.left + 8, newR.top, newR.right, newR.bottom);
            br->SetColor(lumText(old_));
            rt->DrawTextW(L"当前", 2, fmt, oldText, br);
            br->SetColor(lumText(color()));
            rt->DrawTextW(L"新颜色", 3, fmt, newText, br);
        }
        if (focused_) {
            br->SetColor(fluent::palette().accent);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, wDip - 1.5f, hDip - 1.5f),
                                  fluent::metrics::cardRadius, fluent::metrics::cardRadius),
                br, 1.5f);
        }
    }

    void onMouse(UINT msg, WPARAM wp, LPARAM lp) {
        float s = fluent::dipScale(GetDpiForWindow(hwnd_));
        float x = GET_X_LPARAM(lp) / s;
        float y = GET_Y_LPARAM(lp) / s;
        if (msg == WM_LBUTTONDOWN) {
            SetFocus(hwnd_);
            SetCapture(hwnd_);
            if (x >= hueRect().left - 4 && x <= hueRect().right + 4 && y >= 0 && y <= kSvSize)
                dragTarget_ = 2;
            else if (x >= 0 && x <= kSvSize && y >= 0 && y <= kSvSize)
                dragTarget_ = 1;
            else
                dragTarget_ = 0;
        }
        if (dragTarget_ == 0)
            return;
        if (msg == WM_LBUTTONUP) {
            dragTarget_ = 0;
            ReleaseCapture();
            return;
        }
        if (dragTarget_ == 1) {
            sat_ = std::clamp(x / kSvSize, 0.0f, 1.0f);
            val_ = std::clamp(1.0f - y / kSvSize, 0.0f, 1.0f);
        } else {
            hue_ = std::clamp(y / kSvSize, 0.0f, 1.0f) * 360.0f;
        }
        renderNow();
        if (onChanged)
            onChanged();
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        ColorCanvas* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<ColorCanvas*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ColorCanvas*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_NCHITTEST:
            // 分层窗口按像素 alpha 命中测试，透明区域会穿透；整个画布都要可点
            return HTCLIENT;
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
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE:
            if (msg != WM_MOUSEMOVE || self->dragTarget_ != 0)
                self->onMouse(msg, wp, lp);
            return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    float hue_ = 0, sat_ = 0, val_ = 1;
    COLORREF old_ = 0;
    int dragTarget_ = 0; // 1=SV, 2=Hue
    bool focused_ = false;
};

} // namespace

struct ColorPickerDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool backdrop = false;

    fluent::FluentLabel titleLabel;
    fluent::FluentLabel subtitleLabel;
    ColorCanvas canvas;
    fluent::FluentLabel hexLabel;
    fluent::FluentEdit hexEdit;
    fluent::FluentButton okBtn;
    fluent::FluentButton cancelBtn;

    COLORREF initial = 0;
    std::wstring titleText = L"选择颜色";
    bool updatingHex = false; // 防止画布 -> 输入框 -> 画布循环
    ApplyCallback onApply;

    void refreshTheme() {
        titleLabel.refreshTheme();
        subtitleLabel.refreshTheme();
        canvas.refreshTheme();
        hexLabel.refreshTheme();
        hexEdit.refreshTheme();
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
            backdrop = fluent::styleDialogWindow(hwnd, true);
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop, true);
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
        titleLabel.create(hwnd, kIdTitleLabel, titleText.c_str(), false, 20.0f, 600);
        subtitleLabel.create(hwnd, kIdSubtitleLabel, L"调整颜色后点击确定应用", true, 13.0f, 400);
        canvas.create(hwnd, kIdCanvas);
        canvas.setOldColor(initial);
        canvas.setColor(initial);
        canvas.onChanged = [this] { syncHexFromCanvas(); };

        hexLabel.create(hwnd, kIdHexLabel, L"HEX", true);
        hexEdit.create(hwnd, kIdHexEdit, L"#RRGGBB");
        okBtn.create(hwnd, kIdOk, L"确定", true);
        cancelBtn.create(hwnd, kIdCancel, L"取消", false);
        syncHexFromCanvas();
    }

    void syncHexFromCanvas() {
        if (updatingHex)
            return;
        updatingHex = true;
        COLORREF c = canvas.color();
        wchar_t buf[8];
        swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        hexEdit.setText(buf);
        updatingHex = false;
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

        int canvasW = px(ColorCanvas::kSvSize + ColorCanvas::kGap + ColorCanvas::kHueW);
        int canvasH = px(ColorCanvas::kSvSize + ColorCanvas::kGap + ColorCanvas::kSwatchH);
        int canvasY = pad + titleH + subtitleH + px(fluent::metrics::sectionGap);
        canvas.move(pad, canvasY, canvasW, canvasH);

        int hexY = canvasY + canvasH + gap;
        int labelW = px(36);
        int editH = px(fluent::metrics::controlHeight);
        hexLabel.move(pad, hexY + (editH - px(20)) / 2, labelW, px(20));
        hexEdit.move(pad + labelW + px(4), hexY, w - pad * 2 - labelW - px(4), editH);

        int btnH = px(fluent::metrics::controlHeight);
        int okW = px(96), cancelW = px(88);
        int btnY = h - pad - btnH;
        okBtn.move(w - pad - okW - cancelW - gap, btnY, okW, btnH);
        cancelBtn.move(w - pad - cancelW, btnY, cancelW, btnH);
    }

    void onCommand(int id, int code) {
        if (id == kIdCancel && code == BN_CLICKED) {
            destroy();
        } else if (id == kIdOk && code == BN_CLICKED) {
            if (onApply)
                onApply(canvas.color());
            destroy();
        } else if (id == kIdHexEdit && code == EN_CHANGE) {
            if (updatingHex)
                return;
            COLORREF c;
            if (parseHex(hexEdit.text(), c))
                canvas.setColor(c);
        }
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

ColorPickerDialog::ColorPickerDialog() : impl_(std::make_unique<Impl>()) {}

ColorPickerDialog::~ColorPickerDialog() {
    destroy();
}

bool ColorPickerDialog::create(HINSTANCE inst, HWND parent, COLORREF initial,
                               const wchar_t* title, int cascadeIndex) {
    (void)parent; // 托盘窗口是消息窗口，不能作为所有者；与搜索对话框一致使用无所有者窗口
    impl_->inst = inst;
    impl_->initial = initial;
    impl_->titleText = title ? title : L"选择颜色";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricColorPicker";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    // 期望的客户区尺寸（DIP），按标题栏等边框反推整个窗口尺寸，否则底部控件被裁掉
    float clientW = fluent::metrics::pagePadding + ColorCanvas::kSvSize + ColorCanvas::kGap +
                    ColorCanvas::kHueW + fluent::metrics::pagePadding;
    float clientH = fluent::metrics::pagePadding + 28.0f + 20.0f + fluent::metrics::sectionGap +
                    ColorCanvas::kSvSize + ColorCanvas::kGap + ColorCanvas::kSwatchH +
                    fluent::metrics::controlGap +
                    fluent::metrics::controlHeight + fluent::metrics::controlGap +
                    fluent::metrics::controlHeight + fluent::metrics::pagePadding;
    RECT rc{0, 0, static_cast<LONG>(std::lround(clientW * s)),
            static_cast<LONG>(std::lround(clientH * s))};
    AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE,
                             WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    // 同时打开多个时级联偏移，避免完全重叠
    x += static_cast<int>(cascadeIndex * 28 * s);
    y += static_cast<int>(cascadeIndex * 28 * s);

    impl_->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
                                  L"QQMusicLyricColorPicker", title ? title : L"选择颜色",
                                  WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, nullptr,
                                  nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;
    return true;
}

void ColorPickerDialog::show() {
    if (impl_->hwnd)
        ShowWindow(impl_->hwnd, SW_SHOW);
}

void ColorPickerDialog::destroy() {
    impl_->destroy();
}

bool ColorPickerDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND ColorPickerDialog::hwnd() const {
    return impl_->hwnd;
}

void ColorPickerDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}
