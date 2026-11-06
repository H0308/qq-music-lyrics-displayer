#include "ui/taskbar/vinyl_animation.h"
#include <algorithm>
#include <cmath>
namespace {
constexpr float kVinylArmParkedAngle = -18.0f;
constexpr ULONGLONG kVinylArmDurationMs = 240;
constexpr float kVinylRotationDegPerSecond = 30.0f;
} // namespace
bool VinylAnimation::advance(ULONGLONG now, bool playing, bool enabled) {

    if (vinylTickMs_ == 0)
        vinylTickMs_ = now;
    const ULONGLONG elapsed = now - vinylTickMs_;
    vinylTickMs_ = now;
    const float previousArmAngle = vinylArmAngleDeg_;
    const bool armWasMoving = vinylArmStartMs_ != 0;

    // 先采样尚未结束的动作，快速切换播放状态时从当前位置反向，不跳回端点。
    if (vinylArmStartMs_ != 0) {
        const float t =
            std::min(1.0f, static_cast<float>(now - vinylArmStartMs_) / kVinylArmDurationMs);
        const float eased = t * t * (3.0f - 2.0f * t);
        vinylArmAngleDeg_ = vinylArmFromDeg_ + (vinylArmTargetDeg_ - vinylArmFromDeg_) * eased;
        if (t >= 1.0f) {
            vinylArmAngleDeg_ = vinylArmTargetDeg_;
            vinylArmStartMs_ = 0;
        }
    }
    const float target = playing ? 0.0f : kVinylArmParkedAngle;
    const bool animate = enabled;
    if (!animate) {
        vinylArmAngleDeg_ = vinylArmFromDeg_ = vinylArmTargetDeg_ = target;
        vinylArmStartMs_ = 0;
    } else if (target != vinylArmTargetDeg_) {
        vinylArmFromDeg_ = vinylArmAngleDeg_;
        vinylArmTargetDeg_ = target;
        vinylArmStartMs_ = vinylArmAngleDeg_ == target ? 0 : now;
    }
    // 收尾帧也要重绘，不能在清除动作标记后被静止判定跳过。
    const bool changed = vinylArmAngleDeg_ != previousArmAngle;

    // 暂停立即停盘；恢复播放时等唱臂落稳，下一帧才开始推进角度。
    if (!animate || !playing || armWasMoving || vinylArmStartMs_ != 0 || vinylArmAngleDeg_ != 0.0f)
        return changed;
    vinylAngleDeg_ = std::fmod(vinylAngleDeg_ + static_cast<float>(elapsed) *
                                                    kVinylRotationDegPerSecond / 1000.0f,
                               360.0f);
    if (vinylAngleDeg_ < 0.0f)
        vinylAngleDeg_ += 360.0f;

    return changed;
}
