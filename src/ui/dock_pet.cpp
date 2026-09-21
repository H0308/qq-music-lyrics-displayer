#include "dock_pet.h"

#include "resource.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

struct SourceFrame {
    float left;
    float top;
    float right;
    float bottom;
};

// These rectangles are the original cells in the supplied design sheet:
// four walk frames, three interaction frames, and three listening frames.
// They are deliberately kept as source rectangles so the artwork is not redrawn.
constexpr SourceFrame kWalkFrames[] = {
    {0.0f, 0.0f, 171.0f, 170.0f},
    {171.0f, 0.0f, 343.0f, 170.0f},
    {343.0f, 0.0f, 514.0f, 170.0f},
    {514.0f, 0.0f, 686.0f, 170.0f},
};

// 图集上排的第 1/4 帧是左向，第 2/3 帧是右向。
// 方向只改变这两个同向小循环，不再把四个方向姿态串成一个循环。
constexpr int kWalkRightFrames[] = {1, 2};
constexpr int kWalkLeftFrames[] = {0, 3};

constexpr SourceFrame kInteractionFrames[] = {
    {70.0f, 160.0f, 250.0f, 316.0f},
    {250.0f, 160.0f, 440.0f, 316.0f},
    {430.0f, 160.0f, 620.0f, 316.0f},
};

constexpr SourceFrame kListeningFrames[] = {
    {70.0f, 316.0f, 250.0f, 455.0f},
    {250.0f, 316.0f, 440.0f, 455.0f},
    {430.0f, 316.0f, 610.0f, 455.0f},
};

constexpr float kCenterPaddingDip = 22.0f;
constexpr float kWalkSpeedDipPerSecond = 12.0f;
constexpr float kSeatMoveSpeedDipPerSecond = 90.0f;
constexpr float kFrameHeightDip = 44.0f;
constexpr std::uint64_t kWalkFrameMs = 230;
constexpr std::uint64_t kWalkDurationMinMs = 6500;
constexpr std::uint64_t kWalkDurationJitterMs = 5500;
constexpr std::uint64_t kListeningSwayFrameMs = 680;
constexpr std::uint64_t kBlinkDurationMs = 130;
// 歌词行节拍提示的钳制范围：半周期过短会晃眼，过长则不像在跟节奏。
constexpr std::uint32_t kSwayBeatMinMs = 320;
constexpr std::uint32_t kSwayBeatMaxMs = 1400;
// 雀跃一跳：纯位移动画，不需要新素材。
constexpr std::uint64_t kHopDurationMs = 420;
constexpr float kHopHeightDip = 9.0f;
// 行走时的身体起伏：右向两帧的脚步差异很小（素材如此），按步频合成一个
// 轻微的上下颠簸，避免看起来像贴着地面平移。
constexpr float kWalkBobDip = 1.6f;
// 行走侧倾：两个方向的步态帧差异都很细微，渲染到 44dip 后脚步交替几乎
// 不可读。随步频围绕脚底支点做正弦侧倾，用身体语言表达“重心在左右脚
// 之间转移”，比素材本身的脚步差异更容易被看到。
constexpr float kWalkRockDeg = 3.5f;
// 暂停超过该时长后进入瞌睡：底帧本身即闭眼姿态，只需停止睁眼叠加层，
// 再加缓慢呼吸与 Zzz 气泡。
constexpr std::uint64_t kSleepAfterMs = 3 * 60 * 1000;
constexpr std::uint64_t kSleepBreathPeriodMs = 3400;
constexpr std::uint64_t kStandBreathPeriodMs = 2600;
constexpr std::uint64_t kZzzCycleMs = 3000;

// 这些脸部小区域也直接取自原图。叠加时只替换眼睛窄带，
// 这样眨眼/侧看不会把身体姿态或整块面板一起切换掉。
constexpr SourceFrame kClosedEyeOverlay = {315.0f, 386.0f, 367.0f, 400.0f};
// 暂停坐姿的两只眼睛使用同一块原图像素，分别贴到固定目标，避免源帧
// 之间的透视/间距差被缩放后形成大小眼。
constexpr SourceFrame kPausedOpenEyeSource = {166.0f, 233.0f, 176.0f, 251.0f};
constexpr SourceFrame kPausedLeftEyeTarget = {64.0f, 70.0f, 81.0f, 84.0f};
constexpr SourceFrame kPausedRightEyeTarget = {100.0f, 70.0f, 117.0f, 84.0f};

// 相对于每个整帧左上角的眼睛区域。
constexpr SourceFrame kWalkEyeTargets[] = {
    {15.0f, 78.0f, 52.0f, 100.0f},
    {89.0f, 78.0f, 126.0f, 100.0f},
    {90.0f, 78.0f, 136.0f, 100.0f},
    {39.0f, 78.0f, 74.0f, 100.0f},
};

// 四帧角色在格子内的内容中心（按不透明像素包围盒量出）并不一致：左向两帧
// 相差 21 源像素，直接按格子矩形绘制会让整个身体在帧间来回横跳（抽搐感）。
// 把每帧锚到统一的内容中心 81.5（两个方向的帧对均值恰好相同，转向也不跳）。
constexpr float kWalkContentCenterX[] = {71.0f, 78.0f, 85.0f, 92.0f};
constexpr float kWalkAnchorX = 81.5f;

// 左向行走帧：右向帧 1/2 的水平镜像，由 ensureAtlas 生成到一张小位图。
// 右向帧的亮色爪垫在前后脚之间交替（迈步的核心视觉信号），左向原帧的亮爪
// 始终钉在前脚，只有抬起/放下，交替感弱，因此左向直接镜像复用右向帧。
constexpr SourceFrame kWalkMirrorFrames[] = {
    {0.0f, 0.0f, 172.0f, 170.0f},   // 镜像帧1
    {172.0f, 0.0f, 343.0f, 170.0f}, // 镜像帧2
};
constexpr float kWalkMirrorContentCenterX[] = {94.0f, 86.0f};
// 镜像帧的眨眼目标：右向帧目标按各自帧宽水平翻转。
constexpr SourceFrame kWalkMirrorEyeTargets[] = {
    {46.0f, 78.0f, 83.0f, 100.0f}, // 172 - 126 .. 172 - 89
    {35.0f, 78.0f, 81.0f, 100.0f}, // 171 - 136 .. 171 - 90
};

constexpr SourceFrame kLookUpEyeTarget = {88.0f, 62.0f, 132.0f, 82.0f};

template <typename T>
void releaseCom(T*& object) noexcept {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

const SourceFrame& walkingFrame(int direction, int phase) {
    const int* frames = direction >= 0 ? kWalkRightFrames : kWalkLeftFrames;
    return kWalkFrames[frames[std::abs(phase) % 2]];
}

int walkingFrameIndex(int direction, int phase) {
    const int* frames = direction >= 0 ? kWalkRightFrames : kWalkLeftFrames;
    return frames[std::abs(phase) % 2];
}

const SourceFrame& interactionFrame(int index) {
    return kInteractionFrames[std::clamp(index, 0, 2)];
}

const SourceFrame& listeningFrame(int index) {
    return kListeningFrames[std::clamp(index, 0, 2)];
}

const SourceFrame& listeningSwayFrame(int phase) {
    // 中间坐姿是正面闭眼的静止姿态，不放进左右摇头循环，避免经过正面时跳变。
    return kListeningFrames[(std::abs(phase) % 2) == 0 ? 0 : 2];
}

D2D1_RECT_F toD2DRect(const SourceFrame& frame) {
    return D2D1::RectF(frame.left, frame.top, frame.right, frame.bottom);
}

} // namespace

DockPet::~DockPet() {
    discardDeviceResources();
    for (auto*& format : zzzFormats_)
        releaseCom(format);
    releaseCom(dwrite_);
}

void DockPet::releaseAtlas() noexcept {
    releaseCom(atlas_);
    releaseCom(walkMirror_);
    atlasLoadAttempted_ = false;
}

void DockPet::releaseZzz() noexcept {
    releaseCom(zzzBrush_);
}

void DockPet::setSwayBeatMs(std::uint32_t ms) {
    // 只记录速度提示；相位累加器不清零，换行时摇摆不会产生跳变。
    swayFrameMs_ = ms == 0 ? 0 : std::clamp(ms, kSwayBeatMinMs, kSwayBeatMaxMs);
}

void DockPet::hop(std::uint64_t nowMs) {
    if (mode_ == DockPetMode::Hidden)
        return;
    hopStartMs_ = nowMs;
}

void DockPet::setLightTheme(bool light) {
    lightTheme_ = light;
}

void DockPet::setMode(DockPetMode mode, std::uint64_t nowMs) {
    if (mode_ == mode)
        return;

    mode_ = mode;
    lastTickMs_ = nowMs;
    modeStartedMs_ = nowMs;
    frame_ = 0;
    if (mode_ == DockPetMode::Hidden) {
        movingToSeat_ = false;
        nextBlinkMs_ = 0;
        blinkUntilMs_ = 0;
        hopStartMs_ = 0;
        return;
    }
    // 开始/恢复播放（从任何其他状态进入听歌）都雀跃一下。
    if (mode_ == DockPetMode::Listening) {
        swayAccum_ = 0.0f;
        hopStartMs_ = nowMs;
    }

    const float minX = laneLeft_ + kCenterPaddingDip;
    const float maxX = std::max(minX, laneRight_ - kCenterPaddingDip);
    if (mode_ == DockPetMode::Roaming) {
        if (x_ < minX || x_ > maxX)
            x_ = minX;
        movingToSeat_ = false;
        behavior_ = Behavior::Stand;
        behaviorStartedMs_ = nowMs;
        behaviorUntilMs_ = nowMs + 700;
        nextBlinkMs_ = nowMs + 1800 + nextRandom() % 2200;
        blinkUntilMs_ = 0;
        return;
    }

    seatX_ = std::clamp(laneLeft_ + kCenterPaddingDip, minX, maxX);
    movingToSeat_ = std::fabs(x_ - seatX_) > 0.5f;
    frame_ = 0;
    if (mode_ == DockPetMode::Paused)
        nextBlinkMs_ = nowMs + 1400 + nextRandom() % 1800;
    else
        nextBlinkMs_ = 0;
    blinkUntilMs_ = 0;
}

void DockPet::setLane(float left, float right, float height) {
    laneLeft_ = std::max(0.0f, left);
    laneRight_ = std::max(laneLeft_, right);
    laneHeight_ = std::max(1.0f, height);

    const float minX = laneLeft_ + kCenterPaddingDip;
    const float maxX = std::max(minX, laneRight_ - kCenterPaddingDip);
    x_ = std::clamp(x_, minX, maxX);
    seatX_ = std::clamp(laneLeft_ + kCenterPaddingDip, minX, maxX);
    if (mode_ == DockPetMode::Listening || mode_ == DockPetMode::Paused)
        movingToSeat_ = std::fabs(x_ - seatX_) > 0.5f;
}

void DockPet::tick(std::uint64_t nowMs) {
    if (mode_ == DockPetMode::Hidden)
        return;
    if (lastTickMs_ == 0) {
        lastTickMs_ = nowMs;
        return;
    }

    const float dt = std::clamp(static_cast<float>(nowMs - lastTickMs_) / 1000.0f,
                                0.0f, 0.10f);
    lastTickMs_ = nowMs;

    if (mode_ == DockPetMode::Listening || mode_ == DockPetMode::Paused) {
        if (movingToSeat_) {
            const float delta = seatX_ - x_;
            const float step = kSeatMoveSpeedDipPerSecond * dt;
            if (std::fabs(delta) <= step) {
                x_ = seatX_;
                movingToSeat_ = false;
            } else {
                x_ += delta > 0.0f ? step : -step;
            }
        }
        if (!movingToSeat_) {
            if (mode_ == DockPetMode::Listening) {
                // 摇摆节拍来自宿主给出的歌词行时长（setSwayBeatMs）；相位按帧
                // 累加，行时长变化只改变步进速度，不会在换行瞬间跳帧。
                const float frameMs = swayFrameMs_ != 0
                                          ? static_cast<float>(swayFrameMs_)
                                          : static_cast<float>(kListeningSwayFrameMs);
                swayAccum_ += dt * 1000.0f / frameMs;
                // 相位只需 2 帧循环，收拢避免长时间播放后 float 精度损失。
                if (swayAccum_ >= 2.0f)
                    swayAccum_ = std::fmod(swayAccum_, 2.0f);
                frame_ = static_cast<int>(swayAccum_) % 2;
            } else {
                // 暂停时身体固定在正面坐姿，只改变脸部叠加层；瞌睡后底帧即闭眼，
                // 不再调度眨眼。
                frame_ = 0;
                if (sleeping()) {
                    blinkUntilMs_ = 0;
                } else if (blinkUntilMs_ != 0 && nowMs >= blinkUntilMs_) {
                    blinkUntilMs_ = 0;
                    nextBlinkMs_ = nowMs + 2300 + nextRandom() % 2600;
                } else if (blinkUntilMs_ == 0 && nowMs >= nextBlinkMs_) {
                    blinkUntilMs_ = nowMs + kBlinkDurationMs;
                }
            }
        }
        return;
    }

    switch (behavior_) {
    case Behavior::Walk: {
        if (blinkUntilMs_ != 0 && nowMs >= blinkUntilMs_) {
            blinkUntilMs_ = 0;
            nextBlinkMs_ = nowMs + 1800 + nextRandom() % 2200;
        } else if (blinkUntilMs_ == 0 && nowMs >= nextBlinkMs_) {
            blinkUntilMs_ = nowMs + kBlinkDurationMs;
        }
        x_ += static_cast<float>(direction_) * kWalkSpeedDipPerSecond * dt;
        const float minX = laneLeft_ + kCenterPaddingDip;
        const float maxX = std::max(minX, laneRight_ - kCenterPaddingDip);
        const bool hitLeft = x_ <= minX;
        const bool hitRight = x_ >= maxX;
        if (hitLeft || hitRight || nowMs >= behaviorUntilMs_) {
            x_ = std::clamp(x_, minX, maxX);
            if (hitLeft)
                direction_ = 1;
            else if (hitRight)
                direction_ = -1;
            behavior_ = Behavior::Stand;
            behaviorStartedMs_ = nowMs;
            behaviorUntilMs_ = nowMs + 600 + nextRandom() % 900;
            frame_ = 0;
            nextBlinkMs_ = 0;
            blinkUntilMs_ = 0;
        } else {
            // 以当前这段行走的起点计步，避免每次进状态时从随机相位跳入。
            frame_ = static_cast<int>(((nowMs - behaviorStartedMs_) /
                                        kWalkFrameMs) % 2);
        }
        break;
    }
    case Behavior::Sit:
        blinkUntilMs_ = 0;
        nextBlinkMs_ = 0;
        frame_ = 1;
        if (nowMs >= behaviorUntilMs_) {
            behavior_ = Behavior::Stand;
            behaviorStartedMs_ = nowMs;
            behaviorUntilMs_ = nowMs + 500 + nextRandom() % 700;
            frame_ = 0;
        }
        break;
    case Behavior::LookUp:
        // 仰望全程固定使用设计稿中间那张抬头姿势，眨眼只覆盖眼睛窄带。
        frame_ = 1;
        if (blinkUntilMs_ != 0 && nowMs >= blinkUntilMs_) {
            blinkUntilMs_ = 0;
            nextBlinkMs_ = nowMs + 1700 + nextRandom() % 1800;
        } else if (blinkUntilMs_ == 0 && nowMs >= nextBlinkMs_) {
            blinkUntilMs_ = nowMs + kBlinkDurationMs;
        }
        if (nowMs >= behaviorUntilMs_) {
            behavior_ = Behavior::Stand;
            behaviorStartedMs_ = nowMs;
            behaviorUntilMs_ = nowMs + 500 + nextRandom() % 700;
            frame_ = 0;
            nextBlinkMs_ = 0;
            blinkUntilMs_ = 0;
        }
        break;
    case Behavior::AdjustHeadphones:
        blinkUntilMs_ = 0;
        nextBlinkMs_ = 0;
        frame_ = 0;
        if (nowMs >= behaviorUntilMs_) {
            behavior_ = Behavior::Stand;
            behaviorStartedMs_ = nowMs;
            behaviorUntilMs_ = nowMs + 500 + nextRandom() % 700;
            frame_ = 0;
        }
        break;
    case Behavior::Stand:
        blinkUntilMs_ = 0;
        nextBlinkMs_ = 0;
        frame_ = 0;
        if (nowMs >= behaviorUntilMs_)
            chooseRoamingBehavior(nowMs);
        break;
    }
}

bool DockPet::animating() const noexcept {
    return mode_ == DockPetMode::Roaming || mode_ == DockPetMode::Listening ||
           mode_ == DockPetMode::Paused || movingToSeat_;
}

bool DockPet::sleeping() const noexcept {
    return mode_ == DockPetMode::Paused && lastTickMs_ != 0 &&
           lastTickMs_ - modeStartedMs_ >= kSleepAfterMs;
}

void DockPet::discardDeviceResources() noexcept {
    releaseAtlas();
    releaseZzz();
}

bool DockPet::ensureAtlas(ID2D1DeviceContext* target) {
    if (atlas_)
        return true;
    if (!target || atlasLoadAttempted_)
        return false;

    atlasLoadAttempted_ = true;

    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_DOCK_PET_SPRITES), RT_RCDATA);
    if (!resource)
        return false;
    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    const auto* resourceBytes = static_cast<const BYTE*>(LockResource(loaded));
    if (!loaded || !resourceBytes || resourceSize == 0)
        return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    std::vector<BYTE> pixels;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(resourceBytes), resourceSize);
    }
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                              &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);
    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        pixels.resize(static_cast<size_t>(width) * height * 4);
        hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()),
                                   pixels.data());
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (SUCCEEDED(hr)) {
        ID2D1Bitmap1* bitmap = nullptr;
        hr = target->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4,
                                  &properties, &bitmap);
        if (SUCCEEDED(hr))
            atlas_ = bitmap;
        else
            releaseCom(bitmap);
    }

    if (atlas_ && width >= 686 && height >= 170) {
        // 生成左向行走用的镜像位图：右向帧 1/2 逐帧水平翻转（见
        // kWalkMirrorFrames）。失败时左向回退到原帧，只是步态偏弱。
        const struct {
            UINT srcLeft;
            UINT srcWidth;
            UINT dstLeft;
        } mirrorCells[] = {{171u, 172u, 0u}, {343u, 171u, 172u}};
        constexpr UINT mirrorWidth = 343;
        constexpr UINT mirrorHeight = 170;
        std::vector<BYTE> mirrorPixels(mirrorWidth * mirrorHeight * 4, 0);
        for (const auto& cell : mirrorCells) {
            for (UINT y = 0; y < mirrorHeight; ++y) {
                const BYTE* srcRow =
                    pixels.data() + (static_cast<size_t>(y) * width + cell.srcLeft) * 4;
                BYTE* dstRow = mirrorPixels.data() +
                               (static_cast<size_t>(y) * mirrorWidth + cell.dstLeft) * 4;
                for (UINT x = 0; x < cell.srcWidth; ++x) {
                    const BYTE* sp = srcRow + (cell.srcWidth - 1 - x) * 4;
                    BYTE* dp = dstRow + x * 4;
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                    dp[3] = sp[3];
                }
            }
        }
        ID2D1Bitmap1* mirrorBitmap = nullptr;
        if (SUCCEEDED(target->CreateBitmap(D2D1::SizeU(mirrorWidth, mirrorHeight),
                                           mirrorPixels.data(), mirrorWidth * 4,
                                           &properties, &mirrorBitmap)))
            walkMirror_ = mirrorBitmap;
        else
            releaseCom(mirrorBitmap);
    }

    releaseCom(converter);
    releaseCom(frame);
    releaseCom(decoder);
    releaseCom(stream);
    releaseCom(factory);
    return atlas_ != nullptr;
}

void DockPet::draw(ID2D1DeviceContext* target) {
    if (mode_ == DockPetMode::Hidden || !target || laneRight_ <= laneLeft_ || !ensureAtlas(target))
        return;

    const bool sleepNow = sleeping();
    const SourceFrame* source = nullptr;
    ID2D1Bitmap* frameBitmap = atlas_;
    int walkingSourceIndex = -1;
    int walkMirrorPhase = -1;
    bool drawClosedFace = false;
    bool drawPausedEyes = false;
    if (mode_ == DockPetMode::Roaming) {
        switch (behavior_) {
        case Behavior::Walk:
            if (direction_ < 0 && walkMirror_) {
                // 左向使用右向帧的镜像（见 kWalkMirrorFrames）
                walkMirrorPhase = std::abs(frame_) % 2;
                source = &kWalkMirrorFrames[walkMirrorPhase];
                frameBitmap = walkMirror_;
            } else {
                walkingSourceIndex = walkingFrameIndex(direction_, frame_);
                source = &walkingFrame(direction_, frame_);
            }
            drawClosedFace = blinkUntilMs_ != 0;
            break;
        case Behavior::Sit:
            source = &listeningFrame(1);
            break;
        case Behavior::LookUp:
            source = &interactionFrame(frame_);
            drawClosedFace = frame_ == 1 && blinkUntilMs_ != 0;
            break;
        case Behavior::AdjustHeadphones:
            source = &interactionFrame(2);
            break;
        case Behavior::Stand:
            source = &interactionFrame(frame_);
            break;
        }
    } else if (mode_ == DockPetMode::Listening) {
        source = &listeningSwayFrame(frame_);
    } else {
        source = &listeningFrame(1);
        // 瞌睡后底帧的闭眼姿态直接露出，不再叠加睁眼图块。
        drawPausedEyes = !movingToSeat_ && !sleepNow && blinkUntilMs_ == 0;
    }

    const float sourceWidth = source->right - source->left;
    const float sourceHeight = source->bottom - source->top;
    float destinationHeight = std::clamp(
        laneHeight_ * 0.92f, kFrameHeightDip - 2.0f, kFrameHeightDip + 2.0f);
    float destinationWidth = destinationHeight * sourceWidth / sourceHeight;

    // 呼吸：静止姿态（待机站立/坐、瞌睡）沿底部锚点做缓慢的等比缩放，
    // 让画面在两次行为之间不至于完全冻住。
    constexpr float kTwoPi = 6.28318530717958647692f;
    float breathPeriodMs = 0.0f;
    float breathAmount = 0.0f;
    if (sleepNow) {
        breathPeriodMs = static_cast<float>(kSleepBreathPeriodMs);
        breathAmount = 0.028f;
    } else if (mode_ == DockPetMode::Roaming &&
               (behavior_ == Behavior::Stand || behavior_ == Behavior::Sit)) {
        breathPeriodMs = static_cast<float>(kStandBreathPeriodMs);
        breathAmount = 0.02f;
    }
    if (breathPeriodMs > 0.0f) {
        const float phase =
            static_cast<float>(lastTickMs_ % static_cast<std::uint64_t>(breathPeriodMs)) /
            breathPeriodMs;
        const float s = 1.0f + breathAmount * std::sinf(kTwoPi * phase);
        destinationWidth *= s;
        destinationHeight *= s;
    }

    // 行走帧按内容中心对齐（见 kWalkContentCenterX）；脸部叠加层基于
    // destination 计算，会随偏移一起移动，无需额外处理。
    float walkOffsetDip = 0.0f;
    if (walkingSourceIndex >= 0)
        walkOffsetDip = (kWalkAnchorX - kWalkContentCenterX[walkingSourceIndex]) *
                        (destinationWidth / sourceWidth);
    else if (walkMirrorPhase >= 0)
        walkOffsetDip = (kWalkAnchorX - kWalkMirrorContentCenterX[walkMirrorPhase]) *
                        (destinationWidth / sourceWidth);
    const float destinationLeft = x_ - destinationWidth * 0.5f + walkOffsetDip;
    float destinationBottom = laneHeight_ + 1.0f;
    // 行走起伏：与踏步帧同源计时，每步落地（帧切换点）时身体最低。
    if (mode_ == DockPetMode::Roaming && behavior_ == Behavior::Walk &&
        lastTickMs_ >= behaviorStartedMs_) {
        const float stepPhase = static_cast<float>(
            (lastTickMs_ - behaviorStartedMs_) % kWalkFrameMs) /
            static_cast<float>(kWalkFrameMs);
        destinationBottom -=
            kWalkBobDip * std::fabs(std::sinf(kTwoPi * 0.5f * stepPhase));
    }
    // 雀跃一跳：整体沿抛物线离地，双脚不需要专门的离地帧。
    if (hopStartMs_ != 0 && lastTickMs_ >= hopStartMs_) {
        const float p =
            static_cast<float>(lastTickMs_ - hopStartMs_) / static_cast<float>(kHopDurationMs);
        if (p >= 1.0f)
            hopStartMs_ = 0;
        else
            destinationBottom -= 4.0f * kHopHeightDip * p * (1.0f - p);
    }
    const D2D1_RECT_F destination = D2D1::RectF(
        destinationLeft, destinationBottom - destinationHeight,
        destinationLeft + destinationWidth, destinationBottom);
    const D2D1_RECT_F sourceRect = toD2DRect(*source);

    // 行走侧倾（见 kWalkRockDeg）：围绕脚底支点旋转整个角色，眨眼叠加层
    // 在同一变换内绘制，随身体一起倾。旋转周期 = 2 个踏步帧。
    float walkRockDeg = 0.0f;
    if (mode_ == DockPetMode::Roaming && behavior_ == Behavior::Walk &&
        lastTickMs_ >= behaviorStartedMs_) {
        const float frameT = static_cast<float>(lastTickMs_ - behaviorStartedMs_) /
                             static_cast<float>(kWalkFrameMs);
        walkRockDeg = kWalkRockDeg * std::sinf(kTwoPi * 0.5f * frameT);
    }
    D2D1_MATRIX_3X2_F savedTransform{};
    const bool rocked = std::fabs(walkRockDeg) > 0.05f;
    if (rocked) {
        target->GetTransform(&savedTransform);
        target->SetTransform(D2D1::Matrix3x2F::Rotation(
                                 walkRockDeg, D2D1::Point2F(x_, laneHeight_)) *
                                 savedTransform);
    }

    target->DrawBitmap(frameBitmap, &destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                       &sourceRect);

    // 把脸部小图按当前整帧的比例贴回去，动作期间身体不发生跳帧。
    const auto drawFaceOverlay = [&](const SourceFrame& overlay,
                                     const SourceFrame& targetInBaseFrame) {
        const float scaleX = destinationWidth / sourceWidth;
        const float scaleY = destinationHeight / sourceHeight;
        const D2D1_RECT_F overlayDestination = D2D1::RectF(
            destination.left + targetInBaseFrame.left * scaleX,
            destination.top + targetInBaseFrame.top * scaleY,
            destination.left + targetInBaseFrame.right * scaleX,
            destination.top + targetInBaseFrame.bottom * scaleY);
        const D2D1_RECT_F overlaySource = toD2DRect(overlay);
        target->DrawBitmap(atlas_, &overlayDestination, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                           &overlaySource);
    };

    if (drawClosedFace) {
        if (behavior_ == Behavior::Walk && walkingSourceIndex >= 0) {
            drawFaceOverlay(kClosedEyeOverlay, kWalkEyeTargets[walkingSourceIndex]);
        } else if (behavior_ == Behavior::Walk && walkMirrorPhase >= 0) {
            // 闭眼图块近似左右对称，直接取原图集贴到镜像帧的翻转目标位置。
            drawFaceOverlay(kClosedEyeOverlay, kWalkMirrorEyeTargets[walkMirrorPhase]);
        } else if (behavior_ == Behavior::LookUp) {
            drawFaceOverlay(kClosedEyeOverlay, kLookUpEyeTarget);
        }
    } else if (drawPausedEyes) {
        drawFaceOverlay(kPausedOpenEyeSource, kPausedLeftEyeTarget);
        drawFaceOverlay(kPausedOpenEyeSource, kPausedRightEyeTarget);
    }

    if (rocked)
        target->SetTransform(savedTransform);

    if (sleepNow && !movingToSeat_) {
        drawZzz(target, destination.left + destinationWidth * 0.62f,
                destination.top + destinationHeight * 0.08f);
    }
}

void DockPet::drawZzz(ID2D1DeviceContext* target, float headX, float headY) {
    // DWrite 工厂与文字格式都是设备无关资源，只需懒创建一次。
    if (!dwrite_ &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite_))))
        return;
    constexpr float kZzzSizes[3] = {9.0f, 11.0f, 13.5f};
    for (int i = 0; i < 3; ++i) {
        if (!zzzFormats_[i] &&
            FAILED(dwrite_->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC,
                DWRITE_FONT_STRETCH_NORMAL, kZzzSizes[i], L"en-us", &zzzFormats_[i])))
            return;
    }
    if (!zzzBrush_ &&
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f),
                                             &zzzBrush_)))
        return;

    const float base = lightTheme_ ? 0.10f : 1.0f;
    const float cycleT =
        static_cast<float>(lastTickMs_ % kZzzCycleMs) / static_cast<float>(kZzzCycleMs);
    for (int i = 0; i < 3; ++i) {
        // 三个 Z 依次从头顶向右侧飘出，透明度随生命周期淡入淡出。宠物几乎占满
        // 整条走廊的高度，向上飘会很快被窗口顶裁掉，所以漂移以水平为主。
        const float q = std::fmod(cycleT + static_cast<float>(i) / 3.0f, 1.0f);
        const float alphaIn = std::min(q / 0.18f, 1.0f);
        const float alphaOut = std::min((1.0f - q) / 0.25f, 1.0f);
        const float alpha = 0.72f * std::min(alphaIn, alphaOut);
        const float x = headX + q * 10.0f;
        const float y = headY - 2.0f - q * 6.0f;
        zzzBrush_->SetColor(D2D1::ColorF(base, base, base, alpha));
        const D2D1_RECT_F layout = D2D1::RectF(x, y, x + 24.0f, y + 24.0f);
        target->DrawText(L"Z", 1, zzzFormats_[i], layout, zzzBrush_);
    }
}

void DockPet::chooseRoamingBehavior(std::uint64_t nowMs) {
    // 夜间（23:00–07:00）更安静：久坐久看、少走动；白天保持原有分布。
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    const bool night = localTime.wHour >= 23 || localTime.wHour < 7;
    const int sitBelow = night ? 4 : 2;
    const int lookUpBelow = night ? 7 : 4;
    const std::uint32_t adjustChoice = night ? 7u : 4u;

    const std::uint32_t choice = nextRandom() % 10;
    if (choice < static_cast<std::uint32_t>(sitBelow)) {
        behavior_ = Behavior::Sit;
        behaviorStartedMs_ = nowMs;
        behaviorUntilMs_ = night ? nowMs + 2800 + nextRandom() % 1800
                                 : nowMs + 1800 + nextRandom() % 1200;
        frame_ = 1;
        nextBlinkMs_ = 0;
        blinkUntilMs_ = 0;
        return;
    }
    if (choice < static_cast<std::uint32_t>(lookUpBelow)) {
        behavior_ = Behavior::LookUp;
        behaviorStartedMs_ = nowMs;
        behaviorUntilMs_ = nowMs + 2200 + nextRandom() % 1200;
        frame_ = 1;
        nextBlinkMs_ = nowMs + 900 + nextRandom() % 1100;
        blinkUntilMs_ = 0;
        return;
    }
    if (choice == adjustChoice) {
        behavior_ = Behavior::AdjustHeadphones;
        behaviorStartedMs_ = nowMs;
        behaviorUntilMs_ = nowMs + 900;
        frame_ = 0;
        nextBlinkMs_ = 0;
        blinkUntilMs_ = 0;
        return;
    }
    behavior_ = Behavior::Walk;
    behaviorStartedMs_ = nowMs;
    const float minX = laneLeft_ + kCenterPaddingDip;
    const float maxX = std::max(minX, laneRight_ - kCenterPaddingDip);
    if (x_ <= minX + 0.5f)
        direction_ = 1;
    else if (x_ >= maxX - 0.5f)
        direction_ = -1;
    else
        direction_ = (nextRandom() & 1u) == 0 ? -1 : 1;
    behaviorUntilMs_ = nowMs + kWalkDurationMinMs +
                       nextRandom() % kWalkDurationJitterMs;
    frame_ = 0;
    nextBlinkMs_ = nowMs + 1600 + nextRandom() % 2200;
    blinkUntilMs_ = 0;
}

std::uint32_t DockPet::nextRandom() {
    // 只用于行为节奏，不用于安全或业务逻辑；避免为宠物引入随机线程。
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return randomState_;
}
