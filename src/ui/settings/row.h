#pragma once
#include <d2d1.h>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <cwchar>

namespace settings_ui {
inline constexpr float kRowH = 56.0f;
inline constexpr float kRowTallH = 72.0f;
inline constexpr float kHeaderH = 30.0f;
inline constexpr float kTitleMinHeight = 22.0f;
enum class ControlKind {
    Toggle,
    Radio,
    ModeGrid, // 2×2 模式选择卡片
    Slider,
    Button,
    AppList,
    Header, // 分组标题：无卡片背景，不参与交互与焦点
};

struct Row {
    int id = 0;
    ControlKind kind = ControlKind::Toggle;
    std::wstring text;
    std::wstring hint;
    std::wstring valueText;
    std::wstring controlText;
    std::vector<std::wstring> options;
    std::vector<bool> optionEnabled;
    bool showHint = false;
    bool choiceInline = false;
    bool checked = false;
    bool enabled = true;
    int selected = -1;
    int value = 0;
    int minValue = 0;
    int maxValue = 100;
    std::wstring valueSuffix = L"%";
    float controlW = 0.0f;
    float minHeight = kRowH;
    float height = kRowH;
    float gapAfter = 0.0f;
    float titleHeight = kTitleMinHeight;
    float valueHeight = 0.0f;
    D2D1_RECT_F cardRect{};
    D2D1_RECT_F iconRect{};
    D2D1_RECT_F labelRect{};
    D2D1_RECT_F valueRect{};
    D2D1_RECT_F hintRect{};
    D2D1_RECT_F controlRect{};
    D2D1_RECT_F artworkRect{};
    D2D1_RECT_F addAppRect{};
    std::vector<D2D1_RECT_F> appItemRects;
    std::vector<D2D1_RECT_F> appEditRects;
    std::vector<D2D1_RECT_F> appDeleteRects;
    std::vector<D2D1_RECT_F> optionRects;
    std::vector<std::wstring> optionHints;
};

// 页面定义与窗口共用同一套行创建规则。
class RowBuilder {
public:
    explicit RowBuilder(std::vector<Row>& rows) : rows_(rows) {}
    Row& addRow(int id, ControlKind kind, const wchar_t* text, const wchar_t* hint, float controlW,
                float height) {
        Row row;
        row.id = id;
        row.kind = kind;
        row.text = text ? text : L"";
        row.hint = hint ? hint : L"";
        row.showHint = !row.hint.empty();
        row.controlW = controlW;
        row.minHeight = row.height = height;
        rows_.push_back(std::move(row));
        return rows_.back();
    }
    Row& addToggle(int id, const wchar_t* text, bool checked) {
        auto& row = addRow(id, ControlKind::Toggle, text, nullptr, 40.0f, kRowH);
        row.checked = checked;
        return row;
    }
    Row& addRadio(int id, const wchar_t* text, const wchar_t* hint,
                  std::vector<std::wstring> options, int selected, bool enabled, float height) {
        auto& row = addRow(id, ControlKind::Radio, text, hint, 0.0f, height);
        row.options = std::move(options);
        row.optionEnabled.assign(row.options.size(), true);
        row.selected = selected;
        row.enabled = enabled;
        return row;
    }
    Row& addSlider(int id, const wchar_t* text, int value, bool enabled,
                   const wchar_t* hint = nullptr, float height = kRowH) {
        auto& row = addRow(id, ControlKind::Slider, text, hint, 216.0f, height);
        row.value = std::clamp(value, 0, 100);
        row.minValue = 0;
        row.maxValue = 100;
        row.enabled = enabled;
        return row;
    }
    Row& addButton(int id, const wchar_t* text, const wchar_t* hint, const wchar_t* controlText,
                   float height = kRowH) {
        auto& row = addRow(id, ControlKind::Button, text, hint, 132.0f, height);
        row.controlText = controlText ? controlText : L"";
        return row;
    }
    Row& addHeader(const wchar_t* text) {
        auto& row = addRow(0, ControlKind::Header, text, nullptr, 0.0f, kHeaderH);
        row.enabled = false;
        return row;
    }

private:
    std::vector<Row>& rows_;
};
inline std::wstring colorText(COLORREF color) {
    wchar_t text[10]{};
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}

inline Row* findRow(std::vector<Row>& rows, int id) {
    for (auto& row : rows)
        if (row.id == id)
            return &row;
    return nullptr;
}
} // namespace settings_ui
