#include "idle_app_name_dialog.h"

#include "ui/app_icon.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kIdTitle = 501;
constexpr int kIdSubtitle = 502;
constexpr int kIdNameEdit = 503;
constexpr int kIdOk = 504;
constexpr int kIdCancel = 505;
constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
constexpr float kClientW = 360.0f;
constexpr float kBaseClientH = 188.0f;
constexpr float kSubtitleLineHeightDip = 20.0f;

float measureSubtitleHeightDip(const std::wstring& text, float widthDip) {
    if (text.empty())
        return kSubtitleLineHeightDip;

    IDWriteFactory* factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&factory))) ||
        !factory)
        return kSubtitleLineHeightDip;

    IDWriteTextFormat* format = nullptr;
    const HRESULT formatResult = factory->CreateTextFormat(
        fluent::uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &format);
    if (FAILED(formatResult) || !format) {
        factory->Release();
        return kSubtitleLineHeightDip;
    }
    fluent::applyUiFontFallback(format);

    IDWriteTextLayout* layout = nullptr;
    const HRESULT layoutResult = factory->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()), format,
        std::max(1.0f, widthDip), 1000.0f, &layout);
    float height = kSubtitleLineHeightDip;
    if (SUCCEEDED(layoutResult) && layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics)))
            height = std::max(height, std::ceil(metrics.height));
        layout->Release();
    }
    format->Release();
    factory->Release();
    return height;
}

} // namespace

struct IdleAppNameDialog::Impl {
    HWND hwnd = nullptr;
    bool backdrop = false;

    fluent::FluentLabel titleLabel;
    fluent::FluentLabel subtitleLabel;
    fluent::FluentEdit nameEdit;
    fluent::FluentButton okButton;
    fluent::FluentButton cancelButton;

    std::wstring initial;
    std::wstring title;
    std::wstring subtitle;
    std::wstring placeholder;
    int maxLength = 0;
    float subtitleHeightDipValue = kSubtitleLineHeightDip;
    ApplyCallback onApply;

    float subtitleHeightDip() const {
        return subtitleHeightDipValue;
    }

    float clientHeightDip() const {
        return kBaseClientH + subtitleHeightDip() - kSubtitleLineHeightDip;
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

    void refreshTheme() {
        titleLabel.refreshTheme();
        subtitleLabel.refreshTheme();
        nameEdit.refreshTheme();
        okButton.refreshTheme();
        cancelButton.refreshTheme();
    }

    void createControls() {
        titleLabel.create(hwnd, kIdTitle, title.c_str(), false, 20.0f, 600);
        subtitleLabel.create(hwnd, kIdSubtitle, subtitle.c_str(), true, 13.0f, 400);
        nameEdit.create(hwnd, kIdNameEdit, placeholder.c_str());
        if (maxLength > 0)
            SendMessageW(nameEdit.editHwnd(), EM_SETLIMITTEXT,
                         static_cast<WPARAM>(maxLength), 0);
        nameEdit.setText(initial);
        okButton.create(hwnd, kIdOk, L"确定", true);
        cancelButton.create(hwnd, kIdCancel, L"取消", false);
    }

    void layout() {
        if (!hwnd)
            return;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = fluent::dipScale(GetDpiForWindow(hwnd));
        const auto px = [s](float dip) { return static_cast<int>(std::lround(dip * s)); };
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int pad = px(fluent::metrics::pagePadding);
        const int gap = px(fluent::metrics::controlGap);
        const int titleH = px(28.0f);
        const int subtitleH = px(subtitleHeightDip());
        const int editH = px(fluent::metrics::controlHeight);
        const int buttonH = px(fluent::metrics::controlHeight);

        titleLabel.move(pad, pad, w - pad * 2, titleH);
        subtitleLabel.move(pad, pad + titleH, w - pad * 2, subtitleH);
        const int editY = pad + titleH + subtitleH + px(fluent::metrics::sectionGap);
        nameEdit.move(pad, editY, w - pad * 2, editH);

        const int cancelW = px(88.0f);
        const int okW = px(96.0f);
        const int buttonY = h - pad - buttonH;
        cancelButton.move(w - pad - cancelW, buttonY, cancelW, buttonH);
        okButton.move(w - pad - cancelW - gap - okW, buttonY, okW, buttonH);
    }

    void destroy() {
        if (hwnd)
            DestroyWindow(hwnd);
    }

    void onCommand(int id, int code) {
        if (code != BN_CLICKED)
            return;
        if (id == kIdCancel) {
            destroy();
        } else if (id == kIdOk) {
            if (onApply)
                onApply(nameEdit.text());
            destroy();
        }
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
        case WM_GETMINMAXINFO:
            fluent::setDialogMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp),
                                               kDialogStyle, kDialogExStyle, kClientW,
                                               clientHeightDip());
            return 0;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
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
};

IdleAppNameDialog::IdleAppNameDialog() : impl_(std::make_unique<Impl>()) {}

IdleAppNameDialog::~IdleAppNameDialog() {
    destroy();
}

bool IdleAppNameDialog::create(HINSTANCE inst, HWND parent, const std::wstring& initial,
                               const wchar_t* title, const wchar_t* subtitle,
                               const wchar_t* placeholder, int maxLength) {
    impl_->initial = initial;
    impl_->title = title ? title : L"";
    impl_->subtitle = subtitle ? subtitle : L"";
    impl_->placeholder = placeholder ? placeholder : L"";
    impl_->maxLength = maxLength;
    impl_->subtitleHeightDipValue = measureSubtitleHeightDip(
        impl_->subtitle, kClientW - fluent::metrics::pagePadding * 2.0f);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricIdleAppNameDialog";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    UINT dpi = parent ? GetDpiForWindow(parent) : GetDpiForSystem();
    if (dpi == 0)
        dpi = GetDpiForSystem();
    const float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(kClientW * s)),
            static_cast<LONG>(std::lround(impl_->clientHeightDip() * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    RECT ownerRect{};
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const bool hasOwnerRect = parent && GetWindowRect(parent, &ownerRect);
    const RECT& base = hasOwnerRect ? ownerRect : work;
    const int x = base.left + ((base.right - base.left) - w) / 2;
    const int y = base.top + ((base.bottom - base.top) - h) / 2;
    impl_->hwnd = CreateWindowExW(kDialogExStyle,
                                  wc.lpszClassName, impl_->title.c_str(),
                                  kDialogStyle | WS_VISIBLE,
                                  x, y, w, h, parent, nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;
    app_icon::applyWindowIcon(impl_->hwnd);
    return true;
}

void IdleAppNameDialog::show() {
    if (!impl_->hwnd)
        return;
    ShowWindow(impl_->hwnd, SW_SHOW);
    SetForegroundWindow(impl_->hwnd);
    impl_->nameEdit.focus();
}

void IdleAppNameDialog::destroy() {
    impl_->destroy();
}

bool IdleAppNameDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND IdleAppNameDialog::hwnd() const {
    return impl_->hwnd;
}

void IdleAppNameDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}
