#include "ui/media_popup/page_layout.h"
#include <algorithm>

namespace media_popup_detail {
float IdlePageLayout::idleQuickExpandProgress(const IdlePresentation& content) const {
    if (!content.quickStartEnabled)
        return 0.0f;
    if (!content.showText)
        return 1.0f;
    const float linear = std::clamp(progress_, 0.0f, 1.0f);
    return linear * linear * (3.0f - 2.0f * linear);
}

float IdlePageLayout::idleQuickHeaderTop(float collapsedTop,
                                         const IdlePresentation& content) const {
    const float progress = idleQuickExpandProgress(content);
    return collapsedTop + (kIdleQuickExpandedTopDip - collapsedTop) * progress;
}

float IdlePageLayout::idleQuickExpandButtonTop(float headerTop) const {
    // 保持按钮与卡片顶栏一致；展开后顶边仍留出圆角内侧的安全间距。
    return std::max(7.0f, headerTop - 8.0f);
}

std::size_t IdlePageLayout::idleQuickRowCount(const IdlePresentation& content) const {
    if (content.apps.empty())
        return 0;
    return (content.apps.size() + kIdleQuickColumnCount - 1) / kIdleQuickColumnCount;
}

float IdlePageLayout::idleQuickGridContentHeight(const IdlePresentation& content) const {
    const std::size_t rows = idleQuickRowCount(content);
    if (rows == 0)
        return 0.0f;
    return kIdleQuickCellSizeDip + static_cast<float>(rows - 1) * kIdleQuickRowPitchDip;
}

float IdlePageLayout::idleQuickTaskContentHeight(const IdlePresentation& content) const {
    if (content.todayTasks.empty())
        return 0.0f;
    return static_cast<float>(content.todayTasks.size()) * kIdleTaskRowHeightDip +
           static_cast<float>(content.todayTasks.size() - 1) * kIdleTaskRowGapDip;
}

float IdlePageLayout::idleQuickListContentHeight(const IdlePresentation& content) const {
    return content.todayTasksEnabled && tab_ == IdleQuickTab::TodayTasks
               ? idleQuickTaskContentHeight(content)
               : idleQuickGridContentHeight(content);
}

float IdlePageLayout::idleQuickGridLeft(const D2D1_RECT_F& idleListRect) const {
    const float availableWidth = idleListRect.right - idleListRect.left;
    return idleListRect.left + std::max(0.0f, (availableWidth - kIdleQuickGridWidthDip) * 0.5f);
}

float IdlePageLayout::idleQuickListHeight(const IdlePresentation& content, float progress) const {
    const float expandedHeight =
        std::min(kIdleListExpandedHeightDip,
                 std::max(kIdleQuickCellSizeDip, idleQuickListContentHeight(content)));
    return kIdleListHeightDip +
           (expandedHeight - kIdleListHeightDip) * std::clamp(progress, 0.0f, 1.0f);
}

float IdlePageLayout::idleQuickListTop(float headerTop) const {
    const float headerBottom = headerTop + kIdleQuickHeaderHeightDip;
    const float expandButtonTop = idleQuickExpandButtonTop(headerTop);
    return std::max(headerBottom + kIdleListGapDip, expandButtonTop + 32.0f + kIdleListGapDip);
}

float IdlePageLayout::idleQuickTabContentOffset(float progress,
                                                const IdlePresentation& content) const {
    if (!content.todayTasksEnabled)
        return 0.0f;
    return (kIdleQuickTabHeightDip + kIdleQuickTabContentGapDip) * std::clamp(progress, 0.0f, 1.0f);
}

float IdlePageLayout::idleQuickTabTop(float contentTop, float progress,
                                      const IdlePresentation& content) const {
    return contentTop - idleQuickTabContentOffset(progress, content);
}

D2D1_RECT_F IdlePageLayout::idleQuickExpandLocalRect(float w, float headerTop,
                                                     const IdlePresentation& content) const {
    // 收起时按钮仍贴右对齐；随着内容向上展开，再同步向左让出返回箭头
    // 的槽位。页面转场期间沿用当前展开进度，避免旧页快照横向跳位。
    const bool reservePageArrowSlot = reserveArrow_;
    const float reserveProgress = reservePageArrowSlot ? idleQuickExpandProgress(content) : 0.0f;
    const float rightPadding =
        kIdleQuickExpandButtonRightPaddingDip + kIdleQuickPageArrowReserveDip * reserveProgress;
    const float left = std::max(kIdleQuickTitleLeftDip, w - rightPadding - 32.0f);
    const float top = idleQuickExpandButtonTop(headerTop);
    return D2D1::RectF(left, top, left + 32.0f, top + 32.0f);
}

D2D1_RECT_F IdlePageLayout::idleQuickTriggerLocalRect(float w, float headerTop,
                                                      const IdlePresentation& content) const {
    const D2D1_RECT_F button = idleQuickExpandLocalRect(w, headerTop, content);
    return D2D1::RectF(kIdleQuickTitleLeftDip - kIdleQuickTriggerHorizontalPaddingDip,
                       button.top - kIdleQuickTriggerVerticalPaddingDip,
                       button.right + kIdleQuickTriggerHorizontalPaddingDip,
                       button.bottom + kIdleQuickTriggerVerticalPaddingDip);
}

IdleLayout IdlePageLayout::idleLayout(const IdlePresentation& content, float quoteHeight) const {
    IdleLayout layout;
    layout.quoteTop = kIdleQuoteTopDip;
    layout.quoteHeight = quoteHeight;
    layout.sourceTop = layout.quoteTop + quoteHeight + kIdleQuoteSourceGapDip;
    layout.collapsedHeaderTop =
        content.showText ? layout.sourceTop + kIdleQuickHeaderGapDip : kIdleQuickExpandedTopDip;
    layout.headerTop = idleQuickHeaderTop(layout.collapsedHeaderTop, content);
    layout.expandButtonTop = idleQuickExpandButtonTop(layout.headerTop);
    const float progress = idleQuickExpandProgress(content);
    layout.tabTop = idleQuickListTop(layout.headerTop);
    layout.listTop = layout.tabTop + idleQuickTabContentOffset(progress, content);
    return layout;
}

IdleListLayout IdlePageLayout::list(float w, float top, const IdlePresentation& listContent,
                                    float offset) const {
    // 收起时只展示第一排；展开时尽量扩大网格可视区，内容超出后在
    // 列表内部滚动，始终不让内容越过卡片底部。
    IdleListLayout result;
    result.offset = offset;
    const float expandProgress = idleQuickExpandProgress(listContent);
    const float maxListHeight = idleQuickListHeight(listContent, expandProgress);
    const float availableHeight =
        std::max(0.0f, kIdlePopupHeightDip - top - kIdleListBottomPaddingDip);
    const float listHeight = std::min(maxListHeight, availableHeight);
    // 始终为滚动条预留固定槽位，避免滚动条出现或消失时网格横向跳动。
    const float listRight = w - 16.0f - kIdleScrollBarHitWidthDip;
    result.rect = D2D1::RectF(20.0f, top, listRight, top + listHeight);
    const float contentHeight = idleQuickListContentHeight(listContent);
    // 当前可视区放不下完整内容时就允许滚动；展开按钮只扩大可视区，
    // 如果扩大后已经能容纳全部内容，滚动条会自动消失。
    result.maxScroll = std::max(0.0f, contentHeight - listHeight);
    if (result.maxScroll <= 0.0f)
        result.offset = 0.0f;
    if (result.offset > result.maxScroll)
        result.offset = result.maxScroll;

    result.track = {};
    result.thumb = {};
    if (result.maxScroll <= 0.0f)
        return result;

    const float trackTop = result.rect.top;
    const float trackBottom = result.rect.bottom;
    const float trackHeight = trackBottom - trackTop;
    const float thumbHeight =
        std::max(24.0f, trackHeight * listHeight / std::max(listHeight, contentHeight));
    const float usable = std::max(0.0f, trackHeight - thumbHeight);
    const float thumbTop = trackTop + result.offset / result.maxScroll * usable;
    const float trackLeft = w - 12.0f;
    result.track =
        D2D1::RectF(trackLeft, trackTop, trackLeft + kIdleScrollBarWidthDip, trackBottom);
    result.thumb = D2D1::RectF(trackLeft, thumbTop, trackLeft + kIdleScrollBarWidthDip,
                               thumbTop + thumbHeight);
    return result;
}

MediaPageLayout mediaPageLayout(float w, float origin) {
    MediaPageLayout result;
    result.buttons = {
        D2D1::RectF(w * 0.5f - 80.0f, origin + 164.0f, w * 0.5f - 44.0f, origin + 200.0f),
        D2D1::RectF(w * 0.5f - 18.0f, origin + 164.0f, w * 0.5f + 18.0f, origin + 200.0f),
        D2D1::RectF(w * 0.5f + 44.0f, origin + 164.0f, w * 0.5f + 80.0f, origin + 200.0f)};
    result.source = D2D1::RectF(kMediaSourceTextLeftDip, kMediaSourceRowTopDip,
                                kMediaSourceTextRightDip, kMediaSourceRowBottomDip);
    result.sourceHit = D2D1::RectF(8.0f, origin + 8.0f, 220.0f, origin + 40.0f);
    result.title = D2D1::RectF(kPopupTextLeftDip, kMediaTitleTopDip, w - kMediaTextRightReserveDip,
                               kMediaTitleBottomDip);
    result.artist = D2D1::RectF(kPopupTextLeftDip, kMediaArtistTopDip,
                                w - kMediaTextRightReserveDip, kMediaArtistBottomDip);
    result.cover = D2D1::RectF(20.0f, 44.0f, 20.0f + kCoverSizeDip, 44.0f + kCoverSizeDip);
    result.track = D2D1::RectF(20.0f, 148.5f, std::max(21.0f, w - 20.0f), 151.5f);
    result.timeLeft = D2D1::RectF(20.0f, 128.0f, 92.0f, 144.0f);
    result.timeRight = D2D1::RectF(result.track.right - 72.0f, 128.0f, result.track.right, 144.0f);
    return result;
}
} // namespace media_popup_detail
