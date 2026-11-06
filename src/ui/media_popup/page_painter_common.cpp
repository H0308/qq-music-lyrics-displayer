#include "ui/media_popup/page_painter.h"
#include "ui/fluent_theme.h"
#include "util/com_release.h"
#include <algorithm>
#include <cmath>

namespace media_popup_paint {
std::wstring sourceLabel(const std::wstring& source) {
    if (source.find(L"QQMusic") != std::wstring::npos ||
        source.find(L"qqmusic") != std::wstring::npos)
        return L"QQ音乐";
    if (source.find(L"cloudmusic") != std::wstring::npos ||
        source.find(L"Netease") != std::wstring::npos ||
        source.find(L"netease") != std::wstring::npos)
        return L"网易云音乐";
    return L"音乐播放器";
}

std::wstring formatPlaybackTime(int64_t milliseconds) {
    const int64_t totalSeconds = std::max<int64_t>(milliseconds, 0) / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t seconds = totalSeconds % 60;
    auto twoDigits = [](int64_t value) {
        std::wstring text = std::to_wstring(value);
        if (text.size() < 2)
            text.insert(text.begin(), L'0');
        return text;
    };

    if (hours > 0)
        return std::to_wstring(hours) + L":" + twoDigits(minutes) + L":" + twoDigits(seconds);
    return twoDigits(totalSeconds / 60) + L":" + twoDigits(seconds);
}

std::wstring formatPlaybackDuration(int64_t milliseconds) {
    return milliseconds > 0 ? formatPlaybackTime(milliseconds) : L"--:--";
}

bool buttonEnabled(bool available, int index, const OverlayMediaInfo& content) {
    return available && (index == 1 ? content.canPlayPause : true);
}
bool idleCopyAvailable(const IdlePresentation& content) {
    return content.showQuote && content.copyEnabled && !content.loading &&
           !content.sentence.empty();
}
bool idleReturnArrowVisible(const IdlePresentation& content, bool playbackScene, bool available,
                            bool manual) {
    return playbackScene && available && content.quickStartEnabled && manual;
}
float idleUnitHeight(const IdlePresentation& content, bool current, const IdlePresentation& idle,
                     IDWriteTextLayout* layout, float height) {
    return current && layout && content.sentence == idle.sentence ? std::max(18.0f, height) : 18.0f;
}
IdlePageLayout layoutFor(const PagePaintContext& context, const IdlePresentation& content) {
    return {context.idle.quickExpansion.progress, context.idle.idleQuickTab,
            idleReturnArrowVisible(content, context.state.playbackScene, context.state.available,
                                   context.state.idlePanelManual) ||
                context.state.categoryTransition.active()};
}
void drawText(const PagePaintContext& context, ID2D1DeviceContext* rt, const std::wstring& text,
              IDWriteTextFormat* format, const D2D1_RECT_F& rect, ID2D1Brush* brush) {

    if (text.empty() || !format || !brush)
        return;
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void drawScrollingText(const PagePaintContext& context, ID2D1DeviceContext* rt,
                       IDWriteTextLayout* layout, float textWidth, float textHeight,
                       const D2D1_RECT_F& rect, float offset, ID2D1Brush* brush,
                       ID2D1Brush* edgeFadeBrush) {
    auto& textEdgeFadeLayer = context.resources.textEdgeFadeLayer;

    if (!rt || !layout || !brush)
        return;
    const float areaWidth = std::max(0.0f, rect.right - rect.left);
    const float y = rect.top + std::max(0.0f, (rect.bottom - rect.top - textHeight) * 0.5f);
    const bool scrolling = textWidth > areaWidth;
    const float loopWidth = textWidth + kPopupTextPaddingDip * 2.0f;
    const float bases[2] = {rect.left - offset, rect.left - offset + loopWidth};
    const int count = scrolling ? 2 : 1;

    const bool useEdgeFade = scrolling && edgeFadeBrush && textEdgeFadeLayer;
    if (useEdgeFade) {
        rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Identity(), 1.0f, edgeFadeBrush),
                      textEdgeFadeLayer);
    }
    rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < count; ++i) {
        const float x = scrolling ? bases[i] : rect.left;
        rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
    }
    rt->PopAxisAlignedClip();
    if (useEdgeFade)
        rt->PopLayer();
}

void drawChevron(const PagePaintContext& context, ID2D1DeviceContext* rt, D2D1_POINT_2F center,
                 float size, ID2D1Brush* brush, bool right) {

    if (!rt || !brush)
        return;
    const float direction = right ? 1.0f : -1.0f;
    const D2D1_POINT_2F top =
        D2D1::Point2F(center.x - direction * size * 0.45f, center.y - size * 0.62f);
    const D2D1_POINT_2F middle = D2D1::Point2F(center.x + direction * size * 0.35f, center.y);
    const D2D1_POINT_2F bottom =
        D2D1::Point2F(center.x - direction * size * 0.45f, center.y + size * 0.62f);
    rt->DrawLine(top, middle, brush, 1.35f);
    rt->DrawLine(middle, bottom, brush, 1.35f);
}

void drawVerticalChevron(const PagePaintContext& context, ID2D1DeviceContext* rt,
                         D2D1_POINT_2F center, float size, ID2D1Brush* brush, bool up) {

    if (!rt || !brush)
        return;
    const float direction = up ? -1.0f : 1.0f;
    const float halfWidth = size * 0.52f;
    const float shoulderOffset = size * 0.18f;
    const float tipOffset = size * 0.42f;
    // 尖端比两侧端点伸得更远，按实际包围盒中心校正视觉位置。
    const float iconCenterY = center.y - direction * (tipOffset - shoulderOffset) * 0.5f;
    const D2D1_POINT_2F left =
        D2D1::Point2F(center.x - halfWidth, iconCenterY - direction * shoulderOffset);
    const D2D1_POINT_2F middle = D2D1::Point2F(center.x, iconCenterY + direction * tipOffset);
    const D2D1_POINT_2F right =
        D2D1::Point2F(center.x + halfWidth, iconCenterY - direction * shoulderOffset);
    constexpr float kStrokeWidth = 1.5f;
    rt->DrawLine(left, middle, brush, kStrokeWidth);
    rt->DrawLine(middle, right, brush, kStrokeWidth);

    // DrawLine 默认端点是平的，用圆点补齐端帽和折点，形成更柔和的 Fluent 风格。
    const float capRadius = kStrokeWidth * 0.5f;
    rt->FillEllipse(D2D1::Ellipse(left, capRadius, capRadius), brush);
    rt->FillEllipse(D2D1::Ellipse(middle, capRadius, capRadius), brush);
    rt->FillEllipse(D2D1::Ellipse(right, capRadius, capRadius), brush);
}

bool usesBlackTextForControls(const PagePaintContext& context, PopupPage page) {
    auto& brushText = context.resources.brushText;

    if (!context.contrast[static_cast<size_t>(page)])
        return !fluent::isDarkMode(fluent::ThemeTarget::Window);
    if (!brushText)
        return !fluent::isDarkMode(fluent::ThemeTarget::Window);
    const D2D1_COLOR_F textColor = brushText->GetColor();
    return textColor.r + textColor.g + textColor.b < 1.5f;
}

D2D1_COLOR_F adaptiveControlFill(const PagePaintContext& context, bool pressed, PopupPage page) {

    if (usesBlackTextForControls(context, page))
        return pressed ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f)
                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.82f);
    return pressed ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.38f);
}

D2D1_COLOR_F staticControlFill(const PagePaintContext& context, bool pressed, PopupPage page) {

    const D2D1_COLOR_F card = context.cardFills[static_cast<size_t>(page)];
    const float luminance = 0.2126f * card.r + 0.7152f * card.g + 0.0722f * card.b;
    const bool darkCard = luminance < 0.5f;
    if (darkCard)
        return pressed ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)
                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);
    return pressed ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f);
}

void drawAdaptiveControlSurface(const PagePaintContext& context, ID2D1DeviceContext* rt,
                                const D2D1_RECT_F& rect, float radius, bool pressed, PopupPage page,
                                bool hoverSurface) {
    auto& brushControl = context.resources.brushControl;
    auto& brushControlHover = context.resources.brushControlHover;
    auto& brushControlPressed = context.resources.brushControlPressed;

    if (!rt)
        return;
    ID2D1SolidColorBrush* brush =
        hoverSurface ? (pressed ? brushControlPressed : brushControlHover) : brushControl;
    if (!brush)
        return;
    if (!context.contrast[static_cast<size_t>(page)]) {
        if (!context.frosted[static_cast<size_t>(page)] && hoverSurface) {
            // 纯色模式不读取背后应用亮度，但悬浮/按下仍使用固定的反色表面。
            const D2D1_COLOR_F previousColor = brush->GetColor();
            brush->SetColor(staticControlFill(context, pressed, page));
            rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
            brush->SetColor(previousColor);
            return;
        }
        // 磨砂动态适配关闭时，保持原有主题画刷，不读取背后应用亮度。
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
        return;
    }
    const D2D1_COLOR_F previousColor = brush->GetColor();
    brush->SetColor(adaptiveControlFill(context, pressed, page));
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    brush->SetColor(previousColor);
}

void drawCopyButton(const PagePaintContext& context, ID2D1DeviceContext* rt,
                    const D2D1_RECT_F& rect, bool hovered, bool pressed, bool succeeded) {
    auto& brushSecondary = context.resources.brushSecondary;

    if (!rt)
        return;
    if (hovered || pressed)
        drawAdaptiveControlSurface(context, rt, rect, 8.0f, pressed, PopupPage::Idle);

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    if (succeeded) {
        if (brushSecondary) {
            constexpr float kStrokeWidth = 1.9f;
            const D2D1_POINT_2F left = D2D1::Point2F(cx - 7.0f, cy + 0.2f);
            const D2D1_POINT_2F middle = D2D1::Point2F(cx - 2.0f, cy + 5.2f);
            const D2D1_POINT_2F right = D2D1::Point2F(cx + 7.0f, cy - 5.0f);
            rt->DrawLine(left, middle, brushSecondary, kStrokeWidth);
            rt->DrawLine(middle, right, brushSecondary, kStrokeWidth);

            // DrawLine 默认端点是平的，用圆点补齐端帽和折点，让成功态与复制图标
            // 保持一致的柔和线条。
            const float capRadius = kStrokeWidth * 0.5f;
            rt->FillEllipse(D2D1::Ellipse(left, capRadius, capRadius), brushSecondary);
            rt->FillEllipse(D2D1::Ellipse(middle, capRadius, capRadius), brushSecondary);
            rt->FillEllipse(D2D1::Ellipse(right, capRadius, capRadius), brushSecondary);
        }
        return;
    }

    const D2D1_RECT_F back = D2D1::RectF(cx - 6.0f, cy - 7.0f, cx + 4.0f, cy + 6.0f);
    const D2D1_RECT_F front = D2D1::RectF(cx - 3.0f, cy - 5.0f, cx + 7.0f, cy + 8.0f);
    if (brushSecondary) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(back, 2.0f, 2.0f), brushSecondary, 1.15f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(front, 2.0f, 2.0f), brushSecondary, 1.15f);
    }
}

bool createMeasuredTextLayout(const PagePaintContext& context, const std::wstring& text,
                              IDWriteTextFormat* format, float layoutHeight,
                              IDWriteTextLayout** out, float& width, float& height) {

    width = 0.0f;
    height = 0.0f;
    if (!out)
        return false;
    *out = nullptr;
    if (!context.factory || !format || text.empty())
        return false;
    if (FAILED(context.factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                 format, 100000.0f, layoutHeight, out)) ||
        !*out)
        return false;

    // 正常页和转场快照都用同一个行布局基准；外层绘制再按实际 metrics 垂直居中。
    (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED((*out)->GetMetrics(&metrics))) {
        releaseCom(*out);
        return false;
    }
    width = metrics.width;
    height = metrics.height;
    return true;
}

} // namespace media_popup_paint
