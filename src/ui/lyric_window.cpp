#include "lyric_window.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 33; // ~30fps
constexpr UINT kTrayMsg = WM_APP + 100;
constexpr UINT kTrayIconId = 1;

constexpr UINT kCmdToggleThrough = 1001;
constexpr UINT kCmdFontUp = 1002;
constexpr UINT kCmdFontDown = 1003;
constexpr UINT kCmdExit = 1004;

constexpr wchar_t kWndClassName[] = L"QQMusicLyricOverlay";
constexpr wchar_t kFontFamily[] = L"Microsoft YaHei UI";

constexpr float kAnchorRatio = 0.42f; // 当前行垂直锚点
constexpr float kScrollEase = 0.25f;  // 滚动 ease-out 系数
constexpr float kMinFont = 14.0f;
constexpr float kMaxFont = 48.0f;

} // namespace

struct OverlayHost::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool visible = false;
    bool clickThrough = false;

    // 布局（DIP，96dpi 基准）
    float wndW = 860.0f;
    float fontSize = 24.0f;
    UINT dpi = 96;

    float lineHeight() const { return fontSize * 2.2f; }
    float wndH() const { return lineHeight() * 5.0f; }

    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    int currentLine = -1;
    float scroll = 0.0f;
    float scrollTarget = 0.0f;

    // 每行预计算的排版：正常 / 高亮两种格式各自的单行最大宽度与折行结果
    struct LineLayout {
        IDWriteTextLayout* layoutNormal = nullptr;
        IDWriteTextLayout* layoutCurrent = nullptr;
        float naturalWNormal = 0.0f;  // 正常格式单行最大宽度
        float naturalWCurrent = 0.0f; // 高亮格式单行最大宽度
        float heightNormal = 0.0f;    // 按内容宽度折行后的高度
        float heightCurrent = 0.0f;
    };
    std::vector<LineLayout> layouts;
    bool layoutsDirty = true;

    std::function<void()> tick;

    // D2D / GDI 资源
    HDC memdc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    int bmpW = 0;
    int bmpH = 0;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brushBg = nullptr;
    ID2D1SolidColorBrush* brushNormal = nullptr;
    ID2D1SolidColorBrush* brushCurrent = nullptr;
    ID2D1SolidColorBrush* brushShadow = nullptr;
    IDWriteTextFormat* fmtLine = nullptr;
    IDWriteTextFormat* fmtCurrent = nullptr;

    bool dragging = false;
    POINT dragCursor{};
    RECT dragWnd{};

    float scale() const { return (float)dpi / 96.0f; }

    // ---------- 资源 ----------

    void createDeviceResources() {
        if (rt) return;
        if (!d2d) {
            D2D1_FACTORY_OPTIONS opts{};
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts,
                              reinterpret_cast<void**>(&d2d));
        }
        if (!dwrite) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(&dwrite));
        }
        if (!d2d || !dwrite) return;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateDCRenderTarget(&props, &rt))) {
            rt = nullptr;
            return;
        }
        rt->SetDpi((float)dpi, (float)dpi);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f), &brushBg);
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.60f), &brushNormal);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.76f, 0.49f, 1.0f), &brushCurrent); // QQ 绿
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &brushShadow);
        recreateFormats();
    }

    void recreateFormats() {
        if (!dwrite) return;
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            if (*out) {
                (*out)->Release();
                *out = nullptr;
            }
            dwrite->CreateTextFormat(kFontFamily, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", out);
            if (*out) {
                (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };
        make(fontSize, DWRITE_FONT_WEIGHT_NORMAL, &fmtLine);
        make(fontSize * 1.15f, DWRITE_FONT_WEIGHT_BOLD, &fmtCurrent);
    }

    // ---------- 歌词排版（显示前预计算） ----------

    float contentPad() const { return 24.0f; }
    float contentWidth() const { return wndW - contentPad() * 2.0f; }
    float lineGap() const { return fontSize * 0.8f; }

    // 先用无限宽度测单行最大宽度，再限制到内容宽度得到折行高度
    IDWriteTextLayout* buildLayout(IDWriteTextFormat* fmt, const std::wstring& text,
                                   float& naturalW, float& height) {
        IDWriteTextLayout* lay = nullptr;
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 100000.0f,
                                            100000.0f, &lay)))
            return nullptr;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS m{};
        lay->GetMetrics(&m);
        naturalW = m.width;
        lay->SetMaxWidth(contentWidth());
        lay->GetMetrics(&m);
        height = m.height;
        // 布局框默认 100000 高，段落居中会把文字推到框中央；收紧到内容高度
        lay->SetMaxHeight(height);
        return lay;
    }

    void releaseLayouts() {
        for (auto& ll : layouts) {
            if (ll.layoutNormal) ll.layoutNormal->Release();
            if (ll.layoutCurrent) ll.layoutCurrent->Release();
        }
        layouts.clear();
    }

    void rebuildLayouts() {
        layoutsDirty = false;
        releaseLayouts();
        if (!dwrite || !fmtLine || !fmtCurrent) {
            layoutsDirty = true; // 资源未就绪，渲染时重试
            return;
        }
        layouts.resize(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            LineLayout& ll = layouts[i];
            if (lines[i].text.empty()) {
                ll.heightNormal = fontSize * 1.4f;
                ll.heightCurrent = fontSize * 1.15f * 1.4f;
                continue;
            }
            ll.layoutNormal =
                buildLayout(fmtLine, lines[i].text, ll.naturalWNormal, ll.heightNormal);
            ll.layoutCurrent =
                buildLayout(fmtCurrent, lines[i].text, ll.naturalWCurrent, ll.heightCurrent);
            if (ll.heightNormal <= 0.0f) ll.heightNormal = fontSize * 1.4f;
            if (ll.heightCurrent <= 0.0f) ll.heightCurrent = fontSize * 1.15f * 1.4f;
        }
        updateScrollTarget();
    }

    float blockHeight(size_t i, bool cur) const {
        const LineLayout& ll = layouts[i];
        return (cur ? ll.heightCurrent : ll.heightNormal) + lineGap();
    }

    // 使当前行块垂直居中于锚点所需的滚动偏移（DIP）
    void updateScrollTarget() {
        if (currentLine < 0 || layouts.size() != lines.size() ||
            (size_t)currentLine >= layouts.size()) {
            scrollTarget = 0.0f;
            return;
        }
        float prefix = 0.0f;
        for (int i = 0; i < currentLine; ++i) prefix += blockHeight((size_t)i, false);
        scrollTarget = prefix + blockHeight((size_t)currentLine, true) / 2.0f;
    }

    void discardDeviceResources() {
        auto rel = [](IUnknown*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        auto r = [&](IUnknown* p) { if (p) p->Release(); };
        r(fmtLine); fmtLine = nullptr;
        r(fmtCurrent); fmtCurrent = nullptr;
        r(brushBg); brushBg = nullptr;
        r(brushNormal); brushNormal = nullptr;
        r(brushCurrent); brushCurrent = nullptr;
        r(brushShadow); brushShadow = nullptr;
        r(rt); rt = nullptr;
    }

    void releaseAll() {
        releaseLayouts();
        discardDeviceResources();
        if (dwrite) {
            dwrite->Release();
            dwrite = nullptr;
        }
        if (d2d) {
            d2d->Release();
            d2d = nullptr;
        }
        if (memdc) {
            if (oldBmp) SelectObject(memdc, oldBmp);
            if (dib) DeleteObject(dib);
            DeleteDC(memdc);
            memdc = nullptr;
            dib = nullptr;
            oldBmp = nullptr;
        }
    }

    // 按窗口目标尺寸（设备像素）重建 DIB
    void ensureBitmap() {
        int w = (int)std::lround(wndW * scale());
        int h = (int)std::lround(wndH() * scale());
        if (w == bmpW && h == bmpH && memdc) return;
        if (memdc) {
            if (oldBmp) SelectObject(memdc, oldBmp);
            if (dib) DeleteObject(dib);
            DeleteDC(memdc);
            memdc = nullptr;
            dib = nullptr;
            oldBmp = nullptr;
        }
        HDC screen = GetDC(nullptr);
        memdc = CreateCompatibleDC(screen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // 自上而下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!dib) {
            DeleteDC(memdc);
            memdc = nullptr;
            return;
        }
        oldBmp = SelectObject(memdc, dib);
        bmpW = w;
        bmpH = h;
    }

    // ---------- 渲染 ----------

    void drawText(IDWriteTextFormat* fmt, const std::wstring& text, const D2D1_RECT_F& rect,
                  ID2D1SolidColorBrush* brush) {
        rt->DrawTextW(text.c_str(), (UINT32)text.size(), fmt, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }

    void render() {
        if (!visible || !hwnd) return;
        createDeviceResources();
        ensureBitmap();
        if (!rt || !memdc) return;
        float dpiX = 0.0f, dpiY = 0.0f;
        rt->GetDpi(&dpiX, &dpiY);
        if (dpiX != (float)dpi) rt->SetDpi((float)dpi, (float)dpi);

        RECT rc{0, 0, bmpW, bmpH};
        if (FAILED(rt->BindDC(memdc, &rc))) return;
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        D2D1_ROUNDED_RECT bg{D2D1::RectF(0.0f, 0.0f, wndW, wndH()), 14.0f, 14.0f};
        rt->FillRoundedRectangle(bg, brushBg);

        const float anchorY = wndH() * kAnchorRatio;
        if (!lines.empty()) {
            if (layoutsDirty || layouts.size() != lines.size()) rebuildLayouts();
            const float x = contentPad();
            float cum = 0.0f; // 第 i 行块顶相对窗口顶部的累计偏移（未减 scroll）
            for (size_t i = 0; i < lines.size(); ++i) {
                bool cur = ((int)i == currentLine);
                float bh = blockHeight(i, cur);
                float yTop = anchorY + cum - scroll;
                cum += bh;
                if (yTop > wndH() || yTop + bh < 0.0f) continue;
                const LineLayout& ll = layouts[i];
                IDWriteTextLayout* lay = cur ? ll.layoutCurrent : ll.layoutNormal;
                if (!lay) continue;
                float layH = cur ? ll.heightCurrent : ll.heightNormal;
                float y = yTop + (bh - layH) * 0.5f;
                ID2D1SolidColorBrush* brush = cur ? brushCurrent : brushNormal;
                rt->DrawTextLayout(D2D1::Point2F(x + 1.0f, y + 1.5f), lay, brushShadow);
                rt->DrawTextLayout(D2D1::Point2F(x, y), lay, brush);
            }
        } else if (!statusText.empty()) {
            D2D1_RECT_F rect = D2D1::RectF(0.0f, 0.0f, wndW, wndH());
            drawText(fmtLine, statusText, rect, brushNormal);
        }

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardDeviceResources();
            return;
        }

        POINT ptSrc{0, 0};
        SIZE sz{bmpW, bmpH};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, nullptr, nullptr, &sz, memdc, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    // ---------- 事件 ----------

    void onTimer() {
        if (tick) tick();
        float diff = scrollTarget - scroll;
        if (std::fabs(diff) > 0.002f)
            scroll += diff * kScrollEase;
        else
            scroll = scrollTarget;
        render();
    }

    void resizeWindow() {
        if (!hwnd) return;
        int w = (int)std::lround(wndW * scale());
        int h = (int)std::lround(wndH() * scale());
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void changeFont(float delta) {
        fontSize = std::clamp(fontSize + delta, kMinFont, kMaxFont);
        recreateFormats();
        layoutsDirty = true; // 字号变化需重新计算折行
        resizeWindow();
        render();
    }

    void setClickThrough(bool on) {
        clickThrough = on;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (on)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void addTray() {
        NOTIFYICONDATAW d{};
        d.cbSize = sizeof(d);
        d.hWnd = hwnd;
        d.uID = kTrayIconId;
        d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        d.uCallbackMessage = kTrayMsg;
        d.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        lstrcpyW(d.szTip, L"QQ 音乐歌词");
        Shell_NotifyIconW(NIM_ADD, &d);
    }

    void removeTray() {
        NOTIFYICONDATAW d{};
        d.cbSize = sizeof(d);
        d.hWnd = hwnd;
        d.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &d);
    }

    void showTrayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (clickThrough ? MF_CHECKED : 0), kCmdToggleThrough,
                    L"鼠标穿透");
        AppendMenuW(menu, MF_STRING, kCmdFontUp, L"增大字号");
        AppendMenuW(menu, MF_STRING, kCmdFontDown, L"减小字号");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y,
                                  0, hwnd, nullptr);
        DestroyMenu(menu);
        switch (cmd) {
        case kCmdToggleThrough:
            setClickThrough(!clickThrough);
            break;
        case kCmdFontUp:
            changeFont(2.0f);
            break;
        case kCmdFontDown:
            changeFont(-2.0f);
            break;
        case kCmdExit:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            addTray();
            SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kTimerId) onTimer();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            if (!clickThrough) {
                dragging = true;
                SetCapture(hwnd);
                GetCursorPos(&dragCursor);
                GetWindowRect(hwnd, &dragWnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                POINT pt;
                GetCursorPos(&pt);
                SetWindowPos(hwnd, nullptr, dragWnd.left + (pt.x - dragCursor.x),
                             dragWnd.top + (pt.y - dragCursor.y), 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_MOUSEWHEEL:
            changeFont(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 2.0f : -2.0f);
            return 0;
        case WM_DPICHANGED: {
            dpi = HIWORD(wp);
            RECT* sug = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                         sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
            recreateFormats();
            render();
            return 0;
        }
        case kTrayMsg:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showTrayMenu();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            removeTray();
            releaseAll();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
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
        if (self) return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
};

OverlayHost::OverlayHost() : impl_(std::make_unique<Impl>()) {}

OverlayHost::~OverlayHost() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
}

bool OverlayHost::create(HINSTANCE inst) {
    impl_->inst = inst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int pxW = (int)std::lround(impl_->wndW);
    int pxH = (int)std::lround(impl_->wndH());
    int x = work.left + ((work.right - work.left) - pxW) / 2;
    int y = work.bottom - pxH - 80;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyric", WS_POPUP, x, y, pxW, pxH, nullptr,
                             nullptr, inst, impl_.get());
    if (!h) return false;
    impl_->hwnd = h;
    impl_->dpi = GetDpiForWindow(h);
    impl_->resizeWindow();
    return true;
}

HWND OverlayHost::hwnd() const {
    return impl_ ? impl_->hwnd : nullptr;
}

void OverlayHost::setTickCallback(std::function<void()> cb) {
    impl_->tick = std::move(cb);
}

const std::vector<LyricLine>& OverlayHost::lyrics() const {
    return impl_->lines;
}

void OverlayHost::show() {
    if (!impl_->visible) {
        impl_->visible = true;
        ShowWindow(impl_->hwnd, SW_SHOWNA);
    }
    impl_->render();
}

void OverlayHost::hide() {
    if (impl_->visible) {
        impl_->visible = false;
        ShowWindow(impl_->hwnd, SW_HIDE);
    }
}

void OverlayHost::setLyrics(const std::vector<LyricLine>& lines) {
    impl_->lines = lines;
    impl_->currentLine = -1;
    impl_->scroll = 0.0f;
    impl_->scrollTarget = 0.0f;
    impl_->layoutsDirty = true;
    if (!lines.empty()) impl_->statusText.clear();
    impl_->render();
}

void OverlayHost::setCurrentLine(int index) {
    if (index != impl_->currentLine) {
        impl_->currentLine = index;
        impl_->updateScrollTarget();
    }
}

void OverlayHost::setStatusText(const std::wstring& text) {
    impl_->statusText = text;
    impl_->render();
}
