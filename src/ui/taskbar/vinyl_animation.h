#pragma once
#include <windows.h>

// 黑胶只受自己的播放与唱臂时钟控制，不复用歌词或沉浸显隐的暂停状态。
class VinylAnimation {
public:
    bool advance(ULONGLONG now, bool playing, bool enabled);
    float angle() const { return vinylAngleDeg_; }
    float armAngle() const { return vinylArmAngleDeg_; }
    bool armMoving() const { return vinylArmStartMs_ != 0; }
    void resetDisc() { vinylAngleDeg_ = 0.0f; }
    void resetClock(ULONGLONG now) { vinylTickMs_ = now; }

private:
    float vinylAngleDeg_ = 0.0f;
    ULONGLONG vinylTickMs_ = 0;
    float vinylArmAngleDeg_ = -18.0f;
    float vinylArmFromDeg_ = -18.0f;
    float vinylArmTargetDeg_ = -18.0f;
    ULONGLONG vinylArmStartMs_ = 0;
};
