#include "test_support.h"

#include "model/taskbar_appearance.h"
#include "settings/app_settings.h"
#include "settings/display_rules.h"
#include "settings/settings_store.h"
#include "ui/lyric_effects.h"
#include "ui/spectrum_effects.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("display_rules.constraints_and_lyric_conflict_resolution") {
    AppSettings settings;
    settings.spectrumOn_ = true;
    settings.taskbarViewMode_ = TaskbarViewMode::Embedded;

    const auto normalHorizontal =
        display_rules::resolveConstraints(settings, RenderMode::Normal, false);
    EXPECT_FALSE(normalHorizontal.vertical);
    EXPECT_FALSE(normalHorizontal.minimal);
    EXPECT_TRUE(normalHorizontal.spectrumAllowed);
    EXPECT_TRUE(display_rules::spectrumRequested(settings, RenderMode::Normal, false));

    const auto normalVertical =
        display_rules::resolveConstraints(settings, RenderMode::Normal, true);
    EXPECT_TRUE(normalVertical.vertical);
    EXPECT_FALSE(normalVertical.minimal);
    EXPECT_FALSE(normalVertical.spectrumAllowed);
    EXPECT_FALSE(display_rules::spectrumRequested(settings, RenderMode::Normal, true));

    // AppBar 模式固定水平，不受原任务栏竖向影响
    settings.taskbarViewMode_ = TaskbarViewMode::AppBar;
    const auto appBarOnVertical =
        display_rules::resolveConstraints(settings, RenderMode::Normal, true);
    EXPECT_FALSE(appBarOnVertical.vertical);
    EXPECT_TRUE(appBarOnVertical.spectrumAllowed);

    const auto minimalMode =
        display_rules::resolveConstraints(settings, RenderMode::Minimal, false);
    EXPECT_TRUE(minimalMode.minimal);
    EXPECT_FALSE(minimalMode.spectrumAllowed);
    EXPECT_FALSE(display_rules::spectrumRequested(settings, RenderMode::Minimal, false));

    // 星尘与逐字浮波互斥
    uint32_t effects = withLyricEffect(0u, LyricEffect::Stardust, true);
    EXPECT_NE(effects & lyricEffectBit(LyricEffect::Stardust), 0u);
    effects = withLyricEffect(effects, LyricEffect::WordWave, true);
    EXPECT_EQ(effects & lyricEffectBit(LyricEffect::Stardust), 0u);
    EXPECT_NE(effects & lyricEffectBit(LyricEffect::WordWave), 0u);
    effects = withLyricEffect(effects, LyricEffect::Stardust, true);
    EXPECT_NE(effects & lyricEffectBit(LyricEffect::Stardust), 0u);
    EXPECT_EQ(effects & lyricEffectBit(LyricEffect::WordWave), 0u);

    // 星尘与辅助歌词冲突按最后一次用户操作优先级裁决
    settings.secondaryLyricEnabled_ = true;
    settings.lyricEffects_ = lyricEffectBit(LyricEffect::Stardust) |
                             lyricEffectBit(LyricEffect::Ripple);
    EXPECT_TRUE(display_rules::resolveLyricConflict(settings, display_rules::LyricPriority::Secondary));
    EXPECT_TRUE(settings.secondaryLyricEnabled_);
    EXPECT_EQ(settings.lyricEffects_ & lyricEffectBit(LyricEffect::Stardust), 0u);
    EXPECT_NE(settings.lyricEffects_ & lyricEffectBit(LyricEffect::Ripple), 0u);

    settings.lyricEffects_ |= lyricEffectBit(LyricEffect::Stardust);
    EXPECT_TRUE(display_rules::resolveLyricConflict(settings, display_rules::LyricPriority::Effects));
    EXPECT_FALSE(settings.secondaryLyricEnabled_);
    EXPECT_NE(settings.lyricEffects_ & lyricEffectBit(LyricEffect::Stardust), 0u);
}

TEST_CASE("taskbar_appearance.temporary_restrictions_preserve_user_preferences") {
    AppSettings settings;
    settings.secondaryLyricEnabled_ = true;
    settings.preferRomanization_ = true;
    settings.doubleLineLyricsEnabled_ = true;
    settings.lyricAlignment_ = LyricAlignment::Right;
    settings.idleQuoteAlignment_ = LyricAlignment::Center;
    settings.songInfoVisible_ = true;
    settings.hoverControlStyle_ = HoverControlStyle::Popup;
    settings.idleQuoteBackground_ = IdleQuoteBackground::TwinklingStars;
    settings.idleQuoteBackgroundScope_ = IdleQuoteBackgroundScope::All;
    settings.albumCoverEffect_ = AlbumCoverEffect::Vinyl;
    settings.spectrumOn_ = true;
    settings.spectrumStyle_ = SpectrumStyle::DreamyWave;
    settings.spectrumCustomColor_ = true;
    settings.spectrumGradient_ = true;
    settings.spectrumBackground_ = true;
    settings.progressBackground_ = true;
    settings.taskbarBackground_ = static_cast<int>(TaskbarBackground::CoverBlur);

    AppearanceContext context;
    context.renderMode = RenderMode::Normal;
    context.vertical = false;
    const auto normal = resolveTaskbarAppearance(settings, context);
    EXPECT_FALSE(normal.secondaryLyricMode.translation);
    EXPECT_TRUE(normal.secondaryLyricMode.romanization);
    EXPECT_TRUE(normal.doubleLineLyrics);
    EXPECT_EQ(normal.lyricAlignment, LyricAlignment::Right);
    EXPECT_EQ(normal.idleQuoteAlignment, LyricAlignment::Center);
    EXPECT_TRUE(normal.songInfoVisible);
    EXPECT_EQ(normal.hoverControlStyle, HoverControlStyle::Popup);
    EXPECT_EQ(normal.albumCoverEffect, AlbumCoverEffect::Vinyl);
    EXPECT_EQ(normal.spectrumStyle, SpectrumStyle::DreamyWave);
    EXPECT_TRUE(normal.spectrumColor.customized);
    EXPECT_TRUE(normal.spectrumGradient);
    EXPECT_TRUE(normal.spectrumBackground);
    EXPECT_TRUE(normal.progressBackground);
    EXPECT_EQ(normal.background, TaskbarBackground::CoverBlur);

    // 切换到竖向任务栏：临时限制生效，但不修改 settings 本身
    context.vertical = true;
    const auto vertical = resolveTaskbarAppearance(settings, context);
    EXPECT_FALSE(vertical.secondaryLyricMode.translation);
    EXPECT_FALSE(vertical.secondaryLyricMode.romanization);
    EXPECT_FALSE(vertical.doubleLineLyrics);
    EXPECT_EQ(vertical.lyricAlignment, LyricAlignment::Left);
    EXPECT_EQ(vertical.idleQuoteAlignment, LyricAlignment::Left);
    EXPECT_FALSE(vertical.songInfoVisible);
    EXPECT_EQ(vertical.spectrumStyle, SpectrumStyle::Default);
    EXPECT_FALSE(vertical.spectrumColor.customized);
    EXPECT_FALSE(vertical.spectrumGradient);
    EXPECT_FALSE(vertical.spectrumBackground);

    // 切换到极简模式：控件退回内嵌、关闭背景与黑胶
    context.vertical = false;
    context.renderMode = RenderMode::Minimal;
    const auto minimal = resolveTaskbarAppearance(settings, context);
    EXPECT_EQ(minimal.hoverControlStyle, HoverControlStyle::Inline);
    EXPECT_EQ(minimal.idleQuoteBackground, IdleQuoteBackground::None);
    EXPECT_EQ(minimal.idleQuoteBackgroundScope, IdleQuoteBackgroundScope::None);
    EXPECT_EQ(minimal.albumCoverEffect, AlbumCoverEffect::Default);
    EXPECT_FALSE(minimal.spectrumBackground);
    EXPECT_FALSE(minimal.progressBackground);
    EXPECT_EQ(minimal.background, TaskbarBackground::None);

    // 限制解除后重新推导：完全恢复之前的有效外观，证明持久配置未被覆盖
    context.renderMode = RenderMode::Normal;
    context.vertical = false;
    const auto restored = resolveTaskbarAppearance(settings, context);
    EXPECT_TRUE(restored == normal);
}

TEST_CASE("app_settings.json_roundtrip_and_legacy_migrations") {
    AppSettings original;
    original.hasUserFont_ = true;
    original.fontFamily_ = L"Microsoft YaHei UI";
    original.fontSize_ = 15.5f;
    original.fontStyle_ = LyricFontStyle::BoldItalic;
    original.taskbarPosition_ = 1;
    original.taskbarViewMode_ = TaskbarViewMode::AppBar;
    original.appBarEdge_ = AppBarEdge::Bottom;
    original.spectrumOn_ = true;
    original.spectrumStyle_ = SpectrumStyle::Segmented;
    original.spectrumEffects_ =
        spectrumEffectBit(SpectrumEffect::Glow) | spectrumEffectBit(SpectrumEffect::Sparks);
    original.doubleLineLyricsEnabled_ = true;
    original.secondaryLyricEnabled_ = true;
    original.preferRomanization_ = true;
    original.lyricEffects_ =
        lyricEffectBit(LyricEffect::FollowLight) | lyricEffectBit(LyricEffect::WordWave);
    original.idleCustomWelcome_ = L"欢迎回来";
    original.logRetentionDays_ = 14;

    const nlohmann::json dumped = original.toJson();
    AppSettings loaded;
    bool migrateLegacyLog = false;
    loaded.readJson(dumped, false, migrateLegacyLog);

    EXPECT_TRUE(loaded.hasUserFont_);
    EXPECT_EQ(loaded.fontFamily_, std::wstring(L"Microsoft YaHei UI"));
    EXPECT_NEAR(loaded.fontSize_, 15.5, 1e-4);
    EXPECT_EQ(loaded.fontStyle_, LyricFontStyle::BoldItalic);
    EXPECT_EQ(loaded.taskbarPosition_, 1);
    EXPECT_EQ(loaded.taskbarViewMode_, TaskbarViewMode::AppBar);
    EXPECT_EQ(loaded.appBarEdge_, AppBarEdge::Bottom);
    EXPECT_TRUE(loaded.spectrumOn_);
    EXPECT_EQ(loaded.spectrumStyle_, SpectrumStyle::Segmented);
    EXPECT_EQ(loaded.spectrumEffects_, original.spectrumEffects_);
    EXPECT_TRUE(loaded.doubleLineLyricsEnabled_);
    EXPECT_TRUE(loaded.secondaryLyricEnabled_);
    EXPECT_TRUE(loaded.preferRomanization_);
    EXPECT_EQ(loaded.lyricEffects_, original.lyricEffects_);
    EXPECT_EQ(loaded.idleCustomWelcome_, std::wstring(L"欢迎回来"));
    EXPECT_EQ(loaded.logRetentionDays_, 14);

    // 验证旧版字段迁移、越界钳制与欢迎语规则
    nlohmann::json legacy = {
        {"taskbarImmersive", true},
        {"spectrumStyle", "background-wave"},
        {"translationEnabled", false},
        {"romanizationEnabled", true},
        {"songToastDuration", 99},
        {"logRetentionDays", -10},
        {"idleCustomWelcome", "   \t  "},
        {"idleApps", nlohmann::json::array({"C:\\app1.exe", {{"path", "C:\\app2.exe"}, {"customName", "App2"}}})},
    };
    AppSettings migrated;
    migrated.readJson(legacy, true, migrateLegacyLog);
    EXPECT_EQ(migrated.taskbarViewMode_, TaskbarViewMode::Immersive);
    EXPECT_EQ(migrated.spectrumStyle_, SpectrumStyle::DreamyWave);
    EXPECT_TRUE(migrated.spectrumBackground_);
    EXPECT_TRUE(migrated.secondaryLyricEnabled_);
    EXPECT_TRUE(migrated.preferRomanization_);
    EXPECT_EQ(migrated.songToastDurationSec_, 10);
    EXPECT_EQ(migrated.logRetentionDays_, 0);
    EXPECT_TRUE(migrated.idleCustomWelcome_.empty());
    EXPECT_TRUE(migrated.tickTickEnabled_);
    EXPECT_EQ(migrated.idleApps_.size(), 2u);
    EXPECT_EQ(migrated.idleApps_[0].path, std::wstring(L"C:\\app1.exe"));
    EXPECT_EQ(migrated.idleApps_[1].customName, std::wstring(L"App2"));
}

TEST_CASE("settings_store.atomic_save_and_error_statuses") {
    const auto tempDir =
        std::filesystem::temp_directory_path() / "qqmusic_lyric_logic_tests_store";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    const auto filePath = tempDir / "settings.json";

    const auto missing = settings_store::load(filePath);
    EXPECT_EQ(missing.status, settings_store::LoadStatus::Missing);

    nlohmann::json doc = {{"spectrum", true}, {"fontSize", 16}};
    settings_store::save(filePath, doc);
    const auto loaded = settings_store::load(filePath);
    EXPECT_EQ(loaded.status, settings_store::LoadStatus::Loaded);
    EXPECT_TRUE(loaded.document.value("spectrum", false));
    EXPECT_EQ(loaded.document.value("fontSize", 0), 16);

    {
        std::ofstream corrupt(filePath, std::ios::binary | std::ios::trunc);
        corrupt << "{not valid json";
    }
    const auto parseErr = settings_store::load(filePath);
    EXPECT_EQ(parseErr.status, settings_store::LoadStatus::ParseError);

    std::filesystem::remove_all(tempDir, ec);
}
