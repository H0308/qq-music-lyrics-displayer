#pragma once
#include "ui/taskbar_host.h"
#include <array>
#include <cstdint>
namespace taskbar_detail {
enum class RenderInvalidation : uint32_t {
    Paint = 1u << 0,
    Text = 1u << 1,
    SongInfo = 1u << 2,
    Geometry = 1u << 3,
    Layout = 1u << 4,
    Cover = 1u << 5,
    PlatformIcon = 1u << 6,
};

using RenderInvalidationMask = uint32_t;

constexpr RenderInvalidationMask toMask(RenderInvalidation value) {
    return static_cast<RenderInvalidationMask>(value);
}

struct InvalidationSnapshot {
    RenderInvalidationMask mask = 0;
    std::array<uint64_t, 7> generations{};
};

class InvalidationState {
public:
    InvalidationState()
        : pending_(toMask(RenderInvalidation::Paint) | toMask(RenderInvalidation::Text) |
                   toMask(RenderInvalidation::SongInfo) | toMask(RenderInvalidation::Geometry) |
                   toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Cover) |
                   toMask(RenderInvalidation::PlatformIcon)) {}

    void request(RenderInvalidation value) { request(toMask(value)); }

    void request(RenderInvalidationMask mask) {
        pending_ |= mask;
        for (size_t i = 0; i < generations_.size(); ++i) {
            if (mask & (1u << i))
                ++generations_[i];
        }
    }

    bool contains(RenderInvalidation value) const { return (pending_ & toMask(value)) != 0; }

    bool any() const { return pending_ != 0; }

    InvalidationSnapshot begin() const { return InvalidationSnapshot{pending_, generations_}; }

    void commit(const InvalidationSnapshot& snapshot) {
        for (size_t i = 0; i < generations_.size(); ++i) {
            const RenderInvalidationMask bit = 1u << i;
            if ((snapshot.mask & bit) && generations_[i] == snapshot.generations[i])
                pending_ &= ~bit;
        }
    }

private:
    RenderInvalidationMask pending_ = 0;
    std::array<uint64_t, 7> generations_{};
};

// 渲染阶段集中在一个对象内：外部只看到枚举/查询，窗口、转场和资源阶段
// 不再由宿主成员函数各自拼接多个布尔量表达。
class RenderState {
public:
    enum class WindowPhase {
        Hidden,
        Visible,
    };
    enum class DCompTransitionPhase {
        Inactive,
        Active,
        EndRequested,
    };
    enum class SongTransitionPhase {
        Idle,
        Pending,
    };
    enum class DeviceResourcePhase {
        Uninitialized,
        Ready,
    };

    RenderMode mode() const { return mode_; }
    WindowPhase windowPhase() const { return window_; }
    DCompTransitionPhase dcompTransitionPhase() const { return dcompTransition_; }
    DeviceResourcePhase deviceResourcePhase() const { return deviceResources_; }
    bool sessionVisible() const { return sessionVisible_; }
    bool visibilitySuppressed() const { return visibilitySuppressed_; }
    bool songTransitionPending() const { return songTransition_ == SongTransitionPhase::Pending; }

    void setMode(RenderMode mode) { mode_ = mode; }
    void setWindowPhase(WindowPhase phase) { window_ = phase; }
    void setSessionVisible(bool visible) { sessionVisible_ = visible; }
    void setVisibilitySuppressed(bool suppressed) { visibilitySuppressed_ = suppressed; }
    void setDCompTransitionPhase(DCompTransitionPhase phase) { dcompTransition_ = phase; }
    void setDeviceResourcePhase(DeviceResourcePhase phase) { deviceResources_ = phase; }
    void setSongTransitionPending(bool pending) {
        songTransition_ = pending ? SongTransitionPhase::Pending : SongTransitionPhase::Idle;
    }

    void requestInvalidation(RenderInvalidation value) { invalidation_.request(value); }
    void requestInvalidation(RenderInvalidationMask mask) { invalidation_.request(mask); }
    bool isInvalidated(RenderInvalidation value) const { return invalidation_.contains(value); }
    bool hasInvalidation() const { return invalidation_.any(); }
    InvalidationSnapshot beginInvalidation() const { return invalidation_.begin(); }
    void commitInvalidation(const InvalidationSnapshot& snapshot) {
        invalidation_.commit(snapshot);
    }

private:
    RenderMode mode_ = RenderMode::Normal;
    WindowPhase window_ = WindowPhase::Hidden;
    DCompTransitionPhase dcompTransition_ = DCompTransitionPhase::Inactive;
    DeviceResourcePhase deviceResources_ = DeviceResourcePhase::Uninitialized;
    bool sessionVisible_ = false;
    bool visibilitySuppressed_ = false;
    SongTransitionPhase songTransition_ = SongTransitionPhase::Idle;
    InvalidationState invalidation_;
};

} // namespace taskbar_detail
