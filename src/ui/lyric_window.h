#pragma once

#include "lyric/lyric_provider.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <vector>

#include <windows.h>

// 底部控制条操作
enum class MediaControl { Prev = 0, PlayPause = 1, Next = 2 };

// 底部控制条显示的媒体信息
struct OverlayMediaInfo {
    std::wstring title;
    std::wstring artist;
    std::shared_ptr<const std::vector<uint8_t>> thumbnail; // 专辑封面图片字节
    bool canPrev = false;
    bool canPlayPause = false;
    bool canNext = false;
    bool playing = false;
};

// 窗口宿主抽象：第一阶段 OverlayHost（置顶悬浮窗），后续 TaskbarHost 复用同一渲染逻辑
class ILyricHost {
public:
    virtual ~ILyricHost() = default;
    virtual bool create(HINSTANCE inst) = 0;
    virtual HWND hwnd() const = 0;

    virtual void setTickCallback(std::function<void()> cb) = 0;
    virtual void setMediaInfo(const OverlayMediaInfo& info) = 0;
    virtual void setControlCallback(std::function<void(MediaControl)> cb) = 0;

    virtual const std::vector<LyricLine>& lyrics() const = 0;

    virtual bool isTaskbar() const = 0;
    virtual int currentLine() const = 0;
    virtual const std::wstring& statusText() const = 0;

    virtual void changeFont(float delta) = 0;
    virtual void setFont(const std::wstring& family, float size) = 0;
    // 歌词颜色：已播放色 + 逐字未播放色（含透明度，单位 %）。LRC 单行歌词只用已播放色
    virtual void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) = 0;
    virtual void setClickThrough(bool on) = 0;
    virtual bool clickThrough() const = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setLyrics(const std::vector<LyricLine>& lines) = 0;
    virtual void setCurrentLine(int index) = 0;
    virtual void setPosition(int64_t positionMs) = 0; // 每帧下发播放进度，驱动逐字高亮
    virtual void setStatusText(const std::wstring& text) = 0;
};

class OverlayHost : public ILyricHost {
public:
    OverlayHost();
    ~OverlayHost() override;

    bool create(HINSTANCE inst) override;
    HWND hwnd() const override;

    // 每帧（约 30fps）回调：驱动进度推算与当前行更新
    void setTickCallback(std::function<void()> cb) override;

    void setMediaInfo(const OverlayMediaInfo& info) override;      // 底部控制条内容
    void setControlCallback(std::function<void(MediaControl)> cb) override; // 控制条按钮点击

    const std::vector<LyricLine>& lyrics() const override; // UI 线程内读取

    bool isTaskbar() const override;
    int currentLine() const override;
    const std::wstring& statusText() const override;

    void changeFont(float delta) override;
    void setFont(const std::wstring& family, float size) override;
    void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) override;
    void setClickThrough(bool on) override;
    bool clickThrough() const override;

    void show() override;
    void hide() override;
    void setLyrics(const std::vector<LyricLine>& lines) override;
    void setCurrentLine(int index) override;
    void setPosition(int64_t positionMs) override;
    void setStatusText(const std::wstring& text) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
