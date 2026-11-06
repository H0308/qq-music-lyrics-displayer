#include "ui/settings/sections.h"
#include "ui/settings/section_labels.h"
#include "ui/settings/section_metrics.h"
#include "ui/settings/lyric_section.h"
#include "ui/settings/spectrum_section.h"
#include <algorithm>

namespace settings_ui {
bool SettingsSections::onIdleCommand(int id, Row* row, int option) {
    switch (id) {
    case kIdIdleEntry:
        row->checked = !row->checked;
        state.idleEntryEnabled = row->checked;
        updateIdleRowsEnabled();
        updateFloatingCardRowsEnabled();
        if (actions.onIdleEntryEnabled)
            actions.onIdleEntryEnabled(row->checked);
        break;
    case kIdIdleQuote:
        state.idleQuoteEnabled = row->selected == 0;
        state.idleWelcomeEnabled = row->selected == 1;
        updateIdleRowsEnabled();
        if (actions.onIdleTextMode)
            actions.onIdleTextMode(row->selected);
        break;
    case kIdIdleCustomWelcome:
        if (actions.onEditIdleWelcome)
            actions.onEditIdleWelcome();
        break;
    case kIdTickTickEnabled: {
        const bool requested = !row->checked;
        state.tickTickEnabled = requested && state.tickTickApiTokenConfigured;
        row->checked = state.tickTickEnabled;
        updateTickTickRows();
        if (actions.onTickTickEnabled)
            actions.onTickTickEnabled(requested);
        break;
    }
    case kIdTickTickApiToken:
        if (actions.onEditTickTickApiToken)
            actions.onEditTickTickApiToken();
        break;
    case kIdTickTickConnect:
        if (actions.onTickTickConnect)
            actions.onTickTickConnect();
        break;
    case kIdTickTickRefresh:
        if (actions.onTickTickRefresh)
            actions.onTickTickRefresh();
        break;
    case kIdTickTickDisconnect:
        if (actions.onTickTickDisconnect)
            actions.onTickTickDisconnect();
        break;
    case kIdIdleQuoteSource:
        state.idleQuoteSource = row->selected;
        updateIdleQuoteSourceRow();
        if (actions.onIdleQuoteSource)
            actions.onIdleQuoteSource(row->selected);
        break;
    case kIdIdleQuoteRefreshInterval:
        state.idleQuoteRefreshInterval = row->selected;
        if (actions.onIdleQuoteRefreshInterval)
            actions.onIdleQuoteRefreshInterval(row->selected);
        break;
    case kIdIdleQuoteAlignment:
        state.idleQuoteAlignment = row->selected;
        if (actions.onIdleQuoteAlignment)
            actions.onIdleQuoteAlignment(row->selected);
        break;
    case kIdIdleQuoteBackground:
        state.idleQuoteBackground = row->selected;
        if (actions.onIdleQuoteBackground)
            actions.onIdleQuoteBackground(row->selected);
        updateIdleRowsEnabled();
        break;
    case kIdIdleQuoteBackgroundScope:
        state.idleQuoteBackgroundScope = row->selected;
        if (actions.onIdleQuoteBackgroundScope)
            actions.onIdleQuoteBackgroundScope(row->selected);
        break;
    case kIdIdleApps:
        if (isIdleAppDragOption(option)) {
            // 应用条目的普通点击只用于启动拖动，不触发其他操作。
        } else if (option == kIdleAppNamesOption) {
            row->checked = !row->checked;
            state.idleAppNamesVisible = row->checked;
            if (actions.onIdleAppNamesVisible)
                actions.onIdleAppNamesVisible(row->checked);
        } else if (option >= kIdleAppEditOptionBase) {
            const int index = option - kIdleAppEditOptionBase;
            if (index >= 0 && static_cast<size_t>(index) < state.idleApps.size() &&
                actions.onEditIdleApp)
                actions.onEditIdleApp(index);
        } else if (option >= 0) {
            if (actions.onRemoveIdleApp)
                actions.onRemoveIdleApp(option);
        } else if (state.idleApps.size() < kMaxIdleApps && actions.onAddIdleApp) {
            actions.onAddIdleApp();
        }
        break;
    default:
        return false;
    }
    return true;
}

bool SettingsSections::onSpectrumCommand(int id, Row* row) {
    switch (id) {
    case kIdSpectrum:
        row->checked = !row->checked;
        for (int i = 0; i < kSpectrumEffectCount; ++i) {
            if (auto* effect = findRow(kIdSpectrumEffectFirst + i))
                effect->enabled = row->checked;
        }
        if (auto* colorMode = findRow(kIdSpectrumColorMode))
            colorMode->enabled = row->checked;
        if (auto* color = findRow(kIdSpectrumColor))
            color->enabled = row->checked && findRow(kIdSpectrumColorMode) &&
                             findRow(kIdSpectrumColorMode)->selected == 2;
        if (auto* style = findRow(kIdSpectrumStyle))
            style->enabled = row->checked;
        if (auto* gradient = findRow(kIdSpectrumGradient))
            gradient->enabled = row->checked;
        if (auto* background = findRow(kIdSpectrumBackground))
            background->enabled = spectrumBackgroundAvailable();
        if (auto* opacity = findRow(kIdSpectrumOpacity))
            opacity->enabled = spectrumBackgroundAvailable() && findRow(kIdSpectrumBackground) &&
                               findRow(kIdSpectrumBackground)->checked;
        if (actions.onSpectrum)
            actions.onSpectrum(row->checked);
        updateProgressBackgroundRowsEnabled();
        break;
    case kIdSpectrumColorMode:
        row->selected = std::clamp(row->selected, 0, 2);
        state.spectrumCustomColor = row->selected == 2;
        state.spectrumFollowAlbum = row->selected == 1;
        if (auto* color = findRow(kIdSpectrumColor))
            color->enabled =
                state.spectrumCustomColor && findRow(kIdSpectrum) && findRow(kIdSpectrum)->checked;
        if (actions.onSpectrumColorMode)
            actions.onSpectrumColorMode(row->selected);
        break;
    case kIdSpectrumStyle:
        if (auto* background = findRow(kIdSpectrumBackground))
            background->enabled = spectrumBackgroundAvailable();
        if (auto* opacity = findRow(kIdSpectrumOpacity))
            opacity->enabled = spectrumBackgroundAvailable() && findRow(kIdSpectrumBackground) &&
                               findRow(kIdSpectrumBackground)->checked;
        if (actions.onSpectrumStyle)
            actions.onSpectrumStyle(row->selected);
        updateProgressBackgroundRowsEnabled();
        break;
    case kIdSpectrumEffectFirst:
    case kIdSpectrumEffectFirst + 1:
    case kIdSpectrumEffectFirst + 2:
    case kIdSpectrumEffectFirst + 3:
        row->checked = !row->checked;
        if (actions.onSpectrumEffect)
            actions.onSpectrumEffect(row->id - kIdSpectrumEffectFirst, row->checked);
        break;
    case kIdSpectrumGradient:
        row->checked = !row->checked;
        if (actions.onSpectrumGradient)
            actions.onSpectrumGradient(row->checked);
        break;
    case kIdSpectrumBackground:
        row->checked = !row->checked;
        if (auto* opacity = findRow(kIdSpectrumOpacity))
            opacity->enabled = row->checked && spectrumBackgroundAvailable();
        if (actions.onSpectrumBackground)
            actions.onSpectrumBackground(row->checked);
        updateProgressBackgroundRowsEnabled();
        break;
    case kIdSpectrumOpacity:
        if (actions.onSpectrumOpacity)
            actions.onSpectrumOpacity(row->value);
        break;
    default:
        return false;
    }
    return true;
}

bool SettingsSections::onFloatingCardCommand(int id, Row* row) {
    switch (id) {
    case kIdFloatingCardTrigger:
        state.floatingCardTrigger = row->selected;
        if (actions.onFloatingCardTrigger)
            actions.onFloatingCardTrigger(row->selected);
        break;
    case kIdFloatingCardBackground:
        state.floatingCardBackground = row->selected;
        if (actions.onFloatingCardBackground)
            actions.onFloatingCardBackground(row->selected);
        updateFloatingCardRowsEnabled();
        break;
    case kIdFloatingCardFollowAlbum:
        row->checked = !row->checked;
        state.floatingCardFollowAlbum = row->checked;
        if (actions.onFloatingCardFollowAlbum)
            actions.onFloatingCardFollowAlbum(row->checked);
        break;
    case kIdFloatingCardAutoTextContrast:
        row->checked = !row->checked;
        state.floatingCardAutoTextContrast = row->checked;
        if (actions.onFloatingCardAutoTextContrast)
            actions.onFloatingCardAutoTextContrast(row->checked);
        break;
    default:
        return false;
    }
    return true;
}

bool SettingsSections::onSongToastCommand(int id, Row* row) {
    switch (id) {
    case kIdSongToast:
        row->checked = !row->checked;
        state.songToastEnabled = row->checked;
        if (auto* monitor = findRow(kIdSongToastMonitor))
            monitor->enabled = row->checked;
        if (auto* preview = findRow(kIdSongToastPreview))
            preview->enabled = row->checked && state.songToastPreviewAvailable;
        if (auto* duration = findRow(kIdSongToastDuration))
            duration->enabled = row->checked;
        if (auto* skipFullscreen = findRow(kIdSongToastSkipFullscreen))
            skipFullscreen->enabled = row->checked;
        if (auto* position = findRow(kIdSongToastPosition))
            position->enabled = row->checked;
        if (actions.onSongToastEnabled)
            actions.onSongToastEnabled(row->checked);
        break;
    case kIdSongToastDuration:
        if (actions.onSongToastDuration)
            actions.onSongToastDuration(row->value);
        break;
    case kIdSongToastSkipFullscreen:
        row->checked = !row->checked;
        if (actions.onSongToastSkipFullscreen)
            actions.onSongToastSkipFullscreen(row->checked);
        break;
    case kIdSongToastPosition:
        if (actions.onSongToastPosition)
            actions.onSongToastPosition(row->selected);
        break;
    case kIdSongToastMonitor:
        state.songToastFollowMouseMonitor = row->selected == 1;
        if (actions.onSongToastFollowMouseMonitor)
            actions.onSongToastFollowMouseMonitor(state.songToastFollowMouseMonitor);
        break;
    case kIdSongToastPreview:
        if (actions.onPreviewSongToast)
            actions.onPreviewSongToast();
        break;
    default:
        return false;
    }
    return true;
}

void SettingsSections::onCommand(int id, int option) {
    if (id >= kIdOpenDocs && id <= kIdOpenAbout) {
        const auto& action = id == kIdOpenDocs       ? actions.onOpenDocs
                             : id == kIdOpenFeedback ? actions.onOpenFeedback
                                                     : actions.onOpenAbout;
        if (action)
            action();
        return;
    }
    if (id == kIdNav)
        return;
    if (id == kIdIdleAppNames) {
        Row* appRow = findRow(kIdIdleApps);
        if (!appRow || !appRow->enabled)
            return;
        appRow->checked = !appRow->checked;
        state.idleAppNamesVisible = appRow->checked;
        if (actions.onIdleAppNamesVisible)
            actions.onIdleAppNamesVisible(appRow->checked);
        return;
    }
    Row* row = findRow(id);
    if (!row || !row->enabled)
        return;
    if (settings_ui::handleLyricCommand(id, *row, rows[kLyricsPage], state, actions)) {
        return;
    }
    if (onIdleCommand(id, row, option) || onSpectrumCommand(id, row) ||
        onFloatingCardCommand(id, row) || onSongToastCommand(id, row)) {
        return;
    }
    switch (id) {
    case kIdSongInfo:
        row->checked = !row->checked;
        if (actions.onSongInfoVisible)
            actions.onSongInfoVisible(row->checked);
        break;
    case kIdAlbumCover:
        row->checked = !row->checked;
        if (actions.onAlbumCoverVisible)
            actions.onAlbumCoverVisible(row->checked);
        if (auto* effect = findRow(kIdCoverEffect))
            effect->enabled = row->checked && !minimalModeActive();
        if (auto* platformIcon = findRow(kIdPlatformIcon))
            platformIcon->enabled = row->checked;
        break;
    case kIdPlatformIcon:
        row->checked = !row->checked;
        if (actions.onPlatformIconVisible)
            actions.onPlatformIconVisible(row->checked);
        break;
    case kIdCoverEffect:
        if (actions.onCoverEffectVinyl)
            actions.onCoverEffectVinyl(row->selected == 1);
        break;
    case kIdTaskbarContextMenu:
        row->checked = !row->checked;
        if (actions.onTaskbarContextMenu)
            actions.onTaskbarContextMenu(row->checked);
        break;
    case kIdTaskbarViewMode:
        if (row->selected == 1 && (!state.mediaSessionAlive || state.verticalTaskbar)) {
            row->selected = std::clamp(state.taskbarViewMode, 0, 2);
            break;
        }
        state.taskbarViewMode = std::clamp(row->selected, 0, 2);
        updateImmersiveRowsEnabled();
        updateIdleRowsEnabled();
        if (actions.onTaskbarViewMode)
            actions.onTaskbarViewMode(state.taskbarViewMode);
        break;
    case kIdTaskbarMonitor:
        if (actions.onChooseTaskbarMonitor)
            actions.onChooseTaskbarMonitor();
        break;
    case kIdAppBarEdge:
        state.appBarEdge = std::clamp(row->selected, 0, 1);
        if (actions.onAppBarEdge)
            actions.onAppBarEdge(state.appBarEdge);
        break;
    case kIdImmersiveBackgroundAdjustment:
        state.immersiveBackgroundAdjustment = std::clamp(row->selected, 0, 1);
        updateBackgroundAdjustmentPresentation();
        if (actions.onImmersiveBackgroundAdjustment)
            actions.onImmersiveBackgroundAdjustment(state.immersiveBackgroundAdjustment);
        break;
    case kIdDockBackgroundAdjustment:
        state.dockBackgroundAdjustment = std::clamp(row->selected, 0, 1);
        updateBackgroundAdjustmentPresentation();
        if (actions.onDockBackgroundAdjustment)
            actions.onDockBackgroundAdjustment(state.dockBackgroundAdjustment);
        break;
    case kIdImmersiveBackgroundBlur:
        if (actions.onImmersiveBackgroundBlur)
            actions.onImmersiveBackgroundBlur(row->value);
        break;
    case kIdDockBackgroundBlur:
        if (actions.onDockBackgroundBlur)
            actions.onDockBackgroundBlur(row->value);
        break;
    case kIdEmbeddedResourceMonitor:
        row->checked = !row->checked;
        state.embeddedResourceMonitor = row->checked;
        updateImmersiveRowsEnabled();
        updateIdleRowsEnabled();
        if (actions.onEmbeddedResourceMonitor)
            actions.onEmbeddedResourceMonitor(row->checked);
        break;
    case kIdDockResourceGpuUsage:
        row->checked = !row->checked;
        if (actions.onDockResourceGpuUsage)
            actions.onDockResourceGpuUsage(row->checked);
        break;
    case kIdDockResourceDownload:
        row->checked = !row->checked;
        state.dockResourceDownload = row->checked;
        if (actions.onDockResourceDownload)
            actions.onDockResourceDownload(row->checked);
        break;
    case kIdDockResourceUpload:
        row->checked = !row->checked;
        state.dockResourceUpload = row->checked;
        if (actions.onDockResourceUpload)
            actions.onDockResourceUpload(row->checked);
        break;
    case kIdDockResourceCpuFrequency:
        row->checked = !row->checked;
        if (actions.onDockResourceCpuFrequency)
            actions.onDockResourceCpuFrequency(row->checked);
        break;
    case kIdTaskbarBackground:
        if (auto* opacity = findRow(kIdCoverBackgroundOpacity))
            opacity->enabled = row->selected == 1;
        if (actions.onTaskbarBackground)
            actions.onTaskbarBackground(row->selected);
        break;
    case kIdCoverBackgroundOpacity:
        if (actions.onCoverBackgroundOpacity)
            actions.onCoverBackgroundOpacity(row->value);
        break;
    case kIdProgressBackground:
        row->checked = !row->checked;
        updateProgressBackgroundRowsEnabled();
        if (actions.onProgressBackground)
            actions.onProgressBackground(row->checked);
        break;
    case kIdProgressBackgroundOpacity:
        if (actions.onProgressBackgroundOpacity)
            actions.onProgressBackgroundOpacity(row->value);
        break;
    case kIdHoverControls:
        row->checked = !row->checked;
        if (actions.onHoverControls)
            actions.onHoverControls(row->checked);
        if (auto* style = findRow(kIdHoverControlStyle))
            style->enabled = row->checked && !minimalModeActive();
        updateFloatingCardRowsEnabled();
        break;
    case kIdHoverControlStyle:
        if (actions.onHoverControlStyle)
            actions.onHoverControlStyle(row->selected);
        updateFloatingCardRowsEnabled();
        updateIdleRowsEnabled();
        break;
    case kIdTaskbarTheme:
        if (actions.onTaskbarTheme)
            actions.onTaskbarTheme(themeModeFromIndex(row->selected));
        break;
    case kIdWindowTheme:
        if (actions.onWindowTheme)
            actions.onWindowTheme(themeModeFromIndex(row->selected));
        break;
    case kIdRenderMode:
        if (actions.onRenderMode)
            actions.onRenderMode(row->selected);
        break;
    case kIdPickFont:
        if (actions.onPickFont)
            actions.onPickFont();
        break;
    }
}

} // namespace settings_ui
