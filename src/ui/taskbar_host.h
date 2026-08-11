#pragma once

#include "lyric_window.h"

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
    void setMediaInfo(const OverlayMediaInfo& info) override;
    void setControlCallback(std::function<void(MediaControl)> cb) override;

    const std::vector<LyricLine>& lyrics() const override;

    bool isTaskbar() const override;
    int currentLine() const override;
    const std::wstring& statusText() const override;

    void changeFont(float delta) override;
    void setFont(const std::wstring& family, float size) override;
    void setClickThrough(bool on) override;
    bool clickThrough() const override;

    // 歌词外观：字体颜色 / 深色描边+光晕（任务栏独有，桌面歌词有自己的配色）
    void setFontColor(COLORREF rgb);
    void setFontGlow(bool on);

    void show() override;
    void hide() override;
    void setLyrics(const std::vector<LyricLine>& lines) override;
    void setCurrentLine(int index) override;
    void setStatusText(const std::wstring& text) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
