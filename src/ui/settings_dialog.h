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
    std::vector<IdleAppInfo> idleApps;
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
    int mediaPopupTrigger = 0; // 0 悬浮展开 1 点击展开
    int mediaPopupBackground = 0; // 音乐控件卡片：0 纯色 1 磨砂玻璃
    int idleCardBackground = 0; // 0 纯色 1 磨砂玻璃
    COLORREF idleCardBackgroundColor = RGB(255, 255, 255);
    bool mediaPopupFollowAlbum = false; // 磨砂背景跟随专辑
    bool mediaPopupAutoTextContrast = false; // 磨砂背景自动适配文字颜色
    bool songToastEnabled = false;    // 切歌时在屏幕中下方弹出歌曲信息
    int songToastDurationSec = 4;     // 切歌弹窗停留秒数（1~10）
    bool songToastSkipFullscreen = true; // 前台有全屏应用时不弹出
    int songToastPosition = 1;      // 0 中上 1 中下
    fluent::ThemeMode taskbarThemeMode = fluent::ThemeMode::FollowSystem;
    fluent::ThemeMode windowThemeMode = fluent::ThemeMode::FollowApp;
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
    std::function<void()> onAddIdleApp;
    std::function<void(int)> onRemoveIdleApp;
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
    std::function<void(int)> onMediaPopupTrigger;
    std::function<void(int)> onMediaPopupBackground;
    std::function<void(int)> onIdleCardBackground;
    std::function<void(COLORREF)> onIdleCardBackgroundColor;
    std::function<void(bool)> onMediaPopupFollowAlbum;
    std::function<void(bool)> onMediaPopupAutoTextContrast;
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
