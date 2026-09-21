#pragma once

#include <cstdint>

struct ID2D1Bitmap;
struct ID2D1DeviceContext;
struct ID2D1SolidColorBrush;
struct IDWriteFactory;
struct IDWriteTextFormat;

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
    // 宿主按当前歌词行时长给出的摇摆半周期提示（毫秒）；0 表示无节奏信息，
    // 退回默认固定周期。内部做范围钳制，换行时只改速度、不打断当前相位。
    void setSwayBeatMs(std::uint32_t ms);
    // 切歌/恢复播放时的雀跃一跳；Hidden 状态下忽略。
    void hop(std::uint64_t nowMs);
    // 瞌睡气泡（Zzz）颜色跟随任务栏主题。
    void setLightTheme(bool light);
    void tick(std::uint64_t nowMs);

    bool animating() const noexcept;

    // D2D 设备重建时释放由该设备创建的图集位图与画刷。
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
    void releaseZzz() noexcept;
    void chooseRoamingBehavior(std::uint64_t nowMs);
    bool sleeping() const noexcept;
    void drawZzz(ID2D1DeviceContext* target, float headX, float headY);
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

    // 节拍摇摆：以帧为单位的相位累加器，行时长变化只改步进速度。
    std::uint32_t swayFrameMs_ = 0; // 0 表示尚未设置，tick 时取默认值
    float swayAccum_ = 0.0f;
    // 雀跃一跳的起点；0 表示当前没有跳跃。
    std::uint64_t hopStartMs_ = 0;
    bool lightTheme_ = false;

    ID2D1Bitmap* atlas_ = nullptr;
    // 右向行走帧的水平镜像，用作左向行走帧（左向原帧的亮爪不前后交替，
    // 迈步感弱；镜像右向帧后两个方向步态一致）。ensureAtlas 时生成。
    ID2D1Bitmap* walkMirror_ = nullptr;
    bool atlasLoadAttempted_ = false;

    // Zzz 文本：DWrite 工厂与文字格式与设备无关，一直保留到析构；
    // 画刷是设备资源，随 discardDeviceResources 一起释放。
    IDWriteFactory* dwrite_ = nullptr;
    IDWriteTextFormat* zzzFormats_[3] = {nullptr, nullptr, nullptr};
    ID2D1SolidColorBrush* zzzBrush_ = nullptr;
};
