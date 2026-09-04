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

// 媒体卡片展开方式：悬浮停留片刻后展开，或点击歌词区域任意位置立即展开
enum class MediaPopupTrigger {
    Hover,
    Click,
};

enum class SpectrumStyle {
    Default,
    Bars,
    DreamyWave,
};

// 任务栏歌词背景：无 / 封面模糊 / 纯色（跟随任务栏深浅色）；
// 画在最底层，可与播放进度背景、频谱背景波浪叠加
enum class TaskbarBackground {
    None,
    CoverBlur,
    Solid,
};

// 任务栏内容动态背景；绘制在歌词等内容区域下方，不侵入独立频谱容器或悬浮卡片。
enum class IdleQuoteBackground {
    None,
    FallingLeaves,
    TwinklingStars,
    BinaryRain,
    FloatingParticles,
};

// 任务栏内容动态背景的作用范围：不启用 / 仅待机内容（每日一言或默认欢迎语）/ 仅歌词 / 两者。
enum class IdleQuoteBackgroundScope {
    None,
    DailyQuote,
    Lyrics,
    All,
};

// 任务栏渲染模式：极简模式只关闭附加视觉与弹窗，不改变歌词刷新策略。
// 数值保持与 settings.json 中已有的 0/1/2 语义一致，Minimal 追加为 3。
enum class RenderMode {
    Normal = 0,
    Low = 1,
    Stopped = 2,
    Minimal = 3,
};

// 任务栏内嵌歌词宿主：窗口作为 Shell_TrayWnd 的子窗口，锚定在通知区左侧。
// 横向任务栏显示圆角封面、歌名/歌手和当前行歌词（超长自动滚动），鼠标悬浮
// 时歌词区可切换为播放控制；侧边任务栏改用窄栏封面、逐字竖排歌词和纵向控件。
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
    void setAppVolume(const AppVolumeState& state) override;
    void setAppVolumeCallback(std::function<void(int percent)> cb) override;
    void setSourceOpenCallback(std::function<void(const std::wstring&)> cb);
    void setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb);
    void setIdleTaskOpenCallback(std::function<void(const IdleTaskInfo&)> cb);
    void setMediaPopupOpenedCallback(std::function<void()> cb);

    const std::vector<LyricLine>& lyrics() const override;

    bool isTaskbar() const override;
    bool isVerticalTaskbar() const;
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
    void setIdleQuoteAlignment(LyricAlignment alignment);
    void setIdleQuoteBackground(IdleQuoteBackground background);
    void setIdleQuoteBackgroundScope(IdleQuoteBackgroundScope scope);
    // 是否在鼠标悬浮时用播放控件替换右侧歌词，默认开启。
    void setControlsOnHover(bool on);
    // 悬浮播放控件样式：保留当前歌词区内嵌控件，或展开独立媒体卡片。
    void setHoverControlStyle(HoverControlStyle style);
    // 媒体卡片、每日一言和快捷启动卡片共用的展开方式与外观设置。
    void setFloatingCardTrigger(MediaPopupTrigger trigger);
    void setFloatingCardBackground(MediaPopupBackground mode);
    void setFloatingCardBackgroundColor(COLORREF color, bool customized);
    void setFloatingCardFollowAlbum(bool on);
    void setFloatingCardAutoTextContrast(bool on);
    // 主题模式或 Windows 外观发生变化时重建任务栏与媒体卡片资源。
    void refreshTheme();
    void setSongInfoVisible(bool on);
    void setAlbumCoverVisible(bool on);
    void setPlatformIconVisible(bool on);
    void setAlbumCoverEffect(AlbumCoverEffect effect);

    // 渲染模式：0 正常（播放时跟随屏幕刷新率）；1 低渲染（固定 ~30fps，降低 GPU/CPU
    // 占用）；2 完全停止（隐藏窗口、停帧定时器并释放 GPU 设备，仅内存中保留数据状态）；
    // 3 极简（保留正常歌词刷新策略，只关闭附加视觉与弹窗）。
    void setRenderMode(int mode);

    // 音频频谱（任务栏独有）：on 控制显隐，bands 为兼容入口；正式播放链路使用 SpectrumPatch。
    // 频段值仅在 UI 线程读写（onFrame 经宿主定时器回调），无需加锁
    static constexpr int kSpectrumBands = kPresentationSpectrumBands;
    void setSpectrumStyle(SpectrumStyle style);
    void setSpectrumBackground(bool on);
    void setSpectrumOpacity(int percent);
    void setSpectrumVisible(bool on);
    void setSpectrumBands(const std::array<float, kSpectrumBands>& bands);

    // 播放进度背景：从窗口左缘到歌词右缘按播放进度填充专辑主题色；
    // 与背景波浪互斥（背景波浪生效时不绘制）
    void setProgressBackground(bool on);
    void setProgressBackgroundOpacity(int percent);

    // 任务栏歌词背景：封面模糊（不透明度可调）或跟随深浅色的纯色，画在最底层
    void setBackground(TaskbarBackground mode);
    void setCoverBackgroundOpacity(int percent);

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
