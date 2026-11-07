#pragma once
#include <cstddef>
namespace media_popup_detail {
inline constexpr float kPopupWidthDip = 384.0f;
inline constexpr float kPopupHeightDip = 208.0f;
// 每日一言 + 快速启动组合面板与播放中的媒体控件卡片保持同一外层高度。
inline constexpr float kIdlePopupHeightDip = kPopupHeightDip;
inline constexpr float kPopupGapDip = 8.0f;
inline constexpr float kPopupCornerDip = 10.0f;
inline constexpr float kCoverSizeDip = 80.0f;
inline constexpr float kPopupTextLeftDip = 116.0f;
inline constexpr float kPopupTextRightPaddingDip = 20.0f;
inline constexpr float kPopupTextEdgeFadeDip = 8.0f;
inline constexpr float kPopupTextPaddingDip = 8.0f;
inline constexpr float kPopupInfoScrollSpeed = 10.0f;
inline constexpr std::size_t kIdleQuickColumnCount = 5;
inline constexpr float kIdleQuickCellSizeDip = 62.0f;
inline constexpr float kIdleQuickCellGapDip = 6.0f;
inline constexpr float kIdleQuickRowPitchDip = kIdleQuickCellSizeDip + kIdleQuickCellGapDip;
inline constexpr float kIdleQuickGridWidthDip =
    kIdleQuickCellSizeDip * static_cast<float>(kIdleQuickColumnCount) +
    kIdleQuickCellGapDip * static_cast<float>(kIdleQuickColumnCount - 1);
inline constexpr float kIdleQuickIconSizeDip = 24.0f;
inline constexpr float kIdleQuickNamedIconTopDip = 7.0f;
inline constexpr float kIdleQuickNamedTextTopInsetDip = 23.0f;
inline constexpr float kIdleQuickNamedTextBottomInsetDip = 4.0f;
inline constexpr float kIdleListHeightDip = kIdleQuickCellSizeDip;
inline constexpr float kIdleListGapDip = 8.0f;
inline constexpr float kIdleListBottomPaddingDip = 8.0f;
// 展开态把卡片底部的可用空间尽量交给列表；实际高度仍由列表顶部和卡片
// 底部内边距共同限制，内容超过可视区时才启用滚动。
inline constexpr float kIdleListExpandedHeightDip = kIdlePopupHeightDip - kIdleListBottomPaddingDip;
inline constexpr float kIdleQuickVisualClipPaddingDip = 1.0f;
inline constexpr float kIdleQuickExpandedTopDip = 15.0f;
inline constexpr float kIdleQuickTitleLeftDip = 20.0f;
inline constexpr float kIdleQuickTriggerHorizontalPaddingDip = 8.0f;
inline constexpr float kIdleQuickTriggerVerticalPaddingDip = 4.0f;
inline constexpr float kIdleQuickExpandButtonRightPaddingDip = 16.0f;
inline constexpr float kIdleQuickPageArrowReserveDip = 44.0f;
inline constexpr float kIdleQuickTabWidthDip = 104.0f;
inline constexpr float kIdleQuickTabGapDip = 2.0f;
inline constexpr float kIdleQuickTabHeightDip = 26.0f;
inline constexpr float kIdleQuickTabContentGapDip = 8.0f;
inline constexpr float kIdleTaskRowHeightDip = 28.0f;
inline constexpr float kIdleTaskRowGapDip = 6.0f;
inline constexpr float kIdleScrollBarWidthDip = 3.0f;
inline constexpr float kIdleScrollBarHitWidthDip = 12.0f;
// 两个页面的内容布局基准：活页绘制、转场快照与合成层转场共用同一套纵向
// 坐标，避免每条路径各算一份导致几何漂移。
inline constexpr float kIdleHeaderTopDip = 14.0f;
inline constexpr float kIdleHeaderBottomDip = 36.0f;
inline constexpr float kIdleQuoteTopDip = 40.0f;
inline constexpr float kIdleQuoteSourceGapDip = 8.0f;
inline constexpr float kIdleQuickHeaderGapDip = 30.0f;
inline constexpr float kIdleQuickHeaderHeightDip = 16.0f;
inline constexpr float kMediaSourceRowTopDip = 12.0f;
inline constexpr float kMediaSourceRowBottomDip = 34.0f;
inline constexpr float kMediaSourceTextLeftDip = 44.0f;
inline constexpr float kMediaSourceTextRightDip = 220.0f;
inline constexpr float kMediaTitleTopDip = 52.0f;
inline constexpr float kMediaTitleBottomDip = 80.0f;
inline constexpr float kMediaArtistTopDip = 84.0f;
inline constexpr float kMediaArtistBottomDip = 106.0f;
// 标题/歌手右侧为伴听宠物预留的固定座位。座位宽度不足以漫游，
// 宠物只在原地表演（听歌摇摆、唱歌、跳舞、哈欠与瞌睡）。
inline constexpr float kMediaPetZoneWidthDip = 48.0f;
inline constexpr float kMediaPetLaneTopDip = 60.0f;
inline constexpr float kMediaPetLaneHeightDip = 48.0f;
inline constexpr float kMediaTextRightReserveDip =
    kPopupTextRightPaddingDip + kMediaPetZoneWidthDip + 8.0f;

enum class IdleQuickTab { QuickStart, TodayTasks };
} // namespace media_popup_detail
