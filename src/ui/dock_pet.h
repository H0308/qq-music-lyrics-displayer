#pragma once

#include <cstdint>

struct ID2D1Bitmap;
struct ID2D1DeviceContext;

// Dock 内的原始像素宠物状态。角色像素来自 asset/dock_pet_sprites.png，
// 这里仅负责动作帧、位置和播放状态，不参与鼠标命中逻辑。
enum class DockPetMode {
    Hidden,
    Roaming,
    Listening,
    Paused,
};

class DockPet {
public:
    ~DockPet();

    void setMode(DockPetMode mode, std::uint64_t nowMs);
    void setLane(float left, float right, float height);
    void tick(std::uint64_t nowMs);

    bool animating() const noexcept;

    // D2D 设备重建时释放由该设备创建的图集位图。
    void discardDeviceResources() noexcept;

    void draw(ID2D1DeviceContext* target);

private:
    enum class Behavior {
        Stand,
        Walk,
        Sit,
        LookUp,
        AdjustHeadphones,
    };

    bool ensureAtlas(ID2D1DeviceContext* target);
    void releaseAtlas() noexcept;
    void chooseRoamingBehavior(std::uint64_t nowMs);
    std::uint32_t nextRandom();

    DockPetMode mode_ = DockPetMode::Hidden;
    Behavior behavior_ = Behavior::Stand;
    float laneLeft_ = 0.0f;
    float laneRight_ = 0.0f;
    float laneHeight_ = 48.0f;
    float x_ = 0.0f;
    float seatX_ = 0.0f;
    int direction_ = 1;
    int frame_ = 0;
    bool movingToSeat_ = false;
    std::uint64_t lastTickMs_ = 0;
    std::uint64_t modeStartedMs_ = 0;
    std::uint64_t behaviorStartedMs_ = 0;
    std::uint64_t behaviorUntilMs_ = 0;
    std::uint64_t nextBlinkMs_ = 0;
    std::uint64_t blinkUntilMs_ = 0;
    std::uint32_t randomState_ = 0x6D2B79F5u;

    ID2D1Bitmap* atlas_ = nullptr;
    bool atlasLoadAttempted_ = false;
};
