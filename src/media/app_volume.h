#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// 单个应用的独立音量控制（对应音量合成器中该应用的一格，不影响系统主音量）。
// 通过 WASAPI 音频会话枚举匹配目标进程，读写其 ISimpleAudioVolume。
// 所有方法必须在同一线程（UI 线程）调用；ChangedCallback 在 COM 工作线程触发，
// 调用方需自行转发到 UI 线程。
class AppVolumeController {
public:
    using ChangedCallback = std::function<void()>; // 音量/静音/会话可用性可能已变化

    AppVolumeController();
    ~AppVolumeController();

    AppVolumeController(const AppVolumeController&) = delete;
    AppVolumeController& operator=(const AppVolumeController&) = delete;

    // 目标进程可执行文件名列表（如 QQMusic.exe；网易云为 cloudmusic.exe + 桥接插件
    // NeteaseBridge.exe，两者在音量合成器中各占一格）。空列表表示无目标。
    void setTargetProcessNames(std::vector<std::wstring> processNames);
    void setChangedCallback(ChangedCallback cb);

    // 无匹配会话时返回 false；percent 为 0-100。
    bool query(int& percent, bool& muted);
    // 设置音量（0-100）并解除静音；无匹配会话返回 false。
    bool setVolumePercent(int percent);
    bool setMuted(bool muted);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
