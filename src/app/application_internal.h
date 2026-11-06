#pragma once
#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include "idle/idle_quote_provider.h"
#include "idle/ticktick_provider.h"
#include "app_info.h"
#include "ui/font_style.h"
#include "ui/lyric_window.h"
#include "ui/taskbar_host.h"
#include "ui/app_icon.h"
#include "ui/about_dialog.h"
#include "ui/manual_search_dialog.h"
#include "ui/font_picker_dialog.h"
#include "ui/font_color_dialog.h"
#include "ui/idle_app_name_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/song_toast.h"
#include "ui/runtime_log_dialog.h"
#include "ui/dialog_notify.h"
#include "ui/message_dialog.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_menu.h"
#include "ui/fluent_theme.h"
#include "ui/settings_icons.h"
#include "ui/platform_icon.h"
#include "media/smtc_monitor.h"
#include "media/audio_spectrum.h"
#include "media/app_volume.h"
#include "media/playback_controller.h"
#include "media/track_identity.h"
#include "logging/runtime_logger.h"
#include "settings/settings_store.h"
#include "settings/app_settings.h"
#include "model/taskbar_appearance.h"
#include "idle/idle_controller.h"
#include "app/messages.h"
#include "app/presentation_model.h"

#include <windows.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cwctype>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <io.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lyrics_app {

// QQ 切歌时 SMTC 把媒体属性与时间线拆成多条事件投递，歌词请求延迟到这批事件
// 合并完成后发出，避免按不完整的标题/歌手/时长先失败一次（界面闪「暂无歌词」）。
constexpr UINT_PTR kTimerLyricDebounce = 3;
// 切歌弹窗等待封面的超时：SMTC 把封面拆在切歌事件批的后续事件里投递，
// 短暂等待可以带封面弹出；超时则按无封面弹出，不再干等。
constexpr UINT_PTR kTimerSongToastCover = 4;
// 每日一言只按本地周期检查；命中同一周期时不会重复请求网络。
constexpr UINT_PTR kTimerIdleQuote = 5;
// 开机自启时网络或滴答清单服务可能尚未就绪；启动同步失败后按递增间隔补试，
// 避免一次瞬时网络失败让今日任务在本次运行中一直缺席。
constexpr UINT_PTR kTimerTaskbarAutoRestore = 7;
constexpr UINT_PTR kTimerBackgroundSettingsSave = 8;
constexpr UINT_PTR kTimerTaskbarDisplayRebuildRetry = 9;
constexpr UINT kTaskbarAutoRestoreMs = 1000;
constexpr UINT kTaskbarDisplayRebuildRetryMs = 500;
constexpr int kTaskbarDisplayRebuildMaxRetries = 6;
constexpr UINT kSongToastCoverWaitMs = 350;
constexpr UINT kBackgroundSettingsSaveDebounceMs = 300;
constexpr UINT kIdleQuoteCheckMs = 60 * 1000;
constexpr UINT kTrayMsg = WM_APP + 200;
constexpr UINT kTrayIconId = 1;
constexpr UINT kCmdToggleTaskbar = 101;
constexpr UINT kCmdPickFont = 105;
constexpr UINT kCmdExit = 106;
constexpr UINT kCmdManualSearch = 107;
constexpr UINT kCmdFontColorEffect = 108;
constexpr UINT kCmdTaskbarPosNotify = 109;
constexpr UINT kCmdTaskbarPosLeft = 110;
constexpr UINT kCmdSpectrum = 111;
constexpr UINT kCmdAutoStart = 112;
constexpr UINT kCmdFollowAlbum = 113;
constexpr UINT kCmdSecondaryTranslation = 114;
constexpr UINT kCmdSecondaryRomanization = 115;
constexpr UINT kCmdSongInfo = 116;
constexpr UINT kCmdAlbumCover = 117;
constexpr UINT kCmdAlbumCoverEffectDefault = 118;
constexpr UINT kCmdAlbumCoverEffectVinyl = 119;
constexpr UINT kCmdDoubleLineLyrics = 120;
constexpr UINT kCmdPlatformIcon = 121;
constexpr UINT kCmdAbout = 122;
constexpr UINT kCmdLyricAlignLeft = 123;
constexpr UINT kCmdLyricAlignCenter = 124;
constexpr UINT kCmdLyricAlignRight = 125;
constexpr UINT kCmdHoverPlaybackControls = 126;
constexpr UINT kCmdSettings = 127;
constexpr UINT kCmdSwitchLyricSource = 128;
constexpr UINT kCmdRuntimeLog = 129;
constexpr UINT kCmdRenderModeNormal = 130;
constexpr UINT kCmdRenderModeLow = 131;
constexpr UINT kCmdRenderModeStopped = 132;
constexpr UINT kCmdRenderModeMinimal = 133;
constexpr UINT kCmdSecondaryOff = 134;
constexpr UINT kCmdTaskbarModeEmbedded = 135;
constexpr UINT kCmdTaskbarModeImmersive = 136;
constexpr UINT kCmdTaskbarModeAppBar = 137;
constexpr UINT kCmdAppBarTop = 138;
constexpr UINT kCmdAppBarBottom = 139;
constexpr UINT kCmdRestart = 140;
constexpr int kCmdTaskbarMonitorAll = 150;
constexpr int kCmdTaskbarMonitorBase = 151;
constexpr int kDockQuickAppCommandBase = 21000;
constexpr int kUpdatePromptReleasePage = 1;
constexpr int kUpdatePromptDownload = 2;
constexpr int kUpdatePromptAbout = 3;
enum class TaskbarCreateResult {
    Created,
    NoSpace,
    Failed,
};

constexpr size_t kIdleCustomWelcomeMaxLen = 20;

// 开机自启动：HKCU\...\Run（仅当前用户，无需管理员权限；注册表即真源，菜单勾选态实时读取）
constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kAutoStartValueName = L"QQMusicLyric";

const wchar_t* trayCommandName(int command);
const wchar_t* coverSourceDisplayName(CoverSource source);
COLORREF defaultFloatingCardBackgroundColor();
const wchar_t* fontStyleLabel(LyricFontStyle style);
int spectrumStyleIndex(SpectrumStyle style);
int idleQuoteBackgroundIndex(IdleQuoteBackground background);
int idleQuoteBackgroundScopeIndex(IdleQuoteBackgroundScope scope);
std::wstring normalizeIdleCustomWelcome(std::wstring text);
bool validExePath(const std::wstring& path);
std::wstring fallbackExeName(const std::wstring& path);
std::wstring configDir();
bool autoStartEnabled();
bool setAutoStart(bool enable);
const wchar_t* spectrumProcessName(SmtcPlayerType player);
std::vector<std::wstring> volumeProcessNames(SmtcPlayerType player);
fluent::NativeMenuIcon ownMenuIcon(HICON icon);
fluent::NativeMenuIcon shellIconForPath(const std::wstring& path);

struct QqLocalFolderPayload {
    std::wstring path;
};

struct IdleAppPayload {
    std::wstring path;
};

class App {
public:
    App() = default;
    ~App() = default;
    bool initialize(HINSTANCE inst, bool wantTaskbar);
    int messageLoop();
    void finish();

private:
    void dispatchThreadMessage(const MSG& msg);
    bool translateDialogMessage(MSG& msg);
    AppSettings settings_;
    DWORD mainThread = GetCurrentThreadId();
    runtime_log::RuntimeLogger runtimeLogger_;
    SmtcMonitor monitor;
    PlaybackController playback_{[this] { return monitor.snapshot(); },
                                 [this](const SmtcSnapshot& snap, PresentationUpdate update) {
                                     publishPresentationFrame(snap, update);
                                 },
                                 [this](const SmtcSnapshot& snap) { updateRuntimeLogState(snap); },
                                 [this] { tryExtractAlbumColor(); }};
    IdleController idle_{
        settings_,
        [this] { saveSettings(); },
        [this] { publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true}); },
        [this] { refreshSettingsDialog(); },
        [this] { return monitor.snapshot().sessionAlive; },
        [this] { return taskbarVertical_; }};
    AppVolumeController appVolume_;           // 当前音乐应用的独立音量（音量合成器中该应用的一格）
    AppVolumeState appVolumeState_;           // 最近推送给宿主的音量状态（去重用）
    std::unique_ptr<TaskbarHost> taskbarHost; // 具体类型：歌词描边光晕是任务栏独有接口
    std::vector<std::unique_ptr<TaskbarHost>> additionalTaskbarHosts_;
    std::vector<std::wstring> activeTaskbarDisplayIds_;
    std::vector<std::pair<int, std::wstring>> taskbarMonitorMenuItems_;
    bool taskbarAllowOverlap_ = false;
    bool taskbarNoSpaceClosePending_ = false;
    bool taskbarRebuildPending_ = false;
    bool taskbarPendingDisplayRebuild_ = false;
    bool taskbarRebuildAllowOverlap_ = false;
    int taskbarDisplayRebuildRetries_ = 0;
    // 任务栏空间不足时自动隐藏；宿主本身继续保留并后台探测空间变化。
    bool taskbarAutoClosedForNoSpace_ = false;
    bool taskbarSpectrumSuppressedForNoSpace_ = false;
    bool taskbarSpectrumRestoring_ = false;
    bool taskbarAutoRestorePending_ = false;
    bool taskbarManualOpenPending_ = false;
    // 完全停止模式只是临时隐藏任务栏歌词；退出时按进入前的用户开启状态恢复。
    // 在退出后的重新探测期间保留该状态，窗口恢复显示后由实际显示状态接管。
    bool taskbarEnabledBeforeStopped_ = false;
    std::unique_ptr<AboutDialog> aboutDialog;
    std::unique_ptr<ManualSearchDialog> manualSearchDialog;
    std::wstring manualSearchTrackKey;
    std::unique_ptr<FontPickerDialog> fontPickerDialog;
    std::unique_ptr<FontColorDialog> fontColorDialog;
    std::unique_ptr<IdleAppNameDialog> idleAppNameDialog;
    std::unique_ptr<IdleAppNameDialog> idleWelcomeDialog;
    std::unique_ptr<IdleAppNameDialog> tickTickApiTokenDialog;
    std::unique_ptr<SettingsDialog> settingsDialog;
    std::unique_ptr<RuntimeLogDialog> runtimeLogDialog;
    bool songToastCoverWaitArmed_ = false; // 切歌弹窗正在等待封面补齐
    PresentationModel presentation_;

    HWND trayHwnd = nullptr;
    std::vector<std::wstring> dockQuickAppActions_;
    UINT taskbarCreatedMsg_ =
        0; // Explorer 重启广播（只有顶层窗口收得到，托盘窗口不能用 HWND_MESSAGE）
    bool shutdownRequested_ = false;

    // 滴答清单今日任务：普通用户 API 口令由 TickTickProvider 保存在 Windows
    // 凭据管理器；这里只保留连接状态和当前展示快照。
    bool tickTickEnableAfterTokenSave_ = false;

    bool backgroundBlurSettingsSavePending_ = false;
    bool immersiveBackgroundBlurLogPending_ = false;
    bool dockBackgroundBlurLogPending_ = false;
    // 切歌弹窗（灵动岛）：主屏幕中下方短暂弹出歌曲信息，固定鼠标穿透
    std::unique_ptr<SongToast> songToast_;
    // 渲染模式：0 正常；1 低渲染（~30fps，降低 GPU/CPU 占用）；2 完全停止；
    // 3 极简（不降低歌词刷新率，只关闭附加视觉与弹窗）
    int renderMode_ = 0;
    bool taskbarVertical_ = false; // 当前任务栏是否为左右侧的竖向布局
    bool taskbarCompact_ = false;
    bool mediaSessionAlive_ = false; // 用于同步依赖播放会话的设置可用态

    // 频谱（任务栏歌词独有）：开关持久化，开启时捕获线程跟随任务栏宿主启停
    AudioSpectrum spectrum_;
    bool spectrumSessionAlive_ = false;
    std::wstring spectrumSessionKey_;

    HWND updatePromptHwnd_ = nullptr;
    bool updatePromptBackdrop_ = false;
    fluent::FluentDialogSurface updatePromptSurface_;
    std::wstring updatePromptLatestVersion_;
    bool updatePromptUseGiteeSource_ = false;
    D2D1_RECT_F updatePromptTitleRect_{};
    D2D1_RECT_F updatePromptSubtitleRect_{};
    D2D1_RECT_F updatePromptVersionCardRect_{};
    D2D1_RECT_F updatePromptCurrentVersionRect_{};
    D2D1_RECT_F updatePromptLatestVersionRect_{};
    D2D1_RECT_F updatePromptSourceRect_{};
    D2D1_RECT_F updatePromptReleasePageRect_{};
    D2D1_RECT_F updatePromptDownloadRect_{};
    D2D1_RECT_F updatePromptAboutRect_{};
    int updatePromptHoverId_ = 0;
    int updatePromptPressedId_ = 0;
    int updatePromptFocusedId_ = kUpdatePromptAbout;
    bool updatePromptFocusVisible_ = false;
    std::wstring settingsPath_;
    bool qqLocalLyricsPickerOpen_ = false;
    bool idleAppPickerOpen_ = false;

    template <typename Fn> void forEachHost(Fn&& fn) {
        if (taskbarHost)
            fn(taskbarHost.get());
        for (auto& host : additionalTaskbarHosts_) {
            if (host)
                fn(host.get());
        }
    }

    std::vector<TaskbarDisplayInfo> desiredTaskbarDisplays(const SmtcSnapshot& snap) const;

    std::wstring taskbarDisplayLabel(const TaskbarDisplayInfo& display) const;

    std::wstring taskbarMonitorLabel() const;

    void selectTaskbarMonitor(const std::wstring& deviceName);

    void showTaskbarMonitorMenu();

    void updateRuntimeLogState(const SmtcSnapshot& snap);

    // 设置变更只有一个 UI 同步入口，避免每个动作都重复判断窗口生命周期。
    void refreshSettingsDialog(bool onlyWhenOpen = false);

    void enforceSecondaryLyricConstraint();

    void commitAppearanceSettings();

    void logSettingBool(const wchar_t* name, bool value);

    void logSettingInt(const wchar_t* name, int value);

    // 设置项统一直接生效入口：右键菜单与设置页共用同一套应用逻辑
    void applySongInfoVisible(bool on);

    void applyAlbumCoverVisible(bool on);

    void applyPlatformIconVisible(bool on);

    void applyCoverEffect(bool vinyl);

    void applySpectrumOn(bool on);

    void applySpectrumStyle(int style);

    void applySpectrumColorMode(int mode);

    void applySpectrumColor(COLORREF color);

    void applySpectrumGradient(bool on);

    void applySpectrumEffect(int effect, bool on);

    void applyLyricEffect(int effect, bool on);

    void applySpectrumBackground(bool on);

    void applySpectrumOpacity(int percent);

    void applyProgressBackground(bool on);

    void applyProgressBackgroundOpacity(int percent);

    void applyTaskbarBackground(int mode);

    void applyCoverBackgroundOpacity(int percent);

    bool taskbarExpandedConfigured() const;

    void requestTaskbarRebuild();

    void cancelTaskbarDisplayRebuildRetry();

    void scheduleTaskbarDisplayRebuildRetry(bool allowOverlap);

    // 仅由最外层主消息循环调用；重建期间的新显示通知留到下一轮处理。
    void rebuildTaskbarIfPending();

    void syncTaskbarView(const SmtcSnapshot& snap);

    void applyTaskbarViewMode(int mode);

    void applyAppBarEdge(int edge);

    void applyImmersiveBackgroundBlur(int percent);

    void applyDockBackgroundBlur(int percent);

    void scheduleBackgroundBlurSettingsSave(bool immersive);

    void flushBackgroundBlurSettingsSave();

    void applyImmersiveBackgroundAdjustment(int mode);

    void applyDockBackgroundAdjustment(int mode);

    void applyDockResourceVisibility();

    void applyEmbeddedResourceMonitor(bool on);

    void applyDockResourceGpuUsage(bool on);

    void applyDockResourceNetwork(bool download, bool on);

    void applyDockResourceCpuFrequency(bool on);

    bool isRenderMode(RenderMode mode) const;

    bool isMinimalRenderMode() const;

    bool isNormalRenderMode() const;

    // 各设置字段保存用户的常规配置；极简模式只在推送到宿主时临时覆盖有效值，
    // 退出后重新推送原配置，避免用户切换性能模式时丢失显示偏好。竖向任务栏则
    // 额外关闭不适合窄栏的有效显示，但歌词相关配置仍保留，切回横向即可继续使用。
    TaskbarAppearance appearanceForHost(bool vertical) const;
    void publishTaskbarAppearance();
    void applyEffectiveTaskbarSettings();

    // 频谱实际启停 = 用户开关 && 横向任务栏 && 正常渲染模式 && 宿主存在；
    // 低渲染/完全停止/极简模式及竖向任务栏都强制暂停捕获线程。
    void syncSpectrumWithMode();

    void tryRestoreSpectrumAfterNoSpace();

    void syncTaskbarOrientation();

    void applyRenderMode(int mode);

    void applyHoverControls(bool on);

    void applyTaskbarContextMenu(bool on);

    void applyHoverControlStyle(int style);

    void applyFloatingCardTrigger(int mode);

    void applyFloatingCardBackground(int mode);

    void applyFloatingCardFollowAlbum(bool on);

    void applyFloatingCardAutoTextContrast(bool on);

    void applySongToastEnabled(bool on);

    void applySongToastDuration(int seconds);

    void applySongToastSkipFullscreen(bool on);

    void applySongToastFollowMouseMonitor(bool follow);

    void applySongToastPosition(int position);

    // 前台窗口覆盖整块显示器（无边框全屏和独占全屏都命中）时视为全屏应用；
    // 桌面、任务栏等外壳窗口不算。
    bool foregroundFullscreen();

    void cancelSongToastCoverWait();

    // 切歌确认后弹出灵动岛；弹窗懒创建，关闭功能时不创建窗口。
    void notifySongToast();

    void refreshThemeWindows();

    void refreshTheme();

    void applyTaskbarTheme(fluent::ThemeMode mode);

    void applyWindowTheme(fluent::ThemeMode mode);

    void applyFollowAlbum(bool on);

    void applyDoubleLineLyrics(bool on);

    void applyLyricAlignment(int alignment);

    void applyIdleQuoteAlignment(int alignment);

    void applyIdleQuoteBackground(int background);

    void applyIdleQuoteBackgroundScope(int scope);

    void applySecondaryEnabled(bool on);

    void applyPreferRomanization(bool on);

    void applySecondaryChoice(bool romanization);

    void applyQqLocalLyricsEnabled(bool on);

    void applyQqLocalLyricsPersistOrder(bool on);

    void refreshIdleWelcome();

    void addIdleApp(const std::wstring& path);

    void editIdleApp(int index);

    void editIdleWelcome();

    void setIdleAppNamesVisible(bool show);

    void removeIdleApp(int index);

    void reorderIdleApps(int fromIndex, int toIndex);

    void pickIdleApp();

    const PresentationFrame& preparePresentationFrame(const SmtcSnapshot& snap,
                                                      PresentationUpdate update);

    void publishPresentationFrame(const SmtcSnapshot& snap, PresentationUpdate update);

    void armTaskbarAutoRestore();

    void cancelTaskbarAutoRestore();

    void tryRestoreTaskbarAfterNoSpace();

    void configureTaskbarHostCallbacks(TaskbarHost* host, bool primary);

    TaskbarCreateResult createTaskbar(HINSTANCE inst, bool allowOverlap = false);

    void destroyTaskbar();

    bool taskbarEnabledForUserAction();

    void restartApplication();

    void requestQuit();

    void toggleTaskbar();

    void showManualSearch(HINSTANCE inst);

    // 把当前播放状态同步给某个宿主（新建宿主时避免显示“等待播放…”）
    void syncHost(ILyricHost* host);

    void onControl(MediaControl c);

    // 读取当前音乐应用的会话音量并推送给各宿主；状态未变时跳过
    void pushAppVolume();

    // SMTC 来源播放器切换时更新音量控制目标进程；无会话时清空（音量不可用）
    void syncAppVolumeTarget(const SmtcSnapshot& snap);

    // 发起一次歌词请求：网易云逻辑保持不变（事件完整、可直接请求）；
    // QQ 由防抖定时器在切歌事件合并完成后调用。

    // 防抖到点：此时切歌事件批已处理完（线程消息优先于 WM_TIMER 派发），
    // 快照身份必然已等于 playback_.state().currentKey；不等说明还有未处理事件，直接放弃本轮，
    // 由该事件的 onSmtcChanged 重新武装。

    // 状态机：无会话(隐藏) -> 播放中(滚动渲染) <-> 暂停(静止显示)
    void onSmtcChanged();

    // 30fps：插值进度 -> 二分定位当前行
    void onFrame();

    // 统一托盘图标
    bool createTrayWindow(HINSTANCE inst);
    void destroyTray();
    void updateTrayIcon();
    std::vector<fluent::FluentMenuItem> buildMenuItems(bool fullTrayMenu);
    void showTrayMenu();
    void showTrayMenu(POINT screenPt);
    void showTaskbarMenu(POINT screenPt);
    void showDockQuickApps(POINT screenPt);
    std::vector<fluent::FluentMenuItem> buildDockQuickAppItems();
    void onDockQuickAppCommand(int command);
    void launchIdleApp(const std::wstring& path);
    void onMenuCommand(int cmd, const wchar_t* source);
    void showRuntimeLog();
    void initializeRuntimeLogger();
    void setLogDirectory(const std::wstring& path);
    void setLogRetentionDays(int days);
    void showSettings();
    SettingsState currentSettingsState() const;
    SettingsActions buildSettingsActions();
    void pickFont();
    void showFontColorDialog();
    void showAbout(bool downloadUpdate = false);
    void showUpdatePrompt(const std::wstring& latestVersion);
    void onTaskbarPlacementStatus(TaskbarPlacementStatus status);
    void handleTaskbarNoSpace();
    void showTaskbarSpacePrompt();
    void layoutUpdatePrompt();
    void paintUpdatePrompt(fluent::FluentDialogSurface::Painter& painter, float width,
                           float height);
    void drawUpdatePromptButton(fluent::FluentDialogSurface::Painter& painter,
                                const D2D1_RECT_F& rect, const wchar_t* text, bool accent, int id);
    int hitTestUpdatePrompt(float x, float y) const;
    void focusStepUpdatePrompt(int direction);
    void closeUpdatePrompt();
    void onUpdatePromptCommand(int command);
    bool launchUpdateInstaller(const std::wstring& path);
    void onDialogClosed(DialogKind kind);
    void setAutoCheckOnStartup(bool enabled);
    void pickQqLocalLyricsPath();
    void applyQqLocalLyricsPath(const std::wstring& path);
    void applyTickTickEnabled(bool enabled);
    void disconnectTickTick();
    void editTickTickApiToken(bool enableAfterSave = false);
    void applyFontAppearance();
    void applyFontColors();
    COLORREF effectivePlayedColor() const;
    COLORREF effectiveFloatingCardBackgroundColor() const;
    void applyFloatingCardBackgroundColor(COLORREF color);
    const LyricAppearance& currentLyricAppearance() const;
    void tryExtractAlbumColor();
    void loadSettings();
    void saveSettings();
    void showSettingsError(const wchar_t* operation, const std::wstring& message);
    static LRESULT CALLBACK trayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK updatePromptWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};
} // namespace lyrics_app
