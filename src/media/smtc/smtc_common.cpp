#include "media/smtc/smtc_common.h"

#include <chrono>
#include <mutex>

#include <winrt/Windows.Storage.Streams.h>

namespace smtc {

int64_t nowUtcMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t timeSpanMs(winrt::Windows::Foundation::TimeSpan value) {
    return value.count() / 10000;
}

int64_t lastUpdatedMs(winrt::Windows::Foundation::DateTime value) {
    // DateTime 使用 1601 年起的 100ns 单位，换算为 Unix 毫秒。
    constexpr int64_t kFileTimeEpochOffsetMs = 11644473600000LL;
    int64_t ms = value.time_since_epoch().count() / 10000 - kFileTimeEpochOffsetMs;
    return ms > 0 ? ms : 0;
}

bool sameSession(const Session& left, const Session& right) noexcept {
    if (winrt::get_abi(left) == winrt::get_abi(right))
        return true;
    if (!left || !right)
        return false;
    try {
        // GetCurrentSession() 和 GetSessions() 可能返回同一逻辑会话的不同
        // WinRT 对象实例，因此还要比较来源标识。
        auto leftSource = left.SourceAppUserModelId();
        auto rightSource = right.SourceAppUserModelId();
        return !leftSource.empty() && leftSource == rightSource;
    } catch (...) {
        return false;
    }
}

PlaybackStatus mapStatus(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status) {
    using Status = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    switch (status) {
    case Status::Playing:
        return PlaybackStatus::Playing;
    case Status::Paused:
        return PlaybackStatus::Paused;
    case Status::Stopped:
        return PlaybackStatus::Stopped;
    default:
        return PlaybackStatus::Other;
    }
}

std::shared_ptr<const std::vector<uint8_t>> readThumbnail(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& props) {
    try {
        auto ref = props.Thumbnail();
        if (!ref)
            return nullptr;
        auto streamOp = ref.OpenReadAsync();
        if (!streamOp)
            return nullptr;
        auto stream = streamOp.get();
        if (!stream)
            return nullptr;
        uint64_t size = stream.Size();
        if (size == 0 || size > 4 * 1024 * 1024)
            return nullptr;
        auto buffer = std::make_shared<std::vector<uint8_t>>((size_t)size);
        winrt::Windows::Storage::Streams::DataReader reader(stream);
        auto loadOp = reader.LoadAsync((uint32_t)size);
        if (!loadOp)
            return nullptr;
        loadOp.get();
        reader.ReadBytes(winrt::array_view<uint8_t>(buffer->data(), (uint32_t)buffer->size()));
        return buffer;
    } catch (...) {
        return nullptr;
    }
}

} // namespace smtc
