#include "taskbar_host.h"
#include "ui/taskbar/vinyl_animation.h"
#include "ui/taskbar/lyric_transition.h"
#include "ui/taskbar/lyric_typesetter.h"
#include "ui/taskbar/karaoke_tracker.h"
#include "ui/taskbar/text_scroll.h"
#include "ui/taskbar/lyric_text_renderer.h"
#include "ui/taskbar/interaction.h"
#include "ui/taskbar/lyric_effects_renderer.h"
#include "ui/taskbar/appbar_registration.h"
#include "ui/taskbar/placement.h"
#include "ui/taskbar/placement_probe.h"
using taskbar_detail::WindowPlacement;
#include "ui/taskbar/spectrum_renderer.h"
#include "ui/taskbar/animation_clock.h"
using taskbar_detail::kImmersiveSpectrumBarCount;
using taskbar_detail::kImmersiveSpectrumWideBarCount;
using taskbar_detail::kImmersiveSpectrumWideZoneThreshold;
using taskbar_detail::kSpectrumBarRadius;
using taskbar_detail::kSpectrumBarW;
using taskbar_detail::kSpectrumBottomPadding;
using taskbar_detail::kSpectrumGap;
using taskbar_detail::monotonicNowMs;
#include "ui/taskbar/render_state.h"
#include "ui/taskbar/platform.h"
#include "ui/taskbar/window_attachment.h"
#include "ui/taskbar/layout.h"

using taskbar_detail::findTrafficMonitorWnd;
using taskbar_detail::InvalidationSnapshot;
using taskbar_detail::isTaskbarCenterAlign;
using taskbar_detail::isValidTaskbarEdge;
using taskbar_detail::isVerticalTaskbarEdge;
using taskbar_detail::kCoverPadding;
using taskbar_detail::kLeftRatio;
using taskbar_detail::kSongInfoLyricGap;
using taskbar_detail::kTextPadding;
using taskbar_detail::kVerticalControlsGap;
using taskbar_detail::kVerticalCoverGap;
using taskbar_detail::kVerticalLyricGap;
using taskbar_detail::kVerticalRailPadding;
using taskbar_detail::LayoutMetrics;
using taskbar_detail::queryKugouTaskbarWindows;
using taskbar_detail::queryTaskbarButtonsUia;
using taskbar_detail::queryTaskbarEdge;
using taskbar_detail::RenderInvalidation;
using taskbar_detail::RenderInvalidationMask;
using taskbar_detail::RenderState;
using taskbar_detail::taskbarCrossPixels;
using taskbar_detail::toMask;
using taskbar_detail::VerticalLayout;
#include "logging/runtime_logger.h"
#include "ui/app_icon.h"
#include "fluent_theme.h"
#include "dock_pet.h"
#include "lyric_renderer.h"
#include "lyric_star.h"
#include "media_control_icons.h"
#include "media_popup.h"
#include "ui/taskbar/resource_panel.h"
#include "ui/taskbar/cover_image.h"
#include "dock_backdrop.h"
#include "platform_icon.h"
#include "settings_icons.h"
#include "volume_popup.h"

#include <d2d1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <objbase.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId = 2;
constexpr UINT_PTR kTaskbarAttachTimerId = 3;
constexpr UINT kTimerMs = 16;       // 活动帧最大间隔：60Hz 基准
constexpr UINT kTimerMinMs = 8;     // 活动帧最小间隔：高刷封顶 ~125Hz，避免过度唤醒
constexpr UINT kTimerPausedMs = 33; // 暂停时 ~30fps：超长文本继续滚动，任务栏合成开销减半
constexpr UINT kTaskbarAttachRetryMs = 250;
constexpr UINT_PTR kPlacementTimerId = 4;
// 沉浸模式让出任务栏：悬停确认（单次）与显隐过渡/离开检测（轮询）。
constexpr UINT_PTR kImmersiveYieldTimerId = 5;
constexpr UINT_PTR kImmersivePresenceTimerId = 6;
constexpr UINT kImmersiveYieldDelayMs = 100;   // 悬停非控件区多久后让出任务栏
constexpr UINT kImmersiveHideMs = TaskbarPresence::kHideMs; // 淡出时长
constexpr UINT kImmersiveShowMs = TaskbarPresence::kShowMs; // 淡入时长
constexpr UINT kImmersivePresencePollMs = 20;
constexpr UINT kAppBarCallbackMessage = WM_APP + 40;
constexpr UINT kPlacementTimerMs = 500; // 隐藏时只拾取避让结果，不运行完整渲染帧
constexpr UINT kProbeIntervalMs = 3000;
constexpr UINT kNoSpaceProbeIntervalMs = 1000;
constexpr int kSlowTickInterval = 15; // 慢速分支（任务栏位置跟踪等）每 15 帧一次，约 250ms
constexpr wchar_t kWndClassName[] = L"QQMusicLyricTaskbar";

constexpr float kMinWidthDip = 160.0f;
constexpr float kMaxWidthDip = 280.0f;
constexpr float kCompressedMinWidthRatio = 0.5f;

constexpr wchar_t kDragPreviewText[] = L"松手固定到这里";
constexpr float kLyricDragThresholdDip = 6.0f;
// 滚动文本左缘（滚出侧）的渐隐宽度
constexpr float kLyricEdgeFadeDip = 18.0f;
// 歌曲信息与歌词之间的分隔间距
constexpr float kSongInfoDividerWidth = 1.0f;
constexpr float kSongInfoDividerInset = 5.0f;
constexpr float kCornerRadius = 8.0f;
constexpr float kVerticalMinLengthDip = 220.0f;
constexpr float kVerticalMaxLengthDip = 320.0f;

constexpr float kLyricTransitionMs = 280.0f;         // 相邻歌词上下切换时长
constexpr float kSceneTransitionMs = 240.0f;         // 每日一言与歌词内容块上下翻页时长
constexpr float kSongTransitionMs = 220.0f;          // 切歌时新内容滑入时长
constexpr float kSongTransitionTravelDip = 24.0f;    // 切歌时新内容的水平入场距离
constexpr float kLyricPreviewGap = 3.0f;             // 普通双行模式的核心行与下一行间距
constexpr float kLyricPreviewOpacity = 0.90f;        // 下一行预览透明度
constexpr int kImmersiveControlPrevious = 0;
constexpr int kImmersiveControlPlayPause = 1;
constexpr int kImmersiveControlNext = 2;
constexpr int kImmersiveControlVolume = 3;
constexpr int kImmersiveControlExit = 4;
// Dock 的快捷应用入口使用独立图标和命中项，沉浸模式不显示。
constexpr int kDockControlQuickApps = 5;
constexpr int kImmersiveControlMenu = 6;
constexpr int kImmersiveControlCount = 6; // 沉浸模式显示 6 个，索引 5 保留给 Dock 入口
constexpr int kExpandedControlCount = 7;
constexpr int kAppBarControlCount = 7;
constexpr float kAppBarHeightDip = 48.0f;
constexpr float kImmersiveControlRadiusFactor = 0.24f;
constexpr float kImmersiveControlMinRadius = 8.0f;
constexpr float kImmersiveControlMaxRadius = 12.0f;
constexpr float kImmersiveControlPitchFactor = 3.0f;
constexpr float kLyricMainFontScale = 1.18f;
constexpr float kLyricPreviewFontScale = 0.86f;
constexpr float kLyricPreviewScale = kLyricPreviewFontScale / kLyricMainFontScale;
constexpr float kMinFont = 9.0f;
constexpr float kMaxFont = 18.0f;
constexpr float kBaseFontSize = 12.0f;
constexpr float kCompactTaskbarHeightDip = 36.0f;


constexpr float kImmersiveSpectrumClockGap = 10.0f;
constexpr float kDockResourceZoneGap = 10.0f;
constexpr float kResourceFontSize = 13.5f;
constexpr float kDockResourceRightPadding = 3.0f;
constexpr float kDockPetSeatW = 42.0f;
constexpr float kPetLaneMinWidthDip = 44.0f;
constexpr float kVinylRotationDegPerSecond = 30.0f; // 黑胶唱片转速：12 秒一圈，保持视觉克制
constexpr float kVinylArmParkedAngle = -18.0f;
constexpr ULONGLONG kVinylArmDurationMs = 240;
constexpr float kVinylHaloWidth = 0.5f;
constexpr float kVinylOuterRatio = 0.40f; // 唱片直径约为底座边长的 80%，四边各留约 10%
constexpr float kVinylInnerRatio = 0.25f; // 圆形专辑封面半径 / 封面槽边长

// 读取注册表 DWORD，失败返回默认值

bool sameLyricLine(const LyricLine& a, const LyricLine& b) {
    if (a.ms != b.ms || a.text != b.text || a.translation != b.translation ||
        a.romanization != b.romanization || a.chars.size() != b.chars.size())
        return false;
    for (size_t i = 0; i < a.chars.size(); ++i) {
        const LyricChar& ac = a.chars[i];
        const LyricChar& bc = b.chars[i];
        if (ac.startMs != bc.startMs || ac.endMs != bc.endMs || ac.text != bc.text)
            return false;
    }
    return true;
}

bool sameLyrics(const std::vector<LyricLine>& a, const std::vector<LyricLine>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!sameLyricLine(a[i], b[i]))
            return false;
    }
    return true;
}

// 这些位表示“需要重新处理什么”，不再分别由多个布尔量表达。它们不是
// 生命周期阶段：阶段使用下面 RenderState 中的枚举表示，失效位只描述待处理工作。
} // namespace

struct TaskbarHost::Impl {

    mutable SpectrumRenderer spectrum_;
    SpectrumContext spectrumContext(ID2D1DeviceContext* target = nullptr) const {
        SpectrumContext context;
        context.target = target;
        context.visible = spectrumVisible_;
        context.animations = clientAnimations_;
        context.scene = scene_;
        context.nowMs = frameNowMs_;
        context.style = spectrumStyle_;
        context.customColor = spectrumCustomColor_;
        context.color = spectrumColor_;
        context.followAlbum = spectrumFollowAlbum_;
        context.albumColor = spectrumAlbumColor_;
        context.hasAlbumColor = spectrumAlbumColorAvailable_;
        context.gradient = spectrumGradient_;
        context.effects = spectrumEffects_;
        context.background = spectrumBackground_;
        context.opacity = spectrumOpacityPct_;
        context.playedColor = lyricColor_;
        context.compactHeight = compactTaskbarHeight_;
        context.playing = media.playing;
        context.minimal = isMinimalMode();
        context.stopped = isStoppedMode();
        context.horizontalImmersive = horizontalImmersiveMode();
        return context;
    }
    mutable LyricEffectsRenderer lyricEffectsRenderer_;
    LyricEffectsContext lyricEffectsContext(ID2D1DeviceContext* target = nullptr) const {
        LyricEffectsContext context;
        context.target = target;
        context.lines = &lines;
        context.effects = lyricEffects_;
        context.animations = clientAnimations_;
        context.scene = scene_;
        context.currentLine = lyricEffectLayoutLine_;
        context.outgoingLine = outgoingLyricEffectLine_;
        context.currentLayout = typesetter_.resources().lyricLayout_.Get();
        context.outgoingLayout = typesetter_.resources().outgoingLyricLayout_.Get();
        context.width = lyricWidth_;
        context.outgoingWidth = outgoingLyricWidth_;
        context.positionMs = positionMs_;
        context.outgoingDoubleLine = transition_.outgoingDoubleLine();
        context.compactHeight = compactTaskbarHeight_;
        context.doubleLineEnabled = doubleLineLyricsEnabled_;
        context.color = lyricColor_;
        context.glow = lyricGlow_;
        context.outline = lyricOutline_;
        context.fontSize = fontSize_;
        context.outlineBrush = brushLyricOutline_.Get();
        context.playing = media.playing;
        context.minimal = isMinimalMode();
        context.stopped = isStoppedMode();
        context.vertical = isVerticalTaskbar();
        context.currentDoubleLine = useDoubleLineLyrics();
        return context;
    }
    LyricTypesetter typesetter_;
    LyricTransition transition_;
    VinylAnimation vinyl_;
    RenderState renderState_;
    std::optional<TaskbarAppearance> appearance_;
    bool applyingAppearance_ = false;
    bool appearancePositionPending_ = false;
    bool appearanceBrushesPending_ = false;
    bool appearanceFormatsPending_ = false;
    bool appearancePaintPending_ = false;

    void applyAppearance(const TaskbarAppearance& value) {
        if (appearance_ && *appearance_ == value)
            return;
        applyingAppearance_ = true;
        if (!appearance_ || appearance_->secondaryLyricMode != value.secondaryLyricMode)
            setSecondaryLyricMode(value.secondaryLyricMode.translation,
                                  value.secondaryLyricMode.romanization);
        if (!appearance_ || appearance_->doubleLineLyrics != value.doubleLineLyrics)
            setDoubleLineLyrics(value.doubleLineLyrics);
        if (!appearance_ || appearance_->lyricEffects != value.lyricEffects)
            setLyricEffects(value.lyricEffects);
        if (!appearance_ || appearance_->lyricAlignment != value.lyricAlignment)
            setLyricAlignment(value.lyricAlignment);
        if (!appearance_ || appearance_->idleQuoteAlignment != value.idleQuoteAlignment)
            setIdleQuoteAlignment(value.idleQuoteAlignment);
        if (!appearance_ || appearance_->songInfoVisible != value.songInfoVisible)
            setSongInfoVisible(value.songInfoVisible);
        if (!appearance_ || appearance_->controlsOnHover != value.controlsOnHover)
            setControlsOnHover(value.controlsOnHover);
        if (!appearance_ || appearance_->hoverControlStyle != value.hoverControlStyle)
            setHoverControlStyle(value.hoverControlStyle);
        if (!appearance_ || appearance_->floatingCardTrigger != value.floatingCardTrigger)
            setFloatingCardTrigger(value.floatingCardTrigger);
        if (!appearance_ || appearance_->floatingCardBackground != value.floatingCardBackground)
            setFloatingCardBackground(value.floatingCardBackground);
        if (!appearance_ ||
            appearance_->floatingCardBackgroundColor != value.floatingCardBackgroundColor)
            setFloatingCardBackgroundColor(value.floatingCardBackgroundColor.color,
                                           value.floatingCardBackgroundColor.customized);
        if (!appearance_ || appearance_->idleQuoteBackground != value.idleQuoteBackground)
            setIdleQuoteBackground(value.idleQuoteBackground);
        if (!appearance_ || appearance_->idleQuoteBackgroundScope != value.idleQuoteBackgroundScope)
            setIdleQuoteBackgroundScope(value.idleQuoteBackgroundScope);
        if (!appearance_ || appearance_->floatingCardFollowAlbum != value.floatingCardFollowAlbum)
            setFloatingCardFollowAlbum(value.floatingCardFollowAlbum);
        if (!appearance_ ||
            appearance_->floatingCardAutoTextContrast != value.floatingCardAutoTextContrast)
            setFloatingCardAutoTextContrast(value.floatingCardAutoTextContrast);
        if (!appearance_ || appearance_->albumCoverEffect != value.albumCoverEffect)
            setAlbumCoverEffect(value.albumCoverEffect);
        if (!appearance_ || appearance_->spectrumStyle != value.spectrumStyle)
            setSpectrumStyle(value.spectrumStyle);
        if (!appearance_ || appearance_->spectrumColor != value.spectrumColor)
            setSpectrumColor(value.spectrumColor.color, value.spectrumColor.customized,
                             value.spectrumColor.followAlbum);
        if (!appearance_ || appearance_->spectrumAlbumColor != value.spectrumAlbumColor)
            setSpectrumAlbumColor(value.spectrumAlbumColor.color,
                                  value.spectrumAlbumColor.available);
        if (!appearance_ || appearance_->spectrumGradient != value.spectrumGradient)
            setSpectrumGradient(value.spectrumGradient);
        if (!appearance_ || appearance_->spectrumEffects != value.spectrumEffects)
            setSpectrumEffects(value.spectrumEffects);
        if (!appearance_ || appearance_->spectrumBackground != value.spectrumBackground)
            setSpectrumBackground(value.spectrumBackground);
        if (!appearance_ || appearance_->spectrumOpacity != value.spectrumOpacity)
            setSpectrumOpacity(value.spectrumOpacity);
        if (!appearance_ || appearance_->progressBackground != value.progressBackground)
            setProgressBackground(value.progressBackground);
        if (!appearance_ ||
            appearance_->progressBackgroundOpacity != value.progressBackgroundOpacity)
            setProgressBackgroundOpacity(value.progressBackgroundOpacity);
        if (!appearance_ || appearance_->background != value.background)
            setBackground(value.background);
        if (!appearance_ || appearance_->coverBackgroundOpacity != value.coverBackgroundOpacity)
            setCoverBackgroundOpacity(value.coverBackgroundOpacity);
        if (!appearance_ ||
            appearance_->immersiveBackgroundAdjustment != value.immersiveBackgroundAdjustment)
            setImmersiveBackgroundAdjustment(value.immersiveBackgroundAdjustment);
        if (!appearance_ || appearance_->dockBackgroundAdjustment != value.dockBackgroundAdjustment)
            setDockBackgroundAdjustment(value.dockBackgroundAdjustment);
        if (!appearance_ || appearance_->immersiveBackgroundBlur != value.immersiveBackgroundBlur)
            setImmersiveBackgroundBlur(value.immersiveBackgroundBlur);
        if (!appearance_ || appearance_->dockBackgroundBlur != value.dockBackgroundBlur)
            setDockBackgroundBlur(value.dockBackgroundBlur);
        if (!appearance_ || appearance_->dockResourceVisibility != value.dockResourceVisibility)
            setDockResourceVisibility(value.dockResourceVisibility);
        if (!appearance_ || appearance_->albumCoverVisible != value.albumCoverVisible)
            setAlbumCoverVisible(value.albumCoverVisible);
        if (!appearance_ || appearance_->platformIconVisible != value.platformIconVisible)
            setPlatformIconVisible(value.platformIconVisible);
        if (!appearance_ || appearance_->contextMenuEnabled != value.contextMenuEnabled)
            contextMenuEnabled_ = value.contextMenuEnabled;
        if (!appearance_ || appearance_->fontColors != value.fontColors)
            setFontColors(value.fontColors.played, value.fontColors.unplayed,
                          value.fontColors.unplayedAlphaPct);
        if (!appearance_ || appearance_->fontGlow != value.fontGlow)
            setFontGlow(value.fontGlow);
        if (!appearance_ || appearance_->fontOutline != value.fontOutline)
            setFontOutline(value.fontOutline);
        if (!appearance_ || appearance_->fontGlowColors != value.fontGlowColors)
            setFontGlowColors(value.fontGlowColors.glow, value.fontGlowColors.outline);
        appearance_ = value;
        applyingAppearance_ = false;
        const bool formats = std::exchange(appearanceFormatsPending_, false);
        const bool brushes = std::exchange(appearanceBrushesPending_, false);
        const bool position = std::exchange(appearancePositionPending_, false);
        const bool paint = std::exchange(appearancePaintPending_, false);
        if (formats)
            recreateFormats();
        if (brushes)
            createLyricBrushes();
        if (position)
            adjustPosition();
        if (paint)
            flushRenderRequest();
    }

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool timerRunning_ = false;
    UINT timerMs_ = 0; // 当前定时器实际间隔（活动/暂停档位切换用）
    bool placementTimerRunning_ = false;
    UINT displayRefreshHz_ = 60;
    bool allowOverlap_ = false;
    bool probeReady_ = false;
    TaskbarPlacementStatus placementStatus_ = TaskbarPlacementStatus::Unavailable;
    std::function<void(TaskbarPlacementStatus)> onPlacementStatusChanged_;

    // 任务栏句柄与子部件
    HWND taskbar_ = nullptr;
    HMONITOR targetMonitor_ = nullptr;
    std::wstring targetMonitorDeviceName_;
    TaskbarWindowAttachment attachment_;
    AppBarRegistration appBar_;
    AppBarEdge appBarEdge_ = AppBarEdge::Top;
    HWND notify_ = nullptr;
    HWND start_ = nullptr;
    RECT rcTaskbar_{};        // 任务栏屏幕坐标（缓存）
    RECT rcNotify_{};         // 通知区屏幕坐标（缓存）
    RECT rcStart_{};          // 开始按钮屏幕坐标（缓存，找不到时为空）
    RECT rcTrafficMonitor_{}; // TrafficMonitor 屏幕坐标（缓存，未运行时为 empty）
    // 任务栏 XAML 按钮（开始/搜索/任务视图/小组件/应用图标）包围矩形缓存（屏幕坐标）。
    // 以上探测数据由工作线程产出、UI 线程拾取（由 PlacementProbe 交付）
    std::vector<RECT> uiaButtons_;
    // 酷狗进程中与任务栏相交的可见窗口（屏幕坐标）。
    std::vector<RECT> kugouTaskbarWindows_;
    UINT dpi_ = 96;
    bool centerAlign_ = true;
    UINT taskbarEdge_ = ABE_BOTTOM;
    bool lightTheme_ = false;
    int positionMode_ = 0; // 0 = 通知区域前；1 = 任务栏起始端

    // 工作线程只持有独立状态，不持有 Impl；窗口销毁只发停止信号，不能 join。
    // 最后一个持有者负责回收尚未拾取的结果，允许跨进程调用晚于窗口销毁返回。
    PlacementProbe placementProbe_;

    // 歌词状态
    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    std::function<void()> onStatusTextCycleCompleted_;
    int currentLine = -1;
    int64_t positionMs_ = 0; // 播放进度（每帧更新），驱动逐字高亮
    DisplayScene scene_ = DisplayScene::NoPlayback;
    std::wstring trackKey_;
    uint64_t requestGeneration_ = 0;
    uint64_t frameRevision_ = 0;

    // 字体：默认字体族与软件普通窗口一致（fluent::uiFontFamily）
    float fontSize_ = kBaseFontSize;
    float compactTaskbarHeight_ = 0.0f;
    std::wstring fontFamily_ = fluent::uiFontFamily();
    LyricFontStyle fontStyle_ = LyricFontStyle::Normal;

    // 媒体信息
    OverlayMediaInfo media;
    IdlePresentation idle;
    CoverImage cover_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> platformIconBmp;

    // 交互
    std::function<void()> tick;
    std::function<void(MediaControl)> onControl;
    std::function<void()> onImmersiveExit;
    std::function<void(POINT)> onContextMenu;
    std::function<void(POINT)> onImmersiveMenu;
    std::function<void(POINT)> onDockQuickApps;
    std::function<void(int)> onPositionModeChanged;
    bool mouseOver_ = false;
    bool trackingLeave_ = false;
    int immersiveControlHover_ = -1;
    int inlineControlHover_ = -1;
    int immersiveControlPressed_ = -1;
    bool controlsOnHover_ = true;
    bool contextMenuEnabled_ = true;
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    MediaPopupTrigger floatingCardTrigger_ = MediaPopupTrigger::Hover;
    MediaPopup mediaPopup;
    bool mediaPopupEnabled_ = false;
    bool quitting = false;

    // 沉浸模式默认覆盖任务栏；鼠标悬停在播放控件以外的区域时淡出并隐藏窗口，
    // 把任务栏交还给用户，鼠标离开任务栏后再淡入恢复。
    using ImmersivePresence = TaskbarPresence::Phase;
    TaskbarPresence presence_;
    TaskbarDragGesture drag_;
    bool immersiveYieldArmed_ = false;
    bool immersivePresenceTimerRunning_ = false;

    // 应用音量（内嵌控件音量图标 + 悬停滑块浮窗）
    AppVolumeState appVolume_;
    VolumePopup volumePopup_;
    std::function<void(int)> onAppVolume;
    bool volumeHover_ = false;

    KaraokeTracker karaoke_;
    TextScroll scroll_;
    LyricTextRenderer lyricTextRenderer_;

    // 滚动字幕（歌名、歌手、歌词独立滚动）
    float titleWidth_ = 0.0f;
    float titleHeight_ = 0.0f;
    float artistWidth_ = 0.0f;
    float artistHeight_ = 0.0f;
    float lyricWidth_ = 0.0f;
    float lyricHeight_ = 0.0f;
    float secondaryWidth_ = 0.0f;
    float secondaryHeight_ = 0.0f;
    std::wstring lastTitle_;
    std::wstring lastArtist_;
    std::wstring lastLyric_;
    std::wstring lastSecondary_;
    std::wstring clockTimeText_;
    std::wstring clockDateText_;
    uint64_t clockMinuteKey_ = 0;
    ResourcePanel resourcePanel_;
    int slowTick_ = 0; // 慢速分支计数器

    // 渲染

    DCompRenderer renderer;
    uint64_t deviceGeneration_ = 0;
    // 沉浸模式切歌时，歌曲内容会先绘制到独立的 DirectComposition 表面。
    // 绘制辅助函数统一从这里取目标，普通帧保持使用交换链上下文。
    ID2D1DeviceContext* drawTargetOverride_ = nullptr;
    int lyricEffectLayoutLine_ = -1;
    int outgoingLyricEffectLine_ = -1;
    uint32_t lyricEffects_ = 0;
    bool lyricFxWasAnimating_ = false;

    // 侧边任务栏使用独立的竖排布局；中文/符号逐字排列，连续英文片段整组旋转。
    // 横向布局仍保留给上下任务栏和逐字时间轴计算，避免两种排版互相改变测量结果。
    // 行切换动画保留上一帧的布局，避免新行直接替换导致文字瞬移。
    bool lyricLayoutDoubleLine_ = false;
    float outgoingLyricWidth_ = 0.0f;
    float outgoingLyricHeight_ = 0.0f;
    float verticalLyricWidth_ = 0.0f;
    float verticalLyricHeight_ = 0.0f;
    bool verticalLyricRotated_ = false;
    float outgoingVerticalLyricWidth_ = 0.0f;
    float outgoingVerticalLyricHeight_ = 0.0f;
    bool outgoingVerticalLyricRotated_ = false;
    float outgoingLyricBlockHeight_ = 0.0f;
    float outgoingLyricScrollOffset_ = 0.0f;
    float outgoingSecondaryWidth_ = 0.0f;
    float outgoingSecondaryHeight_ = 0.0f;
    float outgoingSecondaryScrollOffset_ = 0.0f;
    float outgoingNextLyricWidth_ = 0.0f;
    float outgoingNextLyricHeight_ = 0.0f;
    float nextLyricWidth_ = 0.0f;
    float nextLyricHeight_ = 0.0f;

    // 行过渡目标：currentLine 是逻辑当前行；transition_.target() 是本次动画要进入的行，
    // transition_.pending() 是动画期间收到的最新目标（latest-frame-wins，只覆盖不累积），
    // 动画结束收尾时统一消费，不让旧的结束逻辑覆盖新行。

    ULONGLONG frameNowMs_ = 0;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHover_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDim_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBtn_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBtnDisabled_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyric_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricDim_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricGlow_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricOutline_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushVinylBase_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushVinylGroove_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brushVinylSurface_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTurntable_;
    COLORREF lyricColor_ = RGB(49, 194, 124);         // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124); // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                  // 逐字未播放不透明度（%）
    COLORREF lyricGlowColor_ = RGB(49, 194, 124);     // 光晕颜色
    COLORREF lyricOutlineColor_ = RGB(0, 0, 0);       // 描边颜色
    bool lyricGlow_ = false;                          // 光晕开关
    bool lyricOutline_ = false;                       // 描边开关
    bool translationEnabled_ = true;
    bool romanizationEnabled_ = false;
    bool doubleLineLyricsEnabled_ = false;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    LyricAlignment idleQuoteAlignment_ = LyricAlignment::Left;
    IdleQuoteBackground idleQuoteBackground_ = IdleQuoteBackground::None;
    IdleQuoteBackgroundScope idleQuoteBackgroundScope_ = IdleQuoteBackgroundScope::DailyQuote;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;
    bool clientAnimations_ = true;
    // 渲染模式：0 正常；1 低渲染（播放中也固定 ~30fps）；2 完全停止（窗口隐藏、
    // 帧定时器停止、GPU 设备释放，数据状态保留在内存）；3 极简（保留歌词刷新率，
    // 仅关闭附加视觉、媒体卡片和切歌弹窗）
    // 频谱：基色可跟随已播放色、专辑主题色或自定义色（createLyricBrushes），
    // bands 由 UI 线程每帧写入
    SpectrumStyle spectrumStyle_ = SpectrumStyle::Default;
    COLORREF spectrumColor_ = RGB(49, 194, 124);
    bool spectrumCustomColor_ = false;
    bool spectrumFollowAlbum_ = false;
    COLORREF spectrumAlbumColor_ = RGB(49, 194, 124);
    bool spectrumAlbumColorAvailable_ = false;
    bool spectrumGradient_ = false;
    uint32_t spectrumEffects_ = 0;
    bool spectrumBackground_ = false;
    int spectrumOpacityPct_ = 40;
    // 播放进度背景：与背景波浪互斥，颜色取专辑主色（无封面时回退系统强调色）
    bool progressBackground_ = false;
    int progressBackgroundOpacityPct_ = 25;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushProgressBg_;
    // 任务栏歌词背景：封面模糊（GaussianBlur → Scale 效果链 + 主题遮罩）或纯色；
    // 画在最底层，可与进度背景、背景波浪叠加
    TaskbarBackground background_ = TaskbarBackground::None;
    int coverBackgroundOpacityPct_ = 60;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBackground_;
    // 沉浸模式使用完整任务栏客户区；AppBar 使用独立的 Shell 工作区协议。
    TaskbarViewMode viewMode_ = TaskbarViewMode::Embedded;
    int immersiveBackgroundBlurPct_ = 88;
    int dockBackgroundBlurPct_ = 88;
    int immersiveBackgroundAdjustment_ = 0;
    int dockBackgroundAdjustment_ = 0;
    bool backgroundBlurSupported_ = true;
    DockBackdrop dockBackdrop_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIdleWarm_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIdleCool_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIdleAccent_;
    DockPet dockPet_;
    // 伴听宠物已见到的曲目（title|artist），用于检测"播放中切歌"并触发雀跃。
    std::wstring dockPetTrack_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dragPreviewStroke_;
    bool spectrumVisible_ = false;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> coverClip_;
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> vinylCoverClip_;
    Microsoft::WRL::ComPtr<ID2D1Layer> coverLayer_;
    // 滚动歌词左缘渐隐：固定两停止点，渐变轴每帧按滚动偏移更新
    // 右缘渐隐带：完全位于歌词区右缘之外的留白（到频谱/窗口边缘的间隙），
    // 不遮挡可读区域内的文字
    // 歌曲信息与歌词之间的悬浮分隔线，使用上下渐隐的主题色画刷
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> songInfoDividerBrush_;
    media_control::Geometry controlGeometry;
    struct SceneResizeAnimation {
        WindowPlacement from{};
        WindowPlacement to{};
        WindowPlacement lastApplied{};
        ULONGLONG startMs = 0;
    };
    std::optional<SceneResizeAnimation> sceneResize_;
    struct SongContentTransition {
        ULONGLONG startMs = 0;
        bool compositorLayer = false;
        bool pendingConsumed = false;
    };
    std::optional<SongContentTransition> songContentTransition_;
    int lastPxW_ = 0;
    int lastPxH_ = 0;
    int lastLogicalPxW_ = 0;
    int lastLogicalPxH_ = 0;
    // 滚动与逐字平滑分别报告是否仍在变化，静止且无脏状态时跳过整帧重绘。
    // 光晕/描边离屏缓存：glow+outline+本体三层只合成一次，滚动/淡变时做 DrawImage 平移，
    // 避免每帧对主歌词重复 16+ 次 DrawTextLayout。双行转场同时绘制旧/新布局，保留两个
    // 缓存槽，避免两套布局在同一帧互相驱逐。布局、画刷或效果组合变化时由效果缓存的失效代次控制。

    bool isWindowVisible() const {
        return renderState_.windowPhase() == RenderState::WindowPhase::Visible;
    }

    bool isSessionVisible() const { return renderState_.sessionVisible(); }

    bool isRenderMode(RenderMode mode) const { return renderState_.mode() == mode; }

    bool isStoppedMode() const { return isRenderMode(RenderMode::Stopped); }

    bool isLyricTransitionPending() const {
        return transition_.phase() == LyricTransition::Phase::Pending;
    }

    bool isLyricTransitionActive() const {
        return transition_.phase() == LyricTransition::Phase::Running;
    }

    bool isLyricTransitionInProgress() const {
        return transition_.phase() != LyricTransition::Phase::Idle;
    }

    void setLyricTransitionPending() { transition_.setPhase(LyricTransition::Phase::Pending); }

    void setLyricTransitionActive() { transition_.setPhase(LyricTransition::Phase::Running); }

    void clearLyricTransitionPhase() { transition_.setPhase(LyricTransition::Phase::Idle); }

    bool isLyricDCompActive() const {
        return renderState_.dcompTransitionPhase() == RenderState::DCompTransitionPhase::Active;
    }

    bool isLyricDCompEndRequested() const {
        return renderState_.dcompTransitionPhase() ==
               RenderState::DCompTransitionPhase::EndRequested;
    }

    void requestLyricDCompEnd() {
        if (isLyricDCompActive())
            renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::EndRequested);
    }

    void clearLyricDCompState() {
        renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::Inactive);
    }

    bool isSongTransitionPending() const { return renderState_.songTransitionPending(); }

    void setSongTransitionPending(bool pending) { renderState_.setSongTransitionPending(pending); }

    bool usesSongContentTransition() const {
        return isExpandedView() || (!isVerticalTaskbar() && embeddedResourceVisible());
    }

    bool immersiveSongContentLayerActive() const {
        return songContentTransition_ && songContentTransition_->compositorLayer;
    }

    bool isSceneResizeActive() const { return sceneResize_.has_value(); }

    void requestInvalidation(RenderInvalidation value) { renderState_.requestInvalidation(value); }

    void requestInvalidation(RenderInvalidationMask mask) {
        renderState_.requestInvalidation(mask);
    }

    bool isInvalidated(RenderInvalidation value) const { return renderState_.isInvalidated(value); }

    void requestFrame() { requestInvalidation(RenderInvalidation::Paint); }

    void flushRenderRequest() {
        if (applyingAppearance_) {
            appearancePaintPending_ = true;
            return;
        }
        if (isWindowVisible() && hwnd)
            render();
    }

    void requestFrameAndFlush() {
        requestFrame();
        flushRenderRequest();
    }

    bool isExpandedView() const {
        return viewMode_ == TaskbarViewMode::Immersive || viewMode_ == TaskbarViewMode::AppBar;
    }

    bool isAppBarView() const { return viewMode_ == TaskbarViewMode::AppBar; }

    bool resourceMonitoringEnabled() const {
        return isAppBarView() || (viewMode_ == TaskbarViewMode::Embedded &&
                                  resourcePanel_.visibility().embeddedEnabled);
    }

    bool embeddedResourceVisible() const {
        return viewMode_ == TaskbarViewMode::Embedded &&
               resourcePanel_.visibility().embeddedEnabled && !isVerticalTaskbar();
    }

    bool embeddedResourceOnly() const {
        return embeddedResourceVisible() && scene_ == DisplayScene::Idle && statusText.empty();
    }

    float resourceZoneWidth() const { return resourcePanel_.width(); }

    float embeddedResourceExtra() const {
        if (embeddedResourceOnly())
            return resourceZoneWidth() + kDockResourceRightPadding;
        return embeddedResourceVisible() ? resourceZoneWidth() + kDockResourceZoneGap : 0.0f;
    }

    int expandedControlCount() const {
        return isAppBarView() ? kAppBarControlCount : kImmersiveControlCount;
    }

    bool expandedControlVisible(int index) const {
        if (!isAppBarView())
            return index >= 0 && index < kExpandedControlCount && index != kDockControlQuickApps;
        return (index >= kImmersiveControlPrevious && index <= kDockControlQuickApps) ||
               index == kImmersiveControlMenu;
    }

    bool shouldShowWindow() const {
        if (!hwnd || isStoppedMode() || !isSessionVisible() || renderState_.visibilitySuppressed())
            return false;
        if (presence_.phase() == ImmersivePresence::Hidden)
            return false;
        if (!allowOverlap_ && (placementStatus_ == TaskbarPlacementStatus::NoSpace ||
                               placementStatus_ == TaskbarPlacementStatus::Unavailable))
            return false;
        return true;
    }

    bool reconcileWindowVisibility(bool prewarmBeforeShow = false) {
        const bool wantVisible = shouldShowWindow();
        const bool nativeVisible = hwnd && IsWindowVisible(hwnd);
        const bool appBarRegistrationMismatch =
            isAppBarView() && (wantVisible != appBar_.registered());
        if (wantVisible == isWindowVisible() && wantVisible == nativeVisible &&
            !appBarRegistrationMismatch)
            return false;

        renderState_.setWindowPhase(wantVisible ? RenderState::WindowPhase::Visible
                                                : RenderState::WindowPhase::Hidden);
        if (wantVisible) {
            if (isAppBarView() && !registerAppBar()) {
                renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
                ShowWindow(hwnd, SW_HIDE);
                setPlacementStatus(TaskbarPlacementStatus::Unavailable);
                return false;
            }
            // 完全停止模式会释放 DComp 设备链。先在原生窗口仍隐藏时完成首帧，
            // 让设备/交换链/字体和位图资源的重建成本不落到用户可见的第一帧。
            if (prewarmBeforeShow)
                requestFrameAndFlush();
            ShowWindow(hwnd, SW_SHOWNA);
            ensureImmersiveZOrder();
            startFrameTimer();
        } else {
            stopFrameTimer();
            clearImmersiveSongContentTransition();
            if (hwnd)
                ShowWindow(hwnd, SW_HIDE);
            if (isAppBarView())
                unregisterAppBar();
        }
        return true;
    }

    float scale() const { return static_cast<float>(dpi_) / 96.0f; }

    ID2D1DeviceContext* drawTarget() {
        return drawTargetOverride_ ? drawTargetOverride_ : renderer.renderTarget();
    }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    bool isVerticalTaskbar() const {
        return !isAppBarView() && isVerticalTaskbarEdge(taskbarEdge_);
    }

    void clientPixelSize(int& width, int& height) const {
        width = 0;
        height = 0;
        if (!hwnd)
            return;
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;
        }
    }

    void logicalClientPixelSize(int& width, int& height) const { clientPixelSize(width, height); }

    void clientPointToLogicalDip(float clientX, float clientY, float& xDip, float& yDip) const {
        xDip = clientX / scale();
        yDip = clientY / scale();
    }

    POINT logicalDipToClientPoint(float xDip, float yDip) const {
        return POINT{static_cast<LONG>(std::lround(xDip * scale())),
                     static_cast<LONG>(std::lround(yDip * scale()))};
    }

    bool updateMediaInfo(const OverlayMediaInfo& info) {
        bool thumbChanged = info.thumbnail != media.thumbnail;
        bool textChanged = info.title != media.title || info.artist != media.artist;
        bool controlsChanged = info.canPrev != media.canPrev ||
                               info.canPlayPause != media.canPlayPause ||
                               info.canNext != media.canNext;
        bool playingChanged = info.playing != media.playing;
        bool platformChanged = info.sourceAppUserModelId != media.sourceAppUserModelId;
        bool dominantColorChanged =
            info.hasDominantColor != media.hasDominantColor ||
            (info.hasDominantColor && info.dominantColor != media.dominantColor);
        bool durationChanged = info.durationMs != media.durationMs;
        media = info;
        if (thumbChanged)
            requestInvalidation(RenderInvalidation::Cover);
        if (platformChanged)
            requestInvalidation(RenderInvalidation::PlatformIcon);
        if (thumbChanged || textChanged)
            vinyl_.resetDisc();
        if (textChanged)
            requestInvalidation(RenderInvalidation::SongInfo);
        if (thumbChanged || textChanged || playingChanged)
            vinyl_.resetClock(monotonicNowMs());
        if (playingChanged)
            updateVinylRotation();
        return thumbChanged || textChanged || controlsChanged || playingChanged ||
               platformChanged || dominantColorChanged || durationChanged;
    }

    // ---------- 窗口创建与定位 ----------

    bool findTaskbar() {
        const auto displays = enumerateTaskbarDisplays();
        auto display = std::find_if(displays.begin(), displays.end(), [this](const auto& item) {
            return !targetMonitorDeviceName_.empty() && item.deviceName == targetMonitorDeviceName_;
        });
        if (display == displays.end())
            display = std::find_if(displays.begin(), displays.end(),
                                   [](const auto& item) { return item.primary; });
        if (display == displays.end() && !displays.empty())
            display = displays.begin();
        if (display == displays.end())
            return false;

        targetMonitor_ = display->monitor;
        taskbar_ = display->taskbarWindow;
        placementProbe_.setTaskbar(taskbar_); // 同步给探测工作线程
        if (!taskbar_ && !isAppBarView())
            return false;
        notify_ = taskbar_ ? FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
        start_ = taskbar_ ? FindWindowExW(taskbar_, nullptr, L"Start", nullptr) : nullptr;
        dpi_ = taskbar_ ? GetDpiForWindow(taskbar_) : GetDpiForSystem();
        if (dpi_ == 0)
            dpi_ = 96;
        centerAlign_ = isTaskbarCenterAlign();
        lightTheme_ = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        updateRects();
        if (taskbar_)
            taskbarEdge_ = queryTaskbarEdge(taskbar_, rcTaskbar_);
        else
            taskbarEdge_ = ABE_BOTTOM;
        return true;
    }

    void updateRects() {
        if (taskbar_)
            GetWindowRect(taskbar_, &rcTaskbar_);
        if (notify_)
            GetWindowRect(notify_, &rcNotify_);
        if (start_)
            GetWindowRect(start_, &rcStart_);
    }

    void scheduleTaskbarAttachRetry() {
        if (hwnd)
            SetTimer(hwnd, kTaskbarAttachTimerId, kTaskbarAttachRetryMs, nullptr);
    }

    void cancelTaskbarAttachRetry() {
        if (hwnd)
            KillTimer(hwnd, kTaskbarAttachTimerId);
    }

    bool attachToTaskbar(HWND window) { return attachment_.attach(window, taskbar_); }

    bool detachFromTaskbar() {
        cancelTaskbarAttachRetry();
        return attachment_.detach(hwnd);
    }

    HMONITOR appBarMonitor() const {
        if (targetMonitor_)
            return targetMonitor_;
        if (taskbar_ && IsWindow(taskbar_))
            return MonitorFromWindow(taskbar_, MONITOR_DEFAULTTOPRIMARY);
        if (hwnd)
            return MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    void updateAppBarZOrder() { appBar_.updateZOrder(hwnd); }

    bool updateAppBarPosition() {
        if (!hwnd || !appBar_.registered())
            return false;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(appBarMonitor(), &monitor))
            return false;

        UINT nextDpi = GetDpiForWindow(hwnd);
        if (nextDpi == 0 && taskbar_)
            nextDpi = GetDpiForWindow(taskbar_);
        if (nextDpi == 0)
            nextDpi = 96;
        if (dpi_ != nextDpi) {
            dpi_ = nextDpi;
            renderer.setDpi(dpi_);
            requestInvalidation(toMask(RenderInvalidation::Layout) |
                                toMask(RenderInvalidation::Text) |
                                toMask(RenderInvalidation::Cover));
        }

        const LONG height =
            std::max<LONG>(1, static_cast<LONG>(std::lround(kAppBarHeightDip * dpi_ / 96.0f)));
        if (!appBar_.positionWindow(hwnd, appBarMonitor(), appBarEdge_, height))
            return false;
        mediaPopup.setAnchor(hwnd);
        setPlacementStatus(TaskbarPlacementStatus::Safe);
        return true;
    }

    bool registerAppBar() {
        if (!isAppBarView() || !hwnd)
            return false;
        if (appBar_.registered())
            return updateAppBarPosition();
        if (!detachFromTaskbar())
            return false;
        if (!appBar_.registerWindow(hwnd, kAppBarCallbackMessage)) {
            runtime_log::writef(L"[appbar] ABM_NEW failed");
            return false;
        }
        if (!updateAppBarPosition()) {
            unregisterAppBar();
            return false;
        }
        runtime_log::writef(L"[appbar] registered edge=%s",
                            appBarEdge_ == AppBarEdge::Top ? L"top" : L"bottom");
        return true;
    }

    void unregisterAppBar() { appBar_.unregisterWindow(hwnd); }

    void retryTaskbarAttach() {
        if (!hwnd)
            return;
        if (!findTaskbar())
            return;

        const bool immersiveBackdropPrepared =
            viewMode_ == TaskbarViewMode::Immersive && !dockBackdrop_.available();
        if (immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (!attachToTaskbar(hwnd)) {
            // 附着失败时仍保持顶层窗口，但必须使用屏幕坐标，避免落到屏幕顶部。
            adjustPosition();
            return;
        }

        cancelTaskbarAttachRetry();
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        adjustPosition();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    bool createWindow(HINSTANCE inst, const std::wstring& monitorDeviceName,
                      TaskbarViewMode initialViewMode) {
        this->inst = inst;
        targetMonitorDeviceName_ = monitorDeviceName;
        viewMode_ = initialViewMode;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        clientAnimations_ = animations != FALSE;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWndClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const HICON icon = app_icon::taskbarIcon();
        wc.hIcon = icon;
        wc.hIconSm = icon;
        RegisterClassExW(&wc);

        if (!findTaskbar())
            return false;

        // 内容完全由 DirectComposition visual 提供；禁用窗口自身的不透明重定向位图，
        // 避免登录或解锁后重建的底面透过半透明遮罩显示为灰底。
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
        HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyricTaskbar", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, inst, this);
        if (!h)
            return false;

        hwnd = h;
        app_icon::applyTaskbarIcon(hwnd);
        const bool immersiveBackdropPrepared = viewMode_ == TaskbarViewMode::Immersive;
        if (immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (isAppBarView()) {
            attachment_.reset();
            renderState_.setVisibilitySuppressed(false);
            setPlacementStatus(TaskbarPlacementStatus::Safe);
        } else if (!attachToTaskbar(h)) {
            scheduleTaskbarAttachRetry();
        }
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (!mediaPopup.create(inst, hwnd))
            runtime_log::writef(L"[taskbar] media popup creation failed");
        if (!volumePopup_.create(inst))
            runtime_log::writef(L"[taskbar] volume popup creation failed");
        updateResourceColumnWidths();
        adjustPosition();
        startProbe(); // 避让探测（阻塞型跨进程调用）全程在工作线程执行
        if (resourceMonitoringEnabled() && !isStoppedMode())
            startResourceMonitoring();
        if (renderState_.visibilitySuppressed() && !isStoppedMode())
            startPlacementTimer();
        return true;
    }

    // 任务栏主轴上的占用区间（屏幕坐标，已合并）：横向任务栏取 [left, right)，
    // 纵向任务栏取 [top, bottom)。来源为 UIA 按钮 + 通知区 + TrafficMonitor + 酷狗窗口。
    // 开始按钮的 HWND 矩形与 UIA StartButton 重复，合并后无害，留着可在 UIA
    // 不可用时兜底。
    std::vector<std::pair<int, int>> occupiedIntervals() const {
        return taskbar_detail::occupiedIntervals(
            {isVerticalTaskbar(), notify_ != nullptr, start_ != nullptr, rcNotify_, rcStart_,
             rcTrafficMonitor_, uiaButtons_, kugouTaskbarWindows_});
    }

    void startPlacementTimer() {
        if (placementTimerRunning_ || !hwnd || isStoppedMode())
            return;
        if (SetTimer(hwnd, kPlacementTimerId, kPlacementTimerMs, nullptr))
            placementTimerRunning_ = true;
    }

    void stopPlacementTimer() {
        if (placementTimerRunning_ && hwnd)
            KillTimer(hwnd, kPlacementTimerId);
        placementTimerRunning_ = false;
    }

    void resumeAfterPlacementAvailable() {
        if ((!probeReady_ && !allowOverlap_) ||
            placementStatus_ == TaskbarPlacementStatus::NoSpace ||
            placementStatus_ == TaskbarPlacementStatus::Unavailable)
            return;

        renderState_.setVisibilitySuppressed(false);
        placementProbe_.setFast(false);
        stopPlacementTimer();
        if (isSessionVisible() && !isStoppedMode()) {
            // 隐藏期间不运行完整帧定时器；恢复前补一次播放进度/当前行，
            // 避免窗口重新出现时先显示旧歌词，等下一帧才追上。
            if (tick)
                tick();
            const bool visibilityChanged = reconcileWindowVisibility(true);
            if (isWindowVisible() && !visibilityChanged)
                requestFrameAndFlush();
        } else if (!isWindowVisible()) {
            stopFrameTimer();
        }
    }

    TaskbarPlacementStatus setPlacementStatus(TaskbarPlacementStatus status) {
        if (placementStatus_ == status)
            return status;
        const TaskbarPlacementStatus previous = placementStatus_;
        placementStatus_ = status;
        if (status == TaskbarPlacementStatus::NoSpace ||
            status == TaskbarPlacementStatus::Unavailable) {
            placementProbe_.setFast(status == TaskbarPlacementStatus::NoSpace);
            startPlacementTimer();
            reconcileWindowVisibility();
            stopFrameTimer();
        } else if (previous == TaskbarPlacementStatus::NoSpace ||
                   previous == TaskbarPlacementStatus::Unavailable) {
            resumeAfterPlacementAvailable();
        }
        if (onPlacementStatusChanged_)
            onPlacementStatusChanged_(status);
        return status;
    }

    void releaseVisibilitySuppression() {
        if (!renderState_.visibilitySuppressed() || !probeReady_)
            return;

        if (placementStatus_ == TaskbarPlacementStatus::NoSpace) {
            // 首次真实探测已经完成：解除“等待首次结果”的抑制，但仍保持隐藏，
            // 后续由低频避让计时器等待空间恢复。
            renderState_.setVisibilitySuppressed(false);
            placementProbe_.setFast(true);
            startPlacementTimer();
            return;
        }
        if (placementStatus_ == TaskbarPlacementStatus::Unavailable)
            return;
        resumeAfterPlacementAvailable();
    }

    void setVisibilitySuppressed(bool on) {
        if (on) {
            renderState_.setVisibilitySuppressed(true);
            reconcileWindowVisibility();
            if (hwnd && !isStoppedMode())
                startPlacementTimer();
            return;
        }

        if (!probeReady_)
            return;
        releaseVisibilitySuppression();
    }

    void show() {
        if (isAppBarView()) {
            renderState_.setVisibilitySuppressed(false);
            setPlacementStatus(TaskbarPlacementStatus::Safe);
            renderState_.setSessionVisible(true);
            reconcileWindowVisibility();
            requestFrameAndFlush();
            return;
        }
        if (renderState_.visibilitySuppressed() && !allowOverlap_)
            return;
        if ((placementStatus_ == TaskbarPlacementStatus::NoSpace ||
             placementStatus_ == TaskbarPlacementStatus::Unavailable) &&
            !allowOverlap_)
            return;
        if (allowOverlap_) {
            renderState_.setVisibilitySuppressed(false);
            placementProbe_.setFast(false);
            stopPlacementTimer();
        }
        renderState_.setSessionVisible(true);
        reconcileWindowVisibility();
        requestFrameAndFlush();
    }

    void hide() {
        renderState_.setSessionVisible(false);
        reconcileWindowVisibility();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.hide();
        mediaPopup.hideImmediate();
    }

    TaskbarPlacementStatus calculateWindowPlacement(WindowPlacement& placement,
                                                    int positionMode = -1,
                                                    bool updateStatus = true) {
        auto finishStatus = [&](TaskbarPlacementStatus status) {
            return updateStatus ? setPlacementStatus(status) : status;
        };
        if (!hwnd || !taskbar_)
            return finishStatus(TaskbarPlacementStatus::Unavailable);

        updateRects();

        const bool vertical = isVerticalTaskbar();
        if (viewMode_ == TaskbarViewMode::Immersive) {
            RECT client{};
            POINT origin{0, 0};
            if (!GetClientRect(taskbar_, &client) || client.right <= client.left ||
                client.bottom <= client.top || !ClientToScreen(taskbar_, &origin))
                return finishStatus(TaskbarPlacementStatus::Unavailable);

            placement.x = origin.x;
            placement.y = origin.y;
            placement.width = client.right - client.left;
            placement.height = client.bottom - client.top;
            placement.availableMajor = vertical ? placement.height : placement.width;
            syncCompactTaskbarLayout(placement.height);
            return finishStatus(TaskbarPlacementStatus::Safe);
        }

        const int taskbarCross = taskbarCrossPixels(rcTaskbar_, taskbarEdge_);
        const int crossMargin = std::max(2, (int)std::lround(2.0f * scale()));
        int crossPx = taskbarCross - crossMargin * 2;
        if (crossPx < 16)
            crossPx = taskbarCross;
        if (crossPx <= 0)
            return finishStatus(TaskbarPlacementStatus::Unavailable);
        syncCompactTaskbarLayout(vertical ? 0 : crossPx);

        const int effectivePositionMode = positionMode < 0 ? positionMode_ : positionMode;

        int gap = std::max(4, (int)std::lround(4.0f * scale()));
        float minWidthDip = vertical ? kVerticalMinLengthDip : kMinWidthDip;
        float maxWidthDip = vertical ? kVerticalMaxLengthDip : kMaxWidthDip;
        float compressedMinWidthDip = minWidthDip * kCompressedMinWidthRatio;
        if (embeddedResourceOnly()) {
            minWidthDip = 0.0f;
            compressedMinWidthDip = 0.0f;
            maxWidthDip = 0.0f;
        } else if (!vertical && !songInfoVisible_ && scene_ != DisplayScene::Idle) {
            // 保留原歌词区宽度，只扣除歌曲信息区；左侧压缩为可见的封面区域。
            const float compactLeftDip = albumCoverVisible_ ? coverSlotWidth(dip(crossPx)) : 0.0f;
            const float flexibleMinDip = kMinWidthDip * (1.0f - kLeftRatio);
            minWidthDip = flexibleMinDip + compactLeftDip;
            compressedMinWidthDip = flexibleMinDip * kCompressedMinWidthRatio + compactLeftDip;
            maxWidthDip = kMaxWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
        }
        int minW = (int)std::lround(minWidthDip * scale());
        int compressedMinW = (int)std::lround(compressedMinWidthDip * scale());
        int maxW = (int)std::lround(maxWidthDip * scale());
        // 独立频谱和内嵌资源区整体加宽；背景波浪复用内容区。
        const float spectrumExtra = spectrumExtraForScene(scene_);
        if (!vertical && spectrumExtra > 0.0f) {
            int extra = (int)std::lround(spectrumExtra * scale());
            minW += extra;
            compressedMinW += extra;
            maxW += extra;
        }

        taskbar_detail::PlacementInput input;
        input.taskbar = rcTaskbar_;
        input.edge = taskbarEdge_;
        input.vertical = vertical;
        input.allowOverlap = allowOverlap_;
        input.positionMode = effectivePositionMode;
        input.gap = gap;
        input.minMajor = minW;
        input.compressedMinMajor = compressedMinW;
        input.maxMajor = maxW;
        input.crossPixels = crossPx;
        input.crossMargin = crossMargin;
        input.occupied = occupiedIntervals();
        return finishStatus(taskbar_detail::calculatePlacement(input, placement));
    }

    void applyWindowPlacement(const WindowPlacement& placement, bool bringToFront = true,
                              bool noRedraw = false) {
        if (!hwnd || placement.width <= 0 || placement.height <= 0)
            return;

        WindowPlacement current;
        const bool hasCurrent = currentWindowPlacement(current);
        const bool moved = !hasCurrent || current.x != placement.x || current.y != placement.y;
        const bool resized = !hasCurrent || current.width != placement.width ||
                             current.height != placement.height;
        if (!moved && !resized && (!bringToFront || GetTopWindow(GetParent(hwnd)) == hwnd))
            return;

        POINT pt{placement.x, placement.y};
        if (attachment_.embedded())
            ScreenToClient(taskbar_, &pt);

        // 把窗口提到任务栏子窗口最前面，避免被其他任务栏子窗口盖住。
        // 尺寸转场期间保持 Z 序且禁止系统自动重绘，画面由后续 render() 统一提交。
        UINT flags = SWP_NOACTIVATE;
        if (!moved)
            flags |= SWP_NOMOVE;
        if (!resized)
            flags |= SWP_NOSIZE;
        if (!bringToFront)
            flags |= SWP_NOZORDER;
        if (noRedraw)
            flags |= SWP_NOREDRAW;
        SetWindowPos(hwnd, bringToFront ? HWND_TOP : nullptr, pt.x, pt.y, placement.width,
                     placement.height, flags);
        mediaPopup.setAnchor(hwnd);
    }

    // TrafficMonitor 是任务栏的外部子窗口，刷新自身内容时可能重新进入任务栏
    // 子窗口 Z 序的顶部。嵌入模式需要和它互相避让，沉浸模式则必须始终覆盖它；
    // 这里只在发现宿主不在顶部时修正，并明确禁止激活，避免伪装成“跑到前台”。
    void ensureImmersiveZOrder() {
        if (viewMode_ != TaskbarViewMode::Immersive || !hwnd || !taskbar_ ||
            !attachment_.embedded() || GetParent(hwnd) != taskbar_)
            return;
        if (GetTopWindow(taskbar_) == hwnd)
            return;
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void adjustPosition() {
        if (applyingAppearance_) {
            appearancePositionPending_ = true;
            return;
        }
        if (drag_.dragging())
            return;
        if (isAppBarView()) {
            cancelSceneWindowResize();
            if (appBar_.registered())
                updateAppBarPosition();
            return;
        }
        WindowPlacement placement;
        const TaskbarPlacementStatus status = calculateWindowPlacement(placement);
        // 探测到的按钮变化未影响最终位置时，继续场景尺寸动画，避免中途跳到终点。
        if (sceneResize_ && status != TaskbarPlacementStatus::Unavailable &&
            status != TaskbarPlacementStatus::NoSpace &&
            placement.x == sceneResize_->to.x && placement.y == sceneResize_->to.y &&
            placement.width == sceneResize_->to.width &&
            placement.height == sceneResize_->to.height)
            return;
        cancelSceneWindowResize();
        if (status != TaskbarPlacementStatus::Unavailable &&
            status != TaskbarPlacementStatus::NoSpace)
            applyWindowPlacement(placement);
    }

    TaskbarPlacementStatus refreshPlacement() {
        adjustPosition();
        return placementStatus_;
    }

    bool currentWindowPlacement(WindowPlacement& placement) const {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
            return false;

        placement.x = rect.left;
        placement.y = rect.top;
        placement.width = rect.right - rect.left;
        placement.height = rect.bottom - rect.top;
        return true;
    }

    static bool usableDragPlacement(TaskbarPlacementStatus status) {
        return status != TaskbarPlacementStatus::Unavailable &&
               status != TaskbarPlacementStatus::NoSpace;
    }

    WindowPlacement dragPreviewFromAnchor(const WindowPlacement& anchor, int mode) const {
        return taskbar_detail::dragPreview(anchor, mode, isVerticalTaskbar(), drag_.previewMajor());
    }

    bool dragPreviewPlacementAt(POINT cursorScreen, WindowPlacement& placement, int& mode) {
        bool found = false;
        long long bestDistance = 0;
        const int cursorMajor = isVerticalTaskbar() ? cursorScreen.y : cursorScreen.x;
        for (int candidateMode = 0; candidateMode <= 1; ++candidateMode) {
            WindowPlacement anchor;
            const TaskbarPlacementStatus status =
                calculateWindowPlacement(anchor, candidateMode, false);
            if (!usableDragPlacement(status))
                continue;

            const WindowPlacement preview = dragPreviewFromAnchor(anchor, candidateMode);
            const int previewStart = isVerticalTaskbar() ? preview.y : preview.x;
            const int previewLength = isVerticalTaskbar() ? preview.height : preview.width;
            const long long distance =
                std::llabs(static_cast<long long>(cursorMajor) * 2 -
                           (static_cast<long long>(previewStart) * 2 + previewLength));
            if (!found || distance < bestDistance ||
                (distance == bestDistance && candidateMode == positionMode_)) {
                found = true;
                bestDistance = distance;
                placement = preview;
                mode = candidateMode;
            }
        }
        return found;
    }

    bool updateDragPreviewPlacement() {
        if (!drag_.dragging())
            return false;
        WindowPlacement placement;
        int mode = positionMode_;
        if (!dragPreviewPlacementAt(drag_.cursor(), placement, mode))
            return false;

        drag_.setCandidate(mode);
        WindowPlacement current;
        if (currentWindowPlacement(current) && current.x == placement.x &&
            current.y == placement.y && current.width == placement.width &&
            current.height == placement.height)
            return false;
        applyWindowPlacement(placement, true, true);
        return true;
    }

    void beginLyricDrag() {
        if (isExpandedView() || drag_.dragging())
            return;
        WindowPlacement current;
        // 预览表达的是用户此刻看到的整个任务栏歌词宿主，而非单独歌词文本：
        // 当前 HWND 主轴尺寸已包含封面、歌曲信息、歌词区和独立频谱。
        const int preview = currentWindowPlacement(current)
                                ? (isVerticalTaskbar() ? current.height : current.width)
                                : 0;
        drag_.begin(preview, positionMode_);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        cancelSceneWindowResize();
        setSongTransitionPending(false);
        renderer.resetRoot();
        clearImmersiveSongContentTransition();
        renderer.clearLyricTransitionLayers();
        clearLyricDCompState();
        mouseOver_ = false;
        trackingLeave_ = false;
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.hide();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        updateDragPreviewPlacement();
        requestFrameAndFlush();
    }

    void finishLyricDrag(bool commit) {
        const bool wasDragging = drag_.dragging();
        const int committedMode = drag_.candidate();
        drag_.end();
        if (GetCapture() == hwnd)
            ReleaseCapture();
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));

        if (!wasDragging)
            return;
        trackingLeave_ = false;
        mouseOver_ = false;
        immersiveControlHover_ = -1;
        if (commit)
            positionMode_ = committedMode;
        requestInvalidation(RenderInvalidation::Layout);
        adjustPosition();
        requestFrameAndFlush();
        if (commit && onPositionModeChanged)
            onPositionModeChanged(positionMode_);
        drag_.clearPreview();
    }

    bool detectChanges() {
        if (!taskbar_ || !IsWindow(taskbar_))
            return false;

        RECT rcTaskbar{}, rcNotify{};
        GetWindowRect(taskbar_, &rcTaskbar);
        if (notify_)
            GetWindowRect(notify_, &rcNotify);
        UINT dpi = GetDpiForWindow(taskbar_);
        bool center = isTaskbarCenterAlign();
        bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        const UINT edge = queryTaskbarEdge(taskbar_, rcTaskbar);
        const bool edgeChanged = edge != taskbarEdge_;
        bool themeChanged = light != lightTheme_;

        RECT rcStart{};
        if (start_)
            GetWindowRect(start_, &rcStart);

        // TrafficMonitor / 酷狗 / UIA 按钮矩形由探测工作线程提供（pickProbeResult），
        // 这里只做非阻塞检查，UI 线程不允许出现阻塞型跨进程调用
        bool changed = dpi != dpi_ || center != centerAlign_ || edgeChanged || themeChanged ||
                       !EqualRect(&rcTaskbar, &rcTaskbar_) || !EqualRect(&rcNotify, &rcNotify_) ||
                       !EqualRect(&rcStart, &rcStart_);
        if (changed) {
            // 封面位图按显示尺寸解码（decodeCover），DPI/任务栏厚度或方向变化时按新尺寸重解码
            if (dpi != dpi_ || edge != taskbarEdge_ ||
                taskbarCrossPixels(rcTaskbar, edge) != taskbarCrossPixels(rcTaskbar_, taskbarEdge_))
                requestInvalidation(RenderInvalidation::Cover);
            dpi_ = dpi;
            centerAlign_ = center;
            taskbarEdge_ = edge;
            if (edgeChanged)
                renderer.resetRoot();
            if (edgeChanged) {
                // 左右侧的竖排文字方向相反；任务栏方向变化时必须重建逐字布局，
                // 同时丢弃沿用自另一方向的滚动偏移。
                requestInvalidation(RenderInvalidation::Text);
                scroll_.resetLyric();
            }
            rcTaskbar_ = rcTaskbar;
            rcNotify_ = rcNotify;
            rcStart_ = rcStart;
            renderer.setDpi(dpi_);
            requestInvalidation(RenderInvalidation::Layout);
            if (themeChanged) {
                lightTheme_ = light;
                updateBackdropSolidColor();
                discardDeviceResources();
            }
        }
        return changed;
    }

    // ---------- 资源 ----------

    void createDeviceResources() {
        if (brushBg_.Get()) {
            renderState_.setDeviceResourcePhase(RenderState::DeviceResourcePhase::Ready);
            return;
        }
        renderer.initialize();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);

        // 根据任务栏深浅主题选择配色，贴近 Windows 11 原生媒体控件
        // 背景使用极低的 alpha，视觉上透明但保证分层窗口命中测试覆盖整个区域
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.01f),
                                      brushBg_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f),
                                      brushHover_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.95f),
                                      brushText_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.30f, 0.30f, 0.75f),
                                      brushDim_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.90f),
                                      brushBtn_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.30f),
                                      brushBtnDisabled_.ReleaseAndGetAddressOf());
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.01f),
                                      brushBg_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f),
                                      brushHover_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.95f),
                                      brushText_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.65f),
                                      brushDim_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.90f),
                                      brushBtn_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.35f),
                                      brushBtnDisabled_.ReleaseAndGetAddressOf());
        }
        // 歌词画刷与主题无关，单独创建（用户可换色，换色时只重建这四个）
        createLyricBrushes();
        // 进度背景颜色每帧经 SetColor 写入，这里只建空画刷
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                  brushProgressBg_.ReleaseAndGetAddressOf());
        // 纯色背景/封面模糊遮罩同样每帧 SetColor
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                  brushBackground_.ReleaseAndGetAddressOf());
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.32f, 0.15f, 1.0f),
                                      brushIdleWarm_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.40f, 0.64f, 1.0f),
                                      brushIdleCool_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.47f, 0.34f, 1.0f),
                                      brushIdleAccent_.ReleaseAndGetAddressOf());
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 0.56f, 0.24f, 1.0f),
                                      brushIdleWarm_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.56f, 0.80f, 1.00f, 1.0f),
                                      brushIdleCool_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.95f, 0.70f, 1.0f),
                                      brushIdleAccent_.ReleaseAndGetAddressOf());
        }
        rt->CreateLayer(coverLayer_.ReleaseAndGetAddressOf());
        lyricTextRenderer_.createDeviceResources(rt);
        recreateFormats();
        typesetter_.setFactory(renderer.dwrite());
        typesetter_.createDragPreviewFormat();
        if (auto* factory = renderer.d2d()) {
            D2D1_STROKE_STYLE_PROPERTIES props{};
            props.startCap = D2D1_CAP_STYLE_FLAT;
            props.endCap = D2D1_CAP_STYLE_FLAT;
            props.dashCap = D2D1_CAP_STYLE_FLAT;
            props.lineJoin = D2D1_LINE_JOIN_ROUND;
            props.miterLimit = 10.0f;
            props.dashStyle = D2D1_DASH_STYLE_DASH;
            props.dashOffset = 0.0f;
            factory->CreateStrokeStyle(props, nullptr, 0,
                                       dragPreviewStroke_.ReleaseAndGetAddressOf());
        }
        renderState_.setDeviceResourcePhase(brushBg_.Get()
                                                ? RenderState::DeviceResourcePhase::Ready
                                                : RenderState::DeviceResourcePhase::Uninitialized);
    }

    // 盘面底色在绘制时跟随专辑，反光以半透明黑白叠加，保留专辑色相。
    void createAlbumCoverBrushes() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        brushVinylBase_.Reset();
        brushVinylGroove_.Reset();
        brushVinylSurface_.Reset();
        brushTurntable_.Reset();
        rt->CreateSolidColorBrush(D2D1::ColorF(0.055f, 0.060f, 0.068f),
                                  brushVinylBase_.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.72f, 0.75f, 0.16f),
                                  brushVinylGroove_.ReleaseAndGetAddressOf());
        const D2D1_GRADIENT_STOP stops[] = {
            {0.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f)},
            {0.22f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f)},
            {0.38f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f)},
            {0.52f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f)},
            {0.76f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)},
            {1.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f)},
        };
        ID2D1GradientStopCollection* collection = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, _countof(stops), D2D1_GAMMA_2_2,
                                                       D2D1_EXTEND_MODE_CLAMP, &collection))) {
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                              D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 1.0f)),
                                          collection, brushVinylSurface_.ReleaseAndGetAddressOf());
            collection->Release();
        }
        rt->CreateSolidColorBrush(D2D1::ColorF(0.07f, 0.08f, 0.09f),
                                  brushTurntable_.ReleaseAndGetAddressOf());
    }

    void releaseAlbumCoverEffectResources() {
        brushVinylBase_.Reset();
        brushVinylGroove_.Reset();
        brushVinylSurface_.Reset();
        brushTurntable_.Reset();
        vinylCoverClip_.Reset();
    }

    // 歌词与黑胶画刷：随用户颜色重建，与主题画刷解耦
    void createLyricBrushes() {
        if (applyingAppearance_) {
            appearanceBrushesPending_ = true;
            return;
        }
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        lyricTextRenderer_.invalidate(); // 颜色变化使离屏缓存失效
        if (brushLyric_.Get()) {
            brushLyric_.Reset();
        }
        if (brushLyricDim_.Get()) {
            brushLyricDim_.Reset();
        }
        if (brushLyricGlow_.Get()) {
            brushLyricGlow_.Reset();
        }
        if (brushLyricOutline_.Get()) {
            brushLyricOutline_.Reset();
        }
        auto rgb = [](COLORREF c, float a) {
            return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f,
                                a);
        };
        rt->CreateSolidColorBrush(rgb(lyricColor_, 1.00f), brushLyric_.ReleaseAndGetAddressOf());
        lyricEffectsRenderer_.setContext(lyricEffectsContext(rt));
        lyricEffectsRenderer_.createBrushes();
        rt->CreateSolidColorBrush(rgb(lyricUnplayedColor_, lyricUnplayedAlphaPct_ / 100.0f),
                                  brushLyricDim_.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(rgb(lyricGlowColor_, 0.28f),
                                  brushLyricGlow_.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(rgb(lyricOutlineColor_, 0.50f),
                                  brushLyricOutline_.ReleaseAndGetAddressOf());
        // 频谱颜色模式独立于歌词颜色设置：默认跟随已播放色，也可跟随专辑主题色或使用自定义色。
        // 尚未提取到当前专辑主色时，专辑模式沿用歌词模式的当前有效已播放色。
        spectrum_.setContext(spectrumContext(rt));
        spectrum_.createBrushes();
        if (albumCoverEffect_ == AlbumCoverEffect::Vinyl)
            createAlbumCoverBrushes();
        else
            releaseAlbumCoverEffectResources();
    }

    void setFontGlowColors(COLORREF glow, COLORREF outline) {
        if (lyricGlowColor_ == glow && lyricOutlineColor_ == outline)
            return;
        lyricGlowColor_ = glow;
        lyricOutlineColor_ = outline;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) {
        if (lyricColor_ == played && lyricUnplayedColor_ == unplayed &&
            lyricUnplayedAlphaPct_ == unplayedAlphaPct)
            return;
        lyricColor_ = played;
        lyricUnplayedColor_ = unplayed;
        lyricUnplayedAlphaPct_ = unplayedAlphaPct;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setFontGlow(bool on) {
        if (lyricGlow_ == on)
            return;
        lyricGlow_ = on;
        if (compactTaskbarHeight_ > 0.0f)
            recreateFormats();
        lyricTextRenderer_.invalidate(); // 效果组合变化，不能复用旧的离屏缓存
        requestFrameAndFlush();
    }

    void setFontOutline(bool on) {
        if (lyricOutline_ == on)
            return;
        lyricOutline_ = on;
        if (compactTaskbarHeight_ > 0.0f)
            recreateFormats();
        lyricTextRenderer_.invalidate(); // 效果组合变化，不能复用旧的离屏缓存
        requestFrameAndFlush();
    }

    void setSecondaryLyricMode(bool translation, bool romanization) {
        // 调用方保证互斥，这里再收紧一次，避免异常状态绘制三行。
        if (translation && romanization)
            romanization = false;
        if (translationEnabled_ == translation && romanizationEnabled_ == romanization)
            return;
        translationEnabled_ = translation;
        romanizationEnabled_ = romanization;
        resetLyricTransition();
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void setDoubleLineLyrics(bool on) {
        if (doubleLineLyricsEnabled_ == on)
            return;
        doubleLineLyricsEnabled_ = on;
        resetLyricTransition();
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void setLyricAlignment(LyricAlignment alignment) {
        if (lyricAlignment_ == alignment)
            return;
        lyricAlignment_ = alignment;
        scroll_.resetLyric();
        scroll_.resetSecondary();
        requestFrameAndFlush();
    }

    void setIdleQuoteAlignment(LyricAlignment alignment) {
        if (idleQuoteAlignment_ == alignment)
            return;
        idleQuoteAlignment_ = alignment;
        scroll_.resetLyric();
        scroll_.resetSecondary();
        requestFrameAndFlush();
    }

    void setIdleQuoteBackground(IdleQuoteBackground background) {
        if (idleQuoteBackground_ == background)
            return;
        idleQuoteBackground_ = background;
        refreshFrameTimer();
        requestFrameAndFlush();
    }

    void setIdleQuoteBackgroundScope(IdleQuoteBackgroundScope scope) {
        if (idleQuoteBackgroundScope_ == scope)
            return;
        idleQuoteBackgroundScope_ = scope;
        refreshFrameTimer();
        requestFrameAndFlush();
    }

    bool isMinimalMode() const { return isRenderMode(RenderMode::Minimal); }

    bool mediaPopupAvailable(bool sessionVisible) const {
        return sessionVisible && !isStoppedMode() && !isMinimalMode();
    }

    static bool isTaskbarMediaScene(DisplayScene scene) {
        return scene == DisplayScene::Searching || scene == DisplayScene::Lyrics ||
               scene == DisplayScene::Spectrum || scene == DisplayScene::Message;
    }

    static bool isTaskbarSceneTransition(DisplayScene from, DisplayScene to) {
        return (from == DisplayScene::Idle && isTaskbarMediaScene(to)) ||
               (isTaskbarMediaScene(from) && to == DisplayScene::Idle);
    }

    bool mediaPopupEnabledForScene() const {
        if (isExpandedView() || isMinimalMode() || isStoppedMode())
            return false;
        if (scene_ == DisplayScene::Idle)
            return isSessionVisible() && idle.quickStartEnabled;
        if (hoverControlStyle_ == HoverControlStyle::Popup)
            return controlsOnHover_;
        return isSessionVisible() && idle.quickStartEnabled;
    }

    void syncMediaPopupEnabled() {
        const bool enabled = mediaPopupEnabledForScene();
        const bool wasEnabled = mediaPopupEnabled_;
        mediaPopupEnabled_ = enabled;
        mediaPopup.setEnabled(enabled);
        if (enabled && !wasEnabled) {
            // 弹窗样式可能在媒体会话已经存在时才开启；补送当前完整快照，
            // 让弹窗的可用状态和展示类别不依赖下一次 SMTC 事件。
            const bool available = mediaPopupAvailable(isSessionVisible());
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setIdleContent(idle, available);
            mediaPopup.setPresentationMode(scene_, available,
                                           hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.setMedia(media, available);
            mediaPopup.endPresentationUpdate();
        } else if (enabled) {
            const bool available = mediaPopupAvailable(isSessionVisible());
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setPresentationMode(scene_, available,
                                           hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.endPresentationUpdate();
        }
        mediaPopup.setTriggerOnHover(floatingCardTrigger_ == MediaPopupTrigger::Hover);
        if (enabled && mouseOver_)
            mediaPopup.onAnchorEnter();
    }

    void releaseCoverBackgroundResources() { cover_.releaseBackground(); }

    void setControlsOnHover(bool on) {
        if (controlsOnHover_ == on)
            return;
        controlsOnHover_ = on;
        volumeHover_ = false;
        volumePopup_.hide();
        syncMediaPopupEnabled();
        requestFrameAndFlush();
    }

    void setHoverControlStyle(HoverControlStyle style) {
        if (hoverControlStyle_ == style)
            return;
        hoverControlStyle_ = style;
        volumeHover_ = false;
        volumePopup_.hide();
        syncMediaPopupEnabled();
        requestFrameAndFlush();
    }

    void setFloatingCardTrigger(MediaPopupTrigger trigger) {
        if (floatingCardTrigger_ == trigger)
            return;
        floatingCardTrigger_ = trigger;
        syncMediaPopupEnabled();
    }

    void setFloatingCardBackground(MediaPopupBackground mode) {
        mediaPopup.setBackgroundMode(mode);
    }

    void setFloatingCardBackgroundColor(COLORREF color, bool customized) {
        mediaPopup.setBackgroundColor(color, customized);
    }

    void setFloatingCardFollowAlbum(bool on) { mediaPopup.setFollowAlbumBackground(on); }

    void setFloatingCardAutoTextContrast(bool on) { mediaPopup.setAutoTextContrast(on); }

    bool backgroundWaveEnabled() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.backgroundWaveEnabled();
    }

    bool integratedSpectrum() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.integratedSpectrum();
    }

    void resetSpectrumVisuals() {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.resetSpectrumVisuals();
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    void resetSpectrumEffectHistory() {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.resetSpectrumEffectHistory();
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    void releaseSpectrumEffectResources() {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.releaseSpectrumEffectResources();
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    void setSpectrumEffects(uint32_t effects) {
        effects &= kSpectrumEffectsMask;
        if (spectrumEffects_ == effects)
            return;
        spectrumEffects_ = effects;
        releaseSpectrumEffectResources();
        requestFrameAndFlush();
    }

    bool spectrumEffectsAnimating() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumEffectsAnimating();
    }

    void updateSpectrumEffects(ULONGLONG now) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.updateSpectrumEffects(now);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    bool spectrumVisualsAnimating() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumVisualsAnimating();
    }

    void updateSpectrumVisuals(ULONGLONG now) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.updateSpectrumVisuals(now);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    bool backgroundWaveAnimating() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.backgroundWaveAnimating();
    }

    void setSpectrumStyle(SpectrumStyle style) {
        if (spectrumStyle_ == style)
            return;
        spectrumStyle_ = style;
        resetSpectrumVisuals();
        if (spectrumVisible_)
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumColor(COLORREF color, bool customized, bool followAlbum) {
        if (spectrumColor_ == color && spectrumCustomColor_ == customized &&
            spectrumFollowAlbum_ == followAlbum)
            return;
        spectrumColor_ = color;
        spectrumCustomColor_ = customized;
        spectrumFollowAlbum_ = followAlbum && !customized;
        resetSpectrumEffectHistory();
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setSpectrumAlbumColor(COLORREF color, bool available) {
        if (spectrumAlbumColor_ == color && spectrumAlbumColorAvailable_ == available)
            return;
        spectrumAlbumColor_ = color;
        spectrumAlbumColorAvailable_ = available;
        if (spectrumFollowAlbum_ && !spectrumCustomColor_) {
            resetSpectrumEffectHistory();
            createLyricBrushes();
        }
        requestFrameAndFlush();
    }

    void setSpectrumGradient(bool on) {
        if (spectrumGradient_ == on)
            return;
        spectrumGradient_ = on;
        resetSpectrumEffectHistory();
        requestFrameAndFlush();
    }

    void setSpectrumBackground(bool on) {
        if (spectrumBackground_ == on)
            return;
        const bool wasBackground = backgroundWaveEnabled();
        spectrumBackground_ = on;
        resetSpectrumEffectHistory();
        if (spectrumVisible_ && wasBackground != backgroundWaveEnabled())
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (spectrumOpacityPct_ == next)
            return;
        spectrumOpacityPct_ = next;
        resetSpectrumEffectHistory();
        if (backgroundWaveEnabled())
            requestFrameAndFlush();
    }

    // 进度背景实际生效条件：用户开启 && 背景波浪未占用背景 && 当前歌曲有时长
    bool progressBackgroundActive() const {
        return progressBackground_ && !backgroundWaveEnabled() && media.durationMs > 0;
    }

    void setProgressBackground(bool on) {
        if (progressBackground_ == on)
            return;
        progressBackground_ = on;
        requestFrameAndFlush();
    }

    void setProgressBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (progressBackgroundOpacityPct_ == next)
            return;
        progressBackgroundOpacityPct_ = next;
        if (progressBackgroundActive())
            requestFrameAndFlush();
    }

    void setBackground(TaskbarBackground mode) {
        if (background_ == mode)
            return;
        background_ = mode;
        if (mode != TaskbarBackground::CoverBlur)
            releaseCoverBackgroundResources();
        requestFrameAndFlush();
    }

    void setCoverBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (coverBackgroundOpacityPct_ == next)
            return;
        coverBackgroundOpacityPct_ = next;
        if (background_ == TaskbarBackground::CoverBlur)
            requestFrameAndFlush();
    }

    void setViewMode(TaskbarViewMode mode) {
        if (viewMode_ == mode)
            return;

        const TaskbarViewMode previousMode = viewMode_;
        if (hwnd && IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_HIDE);
        resetImmersivePresence();
        const bool hadImmersivePress = immersiveControlPressed_ >= 0;
        if (drag_.pressed() || drag_.dragging())
            finishLyricDrag(false);
        immersiveControlPressed_ = -1;
        if (hadImmersivePress && GetCapture() == hwnd)
            ReleaseCapture();
        renderer.resetRoot();
        clearImmersiveSongContentTransition();
        if (previousMode == TaskbarViewMode::AppBar)
            unregisterAppBar();
        stopResourceMonitoring();
        viewMode_ = mode;
        bool immersiveBackdropPrepared = false;
        if (mode == TaskbarViewMode::Immersive && !dockBackdrop_.available()) {
            if (!attachment_.embedded() || detachFromTaskbar())
                initializeBackdropForCurrentViewMode();
            else
                backgroundBlurSupported_ = false;
            immersiveBackdropPrepared = true;
        }
        dockPet_.setMode(DockPetMode::Hidden, monotonicNowMs());
        resourcePanel_.resetPage();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.hide();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        syncMediaPopupEnabled();
        cancelSceneWindowResize();
        if (mode == TaskbarViewMode::AppBar) {
            detachFromTaskbar();
            renderState_.setVisibilitySuppressed(false);
            stopPlacementTimer();
            setPlacementStatus(TaskbarPlacementStatus::Safe);
        } else {
            findTaskbar();
            if (!attachToTaskbar(hwnd))
                scheduleTaskbarAttachRetry();
        }
        if (!isStoppedMode() && resourceMonitoringEnabled())
            startResourceMonitoring();
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        // 沉浸模式仍需要完成首次真实探测，才能解除创建阶段的显示抑制；
        // 探测完成且窗口已可见后才停止避让定时器。否则重启时会一直隐藏。
        if (mode == TaskbarViewMode::Immersive && probeReady_ &&
            !renderState_.visibilitySuppressed())
            stopPlacementTimer();
        else if (mode != TaskbarViewMode::AppBar && !isStoppedMode())
            startPlacementTimer();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        adjustPosition();
        reconcileWindowVisibility();
        requestFrameAndFlush();
    }

    void setAppBarEdge(AppBarEdge edge) {
        if (appBarEdge_ == edge)
            return;
        appBarEdge_ = edge;
        if (isAppBarView() && appBar_.registered())
            updateAppBarPosition();
        requestInvalidation(RenderInvalidation::Layout);
        requestFrameAndFlush();
    }

    void setImmersiveBackgroundBlur(int percent) {
        const int nextBlur = std::clamp(percent, 0, 100);
        if (immersiveBackgroundBlurPct_ == nextBlur)
            return;
        immersiveBackgroundBlurPct_ = nextBlur;
        if (viewMode_ == TaskbarViewMode::Immersive) {
            dockBackdrop_.setBlurPercent(immersiveBackgroundBlurPct_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrame();
        }
    }

    void setDockBackgroundBlur(int percent) {
        const int nextBlur = std::clamp(percent, 0, 100);
        if (dockBackgroundBlurPct_ == nextBlur)
            return;
        dockBackgroundBlurPct_ = nextBlur;
        if (viewMode_ == TaskbarViewMode::AppBar) {
            dockBackdrop_.setBlurPercent(dockBackgroundBlurPct_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrame();
        }
    }

    void setImmersiveBackgroundAdjustment(int mode) {
        const int nextMode = std::clamp(mode, 0, 1);
        if (immersiveBackgroundAdjustment_ == nextMode)
            return;
        immersiveBackgroundAdjustment_ = nextMode;
        if (viewMode_ == TaskbarViewMode::Immersive) {
            updateBackdropSolidColor();
            dockBackdrop_.setAdjustmentMode(immersiveBackgroundAdjustment_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrameAndFlush();
        }
    }

    void setDockBackgroundAdjustment(int mode) {
        const int nextMode = std::clamp(mode, 0, 1);
        if (dockBackgroundAdjustment_ == nextMode)
            return;
        dockBackgroundAdjustment_ = nextMode;
        if (viewMode_ == TaskbarViewMode::AppBar) {
            updateBackdropSolidColor();
            dockBackdrop_.setAdjustmentMode(dockBackgroundAdjustment_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrameAndFlush();
        }
    }

    bool backgroundBlurAvailable() const { return backgroundBlurSupported_; }

    void updateBackdropSolidColor() {
        if (viewMode_ == TaskbarViewMode::Embedded)
            return;
        const COLORREF color =
            fluent::isWindowsAppDarkMode() ? RGB(32, 32, 32) : RGB(243, 243, 243);
        dockBackdrop_.setSolidColor(color);
        if (!dockBackdrop_.available())
            backgroundBlurSupported_ = false;
    }

    void initializeBackdropForCurrentViewMode() {
        if (viewMode_ == TaskbarViewMode::Embedded) {
            dockBackdrop_.setVisible(false);
            backgroundBlurSupported_ = true;
            return;
        }

        if (dockBackdrop_.available()) {
            backgroundBlurSupported_ = true;
            runtime_log::writef(L"[dock-backdrop] init result=reused view=%s hwnd=%p",
                                viewMode_ == TaskbarViewMode::Immersive ? L"immersive" : L"dock",
                                hwnd);
        } else {
            backgroundBlurSupported_ = dockBackdrop_.initialize(hwnd);
        }
        updateBackdropSolidColor();
        dockBackdrop_.setAdjustmentMode(viewMode_ == TaskbarViewMode::Immersive
                                            ? immersiveBackgroundAdjustment_
                                            : dockBackgroundAdjustment_);
        dockBackdrop_.setBlurPercent(viewMode_ == TaskbarViewMode::Immersive
                                         ? immersiveBackgroundBlurPct_
                                         : dockBackgroundBlurPct_);
        dockBackdrop_.setVisible(true);
        if (!dockBackdrop_.available())
            backgroundBlurSupported_ = false;
    }

    void setDockResourceVisibility(const DockResourceVisibility& visibility) {
        if (resourcePanel_.visibility().embeddedEnabled == visibility.embeddedEnabled &&
            resourcePanel_.visibility().download == visibility.download &&
            resourcePanel_.visibility().upload == visibility.upload &&
            resourcePanel_.visibility().gpuUsage == visibility.gpuUsage &&
            resourcePanel_.visibility().cpuFrequency == visibility.cpuFrequency)
            return;
        const bool embeddedChanged =
            resourcePanel_.visibility().embeddedEnabled != visibility.embeddedEnabled;
        resourcePanel_.setVisibility(visibility);
        updateResourceColumnWidths();
        if (embeddedChanged && viewMode_ == TaskbarViewMode::Embedded && hwnd) {
            if (visibility.embeddedEnabled && !isStoppedMode())
                startResourceMonitoring();
            else
                stopResourceMonitoring();
        }
        resourcePanel_.resetPage();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        if (isAppBarView() || viewMode_ == TaskbarViewMode::Embedded)
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumVisible(bool on) {
        if (spectrumVisible_ == on) {
            if (!on)
                spectrum_.clearBands();
            return;
        }
        spectrumVisible_ = on;
        resetSpectrumVisuals();
        if (!on)
            spectrum_.clearBands();
        // 先改窗口宽度再渲染：若在 render 内经 Layout 失效改大小，
        // render 结尾的 present 会用旧尺寸位图把窗口尺寸拽回去（与 setPositionMode 同序）
        adjustPosition();
        requestFrameAndFlush();
    }

    void setSongInfoVisible(bool on) {
        if (songInfoVisible_ == on)
            return;
        songInfoVisible_ = on;
        scroll_.resetTitle();
        scroll_.resetArtist();
        requestInvalidation(RenderInvalidation::SongInfo);
        adjustPosition();
        requestFrameAndFlush();
    }

    void setAlbumCoverVisible(bool on) {
        if (albumCoverVisible_ == on)
            return;
        albumCoverVisible_ = on;
        if (!on) {
            requestInvalidation(RenderInvalidation::PlatformIcon);
            if (platformIconBmp.Get()) {
                platformIconBmp.Reset();
            }
        } else if (platformIconVisible_) {
            requestInvalidation(RenderInvalidation::PlatformIcon);
        }
        requestInvalidation(RenderInvalidation::Text);
        adjustPosition();
        requestFrameAndFlush();
    }

    void setPlatformIconVisible(bool on) {
        if (platformIconVisible_ == on)
            return;
        platformIconVisible_ = on;
        requestInvalidation(RenderInvalidation::PlatformIcon);
        requestFrameAndFlush();
    }

    void setAlbumCoverEffect(AlbumCoverEffect effect) {
        if (albumCoverEffect_ == effect)
            return;
        albumCoverEffect_ = effect;
        vinyl_.resetDisc();
        vinyl_.resetClock(monotonicNowMs());
        requestInvalidation(RenderInvalidation::Geometry);
        if (effect == AlbumCoverEffect::Vinyl)
            createAlbumCoverBrushes();
        else
            releaseAlbumCoverEffectResources();
        updateVinylRotation();
        requestFrameAndFlush();
    }

    void setSpectrumBands(const std::array<float, TaskbarHost::kSpectrumBands>& bands) {
        spectrum_.setBands(bands);
    }

    void applyPlaybackPatch(const PlaybackPatch& patch) {
        // 高频补丁必须属于当前已经应用的完整帧；旧曲目或旧帧的位置不能回写。
        if (patch.frameRevision != frameRevision_ || patch.requestGeneration != requestGeneration_)
            return;

        syncLyricEffectClock(patch.actualPositionMs, patch.playing);
        if (patch.actualPositionMs != positionMs_) {
            positionMs_ = patch.actualPositionMs;
            if (!patch.playing)
                karaoke_.invalidateProgress(); // 暂停中 seek：逐字高亮需要重新收敛
        }
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setProgress(patch.actualPositionMs);
        const bool playingChanged = media.playing != patch.playing;
        if (playingChanged) {
            media.playing = patch.playing;
            vinyl_.resetClock(monotonicNowMs());
            updateVinylRotation();
            mediaPopup.setMedia(media, mediaPopupAvailable(isSessionVisible()));
            mediaPopup.setPresentationMode(scene_, mediaPopupAvailable(isSessionVisible()),
                                           hoverControlStyle_ == HoverControlStyle::Inline);
        }
        mediaPopup.endPresentationUpdate();
        if (playingChanged) {
            // 悬浮控制按钮的播放/暂停图标随状态变化，直接提交一帧；弹窗自身
            // 的完整展示帧已在上面的批量更新中提交。
            // 这里仅保留宿主歌词的刷新，不再重复触发弹窗绘制。
            requestFrameAndFlush();
        }

        if (patch.currentLine == currentLine)
            return;

        if (transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionInProgress()) {
            // 场景翻页期间不重排目标层，先记下最新歌词行，待翻页完成后
            // 统一提交，避免目标层重建把上下翻页打断成瞬移。
            currentLine = patch.currentLine;
            transition_.setNeedsRelayout(true);
            transition_.setRevision(frameRevision_);
            return;
        }

        onLyricLineTargetChanged(patch.currentLine, patch.actualPositionMs, frameRevision_, true);
    }

    void applySpectrumPatch(const SpectrumPatch& patch) {
        if (patch.frameRevision != frameRevision_ || patch.requestGeneration != requestGeneration_)
            return;
        spectrum_.setBands(patch.bands);
    }

    void applyPresentationFrame(const PresentationFrame& frame) {
        // 完整帧只允许按版本向前提交；高频播放补丁不改变这个版本边界。
        if (frame.frameRevision < frameRevision_ ||
            (frame.frameRevision == frameRevision_ && frame.frameRevision != 0 &&
             frame.trackKey != trackKey_))
            return;

        const bool trackChanged = frame.trackKey != trackKey_;
        syncLyricEffectClock(frame.actualPositionMs, frame.media.playing, trackChanged);
        if (trackChanged)
            resetSpectrumVisuals();
        const bool lyricsChanged = !sameLyrics(lines, frame.lyrics);
        const bool lineChanged = frame.currentLine != currentLine;
        const bool statusChanged = frame.statusText != statusText;
        const bool statusOneShotChanged = frame.statusTextOneShot != scroll_.oneShot();
        const DisplayScene previousScene = scene_;
        const bool sceneChanged = frame.scene != scene_;
        if (sceneChanged)
            resetSpectrumEffectHistory();
        const bool sceneWidthChanged = sceneWidthPolicyDiffers(scene_, frame.scene);
        const bool idleChanged =
            frame.idle.sentence != idle.sentence || frame.idle.source != idle.source ||
            frame.idle.loading != idle.loading || frame.idle.showText != idle.showText ||
            frame.idle.showQuote != idle.showQuote || frame.idle.copyEnabled != idle.copyEnabled ||
            frame.idle.quickStartEnabled != idle.quickStartEnabled ||
            frame.idle.apps.size() != idle.apps.size();
        const bool wasVisible = isWindowVisible();
        const bool shouldAnimateScene = isWindowVisible() && frame.visible && clientAnimations_ &&
                                        !isMinimalMode() && !isStoppedMode() &&
                                        isTaskbarSceneTransition(scene_, frame.scene) &&
                                        typesetter_.resources().lyricLayout_.Get();
        const bool continueSceneTransition =
            sceneChanged && transition_.kind() == LyricTransitionKind::Scene &&
            isLyricTransitionInProgress() && isTaskbarMediaScene(scene_) &&
            isTaskbarMediaScene(frame.scene);
        const bool mediaIdentityChanged =
            frame.media.title != media.title || frame.media.artist != media.artist ||
            frame.media.sourceAppUserModelId != media.sourceAppUserModelId ||
            frame.media.thumbnail != media.thumbnail;
        // QQ 的 trackKey 包含时长；同一首歌从未知时长补齐到有效时长时，
        // 即使封面、专辑字段也在同一批事件中更新，也不能启动整卡切歌动画。
        const bool durationOnlyMediaUpdate = trackChanged && frame.durationOnlyUpdate &&
                                             media.durationMs <= 0 && frame.media.durationMs > 0;
        const bool confirmedDurationChange = media.durationMs > 0 && frame.media.durationMs > 0 &&
                                             media.durationMs != frame.media.durationMs;
        const bool songChanged = trackChanged && !trackKey_.empty() && !frame.trackKey.empty() &&
                                 !durationOnlyMediaUpdate &&
                                 (mediaIdentityChanged || confirmedDurationChange);
        const bool mediaChanged = updateMediaInfo(frame.media);
        const bool popupAvailable = mediaPopupAvailable(frame.visible);
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setIdleContent(frame.idle, popupAvailable);
        mediaPopup.setMedia(frame.media, popupAvailable, songChanged);
        // 先同步完整媒体数据，再建立页面转场层，避免 Idle → Media 时目标层
        // 先绘制旧歌曲、随后才收到本帧最新标题/歌手而在转场结束时跳变。
        mediaPopup.setPresentationMode(frame.scene, popupAvailable,
                                       hoverControlStyle_ == HoverControlStyle::Inline);
        mediaPopup.setProgress(frame.actualPositionMs);
        mediaPopup.endPresentationUpdate();

        frameRevision_ = frame.frameRevision;
        requestGeneration_ = frame.requestGeneration;
        trackKey_ = frame.trackKey;
        if (sceneChanged) {
            // 类别切换拥有最高优先级：先结束可能正在进行的歌词行转场，
            // 再把旧场景布局交给新的内容块上下翻页。
            if (continueSceneTransition) {
                // Searching/Spectrum/Message 都属于播放侧内容。歌词加载完成时
                // 继续同一页翻转，不能把中间场景当成一次新的类别切换。
                transition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            } else {
                resetLyricTransition();
                if (shouldAnimateScene) {
                    transition_.setOutgoingScene(scene_);
                    transition_.setOutgoingDoubleLine(lyricLayoutDoubleLine_);
                    transition_.setFromWidth(lastLogicalPxW_);
                    transition_.setFromHeight(lastLogicalPxH_);
                    transition_.setKind(LyricTransitionKind::Scene);
                    transition_.setDirection(frame.scene == DisplayScene::Lyrics ? 1 : -1);
                    setLyricTransitionPending();
                    transition_.setRevision(frame.frameRevision);
                }
            }
        }
        const bool resourceOnlyChanged =
            embeddedResourceVisible() &&
            ((previousScene == DisplayScene::Idle && statusText.empty()) !=
             (frame.scene == DisplayScene::Idle && frame.statusText.empty()));
        scene_ = frame.scene;
        // 窗口目标宽度依赖空闲文案是否为空，必须用新帧的状态计算转场终点。
        statusText = frame.statusText;
        if (sceneWidthChanged) {
            if (shouldAnimateScene) {
                // 与内容转场使用同一段平滑时间曲线，窗口宽度/锚点连续变化，避免
                // 在转场开始或结束时一次性收窄/撑开并触发位图重绑闪烁。
                if (beginSceneWindowResize()) {
                    // 窗口尺寸动画接管本次布局失效；render() 在动画期间不再执行
                    // 一次性的 adjustPosition，成功提交后由失效快照统一确认。
                } else {
                    requestInvalidation(RenderInvalidation::Layout);
                }
            } else {
                cancelSceneWindowResize();
                requestInvalidation(RenderInvalidation::Layout);
            }
        }
        idle = frame.idle;
        if (frame.actualPositionMs != positionMs_ && !media.playing)
            karaoke_.invalidateProgress(); // 暂停中 seek：逐字高亮需要重新收敛
        positionMs_ = frame.actualPositionMs;
        if (statusOneShotChanged || (statusChanged && frame.statusTextOneShot)) {
            scroll_.setOneShot(frame.statusTextOneShot, monotonicNowMs());
            scroll_.resetLyric();
            scroll_.resetClock();
        }
        if (resourceOnlyChanged)
            requestInvalidation(RenderInvalidation::Layout);

        // 只对已有曲目之间的切换做入场动画；首次显示、同曲刷新和会话关闭保持即时提交。
        setSongTransitionPending(songChanged && frame.visible && !isMinimalMode() &&
                                 !isStoppedMode() && clientAnimations_);

        const bool sceneTransitionInProgress =
            transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionInProgress();
        if (trackChanged || lyricsChanged) {
            lines = frame.lyrics;
            currentLine = frame.currentLine;
            if (!sceneTransitionInProgress) {
                resetLyricTransition();
                typesetter_.clearNextContent();
                nextLyricWidth_ = 0.0f;
                nextLyricHeight_ = 0.0f;
                requestInvalidation(RenderInvalidation::Text);
            } else if (!sceneChanged || continueSceneTransition) {
                transition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            }
        } else if (lineChanged) {
            if (sceneTransitionInProgress) {
                currentLine = frame.currentLine;
                transition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            } else {
                onLyricLineTargetChanged(frame.currentLine, frame.actualPositionMs,
                                         frame.frameRevision, frame.animateTransition);
            }
        } else if (isLyricTransitionInProgress()) {
            // 同一目标行的低频媒体/场景更新不应打断动画，但动画版本要跟随最新完整帧。
            transition_.setRevision(frame.frameRevision);
        }
        if (statusChanged || statusOneShotChanged || sceneChanged) {
            if (sceneTransitionInProgress && (!sceneChanged || continueSceneTransition)) {
                transition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            } else {
                requestInvalidation(RenderInvalidation::Text);
            }
        }

        renderState_.setSessionVisible(frame.visible);
        if (isSessionVisible() && placementStatus_ == TaskbarPlacementStatus::NoSpace)
            startPlacementTimer();
        else if (!isSessionVisible())
            stopPlacementTimer();
        reconcileWindowVisibility();
        syncMediaPopupEnabled();

        if (isWindowVisible() &&
            (wasVisible != isWindowVisible() || trackChanged || mediaChanged || lyricsChanged ||
             lineChanged || statusChanged || statusOneShotChanged || sceneChanged || idleChanged))
            requestFrameAndFlush();
    }

    // 频谱簇总宽（含柱间间隙）
    float spectrumClusterW() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumClusterW();
    }

    float spectrumVisualClusterW(int barCount) const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumVisualClusterW(barCount);
    }

    bool horizontalImmersiveMode() const { return isExpandedView() && !isVerticalTaskbar(); }

    // 沉浸模式把频谱提升为右侧固定视觉区，长任务栏自动给它更多空间。
    float immersiveSpectrumZoneW(float hostW) const {
        if (!horizontalImmersiveMode() || hostW <= 0.0f)
            return 0.0f;
        return taskbar_detail::expandedSpectrumWidth(hostW);
    }

    float immersiveClockZoneW(float hostW) const {
        if (!horizontalImmersiveMode() || hostW <= 0.0f)
            return 0.0f;
        return taskbar_detail::expandedClockWidth(hostW);
    }

    bool dockResourceExtrasVisible() const { return dockResourcePageCount() > 1; }

    size_t dockResourcePageCount() const { return resourcePanel_.pageCount(); }

    float dockResourceZoneW(float hostW) const {
        if (!isAppBarView() || hostW <= 0.0f)
            return 0.0f;
        return std::min(resourceZoneWidth(), std::max(1.0f, hostW - kDockResourceRightPadding));
    }

    bool hitDockResourceArea(float clientX, float clientY) const {
        if ((!isAppBarView() && !embeddedResourceVisible()) || !dockResourceExtrasVisible())
            return false;

        float x = 0.0f;
        float y = 0.0f;
        clientPointToLogicalDip(clientX, clientY, x, y);
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const float width = dip(pxW);
        const float height = dip(pxH);
        const float resourceW =
            embeddedResourceVisible()
                ? std::min(resourceZoneWidth(), std::max(1.0f, width - kDockResourceRightPadding))
                : dockResourceZoneW(width);
        const float resourceX = width - resourceW - kDockResourceRightPadding;
        return x >= resourceX && x <= width - kDockResourceRightPadding && y >= 0.0f && y <= height;
    }

    void advanceDockResourcePage(int steps) {
        if (!resourcePanel_.advancePage(steps))
            return;
        requestInvalidation(toMask(RenderInvalidation::Paint) | toMask(RenderInvalidation::Text));
        requestFrameAndFlush();
    }

    int immersiveSpectrumBarCount(float visualW) const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.immersiveSpectrumBarCount(visualW);
    }

    float spectrumContentWForScene(DisplayScene scene, float hostW) const {
        if (scene == DisplayScene::Idle || !spectrumVisible_ || backgroundWaveEnabled() ||
            integratedSpectrum())
            return 0.0f;
        return horizontalImmersiveMode() ? immersiveSpectrumZoneW(hostW) : spectrumClusterW();
    }

    // 嵌入模式按开关预留独立频谱和资源区；沉浸模式始终保留右侧
    // “频谱 + 时钟/资源状态”固定区，避免开关频谱时歌词安全区发生变化。
    float spectrumExtraForScene(DisplayScene scene, float hostW = -1.0f) const {
        if (hostW <= 0.0f) {
            int pxW = 0;
            int pxH = 0;
            logicalClientPixelSize(pxW, pxH);
            hostW = dip(pxW);
        }
        // 沉浸模式的最右侧始终保留给频谱和时钟；关闭频谱只隐藏其可视内容，
        // 不让歌词安全区或居中效果跟着变化。
        if (horizontalImmersiveMode()) {
            const float rightPadding = isAppBarView() ? kDockResourceRightPadding : kTextPadding;
            float extra = immersiveSpectrumZoneW(hostW) + rightPadding;
            if (isAppBarView())
                extra += dockResourceZoneW(hostW) + kDockResourceZoneGap;
            return extra;
        }
        if (scene == DisplayScene::Idle || !spectrumVisible_ || backgroundWaveEnabled() ||
            integratedSpectrum())
            return embeddedResourceExtra();
        return spectrumContentWForScene(scene, hostW) + kTextPadding + embeddedResourceExtra();
    }

    bool sceneUsesCompactWidth(DisplayScene scene) const {
        return !isVerticalTaskbar() && !songInfoVisible_ && scene != DisplayScene::Idle;
    }

    bool sceneWidthPolicyDiffers(DisplayScene from, DisplayScene to) const {
        if (isVerticalTaskbar() || isExpandedView())
            return false;
        return sceneUsesCompactWidth(from) != sceneUsesCompactWidth(to) ||
               spectrumExtraForScene(from) != spectrumExtraForScene(to);
    }

    void cancelSceneWindowResize() { sceneResize_.reset(); }

    bool beginSceneWindowResize() {
        if (isExpandedView())
            return false;
        WindowPlacement from;
        WindowPlacement to;
        if (!currentWindowPlacement(from))
            return false;
        const TaskbarPlacementStatus placementStatus = calculateWindowPlacement(to);
        if (placementStatus == TaskbarPlacementStatus::Unavailable ||
            placementStatus == TaskbarPlacementStatus::NoSpace)
            return false;
        if (from.x == to.x && from.y == to.y && from.width == to.width &&
            from.height == to.height) {
            cancelSceneWindowResize();
            return false;
        }

        sceneResize_ = SceneResizeAnimation{from, to, from, monotonicNowMs()};
        return true;
    }

    bool updateSceneWindowResize(ULONGLONG now) {
        if (!sceneResize_)
            return false;
        SceneResizeAnimation& resize = *sceneResize_;
        if (now < resize.startMs)
            now = resize.startMs;

        const float progress =
            std::clamp(static_cast<float>(now - resize.startMs) / kSceneTransitionMs, 0.0f, 1.0f);
        const float t = smoothStep(progress);
        WindowPlacement placement;
        placement.x =
            static_cast<int>(std::lround(resize.from.x + (resize.to.x - resize.from.x) * t));
        placement.y =
            static_cast<int>(std::lround(resize.from.y + (resize.to.y - resize.from.y) * t));
        placement.width = static_cast<int>(
            std::lround(resize.from.width + (resize.to.width - resize.from.width) * t));
        placement.height = static_cast<int>(
            std::lround(resize.from.height + (resize.to.height - resize.from.height) * t));

        const bool changed = placement.x != resize.lastApplied.x ||
                             placement.y != resize.lastApplied.y ||
                             placement.width != resize.lastApplied.width ||
                             placement.height != resize.lastApplied.height;
        if (changed) {
            applyWindowPlacement(placement, false, true);
            resize.lastApplied = placement;
        }
        if (progress >= 1.0f) {
            sceneResize_.reset();
        }
        return true;
    }

    // 新增样式：普通柱状图，柱底贴近歌词窗口下边缘，电平只向上增长。

    void drawBottomSpectrum(float x, float h, float width) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.drawBottomSpectrum(x, h, width);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    // 填充波浪：以 baseY 为底边，width 可覆盖独立频谱区或歌曲内容背景区。

    void drawBackgroundWaveSpectrum(float x, float h, float width) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.drawBackgroundWaveSpectrum(x, h, width);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    // 默认保留原有的中线对称频谱效果；柱状图样式从窗口下边缘向上增长。
    // 沉浸模式中，柱状频谱在右侧宽频谱区内居中，梦幻波浪则铺满整个区域。
    void drawSpectrum(float x, float h, float width) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.drawSpectrum(x, h, width);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    bool refreshImmersiveClockText() {
        if (!horizontalImmersiveMode())
            return false;

        SYSTEMTIME now{};
        GetLocalTime(&now);
        const uint64_t dateKey = (static_cast<uint64_t>(now.wYear) * 10000ULL) +
                                 (static_cast<uint64_t>(now.wMonth) * 100ULL) + now.wDay;
        const uint64_t minuteKey =
            (dateKey * 10000ULL) + (static_cast<uint64_t>(now.wHour) * 100ULL) + now.wMinute;
        if (clockMinuteKey_ == minuteKey && !clockTimeText_.empty() && !clockDateText_.empty())
            return false;

        wchar_t timeText[64]{};
        wchar_t dateText[64]{};
        if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &now, nullptr, timeText,
                             static_cast<int>(_countof(timeText))))
            swprintf_s(timeText, L"%02u:%02u", now.wHour, now.wMinute);
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &now, nullptr, dateText,
                             static_cast<int>(_countof(dateText)), nullptr))
            swprintf_s(dateText, L"%04u/%02u/%02u", now.wYear, now.wMonth, now.wDay);

        const bool changed = clockTimeText_ != timeText || clockDateText_ != dateText;
        clockTimeText_ = timeText;
        clockDateText_ = dateText;
        clockMinuteKey_ = minuteKey;
        return changed;
    }

    void startResourceMonitoring() { resourcePanel_.start(); }

    void stopResourceMonitoring() { resourcePanel_.stop(); }

    bool refreshResourceSnapshot() {
        return resourceMonitoringEnabled() && resourcePanel_.refresh();
    }

    void drawImmersiveClock(float x, float h, float width) {
        auto* rt = renderer.renderTarget();
        if (!rt || width <= 0.0f || !typesetter_.resources().fmtClockTime_.Get() ||
            !typesetter_.resources().fmtClockDate_.Get() || !brushText_.Get() || !brushDim_.Get())
            return;
        typesetter_.resources().fmtClockTime_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        const float blockH = std::min(35.0f, std::max(1.0f, h - 2.0f));
        const float top = std::max(0.0f, (h - blockH) * 0.5f);
        const float timeH = blockH * 0.56f;
        rt->DrawTextW(clockTimeText_.c_str(), static_cast<UINT32>(clockTimeText_.size()),
                      typesetter_.resources().fmtClockTime_.Get(),
                      D2D1::RectF(x, top, x + width, top + timeH), brushText_.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        rt->DrawTextW(clockDateText_.c_str(), static_cast<UINT32>(clockDateText_.size()),
                      typesetter_.resources().fmtClockDate_.Get(),
                      D2D1::RectF(x, top + timeH, x + width, top + blockH), brushDim_.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void drawDockResourceStatus(float x, float h, float width) {
        if (resourceMonitoringEnabled() && ensureResourceFormat())
            resourcePanel_.draw(renderer.renderTarget(), typesetter_.resources().fmtResource_.Get(),
                                brushText_.Get(), x, h, width);
    }

    DockPetMode dockPetMode() const {
        if (!horizontalImmersiveMode() || !isSessionVisible() || isMinimalMode() || isStoppedMode())
            return DockPetMode::Hidden;
        if (scene_ == DisplayScene::Idle)
            return DockPetMode::Roaming;
        return media.playing ? DockPetMode::Listening : DockPetMode::Paused;
    }

    void drawDockPet(float w, float h, float leftW) {
        const ULONGLONG now = frameNowMs_ != 0 ? frameNowMs_ : monotonicNowMs();
        const DockPetMode mode = dockPetMode();
        if (mode == DockPetMode::Hidden) {
            dockPet_.setMode(mode, now);
            return;
        }

        // Dock 右侧为“频谱 + 资源区”，沉浸模式为“频谱 + 时钟”，统一按右侧固定区起点收束。
        const float spectrumX = w - spectrumExtraForScene(scene_, w);
        const float start =
            songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
        const float laneLeft = leftW + start + immersiveControlsWidth(h);
        const float laneRight = mode == DockPetMode::Roaming
                                    ? spectrumX - kTextPadding
                                    : std::min(spectrumX - kTextPadding, laneLeft + kDockPetSeatW);
        const float minLaneWidth = mode == DockPetMode::Roaming ? kPetLaneMinWidthDip : 28.0f;
        if (laneRight <= laneLeft + minLaneWidth) {
            dockPet_.setMode(DockPetMode::Hidden, now);
            return;
        }

        dockPet_.setLane(laneLeft, laneRight, h);
        dockPet_.setMode(mode, now);
        dockPet_.setLightTheme(lightTheme_);
        // 播放中切歌（曲目变化但始终处于 Listening）时补一次雀跃；
        // 暂停/恢复与开始播放的雀跃由 DockPet::setMode 内部处理。
        if (mode == DockPetMode::Listening) {
            const std::wstring track = media.title + L"|" + media.artist;
            if (track != dockPetTrack_) {
                if (!dockPetTrack_.empty())
                    dockPet_.hop(now);
                dockPetTrack_ = track;
            }
        }
        dockPet_.draw(drawTarget());
    }

    void drawImmersiveRightSide(float w, float h, bool showSpectrum) {
        if (!horizontalImmersiveMode())
            return;

        const bool dock = isAppBarView();
        const auto side = taskbar_detail::expandedSideLayout(w, dock, dockResourceZoneW(w));
        const float zoneW = side.zoneWidth;
        const float zoneX = side.zoneX;
        const float resourceW = side.resourceWidth;
        const float resourceX = side.resourceX;
        const float clockW = side.clockWidth;
        const float clockX = side.clockX;
        const float spectrumRight = side.spectrumRight;
        const float spectrumW = side.spectrumWidth;
        if (showSpectrum && spectrumW > 0.0f) {
            if (spectrumStyle_ == SpectrumStyle::DreamyWave) {
                drawSpectrum(zoneX, h, spectrumW);
            } else {
                // 先按原始频谱区确定柱数；Dock 贴右对齐，普通沉浸模式仍按原逻辑居中。
                const int availableCount =
                    std::max(1, static_cast<int>(std::floor((spectrumW + kSpectrumGap) /
                                                            (kSpectrumBarW + kSpectrumGap))));
                const int visualCount = std::min(immersiveSpectrumBarCount(zoneW), availableCount);
                const float clusterW = spectrumVisualClusterW(visualCount);
                const float naturalX = zoneX + std::max(0.0f, (zoneW - clusterW) * 0.5f);
                // Dock 模式将频谱簇贴到右边界，保证它与资源区之间只保留
                // kDockResourceZoneGap；普通沉浸模式继续保持原有居中位置。
                const float clusterX =
                    dock ? std::max(zoneX, spectrumRight - clusterW)
                         : std::max(zoneX, std::min(naturalX, spectrumRight - clusterW));
                spectrum_.setContext(spectrumContext(drawTarget()));
                spectrum_.drawCluster(clusterX, h, clusterW, visualCount);
                if (spectrum_.consumeRedraw())
                    requestFrame();
            }
        }
        if (dock)
            drawDockResourceStatus(resourceX, h, resourceW);
        else
            drawImmersiveClock(clockX, h, clockW);
    }

    float backgroundWaveRight(float w) const {
        if (!horizontalImmersiveMode())
            return w - kTextPadding - embeddedResourceExtra();
        if (isAppBarView()) {
            return w - dockResourceZoneW(w) - kDockResourceRightPadding - kDockResourceZoneGap;
        }
        return w - immersiveClockZoneW(w) - kImmersiveSpectrumClockGap - kTextPadding;
    }

    bool taskbarDynamicBackgroundVisible() const {
        if (scene_ == DisplayScene::NoPlayback ||
            idleQuoteBackground_ == IdleQuoteBackground::None || isMinimalMode())
            return false;

        switch (idleQuoteBackgroundScope_) {
        case IdleQuoteBackgroundScope::DailyQuote:
            // “每日一言”范围覆盖整个待机内容场景：关闭每日一言后
            // 显示的默认欢迎语同样属于待机内容，不随开关丢失动态背景。
            return scene_ == DisplayScene::Idle;
        case IdleQuoteBackgroundScope::Lyrics:
            return scene_ == DisplayScene::Lyrics;
        case IdleQuoteBackgroundScope::All:
            return scene_ == DisplayScene::Idle || scene_ == DisplayScene::Lyrics;
        case IdleQuoteBackgroundScope::None:
        default:
            return false;
        }
    }

    bool taskbarDynamicBackgroundAnimating() const {
        return taskbarDynamicBackgroundVisible() && clientAnimations_;
    }

    void drawIdleQuoteBackground(float w, float h, float contentW) {
        auto* rt = renderer.renderTarget();
        if (!rt || w <= 0.0f || h <= 0.0f || !taskbarDynamicBackgroundVisible())
            return;

        const float effectW = std::clamp(contentW, 1.0f, w);

        const float time = static_cast<float>(frameNowMs_ % 600000ULL) / 1000.0f;
        auto* warm = brushIdleWarm_.Get() ? brushIdleWarm_.Get() : brushText_.Get();
        auto* cool = brushIdleCool_.Get() ? brushIdleCool_.Get() : brushText_.Get();
        auto* accent = brushIdleAccent_.Get() ? brushIdleAccent_.Get() : brushText_.Get();
        if (!warm && !cool && !accent)
            return;

        auto drawWithOpacity = [](ID2D1SolidColorBrush* brush, float opacity, const auto& draw) {
            if (!brush)
                return;
            const float previous = brush->GetOpacity();
            brush->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
            draw();
            brush->SetOpacity(previous);
        };

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, effectW, h),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        switch (idleQuoteBackground_) {
        case IdleQuoteBackground::FallingLeaves: {
            struct Leaf {
                float x;
                float y;
                float size;
                float fallSpeed;
                float drift;
                float phase;
                float angle;
            };
            static constexpr Leaf leaves[] = {
                {0.03f, -0.22f, 3.6f, 9.4f, 0.12f, 0.4f, -24.0f},
                {0.10f, 0.46f, 3.0f, 7.2f, 0.16f, 2.1f, 18.0f},
                {0.17f, 0.06f, 4.3f, 12.0f, 0.10f, 4.0f, 42.0f},
                {0.24f, 0.78f, 2.8f, 8.6f, 0.14f, 1.2f, -12.0f},
                {0.31f, -0.04f, 3.8f, 10.8f, 0.17f, 3.5f, 28.0f},
                {0.38f, 0.30f, 3.1f, 7.8f, 0.11f, 5.1f, -36.0f},
                {0.45f, 0.93f, 4.6f, 11.2f, 0.15f, 0.9f, 12.0f},
                {0.52f, 0.14f, 2.9f, 8.2f, 0.18f, 2.8f, -48.0f},
                {0.59f, 0.61f, 3.7f, 10.0f, 0.12f, 4.6f, 34.0f},
                {0.66f, -0.28f, 3.2f, 7.0f, 0.16f, 1.6f, -8.0f},
                {0.73f, 0.38f, 4.1f, 12.6f, 0.10f, 3.9f, 52.0f},
                {0.80f, 0.84f, 2.7f, 8.8f, 0.14f, 5.4f, -30.0f},
                {0.87f, 0.18f, 3.5f, 9.8f, 0.17f, 2.6f, 20.0f},
                {0.95f, 0.56f, 4.4f, 11.8f, 0.11f, 0.7f, -42.0f},
            };
            const float loopH = h + 16.0f;
            for (size_t i = 0; i < _countof(leaves); ++i) {
                float y = std::fmod(leaves[i].y * h + time * leaves[i].fallSpeed, loopH);
                if (y < 0.0f)
                    y += loopH;
                y -= 8.0f;
                float x = leaves[i].x * effectW +
                          std::sin(time * 0.62f + leaves[i].phase) * leaves[i].drift * effectW;
                x = std::fmod(x + effectW, effectW);
                if (x < 0.0f)
                    x += effectW;
                const D2D1_POINT_2F center = D2D1::Point2F(x, y);
                const float angle =
                    leaves[i].angle + std::sin(time * 0.88f + leaves[i].phase) * 24.0f;
                drawWithOpacity(warm, 0.22f + (i % 4) * 0.045f, [&] {
                    D2D1_MATRIX_3X2_F previous{};
                    rt->GetTransform(&previous);
                    rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle, center));
                    rt->FillEllipse(D2D1::Ellipse(center, leaves[i].size, leaves[i].size * 0.46f),
                                    warm);
                    rt->DrawLine(D2D1::Point2F(x - leaves[i].size * 0.52f, y),
                                 D2D1::Point2F(x + leaves[i].size * 0.52f, y), warm, 0.70f);
                    rt->SetTransform(previous);
                });
            }
            break;
        }
        case IdleQuoteBackground::TwinklingStars: {
            struct Star {
                float x;
                float y;
                float radius;
                float phase;
                float speed;
            };
            static constexpr Star stars[] = {
                {0.03f, 0.18f, 0.92f, 0.2f, 1.25f}, {0.07f, 0.68f, 0.78f, 2.4f, 0.92f},
                {0.12f, 0.42f, 1.18f, 4.1f, 1.46f}, {0.16f, 0.86f, 0.84f, 1.1f, 0.78f},
                {0.21f, 0.08f, 0.76f, 3.2f, 1.12f}, {0.25f, 0.56f, 1.06f, 5.0f, 1.36f},
                {0.30f, 0.30f, 0.88f, 0.6f, 0.86f}, {0.34f, 0.76f, 0.74f, 2.9f, 1.58f},
                {0.39f, 0.16f, 1.10f, 4.7f, 1.04f}, {0.43f, 0.92f, 0.82f, 1.8f, 1.32f},
                {0.48f, 0.46f, 0.96f, 3.7f, 0.74f}, {0.52f, 0.12f, 0.76f, 5.6f, 1.18f},
                {0.56f, 0.70f, 1.24f, 1.5f, 1.42f}, {0.60f, 0.34f, 0.80f, 3.0f, 0.88f},
                {0.64f, 0.84f, 1.02f, 4.4f, 1.24f}, {0.68f, 0.22f, 0.72f, 0.9f, 1.60f},
                {0.72f, 0.52f, 1.14f, 2.1f, 1.02f}, {0.76f, 0.06f, 0.86f, 5.2f, 1.38f},
                {0.80f, 0.76f, 1.00f, 3.4f, 0.80f}, {0.84f, 0.38f, 0.78f, 1.7f, 1.50f},
                {0.88f, 0.16f, 1.20f, 4.8f, 1.14f}, {0.92f, 0.62f, 0.82f, 2.6f, 0.96f},
                {0.96f, 0.30f, 1.08f, 0.7f, 1.28f}, {0.985f, 0.90f, 0.74f, 5.8f, 0.72f},
            };
            for (size_t i = 0; i < _countof(stars); ++i) {
                const float twinkle =
                    0.5f + 0.5f * std::sin(time * stars[i].speed + stars[i].phase);
                const D2D1_POINT_2F center = D2D1::Point2F(stars[i].x * effectW, stars[i].y * h);
                const float radius = stars[i].radius * (0.72f + twinkle * 0.52f);
                drawWithOpacity(cool, 0.14f + twinkle * 0.30f, [&] {
                    rt->FillEllipse(D2D1::Ellipse(center, radius, radius), cool);
                    if (i % 4 == 0 && twinkle > 0.52f) {
                        const float ray = radius * (2.4f + twinkle * 1.5f);
                        rt->DrawLine(D2D1::Point2F(center.x - ray, center.y),
                                     D2D1::Point2F(center.x + ray, center.y), cool, 0.70f);
                        rt->DrawLine(D2D1::Point2F(center.x, center.y - ray),
                                     D2D1::Point2F(center.x, center.y + ray), cool, 0.70f);
                    }
                });
            }
            break;
        }
        case IdleQuoteBackground::BinaryRain: {
            if (typesetter_.resources().fmtSecondary_.Get()) {
                const int columns = std::clamp(static_cast<int>(effectW / 23.0f), 5, 10);
                const float columnW = effectW / static_cast<float>(columns);
                const float rowGap = std::max(9.0f, h / 3.6f);
                const ULONGLONG bitTick = frameNowMs_ / 190ULL;
                for (int col = 0; col < columns; ++col) {
                    const float head =
                        std::fmod(time * (7.0f + static_cast<float>(col % 3) * 2.0f) +
                                      static_cast<float>(col) * 8.0f,
                                  h + rowGap * 4.0f) -
                        rowGap * 2.0f;
                    for (int row = -1; row < 5; ++row) {
                        const float y = head + static_cast<float>(row) * rowGap;
                        if (y < -rowGap || y > h)
                            continue;
                        const bool one = ((bitTick + static_cast<ULONGLONG>(col * 13 + row * 7 +
                                                                            col * row * 3)) &
                                          1ULL) != 0;
                        wchar_t digit[2] = {one ? L'1' : L'0', L'\0'};
                        const float trail =
                            std::clamp(1.0f - std::fabs(y - head) / (rowGap * 3.5f), 0.0f, 1.0f);
                        const float opacity = 0.045f + trail * 0.16f;
                        const D2D1_RECT_F rect = D2D1::RectF(
                            columnW * (static_cast<float>(col) + 0.5f) - 4.0f, y,
                            columnW * (static_cast<float>(col) + 0.5f) + 4.0f, y + rowGap + 2.0f);
                        drawWithOpacity(accent, opacity, [&] {
                            rt->DrawTextW(digit, 1, typesetter_.resources().fmtSecondary_.Get(),
                                          rect, accent, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                          DWRITE_MEASURING_MODE_NATURAL);
                        });
                    }
                }
            }
            break;
        }
        case IdleQuoteBackground::FloatingParticles: {
            struct Particle {
                float x;
                float y;
                float radius;
                float speed;
                float amplitude;
                float phase;
            };
            static constexpr Particle particles[] = {
                {0.02f, 0.28f, 1.18f, 4.0f, 0.18f, 0.4f}, {0.07f, 0.74f, 0.92f, 5.5f, 0.14f, 2.3f},
                {0.12f, 0.48f, 1.48f, 2.8f, 0.20f, 4.2f}, {0.17f, 0.12f, 1.00f, 4.8f, 0.13f, 1.2f},
                {0.22f, 0.86f, 1.26f, 3.3f, 0.17f, 3.5f}, {0.27f, 0.38f, 0.86f, 6.2f, 0.22f, 5.0f},
                {0.32f, 0.66f, 1.38f, 3.7f, 0.16f, 0.9f}, {0.37f, 0.22f, 0.96f, 5.2f, 0.19f, 2.8f},
                {0.42f, 0.80f, 1.10f, 3.1f, 0.14f, 4.8f}, {0.47f, 0.52f, 1.56f, 4.6f, 0.18f, 1.7f},
                {0.52f, 0.08f, 0.90f, 5.8f, 0.21f, 3.9f}, {0.57f, 0.92f, 1.32f, 2.6f, 0.15f, 5.4f},
                {0.62f, 0.34f, 1.16f, 4.3f, 0.20f, 1.5f}, {0.67f, 0.70f, 0.88f, 6.0f, 0.13f, 3.1f},
                {0.72f, 0.18f, 1.44f, 3.4f, 0.17f, 4.5f}, {0.77f, 0.84f, 1.02f, 4.9f, 0.22f, 0.7f},
                {0.82f, 0.44f, 1.30f, 2.9f, 0.16f, 2.0f}, {0.87f, 0.06f, 0.94f, 5.6f, 0.19f, 3.6f},
                {0.92f, 0.62f, 1.50f, 3.9f, 0.14f, 5.1f}, {0.97f, 0.30f, 1.08f, 4.5f, 0.20f, 1.0f},
            };
            for (size_t i = 0; i < _countof(particles); ++i) {
                float x = std::fmod(particles[i].x * effectW + time * particles[i].speed, effectW);
                if (x < 0.0f)
                    x += effectW;
                const float y = particles[i].y * h + std::sin(time * 0.72f + particles[i].phase) *
                                                         particles[i].amplitude * h;
                const float pulse =
                    0.5f + 0.5f * std::sin(time * (1.1f + i * 0.04f) + particles[i].phase);
                auto* brush = i % 3 == 0 ? accent : cool;
                drawWithOpacity(brush, 0.15f + pulse * 0.26f, [&] {
                    const float radius = particles[i].radius * (0.72f + pulse * 0.48f);
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush);
                    if (i % 2 == 0) {
                        rt->DrawLine(D2D1::Point2F(x - particles[i].speed * 0.7f, y),
                                     D2D1::Point2F(x - radius, y), brush, 0.65f);
                    }
                });
            }
            break;
        }
        case IdleQuoteBackground::None:
        default:
            break;
        }
        rt->PopAxisAlignedClip();
    }

    void drawVinylCover(float coverX, float coverY, float s) {
        auto* rt = drawTarget();
        if (!rt || s <= 0.0f)
            return;

        // 使用任务栏内容主题，与普通设置窗口的主题来源独立。
        const auto deckFill =
            lightTheme_ ? D2D1::ColorF(0.97f, 0.975f, 0.98f) : D2D1::ColorF(0.07f, 0.08f, 0.09f);
        const auto deckStroke =
            lightTheme_ ? D2D1::ColorF(0.77f, 0.79f, 0.81f) : D2D1::ColorF(0.27f, 0.28f, 0.30f);
        const auto platterRim =
            lightTheme_ ? D2D1::ColorF(0.82f, 0.84f, 0.86f) : D2D1::ColorF(0.37f, 0.39f, 0.42f);
        const auto armShadow = lightTheme_ ? D2D1::ColorF(0.45f, 0.47f, 0.49f, 0.60f)
                                           : D2D1::ColorF(0.02f, 0.025f, 0.03f);
        const auto metal =
            lightTheme_ ? D2D1::ColorF(0.86f, 0.88f, 0.90f) : D2D1::ColorF(0.65f, 0.67f, 0.69f);
        const auto pivotFill = lightTheme_ ? D2D1::ColorF(0.99f, 0.99f, 0.99f) : metal;
        const auto cartridge =
            lightTheme_ ? D2D1::ColorF(0.95f, 0.96f, 0.97f) : D2D1::ColorF(0.12f, 0.13f, 0.15f);

        D2D1_POINT_2F center = D2D1::Point2F(coverX + s * 0.5f, coverY + s * 0.5f);
        float outerRadius = std::max(1.0f, s * kVinylOuterRatio);
        float haloWidth = std::min(kVinylHaloWidth, outerRadius * 0.25f);
        float recordRadius = std::max(0.5f, outerRadius - haloWidth);
        float innerRadius = vinylInnerRadius(s);
        D2D1_ELLIPSE outer{center, outerRadius, outerRadius};
        D2D1_ELLIPSE record{center, recordRadius, recordRadius};
        D2D1_ELLIPSE inner{center, innerRadius, innerRadius};

        // 固定唱机底座始终位于原封面槽内，不随盘面旋转，也不挤占歌曲文字。
        if (brushTurntable_.Get()) {
            const auto deck =
                D2D1::RoundedRect(D2D1::RectF(coverX, coverY, coverX + s, coverY + s), 4.0f, 4.0f);
            brushTurntable_->SetColor(deckFill);
            rt->FillRoundedRectangle(deck, brushTurntable_.Get());
            brushTurntable_->SetColor(deckStroke);
            rt->DrawRoundedRectangle(deck, brushTurntable_.Get(), 0.6f);
        }

        // 光照固定在唱机上，盘面和封面在光照下旋转。
        D2D1_MATRIX_3X2_F previous{};
        rt->GetTransform(&previous);
        if (brushTurntable_.Get()) {
            brushTurntable_->SetColor(platterRim);
            rt->FillEllipse(outer, brushTurntable_.Get());
        }
        if (brushVinylBase_.Get()) {
            brushVinylBase_->SetColor(media.hasDominantColor
                                          ? fluent::toD2D(media.dominantColor)
                                          : (lightTheme_ ? D2D1::ColorF(0.72f, 0.73f, 0.74f)
                                                         : D2D1::ColorF(0.055f, 0.060f, 0.068f)));
            rt->FillEllipse(record, brushVinylBase_.Get());
        }
        if (brushVinylSurface_.Get()) {
            brushVinylSurface_->SetStartPoint(
                D2D1::Point2F(center.x - recordRadius * 0.8f, center.y - recordRadius * 0.6f));
            brushVinylSurface_->SetEndPoint(
                D2D1::Point2F(center.x + recordRadius * 0.8f, center.y + recordRadius * 0.6f));
            rt->FillEllipse(record, brushVinylSurface_.Get());
        }
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(vinyl_.angle(), center) * previous);

        if (brushVinylGroove_.Get()) {
            brushVinylGroove_->SetColor(lightTheme_ ? D2D1::ColorF(0.25f, 0.27f, 0.29f, 0.20f)
                                                    : D2D1::ColorF(0.70f, 0.72f, 0.75f, 0.16f));
            // 细窄的低对比唱纹，避免小尺寸下粗同心圆看起来像一圈圈边框。
            for (float radius = innerRadius + 0.9f; radius < recordRadius - 0.4f; radius += 1.1f) {
                rt->DrawEllipse(D2D1_ELLIPSE{center, radius, radius}, brushVinylGroove_.Get(),
                                0.35f);
            }
        }

        D2D1_RECT_F innerRect = D2D1::RectF(center.x - innerRadius, center.y - innerRadius,
                                            center.x + innerRadius, center.y + innerRadius);
        if (cover_.bitmap() && vinylCoverClip_.Get() && coverLayer_.Get()) {
            rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), vinylCoverClip_.Get(),
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Translation(coverX, coverY)),
                          coverLayer_.Get());
            rt->DrawBitmap(cover_.bitmap(), innerRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
        } else if (brushDim_.Get()) {
            rt->FillEllipse(inner, brushDim_.Get());
        }
        if (brushVinylGroove_.Get())
            rt->DrawEllipse(inner, brushVinylGroove_.Get(), 0.8f);
        if (brushVinylBase_.Get()) {
            brushVinylBase_->SetColor(lightTheme_ ? D2D1::ColorF(0.78f, 0.80f, 0.82f)
                                                  : D2D1::ColorF(0.035f, 0.04f, 0.05f));
            rt->FillEllipse(
                D2D1_ELLIPSE{center, std::max(0.8f, s * 0.025f), std::max(0.8f, s * 0.025f)},
                brushVinylBase_.Get());
        }
        rt->SetTransform(previous);

        // 唱臂绕固定支点收放，盘面旋转与唱臂动作分别计时。
        if (brushTurntable_.Get()) {
            const auto point = [&](float x, float y) {
                return D2D1::Point2F(coverX + s * x, coverY + s * y);
            };
            const auto pivot = point(0.85f, 0.17f);
            const auto elbow = point(0.81f, 0.55f);
            const auto stylus = point(0.73f, 0.75f);
            rt->SetTransform(D2D1::Matrix3x2F::Rotation(vinyl_.armAngle(), pivot) * previous);
            brushTurntable_->SetColor(armShadow);
            rt->FillEllipse(D2D1::Ellipse(pivot, s * 0.075f, s * 0.075f), brushTurntable_.Get());
            rt->DrawLine(pivot, elbow, brushTurntable_.Get(), std::max(1.2f, s * 0.045f));
            rt->DrawLine(elbow, stylus, brushTurntable_.Get(), std::max(1.2f, s * 0.045f));
            brushTurntable_->SetColor(metal);
            rt->DrawLine(pivot, elbow, brushTurntable_.Get(), std::max(0.65f, s * 0.023f));
            rt->DrawLine(elbow, stylus, brushTurntable_.Get(), std::max(0.65f, s * 0.023f));
            brushTurntable_->SetColor(pivotFill);
            rt->FillEllipse(D2D1::Ellipse(pivot, s * 0.035f, s * 0.035f), brushTurntable_.Get());
            brushTurntable_->SetColor(cartridge);
            rt->DrawLine(point(0.75f, 0.70f), point(0.71f, 0.79f), brushTurntable_.Get(),
                         std::max(1.5f, s * 0.07f));
            rt->SetTransform(previous);
        }
    }

    void drawPlatformIcon(float coverX, float coverY, float s) {
        auto* rt = drawTarget();
        if (!rt || !albumCoverVisible_ || !platformIconVisible_ || !platformIconBmp.Get() ||
            s <= 0.0f)
            return;

        const float iconSize = std::max(1.0f, std::min(s * 0.50f, 13.0f));
        const float inset = std::min(1.5f, s * 0.06f);
        D2D1_RECT_F iconRect =
            D2D1::RectF(coverX + s - iconSize - inset, coverY + s - iconSize - inset,
                        coverX + s - inset, coverY + s - inset);
        rt->DrawBitmap(platformIconBmp.Get(), iconRect, 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void setPositionMode(int mode) {
        mode = mode == 1 ? 1 : 0;
        if (positionMode_ == mode)
            return;
        positionMode_ = mode;
        adjustPosition();
        requestFrameAndFlush();
    }

    // Explorer 重启后由托盘主窗口调用（TaskbarCreated 广播只发给顶层窗口）。
    // 作为 Shell_TrayWnd 的子窗口，歌词窗在 explorer 退出时会随任务栏一起被销毁，
    // 此时只剩野句柄，必须整体重建；若幸存（崩溃非正常退出）则重新附着即可。
    // 歌词/媒体/字体等状态都保存在 Impl 成员里，重建窗口后原样恢复
    void onTaskbarCreated() {
        runtime_log::writef(L"[taskbar] TaskbarCreated: hwnd=%p alive=%d visible=%d timer=%d", hwnd,
                            (hwnd && IsWindow(hwnd)) ? 1 : 0, isWindowVisible() ? 1 : 0,
                            timerRunning_ ? 1 : 0);
        if (hwnd && IsWindow(hwnd)) {
            if (findTaskbar()) {
                const bool immersiveBackdropPrepared =
                    viewMode_ == TaskbarViewMode::Immersive && !dockBackdrop_.available();
                if (immersiveBackdropPrepared) {
                    if (!attachment_.embedded() || detachFromTaskbar())
                        initializeBackdropForCurrentViewMode();
                    else
                        backgroundBlurSupported_ = false;
                }
                if (isAppBarView()) {
                    // Explorer 重启后 Shell 的 AppBar 注册表已丢失，本地标记不再可信。
                    appBar_.forget();
                    cancelTaskbarAttachRetry();
                    if (isSessionVisible() && !isStoppedMode())
                        registerAppBar();
                } else if (!attachToTaskbar(hwnd)) {
                    scheduleTaskbarAttachRetry();
                } else {
                    cancelTaskbarAttachRetry();
                }
                if (!immersiveBackdropPrepared)
                    initializeBackdropForCurrentViewMode();
                adjustPosition();
                reconcileWindowVisibility();
                requestFrameAndFlush();
            } else {
                attachment_.reset();
                if (!isAppBarView())
                    scheduleTaskbarAttachRetry();
            }
            return;
        }
        // 旧窗口未收到 WM_DESTROY 时也要停止旧探测，避免迟到结果进入新窗口。
        stopProbe();
        dockBackdrop_.reset();
        hwnd = nullptr;
        resetImmersivePresence();
        attachment_.reset();
        // Explorer 强制退出时旧窗口可能没有走 WM_DESTROY，Shell 侧与本地的
        // AppBar 注册状态都必须按已失效处理，避免重建窗口跳过 ABM_NEW。
        appBar_.forget();
        // 旧窗口被系统侧销毁时不一定投递 WM_DESTROY（Explorer 被强杀），
        // timerRunning_ 可能残留为 true，但定时器已随旧窗口消失；重建时同样
        // 等待新任务栏的首个探测结果，避免按旧矩形先显示一帧。
        timerRunning_ = false;
        timerMs_ = 0;
        renderState_.setVisibilitySuppressed(true);
        renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
        probeReady_ = false;
        placementProbe_.clearResult();
        if (createWindow(inst, targetMonitorDeviceName_, viewMode_)) {
            if (isAppBarView()) {
                renderState_.setVisibilitySuppressed(false);
                setPlacementStatus(TaskbarPlacementStatus::Safe);
            }
            reconcileWindowVisibility();
            if (isWindowVisible())
                requestFrameAndFlush();
        }
    }

    void recreateClockFormats() {
        typesetter_.setFactory(renderer.dwrite());
        typesetter_.recreateClockFormats(
            fontFamily_, fontStyle_,
            fitCompactFontSize(13.5f, (compactTaskbarHeight_ - 2.0f) * 0.56f,
                               DWRITE_FONT_WEIGHT_SEMI_BOLD),
            fitCompactFontSize(11.0f, (compactTaskbarHeight_ - 2.0f) * 0.44f));
    }

    bool ensureResourceFormat() {
        if (!renderer.dwrite())
            renderer.initialize();
        typesetter_.setFactory(renderer.dwrite());
        return typesetter_.ensureResourceFormat(
            fontFamily_, fontStyle_,
            fitCompactFontSize(kResourceFontSize, compactTaskbarHeight_ - 2.0f));
    }

    bool updateResourceColumnWidths() {
        return ensureResourceFormat() &&
               resourcePanel_.updateColumnWidths(renderer.dwrite(),
                                                 typesetter_.resources().fmtResource_.Get());
    }

    void recreateResourceFormat() {
        typesetter_.resetResourceFormat();
        if (updateResourceColumnWidths())
            requestInvalidation(RenderInvalidation::Layout);
    }

    float fitCompactFontSize(float requested, float budget,
                             DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
        if (compactTaskbarHeight_ <= 0.0f)
            return requested;
        auto* dwrite = renderer.dwrite();
        if (!dwrite)
            return requested;
        IDWriteTextFormat* format = nullptr;
        if (FAILED(dwrite->CreateTextFormat(fontFamily_.c_str(), nullptr,
                                            isBoldFontStyle(fontStyle_) ? DWRITE_FONT_WEIGHT_BOLD
                                                                        : weight,
                                            dwriteStyleOf(fontStyle_), DWRITE_FONT_STRETCH_NORMAL,
                                            requested, L"zh-cn", &format)))
            return requested;
        fluent::applyUiFontFallback(format);
        IDWriteTextLayout* sample = nullptr;
        float fitted = requested;
        if (SUCCEEDED(dwrite->CreateTextLayout(L"永Ag", 3, format, 4096.0f, 4096.0f, &sample))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(sample->GetMetrics(&metrics)) && metrics.height > 0.0f)
                fitted *= std::min(1.0f, std::max(1.0f, budget) / metrics.height);
            sample->Release();
        }
        format->Release();
        return fitted;
    }

    void syncCompactTaskbarLayout(int heightPixels) {
        const float height = dip(heightPixels);
        const float taskbarHeight = dip(rcTaskbar_.bottom - rcTaskbar_.top);
        const float compactHeight =
            !isVerticalTaskbar() && !isAppBarView() && taskbarHeight > 0.0f &&
                    taskbarHeight < kCompactTaskbarHeightDip && height > 0.0f
                ? height
                : 0.0f;
        if (compactHeight == compactTaskbarHeight_)
            return;
        compactTaskbarHeight_ = compactHeight;
        resourcePanel_.setCompact(compactHeight > 0.0f);
        resetLyricTransition();
        clearImmersiveSongContentTransition();
        releaseSpectrumEffectResources();
        recreateFormats();
        requestInvalidation(
            toMask(RenderInvalidation::Text) | toMask(RenderInvalidation::SongInfo) |
            toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Geometry) |
            toMask(RenderInvalidation::Cover));
    }

    void recreateFormats() {
        if (applyingAppearance_) {
            appearanceFormatsPending_ = true;
            return;
        }
        recreateResourceFormat();
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite)
            return;
        recreateClockFormats();
        const float lyricPad = lyricGlow_ ? 6.0f : lyricOutline_ ? 4.0f : 2.0f;
        LyricFontSizes sizes;
        sizes.title = fitCompactFontSize(kBaseFontSize * 1.05f,
                                         (compactTaskbarHeight_ - 6.0f) * 1.05f / 1.97f,
                                         DWRITE_FONT_WEIGHT_SEMI_BOLD);
        sizes.artist = fitCompactFontSize(kBaseFontSize * 0.92f,
                                          (compactTaskbarHeight_ - 6.0f) * 0.92f / 1.97f);
        sizes.lyric =
            fitCompactFontSize(fontSize_ * kLyricMainFontScale, compactTaskbarHeight_ - lyricPad);
        sizes.next = fontSize_ * kLyricPreviewFontScale;
        sizes.secondary = fontSize_ * 0.78f;
        typesetter_.setFactory(dwrite);
        typesetter_.recreateLyricFormats(fontFamily_, fontStyle_, sizes);
        requestInvalidation(RenderInvalidation::Text);
        requestInvalidation(RenderInvalidation::SongInfo);
    }

    void changeFont(float delta) { setFont(fontFamily_, fontSize_ + delta, fontStyle_); }

    LyricFontSizeContext fontSizeContext() const {
        LyricFontSizeContext context;
        context.minSize = kMinFont;
        context.maxSize = kMaxFont;
        RECT client{};
        if (!hwnd || !GetClientRect(hwnd, &client))
            return context;
        const bool vertical = isVerticalTaskbar();
        const float crossAxis =
            static_cast<float>(vertical ? client.right - client.left : client.bottom - client.top) /
            scale();
        const float effectPad = lyricGlow_ ? 4.8f : lyricOutline_ ? 2.4f : 0.0f;
        context.availableDip = std::max(0.0f, crossAxis - 4.0f - effectPad);
        context.mainScale = kLyricMainFontScale;
        if (compactTaskbarHeight_ > 0.0f && typesetter_.resources().fmtLyric_.Get() &&
            fontSize_ > 0.0f)
            context.mainScale = typesetter_.resources().fmtLyric_->GetFontSize() / fontSize_;
        if (!vertical && compactTaskbarHeight_ <= 0.0f && doubleLineLyricsEnabled_) {
            context.extraScale = kLyricPreviewFontScale;
            context.gapDip = kLyricPreviewGap;
        } else if (!vertical && compactTaskbarHeight_ <= 0.0f &&
                   (translationEnabled_ || romanizationEnabled_)) {
            context.extraScale = 0.78f;
            context.gapDip = 1.0f;
        }
        context.star = lyricStarEnabled(lyricEffectLayoutLine_);
        return context;
    }

    void setFont(const std::wstring& family, float size, LyricFontStyle style) {
        fontFamily_ = family;
        fontSize_ = std::clamp(size, kMinFont, kMaxFont);
        fontStyle_ = style;
        recreateFormats();
        requestInvalidation(RenderInvalidation::Layout);
        if (hwnd && (isAppBarView() || embeddedResourceVisible()))
            adjustPosition();
        requestFrameAndFlush();
    }

    void releaseDeviceResources() {
        lyricEffectsRenderer_.discardDeviceResources();
        spectrum_.discardDeviceResources();
        cancelSceneWindowResize();
        typesetter_.discardDeviceResources();
        brushBg_.Reset();
        brushHover_.Reset();
        brushText_.Reset();
        brushDim_.Reset();
        brushBtn_.Reset();
        brushBtnDisabled_.Reset();
        brushLyric_.Reset();
        brushLyricDim_.Reset();
        brushLyricGlow_.Reset();
        brushLyricOutline_.Reset();
        brushVinylBase_.Reset();
        brushVinylGroove_.Reset();
        brushVinylSurface_.Reset();
        brushTurntable_.Reset();
        brushProgressBg_.Reset();
        brushBackground_.Reset();
        brushIdleWarm_.Reset();
        brushIdleCool_.Reset();
        brushIdleAccent_.Reset();
        dockPet_.discardDeviceResources();
        dragPreviewStroke_.Reset();
        cover_.discardDeviceResources();
        lyricTextRenderer_.discardDeviceResources();
        karaoke_.clearGeometry();
        coverClip_.Reset();
        vinylCoverClip_.Reset();
        coverLayer_.Reset();
        songInfoDividerBrush_.Reset();
        media_control::release(controlGeometry);
        if (platformIconBmp.Get()) {
            platformIconBmp.Reset();
        }
        drawTargetOverride_ = nullptr;
        renderState_.setDeviceResourcePhase(RenderState::DeviceResourcePhase::Uninitialized);
        songContentTransition_.reset();
        clearLyricDCompState();
        requestInvalidation(
            toMask(RenderInvalidation::Text) | toMask(RenderInvalidation::SongInfo) |
            toMask(RenderInvalidation::Geometry) | toMask(RenderInvalidation::Layout) |
            toMask(RenderInvalidation::Cover) | toMask(RenderInvalidation::PlatformIcon));
    }

    void discardDeviceResources() {
        releaseDeviceResources();
        renderer.discard();
        deviceGeneration_ = 0;
    }

    bool bindRenderer(int width, int height) {
        if (!renderer.bind(hwnd, width, height))
            return false;
        if (deviceGeneration_ != renderer.deviceGeneration()) {
            // 其他窗口也可能使共享设备失效，不能只依赖本窗口的 Present 失败路径。
            if (deviceGeneration_ != 0)
                releaseDeviceResources();
            deviceGeneration_ = renderer.deviceGeneration();
            if (viewMode_ == TaskbarViewMode::Immersive) {
                const ULONGLONG now = GetTickCount64();
                const float opacity = immersivePresenceOpacity(now);
                renderer.setRootOpacity(opacity);
                if (presence_.startMs() != 0 && (presence_.phase() == ImmersivePresence::Hiding ||
                                                 presence_.phase() == ImmersivePresence::Showing)) {
                    const bool hiding = presence_.phase() == ImmersivePresence::Hiding;
                    const UINT duration = hiding ? kImmersiveHideMs : kImmersiveShowMs;
                    const ULONGLONG elapsed = now - presence_.startMs();
                    if (elapsed < duration)
                        renderer.animateRoot(0.0f, 0.0f, 0.0f, 0.0f, opacity, hiding ? 0.0f : 1.0f,
                                             static_cast<float>(duration - elapsed) / 1000.0f);
                }
            }
        }
        renderer.setDpi(dpi_);
        createDeviceResources();
        return renderState_.deviceResourcePhase() == RenderState::DeviceResourcePhase::Ready;
    }

    void refreshTheme() {
        app_icon::applyTaskbarIcon(hwnd);
        updateBackdropSolidColor();
        const bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        if (light != lightTheme_) {
            lightTheme_ = light;
            discardDeviceResources();
        }
        // 媒体卡片属于普通悬浮窗，使用 Window 主题，而不是任务栏主题。
        mediaPopup.refreshTheme();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    void releaseAll() {
        discardDeviceResources();
        renderer.releaseAll();
    }

    // ---------- 避让探测工作线程 ----------

    // 正常每 3 秒探测一次：TrafficMonitor、酷狗窗口矩形 + UIA 任务栏按钮矩形；
    // 空间不足隐藏时缩短到 1 秒，尽快发现可恢复空间。UIA 属性查询由 explorer
    // 的 UI 线程执行，不能跟随歌词帧率高频调用。
    // 每次循环都重新发布结果（即使没变化），变化比较在 UI 线程拾取时做

    void startProbe() { placementProbe_.start(); }

    void stopProbe() { placementProbe_.stop(); }

    // UI 线程慢速分支：拾取探测结果，与缓存比较有变化才更新。返回是否有变化
    bool pickProbeResult() {
        auto p = placementProbe_.take();
        if (!p)
            return false;
        const bool firstProbe = !probeReady_;
        probeReady_ = true;
        bool changed = !EqualRect(&p->rcTm, &rcTrafficMonitor_);
        if (!changed) {
            if (p->buttons.size() != uiaButtons_.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < p->buttons.size(); ++i) {
                    if (!EqualRect(&p->buttons[i], &uiaButtons_[i])) {
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (!changed) {
            if (p->kugouWindows.size() != kugouTaskbarWindows_.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < p->kugouWindows.size(); ++i) {
                    if (!EqualRect(&p->kugouWindows[i], &kugouTaskbarWindows_[i])) {
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (changed) {
            rcTrafficMonitor_ = p->rcTm;
            uiaButtons_ = std::move(p->buttons);
            kugouTaskbarWindows_ = std::move(p->kugouWindows);
        }
        return changed || firstProbe;
    }

    // ---------- 封面解码 ----------

    void decodeCover() {
        cover_.decode(renderer.renderTarget(), media.thumbnail.get(), dpi_,
                      std::max(1u, static_cast<UINT>(std::ceil(coverSize() * scale()))));
    }

    // 封面模糊链：GaussianBlur → Scale 铺满全窗。效果与设备同生命周期，
    // 封面更换或设备重建后经 coverBlurInput_ 惰性重绑；窗口尺寸每帧可能变化，
    // Scale 参数便宜，直接按帧写入
    ID2D1Effect* ensureCoverBlurChain(float w, float h) {
        return cover_.blurred(renderer.renderTarget(), w, h);
    }

    void decodePlatformIcon() {
        if (platformIconBmp.Get()) {
            platformIconBmp.Reset();
        }
        if (!albumCoverVisible_ || !platformIconVisible_ || media.sourceAppUserModelId.empty())
            return;

        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!platform_icon::readSourceIconPixels(media.sourceAppUserModelId, pixels, width, height))
            return;

        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        ID2D1Bitmap1* decoded = nullptr;
        rt->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4, &props, &decoded);
        platformIconBmp.Attach(decoded);
    }

    int displayLyricLine() const {
        if (scene_ == DisplayScene::NoPlayback || scene_ == DisplayScene::Idle ||
            scene_ == DisplayScene::Searching || scene_ == DisplayScene::Message)
            return -1;
        if (currentLine >= 0 && (size_t)currentLine < lines.size())
            return currentLine;
        return lines.empty() ? -1 : 0;
    }

    std::wstring selectedSecondary(const LyricLine& line) const {
        if (compactTaskbarHeight_ > 0.0f)
            return {};
        if (translationEnabled_)
            return line.translation;
        if (romanizationEnabled_)
            return line.romanization;
        return {};
    }

    bool useDoubleLineLyricsForLine(int lineIndex) const {
        return compactTaskbarHeight_ <= 0.0f && doubleLineLyricsEnabled_ && lineIndex >= 0 &&
               static_cast<size_t>(lineIndex) < lines.size() &&
               selectedSecondary(lines[static_cast<size_t>(lineIndex)]).empty();
    }

    bool useDoubleLineLyrics() const {
        if (isLyricTransitionActive() && typesetter_.resources().lyricLayout_.Get())
            return lyricLayoutDoubleLine_;
        return useDoubleLineLyricsForLine(displayLyricLine());
    }

    // ---------- 行过渡状态 ----------

    void releaseOutgoingLyricLayouts() {
        outgoingLyricEffectLine_ = -1;
        typesetter_.releaseOutgoing();
        outgoingLyricWidth_ = 0.0f;
        outgoingLyricHeight_ = 0.0f;
        outgoingVerticalLyricWidth_ = 0.0f;
        outgoingVerticalLyricHeight_ = 0.0f;
        outgoingVerticalLyricRotated_ = false;
        outgoingSecondaryWidth_ = 0.0f;
        outgoingSecondaryHeight_ = 0.0f;
        outgoingLyricBlockHeight_ = 0.0f;
        outgoingLyricScrollOffset_ = 0.0f;
        outgoingSecondaryScrollOffset_ = 0.0f;
        outgoingNextLyricWidth_ = 0.0f;
        outgoingNextLyricHeight_ = 0.0f;
    }

    // 丢弃当前行过渡：清空待启动/进行中状态并释放旧布局，下一次排版直接提交最终行。
    void resetLyricTransition() {
        if (isSceneResizeActive()) {
            cancelSceneWindowResize();
            requestInvalidation(RenderInvalidation::Layout);
        }
        clearLyricTransitionPhase();
        transition_.setKind(LyricTransitionKind::None);
        transition_.setOutgoingScene(DisplayScene::NoPlayback);
        transition_.setOutgoingDoubleLine(false);
        transition_.setNeedsRelayout(false);
        transition_.setStartMs(0);
        transition_.setRevision(0);
        transition_.clearTarget();
        transition_.clearPending();
        requestLyricDCompEnd();
        releaseOutgoingLyricLayouts();
        karaoke_.cancelEntry();
    }

    // 行目标变化统一入口：动画进行中只记录最新目标（不重启当前动画），
    // 待启动的过渡直接改打最新目标，空闲时发起新过渡；不允许动画时立即提交最终状态。
    void onLyricLineTargetChanged(int newLine, int64_t actualPositionMs, uint64_t revision,
                                  bool allowAnimate) {
        const int previous = currentLine;
        currentLine = newLine;
        const auto decision = transition_.request(previous, newLine, actualPositionMs, revision,
                                                  allowAnimate && clientAnimations_);
        if (decision == LyricTransition::Decision::Reset)
            resetLyricTransition();
        if (decision != LyricTransition::Decision::Queued)
            requestInvalidation(RenderInvalidation::Text);
    }

    // 行过渡收尾：先冻结动画再交换状态。释放旧布局、消费动画期间记录的最新目标；
    // 自然转场中的新行保留逐字追赶过程，避免收尾时又跳到真实位置。
    void finalizeLyricTransition(ULONGLONG now) {
        const bool sceneTransition = transition_.kind() == LyricTransitionKind::Scene;
        const bool sceneNeedsRelayout = transition_.needsRelayout();
        requestLyricDCompEnd();
        releaseOutgoingLyricLayouts();
        clearLyricTransitionPhase();
        transition_.setStartMs(0);

        if (sceneTransition) {
            // 类别转场不消费歌词行的 pendingTarget；场景稳定后用最新完整帧
            // 再做一次排版，避免把转场期间收到的歌词行状态写回旧布局。
            transition_.setKind(LyricTransitionKind::None);
            transition_.setOutgoingScene(DisplayScene::NoPlayback);
            transition_.setOutgoingDoubleLine(false);
            transition_.setNeedsRelayout(false);
            transition_.clearTarget();
            transition_.clearPending();
            transition_.setRevision(0);
            if (sceneNeedsRelayout)
                requestInvalidation(RenderInvalidation::Text);
            return;
        }

        if (transition_.advancePending(frameRevision_)) {
            // 动画期间收到了更新的目标行：当前布局仍是旧目标，以它为旧行立即
            // 发起向最新目标的过渡，不经过稳定的中间帧。
            requestInvalidation(RenderInvalidation::Text);
            // 布局尚未对应新行：解绑逐字平滑状态，排版完成后直接对齐真实目标。
            karaoke_.retarget(now);
            return;
        }
        transition_.clearTarget();
        transition_.setRevision(0);

        karaoke_.finishTransition(currentLine, typesetter_.resources().lyricLayout_.Get(),
                                  positionMs_, karaokeLine() != nullptr, now);
        clearLyricTransitionPhase();
        transition_.setKind(LyricTransitionKind::None);
        transition_.setOutgoingScene(DisplayScene::NoPlayback);
        transition_.setOutgoingDoubleLine(false);
    }

    // ---------- 排版 ----------

    // 把一行歌词拆成逐字换行的布局。中文和符号按 Unicode 码点处理，
    // 常见括号转换为竖排字形；避免 UTF-16 代理项被拆开后出现半个字符。
    // 纯拉丁文本走整句旋转布局。


    void buildTextLayouts(float leftW, float rightW) {
        // 文本布局以超宽无换行创建，度量与区域宽度无关，leftW/rightW 仅保留签名兼容
        (void)leftW;
        (void)rightW;
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !typesetter_.resources().fmtTitle_.Get() ||
            !typesetter_.resources().fmtArtist_.Get() || !typesetter_.resources().fmtLyric_.Get())
            return;

        // 歌曲信息（标题/歌手）独立重建：换行只走下面的歌词分支，
        // 歌曲信息变化也不触碰歌词布局与行过渡状态
        if (isInvalidated(RenderInvalidation::SongInfo)) {
            typesetter_.setFactory(dwrite);
            const auto metrics =
                typesetter_.buildSongInfo(media.title, media.artist, songInfoVisible_);
            titleWidth_ = metrics.titleWidth;
            titleHeight_ = metrics.titleHeight;
            artistWidth_ = metrics.artistWidth;
            artistHeight_ = metrics.artistHeight;
            const bool titleChanged = media.title != lastTitle_;
            const bool artistChanged = media.artist != lastArtist_;
            if (titleChanged) {
                scroll_.resetTitle();
                lastTitle_ = media.title;
            }
            if (artistChanged) {
                scroll_.resetArtist();
                lastArtist_ = media.artist;
            }
            if (titleChanged || artistChanged)
                scroll_.resetClock();
        }
        if (!isInvalidated(RenderInvalidation::Text))
            return;
        lyricTextRenderer_.invalidate(); // 布局指针重建，离屏缓存全部失效
        clearLyricEffectGeometry();
        karaoke_.clearGeometry();

        const bool doubleLineLyrics = useDoubleLineLyricsForLine(displayLyricLine());
        // 准备阶段：先把当前布局移交为旧行，目标行布局构建完成后才记录动画起点，
        // 避免“新布局已替换但动画初始位置还没准备好”导致的文字瞬移。
        bool preparedTransition = false;
        if (isLyricTransitionPending() && typesetter_.resources().lyricLayout_.Get()) {
            // 保留旧行离场前的滚动位置。新布局后面会把 scroll_.lyric() 重置为 0，
            // 不能让旧的超长歌词因此在转场第一帧跳回开头。
            const float outgoingLyricOffset = scroll_.lyric();
            const float outgoingSecondaryOffset = scroll_.secondary();
            const bool outgoingDoubleLine = lyricLayoutDoubleLine_;
            typesetter_.retainCurrentForTransition(outgoingDoubleLine);
            outgoingLyricEffectLine_ = lyricEffectLayoutLine_;
            outgoingLyricWidth_ = lyricWidth_;
            outgoingLyricHeight_ = lyricHeight_;
            outgoingVerticalLyricWidth_ = verticalLyricWidth_;
            outgoingVerticalLyricHeight_ = verticalLyricHeight_;
            outgoingVerticalLyricRotated_ = verticalLyricRotated_;
            outgoingLyricScrollOffset_ = outgoingLyricOffset;
            transition_.setOutgoingDoubleLine(outgoingDoubleLine);
            outgoingSecondaryWidth_ = outgoingDoubleLine ? 0.0f : secondaryWidth_;
            outgoingSecondaryHeight_ = outgoingDoubleLine ? 0.0f : secondaryHeight_;
            outgoingNextLyricWidth_ = outgoingDoubleLine ? nextLyricWidth_ : 0.0f;
            outgoingNextLyricHeight_ = outgoingDoubleLine ? nextLyricHeight_ : 0.0f;
            if (!outgoingDoubleLine)
                outgoingSecondaryScrollOffset_ = outgoingSecondaryOffset;
            const float outgoingPreviewH =
                outgoingDoubleLine ? typesetter_.resources().outgoingNextLyricLayout_.Get()
                                         ? kLyricPreviewGap + outgoingNextLyricHeight_
                                         : 0.0f
                : typesetter_.resources().outgoingSecondaryLayout_.Get()
                    ? 1.0f + outgoingSecondaryHeight_
                    : 0.0f;
            outgoingLyricBlockHeight_ = outgoingLyricHeight_ + outgoingPreviewH;
            preparedTransition = true;
        } else {
            resetLyricTransition();
            typesetter_.clearCurrentContent();
        }
        // 排版已接管待启动转场；成功构建目标布局后会转换为 Running。

        typesetter_.clearNextContent();
        nextLyricWidth_ = 0.0f;
        nextLyricHeight_ = 0.0f;

        int displayLine = displayLyricLine();
        lyricEffectLayoutLine_ = displayLine;
        std::wstring lyric;
        std::wstring secondary;
        if (displayLine >= 0) {
            lyric = lines[(size_t)displayLine].text;
            secondary = selectedSecondary(lines[(size_t)displayLine]);
        } else {
            lyric = statusText;
        }
        LyricTextInput input;
        input.lyric = lyric;
        input.secondary = secondary;
        input.doubleLine = doubleLineLyrics;
        input.vertical = isVerticalTaskbar();
        if (input.vertical) {
            int width = 0, height = 0;
            clientPixelSize(width, height);
            input.verticalWidth = dip(width);
        }
        if (doubleLineLyrics && displayLine >= 0 &&
            static_cast<size_t>(displayLine + 1) < lines.size())
            input.next = lines[static_cast<size_t>(displayLine + 1)].text;
        typesetter_.setFactory(dwrite);
        const auto metrics = typesetter_.buildContent(input);
        lyricWidth_ = metrics.width;
        lyricHeight_ = metrics.height;
        verticalLyricWidth_ = metrics.verticalWidth;
        verticalLyricHeight_ = metrics.verticalHeight;
        verticalLyricRotated_ = metrics.verticalRotated;
        secondaryWidth_ = metrics.secondaryWidth;
        secondaryHeight_ = metrics.secondaryHeight;
        nextLyricWidth_ = metrics.nextWidth;
        nextLyricHeight_ = metrics.nextHeight;
        if (isMinimalMode()) {
            // 极简只改变显示策略；歌词行和逐字时间轴仍由上游完整读取，
            // 这里只不创建供逐字绘制使用的几何缓存。
            karaoke_.clearGeometry();
        } else {
            karaoke_.build(lines, displayLine, typesetter_.resources().lyricLayout_.Get());
        }
        lyricLayoutDoubleLine_ = doubleLineLyrics;
        if (preparedTransition) {
            if (typesetter_.resources().lyricLayout_.Get()) {
                // 目标布局就绪后才启动动画计时：准备布局的这一帧不消耗过渡时长。
                transition_.setStartMs(monotonicNowMs());
                setLyricTransitionActive();
                transition_.setRevision(frameRevision_);
            } else {
                // 目标行没有可绘制布局：放弃过渡，直接回到稳定状态。
                resetLyricTransition();
            }
        }
        const float extent =
            isVerticalTaskbar() && verticalLyricHeight_ > 0.0f ? verticalLyricHeight_ : lyricWidth_;
        scroll_.setLineTiming(lines, currentLine, extent,
                              isVerticalTaskbar() ? kVerticalLyricGap : kTextPadding * 2.0f);
        bool lyricChanged = lyric != lastLyric_;
        bool secondaryChanged = secondary != lastSecondary_;
        if (lyricChanged) {
            scroll_.resetLyric();
            lastLyric_ = lyric;
        }
        if (secondaryChanged) {
            scroll_.resetSecondary();
            lastSecondary_ = secondary;
        }
        if (lyricChanged || secondaryChanged) {
            scroll_.resetClock();
        }
    }

    float coverSize() const {
        if (!hwnd)
            return 24.0f;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const float h = dip(isVerticalTaskbar() ? pxW : pxH);
        return h - kCoverPadding * 2.0f;
    }

    float coverSlotWidth(float heightDip) const {
        return albumCoverVisible_ ? heightDip - kCoverPadding : 0.0f;
    }

    float infoStartX() const {
        return albumCoverVisible_ ? kCoverPadding + coverSize() + kCoverPadding : kTextPadding;
    }

    float lyricStartPadding() const {
        return songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
    }

    float immersiveControlRadius(float heightDip) const {
        return std::clamp(heightDip * kImmersiveControlRadiusFactor, kImmersiveControlMinRadius,
                          kImmersiveControlMaxRadius);
    }

    float immersiveControlsWidth(float heightDip) const {
        const float radius = immersiveControlRadius(heightDip);
        const float pitch = radius * kImmersiveControlPitchFactor;
        const float groupWidth =
            pitch * static_cast<float>(expandedControlCount() - 1) + radius * 2.0f;
        return groupWidth + kTextPadding * 2.0f;
    }

    VerticalLayout verticalLayout() const {
        int width = 0;
        int height = 0;
        clientPixelSize(width, height);
        return taskbar_detail::verticalLayout(dip(width), dip(height), scene_, albumCoverVisible_,
                                              mouseOver_ && controlsOnHover_ &&
                                                  hoverControlStyle_ == HoverControlStyle::Inline);
    }

    LayoutMetrics layoutMetricsForScene(DisplayScene scene, int pxW, int pxH) const {
        const float width = dip(pxW);
        const float height = dip(pxH);
        const bool expandedControls = isExpandedView() && !isVerticalTaskbar();
        return taskbar_detail::horizontalLayout(
            {width, height, scene, isExpandedView(), expandedControls, songInfoVisible_,
             albumCoverVisible_, expandedControls ? immersiveControlsWidth(height) : 0.0f,
             spectrumExtraForScene(scene, width),
             isExpandedView() && songInfoVisible_ && albumCoverVisible_ ? coverSize() : 0.0f,
             coverSlotWidth(height)});
    }

    LayoutMetrics layoutMetrics(int pxW, int pxH) const {
        return layoutMetricsForScene(scene_, pxW, pxH);
    }

    struct LyricArea {
        float x = 0.0f;
        float w = 0.0f;
    };

    LyricArea lyricAreaForScene(DisplayScene scene, int pxW, int pxH) const {
        const LayoutMetrics layout = layoutMetricsForScene(scene, pxW, pxH);
        const float start =
            songInfoVisible_ && scene != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
        // 这里返回的是“可绘制安全区”，不再把它当作歌词的居中基准。
        // 右边界统一落在频谱之前，左边界统一落在封面/歌曲信息/沉浸控件之后。
        float left = layout.leftW + start + layout.immersiveControlsW;
        if (horizontalImmersiveMode() && scene != DisplayScene::Idle && isSessionVisible() &&
            !isMinimalMode() && !isStoppedMode())
            left += kDockPetSeatW;
        const float right = layout.w - spectrumExtraForScene(scene, layout.w) - kTextPadding;
        return {left, std::max(1.0f, right - left)};
    }

    // 沉浸模式始终以整条任务栏的几何中心作为歌词锚点。封面、歌曲信息、
    // 操作控件、频谱及时钟只参与安全区和裁剪；当空间不足时由绘制路径
    // 把文字约束在安全区内，不能反过来改变正常歌词的中心位置。
    float immersiveLyricCenterX() const {
        if (!isExpandedView() || isVerticalTaskbar())
            return -1.0f;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return -1.0f;
        return layout.w * 0.5f;
    }

    // 当前行有逐字时间轴且歌词布局对应该行时返回该行；极简模式强制使用普通横向滚动。
    const LyricLine* karaokeLine() const {
        if (isMinimalMode())
            return nullptr;
        if (currentLine < 0 || (size_t)currentLine >= lines.size())
            return nullptr;
        const LyricLine* line = &lines[(size_t)currentLine];
        if (line->chars.empty() || !typesetter_.resources().lyricLayout_.Get() ||
            line->text != lastLyric_)
            return nullptr;
        return line;
    }


    // 逐字填充目标进度 x（布局像素坐标）：已唱边界 + 当前 token 按时长比例推进；
    // durOut 输出当前 token 时长（ms），供平滑时间常数使用。X 坐标在排版时缓存，
    // 播放过程中只做时间定位，不再每帧调用 DirectWrite 命中测试。

    // 平滑步进：过渡时间常数取当前字时长的 1/4（40~200ms），指数趋近目标，
    // 每个字的过渡快慢随其时长自然变化，且同步误差有界（约 τ）。
    // SMTC 锚点校正造成的目标抖动经低通后不再闪烁；非自然切换或大幅 seek 直接对齐，
    // 自然转场中的新行则从首字平滑追赶真实位置。
    float karaokeSmoothStep(const LyricLine&) {
        return karaoke_.advance(currentLine, typesetter_.resources().lyricLayout_.Get(),
                                positionMs_, clientAnimations_, isLyricTransitionActive(),
                                monotonicNowMs());
    }

    float vinylInnerRadius(float s) const {
        float outerRadius = std::max(1.0f, s * kVinylOuterRatio);
        float recordRadius =
            std::max(0.5f, outerRadius - std::min(kVinylHaloWidth, outerRadius * 0.25f));
        return std::min(s * kVinylInnerRatio, std::max(1.0f, recordRadius - 1.0f));
    }

    // ---------- 图标几何 ----------

    void ensureGeometry() {
        if (!isInvalidated(RenderInvalidation::Geometry))
            return;
        ID2D1Factory* d2d = renderer.d2d();
        if (!d2d)
            return;

        if (coverClip_.Get()) {
            coverClip_.Reset();
        }
        if (vinylCoverClip_.Get()) {
            vinylCoverClip_.Reset();
        }
        media_control::release(controlGeometry);

        float s = coverSize();
        D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, s, s), 4.0f, 4.0f};
        d2d->CreateRoundedRectangleGeometry(rr, coverClip_.ReleaseAndGetAddressOf());
        if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
            D2D1_ELLIPSE coverEllipse{D2D1::Point2F(s * 0.5f, s * 0.5f), vinylInnerRadius(s),
                                      vinylInnerRadius(s)};
            d2d->CreateEllipseGeometry(coverEllipse, vinylCoverClip_.ReleaseAndGetAddressOf());
        }

        if (!media_control::create(d2d, controlGeometry))
            requestInvalidation(RenderInvalidation::Geometry);
    }

    // ---------- 渲染 ----------

    // ---------- 内嵌悬浮控件布局 ----------

    // 上一首/播放/下一首 + 音量按钮整体居中：centers[0..2] 为播放控制，
    // centers[3] 为音量按钮。返回 false 表示当前不在内嵌控件展示状态。
    bool inlineControlsLayout(float centers[4], float& cy, float& r) const {
        if (isVerticalTaskbar())
            return false;
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return false;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return false;
        cy = layout.h * 0.5f;
        r = layout.h * 0.26f;
        const float spacing = r * 2.8f;
        // 音量按钮与下一首之间多留 0.9r：音量字形比三角形宽，等间距会显得挤
        const float volumeGap = spacing + r * 0.9f;
        // 组跨度 [cx-spacing, cx+spacing+volumeGap]，整体居中即 cx 左移 volumeGap/2
        const float controlsW = std::max(1.0f, layout.rightW - embeddedResourceExtra());
        const float cx = layout.leftW + controlsW * 0.5f - volumeGap * 0.5f;
        for (int i = 0; i < 3; ++i)
            centers[i] = cx + (i - 1) * spacing;
        centers[3] = cx + spacing + volumeGap;
        return true;
    }

    // 沉浸模式专属控件：始终显示在歌曲信息分隔线右侧，不受普通任务栏歌词
    // 的悬浮控件开关和样式设置影响。控件组的几何区域也会从歌词区预先扣除。
    bool immersiveControlsLayout(D2D1_POINT_2F centers[kExpandedControlCount], float& cy,
                                 float& r) const {
        if (!isExpandedView() || isVerticalTaskbar())
            return false;

        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f || layout.immersiveControlsW <= 0.0f)
            return false;

        const float start =
            songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
        r = immersiveControlRadius(layout.h);
        const float pitch = r * kImmersiveControlPitchFactor;
        const float groupW = pitch * static_cast<float>(expandedControlCount() - 1) + r * 2.0f;
        const float groupLeft =
            layout.leftW + start + std::max(0.0f, (layout.immersiveControlsW - groupW) * 0.5f);
        cy = layout.h * 0.5f;
        int visibleIndex = 0;
        for (int i = 0; i < kExpandedControlCount; ++i) {
            if (!expandedControlVisible(i))
                continue;
            centers[i] = D2D1::Point2F(groupLeft + r + visibleIndex * pitch, cy);
            ++visibleIndex;
        }
        return true;
    }

    // includeDisabled：让出任务栏的判定把暂不可用的按钮也算作控件区域。
    int hitImmersiveControl(float x, float y, bool includeDisabled = false) const {
        D2D1_POINT_2F centers[kExpandedControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!immersiveControlsLayout(centers, cy, r))
            return -1;

        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        for (int i = 0; i < kExpandedControlCount; ++i) {
            if (!expandedControlVisible(i))
                continue;
            bool enabled = true;
            switch (i) {
            case kImmersiveControlPrevious:
                enabled = media.canPrev;
                break;
            case kImmersiveControlPlayPause:
                enabled = media.canPlayPause;
                break;
            case kImmersiveControlNext:
                enabled = media.canNext;
                break;
            case kDockControlQuickApps:
                enabled = static_cast<bool>(onDockQuickApps);
                break;
            case kImmersiveControlMenu:
                enabled = static_cast<bool>(onImmersiveMenu);
                break;
            default:
                break;
            }
            if (!enabled && !includeDisabled)
                continue;
            if (std::hypot(logicalX - centers[i].x, logicalY - centers[i].y) <= r + 4.0f)
                return i;
        }
        return -1;
    }

    void drawExitImmersiveButton(const D2D1_POINT_2F& center, float radius) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushBtn_.Get())
            return;

        // 视图边框配合两支向内收起的箭头，明确表达“退出沉浸模式”，
        // 同时避开托盘“退出程序”所使用的门与外向箭头语义。
        const float frame = radius * 0.74f;
        const float start = radius * 0.50f;
        const float end = radius * 0.08f;
        const float head = radius * 0.22f;
        const float stroke = std::clamp(radius * 0.14f, 1.1f, 1.7f);

        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center.x - frame, center.y - frame,
                                                               center.x + frame, center.y + frame),
                                                   radius * 0.15f, radius * 0.15f),
                                 brushBtn_.Get(), stroke);

        const auto topLeftEnd = D2D1::Point2F(center.x - end, center.y - end);
        rt->DrawLine(D2D1::Point2F(center.x - start, center.y - start), topLeftEnd, brushBtn_.Get(),
                     stroke);
        rt->DrawLine(topLeftEnd, D2D1::Point2F(topLeftEnd.x - head, topLeftEnd.y), brushBtn_.Get(),
                     stroke);
        rt->DrawLine(topLeftEnd, D2D1::Point2F(topLeftEnd.x, topLeftEnd.y - head), brushBtn_.Get(),
                     stroke);

        const auto bottomRightEnd = D2D1::Point2F(center.x + end, center.y + end);
        rt->DrawLine(D2D1::Point2F(center.x + start, center.y + start), bottomRightEnd,
                     brushBtn_.Get(), stroke);
        rt->DrawLine(bottomRightEnd, D2D1::Point2F(bottomRightEnd.x + head, bottomRightEnd.y),
                     brushBtn_.Get(), stroke);
        rt->DrawLine(bottomRightEnd, D2D1::Point2F(bottomRightEnd.x, bottomRightEnd.y + head),
                     brushBtn_.Get(), stroke);
    }

    void drawImmersiveControls() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        D2D1_POINT_2F centers[kExpandedControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!immersiveControlsLayout(centers, cy, r))
            return;

        if (brushHover_.Get()) {
            for (int i = 0; i < kExpandedControlCount; ++i) {
                if (expandedControlVisible(i) && immersiveControlHover_ == i) {
                    const auto color = brushHover_->GetColor();
                    auto fill = color;
                    if (immersiveControlPressed_ == i)
                        fill.a = std::min(1.0f, color.a * 1.8f);
                    brushHover_->SetColor(fill);
                    const float half = r + 4.0f;
                    const float corner = std::min(6.0f, half * 0.4f);
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(centers[i].x - half, centers[i].y - half,
                                                      centers[i].x + half, centers[i].y + half),
                                          corner, corner),
                        brushHover_.Get());
                    brushHover_->SetColor(color);
                }
            }
        }
        drawButton(kImmersiveControlPrevious, centers[kImmersiveControlPrevious], r);
        drawButton(kImmersiveControlPlayPause, centers[kImmersiveControlPlayPause], r);
        drawButton(kImmersiveControlNext, centers[kImmersiveControlNext], r);
        drawVolumeButton(centers[kImmersiveControlVolume], r);
        drawExitImmersiveButton(centers[kImmersiveControlExit], r);
        if (expandedControlVisible(kDockControlQuickApps)) {
            settings_icon::draw(rt, settings_icon::Kind::QuickLaunch,
                                D2D1::RectF(centers[kDockControlQuickApps].x - r * 0.82f,
                                            centers[kDockControlQuickApps].y - r * 0.82f,
                                            centers[kDockControlQuickApps].x + r * 0.82f,
                                            centers[kDockControlQuickApps].y + r * 0.82f),
                                onDockQuickApps ? brushBtn_.Get() : brushBtnDisabled_.Get(), 1.45f);
        }
        if (brushBtn_.Get()) {
            const float dotRadius = std::clamp(r * 0.13f, 1.0f, 1.6f);
            const float dotPitch = r * 0.48f;
            for (int i = -1; i <= 1; ++i) {
                rt->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(centers[kImmersiveControlMenu].x + i * dotPitch,
                                                centers[kImmersiveControlMenu].y),
                                  dotRadius, dotRadius),
                    brushBtn_.Get());
            }
        }
    }

    void drawInlineControlBackground(int index, const D2D1_POINT_2F& center, float radius,
                                     bool enabled) {
        auto* rt = drawTarget();
        if (!rt || !brushHover_.Get() || !enabled || isExpandedView() || !mouseOver_ ||
            inlineControlHover_ != index)
            return;
        const bool pressed = drag_.pressed() && !drag_.dragging() && GetCapture() == hwnd;
        const auto color = brushHover_->GetColor();
        auto fill = color;
        if (pressed)
            fill.a = std::min(1.0f, color.a * 1.8f);
        brushHover_->SetColor(fill);
        const float half = radius + 2.0f;
        const float corner = std::min(6.0f, half * 0.4f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center.x - half, center.y - half,
                                                               center.x + half, center.y + half),
                                                   corner, corner),
                                 brushHover_.Get());
        brushHover_->SetColor(color);
    }

    void drawVolumeButton(const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        drawInlineControlBackground(3, c, r, appVolume_.available);
        ID2D1SolidColorBrush* brush =
            appVolume_.available ? brushBtn_.Get() : brushBtnDisabled_.Get();
        const int level = !appVolume_.available || appVolume_.muted ? 0
                          : appVolume_.percent == 0                 ? 1
                          : appVolume_.percent < 50                 ? 2
                                                                    : 3;
        media_control::drawVolume(rt, c, r * 0.8f, brush, level);
    }

    bool hitVolumeButton(float x, float y) const {
        if (isExpandedView())
            return hitImmersiveControl(x, y) == kImmersiveControlVolume;
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (!layout.showControls)
                return false;
            float logicalX = 0.0f;
            float logicalY = 0.0f;
            clientPointToLogicalDip(x, y, logicalX, logicalY);
            return std::hypot(logicalX - layout.controlCenters[3].x,
                              logicalY - layout.controlCenters[3].y) <= layout.controlRadius + 4.0f;
        }
        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r))
            return false;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        x = logicalX;
        y = logicalY;
        return std::hypot(x - centers[3], y - cy) <= r + 4.0f;
    }

    // 音量按钮的屏幕坐标矩形（音量滑块浮窗的锚点）
    RECT volumeButtonScreenRect() const {
        if (isExpandedView()) {
            D2D1_POINT_2F centers[kExpandedControlCount]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (immersiveControlsLayout(centers, cy, r)) {
                const float s = scale();
                POINT pt = logicalDipToClientPoint(centers[kImmersiveControlVolume].x,
                                                   centers[kImmersiveControlVolume].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((r + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (layout.showControls) {
                const float s = scale();
                POINT pt =
                    logicalDipToClientPoint(layout.controlCenters[3].x, layout.controlCenters[3].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((layout.controlRadius + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            return rc;
        }
        const float s = scale();
        POINT pt = logicalDipToClientPoint(centers[3], cy);
        ClientToScreen(hwnd, &pt);
        const int half = static_cast<int>(std::lround((r + 6.0f) * s));
        return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
    }

    void prepareForExternalPopup() {
        cancelImmersiveYield();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        requestFrameAndFlush();
    }

    void openTaskbarMenu(POINT screenPoint) {
        if (!onContextMenu)
            return;
        prepareForExternalPopup();
        onContextMenu(screenPoint);
    }

    void openImmersiveMenu(POINT screenPoint) {
        if (!onImmersiveMenu)
            return;
        prepareForExternalPopup();
        onImmersiveMenu(screenPoint);
    }

    // ---------- 沉浸模式让出任务栏 ----------
    bool immersiveYieldAvailable() const {
        return viewMode_ == TaskbarViewMode::Immersive && hwnd && taskbar_ && isWindowVisible();
    }

    // 与 DCompRenderer::animateRoot 的 smoothstep 曲线一致，用于中途反向时取当前透明度。
    float immersivePresenceOpacity(ULONGLONG now) const { return presence_.opacity(now); }

    // 内容（DComp 根视觉）与模糊背景（Windows.UI.Composition 层）同步淡入淡出。
    void animateImmersivePresence(float from, float to, UINT durationMs) {
        if (!renderer.animateRoot(0.0f, 0.0f, 0.0f, 0.0f, from, to,
                                  static_cast<float>(durationMs) / 1000.0f))
            renderer.setRootOpacity(to);
        renderer.commit();
        dockBackdrop_.animatePresence(from, to, durationMs);
    }

    void startImmersivePresenceTimer() {
        if (immersivePresenceTimerRunning_ || !hwnd)
            return;
        if (SetTimer(hwnd, kImmersivePresenceTimerId, kImmersivePresencePollMs, nullptr))
            immersivePresenceTimerRunning_ = true;
    }

    void stopImmersivePresenceTimer() {
        if (immersivePresenceTimerRunning_ && hwnd)
            KillTimer(hwnd, kImmersivePresenceTimerId);
        immersivePresenceTimerRunning_ = false;
    }

    void cancelImmersiveYield() {
        if (immersiveYieldArmed_ && hwnd)
            KillTimer(hwnd, kImmersiveYieldTimerId);
        immersiveYieldArmed_ = false;
    }

    bool immersiveYieldAllowedState() const {
        return presence_.phase() == ImmersivePresence::Shown ||
               presence_.phase() == ImmersivePresence::Showing;
    }

    // 鼠标移动时更新让出判定：只有停在播放控件以外的区域才开始计时，
    // 划过控件之间的空隙去点按钮不会误触发。
    void updateImmersiveYield(int hoveredControl) {
        if (immersiveYieldAvailable() && (hoveredControl >= 0 || immersiveControlPressed_ >= 0))
            cancelImmersiveHide();
        if (!immersiveYieldAvailable() || !immersiveYieldAllowedState() || hoveredControl >= 0 ||
            immersiveControlPressed_ >= 0) {
            cancelImmersiveYield();
            return;
        }
        if (!immersiveYieldArmed_ &&
            SetTimer(hwnd, kImmersiveYieldTimerId, kImmersiveYieldDelayMs, nullptr))
            immersiveYieldArmed_ = true;
    }

    void onImmersiveYieldTimer() {
        cancelImmersiveYield();
        if (!immersiveYieldAvailable() || !immersiveYieldAllowedState() ||
            immersiveControlPressed_ >= 0)
            return;
        POINT cursor{};
        RECT windowRect{};
        if (!GetCursorPos(&cursor) || !GetWindowRect(hwnd, &windowRect) ||
            !PtInRect(&windowRect, cursor))
            return;
        POINT client = cursor;
        ScreenToClient(hwnd, &client);
        if (hitImmersiveControl(static_cast<float>(client.x), static_cast<float>(client.y), true) >=
            0)
            return;
        beginImmersiveHide();
    }

    void beginImmersiveHide() {
        const ULONGLONG now = GetTickCount64();
        const float from = immersivePresenceOpacity(now);
        presence_.beginHide(now, from);
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        animateImmersivePresence(from, 0.0f, kImmersiveHideMs);
        startImmersivePresenceTimer();
        requestFrameAndFlush();
    }

    void cancelImmersiveHide() {
        cancelImmersiveYield();
        if (presence_.phase() != ImmersivePresence::Hiding)
            return;
        const ULONGLONG now = GetTickCount64();
        const float from = immersivePresenceOpacity(now);
        presence_.beginShow(now, from);
        animateImmersivePresence(from, 1.0f, kImmersiveShowMs);
        startImmersivePresenceTimer();
    }

    void finishImmersiveHide() {
        presence_.hide();
        const bool hadPress = immersiveControlPressed_ >= 0;
        immersiveControlPressed_ = -1;
        if (hadPress && GetCapture() == hwnd)
            ReleaseCapture();
        mouseOver_ = false;
        trackingLeave_ = false;
        immersiveControlHover_ = -1;
        volumeHover_ = false;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        reconcileWindowVisibility();
    }

    void beginImmersiveShow() {
        presence_.prepareShow();
        // 先以透明状态完成首帧并显示窗口，再启动淡入，避免隐藏期间的旧帧闪现。
        renderer.setRootOpacity(0.0f);
        dockBackdrop_.animatePresence(0.0f, 0.0f, 0);
        requestInvalidation(RenderInvalidation::Paint);
        reconcileWindowVisibility(true);
        if (!isWindowVisible()) {
            // 停止模式、会话不可见等其他原因仍要求隐藏：直接回到常态，交由原有可见性逻辑。
            resetImmersivePresence();
            return;
        }
        presence_.startShow(GetTickCount64());
        animateImmersivePresence(0.0f, 1.0f, kImmersiveShowMs);
        startImmersivePresenceTimer();
    }

    // 鼠标离开任务栏且没有按住按键（例如正往任务栏按钮上拖放文件）时才恢复。
    bool immersiveCursorAwayFromTaskbar() const {
        POINT cursor{};
        if (!GetCursorPos(&cursor))
            return false;
        RECT taskbarRect{};
        if (taskbar_ && GetWindowRect(taskbar_, &taskbarRect) && PtInRect(&taskbarRect, cursor))
            return false;
        for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) {
            if ((GetAsyncKeyState(button) & 0x8000) != 0)
                return false;
        }
        return true;
    }

    void onImmersivePresenceTimer() {
        if (viewMode_ != TaskbarViewMode::Immersive) {
            resetImmersivePresence();
            return;
        }
        const bool away =
            presence_.phase() == ImmersivePresence::Hidden && immersiveCursorAwayFromTaskbar();
        switch (presence_.advance(GetTickCount64(), away)) {
        case TaskbarPresence::Action::Hide:
            finishImmersiveHide();
            break;
        case TaskbarPresence::Action::Show:
            beginImmersiveShow();
            break;
        case TaskbarPresence::Action::StopTimer:
            stopImmersivePresenceTimer();
            break;
        case TaskbarPresence::Action::None:
            break;
        }
    }

    // 切换展示模式、窗口销毁等场景立即回到常态（不透明、不让出）。
    void resetImmersivePresence() {
        cancelImmersiveYield();
        stopImmersivePresenceTimer();
        const bool wasShown = presence_.phase() == ImmersivePresence::Shown;
        presence_.reset();
        if (!wasShown) {
            renderer.setRootOpacity(1.0f);
            renderer.commit();
            dockBackdrop_.animatePresence(1.0f, 1.0f, 0);
        }
    }

    void openDockQuickApps(POINT screenPoint) {
        if (!isAppBarView() || !onDockQuickApps)
            return;
        prepareForExternalPopup();
        onDockQuickApps(screenPoint);
    }

    void activateImmersiveControl(int index) {
        switch (index) {
        case kImmersiveControlPrevious:
        case kImmersiveControlPlayPause:
        case kImmersiveControlNext:
            if (onControl)
                onControl(static_cast<MediaControl>(index));
            break;
        case kImmersiveControlVolume:
            if (appVolume_.available) {
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect(), false, taskbarEdge_ == ABE_LEFT);
            }
            break;
        case kImmersiveControlExit:
            if (onImmersiveExit)
                onImmersiveExit();
            break;
        case kDockControlQuickApps: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            openDockQuickApps(pt);
            break;
        }
        case kImmersiveControlMenu: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            // 菜单按钮是完整托盘菜单的固定入口，不受“右键显示
            // 任务栏歌词菜单”开关影响，也不复用任务栏歌词的精简菜单。
            openImmersiveMenu(pt);
            break;
        }
        default:
            break;
        }
    }

    void drawButton(int idx, const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        bool enabled = idx == 0 ? media.canPrev : idx == 1 ? media.canPlayPause : media.canNext;
        drawInlineControlBackground(idx, c, r, enabled);
        ID2D1SolidColorBrush* brush = enabled ? brushBtn_.Get() : brushBtnDisabled_.Get();

        media_control::draw(rt, controlGeometry, idx, media.playing, c, r, brush);
    }

    int hitButton(float x, float y) const {
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return -1;
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (!layout.showControls)
                return -1;
            float logicalX = 0.0f;
            float logicalY = 0.0f;
            clientPointToLogicalDip(x, y, logicalX, logicalY);
            for (int i = 0; i < 3; ++i) {
                const bool enabled = i == 0   ? media.canPrev
                                     : i == 1 ? media.canPlayPause
                                              : media.canNext;
                if (enabled && std::hypot(logicalX - layout.controlCenters[i].x,
                                          logicalY - layout.controlCenters[i].y) <=
                                   layout.controlRadius + 4.0f)
                    return i;
            }
            return -1;
        }
        // 鼠标消息使用像素坐标，而 render 使用 DIP；先统一到 DIP，
        // 并与 render 使用完全相同的左侧分区计算。
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        float w = layout.w;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        x = logicalX;
        y = logicalY;
        float leftW = layout.leftW;
        if (x < leftW || x > w)
            return -1;

        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r))
            return -1;
        for (int i = 0; i < 3; ++i) {
            bool en = i == 0 ? media.canPrev : i == 1 ? media.canPlayPause : media.canNext;
            if (!en)
                continue;
            if (std::hypot(x - centers[i], y - cy) <= r + 4.0f)
                return i;
        }
        return -1;
    }

    struct LyricTransitionSample {
        float progress = 1.0f;
        float movement = 1.0f;
        float fadeOut = 1.0f;
        float fadeIn = 1.0f;
    };

    static float smoothStep(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void beginImmersiveSongContentTransition() {
        if (!usesSongContentTransition() || isMinimalMode() || !isSongTransitionPending() ||
            songContentTransition_)
            return;
        songContentTransition_ = SongContentTransition{monotonicNowMs()};
    }

    void clearImmersiveSongContentTransition() {
        if (songContentTransition_ && songContentTransition_->compositorLayer)
            renderer.clearLyricTransitionLayers();
        songContentTransition_.reset();
    }

    void finishImmersiveSongContentTransitionIfNeeded() {
        if (!songContentTransition_ || !songContentTransition_->compositorLayer)
            return;
        const ULONGLONG now = monotonicNowMs();
        if (now < songContentTransition_->startMs ||
            now - songContentTransition_->startMs <
                static_cast<ULONGLONG>(std::ceil(kSongTransitionMs)))
            return;
        renderer.clearLyricTransitionLayers();
        songContentTransition_.reset();
        // 下一帧恢复到交换链绘制最新歌曲内容，并与合成层的结束点衔接。
        requestFrame();
    }

    // D2D 回退路径：沉浸模式的遮罩、控件和频谱保持静止，只把封面、歌曲信息
    // 和歌词作为一组内容在 D2D 坐标系内滑入。
    bool applyImmersiveSongContentTransform(ID2D1DeviceContext* rt) {
        if (!rt || !usesSongContentTransition() || !songContentTransition_ ||
            songContentTransition_->compositorLayer)
            return false;

        ULONGLONG now = monotonicNowMs();
        if (now < songContentTransition_->startMs)
            now = songContentTransition_->startMs;
        const float progress = std::clamp(
            static_cast<float>(now - songContentTransition_->startMs) / kSongTransitionMs, 0.0f,
            1.0f);
        if (progress >= 1.0f) {
            songContentTransition_.reset();
            return false;
        }

        const float offset = kSongTransitionTravelDip * (1.0f - smoothStep(progress));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(offset, 0.0f));
        return true;
    }

    static float rangedSmoothStep(float t, float start, float end) {
        if (end <= start)
            return t >= end ? 1.0f : 0.0f;
        return smoothStep((t - start) / (end - start));
    }

    float lyricTransitionDurationMs() const {
        return transition_.kind() == LyricTransitionKind::Scene ? kSceneTransitionMs
                                                                : kLyricTransitionMs;
    }

    LyricTransitionSample lyricTransitionSample() const {
        LyricTransitionSample sample;
        if (!isLyricTransitionActive() || transition_.startMs() == 0)
            return sample;

        // applyPresentationFrame() 可以在定时器之外直接触发 render()。此时
        // frameNowMs_ 仍可能早于刚设置的起点，不能用 ULONGLONG 直接相减。
        ULONGLONG now = frameNowMs_;
        if (now < transition_.startMs())
            now = monotonicNowMs();
        if (now < transition_.startMs())
            now = transition_.startMs();
        sample.progress = std::clamp(static_cast<float>(now - transition_.startMs()) /
                                         lyricTransitionDurationMs(),
                                     0.0f, 1.0f);
        sample.movement = smoothStep(sample.progress);
        sample.fadeOut = rangedSmoothStep(sample.progress, 0.08f, 0.90f);
        sample.fadeIn = rangedSmoothStep(sample.progress, 0.14f, 1.0f);
        return sample;
    }

    // 绘制可滚动文本：容得下则按 alignment 对齐，容不下则向左无缝滚动。
    // outline/glow 非空时先画 8 方向光晕层和深色描边层，再画主文字。
    // karaokeBrush 非空时启用逐字高亮：整行先用 brush（未播放色）画一遍，
    // 再按像素裁剪出 [文本起点, karaokeX] 区域用 karaokeBrush（已播放色）画第二遍，
    // 实现字内平滑填充（裁剪基于像素，不受滚动偏移影响）
    // 将 glow+outline+本体三层合成到离屏位图（独立的兼容渲染目标，不嵌套主 BeginDraw）。
    // 成功返回可绘制位图（调用方负责 Release），失败返回 nullptr 走直接绘制兜底。
    // 必须在画刷透明度被修改之前调用，缓存内容始终是自然透明度。
    ID2D1Bitmap* textFxBitmap(IDWriteTextLayout* layout, float textW, float textH,
                              ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow) {
        return lyricTextRenderer_.textFxBitmap(lyricTextContext(nullptr), layout, textW, textH,
                                               brush, outline, glow);
    }

    // 滚动文本左缘渐隐画笔：固定两停止点（透明 → 不透明），创建一次终身复用，
    // 几何完全由每帧移动渐变轴实现

    ID2D1LinearGradientBrush* ensureSongInfoDividerBrush() {
        if (songInfoDividerBrush_.Get())
            return songInfoDividerBrush_.Get();
        auto* rt = drawTarget();
        if (!rt)
            return nullptr;

        const D2D1_COLOR_F base = brushDim_.Get()
                                      ? brushDim_->GetColor()
                                      : (lightTheme_ ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f)
                                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f));
        const float peakAlpha = std::clamp(base.a * 0.72f, 0.0f, 1.0f);
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(base.r, base.g, base.b, 0.0f)},
            {0.18f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha * 0.42f)},
            {0.50f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha)},
            {0.82f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha * 0.42f)},
            {1.0f, D2D1::ColorF(base.r, base.g, base.b, 0.0f)},
        };
        ID2D1GradientStopCollection* stopCollection = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, _countof(stops), &stopCollection)) ||
            !stopCollection)
            return nullptr;

        const HRESULT hr =
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                              D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
                                          D2D1::BrushProperties(), stopCollection,
                                          songInfoDividerBrush_.ReleaseAndGetAddressOf());
        stopCollection->Release();
        return FAILED(hr) ? nullptr : songInfoDividerBrush_.Get();
    }

    void drawSongInfoDivider(float leftW, float h) {
        auto* rt = drawTarget();
        if (!rt || !songInfoVisible_ || scene_ == DisplayScene::Idle || h <= 2.0f ||
            kSongInfoLyricGap <= kSongInfoDividerWidth)
            return;

        const float top = std::min(kSongInfoDividerInset, h * 0.25f);
        const float bottom = std::max(top + kSongInfoDividerWidth, h - top);
        if (bottom <= top)
            return;
        auto* brush = ensureSongInfoDividerBrush();
        if (!brush)
            return;

        brush->SetStartPoint(D2D1::Point2F(0.0f, top));
        brush->SetEndPoint(D2D1::Point2F(0.0f, bottom));
        const float x = leftW + (kSongInfoLyricGap - kSongInfoDividerWidth) * 0.5f;
        const D2D1_RECT_F rect = D2D1::RectF(x, top, x + kSongInfoDividerWidth, bottom);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(rect, kSongInfoDividerWidth * 0.5f, kSongInfoDividerWidth * 0.5f),
            brush);
    }

    // 区域内左缘渐隐：渐隐带宽度随滚出量从 0 长到 fadeW，且左缘处透明度恒为 0。
    // 若让整个渐隐带按滚出量淡入，则滚出不足 fadeW 时左缘仍有残余不透明度，
    // 半截字符会在边界硬裁剪处留下一条竖线

    // 右缘渐隐带画笔：渐隐带完全位于可视区右缘之外（rightEdge 起 extend 宽），
    // 可读区域内文字始终全不透明，只有越过边界的部分向外渐隐


    // 竖排滚动文本的两侧渐隐几何。竖排跑马灯向下滚动（offset > 0）：内容从底部
    // 滚出、顶部滚入；逐字跟随向上回收（offset < 0）：从顶部滚出、底部滚入。
    // 与横向同一套语义：滚出侧在区域内渐隐（带宽随滚出量从 0 建立；每日一言这类
    // 内容确定的无限循环跑马灯恒定保持，避免绕回时渐隐消失再出现），滚入侧借位
    // 到可视区外渐隐，可读区域内文字始终全不透明


    // 竖排滚出侧渐隐画笔：复用横向左缘画笔（停止点 0=透明 1=不透明），
    // 每帧改设竖向渐变轴；边界处透明度恒为 0，半截字符不会在边界留下亮线

    // 竖排滚入侧借位渐隐画笔：复用横向右缘画笔（停止点 0=不透明 1=透明），
    // 渐隐带完全位于可视区之外（edgeY 起向外 extend 宽）

    bool lyricEffectEnabled(LyricEffect effect) const {
        lyricEffectsRenderer_.setContext(lyricEffectsContext());
        return lyricEffectsRenderer_.lyricEffectEnabled(effect);
    }

    bool lyricStarEnabled(int lineIndex) const {
        lyricEffectsRenderer_.setContext(lyricEffectsContext());
        return lyricEffectsRenderer_.lyricStarEnabled(lineIndex);
    }

    float lyricStarRadius() const { return LyricEffectsRenderer::starRadius(fontSize_); }

    float lyricStarTextShift(IDWriteTextLayout* layout) const {
        const bool outgoing = layout == typesetter_.resources().outgoingLyricLayout_.Get() ||
                              layout == typesetter_.resources().outgoingSecondaryLayout_.Get();
        const bool current = layout == typesetter_.resources().lyricLayout_.Get() ||
                             layout == typesetter_.resources().secondaryLayout_.Get();
        const int line = outgoing  ? outgoingLyricEffectLine_
                         : current ? lyricEffectLayoutLine_
                                   : -1;
        if (!lyricStarEnabled(line))
            return 0.0f;
        const float blockH =
            outgoing
                ? outgoingLyricHeight_ + (typesetter_.resources().outgoingSecondaryLayout_.Get()
                                              ? 1.0f + outgoingSecondaryHeight_
                                              : 0.0f)
                : lyricHeight_ + (typesetter_.resources().secondaryLayout_.Get()
                                      ? 1.0f + secondaryHeight_
                                      : 0.0f);
        const float h = lastPxH_ / scale();
        return std::min(8.0f, std::max(0.0f, (h - blockH) * 0.5f - 2.0f));
    }

    void syncLyricEffectClock(int64_t position, bool playing, bool reset = false) {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricEffectsRenderer_.syncLyricEffectClock(position, playing, reset);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void setLyricEffects(uint32_t effects) {
        effects &= kLyricEffectsMask;
        if (lyricEffects_ == effects)
            return;
        lyricEffects_ = effects;
        clearLyricEffectGeometry();
        syncLyricEffectClock(positionMs_, media.playing, true);
        if (isLyricDCompActive()) {
            renderer.clearLyricTransitionLayers();
            clearLyricDCompState();
        }
        clearImmersiveSongContentTransition();
        requestFrameAndFlush();
    }

    bool lyricEffectsAnimating() const {
        lyricEffectsRenderer_.setContext(lyricEffectsContext());
        return lyricEffectsRenderer_.lyricEffectsAnimating();
    }

    void clearLyricEffectGeometry() {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricEffectsRenderer_.clearLyricEffectGeometry();
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    float lyricEffectProgress(int lineIndex) {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        return lyricEffectsRenderer_.lyricEffectProgress(lineIndex);
    }

    // 所有坐标均在实际文字布局的局部空间，复用调用方的缩放、旋转和渐隐裁剪。
    // rangeStart/rangeSize 把中英混排竖栏的每一段映射回同一句时间轴。
    void drawLyricOverlay(IDWriteTextLayout* layout, int lineIndex, float x, float y, float width,
                          float height, bool vertical, float opacity, float progress,
                          bool foreground, float rangeStart = 0.0f, float rangeSize = 1.0f) {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricEffectsRenderer_.drawLyricOverlay(layout, lineIndex, x, y, width, height, vertical,
                                               opacity, progress, foreground, rangeStart,
                                               rangeSize);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    LyricTextDrawContext lyricTextContext(IDWriteTextLayout* layout) {
        return {drawTarget(),
                &typesetter_.resources(),
                lyricEffectsRenderer_,
                dpi_,
                fontSize_,
                lastPxW_ / scale(),
                layout ? lyricStarTextShift(layout) : 0.0f,
                lyricEffectLayoutLine_,
                outgoingLyricEffectLine_,
                lyricEffects_};
    }

    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                           ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                           float opacity = 1.0f, LyricAlignment alignment = LyricAlignment::Center,
                           bool singleCopy = false, float rightExtend = 0.0f,
                           bool constantEdgeFade = false, float leftFadeDip = 0.0f,
                           float centerAnchor = -1.0f) {
        const auto context = lyricTextContext(layout);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricTextRenderer_.drawScrollingText(context, layout, textW, textH, areaW, x, y, offset,
                                             brush, outline, glow, karaokeBrush, karaokeX, opacity,
                                             alignment, singleCopy, rightExtend, constantEdgeFade,
                                             leftFadeDip, centerAnchor);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void drawVerticalScrollingText(
        IDWriteTextLayout* layout, float textW, float textH, float areaH, float x, float y,
        float offset, ID2D1Brush* brush, ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
        ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f, float opacity = 1.0f,
        LyricAlignment alignment = LyricAlignment::Center, bool singleCopy = false,
        bool rotated = false, float topExtend = 0.0f, float bottomExtend = 0.0f,
        bool constantEdgeFade = false, bool skipSlotClip = false, float slotY = -1.0f,
        int effectLine = -1, float effectRangeStart = 0.0f, float effectRangeSize = 1.0f,
        float effectProgress = -1.0f) {
        const auto context = lyricTextContext(layout);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricTextRenderer_.drawVerticalScrollingText(
            context, layout, textW, textH, areaH, x, y, offset, brush, outline, glow, karaokeBrush,
            karaokeX, opacity, alignment, singleCopy, rotated, topExtend, bottomExtend,
            constantEdgeFade, skipSlotClip, slotY, effectLine, effectRangeStart, effectRangeSize,
            effectProgress);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void drawVerticalLyricParts(const std::vector<VerticalLyricPart>& parts, float blockH,
                                float areaH, float railW, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeProgress = 0.0f,
                                float opacity = 1.0f, bool singleCopy = false,
                                float topExtend = 0.0f, float bottomExtend = 0.0f,
                                bool constantEdgeFade = false, float slotY = -1.0f) {
        const auto context = lyricTextContext(nullptr);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricTextRenderer_.drawVerticalLyricParts(
            context, parts, blockH, areaH, railW, y, offset, brush, outline, glow, karaokeBrush,
            karaokeProgress, opacity, singleCopy, topExtend, bottomExtend, constantEdgeFade, slotY);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void drawVerticalLyrics(const VerticalLayout& layout) {
        auto* rt = drawTarget();
        if (!rt || layout.lyricBottom <= layout.lyricY)
            return;

        const float areaH = layout.lyricBottom - layout.lyricY;
        const bool idleScene = scene_ == DisplayScene::Idle;
        ID2D1Brush* primaryBrush =
            idleScene ? static_cast<ID2D1Brush*>(brushText_.Get())
                      : static_cast<ID2D1Brush*>(brushLyric_.Get() ? brushLyric_.Get()
                                                                   : brushText_.Get());
        const bool outgoingIdleScene = transition_.outgoingScene() == DisplayScene::Idle;
        ID2D1Brush* outgoingBrush =
            outgoingIdleScene ? static_cast<ID2D1Brush*>(brushText_.Get()) : primaryBrush;
        ID2D1Brush* effectOutline = !idleScene && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                        : nullptr;
        ID2D1Brush* effectGlow =
            !idleScene && lyricGlow_ ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get()) : nullptr;
        ID2D1Brush* outgoingOutline = !outgoingIdleScene && lyricOutline_
                                          ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                          : nullptr;
        ID2D1Brush* outgoingGlow = !outgoingIdleScene && lyricGlow_
                                       ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get())
                                       : nullptr;
        const LyricAlignment alignment = LyricAlignment::Center;

        auto drawLine = [&](IDWriteTextLayout* textLayout,
                            const std::vector<VerticalLyricPart>* parts, float textW, float textH,
                            float y, float offset, ID2D1Brush* brush, float opacity,
                            bool singleCopy, bool rotated, ID2D1Brush* outline, ID2D1Brush* glow,
                            ID2D1Brush* karaokeBrush, float karaokeProgress, bool constantFade) {
            // 滚入侧借位宽度：与竖排各区间距一致（封面与歌词间、歌词与底部/控件间
            // 均为 6 dip 或窗口边缘内边距），渐隐带不外溢到封面/控件上。
            // slotY 始终传歌词槽顶：转场期间 y 随动画移动，但裁剪/渐隐锚定歌词槽
            if (parts && !parts->empty()) {
                drawVerticalLyricParts(*parts, textH, areaH, layout.w, y, offset, brush, outline,
                                       glow, karaokeBrush, karaokeProgress, opacity, singleCopy,
                                       kVerticalLyricGap, kVerticalLyricGap, constantFade,
                                       layout.lyricY);
                return;
            }
            // 旋转英文的实际横向宽度通常小于任务栏槽宽，单独居中，避免整句贴在
            // 侧边栏外沿；中文逐字布局本身已经使用完整槽宽。
            const float textX = rotated ? (layout.w - textW) * 0.5f : 0.0f;
            drawVerticalScrollingText(textLayout, textW, textH, areaH, textX, y, offset, brush,
                                      outline, glow, karaokeBrush, karaokeProgress, opacity,
                                      alignment, singleCopy, rotated, kVerticalLyricGap,
                                      kVerticalLyricGap, constantFade, false, layout.lyricY);
        };

        auto verticalKaraokeProgress = [&](float karaokeX) {
            return lyricWidth_ > 0.0f ? std::clamp(karaokeX / lyricWidth_, 0.0f, 1.0f) : 0.0f;
        };

        // 不再使用整段统一裁剪：各绘制路径自带裁剪/渐隐图层，滚入侧借位内容
        // 需要画出可视槽之外才能形成渐隐带
        if (isLyricTransitionActive() &&
            (typesetter_.resources().outgoingVerticalLyricLayout_.Get() ||
             !typesetter_.resources().outgoingVerticalLyricParts_.empty())) {
            const LyricTransitionSample transition = lyricTransitionSample();
            const float travel = std::clamp(areaH * 0.24f, 18.0f, 48.0f);
            const float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
            const float oldY = layout.lyricY - direction * travel * transition.movement;
            const float newY = layout.lyricY + direction * travel * (1.0f - transition.movement);
            drawLine(typesetter_.resources().outgoingVerticalLyricLayout_.Get(),
                     &typesetter_.resources().outgoingVerticalLyricParts_,
                     outgoingVerticalLyricWidth_, outgoingVerticalLyricHeight_, oldY,
                     outgoingLyricScrollOffset_, outgoingBrush, 1.0f - transition.fadeOut, true,
                     outgoingVerticalLyricRotated_, outgoingOutline, outgoingGlow, nullptr, 0.0f,
                     outgoingIdleScene);

            const LyricLine* incomingLine = !idleScene ? karaokeLine() : nullptr;
            const bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
            const float incomingKaraokeX =
                incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
            drawLine(typesetter_.resources().verticalLyricLayout_.Get(),
                     &typesetter_.resources().verticalLyricParts_, verticalLyricWidth_,
                     verticalLyricHeight_, newY, scroll_.lyric(),
                     incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                     : primaryBrush,
                     transition.fadeIn, true, verticalLyricRotated_, effectOutline, effectGlow,
                     incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
                     verticalKaraokeProgress(incomingKaraokeX), idleScene);
        } else if (typesetter_.resources().verticalLyricLayout_.Get() ||
                   !typesetter_.resources().verticalLyricParts_.empty()) {
            const LyricLine* currentLine = !idleScene ? karaokeLine() : nullptr;
            const bool karaoke = currentLine && brushLyric_.Get() && brushLyricDim_.Get();
            const float karaokeX = karaoke ? karaokeSmoothStep(*currentLine) : 0.0f;
            drawLine(typesetter_.resources().verticalLyricLayout_.Get(),
                     &typesetter_.resources().verticalLyricParts_, verticalLyricWidth_,
                     verticalLyricHeight_, layout.lyricY, scroll_.lyric(),
                     karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : primaryBrush, 1.0f,
                     false, verticalLyricRotated_, effectOutline, effectGlow,
                     karaoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
                     verticalKaraokeProgress(karaokeX), idleScene);
        }
    }

    LyricAlignment activeLyricAlignment() const {
        if (scene_ == DisplayScene::Idle)
            return idleQuoteAlignment_;
        return isExpandedView() ? LyricAlignment::Center : lyricAlignment_;
    }

    LyricAlignment lyricAlignmentForScene(DisplayScene scene) const {
        if (scene == DisplayScene::Idle)
            return idleQuoteAlignment_;
        return isExpandedView() ? LyricAlignment::Center : lyricAlignment_;
    }

    void drawLyricScrollingTextAligned(IDWriteTextLayout* layout, float textW, float textH,
                                       float areaW, float x, float y, float offset,
                                       ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow,
                                       ID2D1Brush* karaokeBrush, float karaokeX, float opacity,
                                       LyricAlignment alignment, bool singleCopy = false) {
        // 左缘渐隐宽度分场景：歌曲信息可见时用与分隔间距一致的 8 dip，且渐隐始终
        // 在歌词区域内，文字不会画进信息区；信息区隐藏时用默认宽度
        const float leftFadeDip =
            songInfoVisible_ && scene_ != DisplayScene::Idle ? kTextPadding : 0.0f;
        const float centerAnchor = immersiveLyricCenterX();
        // 每日一言与歌曲信息同属内容确定的无限循环跑马灯，是否滚动在内容确定时
        // 已知，渐隐恒定保持，避免每轮循环绕回时左缘渐隐消失再出现
        drawScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                          karaokeBrush, karaokeX, opacity, alignment, singleCopy, kTextPadding,
                          scene_ == DisplayScene::Idle, leftFadeDip, centerAnchor);
    }

    void drawLyricScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                                float x, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                                float opacity = 1.0f, bool singleCopy = false) {
        drawLyricScrollingTextAligned(layout, textW, textH, areaW, x, y, offset, brush, outline,
                                      glow, karaokeBrush, karaokeX, opacity, activeLyricAlignment(),
                                      singleCopy);
    }

    void drawScaledScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                                 float x, float y, float offset, ID2D1Brush* brush, float opacity,
                                 float scale, ID2D1Brush* outline = nullptr,
                                 ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
                                 float karaokeX = 0.0f, bool singleCopy = false,
                                 float visibleW = 0.0f) {
        auto* rt = drawTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        if (scale >= 0.999f) {
            drawLyricScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                                   karaokeBrush, karaokeX, opacity, singleCopy);
            return;
        }

        float anchorX = x + areaW * 0.5f;
        if (activeLyricAlignment() == LyricAlignment::Left)
            anchorX = x;
        else if (activeLyricAlignment() == LyricAlignment::Right)
            anchorX = x + areaW;
        else if (const float centerAnchor = immersiveLyricCenterX(); centerAnchor >= 0.0f)
            anchorX = centerAnchor;
        const D2D1_POINT_2F anchor = D2D1::Point2F(anchorX, y + textH * 0.5f);
        // 外层裁剪先在最终窗口坐标中固定下来，再缩放文字。否则裁剪矩形会和文字
        // 一起围绕任务栏中心缩放；沉浸 / Dock 的歌词安全区左右不对称时，右边界
        // 会短暂移入频谱、时钟和控件区域，转场结束恢复普通裁剪时形成闪烁。
        const int effectLine = layout == typesetter_.resources().lyricLayout_.Get()
                                   ? lyricEffectLayoutLine_
                               : layout == typesetter_.resources().outgoingLyricLayout_.Get()
                                   ? outgoingLyricEffectLine_
                                   : -1;
        const float starPad =
            lyricStarEnabled(effectLine) ? lyricStarRadius() * 2.0f + 10.0f : 0.0f;
        const float textShift = lyricStarTextShift(layout);
        const D2D1_RECT_F safeClip =
            D2D1::RectF(x, y - starPad, x + areaW + kTextPadding, y + textH + textShift);
        rt->PushAxisAlignedClip(safeClip, D2D1_ANTIALIAS_MODE_ALIASED);
        // visibleW 是转场期间可见的文本坐标宽度；缩放轴与歌词的实际居中锚点
        // 保持一致，避免新旧两句在入场时发生横向漂移。
        const float drawW = visibleW > 0.0f ? visibleW : areaW;
        D2D1_MATRIX_3X2_F previous{};
        rt->GetTransform(&previous);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, anchor) * previous);
        drawLyricScrollingText(layout, textW, textH, drawW, x, y, offset, brush, outline, glow,
                               karaokeBrush, karaokeX, opacity, singleCopy);
        rt->SetTransform(previous);
        rt->PopAxisAlignedClip();
    }

    void drawDoubleLineLyrics(float lyricAreaX, float lyricAreaW, float h,
                              ID2D1Brush* primaryBrush) {
        if (!typesetter_.resources().lyricLayout_.Get())
            return;

        const float lyricBlockH = lyricHeight_ + (typesetter_.resources().nextLyricLayout_.Get()
                                                      ? kLyricPreviewGap + nextLyricHeight_
                                                      : 0.0f);
        const float coreY = h * 0.5f - lyricBlockH * 0.5f;
        ID2D1Brush* coreBrush = primaryBrush        ? primaryBrush
                                : brushLyric_.Get() ? static_cast<ID2D1Brush*>(brushLyric_.Get())
                                                    : static_cast<ID2D1Brush*>(brushText_.Get());
        ID2D1Brush* previewBrush = brushLyricDim_.Get()
                                       ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                       : static_cast<ID2D1Brush*>(brushDim_.Get());
        ID2D1Brush* effectOutline = scene_ != DisplayScene::Idle && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                        : nullptr;
        ID2D1Brush* effectGlow = scene_ != DisplayScene::Idle && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get())
                                     : nullptr;
        if (isLyricTransitionActive() && typesetter_.resources().outgoingLyricLayout_.Get()) {
            const LyricTransitionSample transition = lyricTransitionSample();
            const float movementT = transition.movement;
            float outgoingBlockH =
                outgoingLyricBlockHeight_ > 0.0f ? outgoingLyricBlockHeight_ : outgoingLyricHeight_;
            float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
            float travel = std::max(outgoingLyricHeight_, lyricHeight_) + kLyricPreviewGap;
            float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
            float oldShift = -direction * travel * movementT;
            float incomingStartY = direction > 0.0f
                                       ? outgoingY + outgoingLyricHeight_ + kLyricPreviewGap
                                       : coreY - travel;
            float incomingY = incomingStartY + (coreY - incomingStartY) * movementT;
            const LyricLine* incomingLine = karaokeLine();
            bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
            ID2D1Brush* incomingBrush =
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : coreBrush;
            float incomingProgX = incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
            float incomingScale = kLyricPreviewScale + (1.0f - kLyricPreviewScale) * movementT;
            // y 是主字号布局的位置；缩小时校正半个缩放差，使视觉中心跟随位移而不跳动。
            float scaledIncomingY = incomingY - lyricHeight_ * (1.0f - incomingScale) * 0.5f;

            // 超出部分的消失与第二行上移同步：转场起点取预览期实际可见的右边界
            // （预览放得下=整行可见；放不下=预览渐隐位置，均换算到核心行文本坐标），
            // 随转场进度收敛到核心行可视宽，避免动画第一帧尾部瞬间消失。
            float incomingVisibleW = 0.0f;
            if (lyricWidth_ > lyricAreaW) {
                // 当前缩放比例下恰好覆盖歌词安全区所需的文本坐标宽度。
                // 使用实时比例而不是对两个宽度线性插值，避免中间帧的可见宽度
                // 反而大于安全区，并由外层固定裁剪保证不会侵入右侧固定内容。
                incomingVisibleW = std::min(lyricWidth_, lyricAreaW / incomingScale);
            }

            // 下一行在转场前已经位于核心行下方；转场从这个位置接入核心，避免跳变。
            drawLyricScrollingText(typesetter_.resources().outgoingLyricLayout_.Get(),
                                   outgoingLyricWidth_, outgoingLyricHeight_, lyricAreaW,
                                   lyricAreaX, outgoingY + oldShift, outgoingLyricScrollOffset_,
                                   coreBrush, effectOutline, effectGlow, nullptr, 0.0f,
                                   1.0f - transition.fadeOut);
            if (!transition_.outgoingDoubleLine() &&
                typesetter_.resources().outgoingSecondaryLayout_.Get())
                drawLyricScrollingText(typesetter_.resources().outgoingSecondaryLayout_.Get(),
                                       outgoingSecondaryWidth_, outgoingSecondaryHeight_,
                                       lyricAreaW, lyricAreaX,
                                       outgoingY + outgoingLyricHeight_ + 1.0f + oldShift,
                                       outgoingSecondaryScrollOffset_, brushDim_.Get(), nullptr,
                                       nullptr, nullptr, 0.0f, 1.0f - transition.fadeOut);
            drawScaledScrollingText(
                typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_, lyricAreaW,
                lyricAreaX, scaledIncomingY, scroll_.lyric(), incomingBrush,
                kLyricPreviewOpacity + (1.0f - kLyricPreviewOpacity) * transition.fadeIn,
                incomingScale, effectOutline, effectGlow,
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
                incomingProgX, true, incomingVisibleW);
            return;
        }

        // 核心行仍保留原有逐字高亮；下一行只作为低透明度预览，不参与逐字填充。
        const LyricLine* curLine = karaokeLine();
        bool karaoke = curLine && brushLyric_.Get() && brushLyricDim_.Get();
        float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
        drawLyricScrollingText(
            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_, lyricAreaW,
            lyricAreaX, coreY, scroll_.lyric(),
            karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : coreBrush, effectOutline,
            effectGlow, karaoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr, progX);

        if (typesetter_.resources().nextLyricLayout_.Get()) {
            float nextY = coreY + lyricHeight_ + kLyricPreviewGap;
            drawLyricScrollingText(typesetter_.resources().nextLyricLayout_.Get(), nextLyricWidth_,
                                   nextLyricHeight_, lyricAreaW, lyricAreaX, nextY, 0.0f,
                                   previewBrush, nullptr, nullptr, nullptr, 0.0f,
                                   kLyricPreviewOpacity);
        }
    }

    // 每日一言与歌词属于任务栏的两个展示场景。场景切换时内容层与外层尺寸
    // 使用同一段转场时序，旧内容仍按切换前的内容区绘制，避免水平布局跳变。
    void drawSceneTransition(float w, float lyricAreaX, float lyricAreaW, float h,
                             float lyricBlockH) {
        auto* rt = drawTarget();
        if (!rt || !typesetter_.resources().lyricLayout_.Get() ||
            !typesetter_.resources().outgoingLyricLayout_.Get() || lastPxW_ <= 0 || lastPxH_ <= 0)
            return;

        const int outgoingPxW =
            transition_.fromWidth() > 0 ? transition_.fromWidth() : lastLogicalPxW_;
        const int outgoingPxH =
            transition_.fromHeight() > 0 ? transition_.fromHeight() : lastLogicalPxH_;
        const LyricArea outgoingArea =
            lyricAreaForScene(transition_.outgoingScene(), outgoingPxW, outgoingPxH);
        const LyricTransitionSample transition = lyricTransitionSample();
        const float movementT = transition.movement;
        const float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
        const bool incomingDoubleLine =
            useDoubleLineLyrics() && typesetter_.resources().nextLyricLayout_.Get();
        const float incomingBlockH =
            incomingDoubleLine ? lyricHeight_ + kLyricPreviewGap + nextLyricHeight_ : lyricBlockH;
        const float outgoingPreviewH =
            transition_.outgoingDoubleLine() &&
                    typesetter_.resources().outgoingNextLyricLayout_.Get()
                ? kLyricPreviewGap + outgoingNextLyricHeight_
            : typesetter_.resources().outgoingSecondaryLayout_.Get()
                ? 1.0f + outgoingSecondaryHeight_
                : 0.0f;
        const float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                         ? outgoingLyricBlockHeight_
                                         : outgoingLyricHeight_ + outgoingPreviewH;
        const float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
        const float incomingY = h * 0.5f - incomingBlockH * 0.5f;
        const float travel = std::max(incomingBlockH, outgoingBlockH);
        const float oldShift = -direction * travel * movementT;
        const float newShift = direction * travel * (1.0f - movementT);

        auto primaryBrushForScene = [this](DisplayScene scene) -> ID2D1Brush* {
            if (scene == DisplayScene::Idle)
                return brushText_.Get();
            return brushLyric_.Get() ? static_cast<ID2D1Brush*>(brushLyric_.Get())
                                     : static_cast<ID2D1Brush*>(brushText_.Get());
        };
        const bool outgoingEffects = transition_.outgoingScene() != DisplayScene::Idle;
        const bool incomingEffects = scene_ != DisplayScene::Idle;
        ID2D1Brush* outgoingBrush = primaryBrushForScene(transition_.outgoingScene());
        ID2D1Brush* incomingBrush = primaryBrushForScene(scene_);
        ID2D1Brush* outgoingOutline =
            outgoingEffects && lyricOutline_ ? brushLyricOutline_.Get() : nullptr;
        ID2D1Brush* outgoingGlow = outgoingEffects && lyricGlow_ ? brushLyricGlow_.Get() : nullptr;
        ID2D1Brush* incomingOutline =
            incomingEffects && lyricOutline_ ? brushLyricOutline_.Get() : nullptr;
        ID2D1Brush* incomingGlow = incomingEffects && lyricGlow_ ? brushLyricGlow_.Get() : nullptr;

        const LyricLine* incomingLine = scene_ == DisplayScene::Lyrics ? karaokeLine() : nullptr;
        const bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
        if (incomingKaraoke)
            incomingBrush = brushLyricDim_.Get();
        const float incomingProgX = incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, h), D2D1_ANTIALIAS_MODE_ALIASED);
        drawLyricScrollingTextAligned(
            typesetter_.resources().outgoingLyricLayout_.Get(), outgoingLyricWidth_,
            outgoingLyricHeight_, outgoingArea.w, outgoingArea.x, outgoingY + oldShift,
            outgoingLyricScrollOffset_, outgoingBrush, outgoingOutline, outgoingGlow, nullptr, 0.0f,
            1.0f - transition.fadeOut, lyricAlignmentForScene(transition_.outgoingScene()));
        if (transition_.outgoingDoubleLine() &&
            typesetter_.resources().outgoingNextLyricLayout_.Get()) {
            ID2D1Brush* previewBrush = brushLyricDim_.Get()
                                           ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                           : static_cast<ID2D1Brush*>(brushDim_.Get());
            drawLyricScrollingTextAligned(
                typesetter_.resources().outgoingNextLyricLayout_.Get(), outgoingNextLyricWidth_,
                outgoingNextLyricHeight_, outgoingArea.w, outgoingArea.x,
                outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f, previewBrush,
                nullptr, nullptr, nullptr, 0.0f, kLyricPreviewOpacity * (1.0f - transition.fadeOut),
                lyricAlignmentForScene(transition_.outgoingScene()), true);
        } else if (typesetter_.resources().outgoingSecondaryLayout_.Get()) {
            drawLyricScrollingTextAligned(
                typesetter_.resources().outgoingSecondaryLayout_.Get(), outgoingSecondaryWidth_,
                outgoingSecondaryHeight_, outgoingArea.w, outgoingArea.x,
                outgoingY + outgoingLyricHeight_ + 1.0f + oldShift, outgoingSecondaryScrollOffset_,
                brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f, 1.0f - transition.fadeOut,
                lyricAlignmentForScene(transition_.outgoingScene()));
        }

        drawLyricScrollingTextAligned(
            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_, lyricAreaW,
            lyricAreaX, incomingY + newShift, scroll_.lyric(), incomingBrush, incomingOutline,
            incomingGlow, incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
            incomingProgX, transition.fadeIn, lyricAlignmentForScene(scene_), true);
        if (incomingDoubleLine) {
            ID2D1Brush* previewBrush = brushLyricDim_.Get()
                                           ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                           : static_cast<ID2D1Brush*>(brushDim_.Get());
            drawLyricScrollingTextAligned(
                typesetter_.resources().nextLyricLayout_.Get(), nextLyricWidth_, nextLyricHeight_,
                lyricAreaW, lyricAreaX, incomingY + lyricHeight_ + kLyricPreviewGap + newShift,
                0.0f, previewBrush, nullptr, nullptr, nullptr, 0.0f,
                kLyricPreviewOpacity * transition.fadeIn, lyricAlignmentForScene(scene_), true);
        } else if (typesetter_.resources().secondaryLayout_.Get()) {
            drawLyricScrollingTextAligned(
                typesetter_.resources().secondaryLayout_.Get(), secondaryWidth_, secondaryHeight_,
                lyricAreaW, lyricAreaX, incomingY + lyricHeight_ + 1.0f + newShift,
                scroll_.secondary(), brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f,
                transition.fadeIn, lyricAlignmentForScene(scene_), true);
        }
        rt->PopAxisAlignedClip();
    }

    void drawTransitionTextTo(ID2D1DeviceContext* rt, IDWriteTextLayout* layout, float textW,
                              float textH, float areaW, float x, float y, float offset,
                              ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
                              ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
                              float karaokeX = 0.0f, bool singleCopy = false) {
        if (!rt || !layout || !brush || areaW <= 0.0f)
            return;
        y += lyricStarTextShift(layout);
        D2D1_RECT_F clip{x, y, x + areaW, y + textH};
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        float bases[2];
        int n = 0;
        const float centerAnchor = immersiveLyricCenterX();
        auto alignedBase = [&]() {
            float freeW = std::max(0.0f, areaW - textW);
            switch (activeLyricAlignment()) {
            case LyricAlignment::Left:
                return x;
            case LyricAlignment::Right:
                return x + freeW;
            case LyricAlignment::Center:
            default:
                if (centerAnchor >= 0.0f)
                    return std::clamp(centerAnchor - textW * 0.5f, x, x + freeW);
                return x + freeW * 0.5f;
            }
        };
        if (karaokeBrush || singleCopy) {
            bases[n++] = (textW <= areaW) ? alignedBase() : x - offset;
        } else if (textW <= areaW) {
            bases[n++] = alignedBase();
        } else {
            float loopW = textW + kTextPadding * 2.0f;
            bases[n++] = x - offset;
            bases[n++] = x - offset + loopW;
        }
        static constexpr float kDirs[8][2] = {
            {1.0f, 0.0f},  {0.7071f, 0.7071f},   {0.0f, 1.0f},  {-0.7071f, 0.7071f},
            {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}};
        for (int i = 0; i < n; ++i) {
            if (glow) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 2.4f, y + d[1] * 2.4f),
                                       layout, glow);
            }
            if (outline) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 1.2f, y + d[1] * 1.2f),
                                       layout, outline);
            }
        }
        for (int i = 0; i < n; ++i)
            rt->DrawTextLayout(D2D1::Point2F(bases[i], y), layout, brush);
        if (karaokeBrush && karaokeX > 0.0f) {
            rt->PushAxisAlignedClip(D2D1::RectF(bases[0], y, bases[0] + karaokeX, y + textH),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->DrawTextLayout(D2D1::Point2F(bases[0], y), layout, karaokeBrush);
            rt->PopAxisAlignedClip();
        }
        rt->PopAxisAlignedClip();
    }

    void drawLyricBlockSnapshot(ID2D1DeviceContext* dc, bool outgoing, float lyricAreaX,
                                float lyricAreaW, float h) {
        IDWriteTextLayout* main = outgoing ? typesetter_.resources().outgoingLyricLayout_.Get()
                                           : typesetter_.resources().lyricLayout_.Get();
        IDWriteTextLayout* secondary = outgoing
                                           ? typesetter_.resources().outgoingSecondaryLayout_.Get()
                                           : typesetter_.resources().secondaryLayout_.Get();
        if (!main)
            return;
        const float mainW = outgoing ? outgoingLyricWidth_ : lyricWidth_;
        const float mainH = outgoing ? outgoingLyricHeight_ : lyricHeight_;
        const float secondaryW = outgoing ? outgoingSecondaryWidth_ : secondaryWidth_;
        const float secondaryH = outgoing ? outgoingSecondaryHeight_ : secondaryHeight_;
        const float gap = secondary ? 1.0f : 0.0f;
        const float blockH = mainH + gap + secondaryH;
        const float y = h * 0.5f - blockH * 0.5f;
        ID2D1Brush* mainBrush = scene_ == DisplayScene::Idle
                                    ? static_cast<ID2D1Brush*>(brushText_.Get())
                                    : static_cast<ID2D1Brush*>(
                                          brushLyric_.Get() ? brushLyric_.Get() : brushText_.Get());
        float mainOffset = 0.0f;
        float secondaryOffset = 0.0f;
        ID2D1Brush* karaokeBrush = nullptr;
        float karaokeX = 0.0f;
        if (!outgoing) {
            const LyricLine* incomingLine = karaokeLine();
            const bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
            mainBrush =
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : mainBrush;
            if (incomingKaraoke) {
                karaokeBrush = brushLyric_.Get();
                karaokeX = karaokeSmoothStep(*incomingLine);
            }
            mainOffset = scroll_.lyric();
            secondaryOffset = scroll_.secondary();
        } else {
            mainOffset = outgoingLyricScrollOffset_;
            secondaryOffset = outgoingSecondaryScrollOffset_;
        }
        ID2D1Brush* effectOutline = scene_ != DisplayScene::Idle && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                        : nullptr;
        ID2D1Brush* effectGlow = scene_ != DisplayScene::Idle && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get())
                                     : nullptr;
        drawTransitionTextTo(dc, main, mainW, mainH, lyricAreaW, lyricAreaX, y, mainOffset,
                             mainBrush, effectOutline, effectGlow, karaokeBrush, karaokeX,
                             !outgoing);
        if (secondary)
            drawTransitionTextTo(dc, secondary, secondaryW, secondaryH, lyricAreaW, lyricAreaX,
                                 y + mainH + gap, secondaryOffset, brushDim_.Get(), nullptr,
                                 nullptr, nullptr, 0.0f, !outgoing);
    }

    bool prepareLyricTransitionDComp(float lyricAreaX, float lyricAreaW, float h,
                                     float lyricBlockH) {
        if (isVerticalTaskbar() || useDoubleLineLyrics() || transition_.outgoingDoubleLine() ||
            !isLyricTransitionActive() || !typesetter_.resources().outgoingLyricLayout_.Get() ||
            !typesetter_.resources().lyricLayout_.Get() || lastPxW_ <= 0 || lastPxH_ <= 0)
            return false;
        if (!renderer.ensureLyricTransitionLayers(lastPxW_, lastPxH_, lastPxW_, lastPxH_))
            return false;

        if (ID2D1DeviceContext* dc = renderer.beginLyricLayerDraw(0)) {
            drawLyricBlockSnapshot(dc, true, lyricAreaX, lyricAreaW, h);
            if (!renderer.endLyricLayerDraw(0, dc))
                return false;
        } else {
            return false;
        }
        if (ID2D1DeviceContext* dc = renderer.beginLyricLayerDraw(1)) {
            drawLyricBlockSnapshot(dc, false, lyricAreaX, lyricAreaW, h);
            if (!renderer.endLyricLayerDraw(1, dc))
                return false;
        } else {
            return false;
        }

        const float outgoingGap =
            typesetter_.resources().outgoingSecondaryLayout_.Get() ? 1.0f : 0.0f;
        const float outgoingBlockH = outgoingLyricHeight_ + outgoingGap + outgoingSecondaryHeight_;
        const float travel = std::max(lyricBlockH, outgoingBlockH) * scale();
        const float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
        const float durationSec = kLyricTransitionMs / 1000.0f;
        if (!renderer.animateLyricLayer(0, 0.0f, -direction * travel, 1.0f, 0.0f, durationSec))
            return false;
        if (!renderer.animateLyricLayer(1, direction * travel, 0.0f, 0.0f, 1.0f, durationSec))
            return false;
        // 不在此单独 Commit：图层上线随 render() 末尾 present() 的 Commit 与底层新帧
        // 同批生效，避免「图层已上屏、底层旧歌词未撤」的双画帧。动画以提交时刻为起点，
        // 延迟到 present 提交仅差一次绘制耗时，不影响时长。
        renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::Active);
        return true;
    }

    void syncLyricTransitionDComp(bool showControls, float lyricAreaX, float lyricAreaW, float h,
                                  float lyricBlockH) {
        // 图层增删不单独 Commit：改动挂起到 render() 末尾 present() 的 Commit，与承载
        // 歌词的底层新帧同批上屏，避免「图层已撤、底层新帧未上屏」的空窗闪烁。
        // 沉浸切歌期间整组歌曲内容占用同一套合成层；歌词行转场不能再抢占
        // lyricLayers_，否则会把正在滑入的歌曲快照替换掉。
        if (songContentTransition_ && songContentTransition_->compositorLayer)
            return;
        if ((isVerticalTaskbar() && isLyricDCompActive()) || isLyricDCompEndRequested() ||
            (showControls && isLyricDCompActive()) || (isLyricDCompActive() && karaokeLine())) {
            renderer.clearLyricTransitionLayers();
            clearLyricDCompState();
        }
        if (showControls)
            return;
        // 逐字高亮需要每帧跟随真实播放位置；DComp 快照是静态位图，不能在入场期间
        // 更新填充边界，因此逐字歌词转场保留 D2D 路径，普通歌词仍使用合成器动画。
        if (transition_.kind() != LyricTransitionKind::Scene && !isLyricDCompActive() &&
            isLyricTransitionActive() && !useDoubleLineLyrics() &&
            !transition_.outgoingDoubleLine() && !karaokeLine() &&
            typesetter_.resources().outgoingLyricLayout_.Get() &&
            typesetter_.resources().lyricLayout_.Get()) {
            if (!prepareLyricTransitionDComp(lyricAreaX, lyricAreaW, h, lyricBlockH)) {
                renderer.clearLyricTransitionLayers();
                clearLyricDCompState();
            }
        }
    }

    void drawHorizontalSongContent(ID2D1DeviceContext* contentRt, float w, float h, float leftW,
                                   float lyricAreaX, float lyricAreaW, float lyricBlockH,
                                   ID2D1Brush* primaryBrush, bool idleScene,
                                   bool lyricEffectsEnabled, bool showControls, bool showSpectrum) {
        if (!contentRt)
            return;

        ID2D1DeviceContext* previousTarget = drawTargetOverride_;
        drawTargetOverride_ = contentRt;

        // 左侧封面
        const float s = coverSize();
        const float coverX = kCoverPadding;
        const float coverY = (h - s) * 0.5f;
        if (albumCoverVisible_ && !idleScene) {
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(coverX, coverY, s);
            } else {
                const D2D1_RECT_F coverRect = D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
                if (cover_.bitmap() && coverClip_.Get() && coverLayer_.Get()) {
                    contentRt->PushLayer(
                        D2D1::LayerParameters1(D2D1::InfiniteRect(), coverClip_.Get(),
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                               D2D1::Matrix3x2F::Translation(coverX, coverY)),
                        coverLayer_.Get());
                    contentRt->DrawBitmap(cover_.bitmap(), coverRect, 1.0f,
                                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    contentRt->PopLayer();
                } else {
                    D2D1_ROUNDED_RECT rr{coverRect, 4.0f, 4.0f};
                    contentRt->FillRoundedRectangle(rr, brushDim_.Get());
                }
            }
            // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
            drawPlatformIcon(coverX, coverY, s);
        }

        if (songInfoVisible_ && !idleScene) {
            // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
            // 左右边缘都保留渐隐，右缘在分隔线前收束，避免标题/歌手撞到歌词区
            const float infoX = infoStartX();
            const float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
            const float infoGap = 2.0f;
            const float totalInfoH = titleHeight_ + infoGap + artistHeight_;
            const float infoY = (h - totalInfoH) * 0.5f;
            drawScrollingText(typesetter_.resources().titleLayout_.Get(), titleWidth_, titleHeight_,
                              infoW, infoX, infoY, scroll_.title(), brushText_.Get(), nullptr,
                              nullptr, nullptr, 0.0f, 1.0f, LyricAlignment::Center, false,
                              kTextPadding, true, kTextPadding);
            drawScrollingText(typesetter_.resources().artistLayout_.Get(), artistWidth_,
                              artistHeight_, infoW, infoX, infoY + titleHeight_ + infoGap,
                              scroll_.artist(), brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f,
                              1.0f, LyricAlignment::Center, false, kTextPadding, true,
                              kTextPadding);
            drawSongInfoDivider(leftW, h);
        }

        if (showControls) {
            float centers[4]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (inlineControlsLayout(centers, cy, r)) {
                for (int i = 0; i < 3; ++i)
                    drawButton(i, D2D1::Point2F(centers[i], cy), r);
                drawVolumeButton(D2D1::Point2F(centers[3], cy), r);
            }
        } else {
            if (showSpectrum && !isExpandedView()) {
                const float spectrumW = spectrumContentWForScene(scene_, w);
                drawSpectrum(w - embeddedResourceExtra() - spectrumW - kTextPadding, h, spectrumW);
            }
            if (transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionActive() &&
                typesetter_.resources().outgoingLyricLayout_.Get()) {
                drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
            } else if (useDoubleLineLyrics()) {
                drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
            } else if (!isLyricDCompActive()) {
                if (isLyricTransitionActive() &&
                    typesetter_.resources().outgoingLyricLayout_.Get()) {
                    // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                    const LyricTransitionSample transition = lyricTransitionSample();
                    const float movementT = transition.movement;
                    const LyricLine* incomingLine = karaokeLine();
                    const bool incomingKaraoke =
                        incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
                    ID2D1Brush* incomingBrush = incomingKaraoke
                                                    ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                                    : primaryBrush;
                    const float incomingProgX =
                        incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
                    const float outgoingPreviewH =
                        transition_.outgoingDoubleLine() &&
                                typesetter_.resources().outgoingNextLyricLayout_.Get()
                            ? kLyricPreviewGap + outgoingNextLyricHeight_
                        : typesetter_.resources().outgoingSecondaryLayout_.Get()
                            ? 1.0f + outgoingSecondaryHeight_
                            : 0.0f;
                    const float outgoingGap =
                        transition_.outgoingDoubleLine() &&
                                typesetter_.resources().outgoingNextLyricLayout_.Get()
                            ? kLyricPreviewGap
                        : typesetter_.resources().outgoingSecondaryLayout_.Get() ? 1.0f
                                                                                 : 0.0f;
                    const float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                                     ? outgoingLyricBlockHeight_
                                                     : outgoingLyricHeight_ + outgoingPreviewH;
                    const float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                    const float travel = std::max(lyricBlockH, outgoingBlockH);
                    const float oldShift =
                        -static_cast<float>(transition_.direction()) * travel * movementT;
                    const float newShift =
                        static_cast<float>(transition_.direction()) * travel * (1.0f - movementT);
                    drawLyricScrollingText(
                        typesetter_.resources().outgoingLyricLayout_.Get(), outgoingLyricWidth_,
                        outgoingLyricHeight_, lyricAreaW, lyricAreaX, outgoingY + oldShift,
                        outgoingLyricScrollOffset_, primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get() : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                        nullptr, 0.0f, 1.0f - transition.fadeOut);
                    if (transition_.outgoingDoubleLine() &&
                        typesetter_.resources().outgoingNextLyricLayout_.Get()) {
                        ID2D1Brush* previewBrush =
                            brushLyricDim_.Get() ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                                 : static_cast<ID2D1Brush*>(brushDim_.Get());
                        drawLyricScrollingText(
                            typesetter_.resources().outgoingNextLyricLayout_.Get(),
                            outgoingNextLyricWidth_, outgoingNextLyricHeight_, lyricAreaW,
                            lyricAreaX,
                            outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f,
                            previewBrush, nullptr, nullptr, nullptr, 0.0f,
                            kLyricPreviewOpacity * (1.0f - transition.fadeOut));
                    } else if (typesetter_.resources().outgoingSecondaryLayout_.Get()) {
                        drawLyricScrollingText(
                            typesetter_.resources().outgoingSecondaryLayout_.Get(),
                            outgoingSecondaryWidth_, outgoingSecondaryHeight_, lyricAreaW,
                            lyricAreaX, outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                            outgoingSecondaryScrollOffset_, brushDim_.Get(), nullptr, nullptr,
                            nullptr, 0.0f, 1.0f - transition.fadeOut);
                    }
                    // 入场行同步显示真实播放位置的逐字填充，避免转场结束时突然跳色。
                    drawLyricScrollingText(
                        typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                        lyricAreaW, lyricAreaX, h * 0.5f - lyricBlockH * 0.5f + newShift,
                        scroll_.lyric(), incomingBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get() : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                        incomingKaraoke ? brushLyric_.Get() : nullptr, incomingProgX,
                        transition.fadeIn, true);
                    if (typesetter_.resources().secondaryLayout_.Get())
                        drawLyricScrollingText(
                            typesetter_.resources().secondaryLayout_.Get(), secondaryWidth_,
                            secondaryHeight_, lyricAreaW, lyricAreaX,
                            h * 0.5f - lyricBlockH * 0.5f + lyricHeight_ +
                                (typesetter_.resources().secondaryLayout_.Get() ? 1.0f : 0.0f) +
                                newShift,
                            scroll_.secondary(), brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f,
                            transition.fadeIn, true);
                } else {
                    // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                    // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                    const LyricLine* curLine = karaokeLine();
                    const bool karaoke = curLine && brushLyric_.Get() && brushLyricDim_.Get();
                    const float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                    drawLyricScrollingText(
                        typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                        lyricAreaW, lyricAreaX, h * 0.5f - lyricBlockH * 0.5f, scroll_.lyric(),
                        karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get() : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                        karaoke ? brushLyric_.Get() : nullptr, progX);
                    // 翻译/罗马音仅作整行附属文本，不参与逐字裁剪、描边或光晕。
                    if (typesetter_.resources().secondaryLayout_.Get())
                        drawLyricScrollingText(
                            typesetter_.resources().secondaryLayout_.Get(), secondaryWidth_,
                            secondaryHeight_, lyricAreaW, lyricAreaX,
                            h * 0.5f - lyricBlockH * 0.5f + lyricHeight_ +
                                (typesetter_.resources().secondaryLayout_.Get() ? 1.0f : 0.0f),
                            scroll_.secondary(), brushDim_.Get());
                }
            }
        }

        drawTargetOverride_ = previousTarget;
    }

    D2D1_ROUNDED_RECT taskbarBackgroundRect(float w, float h) const {
        const float radius = std::min(kCornerRadius, w * 0.5f);
        return D2D1_ROUNDED_RECT{D2D1::RectF(0.0f, 0.0f, w, h), radius, radius};
    }

    void drawTaskbarBackground(float w, float h, float progressExtent, bool vertical,
                               float dynamicBackgroundW, ID2D1Effect* coverBlurChain) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        const D2D1_ROUNDED_RECT bg = taskbarBackgroundRect(w, h);
        if (isAppBarView() && brushBackground_.Get()) {
            // 扩展模式会在下方提前返回；Dock 面向工作区的一侧要在此处绘制分隔线。
            const bool appDark = fluent::isWindowsAppDarkMode();
            brushBackground_->SetColor(appDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f)
                                               : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f));
            const float pixel = 1.0f / std::max(scale(), 1.0f);
            const float top = appBarEdge_ == AppBarEdge::Top ? std::max(0.0f, h - pixel) : 0.0f;
            rt->FillRectangle(D2D1::RectF(0.0f, top, w, std::min(h, top + pixel)),
                              brushBackground_.Get());
        }
        if (isExpandedView()) {
            // 主背景由 Composition 模糊层绘制；动态装饰仍需绘制到透明宿主上。
            if (taskbarDynamicBackgroundVisible())
                drawIdleQuoteBackground(w, h, dynamicBackgroundW);
            return;
        }

        if (coverBlurChain && coverLayer_.Get() && brushBackground_.Get()) {
            ID2D1RoundedRectangleGeometry* clip = nullptr;
            if (auto* factory = renderer.d2d())
                factory->CreateRoundedRectangleGeometry(bg, &clip);
            if (clip) {
                rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::Matrix3x2F::Identity(),
                                                     coverBackgroundOpacityPct_ / 100.0f),
                              coverLayer_.Get());
                rt->DrawImage(coverBlurChain, D2D1::Point2F(0.0f, 0.0f));
                rt->PopLayer();
                clip->Release();
                brushBackground_->SetColor(lightTheme_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f)
                                                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
                rt->FillRoundedRectangle(bg, brushBackground_.Get());
            }
        } else if (background_ == TaskbarBackground::Solid && brushBackground_.Get()) {
            brushBackground_->SetColor(lightTheme_ ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.85f)
                                                   : D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.85f));
            rt->FillRoundedRectangle(bg, brushBackground_.Get());
        }

        if (!isExpandedView()) {
            rt->FillRoundedRectangle(bg, brushBg_.Get());
            if (mouseOver_ && brushHover_.Get())
                rt->FillRoundedRectangle(bg, brushHover_.Get());
        }

        if (progressBackgroundActive() && brushProgressBg_.Get() && media.durationMs > 0) {
            const float fraction = static_cast<float>(
                std::clamp(static_cast<double>(positionMs_) / static_cast<double>(media.durationMs),
                           0.0, 1.0));
            const float fillExtent = progressExtent * fraction;
            if (fillExtent > 0.5f) {
                const COLORREF c =
                    media.hasDominantColor ? media.dominantColor : fluent::accentColor();
                brushProgressBg_->SetColor(
                    fluent::toD2D(c, progressBackgroundOpacityPct_ / 100.0f));
                const float fillRadius = vertical
                                             ? std::min({bg.radiusX, fillExtent * 0.5f, w * 0.5f})
                                             : std::min({bg.radiusX, fillExtent * 0.5f, h * 0.5f});
                const D2D1_RECT_F fillRect = vertical ? D2D1::RectF(0.0f, 0.0f, w, fillExtent)
                                                      : D2D1::RectF(0.0f, 0.0f, fillExtent, h);
                rt->FillRoundedRectangle(D2D1::RoundedRect(fillRect, fillRadius, fillRadius),
                                         brushProgressBg_.Get());
            }
        }

        if (taskbarDynamicBackgroundVisible())
            drawIdleQuoteBackground(w, h, dynamicBackgroundW);
    }

    bool finishTaskbarFrame(HRESULT hr) {
        if (hr == D2DERR_RECREATE_TARGET) {
            runtime_log::writef(
                L"[taskbar] EndDraw requests device recreate, discarding device resources");
            discardDeviceResources();
            return false;
        }
        if (SUCCEEDED(hr)) {
            const bool startSongTransition = isSongTransitionPending();
            const bool immersiveLayerActive = immersiveSongContentLayerActive();
            if (startSongTransition) {
                // 展开视图的根视觉只承载沉浸模式显隐过渡，切歌不能把它复位。
                if (!isExpandedView())
                    renderer.resetRoot();
                if (!isMinimalMode() && !usesSongContentTransition()) {
                    const float travel = kSongTransitionTravelDip * scale();
                    const bool vertical = isVerticalTaskbar();
                    const float fromX = vertical ? 0.0f : travel;
                    const float fromY =
                        vertical ? (taskbarEdge_ == ABE_LEFT ? -travel : travel) : 0.0f;
                    if (!renderer.animateRoot(fromX, 0.0f, fromY, 0.0f, 0.0f, 1.0f,
                                              kSongTransitionMs / 1000.0f))
                        renderer.resetRoot();
                }
            }
            // Present 失败（设备丢失/重置）时丢弃设备链，下一帧惰性重建
            if (!renderer.present()) {
                discardDeviceResources();
                return false;
            }
            // 只有真正 Present 成功后才消费该阶段；设备丢失时保留 Pending，
            // 下一次资源重建仍会以同一首歌的完整帧启动转场。
            if (startSongTransition) {
                // 合成层已经在运行时，新的切歌请求不能在本帧被旧动画消费；
                // 等当前层收尾后再启动下一张快照。
                if (immersiveLayerActive) {
                    if (songContentTransition_ && !songContentTransition_->pendingConsumed) {
                        setSongTransitionPending(false);
                        songContentTransition_->pendingConsumed = true;
                    }
                } else {
                    setSongTransitionPending(false);
                }
            }
            return true;
        } else {
            runtime_log::writef(L"[taskbar] EndDraw failed: 0x%08X", hr);
            discardDeviceResources();
            return false;
        }
    }

    bool renderVerticalTaskbar(float w, float h, ID2D1Effect* coverBlurChain) {
        auto* rt = renderer.renderTarget();
        if (!rt || w <= 0.0f || h <= 0.0f)
            return false;

        const VerticalLayout layout = verticalLayout();
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        drawTaskbarBackground(w, h, h, true, w, coverBlurChain);

        const bool playbackScene =
            scene_ != DisplayScene::Idle && scene_ != DisplayScene::NoPlayback;
        if (layout.coverSize > 0.0f && playbackScene) {
            const float s = layout.coverSize;
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(layout.coverX, layout.coverY, s);
            } else {
                const D2D1_RECT_F coverRect =
                    D2D1::RectF(layout.coverX, layout.coverY, layout.coverX + s, layout.coverY + s);
                if (cover_.bitmap() && coverClip_.Get() && coverLayer_.Get()) {
                    rt->PushLayer(D2D1::LayerParameters1(
                                      D2D1::InfiniteRect(), coverClip_.Get(),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::Matrix3x2F::Translation(layout.coverX, layout.coverY)),
                                  coverLayer_.Get());
                    rt->DrawBitmap(cover_.bitmap(), coverRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    rt->PopLayer();
                } else if (brushDim_.Get()) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(coverRect, std::min(4.0f, s * 0.18f),
                                                               std::min(4.0f, s * 0.18f)),
                                             brushDim_.Get());
                }
            }
            drawPlatformIcon(layout.coverX, layout.coverY, layout.coverSize);
        }

        if (layout.showControls) {
            for (int i = 0; i < 3; ++i)
                drawButton(i, layout.controlCenters[i], layout.controlRadius);
            drawVolumeButton(layout.controlCenters[3], layout.controlRadius);
        } else {
            drawVerticalLyrics(layout);
        }

        return finishTaskbarFrame(rt->EndDraw());
    }

    bool renderDragPreview(float w, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushBackground_.Get() || !brushDim_.Get() || !brushText_.Get() ||
            !typesetter_.resources().fmtDragPreview_.Get() || w <= 0.0f || h <= 0.0f)
            return false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const float inset = 1.5f;
        const float radius = std::max(1.0f, std::min(kCornerRadius, std::min(w, h) * 0.5f - inset));
        const D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(
            D2D1::RectF(inset, inset, std::max(inset, w - inset), std::max(inset, h - inset)),
            radius, radius);
        brushBackground_->SetColor(lightTheme_ ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.88f)
                                               : D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.82f));
        rt->FillRoundedRectangle(preview, brushBackground_.Get());
        rt->DrawRoundedRectangle(preview, brushDim_.Get(), 1.25f, dragPreviewStroke_.Get());

        const float textInset = 8.0f;
        const D2D1_RECT_F textRect =
            D2D1::RectF(textInset, textInset, std::max(textInset, w - textInset),
                        std::max(textInset, h - textInset));
        if (isVerticalTaskbar()) {
            constexpr wchar_t verticalText[] = L"松\n手\n固\n定\n到\n这\n里";
            rt->DrawTextW(verticalText, _countof(verticalText) - 1,
                          typesetter_.resources().fmtDragPreview_.Get(), textRect, brushText_.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            rt->DrawTextW(kDragPreviewText, _countof(kDragPreviewText) - 1,
                          typesetter_.resources().fmtDragPreview_.Get(), textRect, brushText_.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return finishTaskbarFrame(rt->EndDraw());
    }

    void render() {
        if (applyingAppearance_) {
            appearancePaintPending_ = true;
            return;
        }
        if (!isWindowVisible() || !hwnd)
            return;

        int currentWidth = 0;
        int currentHeight = 0;
        clientPixelSize(currentWidth, currentHeight);
        if (currentHeight > 0)
            syncCompactTaskbarLayout(currentHeight);

        if (isExpandedView() && isSceneResizeActive())
            cancelSceneWindowResize();
        if (isSceneResizeActive())
            updateSceneWindowResize(monotonicNowMs());

        // 先调整窗口，再读取客户区并绑定位图，避免用旧尺寸的位图提交后
        // 把刚刚收缩/扩展的窗口尺寸恢复回去。
        if (isInvalidated(RenderInvalidation::Layout) && !isSceneResizeActive() &&
            !drag_.dragging()) {
            adjustPosition();
            if (!isWindowVisible())
                return;
        }
        ensureImmersiveZOrder();

        int pxW = 0;
        int pxH = 0;
        clientPixelSize(pxW, pxH);
        if (pxW <= 0 || pxH <= 0)
            return;
        const bool vertical = isVerticalTaskbar();
        int logicalPxW = pxW;
        int logicalPxH = pxH;
        if (pxW != lastPxW_ || pxH != lastPxH_) {
            if (isLyricDCompActive())
                requestLyricDCompEnd();
            if (songContentTransition_ && songContentTransition_->compositorLayer)
                clearImmersiveSongContentTransition();
            lastPxW_ = pxW;
            lastPxH_ = pxH;
            requestInvalidation(RenderInvalidation::Geometry);
            if (vertical)
                requestInvalidation(RenderInvalidation::Text);
            // 横向文本布局以超宽无换行创建，度量与窗口尺寸无关；侧边竖排布局
            // 依赖任务栏窄栏宽度，已在上面标记 Text 失效以便重建。
        }
        lastLogicalPxW_ = logicalPxW;
        lastLogicalPxH_ = logicalPxH;

        syncCompactTaskbarLayout(pxH);

        if (!bindRenderer(pxW, pxH))
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        // 预处理和资源创建阶段可能追加失效项；从这里开始才捕获本次提交快照，
        // 避免把本帧刚处理的尺寸/字体失效错误地留到下一帧。
        const InvalidationSnapshot invalidation = renderState_.beginInvalidation();

        if (drag_.dragging()) {
            if (isInvalidated(RenderInvalidation::Text) ||
                isInvalidated(RenderInvalidation::SongInfo))
                buildTextLayouts(0.0f, 0.0f);
            if (updateDragPreviewPlacement()) {
                clientPixelSize(pxW, pxH);
                if (pxW <= 0 || pxH <= 0 || !bindRenderer(pxW, pxH))
                    return;
                rt = renderer.renderTarget();
                if (!rt)
                    return;
                renderer.setDpi(dpi_);
            }
            if (renderDragPreview(dip(pxW), dip(pxH))) {
                // 预览只消费本帧实际处理过的文字/布局/绘制失效；封面、图标与普通
                // 内容几何继续保留，松手恢复歌词后再由完整渲染链处理。
                InvalidationSnapshot previewInvalidation = invalidation;
                previewInvalidation.mask &=
                    toMask(RenderInvalidation::Paint) | toMask(RenderInvalidation::Text) |
                    toMask(RenderInvalidation::SongInfo) | toMask(RenderInvalidation::Layout);
                renderState_.commitInvalidation(previewInvalidation);
            }
            return;
        }

        ensureGeometry();
        if (isInvalidated(RenderInvalidation::Cover))
            decodeCover();
        if (isInvalidated(RenderInvalidation::PlatformIcon))
            decodePlatformIcon();

        if (vertical) {
            if (isInvalidated(RenderInvalidation::Text) ||
                isInvalidated(RenderInvalidation::SongInfo))
                buildTextLayouts(0.0f, 0.0f);
            if (isLyricDCompActive() || isLyricDCompEndRequested()) {
                renderer.clearLyricTransitionLayers();
                clearLyricDCompState();
            }
            ID2D1Effect* coverBlurChain =
                background_ == TaskbarBackground::CoverBlur && cover_.bitmap()
                    ? ensureCoverBlurChain(dip(pxW), dip(pxH))
                    : nullptr;
            if (renderVerticalTaskbar(dip(pxW), dip(pxH), coverBlurChain))
                renderState_.commitInvalidation(invalidation);
            return;
        }

        // 时钟按本地分钟缓存；首帧在这里同步，后续由帧定时器仅在分钟变化时重绘。
        if (!isAppBarView())
            refreshImmersiveClockText();

        LayoutMetrics layout = layoutMetrics(logicalPxW, logicalPxH);
        float w = layout.w;
        float h = layout.h;
        float leftW = layout.leftW;
        float rightW = layout.rightW;

        if (isInvalidated(RenderInvalidation::Text) || isInvalidated(RenderInvalidation::SongInfo))
            buildTextLayouts(leftW, rightW);

        const LyricArea lyricArea = lyricAreaForScene(scene_, logicalPxW, logicalPxH);
        const float lyricAreaX = lyricArea.x;
        const float lyricAreaW = lyricArea.w;
        float secondaryGap = typesetter_.resources().secondaryLayout_.Get() ? 1.0f : 0.0f;
        float lyricBlockH = lyricHeight_ + secondaryGap + secondaryHeight_;
        float lyricY = h * 0.5f - lyricBlockH * 0.5f;
        const bool idleScene = scene_ == DisplayScene::Idle;
        // 每日一言属于任务栏主题正文，不继承上一次播放歌词的用户自定义颜色。
        const bool lyricEffectsEnabled = !idleScene;
        ID2D1Brush* primaryBrush =
            idleScene ? static_cast<ID2D1Brush*>(brushText_.Get())
                      : static_cast<ID2D1Brush*>(brushLyric_.Get() ? brushLyric_.Get()
                                                                   : brushText_.Get());
        // 只有开启悬浮控件且鼠标位于窗口内时才替换歌词，否则保持歌词/频谱视图。
        bool showControls =
            mouseOver_ && controlsOnHover_ && hoverControlStyle_ == HoverControlStyle::Inline &&
            !idleScene && !isExpandedView() &&
            !(transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionActive());
        const bool backgroundSpectrum = !idleScene && spectrumVisible_ && backgroundWaveEnabled();
        // 独立频谱占用歌词区右端；背景波浪不改变窗口宽度和歌词布局。
        bool showSpectrum = !idleScene && spectrumVisible_ && !showControls &&
                            !backgroundSpectrum && !integratedSpectrum();
        const bool bottomSpectrum = !idleScene && spectrumVisible_ && !showControls &&
                                    spectrumStyle_ == SpectrumStyle::BottomBand;
        syncLyricTransitionDComp(showControls, lyricAreaX, lyricAreaW, h, lyricBlockH);

        // 模糊效果链涉及资源创建（CreateEffect），与画刷一样放在 BeginDraw 之前
        ID2D1Effect* coverBlurChain = background_ == TaskbarBackground::CoverBlur && cover_.bitmap()
                                          ? ensureCoverBlurChain(w, h)
                                          : nullptr;

        beginImmersiveSongContentTransition();
        bool compositorSongContent =
            songContentTransition_ && songContentTransition_->compositorLayer;
        bool compositorSongContentPending = false;
        if (usesSongContentTransition() && songContentTransition_ &&
            !songContentTransition_->compositorLayer) {
            // 歌词行转场也使用 lyricLayers_。整首歌切换时先交给歌曲内容层，
            // 保证封面、歌曲信息和歌词成为同一张快照，避免两套合成层互相抢占。
            renderer.clearLyricTransitionLayers();
            clearLyricDCompState();
            if (renderer.ensureLyricTransitionLayers(lastPxW_, lastPxH_, 1, 1)) {
                compositorSongContent = true;
                compositorSongContentPending = true;
            }
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const float dynamicBackgroundW = embeddedResourceOnly() ? w
                                         : showSpectrum || embeddedResourceVisible()
                                             ? std::max(1.0f, lyricAreaX + lyricAreaW)
                                             : w;
        // 背景、进度和动态底图由横竖两种布局共用，避免后续修复只落在一条路径。
        drawTaskbarBackground(w, h, lyricAreaX + lyricAreaW, false, dynamicBackgroundW,
                              coverBlurChain);

        if (backgroundSpectrum) {
            const float waveX = infoStartX();
            const float waveRight = backgroundWaveRight(w);
            const float waveW = std::max(1.0f, waveRight - waveX);
            drawBackgroundWaveSpectrum(waveX, h, waveW);
        }
        if (bottomSpectrum)
            drawBottomSpectrum(lyricAreaX, h, lyricAreaW);

        // 沉浸模式的操作组、频谱和时钟都属于固定遮罩层内容，必须在歌曲内容
        // 过渡变换之前绘制；切歌时只让封面、歌曲信息和歌词内容移动。
        if (isExpandedView()) {
            drawImmersiveControls();
            if (!idleScene)
                drawDockPet(w, h, leftW);
            drawImmersiveRightSide(w, h, showSpectrum);
        } else if (embeddedResourceVisible()) {
            const float resourceWidth =
                std::min(resourceZoneWidth(), std::max(1.0f, w - kDockResourceRightPadding));
            drawDockResourceStatus(w - resourceWidth - kDockResourceRightPadding, h, resourceWidth);
        }

        const bool clipSongContent = embeddedResourceVisible();
        const float songContentRight = w - embeddedResourceExtra();
        if (clipSongContent)
            rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, songContentRight, h),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        bool contentTransitionActive = false;
        if (!compositorSongContent) {
            contentTransitionActive = applyImmersiveSongContentTransform(rt);
        }

        // 左侧封面
        if (!compositorSongContent) {
            float s = coverSize();
            float coverX = kCoverPadding;
            float coverY = (h - s) * 0.5f;
            if (albumCoverVisible_ && !idleScene) {
                if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                    drawVinylCover(coverX, coverY, s);
                } else {
                    D2D1_RECT_F coverRect = D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
                    if (cover_.bitmap() && coverClip_.Get() && coverLayer_.Get()) {
                        rt->PushLayer(
                            D2D1::LayerParameters1(D2D1::InfiniteRect(), coverClip_.Get(),
                                                   D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                   D2D1::Matrix3x2F::Translation(coverX, coverY)),
                            coverLayer_.Get());
                        rt->DrawBitmap(cover_.bitmap(), coverRect, 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        rt->PopLayer();
                    } else {
                        D2D1_ROUNDED_RECT rr{coverRect, 4.0f, 4.0f};
                        rt->FillRoundedRectangle(rr, brushDim_.Get());
                    }
                }
                // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
                drawPlatformIcon(coverX, coverY, s);
            }

            if (songInfoVisible_ && !idleScene) {
                // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
                // 左右边缘都保留渐隐，右缘在分隔线前收束，避免标题/歌手撞到歌词区
                float infoX = infoStartX();
                float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
                float infoGap = 2.0f;
                float totalInfoH = titleHeight_ + infoGap + artistHeight_;
                float infoY = (h - totalInfoH) * 0.5f;
                drawScrollingText(typesetter_.resources().titleLayout_.Get(), titleWidth_,
                                  titleHeight_, infoW, infoX, infoY, scroll_.title(),
                                  brushText_.Get(), nullptr, nullptr, nullptr, 0.0f, 1.0f,
                                  LyricAlignment::Center, false, kTextPadding, true, kTextPadding);
                drawScrollingText(typesetter_.resources().artistLayout_.Get(), artistWidth_,
                                  artistHeight_, infoW, infoX, infoY + titleHeight_ + infoGap,
                                  scroll_.artist(), brushDim_.Get(), nullptr, nullptr, nullptr,
                                  0.0f, 1.0f, LyricAlignment::Center, false, kTextPadding, true,
                                  kTextPadding);
                drawSongInfoDivider(leftW, h);
            }

            if (showControls) {
                float centers[4]{};
                float cy = 0.0f;
                float r = 0.0f;
                if (inlineControlsLayout(centers, cy, r)) {
                    for (int i = 0; i < 3; ++i)
                        drawButton(i, D2D1::Point2F(centers[i], cy), r);
                    drawVolumeButton(D2D1::Point2F(centers[3], cy), r);
                }
            } else {
                if (showSpectrum && !isExpandedView()) {
                    const float spectrumW = spectrumContentWForScene(scene_, w);
                    drawSpectrum(w - embeddedResourceExtra() - spectrumW - kTextPadding, h,
                                 spectrumW);
                }
                if (transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionActive() &&
                    typesetter_.resources().outgoingLyricLayout_.Get()) {
                    drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
                } else if (useDoubleLineLyrics()) {
                    drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
                } else if (!isLyricDCompActive()) {
                    if (isLyricTransitionActive() &&
                        typesetter_.resources().outgoingLyricLayout_.Get()) {
                        // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                        const LyricTransitionSample transition = lyricTransitionSample();
                        const float movementT = transition.movement;
                        const LyricLine* incomingLine = karaokeLine();
                        bool incomingKaraoke =
                            incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
                        ID2D1Brush* incomingBrush =
                            incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                            : primaryBrush;
                        const float incomingProgX =
                            incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
                        const float outgoingPreviewH =
                            transition_.outgoingDoubleLine() &&
                                    typesetter_.resources().outgoingNextLyricLayout_.Get()
                                ? kLyricPreviewGap + outgoingNextLyricHeight_
                            : typesetter_.resources().outgoingSecondaryLayout_.Get()
                                ? 1.0f + outgoingSecondaryHeight_
                                : 0.0f;
                        float outgoingGap =
                            transition_.outgoingDoubleLine() &&
                                    typesetter_.resources().outgoingNextLyricLayout_.Get()
                                ? kLyricPreviewGap
                            : typesetter_.resources().outgoingSecondaryLayout_.Get() ? 1.0f
                                                                                     : 0.0f;
                        float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                                   ? outgoingLyricBlockHeight_
                                                   : outgoingLyricHeight_ + outgoingPreviewH;
                        float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                        float travel = std::max(lyricBlockH, outgoingBlockH);
                        float oldShift =
                            -static_cast<float>(transition_.direction()) * travel * movementT;
                        float newShift = static_cast<float>(transition_.direction()) * travel *
                                         (1.0f - movementT);
                        drawLyricScrollingText(
                            typesetter_.resources().outgoingLyricLayout_.Get(), outgoingLyricWidth_,
                            outgoingLyricHeight_, lyricAreaW, lyricAreaX, outgoingY + oldShift,
                            outgoingLyricScrollOffset_, primaryBrush,
                            lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get()
                                                                 : nullptr,
                            lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                            nullptr, 0.0f, 1.0f - transition.fadeOut);
                        if (transition_.outgoingDoubleLine() &&
                            typesetter_.resources().outgoingNextLyricLayout_.Get()) {
                            ID2D1Brush* previewBrush =
                                brushLyricDim_.Get()
                                    ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                    : static_cast<ID2D1Brush*>(brushDim_.Get());
                            drawLyricScrollingText(
                                typesetter_.resources().outgoingNextLyricLayout_.Get(),
                                outgoingNextLyricWidth_, outgoingNextLyricHeight_, lyricAreaW,
                                lyricAreaX,
                                outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift,
                                0.0f, previewBrush, nullptr, nullptr, nullptr, 0.0f,
                                kLyricPreviewOpacity * (1.0f - transition.fadeOut));
                        } else if (typesetter_.resources().outgoingSecondaryLayout_.Get())
                            drawLyricScrollingText(
                                typesetter_.resources().outgoingSecondaryLayout_.Get(),
                                outgoingSecondaryWidth_, outgoingSecondaryHeight_, lyricAreaW,
                                lyricAreaX,
                                outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                                outgoingSecondaryScrollOffset_, brushDim_.Get(), nullptr, nullptr,
                                nullptr, 0.0f, 1.0f - transition.fadeOut);
                        // 入场行同步显示真实播放位置的逐字填充，避免转场结束时突然跳色。
                        drawLyricScrollingText(
                            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                            lyricAreaW, lyricAreaX, lyricY + newShift, scroll_.lyric(),
                            incomingBrush,
                            lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get()
                                                                 : nullptr,
                            lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                            incomingKaraoke ? brushLyric_.Get() : nullptr, incomingProgX,
                            transition.fadeIn, true);
                        if (typesetter_.resources().secondaryLayout_.Get())
                            drawLyricScrollingText(typesetter_.resources().secondaryLayout_.Get(),
                                                   secondaryWidth_, secondaryHeight_, lyricAreaW,
                                                   lyricAreaX,
                                                   lyricY + lyricHeight_ + secondaryGap + newShift,
                                                   scroll_.secondary(), brushDim_.Get(), nullptr,
                                                   nullptr, nullptr, 0.0f, transition.fadeIn, true);
                    } else {
                        // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                        // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                        const LyricLine* curLine = karaokeLine();
                        bool karaoke = curLine && brushLyric_.Get() && brushLyricDim_.Get();
                        float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                        drawLyricScrollingText(
                            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                            lyricAreaW, lyricAreaX, lyricY, scroll_.lyric(),
                            karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : primaryBrush,
                            lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get()
                                                                 : nullptr,
                            lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                            karaoke ? brushLyric_.Get() : nullptr, progX);
                        // 翻译/罗马音仅作整行附属文本，不参与逐字裁剪、描边或光晕。
                        if (typesetter_.resources().secondaryLayout_.Get())
                            drawLyricScrollingText(typesetter_.resources().secondaryLayout_.Get(),
                                                   secondaryWidth_, secondaryHeight_, lyricAreaW,
                                                   lyricAreaX, lyricY + lyricHeight_ + secondaryGap,
                                                   scroll_.secondary(), brushDim_.Get());
                    }
                }
            }
        }

        if (contentTransitionActive)
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        if (clipSongContent)
            rt->PopAxisAlignedClip();
        // Idle 内容先绘制每日一言，再把宠物放到内容层上方，保证两者同时可见。
        if (isExpandedView() && idleScene)
            drawDockPet(w, h, leftW);

        HRESULT frameHr = rt->EndDraw();
        // 合成器继续负责转场位移，只刷新其中的动态歌词内容，不重启动画。
        if (SUCCEEDED(frameHr) && compositorSongContent && !compositorSongContentPending &&
            lyricEffectsAnimating()) {
            if (auto* layerRt = renderer.beginLyricLayerDraw(0)) {
                drawHorizontalSongContent(layerRt, w, h, leftW, lyricAreaX, lyricAreaW, lyricBlockH,
                                          primaryBrush, idleScene, lyricEffectsEnabled,
                                          showControls, showSpectrum);
                if (!renderer.endLyricLayerDraw(0, layerRt))
                    clearImmersiveSongContentTransition();
            }
        }
        if (SUCCEEDED(frameHr) && compositorSongContentPending) {
            bool layerReady = false;
            if (ID2D1DeviceContext* layerRt = renderer.beginLyricLayerDraw(0)) {
                drawHorizontalSongContent(layerRt, w, h, leftW, lyricAreaX, lyricAreaW, lyricBlockH,
                                          primaryBrush, idleScene, lyricEffectsEnabled,
                                          showControls, showSpectrum);
                layerReady = renderer.endLyricLayerDraw(0, layerRt);
            }
            if (layerReady) {
                const float travelPx = kSongTransitionTravelDip * scale();
                layerReady = renderer.animateLyricLayerX(
                    0, travelPx, 0.0f, 0.0f, 1.0f, 1.0f, kSongTransitionMs / 1000.0f,
                    clipSongContent ? songContentRight * scale() : 0.0f);
            }
            if (layerReady && songContentTransition_) {
                songContentTransition_->compositorLayer = true;
                // DComp 动画的真实起点在本次 Present/Commit 附近；快照绘制可能耗时，
                // 不能沿用创建转场时的时间，否则动画会被提前收尾。
                songContentTransition_->startMs = monotonicNowMs();
            } else {
                renderer.clearLyricTransitionLayers();
                clearLyricDCompState();
                // 合成层创建失败时回到原有 D2D 路径，当前帧仍然完整可见。
                rt->BeginDraw();
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
                drawTaskbarBackground(w, h, lyricAreaX + lyricAreaW, false, dynamicBackgroundW,
                                      coverBlurChain);
                if (backgroundSpectrum) {
                    const float waveX = infoStartX();
                    const float waveRight = backgroundWaveRight(w);
                    const float waveW = std::max(1.0f, waveRight - waveX);
                    drawBackgroundWaveSpectrum(waveX, h, waveW);
                }
                if (bottomSpectrum)
                    drawBottomSpectrum(lyricAreaX, h, lyricAreaW);
                if (isExpandedView()) {
                    drawImmersiveControls();
                    if (!idleScene)
                        drawDockPet(w, h, leftW);
                    drawImmersiveRightSide(w, h, showSpectrum);
                } else if (embeddedResourceVisible()) {
                    const float resourceWidth = std::min(
                        resourceZoneWidth(), std::max(1.0f, w - kDockResourceRightPadding));
                    drawDockResourceStatus(w - resourceWidth - kDockResourceRightPadding, h,
                                           resourceWidth);
                }
                if (clipSongContent)
                    rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, songContentRight, h),
                                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                contentTransitionActive = applyImmersiveSongContentTransform(rt);
                // 失败回退时仍需绘制歌曲内容；这里复用与快照相同的绘制入口，
                // 避免回退路径和正常路径再次产生视觉差异。
                drawHorizontalSongContent(rt, w, h, leftW, lyricAreaX, lyricAreaW, lyricBlockH,
                                          primaryBrush, idleScene, lyricEffectsEnabled,
                                          showControls, showSpectrum);
                if (contentTransitionActive)
                    rt->SetTransform(D2D1::Matrix3x2F::Identity());
                if (clipSongContent)
                    rt->PopAxisAlignedClip();
                if (isExpandedView() && idleScene)
                    drawDockPet(w, h, leftW);
                frameHr = rt->EndDraw();
            }
        }
        if (finishTaskbarFrame(frameHr))
            renderState_.commitInvalidation(invalidation);
    }

    // ---------- 滚动字幕 ----------

    void updateScroll() {
        const ULONGLONG now = monotonicNowMs();
        frameNowMs_ = now;
        if (isLyricTransitionInProgress() && transition_.revision() != 0 &&
            transition_.revision() != frameRevision_) {
            // 不存在历史过渡队列：版本过期时直接丢弃旧过渡，下一次排版只处理当前行。
            resetLyricTransition();
            requestInvalidation(RenderInvalidation::Text);
        }

        const bool wasTransitioning = isLyricTransitionInProgress();
        if (isLyricTransitionActive() &&
            now - transition_.startMs() >= static_cast<ULONGLONG>(lyricTransitionDurationMs())) {
            finalizeLyricTransition(now);
        }

        TextScrollInput input;
        input.now = now;
        input.animations = clientAnimations_;
        input.wasTransitioning = wasTransitioning;
        input.hold = isLyricTransitionInProgress();
        input.vertical = isVerticalTaskbar();
        input.playing = media.playing;
        input.idle = scene_ == DisplayScene::Idle;
        input.karaoke = karaokeLine() != nullptr;
        input.karaokeProgress = karaoke_.progressForLine(currentLine);
        input.songInfoVisible = songInfoVisible_;
        input.showInlineControls =
            mouseOver_ && controlsOnHover_ && hoverControlStyle_ == HoverControlStyle::Inline;
        input.titleWidth = titleWidth_;
        input.artistWidth = artistWidth_;
        input.lyricWidth = lyricWidth_;
        input.secondaryWidth = secondaryWidth_;
        int width = 0, height = 0;
        logicalClientPixelSize(width, height);
        if (input.vertical) {
            const auto layout = verticalLayout();
            input.area = layout.lyricBottom - layout.lyricY;
            input.verticalExtent = verticalLyricHeight_ > 0.0f ? verticalLyricHeight_ : lyricWidth_;
            input.showControls = layout.showControls;
        } else {
            const auto layout = layoutMetrics(width, height);
            input.areaValid = layout.w > 0.0f && layout.h > 0.0f;
            input.infoWidth = std::max(1.0f, layout.leftW - infoStartX() - kTextPadding);
            input.area = lyricAreaForScene(scene_, width, height).w;
        }
        scroll_.advance(input);
    }

    void updateVinylRotation() {
        if (vinyl_.advance(monotonicNowMs(), media.playing,
                           albumCoverEffect_ == AlbumCoverEffect::Vinyl && albumCoverVisible_ &&
                               clientAnimations_))
            requestFrame();
    }

    // ---------- 事件 ----------

    void updateDisplayRefresh() {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency >= 24)
            displayRefreshHz_ = mode.dmDisplayFrequency;
    }

    void refreshFrameTimer() {
        if (!timerRunning_ || !hwnd)
            return;
        const UINT wantMs = desiredFrameMs();
        if (wantMs == timerMs_)
            return;
        SetTimer(hwnd, kTimerId, wantMs, nullptr);
        timerMs_ = wantMs;
    }

    UINT activeFrameMs() const {
        // 只有低渲染模式固定 ~30fps；极简模式仍使用正常刷新率，歌词帧率不变。
        if (isRenderMode(RenderMode::Low))
            return kTimerPausedMs;
        const UINT hz = displayRefreshHz_ ? displayRefreshHz_ : 60;
        return std::clamp(1000 / hz, kTimerMinMs, kTimerMs);
    }

    bool hasHighFrequencyAnimation() const {
        const bool songLayerActive = immersiveSongContentLayerActive();
        if (vinyl_.armMoving() && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        if (horizontalImmersiveMode() && clientAnimations_ && dockPet_.animating())
            return true;
        if (isLyricTransitionInProgress() && !songLayerActive)
            return true;
        if (isSceneResizeActive())
            return true;
        if (songContentTransition_ && !songContentTransition_->compositorLayer)
            return true;
        if (taskbarDynamicBackgroundAnimating() || backgroundWaveAnimating())
            return true;
        if (spectrumVisualsAnimating() || spectrumEffectsAnimating() || lyricEffectsAnimating())
            return true;
        if (media.playing && scroll_.animating() && !songLayerActive)
            return true;
        if (media.playing && clientAnimations_ && karaokeLine() && !songLayerActive)
            return true;
        if (media.playing && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        // 播放中的进度背景每帧都在推进
        if (media.playing && progressBackgroundActive())
            return true;
        if (media.playing && spectrumVisible_) {
            for (float v : spectrum_.bands())
                if (v > 0.01f)
                    return true;
        }
        return false;
    }

    UINT desiredFrameMs() const {
        return hasHighFrequencyAnimation() ? activeFrameMs() : kTimerPausedMs;
    }

    // 帧定时器只在窗口可见时运行：隐藏后的固定间隔 tick（进度插值、滚动、
    // 快照读取）没有任何可见效果，可见性恢复由 SMTC 事件驱动（不依赖定时器）。
    void startFrameTimer() {
        if (!timerRunning_ && hwnd) {
            updateDisplayRefresh();
            const UINT initialMs = desiredFrameMs();
            SetTimer(hwnd, kTimerId, initialMs, nullptr);
            timerRunning_ = true;
            timerMs_ = initialMs;
        }
    }

    void stopFrameTimer() {
        if (timerRunning_ && hwnd) {
            KillTimer(hwnd, kTimerId);
            timerRunning_ = false;
            timerMs_ = 0;
        }
    }

    void setRenderMode(RenderMode mode) {
        if (mode == renderState_.mode())
            return;
        const bool leavingStopped = isStoppedMode() && mode != RenderMode::Stopped;
        const bool wasMinimal = isMinimalMode();
        renderState_.setMode(mode);
        if (leavingStopped) {
            if (resourceMonitoringEnabled())
                startResourceMonitoring();
            if (isAppBarView()) {
                renderState_.setVisibilitySuppressed(false);
                setPlacementStatus(TaskbarPlacementStatus::Safe);
            } else {
                // 完全停止期间避让缓存可能已经过期。恢复显示前必须重新等待一次
                // 真实探测，不能按旧的“有空间”结果先显示一帧再隐藏。
                renderState_.setVisibilitySuppressed(true);
                probeReady_ = false;
                placementProbe_.clearResult();
            }
            stopFrameTimer();
            stopPlacementTimer();
            reconcileWindowVisibility();
        }
        if (wasMinimal != isMinimalMode()) {
            // 极简关闭逐字绘制后，清掉当前帧的逐字状态；退出时由重新排版恢复几何缓存。
            requestInvalidation(RenderInvalidation::Text);
            karaoke_.clearGeometry();
            karaoke_.reset();
        }
        const bool popupAvailable = mediaPopupAvailable(isSessionVisible());
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setIdleContent(idle, popupAvailable);
        mediaPopup.setMedia(media, popupAvailable);
        mediaPopup.setPresentationMode(scene_, popupAvailable,
                                       hoverControlStyle_ == HoverControlStyle::Inline);
        mediaPopup.endPresentationUpdate();
        syncMediaPopupEnabled();
        if (isMinimalMode()) {
            // 极简仍保留歌词和方形封面，因此不释放主任务栏渲染器；只清掉附加背景链和媒体卡片。
            volumeHover_ = false;
            volumePopup_.hide();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            releaseCoverBackgroundResources();
            // 切歌转场只改变合成器根视觉；进入极简时立即恢复到静止位置。
            setSongTransitionPending(false);
            clearImmersiveSongContentTransition();
            renderer.resetRoot();
        }
        if (mode == RenderMode::Stopped) {
            // 完全停止：隐藏窗口、停帧定时器、释放 GPU 设备链。此后 SMTC 事件仍更新
            // 内存中的歌词/媒体数据，但 render() 因窗口隐藏直接早退，不占 GPU/CPU
            volumeHover_ = false;
            volumePopup_.hide();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            reconcileWindowVisibility();
            stopPlacementTimer();
            stopResourceMonitoring();
            releaseAll();
            return;
        }
        // 帧定时器运行中则立即按新档位调整间隔（正常/极简=跟随刷新率，低渲染=~30fps）
        if (timerRunning_) {
            const UINT wantMs = desiredFrameMs();
            if (wantMs != timerMs_) {
                SetTimer(hwnd, kTimerId, wantMs, nullptr);
                timerMs_ = wantMs;
            }
        }
        if (!isAppBarView() && renderState_.visibilitySuppressed() && !timerRunning_)
            startPlacementTimer();
        // 从完全停止恢复：按最近会话可见性立即还原窗口；设备链由 render() 惰性重建
        reconcileWindowVisibility();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    // 静止判定：所有动画源都停止且没有待处理的布局/资源变化时，跳过整帧重绘。
    // 跳过时屏幕上保持上一次 DirectComposition 提交的内容，不会闪烁或丢状态。
    bool needsFrameRender() const {
        const bool songLayerActive = immersiveSongContentLayerActive();
        if (vinyl_.armMoving() && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        if (renderState_.hasInvalidation())
            return true;
        if (horizontalImmersiveMode() && clientAnimations_ && dockPet_.animating())
            return true;
        if (isLyricTransitionInProgress() && !songLayerActive)
            return true;
        if (isSceneResizeActive())
            return true;
        if (songContentTransition_ && !songContentTransition_->compositorLayer)
            return true;
        if (taskbarDynamicBackgroundAnimating() || backgroundWaveAnimating())
            return true;
        if (scroll_.animating() && !songLayerActive)
            return true;
        // 逐字平滑未收敛（暂停 seek 后）时渲染到收敛为止
        if (karaokeLine() && !karaoke_.settled() && !songLayerActive)
            return true;
        // 播放中的逐字推进和黑胶旋转每帧都在变
        if (media.playing && !songLayerActive &&
            (karaokeLine() || (clientAnimations_ && albumCoverVisible_ &&
                               albumCoverEffect_ == AlbumCoverEffect::Vinyl)))
            return true;
        // 进度背景随播放进度持续推进
        if (media.playing && progressBackgroundActive())
            return true;
        // 频谱柱等静音衰减到阈值以下才算静止
        if (spectrumVisualsAnimating() || spectrumEffectsAnimating() || lyricEffectsAnimating())
            return true;
        if (spectrumVisible_) {
            for (float v : spectrum_.bands())
                if (v > 0.01f)
                    return true;
        }
        return false;
    }

    void processPlacementProbe() {
        updateDisplayRefresh();
        const bool probeWasReady = probeReady_;
        bool changed = detectChanges();
        if (pickProbeResult())
            changed = true;
        if (changed)
            adjustPosition();
        if (!probeWasReady && probeReady_) {
            // 首次真实探测结果到达后再决定是否显示；否则创建期间可能先按
            // 空占用区显示一帧，随后被 UIA/窗口探测结果立即隐藏而产生闪烁。
            if (onPlacementStatusChanged_)
                onPlacementStatusChanged_(placementStatus_);
        }
        if (probeReady_)
            releaseVisibilitySuppression();
    }

    void onTimer() {
        if (tick)
            tick();
        finishImmersiveSongContentTransitionIfNeeded();
        // TrafficMonitor 可能在两次渲染之间调整自己的任务栏子窗口 Z 序；
        // 即使当前画面静止，也要及时把沉浸宿主恢复到最上层。
        ensureImmersiveZOrder();
        // 只有可见动画源跟随当前显示器刷新率（封顶 ~125fps）；静态播放降到 ~30fps，
        // 避免歌词没有动画时仍高频轮询和提交。状态事件（悬停、恢复播放、seek）走同步
        // render 路径，不依赖定时器，低档位下也不会延迟显示。
        const UINT wantMs = desiredFrameMs();
        if (timerRunning_ && wantMs != timerMs_) {
            SetTimer(hwnd, kTimerId, wantMs, nullptr);
            timerMs_ = wantMs;
        }
        frameNowMs_ = monotonicNowMs();
        updateSpectrumVisuals(frameNowMs_);
        updateSpectrumEffects(frameNowMs_);
        const bool lyricAnimating = lyricEffectsAnimating();
        if (lyricAnimating || lyricFxWasAnimating_)
            requestFrame();
        lyricFxWasAnimating_ = lyricAnimating;
        dockPet_.tick(frameNowMs_);
        updateVinylRotation();
        if (refreshResourceSnapshot())
            requestFrame();
        // 任务栏位置/DPI/主题跟踪与避让探测结果拾取，放到慢速分支，不跟 60fps 走
        if (++slowTick_ >= kSlowTickInterval) {
            slowTick_ = 0;
            processPlacementProbe();
        }
        updateScroll();
        if (!isAppBarView() && refreshImmersiveClockText())
            requestFrame();
        const bool statusCycleCallbackHandled = scroll_.consumeCompleted();
        if (statusCycleCallbackHandled) {
            if (onStatusTextCycleCompleted_)
                onStatusTextCycleCompleted_();
        }
        // 静止场景跳过整帧重绘：动画源全部停止且无脏状态时，画面保持上一帧内容
        if (!statusCycleCallbackHandled && needsFrameRender())
            requestFrameAndFlush();
    }

    void onPlacementTimer() {
        if (isStoppedMode() || !hwnd)
            return;
        processPlacementProbe();
    }

    void trackMouseLeave() {
        if (trackingLeave_)
            return;
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        trackingLeave_ = true;
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            // 普通创建保持不可见；若正在等待首次避让探测，定时器会由
            // createWindow() 启动，但仍禁止提交可见窗口。
            return 0;
        case WM_TIMER:
            if (wp == kTimerId)
                onTimer();
            else if (wp == kPlacementTimerId)
                onPlacementTimer();
            else if (wp == kTaskbarAttachTimerId)
                retryTaskbarAttach();
            else if (wp == kImmersiveYieldTimerId)
                onImmersiveYieldTimer();
            else if (wp == kImmersivePresenceTimerId)
                onImmersivePresenceTimer();
            return 0;
        case WM_DISPLAYCHANGE:
            // 显示拓扑/分辨率变化（远程接入、虚拟屏开关等）是设备丢失的前置信号，
            // 记录下来便于和随后的 device lost / 重建日志对照。
            runtime_log::writef(
                L"[display] changed: %lux%lu %lubpp", static_cast<unsigned long>(LOWORD(lp)),
                static_cast<unsigned long>(HIWORD(lp)), static_cast<unsigned long>(wp));
            findTaskbar();
            updateDisplayRefresh();
            if (isAppBarView() && appBar_.registered())
                updateAppBarPosition();
            else
                adjustPosition();
            // 不等待歌词、频谱等下一次内容变化才发现客户区尺寸已改变。立即按新
            // 任务栏几何重算安全区，并让 renderer.bind() 在下一帧同步交换链尺寸。
            requestInvalidation(toMask(RenderInvalidation::Layout) |
                                toMask(RenderInvalidation::Geometry));
            requestFrameAndFlush();
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                // SetWindowPos/Explorer/AppBar 都可能直接改变客户区；显式唤醒一帧，
                // 避免 DXGI_SCALING_STRETCH 暂时拉伸旧帧直到下一句歌词出现。
                requestInvalidation(RenderInvalidation::Geometry);
                requestFrame();
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_DPICHANGED:
            if (isAppBarView() && appBar_.registered()) {
                dpi_ = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
                renderer.setDpi(dpi_);
                requestInvalidation(toMask(RenderInvalidation::Layout) |
                                    toMask(RenderInvalidation::Text) |
                                    toMask(RenderInvalidation::Cover));
                updateAppBarPosition();
                requestFrameAndFlush();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ACTIVATE:
            if (appBar_.registered()) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                data.lParam = LOWORD(wp) != WA_INACTIVE;
                SHAppBarMessage(ABM_ACTIVATE, &data);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_WINDOWPOSCHANGED:
            if (appBar_.registered()) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            // 整个窗口区域都视为客户区，让透明背景也能接收鼠标消息
            return HTCLIENT;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && drag_.dragging()) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_MOUSEMOVE: {
            POINT cursor{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &cursor);
            if (drag_.pressed()) {
                const int thresholdX = std::max(GetSystemMetrics(SM_CXDRAG),
                                                (int)std::lround(kLyricDragThresholdDip * scale()));
                const int thresholdY = std::max(GetSystemMetrics(SM_CYDRAG),
                                                (int)std::lround(kLyricDragThresholdDip * scale()));
                if (drag_.move(cursor, thresholdX, thresholdY))
                    beginLyricDrag();
                if (drag_.dragging() && updateDragPreviewPlacement())
                    requestFrameAndFlush();
                return 0;
            }
            bool wasOver = mouseOver_;
            mouseOver_ = true;
            if (mediaPopupEnabledForScene())
                mediaPopup.onAnchorEnter();
            if (!wasOver)
                requestFrameAndFlush();
            trackMouseLeave();
            const float mouseX = static_cast<float>(GET_X_LPARAM(lp));
            const float mouseY = static_cast<float>(GET_Y_LPARAM(lp));
            const int immersiveHover = isExpandedView() ? hitImmersiveControl(mouseX, mouseY) : -1;
            if (immersiveHover != immersiveControlHover_) {
                immersiveControlHover_ = immersiveHover;
                requestFrameAndFlush();
            }
            if (viewMode_ == TaskbarViewMode::Immersive)
                updateImmersiveYield(hitImmersiveControl(mouseX, mouseY, true));
            // 内嵌控件的音量按钮：悬停弹出音量滑块浮窗
            const bool volHover = isExpandedView() ? immersiveHover == kImmersiveControlVolume
                                                   : hitVolumeButton(mouseX, mouseY);
            const int inlineHover = isExpandedView() ? -1
                                    : volHover       ? 3
                                                     : hitButton(mouseX, mouseY);
            if (inlineHover != inlineControlHover_) {
                inlineControlHover_ = inlineHover;
                requestFrameAndFlush();
            }
            if (volHover != volumeHover_) {
                volumeHover_ = volHover;
                if (!volHover)
                    volumePopup_.onAnchorLeave();
            }
            if (volHover && appVolume_.available && immersiveYieldAllowedState()) {
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect(), isVerticalTaskbar(),
                                      taskbarEdge_ == ABE_LEFT);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (drag_.pressed() || drag_.dragging())
                return 0;
            cancelImmersiveYield();
            mouseOver_ = false;
            trackingLeave_ = false;
            volumeHover_ = false;
            immersiveControlHover_ = -1;
            inlineControlHover_ = -1;
            volumePopup_.onAnchorLeave();
            mediaPopup.onAnchorLeave();
            requestFrameAndFlush();
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮消息使用屏幕坐标；音量图标上滚动直接调整应用音量（每格 ±2）
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const bool volumeHit =
                isExpandedView()
                    ? hitImmersiveControl(static_cast<float>(pt.x), static_cast<float>(pt.y)) ==
                          kImmersiveControlVolume
                    : hitVolumeButton(static_cast<float>(pt.x), static_cast<float>(pt.y));
            if (appVolume_.available && onAppVolume && volumeHit) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0)
                    onAppVolume(std::clamp(appVolume_.percent + steps * 2, 0, 100));
                return 0;
            }
            if (dockResourceExtrasVisible() &&
                hitDockResourceArea(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0)
                    advanceDockResourcePage(steps);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // 点击与拖动从同一次按下并行识别：未越过系统拖动阈值仍按原按钮/卡片
            // 点击处理，越过阈值后才取消点击并进入位置拖拽。
            if (isExpandedView()) {
                immersiveControlPressed_ = hitImmersiveControl(
                    static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
                if (immersiveControlPressed_ >= 0) {
                    if (viewMode_ == TaskbarViewMode::Immersive)
                        cancelImmersiveHide();
                    SetCapture(hwnd);
                }
                requestFrameAndFlush();
                return 0;
            }
            POINT pressPoint{};
            GetCursorPos(&pressPoint);
            drag_.press(pressPoint, positionMode_);
            SetCapture(hwnd);
            if (inlineControlHover_ >= 0)
                requestFrameAndFlush();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (isExpandedView()) {
                const int pressed = immersiveControlPressed_;
                const int released = hitImmersiveControl(static_cast<float>(GET_X_LPARAM(lp)),
                                                         static_cast<float>(GET_Y_LPARAM(lp)));
                immersiveControlPressed_ = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                // 沉浸层可能在一次任务栏点击的按下与松开之间出现。只有本窗口
                // 确实收到同一控件的按下事件，才把随后到达的松开解释为点击。
                if (pressed >= 0 && pressed == released)
                    activateImmersiveControl(pressed);
                requestFrameAndFlush();
                return 0;
            }
            const bool hadPress = drag_.pressed() || drag_.dragging();
            const bool wasDragging = drag_.dragging();
            if (hadPress)
                finishLyricDrag(true);
            if (hadPress && inlineControlHover_ >= 0)
                requestFrameAndFlush();
            if (wasDragging)
                return 0;
            if (hadPress) {
                RECT client{};
                POINT releasePoint{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                GetClientRect(hwnd, &client);
                if (!PtInRect(&client, releasePoint))
                    return 0;
            }
            float mx = static_cast<float>(GET_X_LPARAM(lp));
            float my = static_cast<float>(GET_Y_LPARAM(lp));
            int btn = hitButton(mx, my);
            if (btn >= 0 && onControl)
                onControl(static_cast<MediaControl>(btn));
            // 卡片的当前页面自己判断是否为点击展开；这样无播放时的每日一言卡片
            // 可以独立于媒体控件样式使用点击展开。
            else if (!isMinimalMode() && !isExpandedView())
                mediaPopup.onAnchorClick();
            return 0;
        }
        case WM_CAPTURECHANGED:
            immersiveControlPressed_ = -1;
            if (drag_.pressed() || drag_.dragging())
                finishLyricDrag(false);
            return 0;
        case WM_CANCELMODE: {
            const bool hadImmersivePress = immersiveControlPressed_ >= 0;
            immersiveControlPressed_ = -1;
            if (hadImmersivePress && GetCapture() == hwnd)
                ReleaseCapture();
            if (drag_.pressed() || drag_.dragging())
                finishLyricDrag(false);
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (!contextMenuEnabled_ || !onContextMenu)
                return 0;

            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }

            openTaskbarMenu(pt);
            return 0;
        }
        case kAppBarCallbackMessage:
            if (!appBar_.registered())
                return 0;
            switch (wp) {
            case ABN_POSCHANGED:
                updateAppBarPosition();
                break;
            case ABN_FULLSCREENAPP:
                appBar_.setFullscreen(lp != FALSE);
                runtime_log::writef(L"[dock] fullscreen %s; z-order=%s",
                                    appBar_.fullscreen() ? L"opened" : L"closed",
                                    appBar_.fullscreen() ? L"bottom" : L"topmost");
                updateAppBarZOrder();
                break;
            case ABN_STATECHANGE:
                updateAppBarZOrder();
                break;
            case ABN_WINDOWARRANGE:
                if (lp)
                    ShowWindow(hwnd, SW_HIDE);
                else if (shouldShowWindow())
                    ShowWindow(hwnd, SW_SHOWNA);
                break;
            default:
                break;
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                quitting = true;
                if (hwnd)
                    DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            quitting = true;
            if (hwnd)
                DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            runtime_log::writef(L"[taskbar] WM_DESTROY (visible=%d)", isWindowVisible() ? 1 : 0);
            resetImmersivePresence();
            dockBackdrop_.reset();
            renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
            unregisterAppBar();
            KillTimer(hwnd, kTaskbarAttachTimerId);
            attachment_.reset();
            stopFrameTimer();
            stopPlacementTimer();
            stopProbe();
            stopResourceMonitoring();
            drag_.end();
            volumePopup_.destroy();
            mediaPopup.destroy();
            releaseAll();
            hwnd = nullptr;
            if (quitting)
                PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(cs->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
};

void TaskbarHost::applyAppearance(const TaskbarAppearance& appearance) {
    impl_->applyAppearance(appearance);
}

TaskbarHost::TaskbarHost() : impl_(std::make_unique<Impl>()) {}

TaskbarHost::~TaskbarHost() {
    if (impl_)
        impl_->stopProbe();
    if (impl_ && impl_->hwnd)
        DestroyWindow(impl_->hwnd);
}

bool TaskbarHost::create(HINSTANCE inst) {
    return impl_->createWindow(inst, L"", TaskbarViewMode::Embedded);
}

bool TaskbarHost::create(HINSTANCE inst, const std::wstring& monitorDeviceName,
                         TaskbarViewMode initialViewMode) {
    return impl_->createWindow(inst, monitorDeviceName, initialViewMode);
}

HWND TaskbarHost::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

void TaskbarHost::setTickCallback(std::function<void()> cb) { impl_->tick = std::move(cb); }

void TaskbarHost::applyPresentationFrame(const PresentationFrame& frame) {
    impl_->applyPresentationFrame(frame);
}

void TaskbarHost::applyPlaybackPatch(const PlaybackPatch& patch) {
    impl_->applyPlaybackPatch(patch);
}

void TaskbarHost::applySpectrumPatch(const SpectrumPatch& patch) {
    impl_->applySpectrumPatch(patch);
}

void TaskbarHost::setControlCallback(std::function<void(MediaControl)> cb) {
    impl_->onControl = cb;
    impl_->mediaPopup.setControlCallback(std::move(cb));
}

void TaskbarHost::setImmersiveExitCallback(std::function<void()> cb) {
    impl_->onImmersiveExit = std::move(cb);
}

void TaskbarHost::setAppVolume(const AppVolumeState& state) {
    const bool changed = state.available != impl_->appVolume_.available ||
                         state.percent != impl_->appVolume_.percent ||
                         state.muted != impl_->appVolume_.muted;
    impl_->appVolume_ = state;
    impl_->volumePopup_.setVolume(state.percent, state.muted, state.available);
    impl_->mediaPopup.setAppVolume(state);
    if (!state.available)
        impl_->volumePopup_.hide();
    if (changed) {
        impl_->requestFrame();
        impl_->flushRenderRequest();
    }
}

void TaskbarHost::setAppVolumeCallback(std::function<void(int)> cb) {
    impl_->onAppVolume = cb;
    impl_->volumePopup_.setCallback(cb);
    impl_->mediaPopup.setAppVolumeCallback(std::move(cb));
}

void TaskbarHost::setSourceOpenCallback(std::function<void(const std::wstring&)> cb) {
    impl_->mediaPopup.setSourceOpenCallback(std::move(cb));
}

void TaskbarHost::setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb) {
    impl_->mediaPopup.setIdleAppOpenCallback(std::move(cb));
}

void TaskbarHost::setIdleTaskOpenCallback(std::function<void(const IdleTaskInfo&)> cb) {
    impl_->mediaPopup.setIdleTaskOpenCallback(std::move(cb));
}

void TaskbarHost::setIdleTaskCompleteCallback(std::function<void(const IdleTaskInfo&)> cb) {
    impl_->mediaPopup.setIdleTaskCompleteCallback(std::move(cb));
}

void TaskbarHost::setMediaPopupOpenedCallback(std::function<void()> cb) {
    impl_->mediaPopup.setPanelOpenedCallback(std::move(cb));
}

void TaskbarHost::setContextMenuCallback(std::function<void(POINT)> cb) {
    impl_->onContextMenu = std::move(cb);
}

void TaskbarHost::setImmersiveMenuCallback(std::function<void(POINT)> cb) {
    impl_->onImmersiveMenu = std::move(cb);
}

void TaskbarHost::setDockQuickAppsCallback(std::function<void(POINT)> cb) {
    impl_->onDockQuickApps = std::move(cb);
}

void TaskbarHost::setPositionModeChangedCallback(std::function<void(int)> cb) {
    impl_->onPositionModeChanged = std::move(cb);
}

void TaskbarHost::setStatusTextCycleCompletedCallback(std::function<void()> cb) {
    impl_->onStatusTextCycleCompleted_ = std::move(cb);
}

void TaskbarHost::setPlacementStatusCallback(std::function<void(TaskbarPlacementStatus)> cb) {
    impl_->onPlacementStatusChanged_ = std::move(cb);
}

void TaskbarHost::setAllowOverlap(bool on) {
    impl_->allowOverlap_ = on;
    if (impl_->reconcileWindowVisibility() && impl_->isWindowVisible())
        impl_->requestFrameAndFlush();
}

void TaskbarHost::setVisibilitySuppressed(bool on) { impl_->setVisibilitySuppressed(on); }

TaskbarPlacementStatus TaskbarHost::refreshPlacement() { return impl_->refreshPlacement(); }

TaskbarPlacementStatus TaskbarHost::placementStatus() const { return impl_->placementStatus_; }

bool TaskbarHost::isDisplayed() const {
    return impl_ && impl_->isWindowVisible() && impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

const std::vector<LyricLine>& TaskbarHost::lyrics() const { return impl_->lines; }

void TaskbarHost::show() { impl_->show(); }

void TaskbarHost::hide() { impl_->hide(); }

bool TaskbarHost::isTaskbar() const { return true; }

bool TaskbarHost::isVerticalTaskbar() const { return impl_ && impl_->isVerticalTaskbar(); }

bool TaskbarHost::isCompactTaskbar() const { return impl_ && impl_->compactTaskbarHeight_ > 0.0f; }

int TaskbarHost::currentLine() const { return impl_->currentLine; }

const std::wstring& TaskbarHost::statusText() const { return impl_->statusText; }

void TaskbarHost::changeFont(float delta) { impl_->changeFont(delta); }

LyricFontSizeContext TaskbarHost::fontSizeContext() const { return impl_->fontSizeContext(); }

void TaskbarHost::setFont(const std::wstring& family, float size, LyricFontStyle style) {
    impl_->setFont(family, size, style);
}

void TaskbarHost::refreshTheme() { impl_->refreshTheme(); }

void TaskbarHost::setRenderMode(RenderMode mode) { impl_->setRenderMode(mode); }

void TaskbarHost::setSpectrumVisible(bool on) { impl_->setSpectrumVisible(on); }

void TaskbarHost::setViewMode(TaskbarViewMode mode) { impl_->setViewMode(mode); }

void TaskbarHost::setAppBarEdge(AppBarEdge edge) { impl_->setAppBarEdge(edge); }

bool TaskbarHost::backgroundBlurAvailable() const { return impl_->backgroundBlurAvailable(); }

void TaskbarHost::setPositionMode(int mode) { impl_->setPositionMode(mode); }

void TaskbarHost::onTaskbarCreated() { impl_->onTaskbarCreated(); }
