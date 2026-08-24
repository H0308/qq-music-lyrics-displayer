#pragma once

#include "ui/font_style.h"
#include "lyric/lyric_provider.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

// 底部控制条操作
enum class MediaControl { Prev = 0, PlayPause = 1, Next = 2 };

// 悬浮媒体卡片的背景材质
enum class MediaPopupBackground {
    Solid,
    Frosted,
};

// 底部控制条显示的媒体信息
struct OverlayMediaInfo {
    std::wstring title;
    std::wstring artist;
    std::wstring sourceAppUserModelId; // SMTC 当前来源应用标识，用于平台图标和打开入口
    std::shared_ptr<const std::vector<uint8_t>> thumbnail; // 专辑封面图片字节
    int64_t durationMs = 0;             // 当前歌曲总时长；未知时为 0
    bool canPrev = false;
    bool canPlayPause = false;
    bool canNext = false;
    bool playing = false;
};

// 任务栏正式展示的语义场景。频谱柱值不放进完整帧，仍通过任务栏的高频接口更新。
enum class DisplayScene {
    NoPlayback,
    Searching,
    Lyrics,
    Message,
    Spectrum,
};

// 频谱属于高频展示数据，不进入完整展示帧；TaskbarHost 与 AudioSpectrum 共用 12 段。
constexpr int kPresentationSpectrumBands = 12;

// 高频播放补丁只携带逐字高亮和行过渡所需的字段。
// frameRevision 用于阻止旧完整帧之后产生的补丁回写当前宿主。
struct PlaybackPatch {
    uint64_t frameRevision = 0;
    uint64_t requestGeneration = 0;
    int64_t actualPositionMs = 0;
    int64_t lineSelectionPositionMs = 0;
    int currentLine = -1;
    bool playing = false;
};

// 高频频谱补丁不复制歌词、媒体信息或状态文字。
struct SpectrumPatch {
    uint64_t frameRevision = 0;
    uint64_t requestGeneration = 0;
    std::array<float, kPresentationSpectrumBands> bands{};
};

// 一次低频展示提交所需的完整状态。它只描述 UI 状态，不持有播放器、网络或 provider 对象。
struct PresentationFrame {
    uint64_t frameRevision = 0;
    uint64_t requestGeneration = 0;
    std::wstring trackKey;

    DisplayScene scene = DisplayScene::NoPlayback;
    OverlayMediaInfo media;
    std::vector<LyricLine> lyrics;
    std::wstring statusText;

    // 真实播放位置用于逐字高亮；行选择位置单独包含提前量。
    int64_t actualPositionMs = 0;
    int64_t lineSelectionPositionMs = 0;
    int currentLine = -1;

    bool visible = false;
    bool animateTransition = true;
};

// 窗口宿主抽象：TaskbarHost（任务栏内嵌歌词）实现此接口
class ILyricHost {
public:
    virtual ~ILyricHost() = default;
    virtual bool create(HINSTANCE inst) = 0;
    virtual HWND hwnd() const = 0;

    virtual void setTickCallback(std::function<void()> cb) = 0;
    virtual void applyPresentationFrame(const PresentationFrame& frame) = 0;
    virtual void applyPlaybackPatch(const PlaybackPatch& patch) = 0;
    virtual void applySpectrumPatch(const SpectrumPatch& patch) = 0;
    virtual void setMediaInfo(const OverlayMediaInfo& info) = 0;
    virtual void setControlCallback(std::function<void(MediaControl)> cb) = 0;

    virtual const std::vector<LyricLine>& lyrics() const = 0;

    virtual bool isTaskbar() const = 0;
    virtual int currentLine() const = 0;
    virtual const std::wstring& statusText() const = 0;

    virtual void changeFont(float delta) = 0;
    virtual void setFont(const std::wstring& family, float size, LyricFontStyle style) = 0;
    // 歌词颜色：已播放色 + 逐字未播放色（含不透明度，单位 %）。LRC 单行歌词只用已播放色
    virtual void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setLyrics(const std::vector<LyricLine>& lines) = 0;
    virtual void setCurrentLine(int index) = 0; // 兼容入口；正式播放链路使用 PlaybackPatch
    virtual void setPosition(int64_t positionMs) = 0; // 兼容入口；正式播放链路使用 PlaybackPatch
    virtual void setStatusText(const std::wstring& text) = 0;
    // 翻译与罗马音互斥；两者均 false 时只显示原文。
    virtual void setSecondaryLyricMode(bool translation, bool romanization) = 0;
};
