#include "model/taskbar_appearance.h"
#include "settings/app_settings.h"

TaskbarAppearance resolveTaskbarAppearance(const AppSettings& settings,
                                           const AppearanceContext& context) {
    const bool vertical = context.vertical && settings.taskbarViewMode_ != TaskbarViewMode::AppBar;
    const bool minimal = context.renderMode == RenderMode::Minimal;
    TaskbarAppearance result;
    result.secondaryLyricMode = {
        vertical ? false : (settings.secondaryLyricEnabled_ && !settings.preferRomanization_),
        vertical ? false : (settings.secondaryLyricEnabled_ && settings.preferRomanization_)};
    result.doubleLineLyrics = vertical ? false : settings.doubleLineLyricsEnabled_;
    result.lyricEffects = settings.lyricEffects_;
    result.lyricAlignment = vertical ? LyricAlignment::Left : settings.lyricAlignment_;
    result.idleQuoteAlignment = vertical ? LyricAlignment::Left : settings.idleQuoteAlignment_;
    result.songInfoVisible = vertical ? false : settings.songInfoVisible_;
    result.controlsOnHover = settings.hoverPlaybackControls_;
    result.hoverControlStyle = minimal ? HoverControlStyle::Inline : settings.hoverControlStyle_;
    result.floatingCardTrigger = settings.floatingCardTrigger_;
    result.floatingCardBackground = settings.floatingCardBackground_;
    result.floatingCardBackgroundColor = {context.floatingCardBackground,
                                          settings.floatingCardBackgroundColorCustomized_};
    result.idleQuoteBackground =
        minimal ? IdleQuoteBackground::None : settings.idleQuoteBackground_;
    result.idleQuoteBackgroundScope =
        minimal ? IdleQuoteBackgroundScope::None : settings.idleQuoteBackgroundScope_;
    result.floatingCardFollowAlbum = settings.floatingCardFollowAlbum_;
    result.floatingCardAutoTextContrast = settings.floatingCardAutoTextContrast_;
    result.albumCoverEffect = minimal ? AlbumCoverEffect::Default : settings.albumCoverEffect_;
    result.spectrumStyle = vertical ? SpectrumStyle::Default : settings.spectrumStyle_;
    result.spectrumColor = {settings.spectrumColor_, !vertical && settings.spectrumCustomColor_,
                            !vertical && settings.spectrumFollowAlbum_};
    result.spectrumAlbumColor = {context.albumColor, context.albumColorAvailable};
    result.spectrumGradient = !vertical && settings.spectrumGradient_;
    result.spectrumEffects = settings.spectrumEffects_;
    result.spectrumBackground = minimal || vertical ? false : settings.spectrumBackground_;
    result.spectrumOpacity = settings.spectrumOpacity_;
    result.progressBackground = minimal ? false : settings.progressBackground_;
    result.progressBackgroundOpacity = settings.progressBackgroundOpacity_;
    result.background = minimal ? TaskbarBackground::None
                                : static_cast<TaskbarBackground>(settings.taskbarBackground_);
    result.coverBackgroundOpacity = settings.coverBackgroundOpacity_;
    result.immersiveBackgroundAdjustment = settings.immersiveBackgroundAdjustment_;
    result.dockBackgroundAdjustment = settings.dockBackgroundAdjustment_;
    result.immersiveBackgroundBlur = settings.immersiveBackgroundBlur_;
    result.dockBackgroundBlur = settings.dockBackgroundBlur_;
    result.dockResourceVisibility =
        DockResourceVisibility{settings.embeddedResourceMonitor_, settings.dockResourceDownload_,
                               settings.dockResourceUpload_, settings.dockResourceGpuUsage_,
                               settings.dockResourceCpuFrequency_};
    result.albumCoverVisible = settings.albumCoverVisible_;
    result.platformIconVisible = settings.platformIconVisible_;
    result.contextMenuEnabled = settings.taskbarContextMenuEnabled_;
    result.fontColors = {context.playedColor, context.lyricAppearance.unplayed,
                         context.lyricAppearance.unplayedAlphaPct};
    result.fontGlow = context.lyricAppearance.glowOn;
    result.fontOutline = context.lyricAppearance.outlineOn;
    result.fontGlowColors = {context.lyricAppearance.glowColor,
                             context.lyricAppearance.outlineColor};
    return result;
}
