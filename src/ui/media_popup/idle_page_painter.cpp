#include "ui/media_popup/page_painter.h"
#include "ui/fluent_theme.h"
#include "util/com_release.h"
#include <algorithm>
#include <cmath>

namespace media_popup_paint {
void drawIdleQuickHeader(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                         float headerTop, const IdlePresentation& content, bool updateHitTest) {
    auto& brushSecondary = context.resources.brushSecondary;
    auto& fmtIdleHeader = context.resources.fmtIdleHeader;

    drawIdleQuickExpandButton(context, rt, w, headerTop, content, updateHitTest);
    const D2D1_RECT_F expandButton =
        layoutFor(context, content).idleQuickExpandLocalRect(w, headerTop, content);
    const float headerRight =
        content.quickStartEnabled && content.showText ? expandButton.left - 8.0f : w - 16.0f;
    drawText(rt, content.todayTasksEnabled ? L"快捷启动与今日任务" : L"快捷启动",
             fmtIdleHeader,
             D2D1::RectF(16.0f, headerTop, headerRight, headerTop + kIdleQuickHeaderHeightDip),
             brushSecondary);
}

void drawIdleQuickDivider(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                          float headerTop, const IdlePresentation& content) {
    auto& brushProgressTrack = context.resources.brushProgressTrack;

    if (!rt || !brushProgressTrack || !content.showText)
        return;
    const float opacity = 1.0f - layoutFor(context, content).idleQuickExpandProgress(content);
    if (opacity <= 0.0f)
        return;
    const float dividerTop = layoutFor(context, content).idleQuickExpandButtonTop(headerTop) - 4.0f;
    brushProgressTrack->SetOpacity(opacity);
    rt->FillRectangle(D2D1::RectF(16.0f, dividerTop, w - 16.0f, dividerTop + 1.0f),
                      brushProgressTrack);
    brushProgressTrack->SetOpacity(1.0f);
}

void drawIdleQuoteText(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                       const IdlePresentation& content, float quoteHeight,
                       IDWriteTextLayout* layout, float textWidth, float textHeight,
                       float scrollOffset, IDWriteTextLayout* sourceLayout, float sourceWidth,
                       float sourceHeight, float sourceScrollOffset) {
    auto& brushText = context.resources.brushText;
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushDisabled = context.resources.brushDisabled;
    auto& brushIdleTextEdgeFade = context.resources.brushIdleTextEdgeFade;
    auto& fmtIdleQuote = context.resources.fmtIdleQuote;
    auto& fmtIdleSource = context.resources.fmtIdleSource;

    if (!rt)
        return;
    const float quoteTop = kIdleQuoteTopDip;
    const D2D1_RECT_F quoteRect = D2D1::RectF(16.0f, quoteTop, w - 16.0f, quoteTop + quoteHeight);
    if (layout && !content.sentence.empty()) {
        drawScrollingText(context, rt, layout, textWidth, textHeight, quoteRect, scrollOffset,
                          brushText, brushIdleTextEdgeFade);
    } else if (!content.sentence.empty()) {
        drawText(rt, content.sentence, fmtIdleQuote, quoteRect,
                 content.loading ? brushSecondary : brushText);
    } else if (content.loading) {
        drawText(rt, L"正在获取每日一言…", fmtIdleQuote, quoteRect, brushSecondary);
    } else if (content.showQuote) {
        drawText(rt, L"暂时无法获取每日一言", fmtIdleQuote, quoteRect, brushDisabled);
    }

    const float sourceTop = quoteTop + quoteHeight + kIdleQuoteSourceGapDip;
    const D2D1_RECT_F sourceRect = D2D1::RectF(16.0f, sourceTop, w - 16.0f, sourceTop + 18.0f);
    if (sourceLayout && !content.source.empty()) {
        drawScrollingText(context, rt, sourceLayout, sourceWidth, sourceHeight, sourceRect,
                          sourceScrollOffset, brushSecondary, brushIdleTextEdgeFade);
    } else if (!content.source.empty()) {
        drawText(rt, content.source, fmtIdleSource, sourceRect, brushSecondary);
    }
}

void drawIdleQuoteUnit(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                       const IdlePresentation& content, bool current) {
    auto& idle = context.idle.idle;
    auto& idleQuoteLayout = context.resources.idleQuoteLayout;
    auto& idleQuoteSourceLayout = context.resources.idleQuoteSourceLayout;
    auto& idleQuoteWidth = context.idle.idleQuoteWidth;
    auto& idleQuoteHeight = context.idle.idleQuoteHeight;
    auto& idleQuoteSourceWidth = context.idle.idleQuoteSourceWidth;
    auto& idleQuoteSourceHeight = context.idle.idleQuoteSourceHeight;
    auto& idleQuoteScrollOffset = context.idle.idleQuoteScrollOffset;
    auto& idleQuoteSourceScrollOffset = context.idle.idleQuoteSourceScrollOffset;

    const float quoteHeight =
        idleUnitHeight(content, current, context.idle.idle, context.resources.idleQuoteLayout,
                       context.idle.idleQuoteHeight);
    const bool useCurrentQuoteLayout =
        current && idleQuoteLayout && content.sentence == idle.sentence;
    const bool useCurrentSourceLayout =
        current && idleQuoteSourceLayout && content.source == idle.source;
    drawIdleQuoteText(context, rt, w, content, quoteHeight,
                      useCurrentQuoteLayout ? idleQuoteLayout : nullptr,
                      useCurrentQuoteLayout ? idleQuoteWidth : 0.0f,
                      useCurrentQuoteLayout ? idleQuoteHeight : 0.0f,
                      useCurrentQuoteLayout ? idleQuoteScrollOffset : 0.0f,
                      useCurrentSourceLayout ? idleQuoteSourceLayout : nullptr,
                      useCurrentSourceLayout ? idleQuoteSourceWidth : 0.0f,
                      useCurrentSourceLayout ? idleQuoteSourceHeight : 0.0f,
                      useCurrentSourceLayout ? idleQuoteSourceScrollOffset : 0.0f);
}

void drawIdleCopyButton(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                        const IdlePresentation& content, bool updateHitTest) {
    auto& categoryTransition = context.state.categoryTransition;
    auto& cardOriginDip = context.geometry.cardOriginDip;
    auto& hoverCopy = context.interaction.hoverCopy;
    auto& pressedCopy = context.interaction.pressedCopy;
    auto& copySucceeded = context.interaction.copySucceeded;
    auto& copyRect = context.geometry.copyRect;
    auto& brushStroke = context.resources.brushStroke;
    auto& brushText = context.resources.brushText;
    auto& fmtIdleSource = context.resources.fmtIdleSource;

    if (!rt || !idleCopyAvailable(content)) {
        if (updateHitTest)
            copyRect = {};
        return;
    }

    // 复制和返回操作放在同一排，二者之间固定留出 8 DIP，避免悬浮底互相覆盖。
    // 页面互切时箭头可能暂时不绘制，但它的槽位仍要保留，避免复制按钮横向跳动。
    const bool reservePageArrowSlot =
        idleReturnArrowVisible(content, context.state.playbackScene, context.state.available,
                               context.state.idlePanelManual) ||
        categoryTransition.active();
    const float copyRight = reservePageArrowSlot ? w - 52.0f : w - 16.0f;
    const D2D1_RECT_F localCopy = D2D1::RectF(copyRight - 32.0f, 7.0f, copyRight, 39.0f);
    drawCopyButton(context, rt, localCopy, updateHitTest && hoverCopy, updateHitTest && pressedCopy,
                   copySucceeded);
    if (updateHitTest && hoverCopy && !copySucceeded) {
        // 提示与按钮同高并放在左侧，避免遮住一言正文，也不会改变按钮占位。
        constexpr float kTooltipWidthDip = 104.0f;
        const float tooltipRight = localCopy.left - 8.0f;
        const D2D1_RECT_F tooltip =
            D2D1::RectF(std::max(8.0f, tooltipRight - kTooltipWidthDip), 7.0f, tooltipRight, 39.0f);

        // 提示文字和气泡底沿用同一套动态对比逻辑，避免深色背景下出现白字
        // 落在浅色控件底上的低对比组合。
        drawAdaptiveControlSurface(context, rt, tooltip, 8.0f, false, PopupPage::Idle, false);
        if (brushStroke) {
            brushStroke->SetOpacity(0.7f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(tooltip, 8.0f, 8.0f), brushStroke, 0.8f);
            brushStroke->SetOpacity(1.0f);
        }
        drawText(rt, L"复制每日一言", fmtIdleSource,
                 D2D1::RectF(tooltip.left + 8.0f, tooltip.top + 7.0f, tooltip.right - 8.0f,
                             tooltip.bottom - 5.0f),
                 brushText);
    }
    if (updateHitTest)
        copyRect = D2D1::RectF(localCopy.left, cardOriginDip + localCopy.top, localCopy.right,
                               cardOriginDip + localCopy.bottom);
}

void drawIdleQuickExpandButton(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                               float headerTop, const IdlePresentation& content,
                               bool updateHitTest) {
    auto& quickExpansion = context.idle.quickExpansion;
    auto& cardOriginDip = context.geometry.cardOriginDip;
    auto& hoverIdleQuickExpand = context.interaction.hoverIdleQuickExpand;
    auto& pressedIdleQuickExpand = context.interaction.pressedIdleQuickExpand;
    auto& idleQuickExpandRect = context.geometry.idleQuickExpandRect;
    auto& brushSecondary = context.resources.brushSecondary;

    if (!rt || !content.quickStartEnabled || !content.showText) {
        if (updateHitTest)
            idleQuickExpandRect = {};
        return;
    }

    const D2D1_RECT_F local =
        layoutFor(context, content).idleQuickExpandLocalRect(w, headerTop, content);
    const D2D1_RECT_F trigger =
        layoutFor(context, content).idleQuickTriggerLocalRect(w, headerTop, content);
    if (updateHitTest) {
        idleQuickExpandRect = D2D1::RectF(trigger.left, cardOriginDip + trigger.top, trigger.right,
                                          cardOriginDip + trigger.bottom);
    }
    // 高亮与命中测试解耦：理由同 drawPageArrow——快速展开按钮本身就是
    // 转场触发器，层路径不画高亮会在挂载/撤层瞬间闪变。
    if (hoverIdleQuickExpand || pressedIdleQuickExpand) {
        // 可见悬浮底与返回媒体按钮同为 32 DIP；命中区仍额外保留 4 DIP。
        const D2D1_RECT_F hoverRect =
            D2D1::RectF(trigger.left, local.top, trigger.right, local.bottom);
        drawAdaptiveControlSurface(context, rt, hoverRect, 6.0f, pressedIdleQuickExpand,
                                   PopupPage::Idle);
    }

    // 收起状态提示向上展开，展开状态提示向下收起。
    drawVerticalChevron(
        rt,
        D2D1::Point2F((local.left + local.right) * 0.5f, (local.top + local.bottom) * 0.5f), 6.5f,
        brushSecondary, !quickExpansion.expanded);
}

void drawIdleQuickTabs(const PagePaintContext& context, ID2D1DeviceContext* rt, float w, float top,
                       float progress, const IdlePresentation& content, bool updateHitTest) {
    auto& idleQuickTab = context.idle.idleQuickTab;
    auto& hoverIdleQuickTab = context.interaction.hoverIdleQuickTab;
    auto& pressedIdleQuickTab = context.interaction.pressedIdleQuickTab;
    auto& idleQuickTabRects = context.geometry.idleQuickTabRects;
    auto& brushText = context.resources.brushText;
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushAccent = context.resources.brushAccent;
    auto& fmtIdleApp = context.resources.fmtIdleApp;

    if (!rt)
        return;
    if (!content.todayTasksEnabled || progress <= 0.01f) {
        if (updateHitTest) {
            idleQuickTabRects[0] = {};
            idleQuickTabRects[1] = {};
        }
        return;
    }

    const float tabWidth =
        std::min(kIdleQuickTabWidthDip, std::max(1.0f, (w - 32.0f - kIdleQuickTabGapDip) * 0.5f));
    const float left = 16.0f;
    const D2D1_RECT_F tabs[2] = {
        D2D1::RectF(left, top, left + tabWidth, top + kIdleQuickTabHeightDip),
        D2D1::RectF(left + tabWidth + kIdleQuickTabGapDip, top,
                    left + tabWidth * 2.0f + kIdleQuickTabGapDip, top + kIdleQuickTabHeightDip),
    };
    if (updateHitTest) {
        idleQuickTabRects[0] = tabs[0];
        idleQuickTabRects[1] = tabs[1];
    }

    const float opacity = std::clamp(progress, 0.0f, 1.0f);
    const wchar_t* labels[2] = {L"快速启动", L"今日任务"};
    for (int i = 0; i < 2; ++i) {
        const bool selected = (i == 0 && idleQuickTab == IdleQuickTab::QuickStart) ||
                              (i == 1 && idleQuickTab == IdleQuickTab::TodayTasks);
        const bool hovered = i == hoverIdleQuickTab;
        const bool pressed = i == pressedIdleQuickTab;
        if (selected && brushAccent) {
            const float previousOpacity = brushAccent->GetOpacity();
            brushAccent->SetOpacity(previousOpacity * 0.16f * opacity);
            rt->FillRoundedRectangle(D2D1::RoundedRect(tabs[i], 8.0f, 8.0f), brushAccent);
            brushAccent->SetOpacity(previousOpacity);
        }
        if (hovered || pressed)
            drawAdaptiveControlSurface(context, rt, tabs[i], 8.0f, pressed, PopupPage::Idle);
        if (selected && brushAccent) {
            const float previousOpacity = brushAccent->GetOpacity();
            brushAccent->SetOpacity(previousOpacity * opacity);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(tabs[i].left + 12.0f, tabs[i].bottom - 2.0f,
                                              tabs[i].right - 12.0f, tabs[i].bottom),
                                  1.0f, 1.0f),
                brushAccent);
            brushAccent->SetOpacity(previousOpacity);
        }
        ID2D1SolidColorBrush* textBrush = selected ? brushText : brushSecondary;
        if (textBrush) {
            const float previousOpacity = textBrush->GetOpacity();
            textBrush->SetOpacity(previousOpacity * opacity);
            drawText(rt, labels[i], fmtIdleApp, tabs[i], textBrush);
            textBrush->SetOpacity(previousOpacity);
        }
    }
}

void drawIdleTodayTasks(const PagePaintContext& context, ID2D1DeviceContext* rt,
                        const IdlePresentation& content, const D2D1_RECT_F* paintBounds) {
    auto& idleScrollOffset = context.idle.idleScrollOffset;
    auto& hoverIdleTask = context.interaction.hoverIdleTask;
    auto& hoverIdleTaskCheckbox = context.interaction.hoverIdleTaskCheckbox;
    auto& pressedIdleTask = context.interaction.pressedIdleTask;
    auto& pressedIdleTaskCheckbox = context.interaction.pressedIdleTaskCheckbox;
    auto& idleListRect = context.geometry.idleListRect;
    auto& brushStroke = context.resources.brushStroke;
    auto& brushText = context.resources.brushText;
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushDisabled = context.resources.brushDisabled;
    auto& brushIdleCell = context.resources.brushIdleCell;
    auto& brushIdleCellHover = context.resources.brushIdleCellHover;
    auto& brushAccent = context.resources.brushAccent;
    auto& brushTextOnAccent = context.resources.brushTextOnAccent;
    auto& brushTaskPriorityHigh = context.resources.brushTaskPriorityHigh;
    auto& brushTaskPriorityMedium = context.resources.brushTaskPriorityMedium;
    auto& brushTaskPriorityLow = context.resources.brushTaskPriorityLow;
    auto& brushTaskPriorityNone = context.resources.brushTaskPriorityNone;
    auto& fmtSource = context.resources.fmtSource;
    auto& fmtTimeRight = context.resources.fmtTimeRight;
    auto& fmtIdleApp = context.resources.fmtIdleApp;

    if (!rt)
        return;
    if (content.todayTasks.empty()) {
        const std::wstring message = content.todayTasksLoading           ? L"正在同步今日任务…"
                                     : !content.todayTasksStatus.empty() ? content.todayTasksStatus
                                     : !content.todayTasksConnected      ? L"请在设置中连接滴答清单"
                                                                         : L"今天没有待办任务";
        drawText(rt, message, fmtIdleApp, idleListRect,
                 content.todayTasksLoading ? brushSecondary : brushDisabled);
        return;
    }

    for (size_t i = 0; i < content.todayTasks.size(); ++i) {
        const auto& task = content.todayTasks[i];
        const float rowTop = idleListRect.top +
                             static_cast<float>(i) * (kIdleTaskRowHeightDip + kIdleTaskRowGapDip) -
                             idleScrollOffset;
        const D2D1_RECT_F row = D2D1::RectF(idleListRect.left, rowTop, idleListRect.right,
                                            rowTop + kIdleTaskRowHeightDip);
        const auto& bounds = paintBounds ? *paintBounds : idleListRect;
        if (row.bottom < bounds.top || row.top > bounds.bottom)
            continue;

        const bool hovered = static_cast<int>(i) == hoverIdleTask;
        const bool checkboxHovered = static_cast<int>(i) == hoverIdleTaskCheckbox;
        const bool pressed = static_cast<int>(i) == pressedIdleTask;
        const bool checkboxPressed = static_cast<int>(i) == pressedIdleTaskCheckbox;
        rt->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f),
                                 hovered ? brushIdleCellHover : brushIdleCell);
        if (pressed)
            drawAdaptiveControlSurface(context, rt, row, 8.0f, true, PopupPage::Idle);
        if (brushStroke) {
            brushStroke->SetOpacity(hovered || pressed ? 0.85f : 0.45f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), brushStroke,
                                     hovered || pressed ? 1.0f : 0.75f);
            brushStroke->SetOpacity(1.0f);
        }
        ID2D1SolidColorBrush* priorityBrush = nullptr;
        switch (task.priority) {
        case IdleTaskPriority::High:
            priorityBrush = brushTaskPriorityHigh;
            break;
        case IdleTaskPriority::Medium:
            priorityBrush = brushTaskPriorityMedium;
            break;
        case IdleTaskPriority::Low:
            priorityBrush = brushTaskPriorityLow;
            break;
        case IdleTaskPriority::None:
        default:
            priorityBrush = brushTaskPriorityNone;
            break;
        }
        if (priorityBrush) {
            const float previousOpacity = priorityBrush->GetOpacity();
            priorityBrush->SetOpacity(previousOpacity * 0.95f);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(row.left, row.top + 6.0f, row.left + 4.0f, row.bottom - 7.0f), 1.5f,
                    1.5f),
                priorityBrush);
            priorityBrush->SetOpacity(previousOpacity);
        }

        const D2D1_RECT_F checkbox =
            D2D1::RectF(row.left + 12.0f, row.top + 7.0f, row.left + 26.0f, row.top + 21.0f);
        if ((checkboxHovered || checkboxPressed) && !task.completed)
            drawAdaptiveControlSurface(context, rt, checkbox, 4.0f, checkboxPressed,
                                       PopupPage::Idle);
        if (task.completed && brushAccent) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(checkbox, 4.0f, 4.0f), brushAccent);
            ID2D1Brush* checkBrush = brushTextOnAccent ? brushTextOnAccent : brushText;
            if (checkBrush) {
                rt->DrawLine(D2D1::Point2F(checkbox.left + 3.0f, checkbox.top + 7.0f),
                             D2D1::Point2F(checkbox.left + 6.0f, checkbox.top + 10.0f), checkBrush,
                             1.4f);
                rt->DrawLine(D2D1::Point2F(checkbox.left + 6.0f, checkbox.top + 10.0f),
                             D2D1::Point2F(checkbox.right - 3.0f, checkbox.top + 4.0f), checkBrush,
                             1.4f);
            }
        } else if (brushSecondary) {
            brushSecondary->SetOpacity(0.82f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(checkbox, 4.0f, 4.0f), brushSecondary, 1.1f);
            brushSecondary->SetOpacity(1.0f);
        }

        ID2D1Brush* titleBrush = task.completed ? brushDisabled : brushText;
        ID2D1Brush* dueBrush = task.completed ? brushDisabled
                               : task.overdue ? brushAccent
                                              : brushSecondary;
        drawText(
            rt, task.title, fmtSource,
            D2D1::RectF(row.left + 38.0f, row.top + 3.0f, row.right - 96.0f, row.bottom - 3.0f),
            titleBrush);
        drawText(
            rt, task.dueText, fmtTimeRight,
            D2D1::RectF(row.right - 92.0f, row.top + 3.0f, row.right - 8.0f, row.bottom - 3.0f),
            dueBrush);
    }
}

void drawIdleQuickListContent(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                              const IdlePresentation& content, const D2D1_RECT_F* paintBounds) {
    auto& idleScrollOffset = context.idle.idleScrollOffset;
    auto& idleScrollMax = context.idle.idleScrollMax;
    auto& idleQuickTab = context.idle.idleQuickTab;
    auto& hoverIdleApp = context.interaction.hoverIdleApp;
    auto& idleListRect = context.geometry.idleListRect;
    auto& idleScrollTrackRect = context.geometry.idleScrollTrackRect;
    auto& idleScrollThumbRect = context.geometry.idleScrollThumbRect;
    auto& idleIconBitmaps = context.resources.idleIconBitmaps;
    auto& brushStroke = context.resources.brushStroke;
    auto& brushText = context.resources.brushText;
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushDisabled = context.resources.brushDisabled;
    auto& brushProgressTrack = context.resources.brushProgressTrack;
    auto& brushIdleCell = context.resources.brushIdleCell;
    auto& brushIdleCellHover = context.resources.brushIdleCellHover;
    auto& fmtIdleApp = context.resources.fmtIdleApp;

    if (!rt)
        return;
    // 方块边框以几何边界为中心绘制；给可视裁剪区留出 1 DIP，避免首行
    // 的上边框被裁掉，同时不改变列表的布局和命中区域。
    const D2D1_RECT_F visualClip =
        paintBounds
            ? *paintBounds
            : D2D1::RectF(idleListRect.left, idleListRect.top - kIdleQuickVisualClipPaddingDip,
                          idleListRect.right, idleListRect.bottom + kIdleQuickVisualClipPaddingDip);
    rt->PushAxisAlignedClip(visualClip, D2D1_ANTIALIAS_MODE_ALIASED);
    if (content.todayTasksEnabled && idleQuickTab == IdleQuickTab::TodayTasks) {
        drawIdleTodayTasks(context, rt, content, paintBounds);
    } else if (content.apps.empty()) {
        drawText(rt, L"请在设置中添加应用", fmtIdleApp, idleListRect, brushDisabled);
    } else {
        const float columnGap = kIdleQuickCellGapDip;
        const float gridLeft =
            layoutFor(context, content).idleQuickGridLeft(context.geometry.idleListRect);
        for (size_t i = 0; i < content.apps.size(); ++i) {
            const size_t column = i % kIdleQuickColumnCount;
            const size_t rowIndex = i / kIdleQuickColumnCount;
            const float cellTop = idleListRect.top +
                                  static_cast<float>(rowIndex) * kIdleQuickRowPitchDip -
                                  idleScrollOffset;
            const float cellLeft =
                gridLeft + static_cast<float>(column) * (kIdleQuickCellSizeDip + columnGap);
            const D2D1_RECT_F cell =
                D2D1::RectF(cellLeft, cellTop, cellLeft + kIdleQuickCellSizeDip,
                            cellTop + kIdleQuickCellSizeDip);
            if (cell.bottom < visualClip.top || cell.top > visualClip.bottom)
                continue;
            const bool hovered = static_cast<int>(i) == hoverIdleApp;
            const bool valid = content.apps[i].pathValid;
            rt->FillRoundedRectangle(D2D1::RoundedRect(cell, 9.0f, 9.0f),
                                     hovered ? brushIdleCellHover : brushIdleCell);
            if (brushStroke) {
                brushStroke->SetOpacity(hovered ? 0.85f : 0.55f);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(cell, 9.0f, 9.0f), brushStroke,
                                         hovered ? 1.0f : 0.75f);
                brushStroke->SetOpacity(1.0f);
            }

            const bool showName = content.showAppNames;
            const float iconTop =
                showName ? cell.top + kIdleQuickNamedIconTopDip
                         : cell.top + (kIdleQuickCellSizeDip - kIdleQuickIconSizeDip) * 0.5f;
            const D2D1_RECT_F iconRect = D2D1::RectF(
                cell.left + (kIdleQuickCellSizeDip - kIdleQuickIconSizeDip) * 0.5f, iconTop,
                cell.left + (kIdleQuickCellSizeDip + kIdleQuickIconSizeDip) * 0.5f,
                iconTop + kIdleQuickIconSizeDip);
            if (i < idleIconBitmaps.size() && idleIconBitmaps[i])
                rt->DrawBitmap(idleIconBitmaps[i], iconRect, valid ? 1.0f : 0.45f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (showName) {
                const D2D1_RECT_F textRect =
                    D2D1::RectF(cell.left + 4.0f, cell.bottom - kIdleQuickNamedTextTopInsetDip,
                                cell.right - 4.0f, cell.bottom - kIdleQuickNamedTextBottomInsetDip);
                drawText(rt,
                         content.apps[i].name.empty() ? L"未命名应用" : content.apps[i].name,
                         fmtIdleApp, textRect, valid ? brushText : brushDisabled);
            }
        }
    }
    rt->PopAxisAlignedClip();
    if (!paintBounds && idleScrollMax > 0.0f) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(idleScrollTrackRect, 1.5f, 1.5f),
                                 brushProgressTrack);
        rt->FillRoundedRectangle(D2D1::RoundedRect(idleScrollThumbRect, 1.5f, 1.5f),
                                 brushSecondary);
    }
}

} // namespace media_popup_paint
