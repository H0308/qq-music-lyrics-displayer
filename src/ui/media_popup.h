#pragma once

#include "lyric_window.h"

#include <functional>
#include <memory>

#include <windows.h>

// 任务栏歌词悬浮时展开的媒体卡片。它是独立的非激活窗口，
// 只消费宿主已经整理好的媒体快照，不直接访问播放器或歌词提供器。
class MediaPopup {
public:
    MediaPopup();
    ~MediaPopup();

    MediaPopup(const MediaPopup&) = delete;
    MediaPopup& operator=(const MediaPopup&) = delete;

    bool create(HINSTANCE inst, HWND anchor);
    void destroy();

    void setControlCallback(std::function<void(MediaControl)> cb);
    void setSourceOpenCallback(std::function<void(const std::wstring&)> cb);
    void setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb);
    void setIdleTaskOpenCallback(std::function<void(const IdleTaskInfo&)> cb);
    // 面板完成一次隐藏到可见的打开时触发；悬浮和点击打开共用此回调。
    void setPanelOpenedCallback(std::function<void()> cb);
    // 应用音量：图标+数值常驻显示；点击图标展开/收起卡内滑块，拖动经回调上报
    void setAppVolume(const AppVolumeState& state);
    void setAppVolumeCallback(std::function<void(int percent)> cb);
    void setEnabled(bool enabled);
    // 触发方式：true 悬浮停留片刻后展开（默认），false 由点击展开（onAnchorClick）
    void setTriggerOnHover(bool on);
    // 媒体卡片与无播放组合面板共用的背景模式和磨砂颜色。
    void setBackgroundMode(MediaPopupBackground mode);
    void setBackgroundColor(COLORREF color, bool customized);
    // 媒体卡片与无播放组合面板共用的专辑色和文字对比度设置。
    void setFollowAlbumBackground(bool on);
    void setAutoTextContrast(bool on);
    void refreshTheme();
    // animateSongTransition 只由完整展示帧在确认切歌时传入；媒体字段异步补齐不触发转场。
    void setMedia(const OverlayMediaInfo& info, bool available,
                  bool animateSongTransition = false);
    void setIdleContent(const IdlePresentation& content, bool available);
    // 将同一完整展示帧中的多个弹窗状态更新合并为一次绘制/提交。
    void beginPresentationUpdate();
    void endPresentationUpdate();
    // 展示类别与具体内容分开同步：scene 不为 Idle 时表示播放媒体；
    // 悬浮控件样式下，播放中的弹窗允许临时切到“每日一言 + 快捷启动与今日任务”
    // 组合面板，关闭后回到媒体卡片。
    // 内嵌控件展开时使用与无播放状态相同的组合页面。
    void setPresentationMode(DisplayScene scene, bool available, bool inlineControls);
    void setProgress(int64_t positionMs);
    void setAnchor(HWND anchor);

    void onAnchorEnter();
    void onAnchorLeave();
    // 点击锚点（任务栏歌词区域任意位置）：立即展开，不走悬浮延迟
    void onAnchorClick();
    void hideImmediate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
