#include "font_picker_dialog.h"

#include "resource.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kIdFilterEdit = 201;
constexpr int kIdFamilyList = 202;
constexpr int kIdSizeEdit = 203;
constexpr int kIdSizeList = 204;
constexpr int kIdOk = 206;
constexpr int kIdCancel = 207;
constexpr int kIdStyleGroup = 211;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
constexpr float kMinClientWidthDip = 520.0f;
constexpr float kMinClientHeightDip = 468.0f;

constexpr float kListRowHeight = 32.0f;
constexpr float kScrollBarWidth = 3.0f;
constexpr float kScrollBarHitWidth = 12.0f;
constexpr float kScrollBarInset = 8.0f;

constexpr int kSizePresets[] = {8, 9, 10, 11, 12, 14, 16, 18, 20,
                                22, 24, 26, 28, 32, 36, 40, 48};

const std::array<const wchar_t*, 4>& styleNames() {
    static const std::array<const wchar_t*, 4> names = {L"常规", L"加粗", L"斜体", L"粗斜体"};
    return names;
}

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

std::wstring lowerOf(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r)
        c = static_cast<wchar_t>(std::towlower(c));
    return r;
}

struct ScrollBarGeometry {
    float trackTop = 0.0f;
    float thumbHeight = 0.0f;
    float usable = 0.0f;
};

ScrollBarGeometry scrollBarGeometry(float viewHeight, float contentHeight) {
    float trackHeight = std::max(0.0f, viewHeight - kScrollBarInset * 2.0f);
    float thumbHeight = std::min(
        trackHeight, std::max(20.0f, trackHeight * viewHeight / std::max(1.0f, contentHeight)));
    return {kScrollBarInset, thumbHeight, std::max(0.0f, trackHeight - thumbHeight)};
}

float scrollBarThumbY(const ScrollBarGeometry& bar, float scrollY, float maxScroll) {
    if (maxScroll <= 0.0f || bar.usable <= 0.0f)
        return bar.trackTop;
    return bar.trackTop + scrollY / maxScroll * bar.usable;
}

} // namespace

struct FontPickerDialog::Impl {
    struct TextField {
        std::wstring text;
        std::wstring cue;
        size_t caret = 0;
        size_t anchor = 0;
    };

    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;

    fluent::FluentDialogSurface surface;

    std::wstring initFamily;
    float initSize = 16.0f;
    LyricFontStyle initStyle = LyricFontStyle::Normal;

    TextField filterEdit;
    TextField sizeEdit;
    std::vector<std::wstring> families;
    std::vector<int> filtered;
    std::map<int, IDWriteTextFormat*> familyFormats;
    int familySelected = -1;
    int sizeSelected = -1;
    int styleSelected = 0;

    D2D1_RECT_F titleRect{};
    D2D1_RECT_F subtitleRect{};
    D2D1_RECT_F filterRect{};
    D2D1_RECT_F styleLabelRect{};
    D2D1_RECT_F styleRect{};
    D2D1_RECT_F familyListRect{};
    D2D1_RECT_F sizeEditRect{};
    D2D1_RECT_F sizeListRect{};
    D2D1_RECT_F previewRect{};
    D2D1_RECT_F okRect{};
    D2D1_RECT_F cancelRect{};
    std::vector<D2D1_RECT_F> styleOptionRects;

    float familyScroll = 0.0f;
    float sizeScroll = 0.0f;
    int familyWheelAccum = 0;
    int sizeWheelAccum = 0;
    bool scrollDragging = false;
    bool draggingFamily = false;
    float scrollDragGrabDy = 0.0f;

    int hoverId = 0;
    int hoverRow = -1;
    int hoverOption = -1;
    int pressedId = 0;
    int pressedOption = -1;
    int focusedId = kIdFilterEdit;
    bool focusVisible = false;

    ApplyCallback onApply;

    ~Impl() {
        for (auto& [_, format] : familyFormats) {
            if (format)
                format->Release();
        }
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

    static bool contains(const D2D1_RECT_F& rect, float x, float y) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    static bool isCtrlDown() {
        return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    static bool isShiftDown() {
        return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    TextField* editFor(int id) {
        if (id == kIdFilterEdit)
            return &filterEdit;
        if (id == kIdSizeEdit)
            return &sizeEdit;
        return nullptr;
    }

    const TextField* editFor(int id) const {
        if (id == kIdFilterEdit)
            return &filterEdit;
        if (id == kIdSizeEdit)
            return &sizeEdit;
        return nullptr;
    }

    static size_t selectionStart(const TextField& field) {
        return std::min(field.caret, field.anchor);
    }

    static size_t selectionEnd(const TextField& field) {
        return std::max(field.caret, field.anchor);
    }

    static void collapseSelection(TextField& field, bool toEnd) {
        size_t pos = toEnd ? selectionEnd(field) : selectionStart(field);
        field.caret = pos;
        field.anchor = pos;
    }

    void moveCaret(TextField& field, size_t next, bool extend) {
        next = std::min(next, field.text.size());
        if (extend)
            field.caret = next;
        else
            field.caret = field.anchor = next;
        surface.invalidate();
    }

    void replaceSelection(TextField& field, const std::wstring& inserted, bool numeric) {
        const size_t start = selectionStart(field);
        const size_t end = selectionEnd(field);
        std::wstring base = field.text;
        base.erase(start, end - start);

        std::wstring accepted;
        bool hasDot = base.find(L'.') != std::wstring::npos;
        for (wchar_t c : inserted) {
            if (numeric) {
                if (std::iswdigit(c)) {
                    accepted.push_back(c);
                } else if (c == L'.' && !hasDot) {
                    accepted.push_back(c);
                    hasDot = true;
                }
            } else if (c >= L' ' && c != 0x7f) {
                accepted.push_back(c);
            }
        }

        field.text = base.substr(0, start) + accepted + base.substr(start);
        field.caret = start + accepted.size();
        field.anchor = field.caret;
    }

    void copySelection(const TextField& field) const {
        const size_t start = selectionStart(field);
        const size_t end = selectionEnd(field);
        if (start == end || !OpenClipboard(hwnd))
            return;

        EmptyClipboard();
        const std::wstring text = field.text.substr(start, end - start);
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* data = GlobalLock(memory);
            if (data) {
                std::memcpy(data, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory))
                    GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    std::wstring clipboardText() const {
        std::wstring result;
        if (!OpenClipboard(hwnd))
            return result;
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text)
                result = text;
            if (text)
                GlobalUnlock(handle);
        }
        CloseClipboard();
        return result;
    }

    void editChanged(int id) {
        if (id == kIdFilterEdit)
            rebuildFamilyList(filterEdit.text);
        surface.invalidate();
    }

    bool handleEditKey(WPARAM key) {
        TextField* field = editFor(focusedId);
        if (!field)
            return false;

        const bool numeric = focusedId == kIdSizeEdit;
        const bool ctrl = isCtrlDown();
        const bool shift = isShiftDown();
        if (ctrl && (key == 'A' || key == 'a')) {
            field->anchor = 0;
            field->caret = field->text.size();
            surface.invalidate();
            return true;
        }
        if (ctrl && (key == 'C' || key == 'c')) {
            copySelection(*field);
            return true;
        }
        if (ctrl && (key == 'X' || key == 'x')) {
            copySelection(*field);
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"", numeric);
                editChanged(focusedId);
            }
            return true;
        }
        if (ctrl && (key == 'V' || key == 'v')) {
            replaceSelection(*field, clipboardText(), numeric);
            editChanged(focusedId);
            return true;
        }

        switch (key) {
        case VK_LEFT:
            if (!shift && selectionStart(*field) != selectionEnd(*field))
                collapseSelection(*field, false);
            else if (field->caret > 0)
                moveCaret(*field, field->caret - 1, shift);
            else
                surface.invalidate();
            return true;
        case VK_RIGHT:
            if (!shift && selectionStart(*field) != selectionEnd(*field))
                collapseSelection(*field, true);
            else if (field->caret < field->text.size())
                moveCaret(*field, field->caret + 1, shift);
            else
                surface.invalidate();
            return true;
        case VK_HOME:
            moveCaret(*field, 0, shift);
            return true;
        case VK_END:
            moveCaret(*field, field->text.size(), shift);
            return true;
        case VK_BACK:
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"", numeric);
            } else if (field->caret > 0) {
                const size_t start = field->caret - 1;
                field->text.erase(start, 1);
                field->caret = field->anchor = start;
            }
            editChanged(focusedId);
            return true;
        case VK_DELETE:
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"", numeric);
            } else if (field->caret < field->text.size()) {
                field->text.erase(field->caret, 1);
            }
            editChanged(focusedId);
            return true;
        default:
            return false;
        }
    }

    bool handleCharacter(wchar_t character) {
        TextField* field = editFor(focusedId);
        if (!field || character < L' ' || character == 0x7f)
            return false;
        const bool numeric = focusedId == kIdSizeEdit;
        if (numeric && !std::iswdigit(character) && character != L'.')
            return true;
        replaceSelection(*field, std::wstring(1, character), numeric);
        editChanged(focusedId);
        return true;
    }

    void enumerateFonts() {
        families.clear();
        for (auto& [_, format] : familyFormats) {
            if (format)
                format->Release();
        }
        familyFormats.clear();

        IDWriteFactory* dw = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dw))) ||
            !dw)
            return;
        IDWriteFontCollection* collection = nullptr;
        if (SUCCEEDED(dw->GetSystemFontCollection(&collection, FALSE)) && collection) {
            const UINT32 count = collection->GetFontFamilyCount();
            for (UINT32 i = 0; i < count; ++i) {
                IDWriteFontFamily* family = nullptr;
                if (FAILED(collection->GetFontFamily(i, &family)) || !family)
                    continue;
                IDWriteLocalizedStrings* names = nullptr;
                if (SUCCEEDED(family->GetFamilyNames(&names)) && names) {
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    if (FAILED(names->FindLocaleName(L"zh-cn", &index, &exists)) || !exists) {
                        if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists)
                            index = 0;
                    }
                    UINT32 length = 0;
                    names->GetStringLength(index, &length);
                    std::wstring name(length + 1, L'\0');
                    names->GetString(index, name.data(), length + 1);
                    name.resize(length);
                    if (!name.empty())
                        families.push_back(std::move(name));
                    names->Release();
                }
                family->Release();
            }
            collection->Release();
        }
        dw->Release();
        std::sort(families.begin(), families.end());
        families.erase(std::unique(families.begin(), families.end()), families.end());
    }

    void createControls() {
        filterEdit.cue = L"输入以筛选字体";
        sizeEdit.cue = L"字号";
        filterEdit.text.clear();
        filterEdit.caret = filterEdit.anchor = 0;

        wchar_t buf[32];
        swprintf_s(buf, L"%g", static_cast<double>(initSize));
        sizeEdit.text = buf;
        sizeEdit.caret = sizeEdit.anchor = sizeEdit.text.size();

        familySelected = -1;
        sizeSelected = -1;
        styleSelected = static_cast<int>(initStyle);
        focusedId = kIdFilterEdit;
        focusVisible = false;
        familyScroll = 0.0f;
        sizeScroll = 0.0f;
        familyWheelAccum = 0;
        sizeWheelAccum = 0;
        rebuildFamilyList(L"");
        syncSizeListSelection();
    }

    void rebuildFamilyList(const std::wstring& filter) {
        const std::wstring previous = currentFamily();
        const std::wstring loweredFilter = lowerOf(filter);
        filtered.clear();
        for (int i = 0; i < static_cast<int>(families.size()); ++i) {
            if (loweredFilter.empty() ||
                lowerOf(families[i]).find(loweredFilter) != std::wstring::npos)
                filtered.push_back(i);
        }

        familySelected = -1;
        const std::wstring loweredPrevious = lowerOf(previous);
        for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
            if (lowerOf(families[filtered[i]]) == loweredPrevious) {
                familySelected = i;
                break;
            }
        }
        if (familySelected < 0 && !filtered.empty())
            familySelected = 0;
        familyScroll = 0.0f;
        ensureVisible(true, familySelected);
    }

    std::wstring currentFamily() const {
        if (familySelected >= 0 && familySelected < static_cast<int>(filtered.size()))
            return families[filtered[familySelected]];
        return initFamily;
    }

    float currentSize() const {
        try {
            return std::clamp(std::stof(sizeEdit.text), 4.0f, 96.0f);
        } catch (...) {
            return initSize;
        }
    }

    LyricFontStyle currentStyle() const {
        if (styleSelected < 0 || styleSelected >= static_cast<int>(styleNames().size()))
            return initStyle;
        return static_cast<LyricFontStyle>(styleSelected);
    }

    void syncSizeListSelection() {
        const int rounded = static_cast<int>(currentSize() + 0.5f);
        sizeSelected = -1;
        for (int i = 0; i < static_cast<int>(_countof(kSizePresets)); ++i) {
            if (kSizePresets[i] == rounded) {
                sizeSelected = i;
                break;
            }
        }
        ensureVisible(false, sizeSelected);
    }

    float listContentHeight(bool family) const {
        const int count = family ? static_cast<int>(filtered.size())
                                 : static_cast<int>(_countof(kSizePresets));
        return count * kListRowHeight;
    }

    const D2D1_RECT_F& listRect(bool family) const {
        return family ? familyListRect : sizeListRect;
    }

    float& listScroll(bool family) {
        return family ? familyScroll : sizeScroll;
    }

    const float& listScroll(bool family) const {
        return family ? familyScroll : sizeScroll;
    }

    float listMaxScroll(bool family) const {
        const auto& rect = listRect(family);
        return std::max(0.0f, listContentHeight(family) - (rect.bottom - rect.top));
    }

    void clampScroll(bool family) {
        listScroll(family) = std::clamp(listScroll(family), 0.0f, listMaxScroll(family));
    }

    void ensureVisible(bool family, int row) {
        if (row < 0)
            return;
        const auto& rect = listRect(family);
        const float viewHeight = rect.bottom - rect.top;
        const float top = row * kListRowHeight;
        const float bottom = top + kListRowHeight;
        float& scroll = listScroll(family);
        if (top < scroll)
            scroll = top;
        else if (bottom > scroll + viewHeight)
            scroll = bottom - viewHeight;
        clampScroll(family);
    }

    int rowAt(bool family, float x, float y) const {
        const auto& rect = listRect(family);
        if (!contains(rect, x, y))
            return -1;
        const float localY = y - rect.top + listScroll(family);
        const int row = static_cast<int>(localY / kListRowHeight);
        const int count = family ? static_cast<int>(filtered.size())
                                 : static_cast<int>(_countof(kSizePresets));
        return row >= 0 && row < count ? row : -1;
    }

    void scrollBy(bool family, int delta) {
        int& accumulator = family ? familyWheelAccum : sizeWheelAccum;
        accumulator += delta;
        const int notches = accumulator / WHEEL_DELTA;
        if (notches == 0)
            return;
        accumulator -= notches * WHEEL_DELTA;
        listScroll(family) = std::clamp(listScroll(family) - notches * 3.0f * kListRowHeight,
                                        0.0f, listMaxScroll(family));
        surface.invalidate();
    }

    bool listHasScroll(bool family) const {
        return listContentHeight(family) > listRect(family).bottom - listRect(family).top;
    }

    bool isScrollBarHit(bool family, float x) const {
        const auto& rect = listRect(family);
        return listHasScroll(family) && x >= rect.right - kScrollBarHitWidth;
    }

    int styleOptionAt(float x, float y) const {
        for (int i = 0; i < static_cast<int>(styleOptionRects.size()); ++i) {
            if (contains(styleOptionRects[i], x, y))
                return i;
        }
        return -1;
    }

    IDWriteTextFormat* familyFormat(IDWriteFactory* dw, int familyIndex) {
        auto it = familyFormats.find(familyIndex);
        if (it != familyFormats.end())
            return it->second;
        if (!dw || familyIndex < 0 || familyIndex >= static_cast<int>(families.size()))
            return nullptr;

        IDWriteTextFormat* format = nullptr;
        if (FAILED(dw->CreateTextFormat(families[familyIndex].c_str(), nullptr,
                                         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-cn", &format)) ||
            !format)
            return nullptr;
        fluent::applyUiFontFallback(format);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        familyFormats.emplace(familyIndex, format);
        return format;
    }

    void drawTextField(fluent::FluentDialogSurface::Painter& painter,
                       const D2D1_RECT_F& rect, const TextField& field, int id) {
        const auto& p = fluent::palette();
        const bool focused = focusedId == id;
        painter.fillRoundRect(p.cardFill, rect, fluent::metrics::controlRadius);
        painter.strokeRoundRect(p.cardStroke, rect, 1.0f, fluent::metrics::controlRadius);

        const float underlineHeight = focused ? 2.0f : 1.0f;
        if (auto* br = painter.brush(focused ? p.accent : p.separator)) {
            painter.target()->FillRectangle(
                D2D1::RectF(rect.left + 1.0f, rect.bottom - underlineHeight, rect.right - 1.0f,
                            rect.bottom),
                br);
        }

        auto* format = painter.textFormat(13.0f, 400, false, true);
        if (!format)
            return;
        const D2D1_RECT_F textRect = D2D1::RectF(rect.left + 12.0f, rect.top,
                                                 rect.right - 12.0f, rect.bottom);
        const bool hasSelection = selectionStart(field) != selectionEnd(field);
        if (!field.text.empty() && hasSelection) {
            const std::wstring before = field.text.substr(0, selectionStart(field));
            const std::wstring selected =
                field.text.substr(selectionStart(field), selectionEnd(field) - selectionStart(field));
            const float left = textRect.left + painter.measureTextWidth(before, format);
            const float right = std::min(textRect.right,
                                         left + painter.measureTextWidth(selected, format));
            if (right > left)
                painter.target()->FillRectangle(
                    D2D1::RectF(left, textRect.top + 6.0f, right, textRect.bottom - 6.0f),
                    painter.brush(p.listSelected));
        }

        if (field.text.empty())
            painter.drawText(field.cue, format, textRect, p.textSecondary);
        else
            painter.drawText(field.text, format, textRect, p.text);

        if (focused) {
            const std::wstring before = field.text.substr(0, field.caret);
            const float caretX = std::clamp(textRect.left + painter.measureTextWidth(before, format),
                                            textRect.left, textRect.right - 1.0f);
            if (auto* br = painter.brush(p.accent))
                painter.target()->FillRectangle(
                    D2D1::RectF(caretX, textRect.top + 7.0f, caretX + 1.0f,
                                textRect.bottom - 7.0f),
                    br);
        }
    }

    void drawStyleGroup(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        auto* format = painter.textFormat(13.0f, 400, false, true);
        if (!format)
            return;

        styleOptionRects.clear();
        constexpr float circleSize = 16.0f;
        constexpr float textGap = 8.0f;
        constexpr float optionGap = 20.0f;
        const float cy = (styleRect.top + styleRect.bottom) * 0.5f;
        float x = styleRect.left;
        for (int i = 0; i < static_cast<int>(styleNames().size()); ++i) {
            const std::wstring text = styleNames()[i];
            const float textWidth = painter.measureTextWidth(text, format);
            const D2D1_RECT_F optionRect =
                D2D1::RectF(x, styleRect.top, x + circleSize + textGap + textWidth,
                            styleRect.bottom);
            styleOptionRects.push_back(optionRect);
            const bool selected = i == styleSelected;
            const bool hovered = hoverId == kIdStyleGroup && hoverOption == i;
            const bool pressed = pressedId == kIdStyleGroup && pressedOption == i;
            D2D1_ELLIPSE circle{D2D1::Point2F(x + circleSize * 0.5f, cy), circleSize * 0.5f,
                                circleSize * 0.5f};
            if (selected) {
                painter.target()->FillEllipse(
                    circle, painter.brush(hovered || pressed ? p.accentHover : p.accent));
                painter.target()->FillEllipse(D2D1::Ellipse(circle.point, 4.0f, 4.0f),
                                              painter.brush(p.textOnAccent));
            } else {
                if (hovered)
                    painter.target()->FillEllipse(circle, painter.brush(p.listHover));
                painter.target()->DrawEllipse(circle, painter.brush(p.textSecondary),
                                              pressed ? 1.5f : 1.0f);
            }
            painter.drawText(text, format,
                             D2D1::RectF(x + circleSize + textGap, styleRect.top,
                                         x + circleSize + textGap + textWidth + 1.0f,
                                         styleRect.bottom),
                             p.text);
            x += circleSize + textGap + textWidth + optionGap;
        }

        if (focusedId == kIdStyleGroup && focusVisible && styleSelected >= 0 &&
            styleSelected < static_cast<int>(styleOptionRects.size())) {
            const auto& rect = styleOptionRects[styleSelected];
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
        }
    }

    void drawList(fluent::FluentDialogSurface::Painter& painter, bool family) {
        const auto& p = fluent::palette();
        const auto& rect = listRect(family);
        const float height = rect.bottom - rect.top;
        float& scroll = listScroll(family);
        clampScroll(family);

        painter.fillRoundRect(p.cardFill, rect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, rect, 1.0f, fluent::metrics::cardRadius);
        if (focusedId == (family ? kIdFamilyList : kIdSizeList) && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }

        painter.target()->PushAxisAlignedClip(
            D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const int count = family ? static_cast<int>(filtered.size())
                                 : static_cast<int>(_countof(kSizePresets));
        for (int i = 0; i < count; ++i) {
            const float y = rect.top + i * kListRowHeight - scroll;
            const D2D1_RECT_F rowRect = D2D1::RectF(rect.left, y, rect.right, y + kListRowHeight);
            if (rowRect.bottom < rect.top || rowRect.top > rect.bottom)
                continue;
            const bool selected = i == (family ? familySelected : sizeSelected);
            const bool hovered = hoverId == (family ? kIdFamilyList : kIdSizeList) &&
                                 hoverRow == i;
            if (selected)
                painter.fillRoundRect(
                    p.listSelected,
                    D2D1::RectF(rect.left + 4.0f, y + 2.0f, rect.right - 4.0f,
                                y + kListRowHeight - 2.0f));
            else if (hovered)
                painter.fillRoundRect(
                    p.listHover,
                    D2D1::RectF(rect.left + 4.0f, y + 2.0f, rect.right - 4.0f,
                                y + kListRowHeight - 2.0f));
            if (selected)
                painter.fillRoundRect(
                    p.accent,
                    D2D1::RectF(rect.left + 7.0f, y + kListRowHeight * 0.5f - 8.0f,
                                rect.left + 10.0f, y + kListRowHeight * 0.5f + 8.0f),
                    1.5f);

            if (family) {
                const int familyIndex = filtered[i];
                if (auto* format = familyFormat(painter.dwrite(), familyIndex))
                    painter.drawTrimmedText(families[familyIndex], format,
                                            D2D1::RectF(rect.left + 16.0f, y,
                                                        rect.right - 12.0f,
                                                        y + kListRowHeight),
                                            p.text);
            } else {
                painter.drawText(std::to_wstring(kSizePresets[i]),
                                 painter.textFormat(13.0f, 400, false, true),
                                 D2D1::RectF(rect.left + 16.0f, y, rect.right - 12.0f,
                                             y + kListRowHeight),
                                 p.text);
            }
        }

        const float contentHeight = listContentHeight(family);
        if (contentHeight > height && height > 0.0f) {
            const auto bar = scrollBarGeometry(height, contentHeight);
            const float thumbY = rect.top + scrollBarThumbY(bar, scroll, listMaxScroll(family));
            painter.fillRoundRect(
                p.textSecondary,
                D2D1::RectF(rect.right - kScrollBarInset - kScrollBarWidth, thumbY,
                            rect.right - kScrollBarInset, thumbY + bar.thumbHeight),
                kScrollBarWidth * 0.5f);
        }
        painter.target()->PopAxisAlignedClip();
    }

    void drawPreview(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        painter.fillRoundRect(p.cardFill, previewRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, previewRect, 1.0f, fluent::metrics::cardRadius);

        const std::wstring family = currentFamily();
        if (family.empty() || currentSize() <= 0.0f || !painter.dwrite())
            return;

        IDWriteTextFormat* format = nullptr;
        if (SUCCEEDED(painter.dwrite()->CreateTextFormat(
                family.c_str(), nullptr, dwriteWeightOf(currentStyle()), dwriteStyleOf(currentStyle()),
                DWRITE_FONT_STRETCH_NORMAL, currentSize(), L"zh-cn", &format)) &&
            format) {
            fluent::applyUiFontFallback(format);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            painter.drawText(L"AaBb 永字八法 0123", format,
                             D2D1::RectF(previewRect.left + 12.0f, previewRect.top,
                                         previewRect.right - 12.0f, previewRect.bottom),
                             p.text);
            format->Release();
        }
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter,
                    const D2D1_RECT_F& rect, const wchar_t* text, bool accent, int id) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        const D2D1_COLOR_F fill = accent
                                      ? (pressed ? p.accentPressed : hovered ? p.accentHover : p.accent)
                                      : (pressed ? p.controlPressed
                                                 : hovered ? p.controlHover : p.controlFill);
        const D2D1_COLOR_F textColor = accent ? p.textOnAccent : p.text;
        painter.fillRoundRect(fill, rect);
        if (!accent)
            painter.strokeRoundRect(p.cardStroke, rect);
        if (focusedId == id && focusVisible) {
            painter.strokeRoundRect(
                accent ? p.textOnAccent : p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, fluent::metrics::controlRadius - 1.0f);
        }
        painter.drawText(text, painter.textFormat(14.0f, 400, true, true),
                         D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f, rect.bottom),
                         textColor);
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        const auto& p = fluent::palette();
        painter.drawText(L"选择字体", painter.textFormat(20.0f, 600), titleRect, p.text);
        painter.drawText(L"搜索字体、字号和样式并实时预览", painter.textFormat(13.0f, 400),
                         subtitleRect, p.textSecondary);
        drawTextField(painter, filterRect, filterEdit, kIdFilterEdit);
        painter.drawText(L"样式", painter.textFormat(13.0f, 400, false, true), styleLabelRect,
                         p.textSecondary);
        drawStyleGroup(painter);
        drawList(painter, true);
        drawTextField(painter, sizeEditRect, sizeEdit, kIdSizeEdit);
        drawList(painter, false);
        drawPreview(painter);
        drawButton(painter, okRect, L"确定", true, kIdOk);
        drawButton(painter, cancelRect, L"取消", false, kIdCancel);
    }

    int hitTest(float x, float y, int* row = nullptr, int* option = nullptr) const {
        if (row)
            *row = -1;
        if (option)
            *option = -1;
        if (contains(filterRect, x, y))
            return kIdFilterEdit;
        if (contains(familyListRect, x, y)) {
            if (row)
                *row = rowAt(true, x, y);
            return kIdFamilyList;
        }
        if (contains(styleRect, x, y)) {
            if (option)
                *option = styleOptionAt(x, y);
            return kIdStyleGroup;
        }
        if (contains(sizeEditRect, x, y))
            return kIdSizeEdit;
        if (contains(sizeListRect, x, y)) {
            if (row)
                *row = rowAt(false, x, y);
            return kIdSizeList;
        }
        if (contains(okRect, x, y))
            return kIdOk;
        if (contains(cancelRect, x, y))
            return kIdCancel;
        return 0;
    }

    std::vector<int> focusOrder() const {
        return {kIdFilterEdit, kIdFamilyList, kIdStyleGroup, kIdSizeEdit,
                kIdSizeList, kIdOk, kIdCancel};
    }

    void focusStep(int direction) {
        const auto order = focusOrder();
        auto it = std::find(order.begin(), order.end(), focusedId);
        int index = it == order.end() ? (direction > 0 ? -1 : 0)
                                      : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) %
                static_cast<int>(order.size());
        focusedId = order[index];
        focusVisible = true;
        surface.invalidate();
    }

    void selectFamily(int row) {
        if (row < 0 || row >= static_cast<int>(filtered.size()))
            return;
        familySelected = row;
        ensureVisible(true, familySelected);
        surface.invalidate();
    }

    void selectSize(int row) {
        if (row < 0 || row >= static_cast<int>(_countof(kSizePresets)))
            return;
        sizeSelected = row;
        sizeEdit.text = std::to_wstring(kSizePresets[row]);
        sizeEdit.caret = sizeEdit.anchor = sizeEdit.text.size();
        ensureVisible(false, sizeSelected);
        surface.invalidate();
    }

    void selectStyle(int option) {
        if (option < 0 || option >= static_cast<int>(styleNames().size()))
            return;
        styleSelected = option;
        surface.invalidate();
    }

    bool beginScrollDrag(bool family, float x, float y) {
        if (!isScrollBarHit(family, x))
            return false;
        const auto& rect = listRect(family);
        const float viewHeight = rect.bottom - rect.top;
        const float contentHeight = listContentHeight(family);
        const auto bar = scrollBarGeometry(viewHeight, contentHeight);
        const float thumbY = rect.top + scrollBarThumbY(bar, listScroll(family),
                                                        listMaxScroll(family));
        if (y >= thumbY && y <= thumbY + bar.thumbHeight) {
            scrollDragging = true;
            draggingFamily = family;
            scrollDragGrabDy = y - thumbY;
            SetCapture(hwnd);
        } else {
            float& scroll = listScroll(family);
            scroll = std::clamp(scroll + (y < thumbY ? -viewHeight : viewHeight), 0.0f,
                                listMaxScroll(family));
            surface.invalidate();
        }
        return true;
    }

    void updateScrollDrag(float y) {
        if (!scrollDragging)
            return;
        const auto& rect = listRect(draggingFamily);
        const float viewHeight = rect.bottom - rect.top;
        const float contentHeight = listContentHeight(draggingFamily);
        const auto bar = scrollBarGeometry(viewHeight, contentHeight);
        if (bar.usable > 0.0f) {
            const float localY = y - rect.top;
            listScroll(draggingFamily) = std::clamp(
                (localY - bar.trackTop - scrollDragGrabDy) / bar.usable *
                    listMaxScroll(draggingFamily),
                0.0f, listMaxScroll(draggingFamily));
        }
        surface.invalidate();
    }

    void applyAndClose() {
        if (onApply)
            onApply(currentFamily(), currentSize(), currentStyle());
        destroy();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            surface.initialize(hwnd, backdrop);
            enumerateFonts();
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            surface.invalidate();
            return 0;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            surface.invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO:
            setMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp));
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop);
            surface.setBackdrop(backdrop);
            surface.invalidate();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            surface.paint(hdc, backdrop,
                          [this](fluent::FluentDialogSurface::Painter& painter, float w, float h) {
                              paint(painter, w, h);
                          });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            surface.eraseBackground(reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_MOUSEMOVE: {
            const float s = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / s;
            const float y = GET_Y_LPARAM(lp) / s;
            if (scrollDragging) {
                updateScrollDrag(y);
                return 0;
            }
            if (!GetCapture()) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            int row = -1;
            int option = -1;
            const int id = hitTest(x, y, &row, &option);
            if (id != hoverId || row != hoverRow || option != hoverOption) {
                hoverId = id;
                hoverRow = row;
                hoverOption = option;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverId = 0;
            hoverRow = -1;
            hoverOption = -1;
            surface.invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / s;
            const float y = GET_Y_LPARAM(lp) / s;
            int row = -1;
            int option = -1;
            const int id = hitTest(x, y, &row, &option);
            focusedId = id;
            pressedId = id;
            pressedOption = option;

            if (id == kIdFamilyList || id == kIdSizeList) {
                const bool family = id == kIdFamilyList;
                if (beginScrollDrag(family, x, y)) {
                    pressedId = 0;
                    pressedOption = -1;
                    return 0;
                }
                if (row >= 0) {
                    if (family)
                        selectFamily(row);
                    else
                        selectSize(row);
                    SetCapture(hwnd);
                }
            } else if (id == kIdStyleGroup) {
                if (option >= 0) {
                    selectStyle(option);
                    SetCapture(hwnd);
                }
            } else if (id == kIdOk || id == kIdCancel) {
                SetCapture(hwnd);
            }
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const float s = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / s;
            const float y = GET_Y_LPARAM(lp) / s;
            if (scrollDragging) {
                scrollDragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                surface.invalidate();
                return 0;
            }
            int option = -1;
            const int hit = hitTest(x, y, nullptr, &option);
            const int pressed = pressedId;
            const int pressedOptionValue = pressedOption;
            pressedId = 0;
            pressedOption = -1;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (pressed == kIdStyleGroup && hit == pressed && option == pressedOptionValue)
                selectStyle(option);
            else if (pressed == kIdOk && hit == pressed)
                applyAndClose();
            else if (pressed == kIdCancel && hit == pressed)
                destroy();
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const float s = surface.dipScale();
            int row = -1;
            const int id = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &row);
            if (id == kIdFamilyList && row >= 0)
                applyAndClose();
            return 0;
        }
        case WM_CAPTURECHANGED:
            scrollDragging = false;
            pressedId = 0;
            pressedOption = -1;
            surface.invalidate();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float s = surface.dipScale();
            const float x = point.x / s;
            const float y = point.y / s;
            if (contains(familyListRect, x, y))
                scrollBy(true, GET_WHEEL_DELTA_WPARAM(wp));
            else if (contains(sizeListRect, x, y))
                scrollBy(false, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                focusStep(isShiftDown() ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (handleEditKey(wp))
                return 0;
            if ((wp == VK_UP || wp == VK_DOWN) &&
                (focusedId == kIdFamilyList || focusedId == kIdSizeList)) {
                const bool family = focusedId == kIdFamilyList;
                int& selected = family ? familySelected : sizeSelected;
                const int count = family ? static_cast<int>(filtered.size())
                                         : static_cast<int>(_countof(kSizePresets));
                if (count > 0) {
                    selected = selected < 0 ? (wp == VK_DOWN ? 0 : count - 1)
                                           : std::clamp(selected + (wp == VK_DOWN ? 1 : -1), 0,
                                                        count - 1);
                    ensureVisible(family, selected);
                    if (!family)
                        selectSize(selected);
                    else
                        surface.invalidate();
                }
                return 0;
            }
            if ((wp == VK_PRIOR || wp == VK_NEXT) &&
                (focusedId == kIdFamilyList || focusedId == kIdSizeList)) {
                const bool family = focusedId == kIdFamilyList;
                int& selected = family ? familySelected : sizeSelected;
                const int count = family ? static_cast<int>(filtered.size())
                                         : static_cast<int>(_countof(kSizePresets));
                if (count <= 0)
                    return 0;
                const int page = std::max(1, static_cast<int>((listRect(family).bottom -
                                                               listRect(family).top) /
                                                              kListRowHeight) -
                                         1);
                const int direction = wp == VK_NEXT ? 1 : -1;
                selected = selected < 0 ? (direction > 0 ? 0 : count - 1)
                                       : std::clamp(selected + direction * page, 0, count - 1);
                ensureVisible(family, selected);
                if (!family)
                    selectSize(selected);
                else
                    surface.invalidate();
                return 0;
            }
            if ((wp == VK_HOME || wp == VK_END) &&
                (focusedId == kIdFamilyList || focusedId == kIdSizeList)) {
                const bool family = focusedId == kIdFamilyList;
                int& selected = family ? familySelected : sizeSelected;
                const int count = family ? static_cast<int>(filtered.size())
                                         : static_cast<int>(_countof(kSizePresets));
                if (count > 0) {
                    selected = wp == VK_HOME ? 0 : count - 1;
                    ensureVisible(family, selected);
                    if (!family)
                        selectSize(selected);
                    else
                        surface.invalidate();
                }
                return 0;
            }
            if ((wp == VK_LEFT || wp == VK_RIGHT) && focusedId == kIdStyleGroup) {
                const int direction = wp == VK_RIGHT ? 1 : -1;
                selectStyle((styleSelected + direction + static_cast<int>(styleNames().size())) %
                            static_cast<int>(styleNames().size()));
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                if (focusedId == kIdOk)
                    applyAndClose();
                else if (focusedId == kIdCancel)
                    destroy();
                return 0;
            }
            break;
        case WM_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wp)))
                return 0;
            break;
        case WM_IME_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wp)))
                return 0;
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            focusVisible = false;
            surface.invalidate();
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::FontPicker), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);
        const float pad = fluent::metrics::pagePadding;
        const float gap = fluent::metrics::controlGap;

        const float titleH = 30.0f;
        const float subtitleH = 20.0f;
        titleRect = D2D1::RectF(pad, pad, std::max(pad, w - pad), pad + titleH);
        subtitleRect = D2D1::RectF(pad, pad + titleH, std::max(pad, w - pad),
                                    pad + titleH + subtitleH);

        const float filterY = pad + titleH + subtitleH + fluent::metrics::sectionGap;
        filterRect = D2D1::RectF(pad, filterY, std::max(pad, w - pad),
                                 filterY + fluent::metrics::controlHeight);

        const float styleY = filterY + fluent::metrics::controlHeight + gap;
        const float styleLabelW = 48.0f;
        styleLabelRect = D2D1::RectF(pad, styleY, pad + styleLabelW,
                                     styleY + fluent::metrics::controlHeight);
        styleRect = D2D1::RectF(pad + styleLabelW + gap,
                                styleY,
                                std::max(pad + styleLabelW + gap, w - pad),
                                styleY + fluent::metrics::controlHeight);

        const float previewH = 96.0f;
        const float buttonH = fluent::metrics::controlHeight;
        const float buttonY = h - pad - buttonH;
        previewRect = D2D1::RectF(pad, buttonY - gap - previewH, std::max(pad, w - pad),
                                  buttonY - gap);

        const float listTop = styleY + fluent::metrics::controlHeight + gap;
        const float listHeight = std::max(0.0f, previewRect.top - gap - listTop);
        const float listWidth = std::max(0.0f, w - pad * 2.0f - gap);
        const float familyWidth = listWidth * 0.58f;
        familyListRect = D2D1::RectF(pad, listTop, pad + familyWidth, listTop + listHeight);

        const float rightX = familyListRect.right + gap;
        const float rightWidth = std::max(0.0f, w - pad - rightX);
        sizeEditRect = D2D1::RectF(rightX, listTop, rightX + rightWidth,
                                   listTop + fluent::metrics::controlHeight);
        sizeListRect = D2D1::RectF(
            rightX, sizeEditRect.bottom + fluent::metrics::compactGap, rightX + rightWidth,
            listTop + listHeight);

        const float cancelW = 88.0f;
        const float okW = 96.0f;
        cancelRect = D2D1::RectF(w - pad - cancelW, buttonY, w - pad, buttonY + buttonH);
        okRect = D2D1::RectF(w - pad - cancelW - gap - okW, buttonY,
                             w - pad - cancelW - gap, buttonY + buttonH);
        styleOptionRects.clear();
        clampScroll(true);
        clampScroll(false);
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
    impl_->initFamily = family;
    impl_->initSize = sizePt;
    impl_->initStyle = style;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
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
    RECT rc{0, 0, static_cast<LONG>(std::lround(600.0f * s)),
            static_cast<LONG>(std::lround(560.0f * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricFontPicker", L"选择字体",
                                  kDialogStyle, x, y, w, h, nullptr, nullptr, inst, impl_.get());
    return impl_->hwnd != nullptr;
}

void FontPickerDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }
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
