#pragma once

#include "lyric_window.h"

#include <array>
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
    void setEnabled(bool enabled);
    void setBackgroundMode(MediaPopupBackground mode);
    void setFollowAlbumBackground(bool on);
    void setAutoTextContrast(bool on);
    void refreshTheme();
    // animateSongTransition 只由完整展示帧在确认切歌时传入；媒体字段异步补齐不触发转场。
    void setMedia(const OverlayMediaInfo& info, bool available,
                  bool animateSongTransition = false);
    void setProgress(int64_t positionMs);
    void setSpectrumBands(const std::array<float, kPresentationSpectrumBands>& bands);
    void setSpectrumDemandCallback(std::function<void(bool)> cb);
    bool needsAnimation() const;
    void advanceAnimation(ULONGLONG nowMs);
    void setAnchor(HWND anchor);

    void onAnchorEnter();
    void onAnchorLeave();
    void hideImmediate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
