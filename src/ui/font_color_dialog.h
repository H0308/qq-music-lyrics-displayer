#pragma once

#include "ui/font_style.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// Win11 风格“字体颜色与效果”对话框：
// 未播放/已播放字体颜色、未播放不透明度（滑块）、光晕/描边（开关 + 颜色）设置，
// 点击色块弹出取色器；底部示例歌词实时预览效果；
// 确定后才通过回调一次性应用，取消/关闭不影响实际歌词。
class FontColorDialog {
public:
    // 打开时的外观快照。glowOn/outlineOn/unplayedAlphaPct 可在对话框内修改；
    // 字体 只用于预览（仍在“字体…”对话框中调整）
    struct State {
        COLORREF played = 0;
        COLORREF unplayed = 0;
        int unplayedAlphaPct = 100;
        COLORREF glowColor = 0;
        COLORREF outlineColor = 0;
        bool glowOn = false;
        bool outlineOn = false;
        std::wstring fontFamily;   // 预览用字体族，空则用 UI 默认字体
        LyricFontStyle fontStyle = LyricFontStyle::Normal; // 预览用字体样式
        float lyricFontSize = 14.0f; // 预览用歌词字号（与实际任务栏渲染字号一致）
    };

    // 确定时回传的四个颜色、两个效果开关和未播放不透明度
    struct Result {
        COLORREF played;
        COLORREF unplayed;
        int unplayedAlphaPct;
        COLORREF glowColor;
        COLORREF outlineColor;
        bool glowOn;
        bool outlineOn;
    };
    using ApplyCallback = std::function<void(const Result&)>;

    FontColorDialog();
    ~FontColorDialog();

    FontColorDialog(const FontColorDialog&) = delete;
    FontColorDialog& operator=(const FontColorDialog&) = delete;

    bool create(HINSTANCE inst, HWND parent, const State& initial);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;
    void refreshTheme();

    // 主消息循环转发：自身或内嵌取色器消费了该消息时返回 true
    bool isDialogMessage(MSG* msg);

    void setApplyCallback(ApplyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
