#include "app/application_internal.h"
#include "app/application.h"
#include "graphics/gdiplus_runtime.h"
#include "logging/ui_timing.h"
namespace lyrics_app {
App::~App() {
    // 显示线程可能仍读取频谱原子快照；必须在采样器等成员析构前停止宿主。
    destroyTaskbar();
}

void App::restartApplication() {
    if (shutdownRequested_)
        return;
    std::wstring executable(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        message_dialog::showModal(GetModuleHandleW(nullptr), trayHwnd, L"重启应用",
                                  L"无法获取应用路径，重启失败。", L"确定");
        return;
    }
    executable.resize(length);
    std::wstring commandLine =
        L"\"" + executable + L"\" --restart-parent " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process)) {
        runtime_log::writef(L"[lifecycle] restart failed error=%lu", GetLastError());
        message_dialog::showModal(GetModuleHandleW(nullptr), trayHwnd, L"重启应用",
                                  L"无法启动应用，重启失败。", L"确定");
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    runtimeLogger_.write(L"[lifecycle] restart requested");
    requestQuit();
}

void App::requestQuit() {
    if (shutdownRequested_)
        return;
    shutdownRequested_ = true;
    flushBackgroundBlurSettingsSave();
    runtimeLogger_.write(L"[lifecycle] quit requested");
    runtimeLogger_.flushSync();

    // 先销毁仍由 Explorer 承载的任务栏窗口和托盘窗口，再结束消息循环。
    // 仅投递 WM_QUIT 会把这部分清理推迟到 main() 返回后的析构阶段，
    // 任务栏歌词窗口或探测线程可能在此期间继续存活。
    cancelTaskbarAutoRestore();
    cancelTaskbarDisplayRebuildRetry();
    destroyTaskbar();
    closeUpdatePrompt();
    destroyTray();
    PostQuitMessage(0);
}
} // namespace lyrics_app

namespace lyrics_app {
void App::dispatchThreadMessage(const MSG& msg) {
    if (msg.message == kMsgSmtcChanged) {
        onSmtcChanged();
    } else if (msg.message == kMsgAppVolumeChanged) {
        pushAppVolume();
    } else if (msg.message == kMsgLyricReady) {
        playback_.onLyricReady(
            std::unique_ptr<LyricPayload>(reinterpret_cast<LyricPayload*>(msg.lParam)));
    } else if (msg.message == kMsgCoverReady) {
        playback_.onCoverReady(
            std::unique_ptr<CoverPayload>(reinterpret_cast<CoverPayload*>(msg.lParam)));
    } else if (msg.message == kMsgQqCoverInfoReady) {
        playback_.onQqCoverInfoReady(
            std::unique_ptr<QqCoverInfoPayload>(reinterpret_cast<QqCoverInfoPayload*>(msg.lParam)));
    } else if (msg.message == kMsgQqLocalFolderReady) {
        auto payload = std::unique_ptr<QqLocalFolderPayload>(
            reinterpret_cast<QqLocalFolderPayload*>(msg.lParam));
        qqLocalLyricsPickerOpen_ = false;
        if (payload)
            applyQqLocalLyricsPath(payload->path);
    } else if (msg.message == kMsgIdleAppReady) {
        auto payload =
            std::unique_ptr<IdleAppPayload>(reinterpret_cast<IdleAppPayload*>(msg.lParam));
        idleAppPickerOpen_ = false;
        if (payload && !payload->path.empty())
            addIdleApp(payload->path);
    } else if (msg.message == kMsgIdleQuoteReady) {
        idle_.onIdleQuoteReady(
            std::unique_ptr<IdleQuotePayload>(reinterpret_cast<IdleQuotePayload*>(msg.lParam)));
    } else if (msg.message == kMsgHolidayReady) {
        idle_.onHolidayReady(
            std::unique_ptr<HolidayPayload>(reinterpret_cast<HolidayPayload*>(msg.lParam)));
    } else if (msg.message == kMsgTickTickTasksReady) {
        idle_.onTickTickTasksReady(std::unique_ptr<TickTickTasksPayload>(
            reinterpret_cast<TickTickTasksPayload*>(msg.lParam)));
    } else if (msg.message == kMsgTickTickTaskCompleteReady) {
        idle_.onTickTickTaskCompleteReady(std::unique_ptr<TickTickTaskCompletePayload>(
            reinterpret_cast<TickTickTaskCompletePayload*>(msg.lParam)));
    }
}

bool App::translateDialogMessage(MSG& msg) {
    if (aboutDialog && aboutDialog->isOpen() && IsDialogMessageW(aboutDialog->hwnd(), &msg))
        return true;
    if (updatePromptHwnd_ && IsDialogMessageW(updatePromptHwnd_, &msg))
        return true;
    if (idleAppNameDialog && idleAppNameDialog->isOpen() &&
        IsDialogMessageW(idleAppNameDialog->hwnd(), &msg))
        return true;
    if (tickTickApiTokenDialog && tickTickApiTokenDialog->isOpen() &&
        IsDialogMessageW(tickTickApiTokenDialog->hwnd(), &msg))
        return true;
    // 设置、手动搜索和选择字体使用窗口级自绘输入框，字符输入必须经过
    // TranslateMessage 生成 WM_CHAR，再由各自窗口处理；不能交给
    // IsDialogMessageW 抢先消费键盘消息。
    if (fontColorDialog && fontColorDialog->isOpen() && fontColorDialog->isDialogMessage(&msg))
        return true;

    return false;
}

void App::finish() {
    closeUpdatePrompt();
    destroyTray();
    runtimeLogger_.write(L"[lifecycle] message loop ended");
    runtimeLogger_.flushSync();
}

int App::messageLoop() {
    MSG msg;
    for (;;) {
        rebuildTaskbarIfPending();
        // 重建内部的模态循环可能已取走唤醒消息；有新请求时不能先阻塞等待。
        if (taskbarRebuildPending_)
            continue;
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0)
            return result == 0 ? 0 : 1;

        // 覆盖线程消息和 IsDialogMessage 内部触发的按钮操作，不能只量 DispatchMessage。
        struct MessageTiming {
            UINT message;
            wchar_t windowClass[128]{};
            ULONGLONG start;
            explicit MessageTiming(const MSG& value)
                : message(value.message), start(runtime_log::uiTimeMs()) {
                if (value.hwnd)
                    GetClassNameW(value.hwnd, windowClass, _countof(windowClass));
            }
            ~MessageTiming() {
                const ULONGLONG elapsed = runtime_log::uiTimeMs() - start;
                if (elapsed >= 32) {
                    runtime_log::writef(
                        L"[ui][slow-message] class=%s message=0x%04X elapsed=%llums",
                        windowClass[0] ? windowClass : L"thread",
                        static_cast<unsigned>(message), static_cast<unsigned long long>(elapsed));
                }
            }
        } timing(msg);
        if (msg.hwnd == nullptr) {
            dispatchThreadMessage(msg);
            continue;
        }
        if (translateDialogMessage(msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

bool App::initialize(HINSTANCE inst, bool wantTaskbar) {
    loadSettings();
    idle_.prepareIdleApps();
    initializeRuntimeLogger();
    // 无命令行参数时始终开启任务栏歌词（沿用原"所有宿主都关闭则强制任务栏"的行为，
    // 避免用户关闭后找不到显示入口；运行中仍可通过托盘菜单随时关闭）
    if (!createTrayWindow(inst)) {
        runtime_log::writef(L"failed to create tray window");
        return false;
    }
    // 关于窗口保持隐藏，用于在程序启动后自动检查更新；用户打开关于时会再次检查。
    aboutDialog = std::make_unique<AboutDialog>();
    if (!aboutDialog->create(
            inst, trayHwnd, settings_.autoCheckOnStartup_, settings_.useGiteeUpdateSource_,
            [this](bool enabled) { setAutoCheckOnStartup(enabled); },
            [this](bool useGitee) {
                if (settings_.useGiteeUpdateSource_ == useGitee)
                    return;
                settings_.useGiteeUpdateSource_ = useGitee;
                saveSettings();
            },
            [this](const std::wstring& path) { return launchUpdateInstaller(path); },
            [this](const std::wstring& version) { showUpdatePrompt(version); }))
        aboutDialog.reset();
    // 设置窗口预创建（保持隐藏）：把首次打开时的控件构建成本挪到启动阶段；
    // 关闭后仍会销毁，下一次打开重新创建。
    settingsDialog = std::make_unique<SettingsDialog>();
    if (!settingsDialog->create(inst, trayHwnd, currentSettingsState(), buildSettingsActions()))
        settingsDialog.reset();
    if (wantTaskbar) {
        const TaskbarCreateResult result = createTaskbar(inst);
        if (result == TaskbarCreateResult::Failed) {
            runtime_log::writef(L"failed to create taskbar window");
            return false;
        }
        if (result == TaskbarCreateResult::NoSpace)
            runtime_log::writef(L"[taskbar] started tray-only reason=no-space");
    }

    monitor.start([this] { PostThreadMessageW(mainThread, kMsgSmtcChanged, 0, 0); });
    idle_.refreshHolidayCalendar(false);
    idle_.refreshIdleQuote(false);
    idle_.refreshTickTickTasks();
    refreshIdleWelcome();
    appVolume_.setChangedCallback(
        [this] { PostThreadMessageW(mainThread, kMsgAppVolumeChanged, 0, 0); });

    runtime_log::writef(L"QQMusicLyric started, waiting for QQ Music or NetEase Music...");

    return true;
}

namespace {
struct StartupOptions {
    bool valid = true;
    bool verifyRelease = false;
    bool wantTaskbar = true;
    DWORD restartParent = 0;
};

StartupOptions parseStartupOptions() {
    StartupOptions options;
    int verifyArgc = 0;
    LPWSTR* verifyArgv = CommandLineToArgvW(GetCommandLineW(), &verifyArgc);
    if (verifyArgv) {
        for (int i = 1; i < verifyArgc; ++i) {
            if (wcscmp(verifyArgv[i], L"--restart-parent") == 0 && i + 1 < verifyArgc) {
                const std::wstring parentArgument = verifyArgv[++i];
                try {
                    size_t parsed = 0;
                    const unsigned long parentId = std::stoul(parentArgument, &parsed);
                    if (parsed != parentArgument.size() || parentId == 0 ||
                        parentId == GetCurrentProcessId()) {
                        LocalFree(verifyArgv);
                        options.valid = false;
                        return options;
                    }
                    options.restartParent = parentId;
                } catch (const std::exception&) {
                    LocalFree(verifyArgv);
                    options.valid = false;
                    return options;
                }
            }
            if (wcscmp(verifyArgv[i], L"--taskbar") == 0)
                options.wantTaskbar = true;
            if (wcscmp(verifyArgv[i], L"--verify-release") == 0) {
                options.verifyRelease = true;
                break;
            }
        }
        LocalFree(verifyArgv);
    }

    return options;
}
} // namespace

int runApplication() {
    // 安装脚本用这个无界面参数确认候选文件确实来自 Release 配置；
    // 必须在单实例检查之前处理，否则已有程序运行时会掩盖构建类型。
    const StartupOptions options = parseStartupOptions();
    if (!options.valid)
        return 1;
    if (options.verifyRelease) {
#if defined(QQMUSICLYRIC_RELEASE_BUILD) && QQMUSICLYRIC_RELEASE_BUILD
        return 0;
#else
        return 1;
#endif
    }

    if (options.restartParent != 0) {
        HANDLE parentProcess = OpenProcess(SYNCHRONIZE, FALSE, options.restartParent);
        if (parentProcess) {
            const DWORD result = WaitForSingleObject(parentProcess, INFINITE);
            CloseHandle(parentProcess);
            if (result != WAIT_OBJECT_0)
                return 1;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
            return 1;
        }
    }

    // 单实例限制：命名互斥体（Local\ 会话级）。进程退出（含崩溃）时内核自动销毁，
    // 无需手动释放；句柄保持到进程结束即可
    HANDLE singleInstance =
        CreateMutexW(nullptr, TRUE,
                     L"Local\\QQMusicLyric.SingleInstance.{7E3A9C41-2B5D-4F1E-9A6C-0D8B3E5F2A74}");
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        message_dialog::showModal(GetModuleHandleW(nullptr), nullptr, L"QQ 音乐任务栏歌词",
                                  L"QQ 音乐任务栏歌词已在运行中，请勿重复启动。", L"确定");
        CloseHandle(singleInstance);
        return 0;
    }

    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT); // 否则 wprintf 中文输出为 '?'
    // DPI 感知由 app.manifest 声明 PerMonitorV2（运行时 API 请求 V2 会被静默降级为 V1）
    // 提高计时器粒度，否则 SetTimer 的实际触发间隔可能远大于设定值，滚动动画卡顿
    struct TimerResolution {
        TimerResolution() { timeBeginPeriod(1); }
        ~TimerResolution() { timeEndPeriod(1); }
    } timerResolution;
    winrt::init_apartment();
    graphics::gdiplusStatus();

    App app;
    if (!app.initialize(GetModuleHandleW(nullptr), options.wantTaskbar))
        return 1;
    const int exitCode = app.messageLoop();
    app.finish();
    return exitCode;
}

} // namespace lyrics_app
