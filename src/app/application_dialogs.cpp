#include "app/application_internal.h"
namespace lyrics_app {
void App::refreshSettingsDialog(bool onlyWhenOpen) {
    auto* dialog = settingsDialog.get();
    if (dialog && (!onlyWhenOpen || dialog->isOpen()))
        dialog->updateState(currentSettingsState());
}

void App::addIdleApp(const std::wstring& path) {
    if (settings_.idleApps_.size() >= kMaxIdleApps)
        return;
    if (!validExePath(path))
        return;
    for (const auto& app : settings_.idleApps_) {
        if (_wcsicmp(app.path.c_str(), path.c_str()) == 0)
            return;
    }
    IdleAppInfo app;
    app.path = path;
    settings_.idleApps_.push_back(std::move(app));
    idle_.prepareIdleApps();
    runtime_log::writef(L"[action][idle-entry] app-added path=%s", path.c_str());
    saveSettings();
    refreshSettingsDialog();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
}

void App::editIdleApp(int index) {
    if (index < 0 || static_cast<size_t>(index) >= settings_.idleApps_.size())
        return;
    if (idleAppNameDialog && idleAppNameDialog->isOpen()) {
        SetForegroundWindow(idleAppNameDialog->hwnd());
        return;
    }
    idleAppNameDialog.reset();

    auto dialog = std::make_unique<IdleAppNameDialog>();
    const auto& app = settings_.idleApps_[static_cast<size_t>(index)];
    const std::wstring initial = app.customName.empty() ? app.name : app.customName;
    HWND parent = settingsDialog ? settingsDialog->hwnd() : trayHwnd;
    if (!dialog->create(GetModuleHandleW(nullptr), parent, initial))
        return;
    dialog->setApplyCallback([this, index](const std::wstring& name) {
        if (index < 0 || static_cast<size_t>(index) >= settings_.idleApps_.size())
            return;
        auto& app = settings_.idleApps_[static_cast<size_t>(index)];
        if (app.customName == name || (app.customName.empty() && app.name == name))
            return;
        app.customName = name;
        idle_.prepareIdleApps();
        runtime_log::writef(L"[action][idle-entry] app-name-changed index=%d name=%s", index,
                            app.name.c_str());
        saveSettings();
        refreshSettingsDialog();
        publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
    });
    idleAppNameDialog = std::move(dialog);
    idleAppNameDialog->show();
}

void App::editIdleWelcome() {
    if (idleWelcomeDialog && idleWelcomeDialog->isOpen()) {
        SetForegroundWindow(idleWelcomeDialog->hwnd());
        return;
    }
    idleWelcomeDialog.reset();

    auto dialog = std::make_unique<IdleAppNameDialog>();
    HWND parent = settingsDialog ? settingsDialog->hwnd() : trayHwnd;
    if (!dialog->create(GetModuleHandleW(nullptr), parent, settings_.idleCustomWelcome_,
                        L"自定义欢迎语", L"最多 20 个字符，留空后恢复默认欢迎语", L"输入欢迎语",
                        static_cast<int>(kIdleCustomWelcomeMaxLen)))
        return;
    dialog->setApplyCallback([this](const std::wstring& text) {
        const std::wstring normalized = normalizeIdleCustomWelcome(text);
        if (settings_.idleCustomWelcome_ == normalized)
            return;
        settings_.idleCustomWelcome_ = normalized;
        runtime_log::writef(L"[action][idle-quote] custom-welcome len=%llu",
                            static_cast<unsigned long long>(normalized.size()));
        saveSettings();
        publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
    });
    idleWelcomeDialog = std::move(dialog);
    idleWelcomeDialog->show();
}

void App::removeIdleApp(int index) {
    if (index < 0 || static_cast<size_t>(index) >= settings_.idleApps_.size())
        return;
    runtime_log::writef(L"[action][idle-entry] app-removed path=%s",
                        settings_.idleApps_[static_cast<size_t>(index)].path.c_str());
    settings_.idleApps_.erase(settings_.idleApps_.begin() + index);
    saveSettings();
    refreshSettingsDialog();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
}

void App::reorderIdleApps(int fromIndex, int toIndex) {
    if (fromIndex < 0 || static_cast<size_t>(fromIndex) >= settings_.idleApps_.size() ||
        toIndex < 0 || toIndex > static_cast<int>(settings_.idleApps_.size()))
        return;
    if (toIndex == fromIndex || toIndex == fromIndex + 1)
        return;

    const std::wstring path = settings_.idleApps_[static_cast<size_t>(fromIndex)].path;
    auto begin = settings_.idleApps_.begin();
    if (toIndex > fromIndex) {
        std::rotate(begin + fromIndex, begin + fromIndex + 1, begin + toIndex);
    } else {
        std::rotate(begin + toIndex, begin + fromIndex, begin + fromIndex + 1);
    }
    runtime_log::writef(L"[action][idle-entry] app-reordered from=%d to=%d path=%s", fromIndex,
                        toIndex, path.c_str());
    saveSettings();
    refreshSettingsDialog();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
}

void App::pickIdleApp() {
    if (idleAppPickerOpen_)
        return;

    idleAppPickerOpen_ = true;
    runtime_log::writef(L"[action][idle-entry] choose-app start");
    const DWORD mainThreadId = mainThread;

    // 主线程由 C++/WinRT 初始化为 MTA；IFileDialog 的模态界面放到独立 STA，
    // 避免在设置窗口的消息处理过程中同步 Show 导致整个 UI 无响应。
    std::thread([mainThreadId] {
        std::wstring selectedPath;
        const HRESULT init =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(init)) {
            IFileOpenDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog)))) {
                const COMDLG_FILTERSPEC filters[] = {{L"应用程序 (*.exe)", L"*.exe"}};
                DWORD options = 0;
                if (SUCCEEDED(dialog->GetOptions(&options)))
                    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                       FOS_PATHMUSTEXIST);
                dialog->SetFileTypes(_countof(filters), filters);
                dialog->SetTitle(L"添加可打开的应用");
                if (SUCCEEDED(dialog->Show(nullptr))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                            selectedPath = path;
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            CoUninitialize();
        }

        runtime_log::writef(L"[action][idle-entry] choose-app result=%s path=%s",
                            selectedPath.empty() ? L"cancelled" : L"selected",
                            selectedPath.c_str());
        auto* payload = new IdleAppPayload{std::move(selectedPath)};
        if (!PostThreadMessageW(mainThreadId, kMsgIdleAppReady, 0,
                                reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void App::showManualSearch(HINSTANCE inst) {
    runtime_log::writef(L"[action][dialog] open=manual-search title=%s artist=%s",
                        playback_.state().currentTitle.c_str(),
                        playback_.state().currentArtist.c_str());
    if (manualSearchDialog && !manualSearchDialog->isOpen()) {
        manualSearchDialog.reset();
    }
    if (manualSearchDialog) {
        manualSearchDialog->show();
        return;
    }
    const SmtcSnapshot manualTarget = monitor.snapshot();
    const std::wstring targetNeteaseSongId =
        manualTarget.player == SmtcPlayerType::NetEase ? manualTarget.neteaseSongId : L"";
    manualSearchTrackKey = playback_.state().currentKey;
    manualSearchDialog = std::make_unique<ManualSearchDialog>();
    if (!manualSearchDialog->create(
            inst, trayHwnd, &playback_.lyricProvider(), playback_.state().currentTitle,
            playback_.state().currentArtist, playback_.state().currentDurationMs,
            manualTarget.player == SmtcPlayerType::NetEase, targetNeteaseSongId)) {
        manualSearchDialog.reset();
        return;
    }
    manualSearchDialog->setApplyCallback([this](LyricResult result) {
        SmtcSnapshot snap = monitor.snapshot();
        if (manualSearchTrackKey.empty() || !snap.sessionAlive ||
            playback_.state().currentKey != manualSearchTrackKey ||
            makeTrackKey(snap) != manualSearchTrackKey)
            return;

        playback_.applyManualLyrics(std::move(result), snap);
    });
    manualSearchDialog->show();
}

void App::disconnectTickTick() {
    tickTickEnableAfterTokenSave_ = false;
    idle_.disconnectTickTick();
}

void App::editTickTickApiToken(bool enableAfterSave) {
    if (tickTickApiTokenDialog && tickTickApiTokenDialog->isOpen()) {
        if (enableAfterSave)
            tickTickEnableAfterTokenSave_ = true;
        SetForegroundWindow(tickTickApiTokenDialog->hwnd());
        return;
    }
    tickTickApiTokenDialog.reset();
    tickTickEnableAfterTokenSave_ = enableAfterSave;

    auto dialog = std::make_unique<IdleAppNameDialog>();
    HWND parent = settingsDialog ? settingsDialog->hwnd() : trayHwnd;
    if (!dialog->create(
            GetModuleHandleW(nullptr), parent, std::wstring(), L"设置滴答清单 API 口令",
            L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建并复制；留空可清除",
            L"输入 API 口令", 256)) {
        tickTickEnableAfterTokenSave_ = false;
        return;
    }
    dialog->setApplyCallback([this](const std::wstring& text) {
        const bool enableAfterSave = tickTickEnableAfterTokenSave_;
        tickTickEnableAfterTokenSave_ = false;
        idle_.saveTickTickApiToken(text, enableAfterSave);
    });
    tickTickApiTokenDialog = std::move(dialog);
    tickTickApiTokenDialog->show();
}

void App::showSettingsError(const wchar_t* operation, const std::wstring& message) {
    runtime_log::writef(L"[settings] %s failed path=%s", operation, settingsPath_.c_str());
    message_dialog::showModal(GetModuleHandleW(nullptr), trayHwnd, L"设置文件提示", message,
                              L"确定");
}

void App::showRuntimeLog() {
    runtime_log::writef(L"[action][dialog] open=runtime-log");
    if (runtimeLogDialog && !runtimeLogDialog->isOpen())
        runtimeLogDialog.reset();
    if (!runtimeLogDialog) {
        runtimeLogDialog = std::make_unique<RuntimeLogDialog>();
        if (!runtimeLogDialog->create(
                GetModuleHandleW(nullptr), trayHwnd, &runtimeLogger_,
                [this](const std::wstring& path) { setLogDirectory(path); },
                [this](int days) { setLogRetentionDays(days); })) {
            runtimeLogDialog.reset();
            return;
        }
    }
    runtimeLogDialog->show();
}

void App::pickQqLocalLyricsPath() {
    if (qqLocalLyricsPickerOpen_)
        return;

    runtime_log::writef(L"[action][qq-local-lyrics] choose-directory start current=%s",
                        settings_.qqLocalLyricsPath_.c_str());
    qqLocalLyricsPickerOpen_ = true;
    const DWORD mainThreadId = mainThread;
    const std::wstring initialPath = settings_.qqLocalLyricsPath_;

    // IFileDialog 要求 STA；主线程由 C++/WinRT 初始化为 MTA，单独在线程 STA 中打开。
    std::thread([mainThreadId, initialPath] {
        std::wstring selectedPath;
        const HRESULT init =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(init)) {
            IFileDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog)))) {
                DWORD options = 0;
                if (SUCCEEDED(dialog->GetOptions(&options)))
                    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                dialog->SetTitle(L"选择 QQ 音乐本地歌词目录");

                if (!initialPath.empty()) {
                    IShellItem* folder = nullptr;
                    if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.c_str(), nullptr,
                                                              IID_PPV_ARGS(&folder)))) {
                        dialog->SetFolder(folder);
                        folder->Release();
                    }
                }

                if (SUCCEEDED(dialog->Show(nullptr))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            selectedPath = path;
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            CoUninitialize();
        }

        runtime_log::writef(L"[action][qq-local-lyrics] choose-directory result=%s path=%s",
                            selectedPath.empty() ? L"cancelled" : L"selected",
                            selectedPath.c_str());
        auto* payload = new QqLocalFolderPayload{std::move(selectedPath)};
        if (!PostThreadMessageW(mainThreadId, kMsgQqLocalFolderReady, 0,
                                reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void App::showUpdatePrompt(const std::wstring& latestVersion) {
    if (latestVersion.empty())
        return;
    runtime_log::writef(L"[action][update] prompt version=%s source=%s", latestVersion.c_str(),
                        settings_.useGiteeUpdateSource_ ? L"gitee" : L"github");
    if (updatePromptHwnd_) {
        SetForegroundWindow(updatePromptHwnd_);
        return;
    }

    updatePromptLatestVersion_ = latestVersion;
    updatePromptUseGiteeSource_ = settings_.useGiteeUpdateSource_;
    updatePromptHoverId_ = 0;
    updatePromptPressedId_ = 0;
    updatePromptFocusedId_ = kUpdatePromptAbout;
    updatePromptFocusVisible_ = false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = App::updatePromptWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QQMusicLyricUpdatePrompt";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(500.0f * s)),
            static_cast<LONG>(std::lround(280.0f * s))};
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU;
    constexpr DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"发现新版本", style, x, y, w, h,
                                nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) {
        updatePromptLatestVersion_.clear();
        return;
    }
    fluent::ensureDialogMinimumSize(hwnd, style, exStyle, 500.0f, 280.0f);
    app_icon::applyWindowIcon(hwnd);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void App::layoutUpdatePrompt() {
    if (!updatePromptHwnd_)
        return;

    RECT rc{};
    GetClientRect(updatePromptHwnd_, &rc);
    const float s = updatePromptSurface_.dipScale();
    const float w = std::max(0.0f, static_cast<float>(rc.right - rc.left) / s);
    const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / s);
    const float pad = fluent::metrics::pagePadding;
    const float gap = fluent::metrics::controlGap;
    const float titleH = 30.0f;
    const float subtitleH = 20.0f;
    const float titleW = 112.0f;
    const float sourceX = pad + titleW + fluent::metrics::compactGap;
    const float sourceY = pad + 8.0f;
    updatePromptTitleRect_ = D2D1::RectF(pad, pad, pad + titleW, pad + titleH);
    updatePromptSourceRect_ =
        D2D1::RectF(sourceX, sourceY, std::max(sourceX, w - pad), pad + titleH);
    updatePromptSubtitleRect_ =
        D2D1::RectF(pad, pad + titleH, std::max(pad, w - pad), pad + titleH + subtitleH);

    const float cardY = pad + titleH + subtitleH + fluent::metrics::compactGap;
    const float cardH = 102.0f;
    updatePromptVersionCardRect_ = D2D1::RectF(pad, cardY, std::max(pad, w - pad), cardY + cardH);
    const float contentX = pad + 16.0f;
    const float contentW = std::max(20.0f, w - contentX - pad - 16.0f);
    updatePromptCurrentVersionRect_ =
        D2D1::RectF(contentX, cardY + 17.0f, contentX + contentW, cardY + 41.0f);
    updatePromptLatestVersionRect_ =
        D2D1::RectF(contentX, cardY + 57.0f, contentX + contentW, cardY + 81.0f);

    const float buttonH = fluent::metrics::controlHeight;
    const float buttonY = h - pad - buttonH;
    const float releaseW = 128.0f;
    const float downloadW = 112.0f;
    const float aboutW = 128.0f;
    const float totalW = releaseW + downloadW + aboutW + gap * 2.0f;
    const float buttonX = std::max(pad, (w - totalW) * 0.5f);
    updatePromptReleasePageRect_ =
        D2D1::RectF(buttonX, buttonY, buttonX + releaseW, buttonY + buttonH);
    updatePromptDownloadRect_ =
        D2D1::RectF(updatePromptReleasePageRect_.right + gap, buttonY,
                    updatePromptReleasePageRect_.right + gap + downloadW, buttonY + buttonH);
    updatePromptAboutRect_ =
        D2D1::RectF(updatePromptDownloadRect_.right + gap, buttonY,
                    updatePromptDownloadRect_.right + gap + aboutW, buttonY + buttonH);
}

void App::drawUpdatePromptButton(fluent::FluentDialogSurface::Painter& painter,
                                 const D2D1_RECT_F& rect, const wchar_t* text, bool accent,
                                 int id) {
    const auto& p = fluent::palette();
    const bool hovered = updatePromptHoverId_ == id;
    const bool pressed = updatePromptPressedId_ == id;
    const D2D1_COLOR_F fill = accent    ? pressed   ? p.accentPressed
                                          : hovered ? p.accentHover
                                                    : p.accent
                              : pressed ? p.controlPressed
                              : hovered ? p.controlHover
                                        : p.controlFill;
    const D2D1_COLOR_F textColor = accent ? p.textOnAccent : p.text;
    painter.fillRoundRect(fill, rect);
    if (!accent)
        painter.strokeRoundRect(p.controlStroke, rect);
    if (updatePromptFocusedId_ == id && updatePromptFocusVisible_) {
        painter.strokeRoundRect(
            accent ? p.textOnAccent : p.accent,
            D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f, rect.bottom - 1.5f),
            1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
    }
    painter.drawText(std::wstring(text), painter.textFormat(14.0f, 400, true, true),
                     D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f, rect.bottom),
                     textColor);
}

void App::paintUpdatePrompt(fluent::FluentDialogSurface::Painter& painter, float, float) {
    const auto& p = fluent::palette();
    painter.drawText(L"发现新版本", painter.textFormat(21.0f, 600), updatePromptTitleRect_, p.text);
    painter.drawText(std::wstring(L"更新源：") +
                         (updatePromptUseGiteeSource_ ? L"Gitee" : L"GitHub"),
                     painter.textFormat(13.0f, 400), updatePromptSourceRect_, p.textSecondary);
    painter.drawText(L"检测到新的正式版本，可选择查看详情或立即更新",
                     painter.textFormat(13.0f, 400), updatePromptSubtitleRect_, p.textSecondary);

    painter.fillRoundRect(p.cardFill, updatePromptVersionCardRect_, fluent::metrics::cardRadius);
    painter.strokeRoundRect(p.cardStroke, updatePromptVersionCardRect_, 1.0f,
                            fluent::metrics::cardRadius);
    painter.drawText(std::wstring(L"当前版本：") + app_info::kVersion,
                     painter.textFormat(14.0f, 400), updatePromptCurrentVersionRect_, p.text);
    painter.drawText(std::wstring(L"最新版本：") + updatePromptLatestVersion_,
                     painter.textFormat(14.0f, 600), updatePromptLatestVersionRect_, p.text);

    drawUpdatePromptButton(painter, updatePromptReleasePageRect_, L"跳转到发布页", false,
                           kUpdatePromptReleasePage);
    drawUpdatePromptButton(painter, updatePromptDownloadRect_, L"下载更新", true,
                           kUpdatePromptDownload);
    drawUpdatePromptButton(painter, updatePromptAboutRect_, L"进入关于页面", false,
                           kUpdatePromptAbout);
}

int App::hitTestUpdatePrompt(float x, float y) const {
    auto contains = [x, y](const D2D1_RECT_F& rect) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    };
    if (contains(updatePromptReleasePageRect_))
        return kUpdatePromptReleasePage;
    if (contains(updatePromptDownloadRect_))
        return kUpdatePromptDownload;
    if (contains(updatePromptAboutRect_))
        return kUpdatePromptAbout;
    return 0;
}

void App::focusStepUpdatePrompt(int direction) {
    constexpr int ids[] = {kUpdatePromptReleasePage, kUpdatePromptDownload, kUpdatePromptAbout};
    int index = 0;
    for (int i = 0; i < static_cast<int>(std::size(ids)); ++i) {
        if (ids[i] == updatePromptFocusedId_) {
            index = i;
            break;
        }
    }
    index =
        (index + direction + static_cast<int>(std::size(ids))) % static_cast<int>(std::size(ids));
    updatePromptFocusedId_ = ids[index];
    updatePromptFocusVisible_ = true;
    updatePromptSurface_.invalidate();
}

void App::closeUpdatePrompt() {
    if (updatePromptHwnd_)
        DestroyWindow(updatePromptHwnd_);
}

void App::onUpdatePromptCommand(int command) {
    if (command != kUpdatePromptReleasePage && command != kUpdatePromptDownload &&
        command != kUpdatePromptAbout)
        return;

    const wchar_t* action = command == kUpdatePromptReleasePage ? L"open-release-page"
                            : command == kUpdatePromptDownload  ? L"download"
                                                                : L"open-about";
    runtime_log::writef(L"[action][update-prompt] command=%s", action);
    const bool useGiteeSource = updatePromptUseGiteeSource_;
    closeUpdatePrompt();
    if (command == kUpdatePromptReleasePage) {
        const wchar_t* releasePage =
            useGiteeSource ? app_info::kGiteeLatestReleasePage : app_info::kLatestReleasePage;
        ShellExecuteW(nullptr, L"open", releasePage, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (command == kUpdatePromptDownload) {
        showAbout(true);
    } else {
        showAbout();
    }
}

void App::pickFont() {
    runtime_log::writef(L"[action][dialog] open=font-picker current-family=%s size=%.1f style=%s",
                        settings_.fontFamily_.c_str(), static_cast<double>(settings_.fontSize_),
                        fontStyleLabel(settings_.fontStyle_));
    if (!taskbarHost)
        return;
    if (fontPickerDialog && fontPickerDialog->isOpen()) {
        SetForegroundWindow(fontPickerDialog->hwnd());
        return;
    }
    fontPickerDialog = std::make_unique<FontPickerDialog>();
    if (!fontPickerDialog->create(GetModuleHandleW(nullptr), trayHwnd, settings_.fontFamily_,
                                  settings_.fontSize_, settings_.fontStyle_)) {
        fontPickerDialog.reset();
        return;
    }
    fontPickerDialog->setSizeContextCallback([this]() {
        std::vector<LyricFontSizeContext> contexts;
        forEachHost([&](TaskbarHost* host) {
            auto context = host->fontSizeContext();
            if (context.availableDip > 0.0f)
                contexts.push_back(context);
        });
        return contexts;
    });
    fontPickerDialog->setApplyCallback([this](const std::wstring& family, float size,
                                              LyricFontStyle style) {
        settings_.fontFamily_ = family;
        settings_.fontSize_ = std::clamp(size, 4.0f, 96.0f);
        settings_.fontStyle_ = style;
        settings_.hasUserFont_ = true;
        runtime_log::writef(L"[action][font] applied family=%s size=%.1f style=%s",
                            settings_.fontFamily_.c_str(), static_cast<double>(settings_.fontSize_),
                            fontStyleLabel(settings_.fontStyle_));
        forEachHost([&](TaskbarHost* host) {
            host->setFont(settings_.fontFamily_, settings_.fontSize_, settings_.fontStyle_);
        });
        saveSettings();
        if (settingsDialog)
            settingsDialog->updateFontDescription(currentSettingsState().fontDesc);
    });
    fontPickerDialog->show();
}

void App::showFontColorDialog() {
    runtime_log::writef(L"[action][dialog] open=font-color");
    if (!taskbarHost)
        return;
    if (fontColorDialog && fontColorDialog->isOpen()) {
        SetForegroundWindow(fontColorDialog->hwnd());
        return;
    }
    fontColorDialog = std::make_unique<FontColorDialog>();
    FontColorDialog::State st{};
    st.global = settings_.lyricAppearanceGlobal_;
    st.hasGlobalTheme = settings_.hasGlobalLyricAppearance_;
    st.light = settings_.lightLyricAppearance_;
    st.dark = settings_.darkLyricAppearance_;
    st.globalTheme = settings_.globalLyricAppearance_;
    st.fontFamily = settings_.fontFamily_;
    st.fontStyle = settings_.fontStyle_;
    // 与任务栏实际渲染字号一致：setFont 钳制 9..18，绘制时放大 1.18 倍
    float baseSize = settings_.hasUserFont_ ? settings_.fontSize_ : 12.0f;
    st.lyricFontSize = std::clamp(baseSize, 9.0f, 18.0f) * 1.18f;
    if (!fontColorDialog->create(GetModuleHandleW(nullptr), trayHwnd, st)) {
        fontColorDialog.reset();
        return;
    }
    fontColorDialog->setApplyCallback([this](const FontColorDialog::Result& r) {
        settings_.lyricAppearanceGlobal_ = r.global;
        settings_.hasGlobalLyricAppearance_ = r.hasGlobalTheme;
        settings_.lightLyricAppearance_ = r.light;
        settings_.darkLyricAppearance_ = r.dark;
        settings_.globalLyricAppearance_ = r.globalTheme;
        runtime_log::writef(L"[action][font-color] applied global=%d has-global=%d",
                            r.global ? 1 : 0, r.hasGlobalTheme ? 1 : 0);
        applyFontAppearance();
        saveSettings();
    });
    fontColorDialog->show();
}

void App::showSettings() {
    runtime_log::writef(L"[action][dialog] open=settings");
    syncTaskbarOrientation();
    if (settingsDialog && !settingsDialog->isOpen())
        settingsDialog.reset();

    if (!settingsDialog) {
        settingsDialog = std::make_unique<SettingsDialog>();
        if (!settingsDialog->create(GetModuleHandleW(nullptr), trayHwnd, currentSettingsState(),
                                    buildSettingsActions())) {
            settingsDialog.reset();
            return;
        }
    } else if (IsWindowVisible(settingsDialog->hwnd())) {
        // 窗口已打开：仅激活到前台。再走 updateState 会触发全量 layout 重排，
        // 十几个分层子控件同时 SetWindowPos/Z-order 调整会造成可见闪烁。
        if (IsIconic(settingsDialog->hwnd()))
            ShowWindow(settingsDialog->hwnd(), SW_RESTORE);
        SetForegroundWindow(settingsDialog->hwnd());
        return;
    } else {
        // 启动时窗口已预创建但仍隐藏，打开前同步最新状态快照。
        refreshSettingsDialog();
    }
    settingsDialog->show();
}

void App::onDialogClosed(DialogKind kind) {
    runtime_log::writef(L"[action][dialog] closed kind=%d", static_cast<int>(kind));
    auto resetIfClosed = [](auto& dialog) {
        if (dialog && !dialog->isOpen())
            dialog.reset();
    };

    switch (kind) {
    case DialogKind::Settings:
        resetIfClosed(settingsDialog);
        break;
    case DialogKind::About:
        resetIfClosed(aboutDialog);
        break;
    case DialogKind::ManualSearch:
        resetIfClosed(manualSearchDialog);
        break;
    case DialogKind::FontPicker:
        resetIfClosed(fontPickerDialog);
        break;
    case DialogKind::FontColor:
        resetIfClosed(fontColorDialog);
        break;
    case DialogKind::RuntimeLog:
        resetIfClosed(runtimeLogDialog);
        break;
    }
}

void App::showAbout(bool downloadUpdate) {
    runtime_log::writef(L"[action][dialog] open=about download-update=%d", downloadUpdate ? 1 : 0);
    if (aboutDialog && aboutDialog->isOpen()) {
        aboutDialog->show(downloadUpdate);
        return;
    }
    aboutDialog = std::make_unique<AboutDialog>();
    if (!aboutDialog->create(
            GetModuleHandleW(nullptr), trayHwnd, settings_.autoCheckOnStartup_,
            settings_.useGiteeUpdateSource_,
            [this](bool enabled) { setAutoCheckOnStartup(enabled); },
            [this](bool useGitee) {
                if (settings_.useGiteeUpdateSource_ == useGitee)
                    return;
                settings_.useGiteeUpdateSource_ = useGitee;
                runtime_log::writef(L"[action][update] source=%s", useGitee ? L"gitee" : L"github");
                saveSettings();
            },
            [this](const std::wstring& path) { return launchUpdateInstaller(path); },
            [this](const std::wstring& version) { showUpdatePrompt(version); })) {
        aboutDialog.reset();
        return;
    }
    aboutDialog->show(downloadUpdate);
}

bool App::launchUpdateInstaller(const std::wstring& path) {
    if (path.empty())
        return false;
    runtime_log::writef(L"[action][update] installer-start path=%s", path.c_str());
    // 软件内更新使用 Inno Setup 的标准参数，让安装器在安装阶段接管仍存活的旧进程；
    // 外部直接运行安装包不带该参数，仍保持手动退出策略。
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), L"/CLOSEAPPLICATIONS", nullptr,
                                     SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        runtime_log::writef(L"[action][update] installer-start result=failed");
        return false;
    }
    runtime_log::writef(L"[action][update] installer-start result=ok");
    // 更新安装器已经接管后，不能再等待任务栏探测、网络请求等退出析构路径。
    // 这些同步清理在部分电脑上可能长时间阻塞，导致前台窗口和托盘已消失，
    // 但 QQMusicLyric.exe 仍存活，使 Restart Manager 无法再次关闭它。
    // ExitProcess 会立即释放本进程持有的文件句柄，交由已启动的安装器继续覆盖文件。
    ExitProcess(0);
    return true;
}
} // namespace lyrics_app
