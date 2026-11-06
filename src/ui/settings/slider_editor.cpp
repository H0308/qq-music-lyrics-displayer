#include "ui/settings/slider_editor.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <cwctype>
#include <cstring>

namespace settings_ui {
namespace {
bool parseBoundedInteger(const std::wstring& text, int minValue, int maxValue, int& value) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(text[begin]))
        ++begin;
    while (end > begin && std::iswspace(text[end - 1]))
        --end;
    if (begin == end)
        return false;

    int parsed = 0;
    for (size_t i = begin; i < end; ++i) {
        const wchar_t ch = text[i];
        if (ch < L'0' || ch > L'9')
            return false;
        const int digit = static_cast<int>(ch - L'0');
        if (parsed > (INT_MAX - digit) / 10)
            parsed = INT_MAX;
        else
            parsed = parsed * 10 + digit;
    }
    value = std::clamp(parsed, minValue, maxValue);
    return true;
}

} // namespace
void SliderEditor::resetCaret() {
    state_.caretVisible = true;
    KillTimer(owner_.hwnd, kSliderCaretTimer);
    const UINT blink = GetCaretBlinkTime();
    if (blink != INFINITE && blink > 0)
        SetTimer(owner_.hwnd, kSliderCaretTimer, blink, nullptr);
    owner_.invalidate();
}

void SliderEditor::hide() {
    state_.open = false;
    state_.rowId = 0;
    state_.selecting = false;
    KillTimer(owner_.hwnd, kSliderCaretTimer);
    if (GetCapture() == owner_.hwnd)
        ReleaseCapture();
    owner_.invalidate();
}

void SliderEditor::cancel() {
    if (state_.open)
        hide();
}

void SliderEditor::commit() {
    if (!state_.open || state_.committing)
        return;
    state_.committing = true;
    Row* row = owner_.findRow(state_.rowId);
    if (row && row->enabled) {
        int value = row->value;
        if (parseBoundedInteger(state_.text, row->minValue, row->maxValue, value) &&
            value != row->value) {
            row->value = value;
            owner_.apply(row->id);
        }
    }
    hide();
    state_.committing = false;
}

void SliderEditor::layout() {
    if (!state_.open)
        return;
    const Row* row = owner_.findRow(state_.rowId);
    if (!row || row->kind != ControlKind::Slider || !row->enabled ||
        row->cardRect.right <= row->cardRect.left || row->cardRect.bottom <= owner_.viewport.top ||
        row->cardRect.top >= owner_.viewport.bottom)
        hide();
}

void SliderEditor::open(Row& row) {
    if (!row.enabled)
        return;
    if (state_.open && state_.rowId != row.id)
        commit();
    owner_.stopScroll();
    state_.rowId = row.id;
    state_.open = true;
    state_.text = std::to_wstring(row.value);
    state_.caret = state_.text.size();
    state_.anchor = 0;
    state_.canUndo = false;
    owner_.focusedId = row.id;
    SetFocus(owner_.hwnd);
    resetCaret();
}

void SliderEditor::replaceSelection(const std::wstring& inserted) {
    if (inserted.find_first_not_of(L"0123456789") != std::wstring::npos)
        return;
    const size_t first = std::min(state_.caret, state_.anchor);
    const size_t last = std::max(state_.caret, state_.anchor);
    if (state_.text.size() - (last - first) + inserted.size() > 3)
        return;
    state_.undoText = state_.text;
    state_.undoCaret = state_.caret;
    state_.undoAnchor = state_.anchor;
    state_.canUndo = true;
    state_.text.replace(first, last - first, inserted);
    state_.caret = state_.anchor = first + inserted.size();
    resetCaret();
}

bool SliderEditor::copySelection() {
    const size_t first = std::min(state_.caret, state_.anchor);
    const size_t last = std::max(state_.caret, state_.anchor);
    if (first == last || !OpenClipboard(owner_.hwnd))
        return false;
    const std::wstring text = state_.text.substr(first, last - first);
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool copied = false;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* data = GlobalLock(memory)) {
            std::memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            EmptyClipboard();
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied)
            GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

bool SliderEditor::handleKey(WPARAM key) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_TAB || (ctrl && key == 'F')) {
        commit();
        return false;
    }
    if (key == VK_RETURN) {
        commit();
        return true;
    }
    if (key == VK_ESCAPE) {
        cancel();
        return true;
    }
    if (ctrl && key == 'A') {
        state_.anchor = 0;
        state_.caret = state_.text.size();
    } else if ((ctrl && (key == 'C' || key == 'X' || key == VK_INSERT)) ||
               (shift && key == VK_DELETE)) {
        if (copySelection() && (key == 'X' || key == VK_DELETE))
            replaceSelection(L"");
    } else if ((ctrl && key == 'V') || (shift && key == VK_INSERT)) {
        std::wstring text;
        if (OpenClipboard(owner_.hwnd)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* value = static_cast<const wchar_t*>(GlobalLock(data))) {
                    text = value;
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!text.empty())
            replaceSelection(text);
    } else if (ctrl && (key == 'Z' || key == 'Y')) {
        if (state_.canUndo) {
            std::swap(state_.text, state_.undoText);
            std::swap(state_.caret, state_.undoCaret);
            std::swap(state_.anchor, state_.undoAnchor);
        }
    } else if (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END) {
        if (key == VK_HOME)
            state_.caret = 0;
        else if (key == VK_END)
            state_.caret = state_.text.size();
        else if (!shift && state_.caret != state_.anchor)
            state_.caret = key == VK_LEFT ? std::min(state_.caret, state_.anchor)
                                          : std::max(state_.caret, state_.anchor);
        else if (key == VK_LEFT && state_.caret > 0)
            --state_.caret;
        else if (key == VK_RIGHT && state_.caret < state_.text.size())
            ++state_.caret;
        if (!shift)
            state_.anchor = state_.caret;
    } else if (key == VK_BACK || key == VK_DELETE) {
        if (state_.caret == state_.anchor) {
            if (key == VK_BACK && state_.anchor > 0)
                --state_.anchor;
            else if (key == VK_DELETE && state_.anchor < state_.text.size())
                ++state_.anchor;
        }
        replaceSelection(L"");
    }
    resetCaret();
    return true;
}

void SliderEditor::placeCaret(float pointerX, bool extend) {
    size_t nearest = 0;
    for (size_t i = 1; i <= state_.text.size(); ++i)
        if (std::fabs(pointerX - state_.caretStops[i]) <
            std::fabs(pointerX - state_.caretStops[nearest]))
            nearest = i;
    state_.caret = nearest;
    if (!extend)
        state_.anchor = nearest;
    resetCaret();
}

} // namespace settings_ui
