#pragma once

#include <array>
#include <atomic>
#include <thread>

// QQ 音乐音频频谱：WASAPI 进程级回环捕获（仅 QQMusic.exe 进程树）+ FFT 分频段。
// start() 后捕获线程自动寻找 QQ 音乐进程；QQ 未运行 / 暂停 / 退出时频段自动回落为零。
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
    // 通知捕获线程：QQ 音乐的 SMTC 会话已关闭或重新建立。
    void requestReconnect();

    std::array<float, kBandCount> bands() const;

private:
    void run(); // 捕获线程主循环：找进程 -> 捕获 -> 出错/退出后重试

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> reconnectRequested_{false};
    std::array<std::atomic<float>, kBandCount> bands_{};
};
