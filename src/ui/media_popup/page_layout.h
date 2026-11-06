#pragma once
#include "ui/lyric_window.h"
#include "ui/media_popup/metrics.h"
#include <d2d1.h>
#include <array>

namespace media_popup_detail {
struct IdleLayout {
    float quoteTop = 0.0f; // 一言正文顶
    float quoteHeight = 0.0f;
    float sourceTop = 0.0f;          // 来源行顶
    float collapsedHeaderTop = 0.0f; // 快速打开标题行顶（收起态）
    float headerTop = 0.0f;          // 按当前展开进度插值后的标题行顶
    float expandButtonTop = 0.0f;
    float tabTop = 0.0f;  // 快捷启动/今日任务 Tab 顶
    float listTop = 0.0f; // 当前 Tab 内容可视区顶
};

struct IdleListLayout {
    D2D1_RECT_F rect{}, track{}, thumb{};
    float offset{}, maxScroll{};
};
class IdlePageLayout {
public:
    IdlePageLayout(float progress, IdleQuickTab tab, bool reserveArrow)
        : progress_(progress), tab_(tab), reserveArrow_(reserveArrow) {}
    float idleQuickExpandProgress(const IdlePresentation& content) const;
    float idleQuickHeaderTop(float collapsedTop, const IdlePresentation& content) const;
    float idleQuickExpandButtonTop(float headerTop) const;
    std::size_t idleQuickRowCount(const IdlePresentation& content) const;
    float idleQuickGridContentHeight(const IdlePresentation& content) const;
    float idleQuickTaskContentHeight(const IdlePresentation& content) const;
    float idleQuickListContentHeight(const IdlePresentation& content) const;
    float idleQuickGridLeft(const D2D1_RECT_F& idleListRect) const;
    float idleQuickListHeight(const IdlePresentation& content, float progress) const;
    float idleQuickListTop(float headerTop) const;
    float idleQuickTabContentOffset(float progress, const IdlePresentation& content) const;
    float idleQuickTabTop(float contentTop, float progress, const IdlePresentation& content) const;
    D2D1_RECT_F idleQuickExpandLocalRect(float w, float headerTop,
                                         const IdlePresentation& content) const;
    D2D1_RECT_F idleQuickTriggerLocalRect(float w, float headerTop,
                                          const IdlePresentation& content) const;
    IdleLayout idleLayout(const IdlePresentation& content, float quoteHeight) const;

    IdleListLayout list(float width, float top, const IdlePresentation& content,
                        float offset) const;

private:
    float progress_;
    IdleQuickTab tab_;
    bool reserveArrow_;
};
struct MediaPageLayout {
    std::array<D2D1_RECT_F, 3> buttons;
    D2D1_RECT_F source, sourceHit, title, artist, cover, track, timeLeft, timeRight;
};
MediaPageLayout mediaPageLayout(float width, float origin);
} // namespace media_popup_detail
