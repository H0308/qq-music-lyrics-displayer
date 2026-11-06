#include "app/application_internal.h"
namespace lyrics_app {
bool App::createTrayWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = App::trayWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricTray";
    RegisterClassExW(&wc);

    // 必须是不可见的顶层窗口（不能用 HWND_MESSAGE）：系统广播（如 Explorer 重启时的
    // TaskbarCreated）只投递给顶层窗口，message-only 窗口收不到，任务栏歌词将无法恢复
    trayHwnd = CreateWindowExW(0, L"QQMusicLyricTray", L"QQMusicLyricTray", 0, 0, 0, 0, 0, nullptr,
                               nullptr, inst, this);
    if (!trayHwnd)
        return false;
    idle_.setTimerWindow(trayHwnd);
    playback_.setTimerWindow(trayHwnd);
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    updateTrayIcon();
    SetTimer(trayHwnd, kTimerIdleQuote, kIdleQuoteCheckMs, nullptr);
    return true;
}

void App::destroyTray() {
    if (trayHwnd) {
        HWND hwnd = trayHwnd;
        trayHwnd = nullptr;
        KillTimer(hwnd, kTimerIdleQuote);
        KillTimer(hwnd, kTimerTickTickStartupRetry);
        KillTimer(hwnd, kTimerTaskbarAutoRestore);
        taskbarAutoRestorePending_ = false;
        idle_.resetTickTickStartupRetry();
        idle_.setTimerWindow(nullptr);
        playback_.setTimerWindow(nullptr);
        // 先关闭可能打开的 Fluent 菜单（其窗口由托盘窗口所有）
        fluent::FluentMenu::dismiss();
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
    }
}

void App::updateTrayIcon() {
    if (!trayHwnd)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = app_icon::taskbarIcon();
    lstrcpyW(nid.szTip, L"QQ 音乐歌词");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    // 首次创建时 MODIFY 不会生效，用 ADD
    Shell_NotifyIconW(NIM_ADD, &nid);
}

std::vector<fluent::FluentMenuItem> App::buildMenuItems(bool fullTrayMenu) {
    std::vector<fluent::FluentMenuItem> items;
    auto addItem = [&items](int id, const wchar_t* text, settings_icon::Kind icon,
                            bool checked = false, bool enabled = true) {
        fluent::FluentMenuItem it;
        it.id = id;
        it.text = text;
        it.icon = icon;
        it.checked = checked;
        it.enabled = enabled;
        items.push_back(std::move(it));
    };
    auto addSeparator = [&items] {
        fluent::FluentMenuItem it;
        it.separator = true;
        items.push_back(std::move(it));
    };

    const SmtcSnapshot snap = monitor.snapshot();
    const bool taskbarEnabled = taskbarEnabledForUserAction();
    addItem(kCmdToggleTaskbar, taskbarEnabled ? L"关闭任务栏歌词" : L"开启任务栏歌词",
            settings_icon::Kind::Display);
    fluent::FluentMenuItem performance;
    performance.text = L"性能模式";
    performance.icon = settings_icon::Kind::Performance;
    auto addRenderMode = [this, &performance](int id, const wchar_t* text, RenderMode mode) {
        fluent::FluentMenuItem it;
        it.id = id;
        it.text = text;
        it.icon = settings_icon::Kind::RenderMode;
        it.checked = isRenderMode(mode);
        performance.submenu.push_back(std::move(it));
    };
    addRenderMode(kCmdRenderModeNormal, L"正常", RenderMode::Normal);
    addRenderMode(kCmdRenderModeLow, L"低渲染", RenderMode::Low);
    addRenderMode(kCmdRenderModeStopped, L"完全停止", RenderMode::Stopped);
    addRenderMode(kCmdRenderModeMinimal, L"极简", RenderMode::Minimal);
    items.push_back(std::move(performance));
    if (taskbarEnabled) {
        if (settings_.taskbarViewMode_ == TaskbarViewMode::Embedded) {
            fluent::FluentMenuItem pos;
            pos.text = L"任务栏位置";
            pos.icon = settings_icon::Kind::Position;
            fluent::FluentMenuItem sub;
            sub.id = kCmdTaskbarPosNotify;
            sub.text = L"通知区域左侧";
            sub.icon = settings_icon::Kind::Position;
            sub.checked = settings_.taskbarPosition_ == 0;
            pos.submenu.push_back(sub);
            sub.id = kCmdTaskbarPosLeft;
            sub.text = L"任务栏最左侧";
            sub.checked = settings_.taskbarPosition_ == 1;
            pos.submenu.push_back(sub);
            items.push_back(std::move(pos));
        }
        fluent::FluentMenuItem viewMode;
        viewMode.text = L"展示模式";
        viewMode.icon = settings_icon::Kind::Display;
        auto addViewMode = [this, &viewMode](int id, const wchar_t* text, TaskbarViewMode mode,
                                             bool enabled = true) {
            fluent::FluentMenuItem sub;
            sub.id = id;
            sub.text = text;
            sub.icon = settings_icon::Kind::Display;
            sub.checked = settings_.taskbarViewMode_ == mode;
            sub.enabled = enabled;
            viewMode.submenu.push_back(std::move(sub));
        };
        addViewMode(kCmdTaskbarModeEmbedded, L"内嵌", TaskbarViewMode::Embedded);
        if (snap.sessionAlive) {
            addViewMode(kCmdTaskbarModeImmersive, L"沉浸", TaskbarViewMode::Immersive,
                        !taskbarVertical_);
        }
        addViewMode(kCmdTaskbarModeAppBar, L"Dock 模式", TaskbarViewMode::AppBar);
        items.push_back(std::move(viewMode));
        if (settings_.taskbarViewMode_ == TaskbarViewMode::AppBar) {
            fluent::FluentMenuItem edge;
            edge.text = L"Dock 位置";
            edge.icon = settings_icon::Kind::Position;
            fluent::FluentMenuItem sub;
            sub.id = kCmdAppBarTop;
            sub.text = L"顶部";
            sub.icon = settings_icon::Kind::Position;
            sub.checked = settings_.appBarEdge_ == AppBarEdge::Top;
            edge.submenu.push_back(sub);
            sub.id = kCmdAppBarBottom;
            sub.text = L"底部";
            sub.checked = settings_.appBarEdge_ == AppBarEdge::Bottom;
            edge.submenu.push_back(sub);
            items.push_back(std::move(edge));
        }
    }
    const bool canSwitchLyricSource =
        snap.sessionAlive && snap.player == SmtcPlayerType::QQMusic &&
        !playback_.state().lyricLoading_ && !playback_.state().currentLyrics_.empty() &&
        settings_.qqLocalLyricsEnabled_ && !settings_.qqLocalLyricsPath_.empty() &&
        !playback_.state().currentLyricsFromManual_;
    addItem(kCmdManualSearch, L"手动搜索歌词", settings_icon::Kind::Search);
    // 歌词来源切换与当前播放内容直接相关，托盘菜单和任务栏歌词右键菜单都应提供。
    if (canSwitchLyricSource) {
        addItem(kCmdSwitchLyricSource,
                playback_.state().currentLyricsFromLocal_ ? L"切换到在线版歌词"
                                                          : L"切换到本地版歌词",
                settings_icon::Kind::LocalLyrics);
    }
    if (playback_.state().currentHasTranslation_ || playback_.state().currentHasRomanization_) {
        fluent::FluentMenuItem secondary;
        secondary.text = L"辅助歌词";
        secondary.icon = settings_icon::Kind::Lyrics;
        fluent::FluentMenuItem translation;
        translation.id = kCmdSecondaryTranslation;
        translation.text = L"翻译";
        translation.icon = settings_icon::Kind::Language;
        translation.checked = settings_.secondaryLyricEnabled_ && !settings_.preferRomanization_;
        secondary.submenu.push_back(std::move(translation));

        fluent::FluentMenuItem romanization;
        romanization.id = kCmdSecondaryRomanization;
        romanization.text = L"罗马音";
        romanization.icon = settings_icon::Kind::Language;
        romanization.checked = settings_.secondaryLyricEnabled_ && settings_.preferRomanization_;
        secondary.submenu.push_back(std::move(romanization));

        fluent::FluentMenuItem secondaryOff;
        secondaryOff.id = kCmdSecondaryOff;
        secondaryOff.text = L"关闭显示辅助歌词";
        secondaryOff.icon = settings_icon::Kind::Disconnect;
        secondaryOff.checked = !settings_.secondaryLyricEnabled_;
        secondary.submenu.push_back(std::move(secondaryOff));
        items.push_back(std::move(secondary));
    }
    addSeparator();
    if (fullTrayMenu)
        addItem(kCmdRuntimeLog, L"运行日志", settings_icon::Kind::Log);
    addItem(kCmdSettings, L"设置…", settings_icon::Kind::Settings);
    if (fullTrayMenu) {
        addItem(kCmdAutoStart, L"开机自启动", settings_icon::Kind::Trigger, autoStartEnabled());
        addSeparator();
        addItem(kCmdAbout, L"检查更新…", settings_icon::Kind::Download);
        items.back().rightText = app_info::kVersion;
        addItem(kCmdRestart, L"重启应用", settings_icon::Kind::Restart);
        addItem(kCmdExit, L"退出", settings_icon::Kind::Exit);
    }
    return items;
}

void App::showTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    showTrayMenu(pt);
}

void App::showTrayMenu(POINT screenPt) {
    fluent::FluentMenu::show(trayHwnd, screenPt, buildMenuItems(true),
                             [this](int cmd) { onMenuCommand(cmd, L"tray"); });
}

void App::showTaskbarMenu(POINT screenPt) {
    fluent::FluentMenu::show(trayHwnd, screenPt, buildMenuItems(false),
                             [this](int cmd) { onMenuCommand(cmd, L"taskbar-menu"); });
}

std::vector<fluent::FluentMenuItem> App::buildDockQuickAppItems() {
    dockQuickAppActions_.clear();

    std::vector<fluent::FluentMenuItem> items;
    fluent::FluentMenuItem header;
    header.text = L"快捷应用";
    header.icon = settings_icon::Kind::QuickLaunch;
    header.enabled = false;
    items.push_back(std::move(header));

    fluent::FluentMenuItem separator;
    separator.separator = true;
    items.push_back(std::move(separator));

    for (const auto& app : settings_.idleApps_) {
        fluent::FluentMenuItem item;
        item.id = kDockQuickAppCommandBase + static_cast<int>(dockQuickAppActions_.size());
        item.text = app.name.empty() ? fallbackExeName(app.path) : app.name;
        item.icon = settings_icon::Kind::QuickLaunch;
        item.nativeIcon = shellIconForPath(app.path);
        item.enabled = app.pathValid;
        items.push_back(std::move(item));
        dockQuickAppActions_.push_back(app.path);
    }

    if (dockQuickAppActions_.empty()) {
        fluent::FluentMenuItem empty;
        empty.text = L"请在“每日一言与应用速启”中添加应用";
        empty.enabled = false;
        items.push_back(std::move(empty));
    }
    return items;
}

void App::showDockQuickApps(POINT screenPt) {
    fluent::FluentMenu::show(trayHwnd, screenPt, buildDockQuickAppItems(),
                             [this](int command) { onDockQuickAppCommand(command); });
}

void App::onDockQuickAppCommand(int command) {
    const int index = command - kDockQuickAppCommandBase;
    if (index < 0 || static_cast<size_t>(index) >= dockQuickAppActions_.size())
        return;
    launchIdleApp(dockQuickAppActions_[static_cast<size_t>(index)]);
}

void App::launchIdleApp(const std::wstring& path) {
    const bool ok = platform_icon::launchConfiguredExe(path);
    runtime_log::writef(L"[action][idle-entry] app-open path=%s result=%s", path.c_str(),
                        ok ? L"ok" : L"failed");
    if (!ok) {
        for (auto& app : settings_.idleApps_) {
            if (_wcsicmp(app.path.c_str(), path.c_str()) == 0) {
                app.pathValid = validExePath(app.path);
                break;
            }
        }
        refreshSettingsDialog();
        publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
    }
}

void App::onMenuCommand(int cmd, const wchar_t* source) {
    runtime_log::writef(L"[action][%s] command=%s id=%d", source, trayCommandName(cmd), cmd);
    switch (cmd) {
    case kCmdToggleTaskbar:
        toggleTaskbar();
        saveSettings();
        break;
    case kCmdRenderModeNormal:
        applyRenderMode(static_cast<int>(RenderMode::Normal));
        break;
    case kCmdRenderModeLow:
        applyRenderMode(static_cast<int>(RenderMode::Low));
        break;
    case kCmdRenderModeStopped:
        applyRenderMode(static_cast<int>(RenderMode::Stopped));
        break;
    case kCmdRenderModeMinimal:
        applyRenderMode(static_cast<int>(RenderMode::Minimal));
        break;
    case kCmdTaskbarPosNotify:
    case kCmdTaskbarPosLeft:
        if (settings_.taskbarViewMode_ != TaskbarViewMode::Embedded)
            break;
        settings_.taskbarPosition_ = cmd == kCmdTaskbarPosLeft ? 1 : 0;
        logSettingInt(L"taskbar-position", settings_.taskbarPosition_);
        forEachHost([&](TaskbarHost* host) { host->setPositionMode(settings_.taskbarPosition_); });
        saveSettings();
        break;
    case kCmdTaskbarModeEmbedded:
        applyTaskbarViewMode(0);
        break;
    case kCmdTaskbarModeImmersive:
        if (!taskbarVertical_ && monitor.snapshot().sessionAlive)
            applyTaskbarViewMode(1);
        break;
    case kCmdTaskbarModeAppBar:
        applyTaskbarViewMode(2);
        break;
    case kCmdAppBarTop:
        applyAppBarEdge(0);
        break;
    case kCmdAppBarBottom:
        applyAppBarEdge(1);
        break;
    case kCmdSpectrum:
        applySpectrumOn(!settings_.spectrumOn_);
        break;
    case kCmdHoverPlaybackControls:
        applyHoverControls(!settings_.hoverPlaybackControls_);
        break;
    case kCmdPickFont:
        pickFont();
        break;
    case kCmdFontColorEffect:
        showFontColorDialog();
        break;
    case kCmdFollowAlbum:
        applyFollowAlbum(!settings_.lyricFollowAlbum_);
        break;
    case kCmdManualSearch:
        showManualSearch(GetModuleHandleW(nullptr));
        break;
    case kCmdSwitchLyricSource:
        if (playback_.state().currentLyricsFromLocal_)
            playback_.reloadCurrentQqLyrics(true, false, settings_.qqLocalLyricsPersistOrder_);
        else if (!playback_.state().currentLyricsFromManual_)
            playback_.reloadCurrentQqLyrics(false, true, settings_.qqLocalLyricsPersistOrder_);
        break;
    case kCmdRuntimeLog:
        showRuntimeLog();
        break;
    case kCmdSecondaryTranslation:
        applySecondaryChoice(false);
        break;
    case kCmdSecondaryOff:
        applySecondaryEnabled(false);
        break;
    case kCmdDoubleLineLyrics:
        applyDoubleLineLyrics(!settings_.doubleLineLyricsEnabled_);
        break;
    case kCmdLyricAlignLeft:
    case kCmdLyricAlignCenter:
    case kCmdLyricAlignRight:
        applyLyricAlignment(cmd == kCmdLyricAlignCenter ? 1 : cmd == kCmdLyricAlignRight ? 2 : 0);
        break;
    case kCmdSongInfo:
        applySongInfoVisible(!settings_.songInfoVisible_);
        break;
    case kCmdAlbumCover:
        applyAlbumCoverVisible(!settings_.albumCoverVisible_);
        break;
    case kCmdPlatformIcon:
        applyPlatformIconVisible(!settings_.platformIconVisible_);
        break;
    case kCmdAlbumCoverEffectDefault:
    case kCmdAlbumCoverEffectVinyl:
        applyCoverEffect(cmd == kCmdAlbumCoverEffectVinyl);
        break;
    case kCmdSecondaryRomanization:
        applySecondaryChoice(true);
        break;
    case kCmdSettings:
        showSettings();
        break;
    case kCmdAutoStart: {
        bool enable = !autoStartEnabled();
        if (setAutoStart(enable))
            runtime_log::writef(L"[autostart] %s", enable ? L"enabled" : L"disabled");
        else
            runtime_log::writef(L"[autostart] failed to %s", enable ? L"enable" : L"disable");
        break;
    }
    case kCmdAbout:
        showAbout();
        break;
    case kCmdExit:
        requestQuit();
        break;
    case kCmdRestart:
        restartApplication();
        break;
    }
}

LRESULT CALLBACK App::trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!app)
        return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_SETTINGCHANGE || msg == WM_THEMECHANGED) {
        app->refreshTheme();
        return 0;
    }
    if (msg == WM_QUERYENDSESSION)
        return TRUE;
    if (msg == WM_ENDSESSION) {
        if (wp)
            app->requestQuit();
        return 0;
    }
    if (msg == WM_CLOSE) {
        app->requestQuit();
        return 0;
    }
    if (msg == kMsgTaskbarNoSpace) {
        app->handleTaskbarNoSpace();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerLyricDebounce) {
        KillTimer(h, kTimerLyricDebounce);
        app->playback_.onLyricDebounce();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerBackgroundSettingsSave) {
        KillTimer(h, kTimerBackgroundSettingsSave);
        app->flushBackgroundBlurSettingsSave();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerSongToastCover) {
        KillTimer(h, kTimerSongToastCover);
        app->songToastCoverWaitArmed_ = false;
        // 等待封面超时：按当前媒体信息弹出（可能无封面）
        app->notifySongToast();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerIdleQuote) {
        app->idle_.refreshHolidayCalendar(false);
        app->idle_.refreshIdleQuote(false);
        app->refreshIdleWelcome();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerTickTickStartupRetry) {
        KillTimer(h, kTimerTickTickStartupRetry);
        if (app->idle_.state().startupTaskSummaryPending_)
            app->idle_.refreshTickTickTasks();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerTaskbarAutoRestore) {
        app->tryRestoreTaskbarAfterNoSpace();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTimerTaskbarDisplayRebuildRetry) {
        KillTimer(h, kTimerTaskbarDisplayRebuildRetry);
        app->requestTaskbarRebuild();
        return 0;
    }
    if (msg == kMsgDialogClosed) {
        app->onDialogClosed(static_cast<DialogKind>(wp));
        return 0;
    }
    if (msg == kTrayMsg && LOWORD(lp) == WM_RBUTTONUP) {
        app->showTrayMenu();
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE) {
        app->taskbarDisplayRebuildRetries_ = 0;
        app->requestTaskbarRebuild();
        return 0;
    }
    if (msg == kMsgTaskbarRebuild) {
        // 只负责唤醒；即使被菜单等嵌套消息循环取出，也保留重建请求。
        return 0;
    }
    if (msg == WM_DESTROY) {
        app->destroyTray();
        return 0;
    }
    if (app->taskbarCreatedMsg_ && msg == app->taskbarCreatedMsg_) {
        // Explorer 重启：通知区域被清空，托盘图标必须重新添加；
        // 任务栏歌词窗口可能已随 Shell_TrayWnd 一起被销毁，交给宿主重建/重附着
        app->updateTrayIcon();
        if (app->taskbarHost)
            app->forEachHost([](TaskbarHost* host) { host->onTaskbarCreated(); });
        else if (app->taskbarPendingDisplayRebuild_ && !app->isRenderMode(RenderMode::Stopped)) {
            app->taskbarDisplayRebuildRetries_ = 0;
            app->requestTaskbarRebuild();
        } else if (app->taskbarAutoClosedForNoSpace_ && !app->isRenderMode(RenderMode::Stopped)) {
            app->armTaskbarAutoRestore();
            app->tryRestoreTaskbarAfterNoSpace();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace lyrics_app
