#include "font_color_dialog.h"

#include "ui/app_icon.h"
#include "ui/color_picker_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "logging/runtime_logger.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr int kIdSwatchUnplayed = 411;
constexpr int kIdSwatchPlayed = 412;
constexpr int kIdSwatchGlow = 413;
constexpr int kIdSwatchOutline = 414;
constexpr int kIdToggleGlow = 415;
constexpr int kIdToggleOutline = 416;
constexpr int kIdAlphaSlider = 417;
constexpr int kIdTabDark = 418;
constexpr int kIdTabLight = 419;
constexpr int kIdToggleGlobal = 420;
constexpr int kIdOk = 421;
constexpr int kIdCancel = 422;

constexpr int kAlphaMin = 5;
constexpr int kAlphaMax = 100;

constexpr float kOptionsWidth = 340.0f;
constexpr float kPreviewHeight = 96.0f;
constexpr float kOptionsHeight = 16.0f + 5.0f * fluent::metrics::controlHeight +
                                 4.0f * fluent::metrics::controlGap + 16.0f;
constexpr float kMinClientWidthDip = fluent::metrics::pagePadding + kOptionsWidth +
                                     fluent::metrics::pagePadding;
constexpr float kMinClientHeightDip = fluent::metrics::pagePadding + 30.0f + 20.0f +
                                       fluent::metrics::sectionGap + fluent::metrics::controlHeight +
                                       fluent::metrics::sectionGap + fluent::metrics::controlHeight +
                                       fluent::metrics::sectionGap + kOptionsHeight +
                                       fluent::metrics::sectionGap + kPreviewHeight +
                                       fluent::metrics::sectionGap + fluent::metrics::controlHeight +
                                       fluent::metrics::pagePadding;
constexpr float kMinClientAspectRatio = kMinClientWidthDip / kMinClientHeightDip;

const wchar_t kSampleText[] = L"我是你爸爸，养你这么大";

// 与任务栏 drawScrollingText 保持一致的光晕/描边参数。
constexpr float kGlowOffset = 2.4f;
constexpr float kOutlineOffset = 1.2f;
constexpr float kGlowAlpha = 0.28f;
constexpr float kOutlineAlpha = 0.50f;
constexpr float kPlayedRatio = 0.55f;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

const std::array<int, 4> kSwatchIds = {
    kIdSwatchUnplayed,
    kIdSwatchPlayed,
    kIdSwatchGlow,
    kIdSwatchOutline,
};

const std::array<int, 2> kToggleIds = {
    kIdToggleGlow,
    kIdToggleOutline,
};

const std::array<int, 2> kTabIds = {
    kIdTabDark,
    kIdTabLight,
};

bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

bool isShiftDown() {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

} // namespace

struct FontColorDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;

    State state; // 工作副本：确定前的一切修改只落在这里和预览上
    fluent::FluentDialogSurface surface;

    D2D1_RECT_F titleRect{};
    D2D1_RECT_F subtitleRect{};
    D2D1_RECT_F globalLabelRect{};
    D2D1_RECT_F tabsRect{};
    D2D1_RECT_F optionsRect{};
    D2D1_RECT_F previewRect{};
    D2D1_RECT_F alphaSliderRect{};
    D2D1_RECT_F alphaValueRect{};
    D2D1_RECT_F okRect{};
    D2D1_RECT_F cancelRect{};
    D2D1_RECT_F globalToggleRect{};
    D2D1_RECT_F tabDarkRect{};
    D2D1_RECT_F tabLightRect{};
    std::array<D2D1_RECT_F, 5> labelRects{};
    std::array<D2D1_RECT_F, 4> swatchRects{};
    std::array<D2D1_RECT_F, 2> toggleRects{};

    int activeTab = 0; // 0 深色模式，1 浅色模式
    int hoverId = 0;
    int pressedId = 0;
    int focusedId = kIdSwatchUnplayed;
    bool focusVisible = false;
    bool draggingSlider = false;

    // 内嵌取色器：同时最多打开一个，切换色块时丢弃未确认的上一个。
    std::unique_ptr<ColorPickerDialog> picker;
    ApplyCallback onApply;

    ThemeState& activeThemeState() {
        return state.global ? state.globalTheme : activeTab == 0 ? state.dark : state.light;
    }

    const ThemeState& activeThemeState() const {
        return state.global ? state.globalTheme : activeTab == 0 ? state.dark : state.light;
    }

    void setGlobal(bool enabled) {
        if (state.global == enabled)
            return;
        closePicker();
        if (enabled && !state.hasGlobalTheme) {
            state.globalTheme = activeTab == 0 ? state.dark : state.light;
            state.hasGlobalTheme = true;
        }
        if (enabled && (focusedId == kIdTabDark || focusedId == kIdTabLight))
            focusedId = kIdToggleGlobal;
        state.global = enabled;
        runtime_log::writef(L"[action][font-color] global=%s", enabled ? L"on" : L"off");
        layout();
        surface.invalidate();
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

    COLORREF& colorRefOf(int id) {
        ThemeState& theme = activeThemeState();
        switch (id) {
        case kIdSwatchPlayed:
            return theme.played;
        case kIdSwatchUnplayed:
            return theme.unplayed;
        case kIdSwatchGlow:
            return theme.glowColor;
        default:
            return theme.outlineColor;
        }
    }

    const wchar_t* titleOf(int id) const {
        switch (id) {
        case kIdSwatchPlayed:
            return L"已播放字体颜色";
        case kIdSwatchUnplayed:
            return L"未播放字体颜色";
        case kIdSwatchGlow:
            return L"光晕颜色";
        default:
            return L"描边颜色";
        }
    }

    void drawTab(fluent::FluentDialogSurface::Painter& painter,
                 const D2D1_RECT_F& rect, const wchar_t* text, int id, bool selected) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        const D2D1_COLOR_F fill = selected
                                      ? p.listSelected
                                      : (pressed ? p.controlPressed
                                                  : hovered ? p.controlHover : p.controlFill);
        painter.fillRoundRect(fill, rect, fluent::metrics::controlRadius);
        painter.strokeRoundRect(selected ? p.accent : p.cardStroke, rect, 1.0f,
                                fluent::metrics::controlRadius);
        if (focusedId == id && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
        }
        painter.drawText(text, painter.textFormat(13.0f, selected ? 600 : 400, true, true),
                         rect, p.text);
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter,
                    const D2D1_RECT_F& rect, const wchar_t* text, bool accent, int id) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        const D2D1_COLOR_F fill = accent
                                      ? (pressed ? p.accentPressed
                                                 : hovered ? p.accentHover : p.accent)
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
                         D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f,
                                     rect.bottom),
                         textColor);
    }

    void drawSwatch(fluent::FluentDialogSurface::Painter& painter,
                    const D2D1_RECT_F& rect, COLORREF color, int id) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        const D2D1_RECT_F outer = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f,
                                              rect.right - 0.5f, rect.bottom - 0.5f);
        painter.fillRoundRect(pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill,
                              outer);
        painter.strokeRoundRect(hovered ? p.accent : p.cardStroke, outer);

        const D2D1_RECT_F inner = D2D1::RectF(outer.left + 4.0f, outer.top + 4.0f,
                                              outer.right - 4.0f, outer.bottom - 4.0f);
        painter.fillRoundRect(fluent::toD2D(color), inner, 2.5f);
        if (focusedId == id && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
        }
    }

    void drawToggle(fluent::FluentDialogSurface::Painter& painter,
                    const D2D1_RECT_F& rect, bool checked, int id) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool focused = focusedId == id && focusVisible;
        const float trackH = std::min(20.0f, rect.bottom - rect.top);
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(
            rect.left + 0.5f, centerY - trackH * 0.5f, rect.right - 0.5f,
            centerY + trackH * 0.5f);
        const float radius = trackH * 0.5f;
        const float knobR = trackH * 0.5f - 3.5f;
        const float knobX = checked ? track.right - trackH * 0.5f
                                    : track.left + trackH * 0.5f;
        if (checked) {
            painter.fillRoundRect(hovered ? p.accentHover : p.accent, track, radius);
            if (auto* br = painter.brush(p.textOnAccent))
                painter.target()->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR), br);
        } else {
            painter.fillRoundRect(hovered ? p.controlHover : p.controlFill, track, radius);
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
            if (auto* br = painter.brush(p.textSecondary))
                painter.target()->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR), br);
        }
        if (focused) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(track.left + 1.5f, track.top + 1.5f,
                            track.right - 1.5f, track.bottom - 1.5f),
                1.5f, std::max(1.0f, radius - 1.5f));
        }
    }

    void drawAlphaSlider(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const ThemeState& theme = activeThemeState();
        const float trackL = alphaSliderRect.left + 8.0f;
        const float trackR = alphaSliderRect.right - 8.0f;
        const float centerY = (alphaSliderRect.top + alphaSliderRect.bottom) * 0.5f;
        const float thumbR = 8.0f;
        const float t = static_cast<float>(std::clamp(theme.unplayedAlphaPct, kAlphaMin,
                                                      kAlphaMax) - kAlphaMin) /
                        static_cast<float>(kAlphaMax - kAlphaMin);
        const float thumbX = trackL + t * std::max(0.0f, trackR - trackL);
        const D2D1_RECT_F track = D2D1::RectF(trackL, centerY - 2.0f, trackR, centerY + 2.0f);

        painter.fillRoundRect(p.controlFill, track, 2.0f);
        if (thumbX > track.left)
            painter.fillRoundRect(hoverId == kIdAlphaSlider || pressedId == kIdAlphaSlider
                                      ? p.accentHover
                                      : p.accent,
                                  D2D1::RectF(track.left, track.top, thumbX, track.bottom), 2.0f);
        if (auto* br = painter.brush(p.windowBg))
            painter.target()->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), thumbR, thumbR), br);
        if (auto* br = painter.brush(p.accent))
            painter.target()->DrawEllipse(
                D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), thumbR - 1.0f, thumbR - 1.0f),
                br, 2.0f);
        if (focusedId == kIdAlphaSlider && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(alphaSliderRect.left + 1.5f, alphaSliderRect.top + 2.0f,
                            alphaSliderRect.right - 1.5f, alphaSliderRect.bottom - 2.0f),
                1.5f, fluent::metrics::controlRadius);
        }
    }

    void drawPreview(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const ThemeState& theme = activeThemeState();
        const bool darkPreview = state.global
                                     ? fluent::isDarkMode(fluent::ThemeTarget::Taskbar)
                                     : activeTab == 0;
        const D2D1_COLOR_F previewFill = darkPreview
                                             ? fluent::toD2D(RGB(56, 56, 56))
                                             : p.cardFill;
        const D2D1_COLOR_F previewStroke = darkPreview
                                               ? fluent::toD2D(RGB(88, 88, 88))
                                               : p.cardStroke;
        painter.fillRoundRect(previewFill, previewRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(previewStroke, previewRect, 1.0f,
                                fluent::metrics::cardRadius);

        IDWriteFactory* dwrite = painter.dwrite();
        if (!dwrite)
            return;

        const wchar_t* family = state.fontFamily.empty() ? fluent::uiFontFamily()
                                                          : state.fontFamily.c_str();
        IDWriteTextFormat* format = nullptr;
        if (FAILED(dwrite->CreateTextFormat(
                family, nullptr, dwriteWeightOf(state.fontStyle), dwriteStyleOf(state.fontStyle),
                DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, state.lyricFontSize), L"zh-cn",
                &format)) ||
            !format) {
            return;
        }
        if (state.fontFamily.empty())
            fluent::applyUiFontFallback(format);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const float panelW = std::max(1.0f, previewRect.right - previewRect.left);
        const float panelH = std::max(1.0f, previewRect.bottom - previewRect.top);
        IDWriteTextLayout* layout = painter.textLayout(kSampleText, format, panelW, panelH);
        if (!layout) {
            format->Release();
            return;
        }

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)) || metrics.width <= 0.0f ||
            metrics.height <= 0.0f) {
            format->Release();
            return;
        }

        const D2D1_POINT_2F origin = D2D1::Point2F(previewRect.left, previewRect.top);
        const float textX = previewRect.left + (panelW - metrics.width) * 0.5f;
        const float textY = previewRect.top + (panelH - metrics.height) * 0.5f;
        const D2D1_RECT_F panelClip = D2D1::RectF(
            previewRect.left + 1.0f, previewRect.top + 1.0f, previewRect.right - 1.0f,
            previewRect.bottom - 1.0f);
        ID2D1DCRenderTarget* target = painter.target();
        target->PushAxisAlignedClip(panelClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        static constexpr float kDirections[8][2] = {
            {1.0f, 0.0f},        {0.7071f, 0.7071f}, {0.0f, 1.0f},
            {-0.7071f, 0.7071f}, {-1.0f, 0.0f},     {-0.7071f, -0.7071f},
            {0.0f, -1.0f},       {0.7071f, -0.7071f},
        };

        if (theme.glowOn) {
            const D2D1_COLOR_F glow = fluent::toD2D(theme.glowColor, kGlowAlpha);
            for (const auto& direction : kDirections) {
                painter.drawTextLayout(
                    layout,
                    D2D1::Point2F(origin.x + direction[0] * kGlowOffset,
                                  origin.y + direction[1] * kGlowOffset),
                    glow);
            }
        }
        if (theme.outlineOn) {
            const D2D1_COLOR_F outline = fluent::toD2D(theme.outlineColor, kOutlineAlpha);
            for (const auto& direction : kDirections) {
                painter.drawTextLayout(
                    layout,
                    D2D1::Point2F(origin.x + direction[0] * kOutlineOffset,
                                  origin.y + direction[1] * kOutlineOffset),
                    outline);
            }
        }

        painter.drawTextLayout(layout, origin,
                               fluent::toD2D(theme.unplayed,
                                             std::clamp(theme.unplayedAlphaPct, kAlphaMin,
                                                        kAlphaMax) /
                                                 100.0f));
        const float playedRight = textX + metrics.width * kPlayedRatio;
        const D2D1_RECT_F playedClip = D2D1::RectF(
            std::max(panelClip.left, textX), std::max(panelClip.top, textY - 8.0f),
            std::min(panelClip.right, playedRight),
            std::min(panelClip.bottom, textY + metrics.height + 8.0f));
        if (playedClip.right > playedClip.left && playedClip.bottom > playedClip.top) {
            target->PushAxisAlignedClip(playedClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            painter.drawTextLayout(layout, origin, fluent::toD2D(theme.played));
            target->PopAxisAlignedClip();
        }
        target->PopAxisAlignedClip();
        format->Release();
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        const auto& p = fluent::palette();
        const ThemeState& theme = activeThemeState();
        painter.drawText(L"歌词字体颜色与效果", painter.textFormat(20.0f, 600), titleRect, p.text);
        painter.drawText(L"调整歌词颜色、不透明度、光晕和描边", painter.textFormat(13.0f, 400),
                         subtitleRect, p.textSecondary);

        painter.drawText(L"全局设置", painter.textFormat(13.0f, 400), globalLabelRect, p.text);
        drawToggle(painter, globalToggleRect, state.global, kIdToggleGlobal);
        if (!state.global) {
            drawTab(painter, tabDarkRect, L"深色模式", kIdTabDark, activeTab == 0);
            drawTab(painter, tabLightRect, L"浅色模式", kIdTabLight, activeTab == 1);
        }

        painter.fillRoundRect(p.cardFill, optionsRect, fluent::metrics::cardRadius);
        painter.strokeRoundRect(p.cardStroke, optionsRect, 1.0f, fluent::metrics::cardRadius);

        const std::array<const wchar_t*, 5> labels = {
            L"未播放字体颜色", L"已播放字体颜色", L"未播放不透明度", L"光晕颜色", L"描边颜色",
        };
        for (size_t i = 0; i < labels.size(); ++i)
            painter.drawText(labels[i], painter.textFormat(13.0f, 400), labelRects[i], p.text);

        drawSwatch(painter, swatchRects[0], theme.unplayed, kIdSwatchUnplayed);
        drawSwatch(painter, swatchRects[1], theme.played, kIdSwatchPlayed);
        drawAlphaSlider(painter);
        drawSwatch(painter, swatchRects[2], theme.glowColor, kIdSwatchGlow);
        drawSwatch(painter, swatchRects[3], theme.outlineColor, kIdSwatchOutline);
        drawToggle(painter, toggleRects[0], theme.glowOn, kIdToggleGlow);
        drawToggle(painter, toggleRects[1], theme.outlineOn, kIdToggleOutline);

        wchar_t alphaText[8];
        swprintf_s(alphaText, L"%d%%", std::clamp(theme.unplayedAlphaPct, kAlphaMin, kAlphaMax));
        painter.drawText(alphaText, painter.textFormat(13.0f, 400, true, true), alphaValueRect,
                         p.textSecondary);

        drawPreview(painter);
        drawButton(painter, okRect, L"确定", true, kIdOk);
        drawButton(painter, cancelRect, L"取消", false, kIdCancel);
    }

    int hitTest(float x, float y) const {
        if (contains(globalToggleRect, x, y))
            return kIdToggleGlobal;
        if (!state.global && contains(tabDarkRect, x, y))
            return kIdTabDark;
        if (!state.global && contains(tabLightRect, x, y))
            return kIdTabLight;
        for (size_t i = 0; i < kSwatchIds.size(); ++i) {
            if (contains(swatchRects[i], x, y))
                return kSwatchIds[i];
        }
        if (contains(alphaSliderRect, x, y))
            return kIdAlphaSlider;
        for (size_t i = 0; i < kToggleIds.size(); ++i) {
            if (contains(toggleRects[i], x, y))
                return kToggleIds[i];
        }
        if (contains(okRect, x, y))
            return kIdOk;
        if (contains(cancelRect, x, y))
            return kIdCancel;
        return 0;
    }

    std::vector<int> focusOrder() const {
        if (state.global) {
            return {kIdToggleGlobal, kIdSwatchUnplayed, kIdSwatchPlayed, kIdAlphaSlider,
                    kIdToggleGlow, kIdSwatchGlow, kIdToggleOutline, kIdSwatchOutline, kIdOk,
                    kIdCancel};
        }
        return {kIdToggleGlobal, kIdTabDark, kIdTabLight, kIdSwatchUnplayed, kIdSwatchPlayed,
                kIdAlphaSlider, kIdToggleGlow, kIdSwatchGlow, kIdToggleOutline, kIdSwatchOutline,
                kIdOk, kIdCancel};
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

    void setAlpha(int value) {
        ThemeState& theme = activeThemeState();
        const int next = std::clamp(value, kAlphaMin, kAlphaMax);
        if (next == theme.unplayedAlphaPct)
            return;
        theme.unplayedAlphaPct = next;
        surface.invalidate();
    }

    void updateAlphaFromX(float x) {
        const float trackL = alphaSliderRect.left + 8.0f;
        const float trackR = alphaSliderRect.right - 8.0f;
        if (trackR <= trackL)
            return;
        const float t = std::clamp((x - trackL) / (trackR - trackL), 0.0f, 1.0f);
        setAlpha(kAlphaMin + static_cast<int>(std::lround(t * (kAlphaMax - kAlphaMin))));
    }

    void closePicker() {
        if (!picker)
            return;
        picker->destroy();
        picker.reset();
    }

    void refreshTheme() {
        if (hwnd) {
            SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
            app_icon::applyWindowIcon(hwnd);
        }
        if (picker && picker->isOpen()) {
            const HWND pickerHwnd = picker->hwnd();
            SendMessageW(pickerHwnd, WM_THEMECHANGED, 0, 0);
            app_icon::applyWindowIcon(pickerHwnd);
        }
    }

    void openPicker(int swatchId) {
        runtime_log::writef(L"[action][font-color] color-picker-open swatch=%d", swatchId);
        closePicker();
        picker = std::make_unique<ColorPickerDialog>();
        if (!picker->create(inst, hwnd, colorRefOf(swatchId), titleOf(swatchId))) {
            picker.reset();
            return;
        }
        picker->setApplyCallback([this, swatchId](COLORREF color) {
            colorRefOf(swatchId) = color;
            runtime_log::writef(L"[action][font-color] color-applied swatch=%d rgb=#%02X%02X%02X",
                                swatchId, GetRValue(color), GetGValue(color), GetBValue(color));
            surface.invalidate();
        });
        picker->show();
    }

    void selectTab(int tab) {
        if (state.global || activeTab == tab)
            return;
        closePicker();
        activeTab = tab;
        runtime_log::writef(L"[action][font-color] tab=%s", tab == 0 ? L"dark" : L"light");
        draggingSlider = false;
        pressedId = 0;
        surface.invalidate();
    }

    void applyAndClose() {
        if (onApply) {
            onApply(Result{state.global, state.hasGlobalTheme, state.light, state.dark,
                           state.globalTheme});
        }
        destroy();
    }

    void onCommand(int id) {
        runtime_log::writef(L"[action][font-color] command=%d", id);
        switch (id) {
        case kIdTabDark:
            selectTab(0);
            break;
        case kIdTabLight:
            selectTab(1);
            break;
        case kIdToggleGlobal:
            setGlobal(!state.global);
            break;
        case kIdSwatchUnplayed:
        case kIdSwatchPlayed:
        case kIdSwatchGlow:
        case kIdSwatchOutline:
            openPicker(id);
            break;
        case kIdToggleGlow:
            activeThemeState().glowOn = !activeThemeState().glowOn;
            break;
        case kIdToggleOutline:
            activeThemeState().outlineOn = !activeThemeState().outlineOn;
            break;
        case kIdOk:
            applyAndClose();
            return;
        case kIdCancel:
            destroy();
            return;
        default:
            return;
        }
        if (hwnd)
            surface.invalidate();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            surface.initialize(hwnd, backdrop);
            layout();
            return 0;
        case WM_SIZE:
            layout();
            surface.invalidate();
            return 0;
        case WM_GETMINMAXINFO:
            fluent::setDialogMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp),
                                               kDialogStyle, kDialogExStyle,
                                               kMinClientWidthDip, kMinClientHeightDip);
            return 0;
        case WM_SIZING:
            fluent::enforceDialogMinimumAspectRatio(hwnd, wp, reinterpret_cast<RECT*>(lp),
                                                     kMinClientAspectRatio);
            return TRUE;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            surface.initialize(hwnd, backdrop);
            layout();
            surface.invalidate();
            return 0;
        }
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
            if (draggingSlider) {
                updateAlphaFromX(x);
                return 0;
            }
            if (!GetCapture()) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            const int id = hitTest(x, y);
            if (id != hoverId) {
                hoverId = id;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!draggingSlider && hoverId != 0) {
                hoverId = 0;
                surface.invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / s;
            const float y = GET_Y_LPARAM(lp) / s;
            pressedId = hitTest(x, y);
            if (pressedId != 0)
                focusedId = pressedId;
            if (pressedId == kIdAlphaSlider) {
                draggingSlider = true;
                updateAlphaFromX(x);
            }
            if (pressedId != 0)
                SetCapture(hwnd);
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const float s = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / s;
            const float y = GET_Y_LPARAM(lp) / s;
            if (draggingSlider) {
                updateAlphaFromX(x);
                draggingSlider = false;
                pressedId = 0;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                surface.invalidate();
                return 0;
            }
            const int hit = hitTest(x, y);
            const int pressed = pressedId;
            pressedId = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (pressed != 0 && pressed == hit)
                onCommand(pressed);
            surface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            draggingSlider = false;
            pressedId = 0;
            surface.invalidate();
            return 0;
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
            if (!state.global && (focusedId == kIdTabDark || focusedId == kIdTabLight)) {
                if (wp == VK_LEFT || wp == VK_DOWN) {
                    selectTab(0);
                    return 0;
                }
                if (wp == VK_RIGHT || wp == VK_UP) {
                    selectTab(1);
                    return 0;
                }
            }
            if (focusedId == kIdAlphaSlider) {
                const int alpha = activeThemeState().unplayedAlphaPct;
                if (wp == VK_LEFT || wp == VK_DOWN)
                    setAlpha(alpha - 1);
                else if (wp == VK_RIGHT || wp == VK_UP)
                    setAlpha(alpha + 1);
                else if (wp == VK_HOME)
                    setAlpha(kAlphaMin);
                else if (wp == VK_END)
                    setAlpha(kAlphaMax);
                else
                    break;
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                onCommand(focusedId);
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
        case kMsgColorPickerClosed:
            if (picker && !picker->isOpen())
                picker.reset();
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            closePicker();
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::FontColor), 0);
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

        const float rowH = fluent::metrics::controlHeight;
        const float labelH = 20.0f;
        const float labelW = 140.0f;
        const float cardPad = 16.0f;
        const float rowGap = fluent::metrics::controlGap;
        const float labelX = pad + cardPad;
        const float swatchW = 76.0f;
        const float swatchH = 26.0f;
        const float swatchX = std::max(labelX + labelW + gap, w - pad - cardPad - swatchW);
        const float toggleW = 40.0f;
        const float toggleH = 20.0f;
        const float toggleX = swatchX - 10.0f - toggleW;

        const float globalY = pad + titleH + subtitleH + fluent::metrics::sectionGap;
        globalLabelRect = D2D1::RectF(pad, globalY + (rowH - labelH) * 0.5f,
                                      pad + labelW,
                                      globalY + (rowH - labelH) * 0.5f + labelH);
        globalToggleRect = D2D1::RectF(toggleX, globalY + (rowH - toggleH) * 0.5f,
                                       toggleX + toggleW,
                                       globalY + (rowH - toggleH) * 0.5f + toggleH);

        tabsRect = {};
        tabDarkRect = {};
        tabLightRect = {};
        float optionsY = globalY + rowH + fluent::metrics::sectionGap;
        if (!state.global) {
            const float tabInset = 2.0f;
            const float tabGap = fluent::metrics::compactGap;
            tabsRect = D2D1::RectF(pad, optionsY, std::max(pad, w - pad),
                                   optionsY + rowH);
            const float tabW = std::max(
                0.0f, (tabsRect.right - tabsRect.left - tabInset * 2.0f - tabGap) * 0.5f);
            tabDarkRect = D2D1::RectF(tabsRect.left + tabInset, tabsRect.top + tabInset,
                                      tabsRect.left + tabInset + tabW,
                                      tabsRect.bottom - tabInset);
            tabLightRect = D2D1::RectF(tabDarkRect.right + tabGap, tabsRect.top + tabInset,
                                       tabDarkRect.right + tabGap + tabW,
                                       tabsRect.bottom - tabInset);
            optionsY = tabsRect.bottom + fluent::metrics::sectionGap;
        }

        const float optionsH = cardPad + 5.0f * rowH + 4.0f * rowGap + cardPad;
        optionsRect = D2D1::RectF(pad, optionsY, std::max(pad, w - pad), optionsY + optionsH);

        for (int i = 0; i < 5; ++i) {
            const float y = optionsY + cardPad + i * (rowH + rowGap);
            labelRects[i] = D2D1::RectF(labelX, y + (rowH - labelH) * 0.5f,
                                        labelX + labelW,
                                        y + (rowH - labelH) * 0.5f + labelH);
        }
        swatchRects[0] = D2D1::RectF(swatchX, optionsY + cardPad +
                                             (rowH - swatchH) * 0.5f,
                                     swatchX + swatchW,
                                     optionsY + cardPad + (rowH - swatchH) * 0.5f + swatchH);
        swatchRects[1] = D2D1::RectF(swatchX, optionsY + cardPad + rowH + rowGap +
                                             (rowH - swatchH) * 0.5f,
                                     swatchX + swatchW,
                                     optionsY + cardPad + rowH + rowGap +
                                         (rowH - swatchH) * 0.5f + swatchH);

        const float alphaY = optionsY + cardPad + 2.0f * (rowH + rowGap);
        const float valueW = 44.0f;
        const float valueX = w - pad - cardPad - valueW;
        const float sliderX = labelX + labelW + gap;
        alphaSliderRect = D2D1::RectF(sliderX, alphaY, std::max(sliderX, valueX - gap),
                                      alphaY + rowH);
        alphaValueRect = D2D1::RectF(valueX, alphaY + (rowH - labelH) * 0.5f,
                                     valueX + valueW,
                                     alphaY + (rowH - labelH) * 0.5f + labelH);

        const float glowY = optionsY + cardPad + 3.0f * (rowH + rowGap);
        const float outlineY = optionsY + cardPad + 4.0f * (rowH + rowGap);
        swatchRects[2] = D2D1::RectF(swatchX, glowY + (rowH - swatchH) * 0.5f,
                                     swatchX + swatchW,
                                     glowY + (rowH - swatchH) * 0.5f + swatchH);
        swatchRects[3] = D2D1::RectF(swatchX, outlineY + (rowH - swatchH) * 0.5f,
                                     swatchX + swatchW,
                                     outlineY + (rowH - swatchH) * 0.5f + swatchH);
        toggleRects[0] = D2D1::RectF(toggleX, glowY + (rowH - toggleH) * 0.5f,
                                     toggleX + toggleW,
                                     glowY + (rowH - toggleH) * 0.5f + toggleH);
        toggleRects[1] = D2D1::RectF(toggleX, outlineY + (rowH - toggleH) * 0.5f,
                                     toggleX + toggleW,
                                     outlineY + (rowH - toggleH) * 0.5f + toggleH);

        const float buttonH = fluent::metrics::controlHeight;
        const float buttonY = h - pad - buttonH;
        const float cancelW = 88.0f;
        const float okW = 96.0f;
        cancelRect = D2D1::RectF(w - pad - cancelW, buttonY, w - pad, buttonY + buttonH);
        okRect = D2D1::RectF(w - pad - cancelW - gap - okW, buttonY,
                             w - pad - cancelW - gap, buttonY + buttonH);

        const float previewY = optionsRect.bottom + fluent::metrics::sectionGap;
        const float previewBottom = std::max(previewY, buttonY - fluent::metrics::sectionGap);
        previewRect = D2D1::RectF(pad, previewY, std::max(pad, w - pad), previewBottom);
    }

    bool isDialogMessage(MSG* msg) {
        if (hwnd && IsDialogMessageW(hwnd, msg))
            return true;
        if (picker && picker->isOpen() && IsDialogMessageW(picker->hwnd(), msg))
            return true;
        return false;
    }

    void destroy() {
        closePicker();
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

FontColorDialog::FontColorDialog() : impl_(std::make_unique<Impl>()) {}

FontColorDialog::~FontColorDialog() {
    destroy();
}

bool FontColorDialog::create(HINSTANCE inst, HWND parent, const State& initial) {
    impl_->notifyHwnd = parent; // 仅用于关闭通知；托盘窗口不能作为普通窗口的可见所有者
    impl_->inst = inst;
    impl_->state = initial;
    impl_->activeTab = fluent::isDarkMode(fluent::ThemeTarget::Taskbar) ? 0 : 1;
    if (impl_->state.global && !impl_->state.hasGlobalTheme) {
        impl_->state.globalTheme = impl_->activeTab == 0 ? impl_->state.dark : impl_->state.light;
        impl_->state.hasGlobalTheme = true;
    }
    impl_->state.light.unplayedAlphaPct =
        std::clamp(impl_->state.light.unplayedAlphaPct, kAlphaMin, kAlphaMax);
    impl_->state.dark.unplayedAlphaPct =
        std::clamp(impl_->state.dark.unplayedAlphaPct, kAlphaMin, kAlphaMax);
    impl_->state.globalTheme.unplayedAlphaPct =
        std::clamp(impl_->state.globalTheme.unplayedAlphaPct, kAlphaMin, kAlphaMax);
    impl_->focusedId = impl_->state.global
                           ? kIdToggleGlobal
                           : impl_->activeTab == 0 ? kIdTabDark : kIdTabLight;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricFontColor";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const float s = fluent::dipScale(dpi);
    const float clientW = kMinClientWidthDip;
    const float clientH = kMinClientHeightDip;
    RECT rc{0, 0, static_cast<LONG>(std::lround(clientW * s)),
            static_cast<LONG>(std::lround(clientH * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricFontColor",
                                  L"歌词字体颜色与效果", kDialogStyle, x, y, w, h,
                                  nullptr, nullptr, inst, impl_.get());
    if (impl_->hwnd)
        app_icon::applyWindowIcon(impl_->hwnd);
    return impl_->hwnd != nullptr;
}

void FontColorDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }
}

void FontColorDialog::destroy() {
    impl_->destroy();
}

bool FontColorDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND FontColorDialog::hwnd() const {
    return impl_->hwnd;
}

void FontColorDialog::refreshTheme() {
    impl_->refreshTheme();
}

bool FontColorDialog::isDialogMessage(MSG* msg) {
    return impl_->isDialogMessage(msg);
}

void FontColorDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}
