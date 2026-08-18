#include "fluent_controls.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>

namespace fluent {

namespace {

COLORREF toColorRef(const D2D1_COLOR_F& c) {
    return RGB(static_cast<BYTE>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f),
               static_cast<BYTE>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
               static_cast<BYTE>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
}

void registerOnce(const wchar_t* className, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc); // 已注册则失败，忽略
}

template <typename T>
T* selfFromMsg(HWND h, UINT msg, LPARAM lp) {
    T* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<T*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<T*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self;
}

constexpr float kCornerRadius = metrics::controlRadius;

void fillRoundRect(ID2D1DCRenderTarget* rt, ID2D1SolidColorBrush* brush, D2D1_COLOR_F color,
                   const D2D1_RECT_F& rect, float radius = kCornerRadius) {
    brush->SetColor(color);
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

void strokeRoundRect(ID2D1DCRenderTarget* rt, ID2D1SolidColorBrush* brush, D2D1_COLOR_F color,
                     const D2D1_RECT_F& rect, float width = 1.0f,
                     float radius = kCornerRadius) {
    brush->SetColor(color);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, width);
}

} // namespace

// ---------------- LayeredChild ----------------

LayeredChild::~LayeredChild() {
    if (fmt_) {
        fmt_->Release();
        fmt_ = nullptr;
    }
    if (brush_) {
        brush_->Release();
        brush_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool LayeredChild::createLayered(HWND parent, const wchar_t* className, WNDPROC proc, int id,
                                 bool layered, bool tabStop) {
    registerOnce(className, proc);
    layered_ = layered;
    DWORD style = WS_CHILD;
    if (tabStop)
        style |= WS_TABSTOP;
    hwnd_ = CreateWindowExW(layered ? WS_EX_LAYERED : 0, className, L"", style, 0, 0, 10, 10,
                            parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                            GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;
}

void LayeredChild::move(int x, int y, int w, int h) {
    if (!hwnd_)
        return;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    renderNow();
}

ID2D1DCRenderTarget* LayeredChild::beginFrame(float* wDip, float* hDip) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return nullptr;
    if (!renderer_.initialize())
        return nullptr;
    UINT dpi = GetDpiForWindow(hwnd_);
    renderer_.setDpi(dpi);
    if (!renderer_.bindDC(w, h))
        return nullptr;
    auto* rt = renderer_.renderTarget();
    if (!rt)
        return nullptr;
    float s = dipScale(dpi);
    *wDip = w / s;
    *hDip = h / s;
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, clearAlpha_));
    return rt;
}

void LayeredChild::endFrame() {
    auto* rt = renderer_.renderTarget();
    if (!rt)
        return;
    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderer_.discard();
    } else if (layered_) {
        renderer_.present(hwnd_);
    } else {
        // 普通子窗口：BitBlt 提交（内容必须是不透明的）
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HDC hdc = GetDC(hwnd_);
        renderer_.copyToDC(hdc, rc.right - rc.left, rc.bottom - rc.top);
        ReleaseDC(hwnd_, hdc);
    }
}

ID2D1SolidColorBrush* LayeredChild::brush(ID2D1DCRenderTarget* rt) {
    if (!brush_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
    return brush_;
}

IDWriteFactory* LayeredChild::dwrite() {
    return renderer_.dwrite();
}

IDWriteTextFormat* LayeredChild::textFormat(float dipSize, int weight, bool center) {
    if (fmt_ && fmtSize_ == dipSize && fmtWeight_ == weight && fmtCenter_ == center)
        return fmt_;
    if (fmt_) {
        fmt_->Release();
        fmt_ = nullptr;
    }
    IDWriteFactory* dw = renderer_.dwrite();
    if (!dw)
        return nullptr;
    HRESULT hr = dw->CreateTextFormat(uiFontFamily(), nullptr,
                                      static_cast<DWRITE_FONT_WEIGHT>(weight),
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      dipSize, L"zh-cn", &fmt_);
    if (FAILED(hr) || !fmt_)
        return nullptr;
    fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (center)
        fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtSize_ = dipSize;
    fmtWeight_ = weight;
    fmtCenter_ = center;
    return fmt_;
}

void LayeredChild::renderNow() {
    // 画刷绑定在渲染目标上，设备丢失后可能失效，每帧重建
    if (brush_) {
        brush_->Release();
        brush_ = nullptr;
    }
    float w = 0, h = 0;
    if (auto* rt = beginFrame(&w, &h)) {
        render(rt, w, h);
        endFrame();
    }
}

// ---------------- FluentButton ----------------

bool FluentButton::create(HWND parent, int id, const wchar_t* text, bool accent) {
    id_ = id;
    text_ = text ? text : L"";
    accent_ = accent;
    clearAlpha_ = 1.0f / 255.0f; // 整个按钮面可点击（防 alpha=0 穿透）
    return createLayered(parent, L"QQMusicLyricFluentButton", wndProc, id, true, true);
}

void FluentButton::setAccent(bool accent) {
    accent_ = accent;
    renderNow();
}

void FluentButton::setEnabled(bool enabled) {
    EnableWindow(hwnd_, enabled ? TRUE : FALSE);
    renderNow();
}

LRESULT CALLBACK FluentButton::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    FluentButton* self = selfFromMsg<FluentButton>(h, msg, lp);
    if (msg == WM_NCCREATE)
        self->hwnd_ = h;
    if (self)
        return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT FluentButton::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        // 分层窗口按像素 alpha 命中测试，透明区域会穿透；按钮整个矩形都要可点
        return HTCLIENT;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        renderNow();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (!hover_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            hover_ = true;
            renderNow();
        }
        return 0;
    case WM_MOUSELEAVE:
        hover_ = false;
        renderNow();
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowEnabled(hwnd_))
            return 0;
        SetCapture(hwnd_);
        SetFocus(hwnd_);
        pressed_ = true;
        renderNow();
        return 0;
    case WM_LBUTTONUP:
        if (pressed_) {
            pressed_ = false;
            ReleaseCapture();
            renderNow();
            RECT rc;
            GetClientRect(hwnd_, &rc);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&rc, pt) && IsWindowEnabled(hwnd_)) {
                SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, BN_CLICKED),
                             reinterpret_cast<LPARAM>(hwnd_));
            }
        }
        return 0;
    case WM_KEYDOWN:
        if ((wp == VK_SPACE || wp == VK_RETURN) && IsWindowEnabled(hwnd_)) {
            SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, BN_CLICKED),
                         reinterpret_cast<LPARAM>(hwnd_));
        }
        return 0;
    case WM_ENABLE:
        renderNow();
        return 0;
    case WM_SETFOCUS:
        focused_ = true;
        renderNow();
        return 0;
    case WM_KILLFOCUS:
        focused_ = false;
        renderNow();
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void FluentButton::render(ID2D1DCRenderTarget* rt, float wDip, float hDip) {
    const Palette& p = palette();
    bool enabled = IsWindowEnabled(hwnd_) != FALSE;
    D2D1_RECT_F rect = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
    auto* br = brush(rt);
    if (!br)
        return;

    D2D1_COLOR_F fill;
    D2D1_COLOR_F textColor;
    if (!enabled) {
        fill = p.listHover;
        textColor = p.disabled;
    } else if (accent_) {
        fill = pressed_ ? p.accentPressed : (hover_ ? p.accentHover : p.accent);
        textColor = p.textOnAccent;
    } else {
        fill = pressed_ ? p.controlPressed : (hover_ ? p.controlHover : p.controlFill);
        textColor = p.text;
    }
    fillRoundRect(rt, br, fill, rect);
    if (!accent_ || !enabled)
        strokeRoundRect(rt, br, p.cardStroke, rect);

    if (focused_ && enabled) {
        D2D1_COLOR_F focusColor = accent_ ? p.textOnAccent : p.accent;
        strokeRoundRect(rt, br, focusColor,
                        D2D1::RectF(1.5f, 1.5f, wDip - 1.5f, hDip - 1.5f), 1.5f,
                        std::max(1.0f, kCornerRadius - 1.0f));
    }

    if (auto* fmt = textFormat(14.0f, 400, true)) {
        br->SetColor(textColor);
        rt->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), fmt,
                      D2D1::RectF(4.0f, 0.0f, wDip - 4.0f, hDip), br);
    }
}

// ---------------- FluentEdit ----------------

bool FluentEdit::create(HWND parent, int id, const wchar_t* cueBanner) {
    id_ = id;
    // 非分层窗口：分层窗口内的真控件（EDIT）不会被系统绘制
    if (!createLayered(parent, L"QQMusicLyricFluentEdit", wndProc, id, false))
        return false;
    hEdit_ = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(1)),
                             GetModuleHandleW(nullptr), nullptr);
    if (!hEdit_)
        return false;
    editFont_ = createUiFont(GetDpiForWindow(hwnd_));
    SendMessageW(hEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    if (cueBanner)
        SendMessageW(hEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cueBanner));
    return true;
}

std::wstring FluentEdit::text() const {
    int len = GetWindowTextLengthW(hEdit_);
    if (len <= 0)
        return {};
    std::wstring s(len, L'\0');
    GetWindowTextW(hEdit_, s.data(), len + 1);
    s.resize(len);
    return s;
}

void FluentEdit::setText(const std::wstring& text) {
    SetWindowTextW(hEdit_, text.c_str());
}

void FluentEdit::focus() {
    SetFocus(hEdit_);
}

LRESULT CALLBACK FluentEdit::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    FluentEdit* self = selfFromMsg<FluentEdit>(h, msg, lp);
    if (msg == WM_NCCREATE)
        self->hwnd_ = h;
    if (self)
        return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT FluentEdit::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        renderNow();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layoutEdit();
        renderNow();
        return 0;
    case WM_SETFOCUS:
        SetFocus(hEdit_);
        return 0;
    case WM_LBUTTONDOWN:
        // 点击卡片内边距区域也把焦点交给内嵌 EDIT
        SetFocus(hEdit_);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == hEdit_) {
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                focused_ = HIWORD(wp) == EN_SETFOCUS;
                renderNow();
            }
            // 原样转发 EDIT 通知（EN_CHANGE 等）给对话框窗口
            SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, HIWORD(wp)),
                         reinterpret_cast<LPARAM>(hwnd_));
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        const Palette& p = palette();
        // EDIT 背景与卡片同色，视觉上融为一体
        COLORREF bg = toColorRef(p.cardFillSolid);
        if (!editBrush_ || editBrushColor_ != bg) {
            if (editBrush_)
                DeleteObject(editBrush_);
            editBrush_ = CreateSolidBrush(bg);
            editBrushColor_ = bg;
        }
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, toColorRef(p.editText));
        SetBkColor(hdc, bg);
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_DESTROY:
        if (editFont_) {
            DeleteObject(editFont_);
            editFont_ = nullptr;
        }
        if (editBrush_) {
            DeleteObject(editBrush_);
            editBrush_ = nullptr;
        }
        hEdit_ = nullptr;
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void FluentEdit::layoutEdit() {
    if (!hEdit_)
        return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float s = dipScale(GetDpiForWindow(hwnd_));
    int padX = static_cast<int>(12 * s);
    int padY = static_cast<int>(5 * s);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    SetWindowPos(hEdit_, nullptr, padX, padY, w - padX * 2, h - padY * 2 - static_cast<int>(2 * s),
                 SWP_NOZORDER);
}

void FluentEdit::render(ID2D1DCRenderTarget* rt, float wDip, float hDip) {
    const Palette& p = palette();
    auto* br = brush(rt);
    if (!br)
        return;
    // 不透明提交：先用 Mica 底色铺满（圆角外的角落与对话框背景近似一致）
    D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, wDip, hDip);
    br->SetColor(p.windowBg);
    rt->FillRectangle(full, br);
    D2D1_RECT_F rect = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
    fillRoundRect(rt, br, p.cardFillSolid, rect);
    strokeRoundRect(rt, br, p.cardStroke, rect);
    // Win11 输入框底边线：静止时细灰线，聚焦时强调色粗线
    D2D1_RECT_F bottom = D2D1::RectF(rect.left + 1.0f, rect.bottom - (focused_ ? 2.0f : 1.0f),
                                     rect.right - 1.0f, rect.bottom);
    br->SetColor(focused_ ? p.accent : p.separator);
    rt->FillRectangle(bottom, br);
}

// ---------------- FluentList ----------------

namespace {
constexpr float kRowH = 32.0f;
constexpr float kHeaderH = 28.0f;
constexpr float kScrollBarW = 3.0f;
constexpr float kScrollBarHitW = 12.0f;
constexpr UINT_PTR kTipTimerId = 1;
constexpr UINT kTipDelayMs = 400; // 悬浮多久后弹出 tooltip

// 单行绘制文本，超宽时按字符裁剪并加省略号（不换行，避免溢出到相邻行）
void drawTrimmedText(IDWriteFactory* dw, ID2D1DCRenderTarget* rt, const std::wstring& text,
                     IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, ID2D1Brush* br) {
    if (!dw || !fmt)
        return;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                    rect.right - rect.left, rect.bottom - rect.top, &layout)) ||
        !layout)
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trimming, nullptr); // nullptr = 默认省略号
    rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, br);
    layout->Release();
}
} // namespace

bool FluentList::create(HWND parent, int id) {
    id_ = id;
    clearAlpha_ = 1.0f / 255.0f; // 整行（含文字旁空白）可点选（防 alpha=0 穿透）
    return createLayered(parent, L"QQMusicLyricFluentList", wndProc, id, true, true);
}

void FluentList::setItems(std::vector<FluentListItem> items) {
    hideTip();
    items_ = std::move(items);
    selected_ = -1;
    hover_ = -1;
    scrollY_ = 0;
    wheelAccum_ = 0;
    renderNow();
}

void FluentList::clear() {
    setItems({});
}

void FluentList::setSelectedIndex(int idx) {
    if (idx < -1 || idx >= itemCount())
        return;
    if (idx >= 0 && items_[idx].header)
        return;
    if (selected_ == idx)
        return;
    selected_ = idx;
    if (idx >= 0)
        ensureVisible(idx);
    renderNow();
}

float FluentList::rowHeight(int row) const {
    return items_[row].header ? kHeaderH : kRowH;
}

float FluentList::contentHeight() const {
    float h = 0;
    for (int i = 0; i < itemCount(); ++i)
        h += rowHeight(i);
    return h;
}

int FluentList::rowAt(float yDip) const {
    float y = yDip + scrollY_;
    float acc = 0;
    for (int i = 0; i < itemCount(); ++i) {
        acc += rowHeight(i);
        if (y < acc)
            return i;
    }
    return -1;
}

void FluentList::ensureVisible(int row) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float s = dipScale(GetDpiForWindow(hwnd_));
    float viewH = (rc.bottom - rc.top) / s;
    float top = 0;
    for (int i = 0; i < row; ++i)
        top += rowHeight(i);
    float bottom = top + rowHeight(row);
    if (top < scrollY_)
        scrollY_ = top;
    else if (bottom > scrollY_ + viewH)
        scrollY_ = bottom - viewH;
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight() - viewH));
}

void FluentList::notifySelChange() {
    SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, LBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(hwnd_));
}

int FluentList::nextSelectable(int from, int dir) const {
    int i = from;
    while (true) {
        i += dir;
        if (i < 0 || i >= itemCount())
            return -1;
        if (!items_[i].header)
            return i;
    }
}

bool FluentList::rowTextTruncated(int row) {
    IDWriteFactory* dw = dwrite();
    auto* fmt = items_[row].header ? textFormat(12.0f, 600) : textFormat(13.0f);
    if (!dw || !fmt)
        return false;
    const std::wstring& text = items_[row].text;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                    100000.0f, 100.0f, &layout)) ||
        !layout)
        return false;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float wDip = (rc.right - rc.left) / dipScale(GetDpiForWindow(hwnd_));
    float avail = wDip - (items_[row].header ? 24.0f : 28.0f); // 与 render 的行内边距一致
    return m.width > avail;
}

void FluentList::showTip(int row) {
    if (!tooltip_) {
        tooltip_ = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   hwnd_, nullptr, nullptr, nullptr);
        if (!tooltip_)
            return;
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = hwnd_;
        ti.uId = 1;
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float s = dipScale(GetDpiForWindow(hwnd_));
    // 宽度上限取列表自身宽度，超长文本在 tooltip 内折行而不是横贯整个屏幕
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, rc.right - rc.left);

    TOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = hwnd_;
    ti.uId = 1;
    ti.lpszText = const_cast<wchar_t*>(items_[row].text.c_str());
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));

    POINT pt;
    GetCursorPos(&pt);
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0,
                 static_cast<LPARAM>(MAKELPARAM(pt.x + static_cast<int>(12 * s),
                                                pt.y + static_cast<int>(18 * s))));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    tipRow_ = row;
}

void FluentList::hideTip() {
    if (tipArmed_) {
        KillTimer(hwnd_, kTipTimerId);
        tipArmed_ = false;
    }
    if (tipRow_ >= 0 && tooltip_) {
        TOOLINFOW ti{sizeof(ti)};
        ti.hwnd = hwnd_;
        ti.uId = 1;
        SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    }
    tipRow_ = -1;
}

LRESULT CALLBACK FluentList::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    FluentList* self = selfFromMsg<FluentList>(h, msg, lp);
    if (msg == WM_NCCREATE)
        self->hwnd_ = h;
    if (self)
        return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT FluentList::handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto viewDipH = [&] {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return (rc.bottom - rc.top) / dipScale(GetDpiForWindow(hwnd_));
    };
    auto maxScroll = [&] { return std::max(0.0f, contentHeight() - viewDipH()); };
    auto dipPoint = [&](LPARAM l, float& x, float& y) {
        float s = dipScale(GetDpiForWindow(hwnd_));
        x = GET_X_LPARAM(l) / s;
        y = GET_Y_LPARAM(l) / s;
    };

    switch (msg) {
    case WM_NCHITTEST:
        // 分层窗口按像素 alpha 命中测试，透明区域会穿透；列表整行都要可点
        return HTCLIENT;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        renderNow();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFOCUS:
        focused_ = true;
        renderNow();
        return 0;
    case WM_KILLFOCUS:
        focused_ = false;
        renderNow();
        return 0;
    case WM_MOUSEWHEEL: {
        hideTip();
        wheelAccum_ += GET_WHEEL_DELTA_WPARAM(wp);
        int notch = wheelAccum_ / WHEEL_DELTA;
        if (notch == 0)
            return 0;
        wheelAccum_ -= notch * WHEEL_DELTA;
        scrollY_ = std::clamp(scrollY_ - notch * 3.0f * kRowH, 0.0f, maxScroll());
        renderNow();
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x, y;
        dipPoint(lp, x, y);
        if (scrollDrag_) {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            float vh = viewDipH();
            float ch = contentHeight();
            float thumbH = std::max(20.0f, vh * vh / ch);
            float usable = vh - thumbH;
            if (usable > 0)
                scrollY_ = std::clamp((y - scrollDragGrabDy_) / usable * maxScroll(), 0.0f,
                                      maxScroll());
            renderNow();
            return 0;
        }
        int row = rowAt(y);
        if (hover_ == -1) {
            // 鼠标刚从窗外进入，武装 LEAVE 跟踪
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
        }
        if (row != hover_) {
            // 悬浮行变化：收起 tooltip，非标题行重新武装弹出计时
            hideTip();
            hover_ = row;
            renderNow();
            if (row >= 0 && !items_[row].header) {
                SetTimer(hwnd_, kTipTimerId, kTipDelayMs, nullptr);
                tipArmed_ = true;
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTipTimerId) {
            KillTimer(hwnd_, kTipTimerId);
            tipArmed_ = false;
            if (hover_ >= 0 && hover_ < itemCount() && !items_[hover_].header &&
                rowTextTruncated(hover_))
                showTip(hover_);
        }
        return 0;
    case WM_MOUSELEAVE:
        hideTip();
        if (!scrollDrag_) {
            hover_ = -1;
            renderNow();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        hideTip();
        SetFocus(hwnd_);
        float x, y;
        dipPoint(lp, x, y);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        float vh = viewDipH();
        float wDip = (rc.right - rc.left) / dipScale(GetDpiForWindow(hwnd_));
        // 命中滚动条？
        if (contentHeight() > vh && x >= wDip - kScrollBarHitW) {
            float thumbH = std::max(20.0f, vh * vh / contentHeight());
            float usable = vh - thumbH;
            float thumbY = maxScroll() > 0 ? scrollY_ / maxScroll() * usable : 0;
            if (y >= thumbY && y <= thumbY + thumbH) {
                scrollDrag_ = true;
                scrollDragGrabDy_ = y - thumbY;
            } else {
                // 点击空白轨道：翻页
                scrollY_ = std::clamp(scrollY_ + (y < thumbY ? -vh : vh), 0.0f, maxScroll());
                renderNow();
            }
            SetCapture(hwnd_);
            return 0;
        }
        int row = rowAt(y);
        if (row >= 0 && !items_[row].header) {
            selected_ = row;
            renderNow();
            notifySelChange();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (scrollDrag_) {
            scrollDrag_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        float x, y;
        dipPoint(lp, x, y);
        int row = rowAt(y);
        if (row >= 0 && !items_[row].header) {
            SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, LBN_DBLCLK),
                         reinterpret_cast<LPARAM>(hwnd_));
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int target = -1;
        if (wp == VK_DOWN)
            target = selected_ < 0 ? nextSelectable(-1, 1) : nextSelectable(selected_, 1);
        else if (wp == VK_UP)
            target = selected_ < 0 ? nextSelectable(itemCount(), -1) : nextSelectable(selected_, -1);
        else if (wp == VK_PRIOR || wp == VK_NEXT) {
            int dir = wp == VK_NEXT ? 1 : -1;
            int page = std::max(1, static_cast<int>(viewDipH() / kRowH) - 1);
            int t = selected_ < 0 ? (dir > 0 ? -1 : itemCount()) : selected_;
            for (int i = 0; i < page; ++i) {
                int n = nextSelectable(t, dir);
                if (n < 0)
                    break;
                t = n;
            }
            target = t == selected_ ? -1 : t;
        }
        if (target >= 0) {
            selected_ = target;
            ensureVisible(target);
            renderNow();
            notifySelChange();
        }
        return 0;
    }
    case WM_DESTROY:
        hideTip();
        if (tooltip_) {
            DestroyWindow(tooltip_);
            tooltip_ = nullptr;
        }
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void FluentList::render(ID2D1DCRenderTarget* rt, float wDip, float hDip) {
    const Palette& p = palette();
    auto* br = brush(rt);
    if (!br)
        return;

    // 每帧按真实可视高度钳制滚动值（选中定位可能发生在窗口尚未布局完成时）
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight() - hDip));

    D2D1_RECT_F surface = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
    fillRoundRect(rt, br, p.cardFill, surface, metrics::cardRadius);
    strokeRoundRect(rt, br, p.cardStroke, surface, 1.0f, metrics::cardRadius);
    if (focused_)
        strokeRoundRect(rt, br, p.accent, D2D1::RectF(1.5f, 1.5f, wDip - 1.5f, hDip - 1.5f),
                        1.5f, metrics::cardRadius - 1.0f);

    rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, wDip, hDip),
                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = -scrollY_;
    for (int i = 0; i < itemCount(); ++i) {
        float rh = rowHeight(i);
        D2D1_RECT_F rowRect = D2D1::RectF(0.0f, y, wDip, y + rh);
        if (rowRect.bottom >= 0 && rowRect.top <= hDip) {
            bool sel = i == selected_;
            bool hov = i == hover_ && !items_[i].header;
            if (sel)
                fillRoundRect(rt, br, p.listSelected, D2D1::RectF(4.0f, y + 2.0f, wDip - 4.0f, y + rh - 2.0f));
            else if (hov)
                fillRoundRect(rt, br, p.listHover, D2D1::RectF(4.0f, y + 2.0f, wDip - 4.0f, y + rh - 2.0f));
            if (sel) {
                // 左侧强调色指示条
                float cy = y + rh / 2.0f;
                fillRoundRect(rt, br, p.accent, D2D1::RectF(7.0f, cy - 8.0f, 10.0f, cy + 8.0f), 1.5f);
            }
            if (rowDraw_) {
                rowDraw_(rt, rowRect, i, sel, hov);
            } else if (items_[i].header) {
                // 每行按需取格式：不同参数会重建缓存格式，跨行持有指针会悬空
                if (auto* hfmt = textFormat(12.0f, 600)) {
                    br->SetColor(p.textSecondary);
                    drawTrimmedText(dwrite(), rt, items_[i].text, hfmt,
                                    D2D1::RectF(12.0f, y, wDip - 12.0f, y + rh), br);
                }
            } else if (auto* fmt = textFormat(13.0f)) {
                br->SetColor(p.text);
                drawTrimmedText(dwrite(), rt, items_[i].text, fmt,
                                D2D1::RectF(16.0f, y, wDip - 12.0f, y + rh), br);
            }
        }
        y += rh;
    }

    // 细滚动条
    float ch = contentHeight();
    if (ch > hDip && hDip > 0) {
        float thumbH = std::max(20.0f, hDip * hDip / ch);
        float usable = hDip - thumbH;
        float ms = std::max(0.0f, ch - hDip);
        float thumbY = ms > 0 ? scrollY_ / ms * usable : 0;
        fillRoundRect(rt, br, p.textSecondary,
                      D2D1::RectF(wDip - kScrollBarW - 3.0f, thumbY, wDip - 3.0f, thumbY + thumbH),
                      kScrollBarW / 2.0f);
    }

    rt->PopAxisAlignedClip();
}

// ---------------- FluentCard ----------------

bool FluentCard::create(HWND parent, int id) {
    return createLayered(parent, L"QQMusicLyricFluentCard", wndProc, id);
}

LRESULT CALLBACK FluentCard::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    FluentCard* self = selfFromMsg<FluentCard>(h, msg, lp);
    if (msg == WM_NCCREATE)
        self->hwnd_ = h;
    if (!self)
        return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
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
        self->hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void FluentCard::render(ID2D1DCRenderTarget* rt, float wDip, float hDip) {
    const Palette& p = palette();
    auto* br = brush(rt);
    if (!br)
        return;
    D2D1_RECT_F rect = D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f);
    fillRoundRect(rt, br, p.cardFill, rect, metrics::cardRadius);
    strokeRoundRect(rt, br, p.cardStroke, rect, 1.0f, metrics::cardRadius);
}

// ---------------- FluentLabel ----------------

bool FluentLabel::create(HWND parent, int id, const wchar_t* text, bool secondary, float dipSize,
                         int weight) {
    text_ = text ? text : L"";
    secondary_ = secondary;
    dipSize_ = dipSize;
    weight_ = weight;
    return createLayered(parent, L"QQMusicLyricFluentLabel", wndProc, id);
}

void FluentLabel::setText(const std::wstring& text) {
    if (text_ == text)
        return;
    text_ = text;
    renderNow();
}

LRESULT CALLBACK FluentLabel::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    FluentLabel* self = selfFromMsg<FluentLabel>(h, msg, lp);
    if (msg == WM_NCCREATE)
        self->hwnd_ = h;
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
        self->hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void FluentLabel::render(ID2D1DCRenderTarget* rt, float wDip, float hDip) {
    const Palette& p = palette();
    auto* br = brush(rt);
    auto* fmt = textFormat(dipSize_, weight_);
    if (!br || !fmt)
        return;
    br->SetColor(secondary_ ? p.textSecondary : p.text);
    rt->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), fmt,
                  D2D1::RectF(0.0f, 0.0f, wDip, hDip), br,
                  D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace fluent
