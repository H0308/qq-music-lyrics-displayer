#include "resource_monitor.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace system_monitor {
namespace {

// 当前 Windows SDK 的 powrprof.h 不总是公开这个结构体，但
// CallNtPowerInformation(ProcessorInformation) 仍要求使用该固定布局。
struct ProcessorPowerInformation {
    ULONG number = 0;
    ULONG maxMhz = 0;
    ULONG currentMhz = 0;
    ULONG mhzLimit = 0;
    ULONG maxIdleState = 0;
    ULONG currentIdleState = 0;
};

uint64_t fileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

uint64_t monotonicMilliseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool readNetworkTotals(uint64_t& received, uint64_t& sent) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
        return false;

    received = 0;
    sent = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        // 首版统计所有已连接的数据网卡，排除回环与隧道，避免本机流量和 VPN
        // 封装流量被重复算入上下行。
        if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.Type == IF_TYPE_TUNNEL)
            continue;
        received += row.InOctets;
        sent += row.OutOctets;
    }
    FreeMibTable(table);
    return true;
}

} // namespace

struct ResourceMonitor::PerformanceCounters {
    HQUERY query = nullptr;
    HCOUNTER gpuUsage = nullptr;
    HCOUNTER diskRead = nullptr;
    HCOUNTER diskWrite = nullptr;
    bool primed = false;

    ~PerformanceCounters() {
        if (query)
            PdhCloseQuery(query);
    }

    void open() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
            return;

        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &gpuUsage);
        PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0,
                              &diskRead);
        PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0,
                              &diskWrite);
    }

    static bool readCounter(HCOUNTER counter, double& value) {
        if (!counter)
            return false;
        PDH_FMT_COUNTERVALUE formatted{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &formatted) !=
                ERROR_SUCCESS ||
            formatted.CStatus != ERROR_SUCCESS || !std::isfinite(formatted.doubleValue))
            return false;
        value = formatted.doubleValue;
        return true;
    }

    static bool readMaximum(HCOUNTER counter, double& value) {
        if (!counter)
            return false;

        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(
            counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
        if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0)
            return false;

        std::vector<std::uint8_t> buffer(bufferSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount,
                                              items);
        if (status != ERROR_SUCCESS)
            return false;

        bool found = false;
        double maximum = 0.0;
        for (DWORD i = 0; i < itemCount; ++i) {
            const auto& item = items[i];
            if (item.FmtValue.CStatus != ERROR_SUCCESS ||
                !std::isfinite(item.FmtValue.doubleValue))
                continue;
            maximum = found ? std::max(maximum, item.FmtValue.doubleValue)
                            : item.FmtValue.doubleValue;
            found = true;
        }
        if (!found)
            return false;
        value = maximum;
        return true;
    }

    bool collect(ResourceSnapshot& result) {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            primed = false;
            return false;
        }
        if (!primed) {
            primed = true;
            return false;
        }

        bool any = false;
        double value = 0.0;
        if (readMaximum(gpuUsage, value)) {
            result.gpuPercent = std::clamp(value, 0.0, 100.0);
            result.gpuAvailable = true;
            any = true;
        }

        double readBytes = 0.0;
        double writeBytes = 0.0;
        const bool haveRead = readCounter(diskRead, readBytes);
        const bool haveWrite = readCounter(diskWrite, writeBytes);
        if (haveRead || haveWrite) {
            result.diskReadBytesPerSecond = haveRead
                                                ? static_cast<uint64_t>(std::llround(
                                                      std::max(0.0, readBytes)))
                                                : 0;
            result.diskWriteBytesPerSecond = haveWrite
                                                 ? static_cast<uint64_t>(std::llround(
                                                       std::max(0.0, writeBytes)))
                                                 : 0;
            result.diskAvailable = true;
            any = true;
        }

        return any;
    }
};

ResourceMonitor::ResourceMonitor() = default;

ResourceMonitor::~ResourceMonitor() {
    stop();
}

void ResourceMonitor::start() {
    std::lock_guard lock(mutex_);
    if (running_)
        return;

    haveCpuBaseline_ = false;
    haveNetworkBaseline_ = false;
    stopRequested_ = false;
    ResourceSnapshot empty;
    empty.revision = snapshot_.revision + 1;
    snapshot_ = empty;
    running_ = true;
    worker_ = std::thread([this] { run(); });
}

void ResourceMonitor::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_)
            return;
        stopRequested_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();

    std::lock_guard lock(mutex_);
    running_ = false;
}

ResourceSnapshot ResourceMonitor::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void ResourceMonitor::run() {
    performanceCounters_ = std::make_unique<PerformanceCounters>();
    performanceCounters_->open();
    for (;;) {
        ResourceSnapshot next = sample();
        {
            std::lock_guard lock(mutex_);
            next.revision = snapshot_.revision + 1;
            snapshot_ = next;
        }

        std::unique_lock lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopRequested_; }))
            break;
    }
    performanceCounters_.reset();
}

ResourceSnapshot ResourceMonitor::sample() {
    ResourceSnapshot result;

    if (performanceCounters_)
        performanceCounters_->collect(result);

    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const uint64_t idleValue = fileTimeValue(idle);
        const uint64_t kernelValue = fileTimeValue(kernel);
        const uint64_t userValue = fileTimeValue(user);
        if (haveCpuBaseline_ && idleValue >= previousCpuIdle_ &&
            kernelValue >= previousCpuKernel_ && userValue >= previousCpuUser_) {
            const uint64_t idleDelta = idleValue - previousCpuIdle_;
            const uint64_t kernelDelta = kernelValue - previousCpuKernel_;
            const uint64_t userDelta = userValue - previousCpuUser_;
            const uint64_t totalDelta = kernelDelta + userDelta;
            if (totalDelta > 0) {
                const uint64_t busyDelta = totalDelta > idleDelta ? totalDelta - idleDelta : 0;
                result.cpuPercent = std::clamp(
                    static_cast<double>(busyDelta) * 100.0 / static_cast<double>(totalDelta),
                    0.0, 100.0);
                result.cpuAvailable = true;
            }
        }
        previousCpuIdle_ = idleValue;
        previousCpuKernel_ = kernelValue;
        previousCpuUser_ = userValue;
        haveCpuBaseline_ = true;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.memoryPercent = std::min<unsigned>(memory.dwMemoryLoad, 100U);
        result.memoryAvailable = true;
    }

    uint64_t received = 0;
    uint64_t sent = 0;
    const uint64_t nowMs = monotonicMilliseconds();
    if (readNetworkTotals(received, sent)) {
        if (haveNetworkBaseline_ && nowMs > previousNetworkTickMs_ &&
            received >= previousNetworkIn_ && sent >= previousNetworkOut_) {
            const uint64_t elapsedMs = nowMs - previousNetworkTickMs_;
            result.downloadBytesPerSecond = static_cast<uint64_t>(std::llround(
                static_cast<long double>(received - previousNetworkIn_) * 1000.0L /
                static_cast<long double>(elapsedMs)));
            result.uploadBytesPerSecond = static_cast<uint64_t>(std::llround(
                static_cast<long double>(sent - previousNetworkOut_) * 1000.0L /
                static_cast<long double>(elapsedMs)));
            result.networkAvailable = true;
        }
        previousNetworkIn_ = received;
        previousNetworkOut_ = sent;
        previousNetworkTickMs_ = nowMs;
        haveNetworkBaseline_ = true;
    }

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus) && powerStatus.BatteryFlag != 128 &&
        powerStatus.BatteryLifePercent <= 100) {
        result.batteryPercent = powerStatus.BatteryLifePercent;
        result.batteryCharging = powerStatus.ACLineStatus == 1 &&
                                 result.batteryPercent < 100;
        result.batteryAvailable = true;
    }

    const DWORD processorCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (processorCount > 0) {
        std::vector<ProcessorPowerInformation> processors(processorCount);
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, processors.data(),
                                   static_cast<ULONG>(processors.size() *
                                                      sizeof(ProcessorPowerInformation))) ==
            ERROR_SUCCESS) {
            double totalMhz = 0.0;
            size_t validCount = 0;
            for (const auto& processor : processors) {
                if (processor.currentMhz > 0) {
                    totalMhz += processor.currentMhz;
                    ++validCount;
                }
            }
            if (validCount > 0) {
                result.cpuFrequencyGHz = totalMhz / static_cast<double>(validCount) / 1000.0;
                result.cpuFrequencyAvailable = true;
            }
        }
    }

    return result;
}

} // namespace system_monitor
