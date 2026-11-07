#include "test_support.h"

#include "ui/taskbar/interaction.h"
#include "ui/taskbar/karaoke_tracker.h"
#include "ui/taskbar/lyric_transition.h"
#include "ui/taskbar/render_state.h"
#include "ui/taskbar/scene_transition.h"
#include "ui/taskbar/text_scroll.h"
#include "ui/taskbar/vinyl_animation.h"

using taskbar_detail::InvalidationSnapshot;
using taskbar_detail::RenderInvalidation;
using taskbar_detail::RenderState;
using taskbar_detail::SceneTransition;
using taskbar_detail::WindowPlacement;

TEST_CASE("render_state.invalidation_snapshot_preserves_concurrent_dirty_bits") {
    RenderState state;
    EXPECT_TRUE(state.hasInvalidation());

    const InvalidationSnapshot initial = state.beginInvalidation();
    state.commitInvalidation(initial);
    EXPECT_FALSE(state.hasInvalidation());

    // 本帧开始时只有 Paint 和 Text 失效；绘制期间又新增了 Text 与 Layout 失效
    state.requestInvalidation(RenderInvalidation::Paint);
    state.requestInvalidation(RenderInvalidation::Text);
    const InvalidationSnapshot frameSnapshot = state.beginInvalidation();

    state.requestInvalidation(RenderInvalidation::Text);
    state.requestInvalidation(RenderInvalidation::Layout);
    state.commitInvalidation(frameSnapshot);

    EXPECT_FALSE(state.isInvalidated(RenderInvalidation::Paint));
    EXPECT_TRUE(state.isInvalidated(RenderInvalidation::Text));
    EXPECT_TRUE(state.isInvalidated(RenderInvalidation::Layout));
}

TEST_CASE("lyric_transition.queueing_reversal_and_seek_reset") {
    LyricTransition transition;

    // 相邻下一行发起转场
    EXPECT_EQ(transition.request(2, 3, 12000, 1, true), LyricTransition::Decision::Rebuild);
    EXPECT_EQ(transition.phase(), LyricTransition::Phase::Pending);
    EXPECT_EQ(transition.direction(), 1);
    EXPECT_TRUE(transition.target().has_value());
    EXPECT_EQ(transition.target()->lineIndex, 3);

    // 动画进入运行态后，同方向的下一行排队而不打断当前转场
    transition.setPhase(LyricTransition::Phase::Running);
    EXPECT_EQ(transition.request(3, 4, 15000, 2, true), LyricTransition::Decision::Queued);
    EXPECT_TRUE(transition.pending().has_value());
    EXPECT_EQ(transition.pending()->lineIndex, 4);

    // 再次同向换行只保留最新目标
    EXPECT_EQ(transition.request(4, 5, 18000, 3, true), LyricTransition::Decision::Queued);
    EXPECT_TRUE(transition.pending().has_value());
    EXPECT_EQ(transition.pending()->lineIndex, 5);

    // 消费排队目标后进入新一轮 Pending
    EXPECT_TRUE(transition.advancePending(4));
    EXPECT_EQ(transition.phase(), LyricTransition::Phase::Pending);
    EXPECT_FALSE(transition.pending().has_value());
    EXPECT_EQ(transition.target()->lineIndex, 5);

    // 运行中反向切换或跨多行 seek 立即 Reset
    transition.setPhase(LyricTransition::Phase::Running);
    EXPECT_EQ(transition.request(5, 4, 14000, 5, true), LyricTransition::Decision::Reset);
    EXPECT_EQ(transition.request(5, 8, 30000, 6, true), LyricTransition::Decision::Reset);
    EXPECT_EQ(transition.request(5, 6, 20000, 7, false), LyricTransition::Decision::Reset);
}

TEST_CASE("scene_transition.resize_timeline_and_deduplication") {
    SceneTransition scene;
    EXPECT_TRUE(SceneTransition::switchesPage(DisplayScene::Idle, DisplayScene::Lyrics));
    EXPECT_TRUE(SceneTransition::switchesPage(DisplayScene::Spectrum, DisplayScene::Idle));
    EXPECT_FALSE(SceneTransition::switchesPage(DisplayScene::Lyrics, DisplayScene::Spectrum));

    const WindowPlacement from{100, 0, 220, 48, 220};
    const WindowPlacement to{60, 0, 340, 48, 340};

    // 相同几何不启动尺寸动画
    EXPECT_FALSE(scene.beginResize(from, from, 1000));
    EXPECT_FALSE(scene.resizing());

    EXPECT_TRUE(scene.beginResize(from, to, 1000));
    EXPECT_TRUE(scene.resizing());
    EXPECT_TRUE(scene.targets(to));

    // 布局就绪时刻同步动画起点
    scene.synchronizeResizeStart(1100);
    const auto mid = scene.advanceResize(1220); // 半程 120ms / 240ms
    EXPECT_TRUE(mid.has_value());
    EXPECT_EQ(mid->x, 80);
    EXPECT_EQ(mid->width, 280);
    EXPECT_TRUE(scene.resizing());

    const auto end = scene.advanceResize(1340);
    EXPECT_TRUE(end.has_value());
    EXPECT_EQ(end->x, to.x);
    EXPECT_EQ(end->width, to.width);
    EXPECT_FALSE(scene.resizing());
}

TEST_CASE("text_scroll.marquee_karaoke_follow_and_oneshot_completion") {
    TextScroll scroll;
    scroll.setOneShot(true, 1000);

    TextScrollInput input{};
    input.now = 1000;
    input.animations = true;
    input.idle = true;
    input.areaValid = true;
    input.area = 200.0f;
    input.lyricWidth = 120.0f; // 放得下：按固定时长等待 5000ms 完成

    scroll.advance(input);
    EXPECT_FALSE(scroll.consumeCompleted());

    input.now = 6000;
    scroll.advance(input);
    EXPECT_TRUE(scroll.consumeCompleted());
    EXPECT_FALSE(scroll.oneShot());

    // 逐字高亮跟随：将唱到的位置保持在可视区 30% 附近并钳制在 [0, lyricWidth - area]
    TextScroll karaokeScroll;
    TextScrollInput kInput{};
    kInput.now = 1000;
    kInput.animations = true;
    kInput.playing = true;
    kInput.karaoke = true;
    kInput.areaValid = true;
    kInput.area = 100.0f;
    kInput.lyricWidth = 300.0f;
    kInput.karaokeProgress = 150.0f; // target = 150 - 30 = 120
    karaokeScroll.advance(kInput);
    for (ULONGLONG t = 1100; t <= 3000; t += 100) {
        kInput.now = t;
        karaokeScroll.advance(kInput);
    }
    EXPECT_NEAR(karaokeScroll.lyric(), 120.0, 1.0);
}

TEST_CASE("vinyl_animation_and_taskbar_interaction_states") {
    VinylAnimation vinyl;
    EXPECT_NEAR(vinyl.armAngle(), -18.0, 1e-4);

    // 开始播放：先落唱臂（240ms），唱臂落稳前圆盘不转
    vinyl.advance(1000, true, true);
    EXPECT_TRUE(vinyl.armMoving());
    EXPECT_NEAR(vinyl.angle(), 0.0, 1e-4);

    vinyl.advance(1120, true, true);
    EXPECT_TRUE(vinyl.armAngle() > -18.0f && vinyl.armAngle() < 0.0f);
    EXPECT_NEAR(vinyl.angle(), 0.0, 1e-4);

    vinyl.advance(1240, true, true);
    EXPECT_FALSE(vinyl.armMoving());
    EXPECT_NEAR(vinyl.armAngle(), 0.0, 1e-4);

    // 唱臂到位后的下一帧开始旋转
    vinyl.advance(1540, true, true);
    EXPECT_TRUE(vinyl.angle() > 0.0f);

    // 沉浸式避让显隐状态机
    TaskbarPresence presence;
    presence.beginHide(1000, 1.0f);
    EXPECT_EQ(presence.advance(1050, false), TaskbarPresence::Action::None);
    EXPECT_EQ(presence.advance(1100, false), TaskbarPresence::Action::Hide);
    presence.hide();
    EXPECT_EQ(presence.advance(1200, true), TaskbarPresence::Action::None);
    EXPECT_EQ(presence.advance(1380, true), TaskbarPresence::Action::Show);

    // 拖拽手势阈值判定
    TaskbarDragGesture drag;
    drag.press(POINT{100, 100}, 0);
    EXPECT_FALSE(drag.move(POINT{103, 102}, 6, 6));
    EXPECT_TRUE(drag.move(POINT{108, 102}, 6, 6));
    drag.begin(240, 1);
    EXPECT_TRUE(drag.dragging());
    EXPECT_EQ(drag.candidate(), 1);
    drag.end();
    EXPECT_FALSE(drag.dragging());
}
