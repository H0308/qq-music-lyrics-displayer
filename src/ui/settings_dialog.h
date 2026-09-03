#pragma once

#include "idle/idle_types.h"
#include "ui/fluent_theme.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// 设置窗口的初始状态快照（打开时由 App 填充）
struct SettingsState {
    bool idleEntryEnabled = true;
    bool idleQuoteEnabled = true;
    int idleQuoteSource = 0; // 0 一言 1 今日诗词
    int idleQuoteRefreshInterval = 0; // 0 每天 1 每 12 小时 2 每小时
    int idleQuoteAlignment = 0; // 0 左对齐 1 居中 2 右对齐
    int idleQuoteBackground = 0; // 0 无 1 落叶 2 闪烁星星 3 二进制 4 流光粒子
    int idleQuoteBackgroundScope = 1; // 0 都不启用 1 仅每日一言启用 2 仅歌词启用 3 都启用
    bool idleAppNamesVisible = true;
    std::vector<IdleAppInfo> idleApps;
    bool tickTickApiTokenConfigured = false;
    bool tickTickConnected = false;
    bool tickTickConnecting = false;
    bool tickTickSyncing = false;
    std::wstring tickTickStatus;
    bool songInfoVisible = true;
    bool albumCoverVisible = true;
    bool platformIconVisible = false;
    bool coverEffectVinyl = false;
    bool spectrumOn = false;
    int spectrumStyle = 0; // 0 默认 1 柱状图 2 梦幻波浪
    bool spectrumBackground = false; // 将梦幻波浪作为歌曲信息和歌词背景
    int spectrumOpacity = 40; // 背景波浪不透明度（0~100）
    bool progressBackground = false; // 播放进度背景（与背景波浪互斥）
    int progressBackgroundOpacity = 25; // 播放进度背景不透明度（0~100）
    int taskbarBackground = 0; // 任务栏歌词背景：0 无 1 封面模糊 2 纯色（画在最底层，可与其他背景叠加）
    int coverBackgroundOpacity = 60; // 封面模糊背景不透明度（0~100）
    int renderMode = 0; // 0 正常 1 低渲染 2 完全停止 3 极简
    bool hoverControls = true;
    int hoverControlStyle = 0; // 0 内嵌控件 1 媒体卡片
    int floatingCardTrigger = 0; // 0 悬浮展开 1 点击展开
    int floatingCardBackground = 0; // 0 纯色 1 磨砂玻璃
    COLORREF floatingCardBackgroundColor = RGB(255, 255, 255);
    bool floatingCardFollowAlbum = false; // 播放时磨砂背景颜色跟随当前专辑
    bool floatingCardAutoTextContrast = true; // 磨砂卡片自动适配文字颜色
    bool songToastEnabled = false;    // 切歌时在屏幕中下方弹出歌曲信息
    int songToastDurationSec = 4;     // 切歌弹窗停留秒数（1~10）
    bool songToastSkipFullscreen = true; // 前台有全屏应用时不弹出
    int songToastPosition = 1;      // 0 中上 1 中下
    fluent::ThemeMode taskbarThemeMode = fluent::ThemeMode::FollowSystem;
    fluent::ThemeMode windowThemeMode = fluent::ThemeMode::FollowApp;
    bool verticalTaskbar = false; // 侧边任务栏：禁用不适合窄栏的设置项
    bool followAlbum = false;
    bool doubleLineLyrics = false;
    int lyricAlignment = 0; // 0 左对齐 1 居中 2 右对齐
    bool secondaryEnabled = true;
    bool preferRomanization = false;
    int secondaryAvailability = 0; // 0 可用 1 检查中 2 当前歌曲无翻译/罗马音
    bool qqLocalLyricsEnabled = false;
    bool qqLocalLyricsPersistOrder = false;
    std::wstring qqLocalLyricsPath;
    std::wstring fontDesc;         // 字体设置提示文字（如 "Microsoft YaHei UI, 16px, 常规"）
};

// 设置变更回调：用户操作即时生效（与右键菜单语义一致）
struct SettingsActions {
    std::function<void(bool)> onIdleEntryEnabled;
    std::function<void(bool)> onIdleQuoteEnabled;
    std::function<void(int)> onIdleQuoteSource;
    std::function<void(int)> onIdleQuoteRefreshInterval;
    std::function<void(int)> onIdleQuoteAlignment;
    std::function<void(int)> onIdleQuoteBackground;
    std::function<void(int)> onIdleQuoteBackgroundScope;
    std::function<void()> onEditIdleWelcome;
    std::function<void()> onEditTickTickApiToken;
    std::function<void()> onTickTickConnect;
    std::function<void()> onTickTickRefresh;
    std::function<void()> onTickTickDisconnect;
    std::function<void()> onAddIdleApp;
    std::function<void(int)> onEditIdleApp;
    std::function<void(bool)> onIdleAppNamesVisible;
    std::function<void(int)> onRemoveIdleApp;
    std::function<void(int, int)> onReorderIdleApps;
    std::function<void(bool)> onSongInfoVisible;
    std::function<void(bool)> onAlbumCoverVisible;
    std::function<void(bool)> onPlatformIconVisible;
    std::function<void(bool)> onCoverEffectVinyl;
    std::function<void(bool)> onSpectrum;
    std::function<void(int)> onSpectrumStyle;
    std::function<void(bool)> onSpectrumBackground;
    std::function<void(int)> onSpectrumOpacity;
    std::function<void(bool)> onProgressBackground;
    std::function<void(int)> onProgressBackgroundOpacity;
    std::function<void(int)> onTaskbarBackground;
    std::function<void(int)> onCoverBackgroundOpacity;
    std::function<void(int)> onRenderMode;
    std::function<void(bool)> onHoverControls;
    std::function<void(int)> onHoverControlStyle;
    std::function<void(int)> onFloatingCardTrigger;
    std::function<void(int)> onFloatingCardBackground;
    std::function<void(COLORREF)> onFloatingCardBackgroundColor;
    std::function<void(bool)> onFloatingCardFollowAlbum;
    std::function<void(bool)> onFloatingCardAutoTextContrast;
    std::function<void(bool)> onSongToastEnabled;
    std::function<void(int)> onSongToastDuration;
    std::function<void(bool)> onSongToastSkipFullscreen;
    std::function<void(int)> onSongToastPosition;
    std::function<void(fluent::ThemeMode)> onTaskbarTheme;
    std::function<void(fluent::ThemeMode)> onWindowTheme;
    std::function<void()> onPickFont;
    std::function<void()> onFontColorEffect;
    std::function<void(bool)> onFollowAlbum;
    std::function<void(bool)> onDoubleLineLyrics;
    std::function<void(int)> onLyricAlignment;
    std::function<void(bool)> onSecondaryEnabled;
    std::function<void(bool)> onPreferRomanization; // true = 罗马音
    std::function<void(bool)> onQqLocalLyricsEnabled;
    std::function<void(bool)> onQqLocalLyricsPersistOrder;
    std::function<void()> onPickQqLocalLyricsPath;
};

// 设置窗口：左侧大类目录 + 右侧该类别的设置项（Windows 设置式布局）。
class SettingsDialog {
public:
    SettingsDialog();
    ~SettingsDialog();

    SettingsDialog(const SettingsDialog&) = delete;
    SettingsDialog& operator=(const SettingsDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, const SettingsState& state,
                SettingsActions actions);
    // 打开前同步最新状态快照；窗口关闭后销毁，下次打开重新创建
    void updateState(const SettingsState& state);
    void updateFontDescription(const std::wstring& description);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
