#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// 设置窗口的初始状态快照（打开时由 App 填充）
struct SettingsState {
    bool songInfoVisible = true;
    bool albumCoverVisible = true;
    bool platformIconVisible = false;
    bool coverEffectVinyl = false;
    bool spectrumOn = false;
    int renderMode = 0; // 0 正常 1 低渲染 2 完全停止
    bool hoverControls = true;
    bool followAlbum = false;
    bool doubleLineLyrics = false;
    int lyricAlignment = 0; // 0 左对齐 1 居中 2 右对齐
    bool secondaryEnabled = true;
    bool preferRomanization = false;
    int secondaryAvailability = 0; // 0 可用 1 检查中 2 当前歌曲无翻译/罗马音
    bool qqLocalLyricsEnabled = false;
    bool qqLocalLyricsPersistOrder = false;
    std::wstring qqLocalLyricsPath;
    std::wstring fontDesc;         // 字体页提示文字（如 "Microsoft YaHei UI, 16px, 常规"）
};

// 设置变更回调：用户操作即时生效（与右键菜单语义一致）
struct SettingsActions {
    std::function<void(bool)> onSongInfoVisible;
    std::function<void(bool)> onAlbumCoverVisible;
    std::function<void(bool)> onPlatformIconVisible;
    std::function<void(bool)> onCoverEffectVinyl;
    std::function<void(bool)> onSpectrum;
    std::function<void(int)> onRenderMode;
    std::function<void(bool)> onHoverControls;
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
