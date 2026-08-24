#pragma once

#include "lyric_window.h"

#include <array>

enum class AlbumCoverEffect {
    Default,
    Vinyl,
};

enum class LyricAlignment {
    Left,
    Center,
    Right,
};

enum class HoverControlStyle {
    Inline,
    Popup,
};

enum class SpectrumStyle {
    Default,
    Bars,
    DreamyWave,
    BackgroundWave,
};

// 任务栏内嵌歌词宿主：窗口作为 Shell_TrayWnd 的子窗口，锚定在通知区左侧。
// 外观参考 Windows 11 原生媒体控制卡片：左侧显示圆角封面+歌名+歌手，
// 右侧显示当前行歌词（超长自动滚动），鼠标悬浮时右侧叠加显示播放控制按钮。
class TaskbarHost : public ILyricHost {
public:
    TaskbarHost();
    ~TaskbarHost() override;

    bool create(HINSTANCE inst) override;
    HWND hwnd() const override;

    void setTickCallback(std::function<void()> cb) override;
    void applyPresentationFrame(const PresentationFrame& frame) override;
    void applyPlaybackPatch(const PlaybackPatch& patch) override;
    void applySpectrumPatch(const SpectrumPatch& patch) override;
    void setMediaInfo(const OverlayMediaInfo& info) override;
    void setControlCallback(std::function<void(MediaControl)> cb) override;
    void setSourceOpenCallback(std::function<void(const std::wstring&)> cb);

    const std::vector<LyricLine>& lyrics() const override;

    bool isTaskbar() const override;
    int currentLine() const override;
    const std::wstring& statusText() const override;

    void changeFont(float delta) override;
    void setFont(const std::wstring& family, float size, LyricFontStyle style) override;
    void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) override;

    // 歌词外观：光晕 / 描边（任务栏独有，桌面歌词有自己的配色），开关和颜色分别独立
    void setFontGlow(bool on);
    void setFontOutline(bool on);
    void setFontGlowColors(COLORREF glow, COLORREF outline);
    void setSecondaryLyricMode(bool translation, bool romanization) override;
    void setDoubleLineLyrics(bool on);
    void setLyricAlignment(LyricAlignment alignment);
    // 是否在鼠标悬浮时用播放控件替换右侧歌词，默认开启。
    void setControlsOnHover(bool on);
    // 悬浮播放控件样式：保留当前歌词区内嵌控件，或展开独立媒体卡片。
    void setHoverControlStyle(HoverControlStyle style);
    void setMediaPopupSpectrumDemandCallback(std::function<void(bool)> cb);
    // 媒体卡片背景：当前纯色或 Windows 系统磨砂材质。
    void setMediaPopupBackground(MediaPopupBackground mode);
    // 是否让磨砂背景跟随当前专辑主色和频谱变化。
    void setMediaPopupFollowAlbum(bool on);
    // 主题模式或 Windows 外观发生变化时重建任务栏与媒体卡片资源。
    void refreshTheme();
    void setSongInfoVisible(bool on);
    void setAlbumCoverVisible(bool on);
    void setPlatformIconVisible(bool on);
    void setAlbumCoverEffect(AlbumCoverEffect effect);

    // 渲染模式：0 正常（播放时跟随屏幕刷新率）；1 低渲染（固定 ~30fps，降低 GPU/CPU
    // 占用）；2 完全停止（隐藏窗口、停帧定时器并释放 GPU 设备，仅内存中保留数据状态，
    // 切回时按最近会话状态立即还原）
    void setRenderMode(int mode);

    // 音频频谱（任务栏独有）：on 控制显隐，bands 为兼容入口；正式播放链路使用 SpectrumPatch。
    // 频段值仅在 UI 线程读写（onFrame 经宿主定时器回调），无需加锁
    static constexpr int kSpectrumBands = kPresentationSpectrumBands;
    void setSpectrumStyle(SpectrumStyle style);
    void setSpectrumOpacity(int percent);
    void setSpectrumVisible(bool on);
    void setSpectrumBands(const std::array<float, kSpectrumBands>& bands);

    void show() override;
    void hide() override;

    // 任务栏上的锚定位置：0 = 最右空闲区（通知区域左侧，默认）；
    // 1 = 最左空闲区（居中任务栏时在开始按钮左侧，左对齐时在应用图标右侧）。
    // 两种模式都会自动避开小组件/搜索/任务视图/应用图标/TrafficMonitor：
    // 原位优先收缩，压到最小宽度仍放不下时换到容得下的最大空闲区
    void setPositionMode(int mode);
    // Explorer 重启后重建/重附着（托盘主窗口收到 TaskbarCreated 广播后调用）；
    // 广播只投递给顶层窗口，作为任务栏子窗口的歌词窗自己收不到
    void onTaskbarCreated();
    void setLyrics(const std::vector<LyricLine>& lines) override;
    void setCurrentLine(int index) override;
    void setPosition(int64_t positionMs) override;
    void setStatusText(const std::wstring& text) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
