#include "settings/display_rules.h"
#include "settings/app_settings.h"
#include "ui/lyric_effects.h"

namespace display_rules {
DisplayConstraints resolveConstraints(const AppSettings& settings, RenderMode mode, bool vertical) {
    vertical = vertical && settings.taskbarViewMode_ != TaskbarViewMode::AppBar;
    return {vertical, mode == RenderMode::Minimal, !vertical && mode == RenderMode::Normal};
}

bool spectrumRequested(const AppSettings& settings, RenderMode mode, bool vertical) {
    return settings.spectrumOn_ && resolveConstraints(settings, mode, vertical).spectrumAllowed;
}

bool resolveLyricConflict(AppSettings& settings, LyricPriority priority) {
    const uint32_t star = lyricEffectBit(LyricEffect::Stardust);
    if (!settings.secondaryLyricEnabled_ || (settings.lyricEffects_ & star) == 0)
        return false;
    if (priority == LyricPriority::Secondary)
        settings.lyricEffects_ &= ~star;
    else
        settings.secondaryLyricEnabled_ = false;
    return true;
}

} // namespace display_rules
