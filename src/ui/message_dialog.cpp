#include "message_dialog.h"

#include "ui/app_icon.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"

#include <algorithm>
#include <cmath>
#include <dwrite.h>

namespace message_dialog {

namespace {

constexpr wchar_t kWindowClassName[] = L"QQMusicLyricMessageDialog";
constexpr DWORD kWindowStyle = WS_CAPTION | WS_SYSMENU;
constexpr DWORD kWindowExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
constexpr int kPrimaryButtonId = IDOK;
constexpr int kSecondaryButtonId = IDCANCEL;
constexpr float kClientWidthDip = 380.0f;
constexpr float kPagePaddingDip = 24.0f;
constexpr float kMessageTopDip = 22.0f;
constexpr float kMessageButtonGapDip = 18.0f;
constexpr float kButtonHeightDip = fluent::metrics::controlHeight;
constexpr float kButtonGapDip = fluent::metrics::compactGap;
constexpr float kPrimaryButtonWidthDip = 88.0f;
constexpr float kSecondaryButtonWidthDip = 104.0f;
constexpr float kMessageFontSizeDip = 14.0f;
constexpr float kMinimumMessageHeightDip = 22.0f;
constexpr float kBottomPaddingDip = 20.0f;

float measureMessageHeightDip(const std::wstring& message, float widthDip) {
    IDWriteFactory* factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&factory))) ||
        !factory) {
        return kMinimumMessageHeightDip;
    }

    IDWriteTextFormat* format = nullptr;
    const HRESULT formatResult = factory->CreateTextFormat(
        fluent::uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, kMessageFontSizeDip, L"zh-cn", &format);
    if (FAILED(formatResult) || !format) {
        factory->Release();
        return kMinimumMessageHeightDip;
    }
    fluent::applyUiFontFallback(format);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    IDWriteTextLayout* layout = nullptr;
    float height = kMinimumMessageHeightDip;
    if (SUCCEEDED(factory->CreateTextLayout(message.c_str(), static_cast<UINT32>(message.size()),
                                            format, std::max(1.0f, widthDip), 1000.0f,
                                            &layout)) &&
        layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics)))
            height = std::max(height, std::ceil(metrics.height));
        layout->Release();
    }

    format->Release();
    factory->Release();
    return height;
}

struct Impl {
    HINSTANCE inst = nullptr;
    HWND owner = nullptr;
    HWND hwnd = nullptr;
    bool focusPrimary = true;
    bool hasSecondary = false;
    Result result = Result::Closed;
    int initialX = 0;
    int initialY = 0;
    int initialWidth = 0;
    int initialHeight = 0;

    std::wstring title;
    std::wstring message;
    std::wstring primaryLabel;
    std::wstring secondaryLabel;
    float messageHeightDip = kMinimumMessageHeightDip;
    D2D1_RECT_F messageRect{};

    fluent::FluentDialogSurface surface;
    fluent::FluentButton primaryButton;
    fluent::FluentButton secondaryButton;

    float clientHeightDip() const {
        return kMessageTopDip + messageHeightDip + kMessageButtonGapDip +
               kButtonHeightDip + kBottomPaddingDip;
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

    bool create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Impl::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWindowClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = app_icon::windowIcon();
        wc.hIconSm = wc.hIcon;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        const UINT ownerDpi = owner ? GetDpiForWindow(owner) : 0;
        const UINT dpi = ownerDpi ? ownerDpi : GetDpiForSystem();
        const UINT targetDpi = dpi ? dpi : 96;
        const float scale = fluent::dipScale(targetDpi);
        RECT rc{0, 0, static_cast<LONG>(std::lround(kClientWidthDip * scale)),
                static_cast<LONG>(std::lround(clientHeightDip() * scale))};
        if (!AdjustWindowRectExForDpi(&rc, kWindowStyle, FALSE, kWindowExStyle, targetDpi))
            return false;

        RECT work{};
        HMONITOR monitor = owner && IsWindow(owner)
                               ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                               : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
            work = monitorInfo.rcWork;
        else
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (work.right <= work.left || work.bottom <= work.top) {
            work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        initialX = x;
        initialY = y;
        initialWidth = width;
        initialHeight = height;

        hwnd = CreateWindowExW(kWindowExStyle, kWindowClassName, title.c_str(),
                               kWindowStyle, x, y, width, height, owner, nullptr, inst, this);
        if (!hwnd)
            return false;

        app_icon::applyWindowIcon(hwnd);
        return true;
    }

    void show() {
        if (!hwnd)
            return;
        SetWindowPos(hwnd, nullptr, initialX, initialY, initialWidth, initialHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }

    void resizeForDpi(UINT dpi) {
        if (!hwnd)
            return;
        const UINT targetDpi = dpi ? dpi : 96;
        const float scale = fluent::dipScale(targetDpi);
        RECT rc{0, 0, static_cast<LONG>(std::lround(kClientWidthDip * scale)),
                static_cast<LONG>(std::lround(clientHeightDip() * scale))};
        if (!AdjustWindowRectExForDpi(&rc, kWindowStyle, FALSE, kWindowExStyle, targetDpi))
            return;

        RECT current{};
        if (!GetWindowRect(hwnd, &current) || current.right <= current.left ||
            current.bottom <= current.top)
            return;
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int centerX = (current.left + current.right) / 2;
        const int centerY = (current.top + current.bottom) / 2;
        SetWindowPos(hwnd, nullptr, centerX - width / 2, centerY - height / 2, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void createControls() {
        primaryButton.create(hwnd, kPrimaryButtonId, primaryLabel.c_str(), true);
        if (hasSecondary)
            secondaryButton.create(hwnd, kSecondaryButtonId, secondaryLabel.c_str(), false);
    }

    void layout() {
        if (!hwnd)
            return;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        const UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
        const float scale = fluent::dipScale(dpi);
        const auto px = [scale](float dip) {
            return static_cast<int>(std::lround(dip * scale));
        };
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int padding = px(kPagePaddingDip);
        const int buttonHeight = px(kButtonHeightDip);
        const int buttonGap = px(kButtonGapDip);
        const int buttonY = height - px(kBottomPaddingDip) - buttonHeight;
        const int primaryWidth = px(kPrimaryButtonWidthDip);
        const int secondaryWidth = px(kSecondaryButtonWidthDip);

        if (hasSecondary) {
            secondaryButton.move(width - padding - secondaryWidth, buttonY, secondaryWidth,
                                 buttonHeight);
            primaryButton.move(width - padding - secondaryWidth - buttonGap - primaryWidth,
                               buttonY, primaryWidth, buttonHeight);
        } else {
            primaryButton.move(width - padding - primaryWidth, buttonY, primaryWidth,
                               buttonHeight);
        }

        const float widthDip = static_cast<float>(width) / scale;
        const float heightDip = static_cast<float>(height) / scale;
        const float buttonYDip = heightDip - kBottomPaddingDip - kButtonHeightDip;
        messageRect = D2D1::RectF(kPagePaddingDip, kMessageTopDip,
                                   std::max(kPagePaddingDip + 1.0f, widthDip - kPagePaddingDip),
                                   std::max(kMessageTopDip + kMinimumMessageHeightDip,
                                            buttonYDip - kMessageButtonGapDip));
    }

    void refreshTheme() {
        primaryButton.refreshTheme();
        if (hasSecondary)
            secondaryButton.refreshTheme();
        surface.setBackdrop(false);
        surface.invalidate();
    }

    void finish(Result value) {
        result = value;
        if (hwnd)
            DestroyWindow(hwnd);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            fluent::styleDialogWindow(hwnd);
            surface.initialize(hwnd, false);
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            surface.invalidate();
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_DPICHANGED:
            resizeForDpi(LOWORD(wp));
            layout();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            fluent::restyleDialogWindow(hwnd, false);
            refreshTheme();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            surface.paint(hdc, false,
                          [this](fluent::FluentDialogSurface::Painter& painter, float, float) {
                              auto* format = painter.textFormat(kMessageFontSizeDip, 400, false);
                              painter.drawText(message, format, messageRect,
                                               fluent::palette().text);
                          });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            surface.eraseBackground(reinterpret_cast<HDC>(wp), false);
            return 1;
        case WM_COMMAND:
            if (HIWORD(wp) != BN_CLICKED)
                return 0;
            if (LOWORD(wp) == kPrimaryButtonId)
                finish(Result::Primary);
            else if (LOWORD(wp) == kSecondaryButtonId)
                finish(Result::Secondary);
            return 0;
        case WM_CLOSE:
            finish(hasSecondary ? Result::Secondary : Result::Closed);
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

} // namespace

Result showModal(HINSTANCE inst, HWND owner, const std::wstring& title,
                 const std::wstring& message, const wchar_t* primaryLabel,
                 const wchar_t* secondaryLabel, bool focusPrimary) {
    Impl dialog;
    dialog.inst = inst ? inst : GetModuleHandleW(nullptr);
    dialog.owner = owner;
    dialog.title = title;
    dialog.message = message;
    dialog.primaryLabel = primaryLabel ? primaryLabel : L"确定";
    dialog.secondaryLabel = secondaryLabel ? secondaryLabel : L"";
    dialog.hasSecondary = !dialog.secondaryLabel.empty();
    dialog.focusPrimary = focusPrimary;
    dialog.messageHeightDip = measureMessageHeightDip(
        dialog.message, kClientWidthDip - kPagePaddingDip * 2.0f);

    if (!dialog.create())
        return Result::Closed;

    HWND previousFocus = GetFocus();
    const bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled)
        EnableWindow(owner, FALSE);

    dialog.show();
    SetForegroundWindow(dialog.hwnd);
    SetActiveWindow(dialog.hwnd);
    HWND defaultButton = dialog.focusPrimary || !dialog.hasSecondary
                             ? dialog.primaryButton.hwnd()
                             : dialog.secondaryButton.hwnd();
    if (defaultButton)
        SetFocus(defaultButton);

    MSG msg{};
    int getMessageResult = 1;
    while (dialog.hwnd && (getMessageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            (msg.hwnd == dialog.hwnd || IsChild(dialog.hwnd, msg.hwnd))) {
            dialog.finish(dialog.hasSecondary ? Result::Secondary : Result::Closed);
            continue;
        }
        if (!IsDialogMessageW(dialog.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (dialog.hwnd)
        DestroyWindow(dialog.hwnd);
    if (ownerWasEnabled)
        EnableWindow(owner, TRUE);
    if (previousFocus && IsWindow(previousFocus))
        SetFocus(previousFocus);

    if (getMessageResult == 0)
        PostQuitMessage(static_cast<int>(msg.wParam));
    return dialog.result;
}

} // namespace message_dialog
