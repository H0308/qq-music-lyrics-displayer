#pragma once
#include "model/display_types.h"
#include <cstdint>

// 每次提交包含完整的低频外观；播放进度、频谱采样与窗口生命周期独立更新。
struct TaskbarAppearance {
    struct SecondaryLyricMode {
        bool translation{};
        bool romanization{};
        bool operator==(const SecondaryLyricMode&) const = default;
    } secondaryLyricMode;
    bool doubleLineLyrics{};
    uint32_t lyricEffects{};
    LyricAlignment lyricAlignment{};
    LyricAlignment idleQuoteAlignment{};
    bool songInfoVisible{};
    bool controlsOnHover{};
    HoverControlStyle hoverControlStyle{};
    MediaPopupTrigger floatingCardTrigger{};
    MediaPopupBackground floatingCardBackground{};
    struct FloatingCardBackgroundColor {
        COLORREF color{};
        bool customized{};
        bool operator==(const FloatingCardBackgroundColor&) const = default;
    } floatingCardBackgroundColor;
    IdleQuoteBackground idleQuoteBackground{};
    IdleQuoteBackgroundScope idleQuoteBackgroundScope{};
    bool floatingCardFollowAlbum{};
    bool floatingCardAutoTextContrast{};
    AlbumCoverEffect albumCoverEffect{};
    SpectrumStyle spectrumStyle{};
    struct SpectrumColor {
        COLORREF color{};
        bool customized{};
        bool followAlbum{};
        bool operator==(const SpectrumColor&) const = default;
    } spectrumColor;
    struct SpectrumAlbumColor {
        COLORREF color{};
        bool available{};
        bool operator==(const SpectrumAlbumColor&) const = default;
    } spectrumAlbumColor;
    bool spectrumGradient{};
    uint32_t spectrumEffects{};
    bool spectrumBackground{};
    int spectrumOpacity{};
    bool progressBackground{};
    int progressBackgroundOpacity{};
    TaskbarBackground background{};
    int coverBackgroundOpacity{};
    int immersiveBackgroundAdjustment{};
    int dockBackgroundAdjustment{};
    int immersiveBackgroundBlur{};
    int dockBackgroundBlur{};
    DockResourceVisibility dockResourceVisibility{};
    bool albumCoverVisible{};
    bool platformIconVisible{};
    bool contextMenuEnabled{};
    struct FontColors {
        COLORREF played{};
        COLORREF unplayed{};
        int unplayedAlphaPct{};
        bool operator==(const FontColors&) const = default;
    } fontColors;
    bool fontGlow{};
    bool fontOutline{};
    struct FontGlowColors {
        COLORREF glow{};
        COLORREF outline{};
        bool operator==(const FontGlowColors&) const = default;
    } fontGlowColors;
    bool operator==(const TaskbarAppearance&) const = default;
};

struct AppearanceContext {
    RenderMode renderMode = RenderMode::Normal;
    bool vertical = false;
    LyricAppearance lyricAppearance;
    COLORREF playedColor = RGB(49, 194, 124);
    COLORREF albumColor = RGB(49, 194, 124);
    bool albumColorAvailable = false;
    COLORREF floatingCardBackground = RGB(255, 255, 255);
};
struct AppSettings;
TaskbarAppearance resolveTaskbarAppearance(const AppSettings& settings,
                                           const AppearanceContext& context);
