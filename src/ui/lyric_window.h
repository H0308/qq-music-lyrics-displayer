#pragma once

#include "lyric/lyric_provider.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

// 窗口宿主抽象：第一阶段 OverlayHost（置顶悬浮窗），后续 TaskbarHost 复用同一渲染逻辑
class ILyricHost {
public:
    virtual ~ILyricHost() = default;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setLyrics(const std::vector<LyricLine>& lines) = 0;
    virtual void setCurrentLine(int index) = 0;
    virtual void setStatusText(const std::wstring& text) = 0;
};

class OverlayHost : public ILyricHost {
public:
    OverlayHost();
    ~OverlayHost() override;

    bool create(HINSTANCE inst);
    HWND hwnd() const;

    // 每帧（约 30fps）回调：驱动进度推算与当前行更新
    void setTickCallback(std::function<void()> cb);

    const std::vector<LyricLine>& lyrics() const; // UI 线程内读取

    void show() override;
    void hide() override;
    void setLyrics(const std::vector<LyricLine>& lines) override;
    void setCurrentLine(int index) override;
    void setStatusText(const std::wstring& text) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
