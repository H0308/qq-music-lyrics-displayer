#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace system_monitor {

struct ResourceSnapshot {
    bool cpuAvailable = false;
    bool memoryAvailable = false;
    bool networkAvailable = false;
    bool gpuAvailable = false;
    bool diskAvailable = false;
    bool batteryAvailable = false;
    bool cpuFrequencyAvailable = false;
    double cpuPercent = 0.0;
    double gpuPercent = 0.0;
    unsigned memoryPercent = 0;
    uint64_t downloadBytesPerSecond = 0;
    uint64_t uploadBytesPerSecond = 0;
    uint64_t diskReadBytesPerSecond = 0;
    uint64_t diskWriteBytesPerSecond = 0;
    unsigned batteryPercent = 0;
    bool batteryCharging = false;
    double cpuFrequencyGHz = 0.0;
    uint64_t revision = 0;
};

// 系统资源采样器。所有系统查询都在独立线程执行，UI 线程只读取最近一次快照。
class ResourceMonitor {
public:
    ResourceMonitor();
    ~ResourceMonitor();

    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    void start();
    void stop();
    ResourceSnapshot snapshot() const;

private:
    struct PerformanceCounters;

    void run();
    ResourceSnapshot sample();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool running_ = false;
    bool stopRequested_ = false;
    ResourceSnapshot snapshot_;

    bool haveCpuBaseline_ = false;
    uint64_t previousCpuIdle_ = 0;
    uint64_t previousCpuKernel_ = 0;
    uint64_t previousCpuUser_ = 0;

    bool haveNetworkBaseline_ = false;
    uint64_t previousNetworkIn_ = 0;
    uint64_t previousNetworkOut_ = 0;
    uint64_t previousNetworkTickMs_ = 0;

    std::unique_ptr<PerformanceCounters> performanceCounters_;
};

} // namespace system_monitor
