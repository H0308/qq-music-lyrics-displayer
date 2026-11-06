#pragma once

#include <atomic>
#include <cstdint>

// 当前音乐应用的独立音量状态；请求版本仅用于运行期回写，不保存到配置。
struct AppVolumeState {
    bool available = false;
    int percent = 0; // 0-100
    bool muted = false;
    uint64_t requestRevision = 0;
};

struct AppVolumeRequest {
    int percent = 0;
    uint64_t revision = 0;

    // 多个窗口和显示线程共用递增版本，防止旧请求或旧回写覆盖新操作。
    static AppVolumeRequest create(int percent) {
        static std::atomic<uint64_t> revision{0};
        return {percent, revision.fetch_add(1, std::memory_order_relaxed) + 1};
    }
};
