#include "ui/settings/sections.h"
#include "ui/settings/section_labels.h"
#include "ui/settings/section_metrics.h"
#include "ui/settings/lyric_section.h"
#include "ui/settings/spectrum_section.h"
#include <algorithm>

namespace settings_ui {
Row* SettingsSections::findRow(int id) {
    for (auto& page : rows) {
        for (auto& row : page) {
            if (row.id == id)
                return &row;
        }
    }
    return nullptr;
}

bool SettingsSections::minimalModeActive() const {
    if (state.renderMode == kRenderModeMinimal)
        return true;
    const auto* mode = findRow(kIdRenderMode);
    return mode && mode->selected == kRenderModeMinimal;
}

void SettingsSections::updateFloatingCardRowsEnabled() {
    const auto* idleEntry = findRow(kIdIdleEntry);
    const auto* controls = findRow(kIdHoverControls);
    const auto* style = findRow(kIdHoverControlStyle);
    const auto* background = findRow(kIdFloatingCardBackground);
    const bool mediaCardEnabled = controls && controls->checked && style && style->selected == 1;
    const bool cardEnabled =
        !minimalModeActive() && ((idleEntry && idleEntry->checked) || mediaCardEnabled);
    const bool frosted = cardEnabled && background && background->selected == 1;
    if (auto* row = findRow(kIdFloatingCardTrigger))
        row->enabled = cardEnabled;
    if (auto* row = findRow(kIdFloatingCardBackground))
        row->enabled = cardEnabled;
    if (auto* row = findRow(kIdFloatingCardBackgroundColor))
        row->enabled = frosted;
    if (auto* row = findRow(kIdFloatingCardFollowAlbum))
        row->enabled = frosted;
    if (auto* row = findRow(kIdFloatingCardAutoTextContrast))
        row->enabled = frosted;
}

bool SettingsSections::backgroundWaveActive() const {
    const auto* spectrum = findRow(kIdSpectrum);
    const auto* style = findRow(kIdSpectrumStyle);
    const auto* background = findRow(kIdSpectrumBackground);
    return spectrum && spectrum->checked && style && style->selected == 2 && background &&
           background->checked;
}

bool SettingsSections::spectrumBackgroundAvailable() const {
    const auto* spectrum = findRow(kIdSpectrum);
    const auto* style = findRow(kIdSpectrumStyle);
    return spectrum && spectrum->checked && style && style->selected == 2;
}

void SettingsSections::updateProgressBackgroundRowsEnabled() {
    const bool available = !minimalModeActive() && !backgroundWaveActive();
    if (auto* row = findRow(kIdProgressBackground))
        row->enabled = available;
    if (auto* row = findRow(kIdProgressBackgroundOpacity))
        row->enabled =
            available && findRow(kIdProgressBackground) && findRow(kIdProgressBackground)->checked;
}

void SettingsSections::updateBackgroundAdjustmentPresentation() {
    auto update = [this](int modeId, int sliderId, bool immersive) {
        const auto* modeRow = findRow(modeId);
        const int selected = modeRow ? std::clamp(modeRow->selected, 0, 1) : 0;
        if (auto* slider = findRow(sliderId)) {
            slider->text = backgroundAdjustmentLabel(immersive, selected);
            slider->hint = backgroundAdjustmentHint(state.backgroundBlurSupported, selected);
            slider->showHint = true;
            slider->minHeight = kRowTallH;
            slider->height = slider->minHeight;
        }
    };
    update(kIdImmersiveBackgroundAdjustment, kIdImmersiveBackgroundBlur, true);
    update(kIdDockBackgroundAdjustment, kIdDockBackgroundBlur, false);
}

void SettingsSections::updateImmersiveRowsEnabled() {
    auto* mode = findRow(kIdTaskbarViewMode);
    const int selectedMode = mode ? std::clamp(mode->selected, 0, 2) : 0;
    const bool immersiveEnabled = state.mediaSessionAlive && !state.verticalTaskbar;
    const bool appBarEnabled = selectedMode == 2;
    const bool expandedEnabled = appBarEnabled || (selectedMode == 1 && immersiveEnabled);
    if (mode) {
        mode->enabled = true;
        mode->optionEnabled.assign(mode->options.size(), true);
        if (mode->optionEnabled.size() > 1)
            mode->optionEnabled[1] = immersiveEnabled;
    }
    if (auto* row = findRow(kIdAppBarEdge))
        row->enabled = appBarEnabled;
    if (auto* row = findRow(kIdTaskbarMonitor))
        row->enabled = selectedMode != 0;
    if (auto* row = findRow(kIdImmersiveBackgroundAdjustment)) {
        row->selected = std::clamp(state.immersiveBackgroundAdjustment, 0, 1);
        row->enabled = selectedMode == 1 && immersiveEnabled && state.backgroundBlurSupported;
    }
    if (auto* row = findRow(kIdDockBackgroundAdjustment)) {
        row->selected = std::clamp(state.dockBackgroundAdjustment, 0, 1);
        row->enabled = appBarEnabled && state.backgroundBlurSupported;
    }
    if (auto* row = findRow(kIdImmersiveBackgroundBlur)) {
        row->enabled = selectedMode == 1 && immersiveEnabled && state.backgroundBlurSupported;
        row->hint = backgroundAdjustmentHint(state.backgroundBlurSupported,
                                             state.immersiveBackgroundAdjustment);
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
    if (auto* row = findRow(kIdDockBackgroundBlur)) {
        row->enabled = appBarEnabled && state.backgroundBlurSupported;
        row->hint =
            backgroundAdjustmentHint(state.backgroundBlurSupported, state.dockBackgroundAdjustment);
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
    updateBackgroundAdjustmentPresentation();
    const bool embeddedResources = selectedMode == 0 && !state.verticalTaskbar;
    if (auto* row = findRow(kIdEmbeddedResourceMonitor))
        row->enabled = embeddedResources;
    const bool resourceExtrasEnabled =
        appBarEnabled || (embeddedResources && state.embeddedResourceMonitor);
    if (auto* row = findRow(kIdDockResourceDownload))
        row->enabled = resourceExtrasEnabled;
    if (auto* row = findRow(kIdDockResourceUpload))
        row->enabled = resourceExtrasEnabled;
    if (auto* row = findRow(kIdDockResourceGpuUsage))
        row->enabled = resourceExtrasEnabled;
    if (auto* row = findRow(kIdDockResourceCpuFrequency))
        row->enabled = resourceExtrasEnabled;
    auto* controls = findRow(kIdHoverControls);
    if (controls)
        controls->enabled = !expandedEnabled;
    if (auto* row = findRow(kIdHoverControlStyle))
        row->enabled = !expandedEnabled && controls && controls->checked && !minimalModeActive();
    if (auto* row = findRow(kIdAlignment))
        row->enabled = !expandedEnabled && !state.verticalTaskbar;
}

void SettingsSections::updateIdleAppsRowHeight() {
    if (auto* row = findRow(kIdIdleApps)) {
        const size_t count = std::max<size_t>(1, state.idleApps.size());
        row->minHeight = kIdleAppsBaseH + static_cast<float>(count) * kIdleAppItemH;
        row->height = row->minHeight;
    }
}

void SettingsSections::updateTickTickRows() {
    const bool hasToken = state.tickTickApiTokenConfigured;
    const bool enabled = state.tickTickEnabled && hasToken;
    if (auto* row = findRow(kIdTickTickEnabled)) {
        row->checked = enabled;
        row->enabled = true;
        row->hint = hasToken ? (enabled ? L"已开启自动同步，并在快捷启动卡片中显示今日任务。"
                                        : L"已关闭自动同步和今日任务入口；API 口令仍保留在本机。")
                             : L"未配置 API 口令，当前处于隐式关闭状态；开启时会先要求填写口令。";
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
    if (auto* row = findRow(kIdTickTickApiToken)) {
        row->hint = L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建并复制；"
                    L"程序只将口令保存到 Windows 凭据管理器。";
        row->controlText = state.tickTickApiTokenConfigured ? L"已配置" : L"未配置";
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = !hasToken || enabled;
    }
    if (auto* row = findRow(kIdTickTickConnect)) {
        row->controlText = state.tickTickConnecting  ? L"连接中…"
                           : state.tickTickConnected ? L"重新连接"
                                                     : L"连接…";
        row->hint = state.tickTickStatus.empty() ? (state.tickTickApiTokenConfigured
                                                        ? L"连接后，卡片会显示今天的未完成任务。"
                                                        : L"请先填写 API 口令。")
                                                 : state.tickTickStatus;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = enabled && !state.tickTickConnecting && !state.tickTickSyncing;
    }
    if (auto* row = findRow(kIdTickTickRefresh)) {
        row->enabled = enabled && state.tickTickConnected && !state.tickTickSyncing;
        row->controlText = state.tickTickSyncing ? L"同步中…" : L"立即同步";
    }
    if (auto* row = findRow(kIdTickTickDisconnect)) {
        row->enabled = hasToken && !state.tickTickConnecting && !state.tickTickSyncing;
    }
}

void SettingsSections::updateIdleQuoteSourceRow() {
    if (auto* row = findRow(kIdIdleQuoteSource)) {
        row->hint = idleQuoteSourceHint(state.idleQuoteSource);
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = kRowTallH;
    }
}

int SettingsSections::idleTextMode() const {
    return state.idleQuoteEnabled ? 0 : state.idleWelcomeEnabled ? 1 : 2;
}

bool SettingsSections::embeddedResourceOnlyAvailable() const {
    return state.taskbarViewMode == 0 && state.embeddedResourceMonitor && !state.verticalTaskbar;
}

void SettingsSections::updateIdleRowsEnabled() {
    const bool quoteEnabled = state.idleQuoteEnabled;
    if (auto* row = findRow(kIdIdleQuote)) {
        row->selected = idleTextMode();
        row->optionEnabled.assign(row->options.size(), true);
        if (row->optionEnabled.size() > 2)
            row->optionEnabled[2] = embeddedResourceOnlyAvailable() || row->selected == 2;
    }
    if (auto* row = findRow(kIdIdleQuoteSource))
        row->enabled = quoteEnabled;
    if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
        row->enabled = quoteEnabled;
    if (auto* row = findRow(kIdIdleQuoteAlignment))
        row->enabled = quoteEnabled && !state.verticalTaskbar;
    if (auto* row = findRow(kIdIdleCustomWelcome))
        row->enabled = state.idleWelcomeEnabled;
    if (auto* row = findRow(kIdIdleQuoteBackground))
        row->enabled = !minimalModeActive();
    if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
        row->enabled = !minimalModeActive() && state.idleQuoteBackground != 0;
    if (auto* row = findRow(kIdIdleApps))
        row->enabled = !minimalModeActive() && state.idleEntryEnabled;
}

void SettingsSections::updateCompactLyricRows() {
    settings_ui::updateCompactLyricRows(rows[kLyricsPage], state);
}

void SettingsSections::syncIdleRows(const SettingsState& s) {
    const bool vertical = s.verticalTaskbar;
    if (auto* row = findRow(kIdIdleEntry))
        row->checked = s.idleEntryEnabled;
    if (auto* row = findRow(kIdIdleQuote))
        row->selected = idleTextMode();
    if (auto* row = findRow(kIdIdleQuoteSource))
        row->selected = std::clamp(s.idleQuoteSource, 0, 2);
    if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
        row->selected = std::clamp(s.idleQuoteRefreshInterval, 0, 2);
    if (auto* row = findRow(kIdIdleQuoteAlignment))
        row->selected = vertical ? 0 : std::clamp(s.idleQuoteAlignment, 0, 2);
    if (auto* row = findRow(kIdIdleQuoteBackground))
        row->selected = std::clamp(s.idleQuoteBackground, 0, 4);
    if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
        row->selected = std::clamp(s.idleQuoteBackgroundScope, 0, 3);
    if (auto* row = findRow(kIdIdleApps))
        row->checked = s.idleAppNamesVisible;
    if (auto* row = findRow(kIdTickTickEnabled))
        row->checked = s.tickTickEnabled;
    updateIdleQuoteSourceRow();
    updateIdleAppsRowHeight();
    updateTickTickRows();
}

void SettingsSections::syncDisplayRows(const SettingsState& s) {
    const bool vertical = s.verticalTaskbar;
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdSongInfo)) {
        row->checked = vertical ? false : s.songInfoVisible;
        row->enabled = !vertical;
    }
    if (auto* row = findRow(kIdAlbumCover))
        row->checked = s.albumCoverVisible;
    if (auto* row = findRow(kIdPlatformIcon)) {
        row->checked = s.platformIconVisible;
        row->enabled = s.albumCoverVisible;
    }
    if (auto* row = findRow(kIdCoverEffect)) {
        row->selected = minimal ? 0 : (s.coverEffectVinyl ? 1 : 0);
        row->enabled = s.albumCoverVisible && !minimal;
    }
    if (auto* row = findRow(kIdTaskbarContextMenu))
        row->checked = s.taskbarContextMenu;
}

void SettingsSections::syncTaskbarModeRows(const SettingsState& s) {
    const bool vertical = s.verticalTaskbar;
    if (auto* row = findRow(kIdTaskbarViewMode)) {
        row->selected = std::clamp(s.taskbarViewMode, 0, 2);
        row->hint = taskbarViewModeHint(s.mediaSessionAlive, vertical);
        row->showHint = true;
    }
    if (auto* row = findRow(kIdTaskbarMonitor)) {
        row->controlText = s.taskbarMonitorLabel;
        row->enabled = s.taskbarViewMode != 0;
    }
    if (auto* row = findRow(kIdAppBarEdge)) {
        row->selected = std::clamp(s.appBarEdge, 0, 1);
        row->enabled = s.taskbarViewMode == 2;
    }
    if (auto* row = findRow(kIdImmersiveBackgroundAdjustment)) {
        row->selected = std::clamp(s.immersiveBackgroundAdjustment, 0, 1);
        row->enabled = !vertical && s.taskbarViewMode == 1 && s.backgroundBlurSupported;
    }
    if (auto* row = findRow(kIdDockBackgroundAdjustment)) {
        row->selected = std::clamp(s.dockBackgroundAdjustment, 0, 1);
        row->enabled = s.taskbarViewMode == 2 && s.backgroundBlurSupported;
    }
    if (auto* row = findRow(kIdImmersiveBackgroundBlur)) {
        row->value = std::clamp(s.immersiveBackgroundBlur, 0, 100);
        row->enabled = !vertical && s.taskbarViewMode == 1 && s.backgroundBlurSupported;
        row->hint =
            backgroundAdjustmentHint(s.backgroundBlurSupported, s.immersiveBackgroundAdjustment);
        row->showHint = true;
    }
    if (auto* row = findRow(kIdDockBackgroundBlur)) {
        row->value = std::clamp(s.dockBackgroundBlur, 0, 100);
        row->enabled = s.taskbarViewMode == 2 && s.backgroundBlurSupported;
        row->hint = backgroundAdjustmentHint(s.backgroundBlurSupported, s.dockBackgroundAdjustment);
        row->showHint = true;
    }
    updateBackgroundAdjustmentPresentation();
    if (auto* row = findRow(kIdEmbeddedResourceMonitor)) {
        row->checked = s.embeddedResourceMonitor;
        row->enabled = s.taskbarViewMode == 0 && !vertical;
    }
    const bool resourceExtrasEnabled =
        s.taskbarViewMode == 2 ||
        (s.taskbarViewMode == 0 && !vertical && s.embeddedResourceMonitor);
    if (auto* row = findRow(kIdDockResourceDownload)) {
        row->checked = s.dockResourceDownload;
        row->enabled = resourceExtrasEnabled;
    }
    if (auto* row = findRow(kIdDockResourceUpload)) {
        row->checked = s.dockResourceUpload;
        row->enabled = resourceExtrasEnabled;
    }
    if (auto* row = findRow(kIdDockResourceGpuUsage)) {
        row->checked = s.dockResourceGpuUsage;
        row->enabled = resourceExtrasEnabled;
    }
    if (auto* row = findRow(kIdDockResourceCpuFrequency)) {
        row->checked = s.dockResourceCpuFrequency;
        row->enabled = resourceExtrasEnabled;
    }
    settings_ui::updateSpectrumRows(rows[kSpectrumPage], s);
}

void SettingsSections::syncBackgroundRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdTaskbarBackground)) {
        row->selected = minimal ? 0 : std::clamp(s.taskbarBackground, 0, 2);
        row->enabled = !minimal;
    }
    if (auto* row = findRow(kIdCoverBackgroundOpacity)) {
        row->value = std::clamp(s.coverBackgroundOpacity, 0, 100);
        row->enabled = !minimal && s.taskbarBackground == 1;
    }
    if (auto* row = findRow(kIdProgressBackground))
        row->checked = minimal ? false : s.progressBackground;
    if (auto* row = findRow(kIdProgressBackgroundOpacity))
        row->value = std::clamp(s.progressBackgroundOpacity, 0, 100);
    updateProgressBackgroundRowsEnabled();
}

void SettingsSections::syncMediaControlRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdHoverControls))
        row->checked = s.hoverControls;
    if (auto* row = findRow(kIdHoverControlStyle)) {
        row->selected = minimal ? 0 : s.hoverControlStyle;
        row->enabled = s.hoverControls && !minimal;
    }
}

void SettingsSections::syncFloatingCardRows(const SettingsState& s) {
    if (auto* row = findRow(kIdFloatingCardTrigger))
        row->selected = std::clamp(s.floatingCardTrigger, 0, 1);
    if (auto* row = findRow(kIdFloatingCardBackground))
        row->selected = std::clamp(s.floatingCardBackground, 0, 1);
    if (auto* row = findRow(kIdFloatingCardBackgroundColor))
        row->controlText = colorText(s.floatingCardBackgroundColor);
    if (auto* row = findRow(kIdFloatingCardFollowAlbum))
        row->checked = s.floatingCardFollowAlbum;
    if (auto* row = findRow(kIdFloatingCardAutoTextContrast))
        row->checked = s.floatingCardAutoTextContrast;
    updateFloatingCardRowsEnabled();
    updateIdleRowsEnabled();
}

void SettingsSections::syncSongToastRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdSongToast)) {
        row->checked = minimal ? false : s.songToastEnabled;
        row->enabled = !minimal;
    }
    if (auto* row = findRow(kIdSongToastDuration)) {
        row->value = std::clamp(s.songToastDurationSec, 1, 10);
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastSkipFullscreen)) {
        row->checked = s.songToastSkipFullscreen;
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastPosition)) {
        row->selected = std::clamp(s.songToastPosition, 0, 1);
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastMonitor)) {
        row->selected = s.songToastFollowMouseMonitor ? 1 : 0;
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastPreview))
        row->enabled = !minimal && s.songToastEnabled && s.songToastPreviewAvailable;
}

void SettingsSections::syncThemeFontRows(const SettingsState& s) {
    if (auto* row = findRow(kIdTaskbarTheme))
        row->selected = themeModeIndex(s.taskbarThemeMode);
    if (auto* row = findRow(kIdWindowTheme))
        row->selected = themeModeIndex(s.windowThemeMode);
    if (auto* row = findRow(kIdRenderMode))
        row->selected = std::clamp(s.renderMode, 0, kRenderModeMinimal);
    if (auto* row = findRow(kIdPickFont)) {
        row->valueText = s.fontDesc;
        row->hint = kFontSettingNotice;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
}

void SettingsSections::update(const SettingsState& s) {
    state = s;
    syncIdleRows(s);
    syncDisplayRows(s);
    syncTaskbarModeRows(s);
    settings_ui::updateSpectrumRows(rows[kSpectrumPage], s);
    syncBackgroundRows(s);
    syncMediaControlRows(s);
    syncFloatingCardRows(s);
    syncSongToastRows(s);
    syncThemeFontRows(s);
    settings_ui::updateLyricRows(rows[kLyricsPage], s);
    updateImmersiveRowsEnabled();
}

const Row* SettingsSections::findRow(int id) const {
    for (const auto& page : rows) {
        for (const auto& row : page) {
            if (row.id == id)
                return &row;
        }
    }
    return nullptr;
}

} // namespace settings_ui
