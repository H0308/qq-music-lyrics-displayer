#include "test_support.h"

#include "ui/media_popup/page_layout.h"
#include "ui/settings/search_model.h"
#include "ui/settings/sections.h"
#include "ui/taskbar/layout.h"
#include "ui/taskbar/placement.h"

#include <shellapi.h>

using media_popup_detail::IdlePageLayout;
using media_popup_detail::IdleQuickTab;
using media_popup_detail::mediaPageLayout;
using settings_ui::SettingsSections;
using taskbar_detail::OccupiedRegions;
using taskbar_detail::PlacementInput;
using taskbar_detail::WindowPlacement;
using taskbar_detail::calculatePlacement;
using taskbar_detail::dynamicBackgroundRegions;
using taskbar_detail::expandedSideLayout;
using taskbar_detail::horizontalLayout;
using taskbar_detail::occupiedIntervals;
using taskbar_detail::verticalLayout;

TEST_CASE("taskbar_placement.safe_compressed_relocated_nospace_and_forced_overlap") {
    const RECT buttons[] = {{120, 0, 260, 48}, {240, 0, 320, 48}};
    OccupiedRegions regions{};
    regions.vertical = false;
    regions.hasStart = true;
    regions.startRect = {0, 0, 120, 48};
    regions.hasNotify = true;
    regions.notifyRect = {1700, 0, 1920, 48};
    regions.buttons = buttons;
    const auto merged = occupiedIntervals(regions);
    EXPECT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0].first, 0);
    EXPECT_EQ(merged[0].second, 320);
    EXPECT_EQ(merged[1].first, 1700);
    EXPECT_EQ(merged[1].second, 1920);

    PlacementInput input{};
    input.taskbar = {0, 1032, 1920, 1080};
    input.edge = ABE_BOTTOM;
    input.vertical = false;
    input.positionMode = 0;
    input.gap = 6;
    input.minMajor = 300;
    input.compressedMinMajor = 180;
    input.maxMajor = 520;
    input.crossPixels = 48;
    input.occupied = merged;

    WindowPlacement placement{};
    EXPECT_EQ(calculatePlacement(input, placement), TaskbarPlacementStatus::Safe);
    EXPECT_EQ(placement.width, 520);
    EXPECT_EQ(placement.x, 1700 - 6 - 520);

    // 挤压首选区间至 [1460, 1700]，可用 228 < minMajor(300) 但 >= compressedMinMajor(180)
    input.occupied = {{0, 320}, {320, 1460}, {1700, 1920}};
    EXPECT_EQ(calculatePlacement(input, placement), TaskbarPlacementStatus::Compressed);
    EXPECT_EQ(placement.width, 228);

    // 首选右端区间 [1600, 1700] 放不下，换到中间最大空闲区 [400, 900]
    input.occupied = {{0, 400}, {900, 1600}, {1700, 1920}};
    EXPECT_EQ(calculatePlacement(input, placement), TaskbarPlacementStatus::Relocated);
    EXPECT_EQ(placement.width, 488);

    // 全部区间都小于 compressedMinMajor：默认返回 NoSpace；允许重叠时返回 ForcedOverlap 且保持压缩最小宽度
    input.occupied = {{0, 1800}, {1850, 1920}};
    input.allowOverlap = false;
    EXPECT_EQ(calculatePlacement(input, placement), TaskbarPlacementStatus::NoSpace);

    input.allowOverlap = true;
    EXPECT_EQ(calculatePlacement(input, placement), TaskbarPlacementStatus::ForcedOverlap);
    EXPECT_EQ(placement.width, input.compressedMinMajor);
    EXPECT_TRUE(placement.x >= input.taskbar.left);
    EXPECT_TRUE(placement.x + placement.width <= input.taskbar.right);
}

TEST_CASE("taskbar_layout.horizontal_vertical_and_dynamic_background_merging") {
    const D2D1_RECT_F playbackRects[] = {
        {0.0f, 0.0f, 120.0f, 40.0f},
        {110.0f, 0.0f, 260.0f, 40.0f},
        {260.0f, 0.0f, 320.0f, 40.0f},
    };
    const auto merged = dynamicBackgroundRegions(
        300.0f, 40.0f, IdleQuoteBackgroundScope::Lyrics, playbackRects, {});
    EXPECT_EQ(merged.count, 1u);
    EXPECT_NEAR(merged.rects[0].left, 0.0, 1e-4);
    EXPECT_NEAR(merged.rects[0].right, 300.0, 1e-4);

    // 竖向布局在空间过矮时自动隐藏内嵌播放按钮，保留歌词区
    const auto tallVertical = verticalLayout(48.0f, 320.0f, DisplayScene::Lyrics, true, true);
    EXPECT_TRUE(tallVertical.showControls);
    const auto crampedVertical = verticalLayout(48.0f, 90.0f, DisplayScene::Lyrics, true, true);
    EXPECT_FALSE(crampedVertical.showControls);

    const auto sideDock = expandedSideLayout(1920.0f, true, 160.0f);
    EXPECT_NEAR(sideDock.clockWidth, 0.0, 1e-4);
    EXPECT_NEAR(sideDock.resourceWidth, 160.0, 1e-4);
    EXPECT_TRUE(sideDock.spectrumWidth > 0.0f);
}

TEST_CASE("media_popup_layout.idle_and_media_geometry_and_scroll_bounds") {
    IdlePresentation content;
    content.showText = true;
    content.quickStartEnabled = true;
    content.todayTasksEnabled = true;
    content.todayTasks = {
        {L"1", L"p", L"task1", L"", false, false, IdleTaskPriority::High},
        {L"2", L"p", L"task2", L"", false, false, IdleTaskPriority::Medium},
        {L"3", L"p", L"task3", L"", false, false, IdleTaskPriority::Low},
        {L"4", L"p", L"task4", L"", false, false, IdleTaskPriority::None},
        {L"5", L"p", L"task5", L"", false, false, IdleTaskPriority::None},
        {L"6", L"p", L"task6", L"", false, false, IdleTaskPriority::None},
    };

    IdlePageLayout expanded(1.0f, IdleQuickTab::TodayTasks, true);
    const auto layout = expanded.idleLayout(content, 36.0f);
    const auto listLayout = expanded.list(320.0f, layout.listTop, content, 9999.0f);
    EXPECT_TRUE(listLayout.maxScroll > 0.0f);
    EXPECT_NEAR(listLayout.offset, listLayout.maxScroll, 1e-4); // 越界滚动自动钳制到 maxScroll
    EXPECT_TRUE(listLayout.thumb.bottom <= listLayout.track.bottom + 0.01f);

    const auto media = mediaPageLayout(320.0f, 0.0f);
    EXPECT_TRUE(media.buttons[0].left < media.buttons[1].left);
    EXPECT_TRUE(media.buttons[1].left < media.buttons[2].left);
    EXPECT_TRUE(media.track.right > media.track.left);
}

TEST_CASE("settings_sections.search_and_interdependent_row_states") {
    SettingsSearchModel search;
    const std::vector<SettingsSearchEntry> entries{
        {5, settings_ui::kIdSpectrum, L"频谱 > 启用", L"频谱 梦幻波浪"},
        {6, settings_ui::kIdDoubleLine, L"歌词 > 排版", L"双行歌词 预览"},
    };
    search.refresh(L"  梦幻   频谱 ", entries);
    EXPECT_TRUE(search.active());
    EXPECT_EQ(search.results().size(), 1u);
    EXPECT_EQ(search.results()[0].id, settings_ui::kIdSpectrum);
    search.clear();
    EXPECT_FALSE(search.active());

    SettingsSections sections;
    sections.state.spectrumOn = true;
    sections.state.spectrumStyle = 2; // 梦幻波浪
    sections.state.spectrumBackground = true;
    sections.state.progressBackground = true;
    sections.state.doubleLineLyrics = true;
    sections.state.secondaryEnabled = true;
    sections.state.compactTaskbar = true;
    sections.create();

    // 背景波浪激活时，播放进度背景被禁用
    const auto* progressBg = sections.findRow(settings_ui::kIdProgressBackground);
    EXPECT_TRUE(progressBg != nullptr);
    EXPECT_FALSE(progressBg->enabled);

    // 小任务栏下临时停用双行歌词与辅助歌词开关，但保留其勾选偏好
    const auto* doubleLine = sections.findRow(settings_ui::kIdDoubleLine);
    const auto* secondaryOn = sections.findRow(settings_ui::kIdSecondaryOn);
    EXPECT_TRUE(doubleLine != nullptr && secondaryOn != nullptr);
    EXPECT_TRUE(doubleLine->checked);
    EXPECT_FALSE(doubleLine->enabled);
    EXPECT_TRUE(secondaryOn->checked);
    EXPECT_FALSE(secondaryOn->enabled);

    // 未配置滴答清单 API 口令时点击开启开关，保持未勾选状态并将请求转发给回调
    bool tickTickRequested = false;
    sections.actions.onTickTickEnabled = [&](bool enabled) { tickTickRequested = enabled; };
    sections.onCommand(settings_ui::kIdTickTickEnabled);
    EXPECT_TRUE(tickTickRequested);
    EXPECT_FALSE(sections.findRow(settings_ui::kIdTickTickEnabled)->checked);
}
