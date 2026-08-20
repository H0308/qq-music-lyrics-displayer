#include "settings_dialog.h"

#include "resource.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kIdNav = 400;
constexpr int kIdSongInfo = 410;
constexpr int kIdAlbumCover = 411;
constexpr int kIdPlatformIcon = 412;
constexpr int kIdCoverEffect = 413;
constexpr int kIdSpectrum = 414;
constexpr int kIdHoverControls = 415;
constexpr int kIdRenderMode = 416;
constexpr int kIdPickFont = 420;
constexpr int kIdFontColor = 421;
constexpr int kIdFollowAlbum = 422;
constexpr int kIdDoubleLine = 430;
constexpr int kIdAlignment = 431;
constexpr int kIdSecondaryOn = 432;
constexpr int kIdSecondaryType = 433;

constexpr float kWindowW = 760.0f;
constexpr float kWindowH = 520.0f;
constexpr float kNavW = 176.0f;
constexpr float kRowH = 56.0f;
constexpr float kRowTallH = 72.0f;
constexpr float kRowGap = 8.0f;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

float estimateRadioWidth(const std::vector<std::wstring>& options) {
    float w = 0.0f;
    for (const auto& option : options)
        w += 16.0f + 8.0f + static_cast<float>(option.size()) * 14.0f + 20.0f;
    return w > 0.0f ? w - 20.0f : 0.0f;
}

} // namespace

struct SettingsDialog::Impl {
    enum class ControlKind {
        Toggle,
        Radio,
        Button,
    };

    struct Row {
        int id = 0;
        ControlKind kind = ControlKind::Toggle;
        std::wstring text;
        std::wstring hint;
        std::wstring controlText;
        std::vector<std::wstring> options;
        bool showHint = false;
        bool checked = false;
        bool enabled = true;
        int selected = -1;
        float controlW = 0.0f;
        float height = kRowH;
        D2D1_RECT_F cardRect{};
        D2D1_RECT_F labelRect{};
        D2D1_RECT_F hintRect{};
        D2D1_RECT_F controlRect{};
        std::vector<D2D1_RECT_F> optionRects;
    };

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr;
    bool backdrop = false;
    SettingsState state;
    SettingsActions actions;
    int activePage = 0;

    fluent::FluentDialogSurface surface;
    std::array<std::wstring, 3> navItems{L"显示", L"字体与颜色", L"歌词"};
    std::array<std::wstring, 3> pageTitles{L"显示", L"字体与颜色", L"歌词"};
    std::vector<Row> rows[3];
    D2D1_RECT_F navRect{};
    std::array<D2D1_RECT_F, 3> navItemRects{};
    std::array<D2D1_RECT_F, 3> pageTitleRects{};

    int hoverId = 0;
    int hoverOption = -1;
    int pressedId = 0;
    int pressedOption = -1;
    int focusedId = kIdNav;
    bool focusVisible = false;

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

    Row* findRow(int id) {
        for (auto& page : rows) {
            for (auto& row : page) {
                if (row.id == id)
                    return &row;
            }
        }
        return nullptr;
    }

    const Row* findRow(int id) const {
        for (const auto& page : rows) {
            for (const auto& row : page) {
                if (row.id == id)
                    return &row;
            }
        }
        return nullptr;
    }

    Row& addRow(int page, int id, ControlKind kind, const wchar_t* text, const wchar_t* hint,
                float controlW, float height) {
        Row row;
        row.id = id;
        row.kind = kind;
        row.text = text ? text : L"";
        row.hint = hint ? hint : L"";
        row.showHint = !row.hint.empty();
        row.controlW = controlW;
        row.height = height;
        rows[page].push_back(std::move(row));
        return rows[page].back();
    }

    Row& addToggle(int page, int id, const wchar_t* text, bool checked) {
        Row& row = addRow(page, id, ControlKind::Toggle, text, nullptr, 40.0f, kRowH);
        row.checked = checked;
        return row;
    }

    Row& addRadio(int page, int id, const wchar_t* text, const wchar_t* hint,
                  std::vector<std::wstring> options, int selected, bool enabled, float height) {
        Row& row = addRow(page, id, ControlKind::Radio, text, hint,
                          estimateRadioWidth(options), height);
        row.options = std::move(options);
        row.selected = selected;
        row.enabled = enabled;
        return row;
    }

    Row& addButton(int page, int id, const wchar_t* text, const wchar_t* hint,
                   const wchar_t* controlText) {
        Row& row = addRow(page, id, ControlKind::Button, text, hint, 132.0f, kRowH);
        row.controlText = controlText ? controlText : L"";
        return row;
    }

    void createControls() {
        addToggle(0, kIdSongInfo, L"显示歌曲信息", state.songInfoVisible);
        addToggle(0, kIdAlbumCover, L"显示专辑封面", state.albumCoverVisible);
        addToggle(0, kIdPlatformIcon, L"显示平台图标", state.platformIconVisible);
        addRadio(0, kIdCoverEffect, L"专辑封面效果", nullptr, {L"默认", L"黑胶唱片"},
                 state.coverEffectVinyl ? 1 : 0, state.albumCoverVisible, kRowH);
        addToggle(0, kIdSpectrum, L"频谱", state.spectrumOn);
        addToggle(0, kIdHoverControls, L"悬浮时显示播放控件", state.hoverControls);
        addRadio(0, kIdRenderMode, L"性能模式", L"低渲染降帧省 GPU，完全停止仅驻留内存",
                 {L"正常", L"低渲染", L"完全停止"}, state.renderMode, true, kRowTallH);

        addButton(1, kIdPickFont, L"字体", state.fontDesc.c_str(), L"选择字体…");
        addButton(1, kIdFontColor, L"字体颜色与效果", nullptr, L"打开…");
        addToggle(1, kIdFollowAlbum, L"已播放颜色跟随专辑", state.followAlbum);

        addToggle(2, kIdDoubleLine, L"双行歌词", state.doubleLineLyrics);
        addRadio(2, kIdAlignment, L"歌词对齐", nullptr, {L"左对齐", L"居中", L"右对齐"},
                 state.lyricAlignment, true, kRowH);
        addToggle(2, kIdSecondaryOn, L"开启翻译/罗马音", state.secondaryEnabled);
        const wchar_t* secondaryHint = state.secondaryAvailability == 1
                                            ? L"正在检查翻译和罗马音…"
                                            : state.secondaryAvailability == 2
                                                  ? L"当前歌曲无翻译或罗马音"
                                                  : L"";
        addRadio(2, kIdSecondaryType, L"辅助歌词类型", secondaryHint,
                 {L"翻译", L"罗马音"}, state.preferRomanization ? 1 : 0,
                 state.secondaryEnabled && state.secondaryAvailability == 0,
                 *secondaryHint ? kRowTallH : kRowH);
    }

    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float s = surface.dipScale();
        const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);

        navRect = D2D1::RectF(12.0f, 12.0f, 12.0f + kNavW, std::max(12.0f, h - 12.0f));
        for (int i = 0; i < 3; ++i) {
            navItemRects[i] = D2D1::RectF(navRect.left, navRect.top + i * 32.0f,
                                          navRect.right, navRect.top + (i + 1) * 32.0f);
            pageTitleRects[i] = D2D1::RectF(0, 0, 0, 0);
        }

        const float contentX = 12.0f + kNavW + 16.0f;
        const float contentW = std::max(20.0f, w - contentX - 24.0f);
        pageTitleRects[activePage] =
            D2D1::RectF(contentX, 14.0f, contentX + contentW, 42.0f);
        float y = 14.0f + 28.0f + 16.0f;
        for (auto& page : rows) {
            for (auto& row : page) {
                row.cardRect = D2D1::RectF(0, 0, 0, 0);
                row.labelRect = D2D1::RectF(0, 0, 0, 0);
                row.hintRect = D2D1::RectF(0, 0, 0, 0);
                row.controlRect = D2D1::RectF(0, 0, 0, 0);
                row.optionRects.clear();
            }
        }

        for (auto& row : rows[activePage]) {
            const float rowH = row.height;
            row.cardRect = D2D1::RectF(contentX, y, contentX + contentW, y + rowH);
            const float innerX = contentX + 16.0f;
            const float controlX = contentX + contentW - 16.0f - row.controlW;
            const float labelW = std::max(20.0f, controlX - innerX - 12.0f);
            if (row.showHint) {
                row.labelRect = D2D1::RectF(innerX, y + 12.0f, innerX + labelW, y + 34.0f);
                row.hintRect = D2D1::RectF(innerX, y + rowH - 26.0f,
                                           innerX + labelW, y + rowH - 8.0f);
            } else {
                row.labelRect = D2D1::RectF(innerX, y + (rowH - 22.0f) / 2.0f,
                                           innerX + labelW, y + (rowH - 22.0f) / 2.0f + 22.0f);
            }
            const float controlH = row.kind == ControlKind::Button
                                       ? fluent::metrics::controlHeight
                                       : 24.0f;
            row.controlRect = D2D1::RectF(controlX, y + (rowH - controlH) / 2.0f,
                                          controlX + row.controlW,
                                          y + (rowH - controlH) / 2.0f + controlH);
            y += rowH + kRowGap;
        }
    }

    void showPage(int page) {
        if (page < 0 || page >= 3 || page == activePage)
            return;
        activePage = page;
        hoverId = 0;
        hoverOption = -1;
        pressedId = 0;
        pressedOption = -1;
        focusedId = kIdNav;
        layout();
        if (hwnd)
            surface.invalidate();
    }

    void drawNav(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        painter.fillRoundRect(p.cardFill, navRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, navRect, 1.0f, fluent::metrics::cardRadius);
        if (focusedId == kIdNav && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(navRect.left + 1.5f, navRect.top + 1.5f, navRect.right - 1.5f,
                            navRect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }
        auto* format = painter.textFormat(13.0f, 400, false, true);
        for (int i = 0; i < 3; ++i) {
            const bool selected = i == activePage;
            const bool hovered = hoverId == kIdNav && hoverOption == i;
            if (selected)
                painter.fillRoundRect(p.listSelected,
                                      D2D1::RectF(navItemRects[i].left + 4.0f,
                                                  navItemRects[i].top + 2.0f,
                                                  navItemRects[i].right - 4.0f,
                                                  navItemRects[i].bottom - 2.0f));
            else if (hovered)
                painter.fillRoundRect(p.listHover,
                                      D2D1::RectF(navItemRects[i].left + 4.0f,
                                                  navItemRects[i].top + 2.0f,
                                                  navItemRects[i].right - 4.0f,
                                                  navItemRects[i].bottom - 2.0f));
            if (selected)
                painter.fillRoundRect(
                    p.accent,
                    D2D1::RectF(navItemRects[i].left + 7.0f,
                                (navItemRects[i].top + navItemRects[i].bottom) / 2.0f - 8.0f,
                                navItemRects[i].left + 10.0f,
                                (navItemRects[i].top + navItemRects[i].bottom) / 2.0f + 8.0f),
                    1.5f);
            painter.drawText(navItems[i], format,
                             D2D1::RectF(navItemRects[i].left + 16.0f, navItemRects[i].top,
                                         navItemRects[i].right - 12.0f, navItemRects[i].bottom),
                             p.text);
        }
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == row.id;
        const bool pressed = pressedId == row.id;
        D2D1_COLOR_F fill = pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill;
        D2D1_COLOR_F textColor = row.enabled ? p.text : p.disabled;
        if (!row.enabled)
            fill = p.listHover;
        painter.fillRoundRect(fill, row.controlRect);
        painter.strokeRoundRect(p.cardStroke, row.controlRect);
        if (focusedId == row.id && focusVisible && row.enabled) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(row.controlRect.left + 1.5f, row.controlRect.top + 1.5f,
                            row.controlRect.right - 1.5f, row.controlRect.bottom - 1.5f),
                1.5f, fluent::metrics::controlRadius - 1.0f);
        }
        painter.drawText(row.controlText, painter.textFormat(14.0f, 400, true, true),
                         D2D1::RectF(row.controlRect.left + 4.0f, row.controlRect.top,
                                     row.controlRect.right - 4.0f, row.controlRect.bottom),
                         textColor);
    }

    void drawToggle(fluent::FluentDialogSurface::Painter& painter, const Row& row) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == row.id;
        const bool enabled = row.enabled;
        const bool focused = focusedId == row.id && focusVisible;
        const float trackH = std::min(20.0f, row.controlRect.bottom - row.controlRect.top);
        const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(
            row.controlRect.left + 0.5f, centerY - trackH * 0.5f,
            row.controlRect.right - 0.5f, centerY + trackH * 0.5f);
        const float radius = trackH * 0.5f;
        const float knobR = trackH * 0.5f - 3.5f;
        const float knobX = row.checked ? track.right - trackH * 0.5f
                                        : track.left + trackH * 0.5f;
        if (!enabled) {
            painter.fillRoundRect(p.listHover, track, radius);
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
        } else if (row.checked) {
            painter.fillRoundRect(hovered ? p.accentHover : p.accent, track, radius);
        } else {
            painter.fillRoundRect(hovered ? p.controlHover : p.controlFill, track, radius);
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
        }
        if (auto* br = painter.brush(!enabled ? p.disabled
                                               : row.checked ? p.textOnAccent : p.textSecondary)) {
            painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR),
                                             br);
        }
        if (focused && enabled) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(track.left + 1.5f, track.top + 1.5f,
                            track.right - 1.5f, track.bottom - 1.5f),
                1.5f, std::max(1.0f, radius - 1.5f));
        }
    }

    void drawRadio(fluent::FluentDialogSurface::Painter& painter, Row& row) {
        const auto& p = fluent::palette();
        auto* format = painter.textFormat(13.0f, 400, false, true);
        if (!format)
            return;
        const float cy = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        constexpr float kCircle = 16.0f;
        constexpr float kTextGap = 8.0f;
        constexpr float kOptionGap = 20.0f;
        float x = row.controlRect.left;
        row.optionRects.clear();
        for (size_t i = 0; i < row.options.size(); ++i) {
            const bool selected = static_cast<int>(i) == row.selected;
            const bool hovered = row.enabled && hoverId == row.id && hoverOption == static_cast<int>(i);
            const bool pressed = row.enabled && pressedId == row.id &&
                                 pressedOption == static_cast<int>(i);
            const float textW = painter.measureTextWidth(row.options[i], format);
            D2D1_ELLIPSE circle{D2D1::Point2F(x + kCircle * 0.5f, cy), kCircle * 0.5f,
                                kCircle * 0.5f};
            if (!row.enabled) {
                if (auto* br = painter.brush(p.disabled)) {
                    painter.target()->DrawEllipse(circle, br, 1.0f);
                    if (selected)
                        painter.target()->FillEllipse(
                            D2D1::Ellipse(circle.point, 4.0f, 4.0f), br);
                }
            } else if (selected) {
                if (auto* br = painter.brush(hovered || pressed ? p.accentHover : p.accent))
                    painter.target()->FillEllipse(circle, br);
                if (auto* br = painter.brush(p.textOnAccent))
                    painter.target()->FillEllipse(D2D1::Ellipse(circle.point, 4.0f, 4.0f), br);
            } else {
                if (hovered)
                    painter.target()->FillEllipse(circle, painter.brush(p.listHover));
                if (auto* br = painter.brush(p.textSecondary))
                    painter.target()->DrawEllipse(circle, br, pressed ? 1.5f : 1.0f);
            }
            painter.drawText(row.options[i], format,
                             D2D1::RectF(x + kCircle + kTextGap, row.controlRect.top,
                                         x + kCircle + kTextGap + textW + 1.0f,
                                         row.controlRect.bottom),
                             row.enabled ? p.text : p.disabled);
            row.optionRects.push_back(D2D1::RectF(
                x, row.controlRect.top, x + kCircle + kTextGap + textW,
                row.controlRect.bottom));
            x += kCircle + kTextGap + textW + kOptionGap;
        }
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        const auto& p = fluent::palette();
        drawNav(painter);
        painter.drawText(pageTitles[activePage], painter.textFormat(20.0f, 600),
                         pageTitleRects[activePage], p.text);
        for (auto& row : rows[activePage]) {
            painter.fillRoundRect(p.cardFill, row.cardRect, fluent::metrics::cardRadius);
            painter.strokeRoundRect(p.cardStroke, row.cardRect, 1.0f, fluent::metrics::cardRadius);
            painter.drawText(row.text, painter.textFormat(14.0f, 400), row.labelRect, p.text);
            if (row.showHint)
                painter.drawText(row.hint, painter.textFormat(12.0f, 400), row.hintRect,
                                 p.textSecondary);
            if (row.kind == ControlKind::Toggle)
                drawToggle(painter, row);
            else if (row.kind == ControlKind::Radio)
                drawRadio(painter, row);
            else
                drawButton(painter, row);
        }
    }

    int hitTest(float x, float y, int* option = nullptr) const {
        if (option)
            *option = -1;
        for (int i = 0; i < 3; ++i) {
            if (contains(navItemRects[i], x, y)) {
                if (option)
                    *option = i;
                return kIdNav;
            }
        }
        for (const auto& row : rows[activePage]) {
            if (!contains(row.controlRect, x, y))
                continue;
            if (option && row.kind == ControlKind::Radio) {
                for (size_t i = 0; i < row.optionRects.size(); ++i) {
                    if (contains(row.optionRects[i], x, y)) {
                        *option = static_cast<int>(i);
                        break;
                    }
                }
            }
            return row.id;
        }
        return 0;
    }

    std::vector<int> focusOrder() const {
        std::vector<int> order{kIdNav};
        for (const auto& row : rows[activePage]) {
            if (row.enabled)
                order.push_back(row.id);
        }
        return order;
    }

    void focusStep(int direction) {
        const auto order = focusOrder();
        if (order.empty())
            return;
        auto it = std::find(order.begin(), order.end(), focusedId);
        int index = it == order.end() ? (direction > 0 ? -1 : 0)
                                      : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) % order.size();
        focusedId = order[index];
        focusVisible = true;
        surface.invalidate();
    }

    void onCommand(int id) {
        if (id == kIdNav)
            return;
        Row* row = findRow(id);
        if (!row || !row->enabled)
            return;
        switch (id) {
        case kIdSongInfo:
            row->checked = !row->checked;
            if (actions.onSongInfoVisible)
                actions.onSongInfoVisible(row->checked);
            break;
        case kIdAlbumCover:
            row->checked = !row->checked;
            if (actions.onAlbumCoverVisible)
                actions.onAlbumCoverVisible(row->checked);
            if (auto* effect = findRow(kIdCoverEffect))
                effect->enabled = row->checked;
            break;
        case kIdPlatformIcon:
            row->checked = !row->checked;
            if (actions.onPlatformIconVisible)
                actions.onPlatformIconVisible(row->checked);
            break;
        case kIdCoverEffect:
            if (actions.onCoverEffectVinyl)
                actions.onCoverEffectVinyl(row->selected == 1);
            break;
        case kIdSpectrum:
            row->checked = !row->checked;
            if (actions.onSpectrum)
                actions.onSpectrum(row->checked);
            break;
        case kIdHoverControls:
            row->checked = !row->checked;
            if (actions.onHoverControls)
                actions.onHoverControls(row->checked);
            break;
        case kIdRenderMode:
            if (actions.onRenderMode)
                actions.onRenderMode(row->selected);
            break;
        case kIdPickFont:
            if (actions.onPickFont)
                actions.onPickFont();
            break;
        case kIdFontColor:
            if (actions.onFontColorEffect)
                actions.onFontColorEffect();
            break;
        case kIdFollowAlbum:
            row->checked = !row->checked;
            if (actions.onFollowAlbum)
                actions.onFollowAlbum(row->checked);
            break;
        case kIdDoubleLine:
            row->checked = !row->checked;
            if (actions.onDoubleLineLyrics)
                actions.onDoubleLineLyrics(row->checked);
            break;
        case kIdAlignment:
            if (actions.onLyricAlignment)
                actions.onLyricAlignment(row->selected);
            break;
        case kIdSecondaryOn:
            row->checked = !row->checked;
            if (actions.onSecondaryEnabled)
                actions.onSecondaryEnabled(row->checked);
            if (auto* type = findRow(kIdSecondaryType))
                type->enabled = row->checked && state.secondaryAvailability == 0;
            break;
        case kIdSecondaryType:
            if (actions.onPreferRomanization)
                actions.onPreferRomanization(row->selected == 1);
            break;
        }
        if (hwnd)
            surface.invalidate();
    }

    void updateState(const SettingsState& s) {
        state = s;
        if (auto* row = findRow(kIdSongInfo))
            row->checked = s.songInfoVisible;
        if (auto* row = findRow(kIdAlbumCover))
            row->checked = s.albumCoverVisible;
        if (auto* row = findRow(kIdPlatformIcon))
            row->checked = s.platformIconVisible;
        if (auto* row = findRow(kIdCoverEffect)) {
            row->selected = s.coverEffectVinyl ? 1 : 0;
            row->enabled = s.albumCoverVisible;
        }
        if (auto* row = findRow(kIdSpectrum))
            row->checked = s.spectrumOn;
        if (auto* row = findRow(kIdHoverControls))
            row->checked = s.hoverControls;
        if (auto* row = findRow(kIdRenderMode))
            row->selected = s.renderMode;
        if (auto* row = findRow(kIdPickFont)) {
            row->hint = s.fontDesc;
            row->showHint = !row->hint.empty();
            row->height = kRowH;
        }
        if (auto* row = findRow(kIdFollowAlbum))
            row->checked = s.followAlbum;
        if (auto* row = findRow(kIdDoubleLine))
            row->checked = s.doubleLineLyrics;
        if (auto* row = findRow(kIdAlignment))
            row->selected = s.lyricAlignment;
        if (auto* row = findRow(kIdSecondaryOn))
            row->checked = s.secondaryEnabled;
        if (auto* row = findRow(kIdSecondaryType)) {
            row->hint = s.secondaryAvailability == 1
                            ? L"正在检查翻译和罗马音…"
                            : s.secondaryAvailability == 2 ? L"当前歌曲无翻译或罗马音" : L"";
            row->showHint = !row->hint.empty();
            row->height = row->showHint ? kRowTallH : kRowH;
            row->selected = s.preferRomanization ? 1 : 0;
            row->enabled = s.secondaryEnabled && s.secondaryAvailability == 0;
        }
        layout();
        surface.invalidate();
    }

    void updateFontDescription(const std::wstring& description) {
        state.fontDesc = description;
        if (auto* row = findRow(kIdPickFont)) {
            row->hint = description;
            row->showHint = !description.empty();
            row->height = kRowH;
            layout();
            surface.invalidate();
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd, false);
            surface.initialize(hwnd, backdrop);
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
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop, false);
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
            // 顶层表面由 FluentDialogSurface 在 WM_PAINT 内统一绘制，避免 GDI 与 D2D 重叠。
            return 1;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            const float s = surface.dipScale();
            int option = -1;
            const int id = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
            if (id != hoverId || option != hoverOption) {
                hoverId = id;
                hoverOption = option;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverId = 0;
            hoverOption = -1;
            surface.invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            int option = -1;
            pressedId = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
            pressedOption = option;
            if (pressedId != 0)
                focusedId = pressedId;
            if (pressedId != 0)
                SetCapture(hwnd);
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const float s = surface.dipScale();
            int option = -1;
            const int hit = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, &option);
            const int pressed = pressedId;
            const int pressedOptionValue = pressedOption;
            pressedId = 0;
            pressedOption = -1;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (pressed == kIdNav && hit == kIdNav && option >= 0)
                showPage(option);
            else if (pressed != 0 && pressed == hit) {
                Row* row = findRow(pressed);
                if (row && row->enabled) {
                    if (row->kind == ControlKind::Radio) {
                        if (pressedOptionValue >= 0 && pressedOptionValue == option &&
                            pressedOptionValue != row->selected) {
                            row->selected = pressedOptionValue;
                            onCommand(pressed);
                        }
                    } else {
                        onCommand(pressed);
                    }
                }
            }
            surface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressedId = 0;
            pressedOption = -1;
            surface.invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB;
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                focusStep((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (wp == VK_UP || wp == VK_DOWN) {
                if (focusedId == kIdNav) {
                    int next = activePage + (wp == VK_DOWN ? 1 : -1);
                    if (next < 0)
                        next = 2;
                    if (next >= 3)
                        next = 0;
                    showPage(next);
                }
                return 0;
            }
            if (wp == VK_LEFT || wp == VK_RIGHT) {
                Row* row = findRow(focusedId);
                if (row && row->kind == ControlKind::Radio && row->enabled && !row->options.empty()) {
                    int direction = wp == VK_LEFT ? -1 : 1;
                    row->selected = (row->selected + direction +
                                     static_cast<int>(row->options.size())) %
                                    static_cast<int>(row->options.size());
                    onCommand(row->id);
                }
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                if (focusedId != kIdNav) {
                    Row* row = findRow(focusedId);
                    if (row && row->enabled && row->kind != ControlKind::Radio)
                        onCommand(focusedId);
                }
                return 0;
            }
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            focusVisible = false;
            surface.invalidate();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == LBN_SELCHANGE)
                onCommand(LOWORD(wp));
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::Settings), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

SettingsDialog::SettingsDialog() : impl_(std::make_unique<Impl>()) {}

SettingsDialog::~SettingsDialog() {
    if (impl_ && impl_->hwnd)
        impl_->destroy();
}

bool SettingsDialog::create(HINSTANCE inst, HWND parent, const SettingsState& state,
                            SettingsActions actions) {
    impl_->notifyHwnd = parent;
    impl_->inst = inst;
    impl_->state = state;
    impl_->actions = std::move(actions);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricSettingsDialog";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(kWindowW * s)),
            static_cast<LONG>(std::lround(kWindowH * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricSettingsDialog", L"设置",
                                  kDialogStyle, x, y, w, h, nullptr, nullptr, inst,
                                  impl_.get());
    return impl_->hwnd != nullptr;
}

void SettingsDialog::updateState(const SettingsState& state) {
    if (impl_->hwnd)
        impl_->updateState(state);
}

void SettingsDialog::updateFontDescription(const std::wstring& description) {
    if (impl_->hwnd)
        impl_->updateFontDescription(description);
}

void SettingsDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }
}

void SettingsDialog::destroy() {
    impl_->destroy();
}

bool SettingsDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND SettingsDialog::hwnd() const {
    return impl_->hwnd;
}
