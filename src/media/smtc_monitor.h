#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class PlaybackStatus { Stopped, Playing, Paused, Other };

enum class SmtcPlayerType { Unknown, QQMusic, NetEase };

// SMTC 统一快照：锚点进度 + 本机时间，渲染侧据此插值
struct SmtcSnapshot {
    SmtcPlayerType player = SmtcPlayerType::Unknown;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring neteaseSongId;          // 网易云增强会话通过 Genres 传递的歌曲 ID
    std::wstring sourceAppUserModelId;   // 会话来源标识
    bool enhancedSmtc = false;           // 是否为带歌曲 ID/进度的增强会话
    int64_t durationMs = 0;
    int64_t positionMs = 0;   // 锚点进度（snapshot() 返回时已按播放状态插值）
    int64_t anchorUtcMs = 0;  // 锚点对应的本机系统时间
    PlaybackStatus status = PlaybackStatus::Stopped;
    bool sessionAlive = false; // 当前是否存在可消费的 QQ/网易云会话
    std::shared_ptr<const std::vector<uint8_t>> thumbnail; // 专辑封面图片字节（共享避免快照拷贝）
    bool canPrev = false;
    bool canPlayPause = false;
    bool canNext = false;
};

// 监听 Windows 系统媒体会话：保留 QQMusic.exe，并识别 Genres 中带 NCM-{ID} 的网易云增强会话。
class SmtcMonitor {
public:
    using ChangeCallback = std::function<void()>; // 任意事件后触发（WinRT 线程池线程）

    SmtcMonitor();
    ~SmtcMonitor();

    SmtcMonitor(const SmtcMonitor&) = delete;
    SmtcMonitor& operator=(const SmtcMonitor&) = delete;

    void start(ChangeCallback onChange);
    SmtcSnapshot snapshot() const; // 当前快照（Playing 时进度已插值）

    // 播放控制（无会话或调用失败时静默忽略）
    void playPause();
    void skipNext();
    void skipPrevious();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
