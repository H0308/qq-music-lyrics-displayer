#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// 播放器音频频谱：WASAPI 进程级回环捕获（目标进程树）+ FFT 分频段。
// start() 后捕获线程自动寻找目标播放器进程；播放器未运行 / 暂停 / 退出时频段自动回落为零。
// 频段电平为 0~1，任意线程可随时读取（逐元素原子量）。
class AudioSpectrum {
public:
    static constexpr int kBandCount = 6;

    AudioSpectrum() = default;
    ~AudioSpectrum();

    AudioSpectrum(const AudioSpectrum&) = delete;
    AudioSpectrum& operator=(const AudioSpectrum&) = delete;

    void start();
    void stop();
    // 设置当前 SMTC 播放源对应的可执行文件名，例如 QQMusic.exe 或 cloudmusic.exe。
    // 目标变化时会要求捕获线程重建进程级回环会话。
    void setTargetProcessName(const std::wstring& processName);

    // 通知捕获线程：当前播放器的 SMTC 会话已关闭或重新建立。
    void requestReconnect();

    std::array<float, kBandCount> bands() const;

private:
    void run(); // 捕获线程主循环：找进程 -> 捕获 -> 出错/退出后重试

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> reconnectRequested_{false};
    std::array<std::atomic<float>, kBandCount> bands_{};
    mutable std::mutex targetMutex_;
    std::wstring targetProcessName_ = L"QQMusic.exe";
};
