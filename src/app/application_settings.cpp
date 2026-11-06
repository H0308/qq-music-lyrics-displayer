#include "app/application_internal.h"
#include "settings/display_rules.h"
namespace lyrics_app {
void App::enforceSecondaryLyricConstraint() {
    if (display_rules::resolveLyricConflict(settings_, display_rules::LyricPriority::Secondary))
        logSettingInt(L"lyric-effects", static_cast<int>(settings_.lyricEffects_));
}

void App::commitAppearanceSettings() {
    publishTaskbarAppearance();
    refreshSettingsDialog(true);
    saveSettings();
}

void App::logSettingBool(const wchar_t* name, bool value) {
    runtime_log::writef(L"[action][setting] %s=%s", name, value ? L"on" : L"off");
}

void App::logSettingInt(const wchar_t* name, int value) {
    runtime_log::writef(L"[action][setting] %s=%d", name, value);
}

void App::applySongInfoVisible(bool on) {
    settings_.songInfoVisible_ = on;
    logSettingBool(L"song-info-visible", on);
    commitAppearanceSettings();
}

void App::applyAlbumCoverVisible(bool on) {
    settings_.albumCoverVisible_ = on;
    logSettingBool(L"album-cover-visible", on);
    commitAppearanceSettings();
}

void App::applyPlatformIconVisible(bool on) {
    settings_.platformIconVisible_ = on;
    logSettingBool(L"platform-icon-visible", on);
    commitAppearanceSettings();
}

void App::applyCoverEffect(bool vinyl) {
    settings_.albumCoverEffect_ = vinyl ? AlbumCoverEffect::Vinyl : AlbumCoverEffect::Default;
    logSettingBool(L"album-cover-vinyl", vinyl);
    commitAppearanceSettings();
}

void App::applySpectrumOn(bool on) {
    if (taskbarVertical_)
        return;
    settings_.spectrumOn_ = on;
    if (settings_.spectrumOn_)
        taskbarSpectrumSuppressedForNoSpace_ = false;
    syncSpectrumWithMode();
    logSettingBool(L"spectrum", settings_.spectrumOn_);
    saveSettings();
}

void App::applySpectrumStyle(int style) {
    if (taskbarVertical_)
        return;
    settings_.spectrumStyle_ = static_cast<SpectrumStyle>(std::clamp(style, 0, 4));
    publishTaskbarAppearance();
    tryRestoreSpectrumAfterNoSpace();
    logSettingInt(L"spectrum-style", spectrumStyleIndex(settings_.spectrumStyle_));
    saveSettings();
}

void App::applySpectrumColorMode(int mode) {
    if (taskbarVertical_)
        return;
    const int normalizedMode = std::clamp(mode, 0, 2);
    settings_.spectrumCustomColor_ = normalizedMode == 2;
    settings_.spectrumFollowAlbum_ = normalizedMode == 1;
    logSettingInt(L"spectrum-color-mode", normalizedMode);
    commitAppearanceSettings();
}

void App::applySpectrumColor(COLORREF color) {
    if (taskbarVertical_)
        return;
    settings_.spectrumColor_ = color;
    settings_.spectrumCustomColor_ = true;
    settings_.spectrumFollowAlbum_ = false;
    runtime_log::writef(L"[action][setting] spectrum-color=#%02X%02X%02X", GetRValue(color),
                        GetGValue(color), GetBValue(color));
    commitAppearanceSettings();
}

void App::applySpectrumGradient(bool on) {
    if (taskbarVertical_)
        return;
    settings_.spectrumGradient_ = on;
    logSettingBool(L"spectrum-gradient", settings_.spectrumGradient_);
    commitAppearanceSettings();
}

void App::applySpectrumEffect(int effect, bool on) {
    if (effect < 0 || effect >= kSpectrumEffectCount)
        return;
    const uint32_t bit = spectrumEffectBit(kSpectrumEffects[effect]);
    if (on)
        settings_.spectrumEffects_ |= bit;
    else
        settings_.spectrumEffects_ &= ~bit;
    logSettingInt(L"spectrum-effects", static_cast<int>(settings_.spectrumEffects_));
    commitAppearanceSettings();
}

void App::applyLyricEffect(int effect, bool on) {
    if (effect < 0 || effect >= kLyricEffectCount)
        return;
    const uint32_t bit = 1u << effect;
    if (on)
        settings_.lyricEffects_ |= bit;
    else
        settings_.lyricEffects_ &= ~bit;
    if (on && effect == static_cast<int>(LyricEffect::Stardust) &&
        display_rules::resolveLyricConflict(settings_, display_rules::LyricPriority::Effects))
        logSettingBool(L"secondary-lyrics", false);
    logSettingInt(L"lyric-effects", static_cast<int>(settings_.lyricEffects_));
    commitAppearanceSettings();
}

void App::applySpectrumBackground(bool on) {
    if (taskbarVertical_)
        return;
    settings_.spectrumBackground_ = on;
    logSettingBool(L"spectrum-background", settings_.spectrumBackground_);
    commitAppearanceSettings();
}

void App::applySpectrumOpacity(int percent) {
    settings_.spectrumOpacity_ = std::clamp(percent, 0, 100);
    logSettingInt(L"spectrum-opacity", settings_.spectrumOpacity_);
    commitAppearanceSettings();
}

void App::applyProgressBackground(bool on) {
    settings_.progressBackground_ = on;
    logSettingBool(L"progress-background", on);
    commitAppearanceSettings();
}

void App::applyProgressBackgroundOpacity(int percent) {
    settings_.progressBackgroundOpacity_ = std::clamp(percent, 0, 100);
    logSettingInt(L"progress-background-opacity", settings_.progressBackgroundOpacity_);
    commitAppearanceSettings();
}

void App::applyTaskbarBackground(int mode) {
    settings_.taskbarBackground_ = std::clamp(mode, 0, 2);
    logSettingInt(L"taskbar-background", settings_.taskbarBackground_);
    commitAppearanceSettings();
}

void App::applyCoverBackgroundOpacity(int percent) {
    settings_.coverBackgroundOpacity_ = std::clamp(percent, 0, 100);
    logSettingInt(L"cover-background-opacity", settings_.coverBackgroundOpacity_);
    commitAppearanceSettings();
}

void App::applyTaskbarViewMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    settings_.taskbarViewMode_ = static_cast<TaskbarViewMode>(mode);
    if (taskbarHost || taskbarPendingDisplayRebuild_) {
        publishTaskbarAppearance();
        syncTaskbarView(monitor.snapshot());
        if (taskbarHost) {
            syncTaskbarOrientation();
            applyEffectiveTaskbarSettings();
            publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
        }
    }
    logSettingInt(L"taskbar-view-mode", mode);
    saveSettings();
    refreshSettingsDialog(true);
}

void App::applyAppBarEdge(int edge) {
    settings_.appBarEdge_ = edge == 1 ? AppBarEdge::Bottom : AppBarEdge::Top;
    forEachHost([&](TaskbarHost* host) { host->setAppBarEdge(settings_.appBarEdge_); });
    logSettingInt(L"appbar-edge", settings_.appBarEdge_ == AppBarEdge::Bottom ? 1 : 0);
    saveSettings();
    refreshSettingsDialog(true);
}

void App::applyImmersiveBackgroundBlur(int percent) {
    const int nextBlur = std::clamp(percent, 0, 100);
    if (settings_.immersiveBackgroundBlur_ == nextBlur)
        return;
    settings_.immersiveBackgroundBlur_ = nextBlur;
    publishTaskbarAppearance();
    scheduleBackgroundBlurSettingsSave(true);
    if (taskbarHost && !taskbarHost->backgroundBlurAvailable())
        refreshSettingsDialog(true);
}

void App::applyDockBackgroundBlur(int percent) {
    const int nextBlur = std::clamp(percent, 0, 100);
    if (settings_.dockBackgroundBlur_ == nextBlur)
        return;
    settings_.dockBackgroundBlur_ = nextBlur;
    publishTaskbarAppearance();
    scheduleBackgroundBlurSettingsSave(false);
    if (taskbarHost && !taskbarHost->backgroundBlurAvailable())
        refreshSettingsDialog(true);
}

void App::scheduleBackgroundBlurSettingsSave(bool immersive) {
    if (immersive)
        immersiveBackgroundBlurLogPending_ = true;
    else
        dockBackgroundBlurLogPending_ = true;
    backgroundBlurSettingsSavePending_ = true;
    if (trayHwnd && SetTimer(trayHwnd, kTimerBackgroundSettingsSave,
                             kBackgroundSettingsSaveDebounceMs, nullptr) != 0)
        return;
    flushBackgroundBlurSettingsSave();
}

void App::flushBackgroundBlurSettingsSave() {
    if (!backgroundBlurSettingsSavePending_)
        return;
    backgroundBlurSettingsSavePending_ = false;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerBackgroundSettingsSave);
    if (immersiveBackgroundBlurLogPending_) {
        immersiveBackgroundBlurLogPending_ = false;
        logSettingInt(L"immersive-background-blur", settings_.immersiveBackgroundBlur_);
    }
    if (dockBackgroundBlurLogPending_) {
        dockBackgroundBlurLogPending_ = false;
        logSettingInt(L"dock-background-blur", settings_.dockBackgroundBlur_);
    }
    saveSettings();
}

void App::applyImmersiveBackgroundAdjustment(int mode) {
    settings_.immersiveBackgroundAdjustment_ = std::clamp(mode, 0, 1);
    publishTaskbarAppearance();
    logSettingInt(L"immersive-background-adjustment", settings_.immersiveBackgroundAdjustment_);
    saveSettings();
    if (taskbarHost && !taskbarHost->backgroundBlurAvailable())
        refreshSettingsDialog(true);
}

void App::applyDockBackgroundAdjustment(int mode) {
    settings_.dockBackgroundAdjustment_ = std::clamp(mode, 0, 1);
    publishTaskbarAppearance();
    logSettingInt(L"dock-background-adjustment", settings_.dockBackgroundAdjustment_);
    saveSettings();
    if (taskbarHost && !taskbarHost->backgroundBlurAvailable())
        refreshSettingsDialog(true);
}

void App::applyDockResourceVisibility() {
    if (!taskbarHost)
        return;
    DockResourceVisibility visibility;
    visibility.embeddedEnabled = settings_.embeddedResourceMonitor_;
    visibility.download = settings_.dockResourceDownload_;
    visibility.upload = settings_.dockResourceUpload_;
    visibility.gpuUsage = settings_.dockResourceGpuUsage_;
    visibility.cpuFrequency = settings_.dockResourceCpuFrequency_;
    publishTaskbarAppearance();
}

void App::applyEmbeddedResourceMonitor(bool on) {
    settings_.embeddedResourceMonitor_ = on;
    applyDockResourceVisibility();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
    refreshSettingsDialog(true);
    logSettingBool(L"embedded-resource-monitor", on);
    saveSettings();
}

void App::applyDockResourceGpuUsage(bool on) {
    settings_.dockResourceGpuUsage_ = on;
    applyDockResourceVisibility();
    logSettingBool(L"dock-resource-gpu", on);
    saveSettings();
}

void App::applyDockResourceNetwork(bool download, bool on) {
    (download ? settings_.dockResourceDownload_ : settings_.dockResourceUpload_) = on;
    applyDockResourceVisibility();
    logSettingBool(download ? L"dock-resource-download" : L"dock-resource-upload", on);
    saveSettings();
}

void App::applyDockResourceCpuFrequency(bool on) {
    settings_.dockResourceCpuFrequency_ = on;
    applyDockResourceVisibility();
    logSettingBool(L"dock-resource-cpu-frequency", on);
    saveSettings();
}

bool App::isRenderMode(RenderMode mode) const { return renderMode_ == static_cast<int>(mode); }

bool App::isMinimalRenderMode() const { return isRenderMode(RenderMode::Minimal); }

bool App::isNormalRenderMode() const { return isRenderMode(RenderMode::Normal); }

TaskbarAppearance App::appearanceForHost(bool vertical) const {
    AppearanceContext context;
    context.renderMode = static_cast<RenderMode>(renderMode_);
    context.vertical = vertical;
    context.lyricAppearance = currentLyricAppearance();
    context.playedColor = effectivePlayedColor();
    context.albumColor = playback_.state().albumColor_;
    context.albumColorAvailable = playback_.state().hasAlbumColor_;
    context.floatingCardBackground = effectiveFloatingCardBackgroundColor();
    return resolveTaskbarAppearance(settings_, context);
}

void App::publishTaskbarAppearance() {
    forEachHost([&](TaskbarHost* host) {
        host->applyAppearance(appearanceForHost(host->isVerticalTaskbar()));
    });
}

void App::applyEffectiveTaskbarSettings() {
    publishTaskbarAppearance();
    syncTaskbarView(monitor.snapshot());
}

void App::applyRenderMode(int mode) {
    const int nextMode = std::clamp(mode, 0, 3);
    const bool enteringStopped =
        !isRenderMode(RenderMode::Stopped) && nextMode == static_cast<int>(RenderMode::Stopped);
    const bool leavingStopped =
        isRenderMode(RenderMode::Stopped) && nextMode != static_cast<int>(RenderMode::Stopped);
    if (enteringStopped) {
        taskbarEnabledBeforeStopped_ = taskbarEnabledForUserAction();
        cancelTaskbarDisplayRebuildRetry();
    }

    renderMode_ = nextMode;
    if (taskbarHost) {
        forEachHost(
            [&](TaskbarHost* host) { host->setRenderMode(static_cast<RenderMode>(renderMode_)); });
        applyEffectiveTaskbarSettings();
    }
    if (leavingStopped && taskbarEnabledBeforeStopped_) {
        if (taskbarHost) {
            // 切换期间可能没有新的 SMTC 帧，补交当前帧以保留进入停止模式
            // 前的会话可见性；实际显示仍等待宿主完成最新空间探测。
            forEachHost(
                [&](TaskbarHost* host) { host->applyPresentationFrame(presentation_.frame()); });
        } else {
            // 宿主可能在完全停止期间因任务栏重建或菜单操作被销毁，退出时
            // 按原先的开启状态静默重建，不走“手动开启”的提示流程。
            createTaskbar(GetModuleHandleW(nullptr));
        }
    }
    syncSpectrumWithMode();
    if (isMinimalRenderMode()) {
        cancelSongToastCoverWait();
        // 切歌弹窗是懒创建的；极简模式下直接销毁已创建的窗口和渲染资源，
        // 退出极简后下一次真正切歌时再按原配置懒创建。
        songToast_.reset();
    } else if (songToast_) {
        songToast_->setEnabled(settings_.songToastEnabled_);
    }
    if (isRenderMode(RenderMode::Stopped))
        cancelTaskbarAutoRestore();
    else if (taskbarAutoClosedForNoSpace_ && !taskbarHost)
        armTaskbarAutoRestore();
    refreshSettingsDialog(true);
    logSettingInt(L"render-mode", renderMode_);
    saveSettings();
}

void App::applyHoverControls(bool on) {
    if (taskbarExpandedConfigured() && !taskbarVertical_)
        return;
    settings_.hoverPlaybackControls_ = on;
    logSettingBool(L"hover-playback-controls", on);
    commitAppearanceSettings();
}

void App::applyTaskbarContextMenu(bool on) {
    settings_.taskbarContextMenuEnabled_ = on;
    logSettingBool(L"taskbar-context-menu", on);
    commitAppearanceSettings();
}

void App::applyHoverControlStyle(int style) {
    if (taskbarExpandedConfigured() && !taskbarVertical_)
        return;
    settings_.hoverControlStyle_ =
        style == 1 ? HoverControlStyle::Popup : HoverControlStyle::Inline;
    logSettingInt(L"hover-control-style", style == 1 ? 1 : 0);
    commitAppearanceSettings();
}

void App::applyFloatingCardTrigger(int mode) {
    settings_.floatingCardTrigger_ =
        mode == 1 ? MediaPopupTrigger::Click : MediaPopupTrigger::Hover;
    logSettingInt(L"floating-card-trigger", mode == 1 ? 1 : 0);
    commitAppearanceSettings();
}

void App::applyFloatingCardBackground(int mode) {
    settings_.floatingCardBackground_ =
        mode == 1 ? MediaPopupBackground::Frosted : MediaPopupBackground::Solid;
    logSettingInt(L"floating-card-background", mode == 1 ? 1 : 0);
    commitAppearanceSettings();
}

void App::applyFloatingCardFollowAlbum(bool on) {
    settings_.floatingCardFollowAlbum_ = on;
    logSettingBool(L"floating-card-follow-album", on);
    commitAppearanceSettings();
}

void App::applyFloatingCardAutoTextContrast(bool on) {
    settings_.floatingCardAutoTextContrast_ = on;
    logSettingBool(L"floating-card-auto-text-contrast", on);
    commitAppearanceSettings();
}

void App::applySongToastEnabled(bool on) {
    settings_.songToastEnabled_ = on;
    if (isMinimalRenderMode()) {
        cancelSongToastCoverWait();
        songToast_.reset();
    } else if (songToast_) {
        songToast_->setEnabled(on);
    }
    logSettingBool(L"song-toast", on);
    saveSettings();
}

void App::applySongToastDuration(int seconds) {
    settings_.songToastDurationSec_ = std::clamp(seconds, 1, 10);
    if (songToast_)
        songToast_->setDurationSec(settings_.songToastDurationSec_);
    logSettingInt(L"song-toast-duration", settings_.songToastDurationSec_);
    saveSettings();
}

void App::applySongToastSkipFullscreen(bool on) {
    settings_.songToastSkipFullscreen_ = on;
    logSettingBool(L"song-toast-skip-fullscreen", on);
    saveSettings();
}

void App::applySongToastFollowMouseMonitor(bool follow) {
    settings_.songToastFollowMouseMonitor_ = follow;
    if (songToast_)
        songToast_->setFollowMouseMonitor(follow);
    logSettingBool(L"song-toast-follow-mouse-monitor", follow);
    saveSettings();
}

void App::applySongToastPosition(int position) {
    settings_.songToastTop_ = position == 0;
    if (songToast_)
        songToast_->setPlacementTop(settings_.songToastTop_);
    logSettingInt(L"song-toast-position", settings_.songToastTop_ ? 0 : 1);
    saveSettings();
}

bool App::foregroundFullscreen() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == GetShellWindow())
        return false;
    wchar_t className[64]{};
    if (!GetClassNameW(foreground, className, static_cast<int>(std::size(className))))
        return false;
    static const wchar_t* shellClasses[] = {
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
    };
    for (const wchar_t* shell : shellClasses) {
        if (wcscmp(className, shell) == 0)
            return false;
    }
    RECT rc{};
    if (!GetWindowRect(foreground, &rc))
        return false;
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return false;
    const RECT& mon = info.rcMonitor;
    return rc.left <= mon.left && rc.top <= mon.top && rc.right >= mon.right &&
           rc.bottom >= mon.bottom;
}

void App::cancelSongToastCoverWait() {
    if (songToastCoverWaitArmed_ && trayHwnd)
        KillTimer(trayHwnd, kTimerSongToastCover);
    songToastCoverWaitArmed_ = false;
}

void App::notifySongToast() {
    if (isMinimalRenderMode() || !settings_.songToastEnabled_ ||
        presentation_.frame().media.title.empty())
        return;
    if (settings_.songToastSkipFullscreen_ && foregroundFullscreen())
        return;
    if (!songToast_) {
        auto toast = std::make_unique<SongToast>();
        if (!toast->create(GetModuleHandleW(nullptr)))
            return;
        toast->setEnabled(true);
        toast->setDurationSec(settings_.songToastDurationSec_);
        toast->setPlacementTop(settings_.songToastTop_);
        toast->setFollowMouseMonitor(settings_.songToastFollowMouseMonitor_);
        songToast_ = std::move(toast);
    }
    songToast_->showSong(presentation_.frame().media);
    runtime_log::writef(L"[action][song-toast] shown title=%s artist=%s",
                        presentation_.frame().media.title.c_str(),
                        presentation_.frame().media.artist.c_str());
}

void App::refreshThemeWindows() {
    auto refresh = [](HWND hwnd) {
        if (hwnd) {
            SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
            app_icon::applyWindowIcon(hwnd);
        }
    };
    refresh(settingsDialog ? settingsDialog->hwnd() : nullptr);
    refresh(aboutDialog ? aboutDialog->hwnd() : nullptr);
    refresh(manualSearchDialog ? manualSearchDialog->hwnd() : nullptr);
    refresh(fontPickerDialog ? fontPickerDialog->hwnd() : nullptr);
    if (fontColorDialog) {
        fontColorDialog->refreshTheme();
    }
    refresh(runtimeLogDialog ? runtimeLogDialog->hwnd() : nullptr);
    refresh(updatePromptHwnd_);
}

void App::refreshTheme() {
    fluent::setThemeModes(settings_.taskbarThemeMode_, settings_.windowThemeMode_);
    updateTrayIcon();
    if (taskbarHost) {
        forEachHost([](TaskbarHost* host) { host->refreshTheme(); });
        applyFontAppearance();
        publishTaskbarAppearance();
    }
    refreshThemeWindows();
    refreshSettingsDialog(true);
}

void App::applyTaskbarTheme(fluent::ThemeMode mode) {
    settings_.taskbarThemeMode_ = mode;
    refreshTheme();
    logSettingInt(L"taskbar-theme", static_cast<int>(mode));
    saveSettings();
}

void App::applyWindowTheme(fluent::ThemeMode mode) {
    settings_.windowThemeMode_ = mode;
    refreshTheme();
    logSettingInt(L"window-theme", static_cast<int>(mode));
    saveSettings();
}

void App::applyFollowAlbum(bool on) {
    settings_.lyricFollowAlbum_ = on;
    if (on) {
        tryExtractAlbumColor(); // 立即用当前封面取色
        applyFontColors();      // 当前曲目已有缓存主色时也要立即刷新歌词
    } else {
        applyFontColors(); // 恢复配置色
    }
    logSettingBool(L"follow-album-color", on);
    saveSettings();
}

void App::applyDoubleLineLyrics(bool on) {
    settings_.doubleLineLyricsEnabled_ = on;
    logSettingBool(L"double-line-lyrics", on);
    commitAppearanceSettings();
}

void App::applyLyricAlignment(int alignment) {
    if (taskbarExpandedConfigured() && !taskbarVertical_)
        return;
    settings_.lyricAlignment_ = alignment == 1   ? LyricAlignment::Center
                                : alignment == 2 ? LyricAlignment::Right
                                                 : LyricAlignment::Left;
    logSettingInt(L"lyric-alignment", alignment == 1 ? 1 : alignment == 2 ? 2 : 0);
    commitAppearanceSettings();
}

void App::applyIdleQuoteAlignment(int alignment) {
    settings_.idleQuoteAlignment_ = alignment == 1   ? LyricAlignment::Center
                                    : alignment == 2 ? LyricAlignment::Right
                                                     : LyricAlignment::Left;
    logSettingInt(L"daily-quote-alignment", alignment == 1 ? 1 : alignment == 2 ? 2 : 0);
    commitAppearanceSettings();
}

void App::applyIdleQuoteBackground(int background) {
    const int normalized = std::clamp(background, 0, 4);
    settings_.idleQuoteBackground_ = static_cast<IdleQuoteBackground>(normalized);
    logSettingInt(L"daily-quote-background", normalized);
    commitAppearanceSettings();
}

void App::applyIdleQuoteBackgroundScope(int scope) {
    const int normalized = std::clamp(scope, 0, 3);
    settings_.idleQuoteBackgroundScope_ = static_cast<IdleQuoteBackgroundScope>(normalized);
    logSettingInt(L"taskbar-dynamic-background-scope", normalized);
    commitAppearanceSettings();
}

void App::applySecondaryEnabled(bool on) {
    settings_.secondaryLyricEnabled_ = on;
    enforceSecondaryLyricConstraint();
    logSettingBool(L"secondary-lyrics", on);
    commitAppearanceSettings();
}

void App::applyPreferRomanization(bool on) {
    settings_.preferRomanization_ = on;
    enforceSecondaryLyricConstraint();
    logSettingBool(L"prefer-romanization", on);
    commitAppearanceSettings();
}

void App::applySecondaryChoice(bool romanization) {
    // 从右键菜单选择具体类型同时代表开启辅助歌词，避免关闭后菜单只能停留在关闭状态。
    settings_.secondaryLyricEnabled_ = true;
    settings_.preferRomanization_ = romanization;
    enforceSecondaryLyricConstraint();
    logSettingBool(L"secondary-lyrics", true);
    logSettingBool(L"prefer-romanization", romanization);
    commitAppearanceSettings();
}

void App::applyQqLocalLyricsEnabled(bool on) {
    settings_.qqLocalLyricsEnabled_ = on;
    playback_.lyricProvider().setQqLocalLyricsConfig(settings_.qqLocalLyricsEnabled_,
                                                     settings_.qqLocalLyricsPath_);
    logSettingBool(L"qq-local-lyrics", on);
    saveSettings();
    playback_.reloadCurrentQqLyrics();
}

void App::applyQqLocalLyricsPersistOrder(bool on) {
    settings_.qqLocalLyricsPersistOrder_ = on;
    logSettingBool(L"qq-local-lyrics-persist-order", on);
    saveSettings();
}

void App::setIdleAppNamesVisible(bool show) {
    if (settings_.idleAppNamesVisible_ == show)
        return;
    settings_.idleAppNamesVisible_ = show;
    runtime_log::writef(L"[action][idle-entry] app-names-visibility show=%d", show ? 1 : 0);
    saveSettings();
    refreshSettingsDialog();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
}

void App::applyTickTickEnabled(bool enabled) {
    if (enabled && !idle_.hasTickTickToken()) {
        editTickTickApiToken(true);
        return;
    }
    if (!enabled)
        tickTickEnableAfterTokenSave_ = false;
    idle_.applyTickTickEnabled(enabled);
}

void App::loadSettings() {
    std::wstring dir = configDir();
    if (dir.empty())
        return;
    settingsPath_ = dir + L"\\settings.json";
    idle_.initializeCredentials();
    settings_.logDirectory_ = runtime_log::RuntimeLogger::defaultDirectory();
    bool migrateLegacyLogDirectory = false;
    playback_.lyricProvider().setManualOverrideDir(dir + L"\\manual_lyrics");
    playback_.lyricProvider().setQqLyricOrderDir(dir + L"\\manual_lyrics_settings");
    fluent::setThemeModes(settings_.taskbarThemeMode_, settings_.windowThemeMode_);
    try {
        auto loaded = settings_store::load(std::filesystem::path(settingsPath_));
        if (loaded.status == settings_store::LoadStatus::Missing)
            return;
        if (loaded.status == settings_store::LoadStatus::OpenError) {
            showSettingsError(L"load-open",
                              L"无法打开设置文件，可能是权限不足或文件正被其他程序占用。\n\n"
                              L"程序将继续使用当前可用设置。\n\n"
                              L"请检查 settings.json 的访问权限。\n\n"
                              L"设置文件路径：\n" +
                                  settingsPath_);
            return;
        }
        if (loaded.status == settings_store::LoadStatus::ReadError) {
            showSettingsError(L"load-read", L"读取设置文件时发生磁盘或文件系统错误。\n\n"
                                            L"程序将继续使用当前可用设置。\n\n"
                                            L"设置文件路径：\n" +
                                                settingsPath_);
            return;
        }
        if (loaded.status == settings_store::LoadStatus::ParseError) {
            showSettingsError(L"load-parse", L"设置文件内容无法解析，文件可能已经损坏。\n\n"
                                             L"程序将继续使用当前默认设置。\n\n"
                                             L"请修复或删除 settings.json 后重新启动。\n\n"
                                             L"设置文件路径：\n" +
                                                 settingsPath_);
            return;
        }
        settings_.readJson(loaded.document, idle_.hasTickTickToken(), migrateLegacyLogDirectory);
        playback_.lyricProvider().setQqLocalLyricsConfig(settings_.qqLocalLyricsEnabled_,
                                                         settings_.qqLocalLyricsPath_);
    } catch (const std::exception&) {
        showSettingsError(L"load-exception",
                          L"读取设置文件时发生异常，文件可能损坏或当前账户没有访问权限。\n\n"
                          L"程序将继续使用当前可用设置。\n\n"
                          L"设置文件路径：\n" +
                              settingsPath_);
    } catch (...) {
        showSettingsError(L"load-unknown-exception", L"读取设置文件时发生未知错误。\n\n"
                                                     L"程序将继续使用当前可用设置。\n\n"
                                                     L"设置文件路径：\n" +
                                                         settingsPath_);
    }
    idle_.finishSettingsLoad();
    if (migrateLegacyLogDirectory)
        saveSettings();
}

void App::saveSettings() {
    if (settingsPath_.empty())
        return;
    try {
        settings_store::save(std::filesystem::path(settingsPath_), settings_.toJson());
    } catch (const std::exception&) {
        showSettingsError(L"save-exception",
                          L"设置未能保存到磁盘。\n\n"
                          L"请检查磁盘空间、文件权限，或确认 settings.json 未被其他程序占用。\n\n"
                          L"设置文件路径：\n" +
                              settingsPath_);
    } catch (...) {
        showSettingsError(L"save-unknown-exception",
                          L"设置未能保存到磁盘，原因未知。\n\n"
                          L"请检查磁盘空间、文件权限，或确认 settings.json 未被其他程序占用。\n\n"
                          L"设置文件路径：\n" +
                              settingsPath_);
    }
    refreshSettingsDialog();
}

void App::initializeRuntimeLogger() {
    if (settings_.logDirectory_.empty())
        settings_.logDirectory_ = runtime_log::RuntimeLogger::defaultDirectory();
    runtimeLogger_.start(settings_.logDirectory_, settings_.logRetentionDays_);
    settings_.logDirectory_ = runtimeLogger_.directory();
    settings_.logRetentionDays_ = runtimeLogger_.retentionDays();
    updateRuntimeLogState(monitor.snapshot());
}

void App::setLogDirectory(const std::wstring& path) {
    if (path.empty())
        return;
    runtimeLogger_.setDirectory(path);
    settings_.logDirectory_ = runtimeLogger_.directory();
    saveSettings();
}

void App::setLogRetentionDays(int days) {
    settings_.logRetentionDays_ = std::clamp(days, 0, 3650);
    runtimeLogger_.setRetentionDays(settings_.logRetentionDays_);
    saveSettings();
}

void App::applyQqLocalLyricsPath(const std::wstring& selectedPath) {
    if (selectedPath.empty() || selectedPath == settings_.qqLocalLyricsPath_)
        return;
    runtime_log::writef(L"[action][qq-local-lyrics] directory-changed path=%s",
                        selectedPath.c_str());
    settings_.qqLocalLyricsPath_ = selectedPath;
    saveSettings();
    refreshSettingsDialog();
    if (settings_.qqLocalLyricsEnabled_) {
        playback_.lyricProvider().setQqLocalLyricsConfig(true, settings_.qqLocalLyricsPath_);
        playback_.reloadCurrentQqLyrics();
    }
}

LRESULT CALLBACK App::updatePromptWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app)
            app->updatePromptHwnd_ = h;
    }

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
    case WM_CREATE:
        app->updatePromptBackdrop_ = fluent::styleDialogWindow(h);
        app->updatePromptSurface_.initialize(h, app->updatePromptBackdrop_);
        app->layoutUpdatePrompt();
        return 0;
    case WM_SIZE:
        app->layoutUpdatePrompt();
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_DPICHANGED: {
        fluent::applyDialogDpiChange(h, wp, reinterpret_cast<RECT*>(lp), WS_CAPTION | WS_SYSMENU,
                                     WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, 500.0f, 280.0f);
        app->layoutUpdatePrompt();
        app->updatePromptSurface_.invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        app->updatePromptBackdrop_ = fluent::restyleDialogWindow(h, app->updatePromptBackdrop_);
        app->updatePromptSurface_.setBackdrop(app->updatePromptBackdrop_);
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(h, &ps);
        app->updatePromptSurface_.paint(
            hdc, app->updatePromptBackdrop_,
            [app](fluent::FluentDialogSurface::Painter& painter, float width, float height) {
                app->paintUpdatePrompt(painter, width, height);
            });
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        app->updatePromptSurface_.eraseBackground(reinterpret_cast<HDC>(wp),
                                                  app->updatePromptBackdrop_);
        return 1;
    case WM_MOUSEMOVE: {
        if (!GetCapture()) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        const float s = app->updatePromptSurface_.dipScale();
        const int id = app->hitTestUpdatePrompt(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        if (id != app->updatePromptHoverId_) {
            app->updatePromptHoverId_ = id;
            app->updatePromptSurface_.invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        app->updatePromptHoverId_ = 0;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        app->updatePromptFocusVisible_ = false;
        const float s = app->updatePromptSurface_.dipScale();
        app->updatePromptPressedId_ =
            app->hitTestUpdatePrompt(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        if (app->updatePromptPressedId_ != 0) {
            app->updatePromptFocusedId_ = app->updatePromptPressedId_;
            SetCapture(h);
        }
        app->updatePromptSurface_.invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float s = app->updatePromptSurface_.dipScale();
        const int hit = app->hitTestUpdatePrompt(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        const int pressed = app->updatePromptPressedId_;
        app->updatePromptPressedId_ = 0;
        if (GetCapture() == h)
            ReleaseCapture();
        if (pressed != 0 && pressed == hit)
            app->onUpdatePromptCommand(pressed);
        if (app->updatePromptHwnd_ == h)
            app->updatePromptSurface_.invalidate();
        return 0;
    }
    case WM_CAPTURECHANGED:
        app->updatePromptPressedId_ = 0;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTTAB;
    case WM_KEYDOWN:
        if (wp == VK_TAB) {
            app->focusStepUpdatePrompt((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            app->closeUpdatePrompt();
            return 0;
        }
        if (wp == VK_SPACE || wp == VK_RETURN) {
            app->onUpdatePromptCommand(app->updatePromptFocusedId_);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        app->updatePromptFocusVisible_ = true;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_KILLFOCUS:
        app->updatePromptFocusVisible_ = false;
        app->updatePromptSurface_.invalidate();
        return 0;
    case WM_CLOSE:
        app->closeUpdatePrompt();
        return 0;
    case WM_DESTROY:
        app->updatePromptSurface_.discard();
        app->updatePromptHwnd_ = nullptr;
        app->updatePromptHoverId_ = 0;
        app->updatePromptPressedId_ = 0;
        app->updatePromptLatestVersion_.clear();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

const LyricAppearance& App::currentLyricAppearance() const {
    if (settings_.lyricAppearanceGlobal_)
        return settings_.globalLyricAppearance_;
    return fluent::isDarkMode(fluent::ThemeTarget::Taskbar) ? settings_.darkLyricAppearance_
                                                            : settings_.lightLyricAppearance_;
}

COLORREF App::effectivePlayedColor() const {
    return (settings_.lyricFollowAlbum_ && playback_.state().hasAlbumColor_)
               ? playback_.state().albumColor_
               : currentLyricAppearance().played;
}

COLORREF App::effectiveFloatingCardBackgroundColor() const {
    return settings_.floatingCardBackgroundColorCustomized_ ? settings_.floatingCardBackgroundColor_
                                                            : defaultFloatingCardBackgroundColor();
}

void App::applyFloatingCardBackgroundColor(COLORREF color) {
    settings_.floatingCardBackgroundColor_ = color;
    settings_.floatingCardBackgroundColorCustomized_ = true;
    runtime_log::writef(L"[action][floating-card] background-color=#%02X%02X%02X", GetRValue(color),
                        GetGValue(color), GetBValue(color));
    commitAppearanceSettings();
}

void App::applyFontColors() { publishTaskbarAppearance(); }

void App::applyFontAppearance() { publishTaskbarAppearance(); }

void App::setAutoCheckOnStartup(bool enabled) {
    if (settings_.autoCheckOnStartup_ == enabled)
        return;
    settings_.autoCheckOnStartup_ = enabled;
    runtime_log::writef(L"[action][update] auto-check-on-startup=%s", enabled ? L"on" : L"off");
    saveSettings();
}

SettingsState App::currentSettingsState() const {
    SettingsState st;
    const bool vertical = taskbarVertical_;
    const TaskbarAppearance view = appearanceForHost(vertical);
    st.idleEntryEnabled = settings_.idleEntryEnabled_;
    st.idleQuoteEnabled = settings_.idleQuoteEnabled_;
    st.idleWelcomeEnabled = settings_.idleWelcomeEnabled_;
    st.idleQuoteSource = settings_.idleQuoteSource_ == IdleQuoteSource::Jinrishici ? 1
                         : settings_.idleQuoteSource_ == IdleQuoteSource::Netease  ? 2
                                                                                   : 0;
    st.idleQuoteRefreshInterval =
        settings_.idleQuoteRefreshInterval_ == IdleQuoteRefreshInterval::Hourly    ? 2
        : settings_.idleQuoteRefreshInterval_ == IdleQuoteRefreshInterval::HalfDay ? 1
                                                                                   : 0;
    st.idleQuoteBackground = idleQuoteBackgroundIndex(settings_.idleQuoteBackground_);
    st.idleQuoteBackgroundScope =
        idleQuoteBackgroundScopeIndex(settings_.idleQuoteBackgroundScope_);
    st.idleAppNamesVisible = settings_.idleAppNamesVisible_;
    st.idleApps = settings_.idleApps_;
    st.tickTickEnabled = idle_.tickTickEffectiveEnabled();
    st.tickTickApiTokenConfigured = idle_.hasTickTickToken();
    st.tickTickConnected = idle_.state().tickTickConnected_;
    st.tickTickConnecting = idle_.state().tickTickConnecting_;
    st.tickTickSyncing =
        idle_.state().tickTickTasksLoading_ || !idle_.state().tickTickCompletingTaskId_.empty();
    st.tickTickStatus = idle_.state().tickTickStatus_;
    st.verticalTaskbar = vertical;
    st.taskbarViewMode = static_cast<int>(settings_.taskbarViewMode_);
    if (settings_.taskbarViewMode_ == TaskbarViewMode::Embedded)
        st.taskbarMonitorLabel = L"主显示器";
    else
        st.taskbarMonitorLabel = taskbarMonitorLabel();
    st.mediaSessionAlive = monitor.snapshot().sessionAlive;
    st.appBarEdge = settings_.appBarEdge_ == AppBarEdge::Bottom ? 1 : 0;
    st.immersiveBackgroundBlur = settings_.immersiveBackgroundBlur_;
    st.dockBackgroundBlur = settings_.dockBackgroundBlur_;
    st.immersiveBackgroundAdjustment = settings_.immersiveBackgroundAdjustment_;
    st.dockBackgroundAdjustment = settings_.dockBackgroundAdjustment_;
    st.backgroundBlurSupported = !taskbarHost || taskbarHost->backgroundBlurAvailable();
    st.embeddedResourceMonitor = settings_.embeddedResourceMonitor_;
    st.dockResourceGpuUsage = settings_.dockResourceGpuUsage_;
    st.dockResourceDownload = settings_.dockResourceDownload_;
    st.dockResourceUpload = settings_.dockResourceUpload_;
    st.dockResourceCpuFrequency = settings_.dockResourceCpuFrequency_;
    st.songInfoVisible = view.songInfoVisible;
    st.albumCoverVisible = settings_.albumCoverVisible_;
    st.platformIconVisible = settings_.platformIconVisible_;
    st.coverEffectVinyl = settings_.albumCoverEffect_ == AlbumCoverEffect::Vinyl;
    st.spectrumOn = vertical ? false : settings_.spectrumOn_;
    st.spectrumStyle =
        spectrumStyleIndex(vertical ? SpectrumStyle::Default : settings_.spectrumStyle_);
    st.spectrumColor = settings_.spectrumColor_;
    st.spectrumCustomColor = view.spectrumColor.customized;
    st.spectrumFollowAlbum = view.spectrumColor.followAlbum;
    st.spectrumGradient = view.spectrumGradient;
    st.spectrumEffects = settings_.spectrumEffects_;
    st.spectrumBackground = vertical ? false : settings_.spectrumBackground_;
    st.spectrumOpacity = settings_.spectrumOpacity_;
    st.progressBackground = settings_.progressBackground_;
    st.progressBackgroundOpacity = settings_.progressBackgroundOpacity_;
    st.taskbarBackground = settings_.taskbarBackground_;
    st.coverBackgroundOpacity = settings_.coverBackgroundOpacity_;
    st.renderMode = renderMode_;
    st.taskbarContextMenu = settings_.taskbarContextMenuEnabled_;
    st.hoverControls = settings_.hoverPlaybackControls_;
    st.hoverControlStyle = settings_.hoverControlStyle_ == HoverControlStyle::Popup ? 1 : 0;
    st.floatingCardTrigger = settings_.floatingCardTrigger_ == MediaPopupTrigger::Click ? 1 : 0;
    st.floatingCardBackground =
        settings_.floatingCardBackground_ == MediaPopupBackground::Frosted ? 1 : 0;
    st.floatingCardBackgroundColor = effectiveFloatingCardBackgroundColor();
    st.floatingCardFollowAlbum = settings_.floatingCardFollowAlbum_;
    st.floatingCardAutoTextContrast = settings_.floatingCardAutoTextContrast_;
    st.songToastEnabled = settings_.songToastEnabled_;
    st.songToastDurationSec = settings_.songToastDurationSec_;
    st.songToastSkipFullscreen = settings_.songToastSkipFullscreen_;
    st.songToastPosition = settings_.songToastTop_ ? 0 : 1;
    st.songToastFollowMouseMonitor = settings_.songToastFollowMouseMonitor_;
    st.songToastPreviewAvailable =
        st.mediaSessionAlive && !presentation_.frame().media.title.empty();
    st.taskbarThemeMode = settings_.taskbarThemeMode_;
    st.windowThemeMode = settings_.windowThemeMode_;
    st.followAlbum = settings_.lyricFollowAlbum_;
    st.doubleLineLyrics = view.doubleLineLyrics;
    st.compactTaskbar = taskbarHost && taskbarHost->isCompactTaskbar();
    st.lyricEffects = settings_.lyricEffects_;
    st.lyricAlignment = static_cast<int>(view.lyricAlignment);
    st.idleQuoteAlignment = static_cast<int>(view.idleQuoteAlignment);
    st.secondaryEnabled =
        view.secondaryLyricMode.translation || view.secondaryLyricMode.romanization;
    st.preferRomanization = vertical ? false : settings_.preferRomanization_;
    st.secondaryAvailability =
        playback_.state().lyricLoading_ ? SecondaryLyricAvailability::Loading
        : (!playback_.state().currentHasTranslation_ && !playback_.state().currentHasRomanization_)
            ? SecondaryLyricAvailability::Unavailable
            : SecondaryLyricAvailability::Available;
    st.qqLocalLyricsEnabled = settings_.qqLocalLyricsEnabled_;
    st.qqLocalLyricsPersistOrder = settings_.qqLocalLyricsPersistOrder_;
    st.qqLocalLyricsPath = settings_.qqLocalLyricsPath_;
    st.fontDesc = settings_.fontFamily_ + L", " +
                  std::to_wstring(static_cast<int>(std::lround(settings_.fontSize_))) + L"px, " +
                  fontStyleLabel(settings_.fontStyle_);
    return st;
}

SettingsActions App::buildSettingsActions() {
    SettingsActions act;
    act.onOpenDocs = [] {
        ShellExecuteW(nullptr, L"open", L"https://www.taskbar-lyrics.top/docs.html", nullptr,
                      nullptr, SW_SHOWNORMAL);
    };
    act.onOpenFeedback = [] {
        ShellExecuteW(nullptr, L"open", L"https://www.taskbar-lyrics.top/feedback.html", nullptr,
                      nullptr, SW_SHOWNORMAL);
    };
    act.onOpenAbout = [this] { showAbout(); };
    act.onIdleEntryEnabled = [this](bool on) { idle_.applyIdleEntryEnabled(on); };
    act.onIdleTextMode = [this](int mode) { idle_.applyIdleTextMode(mode); };
    act.onIdleQuoteSource = [this](int source) { idle_.applyIdleQuoteSource(source); };
    act.onIdleQuoteRefreshInterval = [this](int interval) {
        idle_.applyIdleQuoteRefreshInterval(interval);
    };
    act.onIdleQuoteBackground = [this](int background) { applyIdleQuoteBackground(background); };
    act.onIdleQuoteBackgroundScope = [this](int scope) { applyIdleQuoteBackgroundScope(scope); };
    act.onEditIdleWelcome = [this] { editIdleWelcome(); };
    act.onAddIdleApp = [this] { pickIdleApp(); };
    act.onEditIdleApp = [this](int index) { editIdleApp(index); };
    act.onIdleAppNamesVisible = [this](bool show) { setIdleAppNamesVisible(show); };
    act.onRemoveIdleApp = [this](int index) { removeIdleApp(index); };
    act.onReorderIdleApps = [this](int fromIndex, int toIndex) {
        reorderIdleApps(fromIndex, toIndex);
    };
    act.onSongInfoVisible = [this](bool on) { applySongInfoVisible(on); };
    act.onAlbumCoverVisible = [this](bool on) { applyAlbumCoverVisible(on); };
    act.onPlatformIconVisible = [this](bool on) { applyPlatformIconVisible(on); };
    act.onCoverEffectVinyl = [this](bool vinyl) { applyCoverEffect(vinyl); };
    act.onTaskbarViewMode = [this](int mode) { applyTaskbarViewMode(mode); };
    act.onChooseTaskbarMonitor = [this] { showTaskbarMonitorMenu(); };
    act.onAppBarEdge = [this](int edge) { applyAppBarEdge(edge); };
    act.onImmersiveBackgroundBlur = [this](int percent) { applyImmersiveBackgroundBlur(percent); };
    act.onDockBackgroundBlur = [this](int percent) { applyDockBackgroundBlur(percent); };
    act.onImmersiveBackgroundAdjustment = [this](int mode) {
        applyImmersiveBackgroundAdjustment(mode);
    };
    act.onDockBackgroundAdjustment = [this](int mode) { applyDockBackgroundAdjustment(mode); };
    act.onEmbeddedResourceMonitor = [this](bool on) { applyEmbeddedResourceMonitor(on); };
    act.onDockResourceGpuUsage = [this](bool on) { applyDockResourceGpuUsage(on); };
    act.onDockResourceDownload = [this](bool on) { applyDockResourceNetwork(true, on); };
    act.onDockResourceUpload = [this](bool on) { applyDockResourceNetwork(false, on); };
    act.onDockResourceCpuFrequency = [this](bool on) { applyDockResourceCpuFrequency(on); };
    act.onSpectrum = [this](bool on) { applySpectrumOn(on); };
    act.onSpectrumStyle = [this](int style) { applySpectrumStyle(style); };
    act.onSpectrumColorMode = [this](int mode) { applySpectrumColorMode(mode); };
    act.onSpectrumColor = [this](COLORREF color) { applySpectrumColor(color); };
    act.onSpectrumGradient = [this](bool on) { applySpectrumGradient(on); };
    act.onSpectrumEffect = [this](int effect, bool on) { applySpectrumEffect(effect, on); };
    act.onSpectrumBackground = [this](bool on) { applySpectrumBackground(on); };
    act.onSpectrumOpacity = [this](int percent) { applySpectrumOpacity(percent); };
    act.onProgressBackground = [this](bool on) { applyProgressBackground(on); };
    act.onProgressBackgroundOpacity = [this](int percent) {
        applyProgressBackgroundOpacity(percent);
    };
    act.onTaskbarBackground = [this](int mode) { applyTaskbarBackground(mode); };
    act.onCoverBackgroundOpacity = [this](int percent) { applyCoverBackgroundOpacity(percent); };
    act.onRenderMode = [this](int mode) { applyRenderMode(mode); };
    act.onTaskbarContextMenu = [this](bool on) { applyTaskbarContextMenu(on); };
    act.onHoverControls = [this](bool on) { applyHoverControls(on); };
    act.onHoverControlStyle = [this](int style) { applyHoverControlStyle(style); };
    act.onFloatingCardTrigger = [this](int mode) { applyFloatingCardTrigger(mode); };
    act.onFloatingCardBackground = [this](int mode) { applyFloatingCardBackground(mode); };
    act.onFloatingCardBackgroundColor = [this](COLORREF color) {
        applyFloatingCardBackgroundColor(color);
    };
    act.onFloatingCardFollowAlbum = [this](bool on) { applyFloatingCardFollowAlbum(on); };
    act.onFloatingCardAutoTextContrast = [this](bool on) { applyFloatingCardAutoTextContrast(on); };
    act.onSongToastEnabled = [this](bool on) { applySongToastEnabled(on); };
    act.onSongToastDuration = [this](int seconds) { applySongToastDuration(seconds); };
    act.onSongToastSkipFullscreen = [this](bool on) { applySongToastSkipFullscreen(on); };
    act.onSongToastPosition = [this](int position) { applySongToastPosition(position); };
    act.onSongToastFollowMouseMonitor = [this](bool follow) {
        applySongToastFollowMouseMonitor(follow);
    };
    act.onPreviewSongToast = [this] { notifySongToast(); };
    act.onTaskbarTheme = [this](fluent::ThemeMode mode) { applyTaskbarTheme(mode); };
    act.onWindowTheme = [this](fluent::ThemeMode mode) { applyWindowTheme(mode); };
    act.onPickFont = [this] { pickFont(); };
    act.onFontColorEffect = [this] { showFontColorDialog(); };
    act.onFollowAlbum = [this](bool on) { applyFollowAlbum(on); };
    act.onDoubleLineLyrics = [this](bool on) { applyDoubleLineLyrics(on); };
    act.onLyricEffect = [this](int effect, bool on) { applyLyricEffect(effect, on); };
    act.onLyricAlignment = [this](int a) { applyLyricAlignment(a); };
    act.onIdleQuoteAlignment = [this](int a) { applyIdleQuoteAlignment(a); };
    act.onSecondaryEnabled = [this](bool on) { applySecondaryEnabled(on); };
    act.onPreferRomanization = [this](bool on) { applyPreferRomanization(on); };
    act.onTickTickEnabled = [this](bool on) { applyTickTickEnabled(on); };
    act.onEditTickTickApiToken = [this] { editTickTickApiToken(); };
    act.onTickTickConnect = [this] { idle_.connectTickTick(); };
    act.onTickTickRefresh = [this] { idle_.refreshTickTickTasks(); };
    act.onTickTickDisconnect = [this] { disconnectTickTick(); };
    act.onQqLocalLyricsEnabled = [this](bool on) { applyQqLocalLyricsEnabled(on); };
    act.onQqLocalLyricsPersistOrder = [this](bool on) { applyQqLocalLyricsPersistOrder(on); };
    act.onPickQqLocalLyricsPath = [this] { pickQqLocalLyricsPath(); };
    return act;
}
} // namespace lyrics_app
