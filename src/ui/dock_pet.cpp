#include "dock_pet.h"

#include "resource.h"

#include <d2d1_1.h>
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
    releaseAtlas();
}

void DockPet::releaseAtlas() noexcept {
    releaseCom(atlas_);
    atlasLoadAttempted_ = false;
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
        return;
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
                frame_ = static_cast<int>(((nowMs - modeStartedMs_) /
                                            kListeningSwayFrameMs) % 2);
            } else {
                // 暂停时身体固定在正面坐姿，只改变脸部叠加层。
                frame_ = 0;
                if (blinkUntilMs_ != 0 && nowMs >= blinkUntilMs_) {
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

void DockPet::discardDeviceResources() noexcept {
    releaseAtlas();
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

    if (SUCCEEDED(hr)) {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        ID2D1Bitmap1* bitmap = nullptr;
        hr = target->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4,
                                  &properties, &bitmap);
        if (SUCCEEDED(hr))
            atlas_ = bitmap;
        else
            releaseCom(bitmap);
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

    const SourceFrame* source = nullptr;
    int walkingSourceIndex = -1;
    bool drawClosedFace = false;
    bool drawPausedEyes = false;
    if (mode_ == DockPetMode::Roaming) {
        switch (behavior_) {
        case Behavior::Walk:
            walkingSourceIndex = walkingFrameIndex(direction_, frame_);
            source = &walkingFrame(direction_, frame_);
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
        drawPausedEyes = !movingToSeat_ && blinkUntilMs_ == 0;
    }

    const float sourceWidth = source->right - source->left;
    const float sourceHeight = source->bottom - source->top;
    const float destinationHeight = std::clamp(
        laneHeight_ * 0.92f, kFrameHeightDip - 2.0f, kFrameHeightDip + 2.0f);
    const float destinationWidth = destinationHeight * sourceWidth / sourceHeight;
    const float destinationLeft = x_ - destinationWidth * 0.5f;
    const float destinationBottom = laneHeight_ + 1.0f;
    const D2D1_RECT_F destination = D2D1::RectF(
        destinationLeft, destinationBottom - destinationHeight,
        destinationLeft + destinationWidth, destinationBottom);
    const D2D1_RECT_F sourceRect = toD2DRect(*source);
    target->DrawBitmap(atlas_, &destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
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
        } else if (behavior_ == Behavior::LookUp) {
            drawFaceOverlay(kClosedEyeOverlay, kLookUpEyeTarget);
        }
    } else if (drawPausedEyes) {
        drawFaceOverlay(kPausedOpenEyeSource, kPausedLeftEyeTarget);
        drawFaceOverlay(kPausedOpenEyeSource, kPausedRightEyeTarget);
    }
}

void DockPet::chooseRoamingBehavior(std::uint64_t nowMs) {
    const std::uint32_t choice = nextRandom() % 10;
    if (choice < 2) {
        behavior_ = Behavior::Sit;
        behaviorStartedMs_ = nowMs;
        behaviorUntilMs_ = nowMs + 1800 + nextRandom() % 1200;
        frame_ = 1;
        nextBlinkMs_ = 0;
        blinkUntilMs_ = 0;
        return;
    }
    if (choice < 4) {
        behavior_ = Behavior::LookUp;
        behaviorStartedMs_ = nowMs;
        behaviorUntilMs_ = nowMs + 2200 + nextRandom() % 1200;
        frame_ = 1;
        nextBlinkMs_ = nowMs + 900 + nextRandom() % 1100;
        blinkUntilMs_ = 0;
        return;
    }
    if (choice == 4) {
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
