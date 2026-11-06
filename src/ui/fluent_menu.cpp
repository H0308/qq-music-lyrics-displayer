#include "fluent_menu.h"

#include "ui/fluent_theme.h"
#include "graphics/bitmap_renderer.h"
#include "ui/platform_icon.h"
#include "logging/runtime_logger.h"
#include "logging/ui_timing.h"
#include <wrl/client.h>

#include <windowsx.h>
#include <shellscalingapi.h> // GetDpiForMonitor

#include <algorithm>
#include <cmath>

namespace fluent {

namespace {

constexpr float kItemH = 36.0f;
constexpr float kSepH = 11.0f;
constexpr float kPadY = 8.0f;
constexpr float kPadX = 8.0f;
constexpr float kItemRadius = 6.0f;
constexpr float kIconLeft = 18.0f;
constexpr float kIconSize = 16.0f;
constexpr float kIconTextGap = 10.0f;
constexpr float kGutter = kIconLeft + kIconSize + kIconTextGap;
constexpr float kCheckRightPad = 28.0f;
constexpr float kSubmenuCheckRightPad = 48.0f;
constexpr float kRightPad = 36.0f;
constexpr float kRightTextGap = 24.0f;
constexpr float kMinWidth = 208.0f;
constexpr float kSubmenuGap = 6.0f;
constexpr UINT kTimerSubmenu = 1;
constexpr UINT kTimerReveal = 2;
constexpr ULONGLONG kRevealDurationMs = 100;
// 子菜单只保留很短的防误触延迟；菜单内容已经在 UI 线程准备完成，
// 过长的延迟会被感知为打开卡顿。
constexpr UINT kSubmenuDelayMs = 150;
// 链外点击关闭菜单（由 WH_MOUSE_LL 钩子投递）
constexpr UINT kMsgOutsideClick = WM_APP + 30;

struct MenuWnd {
    HWND hwnd = nullptr;
    std::vector<FluentMenuItem> items;
    int hover = -1;
    ULONGLONG revealStarted = 0;
    MenuWnd* parent = nullptr;
    int parentRow = -1;
    MenuWnd* child = nullptr;
    FluentMenu::Callback cb; // 仅根菜单持有
    BitmapRenderer renderer;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteTextFormat* fmt = nullptr;
    std::vector<ID2D1Bitmap*> nativeIconBitmaps;
    struct RowText {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> label;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> detail;
        float detailWidth = 0.0f;
    };
    std::vector<RowText> rowText;
    float textWidthDip = 0.0f;
    bool frameReady = false;

    void prepareRowText(float widthDip) {
        if (rowText.size() == items.size() && textWidthDip == widthDip)
            return;
        rowText.clear();
        rowText.resize(items.size());
        textWidthDip = widthDip;
        auto* dw = renderer.dwrite();
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            if (item.separator)
                continue;
            const float stateSpace = item.checked
                ? (item.submenu.empty() ? kCheckRightPad : kSubmenuCheckRightPad) + 10.0f
                : kRightPad;
            float labelRight = widthDip - stateSpace;
            auto& text = rowText[i];
            if (!item.rightText.empty() &&
                SUCCEEDED(dw->CreateTextLayout(item.rightText.c_str(),
                    static_cast<UINT32>(item.rightText.size()), fmt,
                    std::max(1.0f, widthDip - kGutter - stateSpace), kItemH,
                    text.detail.GetAddressOf())) && text.detail) {
                DWRITE_TEXT_METRICS metrics{};
                text.detail->GetMetrics(&metrics);
                text.detailWidth = metrics.width;
                labelRight -= text.detailWidth + kRightTextGap;
            }
            dw->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()), fmt,
                                 std::max(1.0f, labelRight - kGutter), kItemH,
                                 text.label.GetAddressOf());
        }
    }

    MenuWnd* root() {
        MenuWnd* m = this;
        while (m->parent)
            m = m->parent;
        return m;
    }

    bool chainOwns(HWND h) const {
        const MenuWnd* m = this;
        while (m->parent)
            m = m->parent;
        for (; m; m = m->child) {
            if (m->hwnd == h)
                return true;
        }
        return false;
    }

    void closeAll();      // 关闭整条链（含自身）
    void closeChildren(); // 关闭子菜单链
    void closeChain();    // 关闭整棵菜单树
    void closeChainFrom();// 关闭自身及子链（退回父菜单）
    void openSubmenu(bool animate = false); // 为当前悬停行打开子菜单
    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp);

    void releaseNativeIconBitmaps() {
        for (auto*& bitmap : nativeIconBitmaps) {
            if (bitmap) {
                bitmap->Release();
                bitmap = nullptr;
            }
        }
        nativeIconBitmaps.clear();
    }

    void ensureNativeIconBitmaps(ID2D1RenderTarget* target) {
        if (!target)
            return;
        if (nativeIconBitmaps.size() != items.size()) {
            releaseNativeIconBitmaps();
            nativeIconBitmaps.resize(items.size(), nullptr);
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (nativeIconBitmaps[i] || !items[i].nativeIcon)
                continue;
            std::vector<BYTE> pixels;
            UINT width = 0;
            UINT height = 0;
            if (!platform_icon::readHiconPixels(items[i].nativeIcon.get(), pixels, width,
                                                height))
                continue;

            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            target->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4,
                                 &properties, &nativeIconBitmaps[i]);
        }
    }

    bool selectable(int row) const {
        return row >= 0 && row < static_cast<int>(items.size()) && !items[row].separator &&
               items[row].enabled;
    }

    float rowY(int row) const {
        float y = kPadY;
        for (int i = 0; i < row; ++i)
            y += items[i].separator ? kSepH : kItemH;
        return y;
    }

    float heightDip() const {
        float h = kPadY * 2;
        for (const auto& it : items)
            h += it.separator ? kSepH : kItemH;
        return h;
    }

    int rowAt(float yDip) const {
        float y = kPadY;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            float h = items[i].separator ? kSepH : kItemH;
            if (yDip >= y && yDip < y + h)
                return i;
            y += h;
        }
        return -1;
    }

    void measure(IDWriteFactory* dw, float& wDip) {
        float maxText = 0;
        IDWriteTextFormat* f = nullptr;
        if (dw && SUCCEEDED(dw->CreateTextFormat(uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &f)) &&
            f) {
            applyUiFontFallback(f);
            for (const auto& it : items) {
                if (it.separator)
                    continue;
                IDWriteTextLayout* layout = nullptr;
                const std::wstring measuredText = it.rightText.empty()
                    ? it.text : it.text + it.rightText;
                if (SUCCEEDED(dw->CreateTextLayout(measuredText.c_str(), static_cast<UINT32>(measuredText.size()),
                                                   f, 1000.0f, 100.0f, &layout)) &&
                    layout) {
                    DWRITE_TEXT_METRICS m{};
                    layout->GetMetrics(&m);
                    const float stateSpace = it.checked
                        ? (it.submenu.empty() ? kCheckRightPad : kSubmenuCheckRightPad) + 10.0f
                        : kRightPad;
                    maxText = std::max(maxText, m.width + stateSpace +
                        (it.rightText.empty() ? 0.0f : kRightTextGap));
                    layout->Release();
                }
            }
            f->Release();
        }
        wDip = std::max(kMinWidth, maxText + kGutter + 18.0f);
    }

    void render() {
        if (!hwnd)
            return;
        const auto started = runtime_log::uiTimeMs();
        frameReady = false;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0)
            return;
        if (!renderer.initialize())
            return;
        UINT dpi = GetDpiForWindow(hwnd);
        renderer.setDpi(dpi);
        if (!renderer.bindDC(w, h)) {
            releaseNativeIconBitmaps();
            if (brush) {
                brush->Release();
                brush = nullptr;
            }
            return;
        }
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        float s = dipScale(dpi);
        float wDip = w / s;

        if (!brush)
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
        if (!fmt) {
            renderer.dwrite()->CreateTextFormat(uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                13.0f, L"zh-cn", &fmt);
            if (fmt) {
                applyUiFontFallback(fmt);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (!brush || !fmt)
            return;
        ensureNativeIconBitmaps(rt);
        prepareRowText(wDip);

        const Palette& p = palette();
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        // 背景、描边和系统阴影保持稳定，只让内容短促淡入，避免整窗动画拖动阴影。
        rt->Clear(p.windowBg);

        const float reveal = revealStarted == 0 ? 1.0f : std::min(
            1.0f, static_cast<float>(GetTickCount64() - revealStarted) / kRevealDurationMs);
        brush->SetOpacity(reveal);

        float y = kPadY;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const auto& it = items[i];
            if (it.separator) {
                brush->SetColor(p.separator);
                rt->FillRectangle(D2D1::RectF(kIconLeft, y + kSepH / 2.0f - 0.5f,
                                              wDip - kIconLeft,
                                              y + kSepH / 2.0f + 0.5f),
                                  brush);
                y += kSepH;
                continue;
            }
            bool hov = i == hover && it.enabled;
            if (hov || (it.checked && it.enabled)) {
                auto fill = hov ? p.listHover : p.accent;
                if (!hov)
                    fill.a = 0.07f;
                brush->SetColor(fill);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(kPadX, y + 2.0f, wDip - kPadX, y + kItemH - 2.0f),
                                      kItemRadius, kItemRadius),
                    brush);
            }
            if (it.checked) {
                // 图标已经占用左侧识别区，勾选标记移到右侧状态位，避免挤开文字。
                brush->SetColor(it.enabled ? p.accent : p.disabled);
                const float checkRightPad = it.submenu.empty() ? kCheckRightPad
                                                                : kSubmenuCheckRightPad;
                const float cx = wDip - checkRightPad;
                const float cy = y + kItemH / 2.0f;
                rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + 3.5f, cy + 3.5f), brush, 1.6f);
                rt->DrawLine(D2D1::Point2F(cx + 3.5f, cy + 3.5f), D2D1::Point2F(cx + 9.0f, cy - 4.0f),
                             brush, 1.6f);
            }
            const float iconTop = y + (kItemH - kIconSize) * 0.5f;
            const D2D1_RECT_F iconRect =
                D2D1::RectF(kIconLeft, iconTop, kIconLeft + kIconSize,
                            iconTop + kIconSize);
            if (i < static_cast<int>(nativeIconBitmaps.size()) && nativeIconBitmaps[i]) {
                rt->DrawBitmap(nativeIconBitmaps[i], iconRect, reveal * (it.enabled ? 1.0f : 0.45f),
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else if (it.icon != settings_icon::Kind::None) {
                brush->SetColor(it.enabled ? (it.checked ? p.accent : p.textSecondary)
                                           : p.disabled);
                settings_icon::draw(
                    rt, it.icon, iconRect,
                    brush, 1.15f);
            }
            brush->SetColor(it.enabled ? p.text : p.disabled);
            const float stateSpace = it.checked
                ? (it.submenu.empty() ? kCheckRightPad : kSubmenuCheckRightPad) + 10.0f
                : kRightPad;
            float labelRight = wDip - stateSpace;
            const auto& text = rowText[static_cast<size_t>(i)];
            if (text.detail) {
                const float detailLeft = labelRight - text.detailWidth;
                brush->SetColor(it.enabled ? p.textSecondary : p.disabled);
                rt->DrawTextLayout(D2D1::Point2F(detailLeft, y), text.detail.Get(), brush);
            }
            brush->SetColor(it.enabled ? p.text : p.disabled);
            if (text.label)
                rt->DrawTextLayout(D2D1::Point2F(kGutter, y), text.label.Get(), brush);
            if (!it.submenu.empty()) {
                // 子菜单箭头
                brush->SetColor(it.enabled ? p.textSecondary : p.disabled);
                float ax = wDip - 22.0f, ay = y + kItemH / 2.0f;
                rt->DrawLine(D2D1::Point2F(ax, ay - 4.0f), D2D1::Point2F(ax + 3.5f, ay), brush, 1.4f);
                rt->DrawLine(D2D1::Point2F(ax + 3.5f, ay), D2D1::Point2F(ax, ay + 4.0f), brush, 1.4f);
            }
            y += kItemH;
        }

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            releaseNativeIconBitmaps();
            if (brush) {
                brush->Release();
                brush = nullptr;
            }
            renderer.discard();
        } else if (SUCCEEDED(hr)) {
            frameReady = true;
            // 显示前只准备位图，首个 WM_PAINT 复制它，不再重复整张菜单的绘制。
            if (IsWindowVisible(hwnd)) {
                HDC hdc = GetDC(hwnd);
                renderer.copyToDC(hdc, w, h);
                ReleaseDC(hwnd, hdc);
            }
        }
        const auto elapsed = runtime_log::uiTimeMs() - started;
        if (elapsed >= 24) {
            runtime_log::writef(L"[ui][menu-render] rows=%zu elapsed=%llums", items.size(),
                                static_cast<unsigned long long>(elapsed));
        }
    }
};

MenuWnd* g_root = nullptr;
constexpr wchar_t kMenuClass[] = L"QQMusicLyricFluentMenu";

// 菜单存活期间的低级鼠标钩子：消隐不能依赖 WM_ACTIVATE（点任务栏空白、托盘区、
// WS_EX_NOACTIVATE 窗口等不会切换前台窗口，WA_INACTIVE 永远不触发），
// 必须直接检测“点击发生在菜单链之外”这个事件本身
HHOOK g_mouseHook = nullptr;

LRESULT CALLBACK lowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_root) {
        switch (wp) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
        case WM_NCMBUTTONDOWN: {
            auto* mhs = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
            bool inside = false;
            for (MenuWnd* m = g_root; m; m = m->child) {
                if (!m->hwnd)
                    continue;
                RECT rc;
                GetWindowRect(m->hwnd, &rc);
                if (PtInRect(&rc, mhs->pt)) {
                    inside = true;
                    break;
                }
            }
            // 不在任何一级菜单窗口内：投递消息异步关闭，避免在钩子里同步销毁窗口
            if (!inside)
                PostMessageW(g_root->hwnd, kMsgOutsideClick, 0, 0);
            break;
        }
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void MenuWnd::closeChildren() {
    if (child) {
        MenuWnd* c = child;
        child = nullptr;
        c->closeAll();
    }
}

void MenuWnd::closeAll() {
    closeChildren();
    if (this == g_root)
        g_root = nullptr;
    if (parent && parent->child == this)
        parent->child = nullptr;
    HWND h = hwnd;
    hwnd = nullptr;
    if (h)
        DestroyWindow(h); // WM_DESTROY 中 delete this
}

// 创建一级菜单窗口（根或子菜单）
MenuWnd* createMenuLevel(HWND owner, MenuWnd* parent, std::vector<FluentMenuItem> items,
                         const FluentMenu::Callback* cb, POINT screenPt, bool animate = true) {
    auto* mw = new MenuWnd();
    mw->parent = parent;
    mw->parentRow = parent ? parent->hover : -1;
    mw->items = std::move(items);
    if (cb)
        mw->cb = *cb;

    mw->renderer.initialize();
    float wDip = kMinWidth;
    mw->measure(mw->renderer.dwrite(), wDip);
    float hDip = mw->heightDip();

    // 取菜单弹出位置所在显示器的真实 DPI：owner 是不可见托盘窗口，显示 DPI 变化后
    // 隐藏窗口可能收不到 WM_DPICHANGED 导致其 DPI 过期；GetDpiForMonitor 始终返回
    // 显示器当前 DPI，且天然适配多显示器
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    UINT dpi = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpi);
    float s = dipScale(dpi);
    int w = static_cast<int>(std::lround(wDip * s));
    int h = static_cast<int>(std::lround(hDip * s));

    // 边界修正：保持在工作区内
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int x = screenPt.x;
    int y = screenPt.y;
    if (parent) {
        RECT parentRect{};
        GetWindowRect(parent->hwnd, &parentRect);
        const int gap = static_cast<int>(std::lround(kSubmenuGap * s));
        x = parentRect.right + gap;
        if (x + w > mi.rcWork.right)
            x = parentRect.left - w - gap; // 右侧不足时，在父菜单左侧保持同样间距。
    }
    if (x + w > mi.rcWork.right)
        x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom)
        y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left)
        x = mi.rcWork.left;
    if (y < mi.rcWork.top)
        y = mi.rcWork.top;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // 外轮廓和阴影统一由 DWM 绘制，不叠加旧式矩形菜单阴影。
    wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        MenuWnd* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<MenuWnd*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MenuWnd*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        return self->handleMsg(msg, wp, lp);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMenuClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    mw->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuClass, L"", WS_POPUP, x, y, w,
                               h, owner, nullptr, GetModuleHandleW(nullptr), mw);
    if (!mw->hwnd) {
        delete mw;
        return nullptr;
    }
    applyRoundCorners(mw->hwnd, false);
    applyDarkCaption(mw->hwnd, isDarkMode(ThemeTarget::Window));
    const auto& p = palette();
    const auto stroke = p.controlStroke;
    const auto borderChannel = [&](float foreground, float background) {
        return static_cast<BYTE>(std::lround(
            std::clamp(foreground * stroke.a + background * (1.0f - stroke.a), 0.0f, 1.0f) * 255.0f));
    };
    applyBorderColor(mw->hwnd, RGB(borderChannel(stroke.r, p.windowBg.r),
                                  borderChannel(stroke.g, p.windowBg.g),
                                  borderChannel(stroke.b, p.windowBg.b)));
    BOOL clientAnimations = TRUE;
    BOOL menuAnimations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimations, 0);
    SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &menuAnimations, 0);
    if (animate && clientAnimations && menuAnimations &&
        SetTimer(mw->hwnd, kTimerReveal, USER_TIMER_MINIMUM, nullptr))
        mw->revealStarted = GetTickCount64();
    mw->render();
    ShowWindow(mw->hwnd, SW_SHOWNA);
    return mw;
}

} // namespace

// 消息处理放在结构外实现（lambda 转 WNDPROC 需要无捕获静态函数）
LRESULT MenuWnd::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    auto dipY = [&](LPARAM l) {
        return GET_Y_LPARAM(l) / dipScale(GetDpiForWindow(hwnd));
    };
    switch (msg) {
    case WM_SIZE:
        frameReady = false;
        return 0;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        render();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (frameReady) {
            RECT client{};
            GetClientRect(hwnd, &client);
            renderer.copyToDC(hdc, client.right - client.left, client.bottom - client.top);
        } else {
            render();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int row = rowAt(dipY(lp));
        if (!selectable(row))
            row = -1;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme); // 重复武装是幂等的
        if (row != hover) {
            revealStarted = 0;
            KillTimer(hwnd, kTimerReveal);
            hover = row;
            render();
            KillTimer(hwnd, kTimerSubmenu);
            if (selectable(row) && !items[row].submenu.empty() &&
                (!child || child->parentRow != row))
                SetTimer(hwnd, kTimerSubmenu, kSubmenuDelayMs, nullptr);
            if (selectable(row) && items[row].submenu.empty())
                closeChildren();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover != -1) {
            hover = -1;
            KillTimer(hwnd, kTimerSubmenu);
            render();
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimerSubmenu) {
            KillTimer(hwnd, kTimerSubmenu);
            openSubmenu(true);
        } else if (wp == kTimerReveal) {
            if (GetTickCount64() - revealStarted >= kRevealDurationMs) {
                revealStarted = 0;
                KillTimer(hwnd, kTimerReveal);
            }
            render();
        }
        return 0;
    case WM_LBUTTONUP: {
        int row = rowAt(dipY(lp));
        if (!selectable(row))
            return 0;
        if (!items[row].submenu.empty()) {
            openSubmenu();
            return 0;
        }
        int id = items[row].id;
        FluentMenu::Callback cb = root()->cb;
        closeChain();
        if (cb)
            cb(id);
        return 0;
    }
    case WM_KEYDOWN: {
        if (revealStarted != 0) {
            revealStarted = 0;
            KillTimer(hwnd, kTimerReveal);
            render();
        }
        if (wp == VK_ESCAPE) {
            if (parent) {
                closeChainFrom();
            } else {
                closeChain();
            }
            return 0;
        }
        if (wp == VK_DOWN || wp == VK_UP) {
            int dir = wp == VK_DOWN ? 1 : -1;
            int n = static_cast<int>(items.size());
            int i = hover;
            for (int step = 0; step < n; ++step) {
                i = (i + dir + n) % n;
                if (selectable(i)) {
                    hover = i;
                    render();
                    break;
                }
            }
            return 0;
        }
        if (wp == VK_RETURN) {
            if (selectable(hover)) {
                if (!items[hover].submenu.empty()) {
                    openSubmenu();
                } else {
                    int id = items[hover].id;
                    FluentMenu::Callback cb = root()->cb;
                    closeChain();
                    if (cb)
                        cb(id);
                }
            }
            return 0;
        }
        if (wp == VK_RIGHT) {
            if (selectable(hover) && !items[hover].submenu.empty())
                openSubmenu();
            return 0;
        }
        if (wp == VK_LEFT) {
            if (parent)
                closeChainFrom();
            return 0;
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND other = reinterpret_cast<HWND>(lp);
            if (!chainOwns(other))
                closeChain();
        }
        return 0;
    case WM_DPICHANGED:
        // 显示 DPI 变化（切换分辨率/缩放）：菜单是瞬时 UI，直接关闭，
        // 下次弹出时按新 DPI 重建，避免旧尺寸内容被拉伸
        closeChain();
        return 0;
    case kMsgOutsideClick:
        closeChain();
        return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, kTimerSubmenu);
        KillTimer(hwnd, kTimerReveal);
        // 只有根菜单持有鼠标钩子
        if (!parent && g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }
        if (brush) {
            brush->Release();
            brush = nullptr;
        }
        if (fmt) {
            fmt->Release();
            fmt = nullptr;
        }
        releaseNativeIconBitmaps();
        MenuWnd* self = this;
        if (self == g_root)
            g_root = nullptr;
        if (parent && parent->child == self)
            parent->child = nullptr;
        hwnd = nullptr;
        delete self;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MenuWnd::closeChain() {
    root()->closeAll();
}

void MenuWnd::closeChainFrom() {
    // 关闭自身及其子链（Esc/左键 退回父菜单）
    MenuWnd* p = parent;
    closeAll();
    if (p && p->hwnd)
        SetForegroundWindow(p->hwnd);
}

void MenuWnd::openSubmenu(bool animate) {
    if (!selectable(hover) || items[hover].submenu.empty())
        return;
    // 返回同一个父菜单项时复用现有子窗口，不重建或重复播放淡入动画。
    if (child && child->parentRow == hover)
        return;
    if (child)
        closeChildren();
    float s = dipScale(GetDpiForWindow(hwnd));
    float y = rowY(hover);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT pt{rc.right, rc.top + static_cast<int>((y - kPadY) * s)};
    child = createMenuLevel(hwnd, this, items[hover].submenu, nullptr, pt, animate);
    if (child)
        SetForegroundWindow(child->hwnd);
}

void FluentMenu::show(HWND owner, POINT screenPt, std::vector<FluentMenuItem> items, Callback cb) {
    dismiss();
    g_root = createMenuLevel(owner, nullptr, std::move(items), &cb, screenPt);
    if (g_root) {
        SetForegroundWindow(g_root->hwnd);
        // 菜单存活期间全局监听链外点击；低级钩子回调在本线程消息循环中执行
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, lowLevelMouseProc,
                                        GetModuleHandleW(nullptr), 0);
    }
}

void FluentMenu::dismiss() {
    if (g_root)
        g_root->closeAll();
}

} // namespace fluent
