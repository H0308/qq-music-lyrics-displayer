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
    std::wstring sourceAppUserModelId; // SMTC 当前来源应用标识，用于平台图标
    std::shared_ptr<const std::vector<uint8_t>> thumbnail; // 专辑封面图片字节
    bool canPrev = false;
    bool canPlayPause = false;
    bool canNext = false;
    bool playing = false;
};

// 窗口宿主抽象：TaskbarHost（任务栏内嵌歌词）实现此接口
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

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setLyrics(const std::vector<LyricLine>& lines) = 0;
    virtual void setCurrentLine(int index) = 0;
    virtual void setPosition(int64_t positionMs) = 0; // 每帧下发播放进度，驱动逐字高亮
    virtual void setStatusText(const std::wstring& text) = 0;
    // 翻译与罗马音互斥；两者均 false 时只显示原文。
    virtual void setSecondaryLyricMode(bool translation, bool romanization) = 0;
};
