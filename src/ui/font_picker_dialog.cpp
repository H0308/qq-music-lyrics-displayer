#include "font_picker_dialog.h"

#include "ui/dialog_notify.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_theme.h"
#include "resource.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <map>

namespace {

constexpr int kIdFilterEdit = 201;
constexpr int kIdFamilyList = 202;
constexpr int kIdSizeEdit = 203;
constexpr int kIdSizeList = 204;
constexpr int kIdPreview = 205;
constexpr int kIdOk = 206;
constexpr int kIdCancel = 207;
constexpr int kIdTitleLabel = 208;
constexpr int kIdSubtitleLabel = 209;
constexpr int kIdStyleLabel = 210;
constexpr int kIdStyleGroup = 211;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
// 最小客户区仍需容纳字体列表、字号列表和底部预览。
constexpr float kMinClientWidthDip = 520.0f;
constexpr float kMinClientHeightDip = 468.0f;

void setMinimumTrackSize(HWND hwnd, MINMAXINFO* info) {
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi)
        dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(kMinClientWidthDip * s)),
            static_cast<LONG>(std::lround(kMinClientHeightDip * s))};
    if (!AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi))
        return;
    info->ptMinTrackSize.x = std::max(info->ptMinTrackSize.x, rc.right - rc.left);
    info->ptMinTrackSize.y = std::max(info->ptMinTrackSize.y, rc.bottom - rc.top);
}

constexpr int kSizePresets[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 32, 36, 40, 48};

std::wstring lowerOf(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r)
        c = static_cast<wchar_t>(std::towlower(c));
    return r;
}

// 示例文字预览面板
class FontPreviewPanel : public fluent::LayeredChild {
public:
    bool create(HWND parent, int id) {
        return createLayered(parent, L"QQMusicLyricFontPreview", wndProc, id);
    }

    void setFont(const std::wstring& family, float sizePt, LyricFontStyle style) {
        if (family == family_ && sizePt == size_ && style == style_)
            return;
        family_ = family;
        size_ = sizePt;
        style_ = style;
        if (fmt_) {
            fmt_->Release();
            fmt_ = nullptr;
        }
        renderNow();
    }

private:
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

        if (family_.empty() || size_ <= 0)
            return;
        if (!fmt_) {
            IDWriteFactory* dw = nullptr;
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(&dw))) ||
                !dw)
                return;
            dw->CreateTextFormat(family_.c_str(), nullptr, dwriteWeightOf(style_),
                                 dwriteStyleOf(style_), DWRITE_FONT_STRETCH_NORMAL, size_, L"zh-cn",
                                 &fmt_);
            dw->Release();
            if (fmt_) {
                fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (fmt_) {
            const wchar_t* sample = L"AaBb 永字八法 0123";
            br->SetColor(p.text);
            rt->DrawTextW(sample, static_cast<UINT32>(wcslen(sample)), fmt_,
                          D2D1::RectF(8.0f, 0.0f, wDip - 8.0f, hDip), br);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        FontPreviewPanel* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<FontPreviewPanel*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<FontPreviewPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
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
            if (self->fmt_) {
                self->fmt_->Release();
                self->fmt_ = nullptr;
            }
            self->hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    std::wstring family_;
    float size_ = 0;
    LyricFontStyle style_ = LyricFontStyle::Normal;
    IDWriteTextFormat* fmt_ = nullptr;
};

} // namespace

struct FontPickerDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed

    fluent::FluentLabel titleLabel;
    fluent::FluentLabel subtitleLabel;
    fluent::FluentEdit filterEdit;
    fluent::FluentList familyList;
    fluent::FluentLabel styleLabel;
    fluent::FluentRadioGroup styleGroup;
    fluent::FluentEdit sizeEdit;
    fluent::FluentList sizeList;
    FontPreviewPanel preview;
    fluent::FluentButton okBtn;
    fluent::FluentButton cancelBtn;

    std::wstring initFamily;
    float initSize = 16.0f;
    LyricFontStyle initStyle = LyricFontStyle::Normal;

    std::vector<std::wstring> families;         // 全部字体族
    std::vector<int> filtered;                  // 当前列表行 -> families 下标
    std::map<int, IDWriteTextFormat*> fmtCache; // 字体族 -> 行绘制格式

    ApplyCallback onApply;

    ~Impl() {
        for (auto& [_, f] : fmtCache)
            if (f)
                f->Release();
    }

    void refreshTheme() {
        titleLabel.refreshTheme();
        subtitleLabel.refreshTheme();
        filterEdit.refreshTheme();
        familyList.refreshTheme();
        styleLabel.refreshTheme();
        styleGroup.refreshTheme();
        sizeEdit.refreshTheme();
        sizeList.refreshTheme();
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
            fluent::styleDialogWindow(hwnd);
            enumerateFonts();
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            // 分层子窗口移动后不会替父窗口擦除拉伸产生的新客户区，完整重绘父子窗口。
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_GETMINMAXINFO:
            setMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp));
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            fluent::styleDialogWindow(hwnd);
            refreshTheme();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮消息默认发往焦点窗口，这里按光标位置转发给两个列表
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            HWND child = ChildWindowFromPoint(hwnd, pt);
            if (child == familyList.hwnd())
                SendMessageW(familyList.hwnd(), WM_MOUSEWHEEL, wp, lp);
            else if (child == sizeList.hwnd())
                SendMessageW(sizeList.hwnd(), WM_MOUSEWHEEL, wp, lp);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            // 分层子窗口需要一个稳定的不透明宿主底色，避免拉伸后暴露未初始化区域。
            fluent::paintDialogBackground(hwnd, hdc, false);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            fluent::paintDialogBackground(hwnd, reinterpret_cast<HDC>(wp), false);
            return 1;
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::FontPicker), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void enumerateFonts() {
        IDWriteFactory* dw = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dw))) ||
            !dw)
            return;
        IDWriteFontCollection* coll = nullptr;
        if (SUCCEEDED(dw->GetSystemFontCollection(&coll, FALSE)) && coll) {
            UINT32 count = coll->GetFontFamilyCount();
            for (UINT32 i = 0; i < count; ++i) {
                IDWriteFontFamily* fam = nullptr;
                if (FAILED(coll->GetFontFamily(i, &fam)) || !fam)
                    continue;
                IDWriteLocalizedStrings* names = nullptr;
                if (SUCCEEDED(fam->GetFamilyNames(&names)) && names) {
                    UINT32 idx = 0;
                    BOOL exists = FALSE;
                    if (FAILED(names->FindLocaleName(L"zh-cn", &idx, &exists)) || !exists) {
                        if (FAILED(names->FindLocaleName(L"en-us", &idx, &exists)) || !exists)
                            idx = 0;
                    }
                    UINT32 len = 0;
                    names->GetStringLength(idx, &len);
                    std::wstring name(len + 1, L'\0');
                    names->GetString(idx, name.data(), len + 1);
                    name.resize(len);
                    if (!name.empty())
                        families.push_back(name);
                    names->Release();
                }
                fam->Release();
            }
            coll->Release();
        }
        dw->Release();
        std::sort(families.begin(), families.end());
        families.erase(std::unique(families.begin(), families.end()), families.end());
    }

    void createControls() {
        titleLabel.create(hwnd, kIdTitleLabel, L"选择字体", false, 20.0f, 600);
        subtitleLabel.create(hwnd, kIdSubtitleLabel, L"搜索字体、字号和样式并实时预览", true, 13.0f, 400);
        filterEdit.create(hwnd, kIdFilterEdit, L"输入以筛选字体");
        familyList.create(hwnd, kIdFamilyList);
        familyList.setRowDraw([this](ID2D1DCRenderTarget* rt, const D2D1_RECT_F& rect, int row,
                                     bool, bool) { drawFamilyRow(rt, rect, row); });
        styleLabel.create(hwnd, kIdStyleLabel, L"样式", true, 13.0f, 400);
        styleGroup.create(hwnd, kIdStyleGroup);
        styleGroup.setOptions({L"常规", L"加粗", L"斜体", L"粗斜体"});
        styleGroup.setSelectedIndex(static_cast<int>(initStyle));
        sizeEdit.create(hwnd, kIdSizeEdit, L"字号");
        sizeList.create(hwnd, kIdSizeList);
        preview.create(hwnd, kIdPreview);
        okBtn.create(hwnd, kIdOk, L"确定", true);
        cancelBtn.create(hwnd, kIdCancel, L"取消", false);

        // 字号预设
        std::vector<fluent::FluentListItem> sizeItems;
        for (int s : kSizePresets)
            sizeItems.push_back({std::to_wstring(s), false});
        sizeList.setItems(std::move(sizeItems));

        rebuildFamilyList(L"");

        // 初始选中
        wchar_t buf[32];
        swprintf_s(buf, L"%g", static_cast<double>(initSize));
        sizeEdit.setText(buf);
        syncSizeListSelection();
        updatePreview();
    }

    void rebuildFamilyList(const std::wstring& filter) {
        std::wstring f = lowerOf(filter);
        filtered.clear();
        std::vector<fluent::FluentListItem> items;
        for (int i = 0; i < static_cast<int>(families.size()); ++i) {
            if (f.empty() || lowerOf(families[i]).find(f) != std::wstring::npos) {
                filtered.push_back(i);
                items.push_back({families[i], false});
            }
        }
        familyList.setItems(std::move(items));

        // 尽量保持当前字体选中
        int sel = -1;
        std::wstring cur = lowerOf(currentFamily());
        for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
            if (lowerOf(families[filtered[i]]) == cur) {
                sel = i;
                break;
            }
        }
        if (sel < 0 && !filtered.empty())
            sel = 0;
        familyList.setSelectedIndex(sel);
    }

    std::wstring currentFamily() const {
        int row = familyList.selectedIndex();
        if (row >= 0 && row < static_cast<int>(filtered.size()))
            return families[filtered[row]];
        return initFamily;
    }

    float currentSize() const {
        std::wstring t = sizeEdit.text();
        try {
            float v = std::stof(t);
            return std::clamp(v, 4.0f, 96.0f);
        } catch (...) {
            return initSize;
        }
    }

    LyricFontStyle currentStyle() const {
        int index = styleGroup.selectedIndex();
        if (index < 0 || index > static_cast<int>(LyricFontStyle::BoldItalic))
            return initStyle;
        return static_cast<LyricFontStyle>(index);
    }

    void updatePreview() {
        preview.setFont(currentFamily(), currentSize(), currentStyle());
    }

    void syncSizeListSelection() {
        int size = static_cast<int>(currentSize() + 0.5f);
        for (int i = 0; i < sizeList.itemCount(); ++i) {
            if (std::stoi(sizeListText(i)) == size) {
                sizeList.setSelectedIndex(i);
                return;
            }
        }
    }

    // FluentList 未提供取行文本接口，这里直接用预设表
    std::wstring sizeListText(int row) const {
        return std::to_wstring(kSizePresets[row]);
    }

    void drawFamilyRow(ID2D1DCRenderTarget* rt, const D2D1_RECT_F& rect, int row) {
        if (row < 0 || row >= static_cast<int>(filtered.size()))
            return;
        int famIdx = filtered[row];
        IDWriteTextFormat* fmt = nullptr;
        auto it = fmtCache.find(famIdx);
        if (it != fmtCache.end()) {
            fmt = it->second;
        } else {
            IDWriteFactory* dw = nullptr;
            if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                              __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(&dw))) &&
                dw) {
                if (SUCCEEDED(dw->CreateTextFormat(families[famIdx].c_str(), nullptr,
                                                   DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-cn",
                                                   &fmt)) &&
                    fmt) {
                    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    fmtCache[famIdx] = fmt;
                }
                dw->Release();
            }
        }
        if (!fmt)
            return;
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(fluent::palette().text, &br);
        if (!br)
            return;
        const std::wstring& name = families[famIdx];
        rt->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()), fmt,
                      D2D1::RectF(rect.left + 16.0f, rect.top, rect.right - 12.0f, rect.bottom),
                      br);
        br->Release();
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

        int filterH = px(fluent::metrics::controlHeight);
        int filterY = pad + titleH + subtitleH + px(fluent::metrics::sectionGap);
        filterEdit.move(pad, filterY, w - pad * 2, filterH);

        int btnH = px(fluent::metrics::controlHeight);
        int okW = px(96), cancelW = px(88);
        int btnY = h - pad - btnH;
        okBtn.move(w - pad - okW - cancelW - gap, btnY, okW, btnH);
        cancelBtn.move(w - pad - cancelW, btnY, cancelW, btnH);

        int previewH = px(96);
        int previewY = btnY - gap - previewH;
        preview.move(pad, previewY, w - pad * 2, previewH);

        int listTop = filterY + filterH + gap;
        int styleH = px(fluent::metrics::controlHeight);
        int styleLabelW = px(48.0f);
        styleLabel.move(pad, listTop, styleLabelW, styleH);
        styleGroup.move(pad + styleLabelW + gap, listTop,
                        w - pad * 2 - styleLabelW - gap, styleH);

        listTop += styleH + gap;
        int listH = previewY - gap - listTop;
        int familyW = static_cast<int>((w - pad * 2 - gap) * 0.58f);
        familyList.move(pad, listTop, familyW, listH);

        int rightX = pad + familyW + gap;
        int rightW = w - pad - rightX;
        int sizeEditH = px(fluent::metrics::controlHeight);
        sizeEdit.move(rightX, listTop, rightW, sizeEditH);
        int sizeGap = px(fluent::metrics::compactGap);
        sizeList.move(rightX, listTop + sizeEditH + sizeGap, rightW,
                      listH - sizeEditH - sizeGap);
    }

    void onCommand(int id, int code) {
        if (id == kIdCancel && code == BN_CLICKED) {
            destroy();
        } else if (id == kIdOk && code == BN_CLICKED) {
            applyAndClose();
        } else if (id == kIdFamilyList && code == LBN_SELCHANGE) {
            updatePreview();
        } else if (id == kIdFamilyList && code == LBN_DBLCLK) {
            applyAndClose();
        } else if (id == kIdStyleGroup && code == BN_CLICKED) {
            updatePreview();
        } else if (id == kIdSizeList && code == LBN_SELCHANGE) {
            int row = sizeList.selectedIndex();
            if (row >= 0)
                sizeEdit.setText(sizeListText(row));
            updatePreview();
        } else if (id == kIdSizeEdit && code == EN_CHANGE) {
            updatePreview();
        } else if (id == kIdFilterEdit && code == EN_CHANGE) {
            rebuildFamilyList(filterEdit.text());
            updatePreview();
        }
    }

    void applyAndClose() {
        if (onApply)
            onApply(currentFamily(), currentSize(), currentStyle());
        destroy();
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

FontPickerDialog::FontPickerDialog() : impl_(std::make_unique<Impl>()) {}

FontPickerDialog::~FontPickerDialog() {
    destroy();
}

bool FontPickerDialog::create(HINSTANCE inst, HWND parent, const std::wstring& family,
                              float sizePt, LyricFontStyle style) {
    impl_->notifyHwnd = parent; // 仅用于关闭通知；托盘窗口是消息窗口，不能作为所有者
    impl_->inst = inst;
    impl_->initFamily = family;
    impl_->initSize = sizePt;
    impl_->initStyle = style;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricFontPicker";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    // 期望的客户区尺寸，按标题栏/边框反推窗口整体尺寸
    RECT rc{0, 0, static_cast<LONG>(std::lround(600 * s)),
            static_cast<LONG>(std::lround(560 * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricFontPicker", L"选择字体",
                                  kDialogStyle, x, y, w, h, nullptr, nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;
    return true;
}

void FontPickerDialog::show() {
    if (impl_->hwnd)
        ShowWindow(impl_->hwnd, SW_SHOW);
}

void FontPickerDialog::destroy() {
    impl_->destroy();
}

bool FontPickerDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND FontPickerDialog::hwnd() const {
    return impl_->hwnd;
}

void FontPickerDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}
