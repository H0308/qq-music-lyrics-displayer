#pragma once
#include "ui/settings/row.h"
#include <windows.h>
#include <array>
#include <functional>

namespace settings_ui {
inline constexpr UINT_PTR kSliderCaretTimer = 5;
struct SliderEditorOwner {
    HWND& hwnd;
    int& focusedId;
    const D2D1_RECT_F& viewport;
    std::function<Row*(int)> findRow;
    std::function<void(int)> apply;
    std::function<void()> stopScroll, invalidate;
};
class SliderEditor {
public:
    struct State {
        int rowId = 0;
        bool open = false, committing = false;
        std::wstring text;
        size_t caret = 0, anchor = 0;
        std::array<float, 4> caretStops{};
        bool caretVisible = true, selecting = false;
        std::wstring undoText;
        size_t undoCaret = 0, undoAnchor = 0;
        bool canUndo = false;
    };
    explicit SliderEditor(SliderEditorOwner owner) : owner_(std::move(owner)) {}
    const State& state() const { return state_; }
    void setCaretStop(size_t i, float value) { state_.caretStops[i] = value; }
    void setSelecting(bool selecting) { state_.selecting = selecting; }
    void blink() { state_.caretVisible = !state_.caretVisible; }
    void resetCaret();
    void hide();
    void cancel();
    void commit();
    void layout();
    void open(Row& row);
    void replaceSelection(const std::wstring& inserted);
    bool copySelection();
    bool handleKey(WPARAM key);
    void placeCaret(float pointerX, bool extend);

private:
    SliderEditorOwner owner_;
    State state_;
};
} // namespace settings_ui
