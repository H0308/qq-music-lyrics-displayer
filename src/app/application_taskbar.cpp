#include "app/application_internal.h"
#include "settings/display_rules.h"
namespace lyrics_app {
std::vector<TaskbarDisplayInfo> App::desiredTaskbarDisplays(const SmtcSnapshot& snap) const {
    auto displays = enumerateTaskbarDisplays();
    if (displays.empty())
        return {};
    auto primary = std::find_if(displays.begin(), displays.end(),
                                [](const auto& item) { return item.primary; });
    if (primary == displays.end())
        primary = displays.begin();

    if (settings_.taskbarViewMode_ == TaskbarViewMode::Embedded ||
        (settings_.taskbarViewMode_ == TaskbarViewMode::Immersive && !snap.sessionAlive))
        return {*primary};

    if (settings_.taskbarMonitorId_ == L"all") {
        if (settings_.taskbarViewMode_ == TaskbarViewMode::Immersive) {
            std::vector<TaskbarDisplayInfo> horizontal;
            std::copy_if(
                displays.begin(), displays.end(), std::back_inserter(horizontal),
                [](const auto& item) { return item.taskbarWindow && item.horizontalTaskbar; });
            return horizontal.empty() ? std::vector<TaskbarDisplayInfo>{*primary}
                                      : std::move(horizontal);
        }
        return displays;
    }

    auto selected = primary;
    if (!settings_.taskbarMonitorId_.empty()) {
        const auto found = std::find_if(displays.begin(), displays.end(), [this](const auto& item) {
            return item.deviceName == settings_.taskbarMonitorId_;
        });
        if (found != displays.end())
            selected = found;
    }
    if (settings_.taskbarViewMode_ == TaskbarViewMode::Immersive && !selected->taskbarWindow)
        selected = primary;
    return {*selected};
}

std::wstring App::taskbarDisplayLabel(const TaskbarDisplayInfo& display) const {
    std::wstring label = display.displayName;
    if (display.displayName != display.deviceName)
        label += L"（" + display.deviceName + L"）";
    if (display.primary)
        label += L"（主显示器）";
    return label;
}

std::wstring App::taskbarMonitorLabel() const {
    if (settings_.taskbarMonitorId_ == L"all")
        return L"全部显示器";

    const auto compactDeviceName = [](std::wstring deviceName) {
        if (deviceName.rfind(L"\\\\.\\", 0) == 0)
            deviceName.erase(0, 4);
        return deviceName;
    };
    const auto displays = enumerateTaskbarDisplays();
    if (settings_.taskbarMonitorId_.empty())
        return L"主显示器";
    const auto selected = std::find_if(displays.begin(), displays.end(), [this](const auto& item) {
        return item.deviceName == settings_.taskbarMonitorId_;
    });
    if (selected == displays.end())
        return compactDeviceName(settings_.taskbarMonitorId_) + L" 断开";
    return selected->primary ? L"主显示器" : compactDeviceName(selected->deviceName);
}

void App::selectTaskbarMonitor(const std::wstring& deviceName) {
    if (settings_.taskbarMonitorId_ == deviceName)
        return;
    settings_.taskbarMonitorId_ = deviceName;
    saveSettings();
    syncTaskbarView(monitor.snapshot());
    refreshSettingsDialog(true);
}

void App::showTaskbarMonitorMenu() {
    const auto displays = enumerateTaskbarDisplays();
    std::vector<fluent::FluentMenuItem> items;
    taskbarMonitorMenuItems_.clear();
    fluent::FluentMenuItem all;
    all.id = kCmdTaskbarMonitorAll;
    all.text = L"全部显示器";
    all.icon = settings_icon::Kind::Display;
    all.checked = settings_.taskbarMonitorId_ == L"all";
    items.push_back(std::move(all));
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& display = displays[i];
        fluent::FluentMenuItem item;
        item.id = kCmdTaskbarMonitorBase + static_cast<int>(i);
        item.text = taskbarDisplayLabel(display);
        item.icon = settings_icon::Kind::Display;
        item.checked = settings_.taskbarMonitorId_.empty()
                           ? display.primary
                           : settings_.taskbarMonitorId_ == display.deviceName;
        taskbarMonitorMenuItems_.emplace_back(item.id, display.deviceName);
        items.push_back(std::move(item));
    }
    POINT point{};
    GetCursorPos(&point);
    fluent::FluentMenu::show(trayHwnd, point, std::move(items), [this](int command) {
        if (command == kCmdTaskbarMonitorAll) {
            selectTaskbarMonitor(L"all");
            return;
        }
        const auto found =
            std::find_if(taskbarMonitorMenuItems_.begin(), taskbarMonitorMenuItems_.end(),
                         [command](const auto& item) { return item.first == command; });
        if (found != taskbarMonitorMenuItems_.end())
            selectTaskbarMonitor(found->second);
    });
}

bool App::taskbarExpandedConfigured() const {
    return settings_.taskbarViewMode_ != TaskbarViewMode::Embedded;
}

void App::requestTaskbarRebuild() {
    if (shutdownRequested_ || taskbarRebuildPending_)
        return;
    taskbarRebuildPending_ = true;
    // 这里只唤醒主消息循环，窗口过程和模态消息循环都不能直接销毁宿主。
    // SetWindowPos/SHAppBarMessage 等调用可能重入 WM_DISPLAYCHANGE，
    // 必须等原来的窗口调用栈退出后才能释放它仍在使用的对象。
    if (trayHwnd && !PostMessageW(trayHwnd, kMsgTaskbarRebuild, 0, 0))
        runtime_log::writef(L"[display] failed to wake main loop for taskbar rebuild");
}

void App::cancelTaskbarDisplayRebuildRetry() {
    taskbarPendingDisplayRebuild_ = false;
    taskbarRebuildAllowOverlap_ = false;
    taskbarDisplayRebuildRetries_ = 0;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarDisplayRebuildRetry);
}

void App::scheduleTaskbarDisplayRebuildRetry(bool allowOverlap) {
    if (shutdownRequested_ || isRenderMode(RenderMode::Stopped))
        return;
    taskbarPendingDisplayRebuild_ = true;
    taskbarRebuildAllowOverlap_ = allowOverlap;
    if (taskbarDisplayRebuildRetries_ >= kTaskbarDisplayRebuildMaxRetries)
        return;
    ++taskbarDisplayRebuildRetries_;
    if (trayHwnd)
        SetTimer(trayHwnd, kTimerTaskbarDisplayRebuildRetry, kTaskbarDisplayRebuildRetryMs,
                 nullptr);
}

void App::rebuildTaskbarIfPending() {
    if (!taskbarRebuildPending_)
        return;
    taskbarRebuildPending_ = false;
    if (shutdownRequested_ || (!taskbarHost && !taskbarPendingDisplayRebuild_))
        return;

    const bool allowOverlap = taskbarHost ? taskbarAllowOverlap_ : taskbarRebuildAllowOverlap_;
    runtime_log::writef(L"[display] taskbar rebuild started");
    destroyTaskbar();
    if (shutdownRequested_)
        return;
    const TaskbarCreateResult result = createTaskbar(GetModuleHandleW(nullptr), allowOverlap);
    if (result == TaskbarCreateResult::Failed)
        scheduleTaskbarDisplayRebuildRetry(allowOverlap);
    else
        cancelTaskbarDisplayRebuildRetry();
    refreshSettingsDialog(true);
}

void App::syncTaskbarView(const SmtcSnapshot& snap) {
    if (!taskbarHost && !taskbarPendingDisplayRebuild_)
        return;

    const auto displays = desiredTaskbarDisplays(snap);
    std::vector<std::wstring> desiredIds;
    desiredIds.reserve(displays.size());
    for (const auto& display : displays)
        desiredIds.push_back(display.deviceName);
    if (!taskbarHost || desiredIds != activeTaskbarDisplayIds_) {
        requestTaskbarRebuild();
        return;
    }

    forEachHost([&](TaskbarHost* host) {
        host->setAppBarEdge(settings_.appBarEdge_);
        TaskbarViewMode mode = settings_.taskbarViewMode_;
        if (mode == TaskbarViewMode::Immersive && (!snap.sessionAlive || host->isVerticalTaskbar()))
            mode = TaskbarViewMode::Embedded;
        host->setViewMode(mode);
    });
    syncTaskbarOrientation();
}

void App::syncSpectrumWithMode() {
    const bool active = display_rules::spectrumRequested(
                            settings_, static_cast<RenderMode>(renderMode_), taskbarVertical_) &&
                        taskbarHost != nullptr && !taskbarSpectrumSuppressedForNoSpace_;
    if (active) {
        SmtcSnapshot snap = monitor.snapshot();
        const wchar_t* processName = spectrumProcessName(snap.player);
        if (*processName)
            spectrum_.setTargetProcessName(std::wstring(processName));
        spectrum_.start();
        forEachHost([&](TaskbarHost* host) {
            host->setSpectrumVisible(display_rules::spectrumRequested(
                settings_, static_cast<RenderMode>(renderMode_), host->isVerticalTaskbar()));
        });
    } else {
        spectrum_.stop();
        forEachHost([&](TaskbarHost* host) { host->setSpectrumVisible(false); });
    }
}

void App::tryRestoreSpectrumAfterNoSpace() {
    if (!taskbarSpectrumSuppressedForNoSpace_ || taskbarSpectrumRestoring_ ||
        !display_rules::spectrumRequested(settings_, static_cast<RenderMode>(renderMode_),
                                          taskbarVertical_) || !taskbarHost)
        return;

    // 频谱本身会扩大任务栏歌词窗口；恢复空间时先试着恢复用户设置，
    // 如果再次判定无空间，状态回调会重新保留抑制标记并关闭频谱。
    taskbarSpectrumRestoring_ = true;
    taskbarSpectrumSuppressedForNoSpace_ = false;
    syncSpectrumWithMode();
    taskbarSpectrumRestoring_ = false;
}

void App::syncTaskbarOrientation() {
    if (!taskbarHost)
        return;
    const bool compact = taskbarHost->isCompactTaskbar();
    if (compact != taskbarCompact_) {
        taskbarCompact_ = compact;
        refreshSettingsDialog(true);
    }
    const bool vertical = taskbarHost->isVerticalTaskbar();
    if (vertical == taskbarVertical_)
        return;

    taskbarVertical_ = vertical;
    // 方向只影响实际显示与采样，保留用户配置，回到横向后重新应用。
    applyEffectiveTaskbarSettings();
    syncSpectrumWithMode();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
    refreshSettingsDialog(true);
}

void App::armTaskbarAutoRestore() {
    if (shutdownRequested_ || isRenderMode(RenderMode::Stopped) || taskbarHost)
        return;
    taskbarAutoRestorePending_ = true;
    if (trayHwnd && !SetTimer(trayHwnd, kTimerTaskbarAutoRestore, kTaskbarAutoRestoreMs, nullptr)) {
        runtime_log::writef(L"[taskbar] failed to schedule auto-restore");
    }
}

void App::cancelTaskbarAutoRestore() {
    taskbarAutoRestorePending_ = false;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarAutoRestore);
}

void App::tryRestoreTaskbarAfterNoSpace() {
    if (!taskbarAutoRestorePending_)
        return;
    if (shutdownRequested_ || isRenderMode(RenderMode::Stopped)) {
        cancelTaskbarAutoRestore();
        return;
    }
    if (taskbarHost) {
        cancelTaskbarAutoRestore();
        return;
    }

    const TaskbarCreateResult result = createTaskbar(GetModuleHandleW(nullptr));
    if (result == TaskbarCreateResult::Created) {
        cancelTaskbarAutoRestore();
        // 不要仅凭宿主创建成功就关闭提示。此时首次真实的空间探测可能
        // 尚未完成，必须等 onTaskbarPlacementStatus() 确认有可用空间后再关。
        runtime_log::writef(L"[taskbar] auto-restore result=created, awaiting-placement");
    } else if (result == TaskbarCreateResult::Failed) {
        // 真正的宿主创建失败不弹出“空间不足”提示；等待 Explorer 的
        // TaskbarCreated 广播重新触发一次恢复探测。
        cancelTaskbarAutoRestore();
        runtime_log::writef(L"[taskbar] auto-restore result=failed");
    }
    // NoSpace 会保留宿主，由 TaskbarHost 的低频避让探测继续等待空间释放。
}

void App::configureTaskbarHostCallbacks(TaskbarHost* host, bool primary) {
    if (!host)
        return;
    host->setCloseCallback([this] { requestQuit(); });
    host->setSpectrumReader([source = &spectrum_] { return source->bands(); });
    if (primary) {
        host->setTickCallback([this] { onFrame(); });
        host->setStatusTextCycleCompletedCallback(
            [this] { idle_.onStartupTaskSummaryCompleted(); });
        host->setPlacementStatusCallback(
            [this, host](TaskbarPlacementStatus status) {
                // 状态通知异步回到主线程后，不能用旧的空间判定覆盖当前宿主状态。
                if (taskbarHost.get() == host && host->placementStatus() == status)
                    onTaskbarPlacementStatus(status);
            });
    }
    host->setControlCallback([this](MediaControl c) { onControl(c); });
    host->setImmersiveExitCallback([this] { applyTaskbarViewMode(0); });
    host->setDockQuickAppsCallback([this](POINT pt) { showDockQuickApps(pt); });
    host->setImmersiveMenuCallback([this](POINT pt) { showTrayMenu(pt); });
    host->setContextMenuCallback([this](POINT pt) { showTaskbarMenu(pt); });
    host->setPositionModeChangedCallback([this](int mode) {
        mode = mode == 1 ? 1 : 0;
        if (settings_.taskbarPosition_ == mode)
            return;
        settings_.taskbarPosition_ = mode;
        logSettingInt(L"taskbar-position-drag", settings_.taskbarPosition_);
        saveSettings();
    });
    host->setAppVolumeCallback([this](int percent) {
        const bool ok = appVolume_.setVolumePercent(percent);
        runtime_log::writef(L"[action][volume] set percent=%d result=%s", percent,
                            ok ? L"ok" : L"unavailable");
        pushAppVolume();
    });
    host->setSourceOpenCallback([](const std::wstring& source) {
        const bool ok = platform_icon::launchSourceApp(source);
        runtime_log::writef(L"[action][player] open-source source=%s result=%s", source.c_str(),
                            ok ? L"ok" : L"failed");
        if (!ok)
            runtime_log::writef(L"[player] failed to activate source: %s", source.c_str());
    });
    host->setIdleAppOpenCallback([this](const std::wstring& path) { launchIdleApp(path); });
    host->setIdleTaskOpenCallback(
        [this](const IdleTaskInfo& task) { idle_.openTickTickTask(task); });
    host->setIdleTaskCompleteCallback(
        [this](const IdleTaskInfo& task) { idle_.completeTickTickTask(task); });
    host->setMediaPopupOpenedCallback([this] { idle_.refreshTickTickTasks(); });
}

TaskbarCreateResult App::createTaskbar(HINSTANCE inst, bool allowOverlap) {
    if (taskbarHost) {
        if (!allowOverlap)
            return TaskbarCreateResult::Created;

        // 空间不足时保留现有宿主和探测线程；用户确认强制开启时，
        // 直接把同一个宿主切到允许重叠，避免重新创建窗口和等待首次探测。
        taskbarAllowOverlap_ = true;
        taskbarAutoClosedForNoSpace_ = false;
        taskbarSpectrumSuppressedForNoSpace_ = false;
        cancelTaskbarAutoRestore();
        cancelTaskbarDisplayRebuildRetry();
        forEachHost([](TaskbarHost* host) { host->setAllowOverlap(true); });
        const TaskbarPlacementStatus status = taskbarHost->refreshPlacement();
        if (status == TaskbarPlacementStatus::Unavailable) {
            taskbarAllowOverlap_ = false;
            runtime_log::writef(L"[taskbar] forced-open result=unavailable");
            return TaskbarCreateResult::Failed;
        }
        forEachHost([&](TaskbarHost* host) {
            host->show();
            host->applyPresentationFrame(presentation_.frame());
            host->refreshPlacement();
        });
        syncSpectrumWithMode();
        updateTrayIcon();
        return TaskbarCreateResult::Created;
    }

    const SmtcSnapshot snap = monitor.snapshot();
    const auto displays = desiredTaskbarDisplays(snap);
    if (displays.empty()) {
        runtime_log::writef(L"failed to create taskbar host");
        return TaskbarCreateResult::Failed;
    }
    taskbarAllowOverlap_ = allowOverlap;
    activeTaskbarDisplayIds_.clear();
    for (const auto& display : displays)
        activeTaskbarDisplayIds_.push_back(display.deviceName);

    auto createHost = [&](const TaskbarDisplayInfo& display, bool primary) {
        auto host = std::make_unique<TaskbarHost>();
        host->setVisibilitySuppressed(true);
        TaskbarViewMode initialMode = settings_.taskbarViewMode_;
        if (initialMode == TaskbarViewMode::Immersive &&
            (!snap.sessionAlive || !display.horizontalTaskbar || !display.taskbarWindow))
            initialMode = TaskbarViewMode::Embedded;
        if (!host->create(inst, display.deviceName, initialMode)) {
            runtime_log::writef(L"failed to create taskbar host for %s",
                                display.deviceName.c_str());
            return std::unique_ptr<TaskbarHost>{};
        }
        host->setAllowOverlap(allowOverlap);
        configureTaskbarHostCallbacks(host.get(), primary);
        // 完全停止模式下新宿主从创建开始就保持隐藏，避免同步首帧后闪现。
        host->setRenderMode(static_cast<RenderMode>(renderMode_));
        host->setAppBarEdge(settings_.appBarEdge_);
        return host;
    };

    taskbarHost = createHost(displays.front(), true);
    if (!taskbarHost) {
        activeTaskbarDisplayIds_.clear();
        taskbarAllowOverlap_ = false;
        return TaskbarCreateResult::Failed;
    }
    for (size_t i = 1; i < displays.size(); ++i) {
        auto host = createHost(displays[i], false);
        if (host)
            additionalTaskbarHosts_.push_back(std::move(host));
    }

    forEachHost([&](TaskbarHost* host) {
        host->setAppBarEdge(settings_.appBarEdge_);
        syncHost(host);
        if (settings_.hasUserFont_)
            host->setFont(settings_.fontFamily_, settings_.fontSize_, settings_.fontStyle_);
        host->applyAppearance(appearanceForHost(host->isVerticalTaskbar()));

        host->setPositionMode(settings_.taskbarPosition_);
        host->setAppVolume(appVolumeState_);
    });
    applyFontAppearance();
    syncTaskbarOrientation();
    applyEffectiveTaskbarSettings();
    syncSpectrumWithMode();
    const TaskbarPlacementStatus placementStatus = taskbarHost->refreshPlacement();
    for (auto& host : additionalTaskbarHosts_)
        host->refreshPlacement();
    if (placementStatus == TaskbarPlacementStatus::NoSpace) {
        taskbarAutoClosedForNoSpace_ = true;
        taskbarSpectrumSuppressedForNoSpace_ = true;
        spectrum_.stop();
        const bool wasRestoringSpectrum = taskbarSpectrumRestoring_;
        taskbarSpectrumRestoring_ = true;
        forEachHost([](TaskbarHost* host) { host->setSpectrumVisible(false); });
        taskbarSpectrumRestoring_ = wasRestoringSpectrum;
        if (taskbarHost->placementStatus() != TaskbarPlacementStatus::NoSpace) {
            // 释放频谱占用后已有足够空间，继续保留宿主但不需要进入无空间状态。
            taskbarAutoClosedForNoSpace_ = false;
            taskbarSpectrumSuppressedForNoSpace_ = false;
            cancelTaskbarAutoRestore();
            updateTrayIcon();
            return TaskbarCreateResult::Created;
        }
        cancelTaskbarAutoRestore();
        runtime_log::writef(L"[taskbar] auto-hide reason=no-space");
        cancelTaskbarDisplayRebuildRetry();
        updateTrayIcon();
        return TaskbarCreateResult::NoSpace;
    }
    if (placementStatus == TaskbarPlacementStatus::Unavailable) {
        runtime_log::writef(L"[taskbar] placement unavailable after creation");
        destroyTaskbar();
        return TaskbarCreateResult::Failed;
    }
    taskbarAutoClosedForNoSpace_ = false;
    cancelTaskbarAutoRestore();
    cancelTaskbarDisplayRebuildRetry();
    // 宿主会在首次避让探测完成后自行解除显示抑制；强制开启仍重新提交当前帧，
    // 确保“开启”选项立即生效而不必等待下一次媒体事件。
    if (allowOverlap) {
        forEachHost([&](TaskbarHost* host) {
            host->show();
            host->applyPresentationFrame(presentation_.frame());
        });
    }
    updateTrayIcon();
    return TaskbarCreateResult::Created;
}

void App::destroyTaskbar() {
    taskbarRebuildPending_ = false;
    spectrum_.stop(); // 频谱只画在任务栏上，宿主销毁时捕获线程一并停
    taskbarAllowOverlap_ = false;
    taskbarSpectrumSuppressedForNoSpace_ = false;
    taskbarSpectrumRestoring_ = false;
    taskbarNoSpaceClosePending_ = false;
    // 任何销毁路径都结束一次“手动开启待确认”请求，避免旧请求在后续
    // 的自动探测消息中误把自动关闭当成手动开启失败并弹框。
    taskbarManualOpenPending_ = false;
    additionalTaskbarHosts_.clear();
    auto host = std::move(taskbarHost);
    host.reset();
    activeTaskbarDisplayIds_.clear();
    updateTrayIcon();
}

bool App::taskbarEnabledForUserAction() {
    if (isRenderMode(RenderMode::Stopped))
        return false;
    if (!taskbarHost)
        return taskbarPendingDisplayRebuild_;
    const TaskbarPlacementStatus status = taskbarHost->placementStatus();
    if (status == TaskbarPlacementStatus::NoSpace || status == TaskbarPlacementStatus::Unavailable)
        return false;
    // AppBar 在无媒体且关闭空闲入口时会主动隐藏并释放工作区，
    // 但宿主与用户选择仍是开启状态，托盘操作不能把它误判为关闭。
    if (settings_.taskbarViewMode_ == TaskbarViewMode::AppBar)
        return true;
    if (taskbarHost->isDisplayed()) {
        taskbarEnabledBeforeStopped_ = false;
        return true;
    }
    return taskbarEnabledBeforeStopped_;
}

void App::toggleTaskbar() {
    runtime_log::writef(L"[action][taskbar] toggle current=%s",
                        taskbarEnabledForUserAction() ? L"enabled" : L"disabled");
    if (taskbarEnabledForUserAction()) {
        taskbarAutoClosedForNoSpace_ = false;
        taskbarManualOpenPending_ = false;
        cancelTaskbarAutoRestore();
        cancelTaskbarDisplayRebuildRetry();
        destroyTaskbar();
    } else {
        const bool noSpaceAlreadyKnown =
            !isRenderMode(RenderMode::Stopped) &&
            ((taskbarHost && taskbarHost->placementStatus() == TaskbarPlacementStatus::NoSpace) ||
             (!taskbarHost && taskbarAutoClosedForNoSpace_ && taskbarAutoRestorePending_));
        if (noSpaceAlreadyKnown) {
            // 无空间状态已经由当前宿主确认，保留宿主直接提示；用户确认后
            // 由 createTaskbar(..., true) 把同一个宿主切换为允许重叠。
            taskbarAutoClosedForNoSpace_ = true;
            cancelTaskbarAutoRestore();
            showTaskbarSpacePrompt();
            runtime_log::writef(L"[action][taskbar] toggle result=no-space-prompt");
            return;
        }
        // 仅在宿主不是“已确认无空间”的状态下才重建；无空间宿主会保留
        // 后台探测线程，不走销毁/重建路径。
        if (taskbarHost)
            destroyTaskbar();
        cancelTaskbarDisplayRebuildRetry();
        if (isRenderMode(RenderMode::Stopped)) {
            message_dialog::showModal(
                GetModuleHandleW(nullptr), trayHwnd, L"任务栏歌词提示",
                L"当前已开启性能-完全停止模式，如果要显示任务栏歌词，可以在性能设置中选择其他模式",
                L"确定");
            runtime_log::writef(L"[action][taskbar] toggle result=stopped");
            return;
        }
        taskbarManualOpenPending_ = true;
        const TaskbarCreateResult result = createTaskbar(GetModuleHandleW(nullptr));
        if (result == TaskbarCreateResult::NoSpace) {
            taskbarManualOpenPending_ = false;
            showTaskbarSpacePrompt();
        } else if (result == TaskbarCreateResult::Failed) {
            taskbarManualOpenPending_ = false;
            runtime_log::writef(L"[action][taskbar] toggle result=failed");
        }
    }
    runtime_log::writef(L"[action][taskbar] toggle result=%s",
                        taskbarHost ? L"enabled" : L"disabled");
}

void App::onTaskbarPlacementStatus(TaskbarPlacementStatus status) {
    if (status != TaskbarPlacementStatus::NoSpace) {
        taskbarNoSpaceClosePending_ = false;
        taskbarManualOpenPending_ = false;
        if (status != TaskbarPlacementStatus::Unavailable) {
            taskbarAutoClosedForNoSpace_ = false;
            if (!taskbarSpectrumRestoring_) {
                if (taskbarSpectrumSuppressedForNoSpace_) {
                    if (display_rules::spectrumRequested(
                            settings_, static_cast<RenderMode>(renderMode_), taskbarVertical_))
                        tryRestoreSpectrumAfterNoSpace();
                    else
                        // 频谱因用户设置、竖向任务栏或性能模式关闭时，
                        // 无需继续保留“空间不足抑制”状态；回到正常模式时
                        // 由普通同步逻辑按用户设置重新决定是否开启。
                        taskbarSpectrumSuppressedForNoSpace_ = false;
                } else if (taskbarHost && !isRenderMode(RenderMode::Stopped))
                    syncSpectrumWithMode();
            }
        }
        return;
    }
    if (!taskbarHost || taskbarAllowOverlap_ || taskbarNoSpaceClosePending_)
        return;

    taskbarAutoClosedForNoSpace_ = true;
    // 空间不足时只隐藏宿主；后台探测线程继续运行，空间恢复后可直接复用窗口和
    // 渲染资源。频谱停止和提示处理延迟到托盘消息，避免在探测回调内 join 捕获线程。
    taskbarSpectrumSuppressedForNoSpace_ = true;

    // 状态回调可能来自 TaskbarHost 的 WM_TIMER；延迟到托盘窗口消息中处理手动
    // 开启提示，避免在探测/重排调用栈内直接创建模态窗口。
    taskbarNoSpaceClosePending_ = true;
    if (!trayHwnd || !PostMessageW(trayHwnd, kMsgTaskbarNoSpace, 0, 0)) {
        taskbarNoSpaceClosePending_ = false;
        runtime_log::writef(L"[taskbar] failed to schedule no-space state handling");
    }
}

void App::handleTaskbarNoSpace() {
    if (!taskbarNoSpaceClosePending_)
        return;
    taskbarNoSpaceClosePending_ = false;
    const bool shouldPrompt = taskbarManualOpenPending_;
    taskbarManualOpenPending_ = false;
    if (!taskbarHost || taskbarAllowOverlap_)
        return;

    taskbarAutoClosedForNoSpace_ = true;
    spectrum_.stop();
    const bool wasRestoringSpectrum = taskbarSpectrumRestoring_;
    taskbarSpectrumRestoring_ = true;
    forEachHost([](TaskbarHost* host) { host->setSpectrumVisible(false); });
    taskbarSpectrumRestoring_ = wasRestoringSpectrum;
    // 关闭频谱本身可能已经释放了占用空间；此时不再进入无空间处理，
    // 避免把刚恢复的宿主再次标记为自动隐藏。
    if (taskbarHost->placementStatus() != TaskbarPlacementStatus::NoSpace) {
        taskbarAutoClosedForNoSpace_ = false;
        taskbarSpectrumSuppressedForNoSpace_ = false;
        return;
    }
    cancelTaskbarAutoRestore();
    runtime_log::writef(L"[taskbar] auto-hide reason=no-space");
    if (shouldPrompt)
        showTaskbarSpacePrompt();
}

void App::showTaskbarSpacePrompt() {
    runtime_log::writef(L"[action][taskbar] prompt reason=no-space auto-closed=%d",
                        taskbarAutoClosedForNoSpace_ ? 1 : 0);
    // 系统模态框自带标准按钮和关闭行为；显示期间暂停自动恢复定时器，
    // 避免用户尚未选择时后台又创建宿主并改变当前状态。
    cancelTaskbarAutoRestore();
    const message_dialog::Result result = message_dialog::showModal(
        GetModuleHandleW(nullptr), trayHwnd, L"任务栏歌词提示",
        L"当前任务栏可用空间不足，开启任务栏歌词可能遮挡任务栏按钮并影响正常使用。\n\n"
        L"是否仍要开启任务栏歌词？",
        L"开启", L"取消开启", false);
    if (result == message_dialog::Result::Primary) {
        const TaskbarCreateResult createResult = createTaskbar(GetModuleHandleW(nullptr), true);
        if (createResult == TaskbarCreateResult::Failed)
            runtime_log::writef(L"[action][taskbar] forced-open result=failed");
        else if (createResult == TaskbarCreateResult::NoSpace)
            runtime_log::writef(L"[action][taskbar] forced-open result=no-space");
        return;
    }

    if (!shutdownRequested_ && !isRenderMode(RenderMode::Stopped) && taskbarAutoClosedForNoSpace_ &&
        !taskbarHost)
        armTaskbarAutoRestore();
}
} // namespace lyrics_app
