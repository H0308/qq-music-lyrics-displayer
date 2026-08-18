#include "fluent_menu.h"

#include "ui/lyric_renderer.h"
#include "ui/fluent_theme.h"

#include <windowsx.h>
#include <shellscalingapi.h> // GetDpiForMonitor

#include <algorithm>
#include <cmath>

namespace fluent {

namespace {

constexpr float kItemH = 30.0f;
constexpr float kSepH = 9.0f;
constexpr float kPadY = 4.0f;
constexpr float kPadX = 4.0f;
constexpr float kGutter = 28.0f; // 勾选标记区
constexpr float kRightPad = 26.0f;
constexpr float kMinWidth = 180.0f;
constexpr UINT kTimerSubmenu = 1;
constexpr UINT kSubmenuDelayMs = 350;
// 链外点击关闭菜单（由 WH_MOUSE_LL 钩子投递）
constexpr UINT kMsgOutsideClick = WM_APP + 30;

struct MenuWnd {
    HWND hwnd = nullptr;
    std::vector<FluentMenuItem> items;
    int hover = -1;
    MenuWnd* parent = nullptr;
    MenuWnd* child = nullptr;
    FluentMenu::Callback cb; // 仅根菜单持有
    LyricRenderer renderer;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteTextFormat* fmt = nullptr;

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
    void openSubmenu();   // 为当前悬停行打开子菜单
    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp);

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
                if (SUCCEEDED(dw->CreateTextLayout(it.text.c_str(), static_cast<UINT32>(it.text.size()),
                                                   f, 1000.0f, 100.0f, &layout)) &&
                    layout) {
                    DWRITE_TEXT_METRICS m{};
                    layout->GetMetrics(&m);
                    maxText = std::max(maxText, m.width);
                    layout->Release();
                }
            }
            f->Release();
        }
        wDip = std::max(kMinWidth, maxText + kGutter + 8.0f + kRightPad + kPadX * 2);
    }

    void render() {
        if (!hwnd)
            return;
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
        if (!renderer.bindDC(w, h))
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        float s = dipScale(dpi);
        float wDip = w / s, hDip = h / s;

        if (brush) {
            brush->Release();
            brush = nullptr;
        }
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

        const Palette& p = palette();
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        brush->SetColor(p.menuFill);
        rt->Clear(p.menuFill);

        float y = kPadY;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const auto& it = items[i];
            if (it.separator) {
                brush->SetColor(p.separator);
                rt->FillRectangle(D2D1::RectF(kGutter, y + kSepH / 2.0f - 0.5f, wDip - 12.0f,
                                              y + kSepH / 2.0f + 0.5f),
                                  brush);
                y += kSepH;
                continue;
            }
            bool hov = i == hover && it.enabled;
            if (hov) {
                brush->SetColor(p.listHover);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(kPadX, y + 1.0f, wDip - kPadX, y + kItemH - 1.0f),
                                      4.0f, 4.0f),
                    brush);
            }
            if (it.checked) {
                // 勾选标记：手绘折线
                brush->SetColor(it.enabled ? p.text : p.textSecondary);
                float cx = kPadX + 10.0f, cy = y + kItemH / 2.0f;
                rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + 3.5f, cy + 3.5f), brush, 1.6f);
                rt->DrawLine(D2D1::Point2F(cx + 3.5f, cy + 3.5f), D2D1::Point2F(cx + 9.0f, cy - 4.0f),
                             brush, 1.6f);
            }
            brush->SetColor(it.enabled ? p.text : p.textSecondary);
            rt->DrawTextW(it.text.c_str(), static_cast<UINT32>(it.text.size()), fmt,
                          D2D1::RectF(kGutter, y, wDip - kRightPad, y + kItemH), brush);
            if (!it.submenu.empty()) {
                // 子菜单箭头
                brush->SetColor(p.textSecondary);
                float ax = wDip - 16.0f, ay = y + kItemH / 2.0f;
                rt->DrawLine(D2D1::Point2F(ax, ay - 4.0f), D2D1::Point2F(ax + 3.5f, ay), brush, 1.4f);
                rt->DrawLine(D2D1::Point2F(ax + 3.5f, ay), D2D1::Point2F(ax, ay + 4.0f), brush, 1.4f);
            }
            y += kItemH;
        }

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
            renderer.discard();
        else {
            HDC hdc = GetDC(hwnd);
            renderer.copyToDC(hdc, w, h);
            ReleaseDC(hwnd, hdc);
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
                         const FluentMenu::Callback* cb, POINT screenPt) {
    auto* mw = new MenuWnd();
    mw->parent = parent;
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
        x = screenPt.x - static_cast<int>(kPadX * s); // 与父菜单项右侧对齐
        if (x + w > mi.rcWork.right)
            x = screenPt.x - w + static_cast<int>(kPadX * s); // 翻到左侧
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
    wc.style = CS_DROPSHADOW;
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
    applyRoundCorners(mw->hwnd, true);
    suppressBorder(mw->hwnd);
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
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        render();
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
            hover = row;
            render();
            KillTimer(hwnd, kTimerSubmenu);
            if (selectable(row) && !items[row].submenu.empty())
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
            openSubmenu();
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

void MenuWnd::openSubmenu() {
    if (!selectable(hover) || items[hover].submenu.empty())
        return;
    if (child)
        closeChildren();
    float s = dipScale(GetDpiForWindow(hwnd));
    float y = rowY(hover);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT pt{rc.right, rc.top + static_cast<int>(y * s)};
    child = createMenuLevel(hwnd, this, items[hover].submenu, nullptr, pt);
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
