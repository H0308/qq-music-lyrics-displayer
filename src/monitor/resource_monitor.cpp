#include "resource_monitor.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace system_monitor {
namespace {

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
}

ResourceSnapshot ResourceMonitor::sample() {
    ResourceSnapshot result;

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

    return result;
}

} // namespace system_monitor
