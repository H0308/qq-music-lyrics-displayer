#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <vector>

struct PlacementProbeResult {
    RECT rcTm{};
    std::vector<RECT> buttons;
    std::vector<RECT> kugouWindows;
};

// 探测线程仅持有独立状态；窗口销毁只发停止信号，不等待跨进程 UIA 返回。
class PlacementProbe {
public:
    PlacementProbe() = default;
    PlacementProbe(const PlacementProbe&) = delete;
    PlacementProbe& operator=(const PlacementProbe&) = delete;
    ~PlacementProbe() { stop(); }
    void setTaskbar(HWND window) { state_->taskbar = window; }
    void setFast(bool value) { state_->fast = value; }
    void clearResult() { delete state_->out.exchange(nullptr); }
    std::unique_ptr<PlacementProbeResult> take() {
        return std::unique_ptr<PlacementProbeResult>(state_->out.exchange(nullptr));
    }
    void start();
    void stop();

private:
    struct ProbeState {
        std::atomic<bool> stop{false};
        std::atomic<bool> fast{false};
        std::atomic<PlacementProbeResult*> out{nullptr};
        std::atomic<HWND> taskbar{nullptr};

        ~ProbeState() { delete out.exchange(nullptr); }
    };
    std::shared_ptr<ProbeState> state_ = std::make_shared<ProbeState>();
    bool running_ = false;
    static void run(const std::shared_ptr<ProbeState>& state);
};
