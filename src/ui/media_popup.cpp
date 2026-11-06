#include "media_popup.h"
#include "ui/media_popup/visibility.h"
#include "ui/media_popup/transitions.h"
#include "ui/media_popup/page_layout.h"
#include "ui/media_popup/page_painter.h"

#include "util/com_release.h"
#include "graphics/pixel_luminance.h"

#include "dock_pet.h"
#include "fluent_theme.h"
#include "fluent_dialog_surface.h"
#include "graphics/composition_renderer.h"
#include "media_control_icons.h"
#include "platform_icon.h"
#include "logging/runtime_logger.h"
#include "logging/ui_timing.h"

#include <d2d1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace media_popup_detail;
using media_popup_paint::sourceLabel;

namespace {
using graphics::backdropLuminance;

constexpr wchar_t kWndClassName[] = L"QQMusicLyricMediaPopup";
constexpr UINT_PTR kShowTimer = 1;
constexpr UINT_PTR kHideTimer = 2;
constexpr UINT_PTR kCloseTimer = 3;
constexpr UINT_PTR kScrollTimer = 4;
constexpr UINT_PTR kEnterTimer = 5;
constexpr UINT_PTR kVolumeTimer = 6;
constexpr UINT_PTR kIdleQuickExpandTimer = 7;
constexpr UINT_PTR kCategoryTimer = 8;
constexpr UINT_PTR kCopyFeedbackTimer = 9;
constexpr UINT_PTR kIdleSmoothScrollTimer = 10;
constexpr UINT kShowDelayMs = 100;
constexpr UINT kHideDelayMs = 180;
constexpr UINT kOpenAnimationMs = 180;
constexpr UINT kCloseAnimationMs = 140;
constexpr UINT kCopyFeedbackMs = 900;
constexpr float kSongTransitionMs = 220.0f;
constexpr float kSongTransitionTravelDip = 24.0f;
constexpr float kCategoryTransitionMs = 240.0f;
// 页面互切保留方向感，但不把稀疏的卡片内容整体推到可视区外；主要变化由
// 内容透明度完成，避免用户在过渡中只看到空白背景。
constexpr float kCategoryTransitionTravelDip = 24.0f;
constexpr float kInnerContentTransitionMs = 220.0f;
constexpr float kIdleQuickExpandMs = 200.0f;
// 根卡片位移由 DirectComposition 按显示器刷新率执行；只有长文本内容需要
// 重绘，30fps 已足够平滑，也避免与任务栏歌词的高频提交长期争用 UI 线程。
constexpr UINT kScrollTimerMs = 32;
// 页面滑动转场只有 240ms，按 32ms 驱动只有 7-8 帧，肉眼明显卡顿；
// 转场期间改用 16ms（60fps 级别），由 updateScrollTimer 按驱动目的切换间隔。
constexpr UINT kTransitionFrameMs = 16;
// 音量滑块展开/收起过渡时长
constexpr float kVolumeSliderMs = 140.0f;

// d2d1effects.h 只声明这个 GUID；当前工程的链接配置不提供其外部定义，
// 这里保留 Direct2D 标准 Gaussian Blur CLSID 的内部定义。
constexpr CLSID kGaussianBlurClsid = {
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};


bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

D2D1_COLOR_F colorFromRef(COLORREF color, float alpha) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                        GetBValue(color) / 255.0f, alpha);
}

} // namespace

struct MediaPopup::Impl {
    using PopupPage = media_popup_detail::Page;

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND anchor = nullptr;
    UINT dpi = 96;

    bool enabled = false;
    bool available = false;
    bool idleMode = false;
    bool playbackScene = false;
    bool inlineControls = false;
    bool idlePanelManual = false;
    bool triggerOnHover = true;
    bool anchorHover = false;
    bool popupHover = false;
    PopupVisibility visibility;
    bool deferredRender = false;
    int presentationUpdateDepth = 0;
    bool presentationUpdatePending = false;
    bool placedAbove = true;
    // 横向任务栏按上下弹出；侧边任务栏按宿主左右弹出。
    bool sideAnchored = false;
    bool popupOnRightOfAnchor = false;
    bool themeDirty = true;
    MediaPopupBackground backgroundMode = MediaPopupBackground::Solid;
    COLORREF floatingCardBackgroundColor = RGB(255, 255, 255);
    bool floatingCardBackgroundColorCustomized = false;
    bool followAlbumBackground = false;
    bool autoTextContrast = true;
    bool materialNeedsApply = true;
    bool backdropDirty = true;
    // 最近一次背景采样得到的亮度（-1 表示无效）。页面切换时背景位图继续复用，
    // 不需要再次隐藏窗口抓屏；页面文字对比色在绘制当前页面时恢复。
    float lastBackdropLuminance = -1.0f;
    bool coverDirty = true;
    bool sourceIconDirty = true;
    bool textDirty = true;
    bool scrollTimerRunning = false;
    UINT scrollTimerInterval = 0; // kScrollTimer 当前实际间隔，用于按驱动目的切换帧率
    bool clientAnimations = true;
    bool songTransitionPending = false;
    media_popup_detail::LayerTransition categoryTransition;
    media_popup_detail::PageSnapshot categorySnapshot;
    media_popup_detail::LayerTransition quickExpandTransition;
    // 只用于卡片高度变化后的鼠标连续性判断，不扩大可见范围。
    bool categoryHoverEnvelopeActive = false;
    RECT categoryHoverEnvelope{};
    media_popup_detail::IdleContentTransition idleContentTransition;
    media_popup_detail::QuickExpansion quickExpansion;
    bool dynamicBackgroundDirty = true;
    int cardScreenX = 0;
    int cardScreenY = 0;
    int cardWidthPx = 0;
    int cardHeightPx = 0;
    float cardOriginDip = 0.0f;
    float animationTravelPx = 0.0f;

    int hoverButton = -1;
    int pressedButton = -1;
    bool pressedSource = false;
    std::wstring pressedSourceAppUserModelId;
    bool hoverPageArrow = false;
    bool pressedPageArrow = false;
    bool hoverCopy = false;
    bool pressedCopy = false;
    bool copySucceeded = false;
    std::function<void(MediaControl)> onControl;
    std::function<void(const std::wstring&)> onSourceOpen;
    std::function<void(const std::wstring&)> onIdleAppOpen;
    std::function<void(const IdleTaskInfo&)> onIdleTaskOpen;
    std::function<void(const IdleTaskInfo&)> onIdleTaskComplete;
    std::function<void()> onPanelOpened;

    // 应用音量控件：图标+数值固定在右上角；点击图标从图标处向左过渡展开卡内滑块
    AppVolumeState volume;
    bool volumeSliderOn = false;       // 展开目标状态
    float volumeSliderT = 0.0f;        // 当前展开进度 0-1（过渡动画）
    bool volumeSliderOpening = false;  // 过渡方向
    bool volumeDragging = false;
    bool hoverVolume = false;
    bool pressedVolume = false;
    std::function<void(AppVolumeRequest)> onAppVolume;
    D2D1_RECT_F volumeIconRect{};
    D2D1_RECT_F volumeSliderRect{}; // 滑块轨道矩形（目标值；命中测试自行外扩余量）
    D2D1_RECT_F pageArrowRect{};     // 屏幕客户区坐标，供完整按钮区域命中
    D2D1_RECT_F copyRect{};          // 屏幕客户区坐标，供每日一言复制按钮命中
    OverlayMediaInfo media;
    IdlePresentation idle;
    int64_t positionMs = 0;
    // 层转场期间挂起的播放进度（-1 表示无）：转场快照冻结后进度不再直接
    // 更新 positionMs，收尾交接完成后再应用，避免交接瞬间时间文本跳变。
    int64_t deferredPositionMs = -1;
    float idleScrollOffset = 0.0f;
    float idleScrollMax = 0.0f;
    IdleQuickTab idleQuickTab = IdleQuickTab::QuickStart;
    int hoverIdleApp = -1;
    int pressedIdleApp = -1;
    int hoverIdleTask = -1;
    int hoverIdleTaskCheckbox = -1;
    int pressedIdleTask = -1;
    int pressedIdleTaskCheckbox = -1;
    int hoverIdleQuickTab = -1;
    int pressedIdleQuickTab = -1;
    bool hoverIdleQuickExpand = false;
    bool pressedIdleQuickExpand = false;
    bool idleScrollDragging = false;
    float idleScrollDragOffset = 0.0f;
    D2D1_RECT_F idleListRect{};
    D2D1_RECT_F idleScrollTrackRect{};
    D2D1_RECT_F idleScrollThumbRect{};
    D2D1_RECT_F idleQuickExpandRect{};
    D2D1_RECT_F idleQuickTabRects[2]{};
    bool idleTextDirty = true;
    bool idleIconsDirty = true;
    std::vector<ID2D1Bitmap*> idleIconBitmaps;

    CompositionRenderer renderer{false};
    fluent::CompositedScrollView idleScrollView;
    uint64_t deviceGeneration = 0;
    DockPet pet;
    ID2D1SolidColorBrush* brushBackground = nullptr;
    ID2D1SolidColorBrush* brushStroke = nullptr;
    ID2D1SolidColorBrush* brushText = nullptr;
    ID2D1SolidColorBrush* brushSecondary = nullptr;
    ID2D1SolidColorBrush* brushDisabled = nullptr;
    ID2D1SolidColorBrush* brushProgressTrack = nullptr;
    ID2D1SolidColorBrush* brushControl = nullptr;
    ID2D1SolidColorBrush* brushControlHover = nullptr;
    ID2D1SolidColorBrush* brushControlPressed = nullptr;
    ID2D1SolidColorBrush* brushIdleCell = nullptr;
    ID2D1SolidColorBrush* brushIdleCellHover = nullptr;
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* brushAccentHover = nullptr;
    ID2D1SolidColorBrush* brushTextOnAccent = nullptr;
    ID2D1SolidColorBrush* brushTaskPriorityHigh = nullptr;
    ID2D1SolidColorBrush* brushTaskPriorityMedium = nullptr;
    ID2D1SolidColorBrush* brushTaskPriorityLow = nullptr;
    ID2D1SolidColorBrush* brushTaskPriorityNone = nullptr;
    ID2D1LinearGradientBrush* brushDynamicGradient = nullptr;
    ID2D1RadialGradientBrush* brushDynamicGlow = nullptr;
    ID2D1LinearGradientBrush* brushMediaTextEdgeFade = nullptr;
    ID2D1LinearGradientBrush* brushIdleTextEdgeFade = nullptr;
    ID2D1Layer* textEdgeFadeLayer = nullptr;

    IDWriteTextFormat* fmtSource = nullptr;
    IDWriteTextFormat* fmtTimeRight = nullptr;
    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtArtist = nullptr;
    IDWriteTextFormat* fmtIcon = nullptr;
    IDWriteTextFormat* fmtIdleHeader = nullptr;
    IDWriteTextFormat* fmtIdleQuote = nullptr;
    IDWriteTextFormat* fmtIdleSource = nullptr;
    IDWriteTextFormat* fmtIdleApp = nullptr;
    IDWriteInlineObject* idleAppTrimmingSign = nullptr;
    IDWriteTextLayout* titleLayout = nullptr;
    IDWriteTextLayout* artistLayout = nullptr;
    IDWriteTextLayout* idleQuoteLayout = nullptr;
    IDWriteTextLayout* idleQuoteSourceLayout = nullptr;
    float idleQuoteWidth = 0.0f;
    float idleQuoteHeight = 0.0f;
    float idleQuoteSourceWidth = 0.0f;
    float idleQuoteSourceHeight = 0.0f;
    float titleWidth = 0.0f;
    float titleHeight = 0.0f;
    float artistWidth = 0.0f;
    float artistHeight = 0.0f;
    float titleScrollOffset = 0.0f;
    float artistScrollOffset = 0.0f;
    float idleQuoteScrollOffset = 0.0f;
    float idleQuoteSourceScrollOffset = 0.0f;
    ULONGLONG scrollTickMs = 0;
    ID2D1Bitmap* coverBmp = nullptr;
    ID2D1Bitmap* sourceIconBmp = nullptr;
    std::wstring sourceIconLoadedFor;
    ID2D1RoundedRectangleGeometry* coverClip = nullptr;
    ID2D1Layer* coverLayer = nullptr;
    ID2D1Bitmap* backdropBmp = nullptr;
    ID2D1Effect* backdropBlur = nullptr;
    ID2D1RoundedRectangleGeometry* backdropClip = nullptr;
    ID2D1Layer* backdropLayer = nullptr;
    media_control::Geometry controlGeometry;
    MediaPageLayout mediaLayout_{};

    media_popup_paint::PagePaintContext pagePaintContext() {
        return {renderer.dwrite(),
                {hasBackdropTextContrastForPage(PopupPage::Media),
                 hasBackdropTextContrastForPage(PopupPage::Idle)},
                {frostedBackgroundActiveForPage(PopupPage::Media),
                 frostedBackgroundActiveForPage(PopupPage::Idle)},
                {cardFillForPage(PopupPage::Media), cardFillForPage(PopupPage::Idle)},
                {idleIconBitmaps,
                 pet,
                 brushStroke,
                 brushText,
                 brushSecondary,
                 brushDisabled,
                 brushProgressTrack,
                 brushControl,
                 brushControlHover,
                 brushControlPressed,
                 brushIdleCell,
                 brushIdleCellHover,
                 brushAccent,
                 brushAccentHover,
                 brushTextOnAccent,
                 brushTaskPriorityHigh,
                 brushTaskPriorityMedium,
                 brushTaskPriorityLow,
                 brushTaskPriorityNone,
                 brushMediaTextEdgeFade,
                 brushIdleTextEdgeFade,
                 textEdgeFadeLayer,
                 fmtSource,
                 fmtTimeRight,
                 fmtTitle,
                 fmtArtist,
                 fmtIcon,
                 fmtIdleHeader,
                 fmtIdleQuote,
                 fmtIdleSource,
                 fmtIdleApp,
                 titleLayout,
                 artistLayout,
                 idleQuoteLayout,
                 idleQuoteSourceLayout,
                 coverBmp,
                 sourceIconBmp,
                 sourceIconLoadedFor,
                 coverClip,
                 coverLayer,
                 controlGeometry},
                {categorySnapshot, media, positionMs, titleWidth, titleHeight, artistWidth,
                 artistHeight, titleScrollOffset, artistScrollOffset},
                {quickExpansion, idle, idleScrollOffset, idleScrollMax, idleQuickTab,
                 idleQuoteWidth, idleQuoteHeight, idleQuoteSourceWidth, idleQuoteSourceHeight,
                 idleQuoteScrollOffset, idleQuoteSourceScrollOffset},
                {hoverButton, pressedButton, hoverCopy, pressedCopy, copySucceeded, hoverIdleApp,
                 hoverIdleTask, hoverIdleTaskCheckbox, pressedIdleTask, pressedIdleTaskCheckbox,
                 hoverIdleQuickTab, pressedIdleQuickTab, hoverIdleQuickExpand,
                 pressedIdleQuickExpand},
                {cardOriginDip, copyRect, idleListRect, idleScrollTrackRect, idleScrollThumbRect,
                 idleQuickExpandRect, idleQuickTabRects, mediaLayout_},
                {available, playbackScene, idlePanelManual, categoryTransition}};
    }

    PopupPage currentPage() const {
        return idleMode ? PopupPage::Idle : PopupPage::Media;
    }

    bool opensOnHover() const {
        return triggerOnHover;
    }

    bool idlePageAllowed() const {
        return available && idle.quickStartEnabled;
    }

    static int categoryDirection(PopupPage from, PopupPage to) {
        // 媒体卡片进入组合面板时，新面板从右侧进入；返回媒体卡片时反向。
        return from == PopupPage::Media && to == PopupPage::Idle ? 1 : -1;
    }

    float popupContentHeightDip() const {
        return idleMode ? kIdlePopupHeightDip : kPopupHeightDip;
    }

    float idleQuoteAreaWidth() const {
        // 复制按钮已经移到标题行，与返回按钮并排；正文可以使用完整宽度，
        // 文本滚动区域必须与实际绘制区域保持一致。
        return std::max(1.0f, kPopupWidthDip - 32.0f);
    }

    float idleContentTransitionProgress() const {
        return idleContentTransition.progress(GetTickCount64(), kInnerContentTransitionMs);
    }

    // 空闲页（每日一言 + 快速打开）的纵向布局量测：活页绘制、转场快照和
    // 快速展开层转场共用同一份几何，保证三条路径的端点完全一致。
    using IdleLayout = media_popup_detail::IdleLayout;
    IdlePageLayout idlePageLayout(const IdlePresentation& content) const {
        return {quickExpansion.progress, idleQuickTab,
                idleReturnArrowVisible(content) || categoryTransition.active()};
    }
    float idleQuickExpandProgress(const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickExpandProgress(content);
    }
    float idleQuickHeaderTop(float collapsedTop, const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickHeaderTop(collapsedTop, content);
    }
    float idleQuickExpandButtonTop(float headerTop) const {
        return idlePageLayout(idle).idleQuickExpandButtonTop(headerTop);
    }
    std::size_t idleQuickRowCount(const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickRowCount(content);
    }
    float idleQuickGridContentHeight(const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickGridContentHeight(content);
    }
    float idleQuickTaskContentHeight(const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickTaskContentHeight(content);
    }
    float idleQuickListContentHeight(const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickListContentHeight(content);
    }
    float idleQuickGridLeft() const { return idlePageLayout(idle).idleQuickGridLeft(idleListRect); }
    float idleQuickListHeight(const IdlePresentation& content, float progress) const {
        return idlePageLayout(content).idleQuickListHeight(content, progress);
    }
    float idleQuickListTop(float headerTop) const {
        return idlePageLayout(idle).idleQuickListTop(headerTop);
    }
    float idleQuickTabContentOffset(float progress, const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickTabContentOffset(progress, content);
    }
    float idleQuickTabTop(float contentTop, float progress, const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickTabTop(contentTop, progress, content);
    }
    D2D1_RECT_F idleQuickExpandLocalRect(float w, float headerTop,
                                         const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickExpandLocalRect(w, headerTop, content);
    }
    D2D1_RECT_F idleQuickTriggerLocalRect(float w, float headerTop,
                                          const IdlePresentation& content) const {
        return idlePageLayout(content).idleQuickTriggerLocalRect(w, headerTop, content);
    }
    IdleLayout idleLayout(const IdlePresentation& content, float quoteHeight) const {
        return idlePageLayout(content).idleLayout(content, quoteHeight);
    }
    void layoutIdleList(float width, float top, const IdlePresentation* content = nullptr) {
        const auto& value = content ? *content : idle;
        const auto layout = idlePageLayout(value).list(width, top, value, idleScrollOffset);
        idleListRect = layout.rect;
        idleScrollTrackRect = layout.track;
        idleScrollThumbRect = layout.thumb;
        idleScrollOffset = layout.offset;
        idleScrollMax = layout.maxScroll;
    }

    // 快速启动标题行：任务功能开启时补充“今日任务”；活页、快照与展开层
    // 转场共用同一套几何。
    void drawIdleQuickHeader(ID2D1DeviceContext* rt, float w, float headerTop,
                             const IdlePresentation& content, bool updateHitTest) {
        media_popup_paint::drawIdleQuickHeader(pagePaintContext(), rt, w, headerTop, content,
                                               updateHitTest);
    }

    void drawIdleQuickDivider(ID2D1DeviceContext* rt, float w, float headerTop,
                              const IdlePresentation& content) {
        media_popup_paint::drawIdleQuickDivider(pagePaintContext(), rt, w, headerTop, content);
    }

    MediaPopupBackground backgroundModeForPage(PopupPage page) const {
        (void)page;
        return backgroundMode;
    }

    bool frostedBackgroundActiveForPage(PopupPage page) const {
        return backgroundModeForPage(page) == MediaPopupBackground::Frosted;
    }

    bool dynamicBackgroundEnabledForPage(PopupPage page) const {
        (void)page;
        // 专辑色只在播放场景且存在有效专辑色时生效；无播放的组合面板
        // 继续使用用户设置的磨砂颜色。
        return followAlbumBackground && playbackScene && media.hasDominantColor &&
               backgroundMode == MediaPopupBackground::Frosted;
    }

    MediaPopupBackground activeBackgroundMode() const {
        return backgroundModeForPage(currentPage());
    }

    bool frostedBackgroundActive() const {
        return frostedBackgroundActiveForPage(currentPage());
    }

    bool dynamicBackgroundEnabled() const {
        return dynamicBackgroundEnabledForPage(currentPage());
    }

    bool backdropTextContrastEnabledForPage(PopupPage page) const {
        // 媒体卡片与每日一言/快捷启动卡片共用同一个动态文字颜色开关。
        return frostedBackgroundActiveForPage(page) && autoTextContrast;
    }

    bool hasBackdropTextContrastForPage(PopupPage page) const {
        return backdropTextContrastEnabledForPage(page) && backdropBmp &&
               lastBackdropLuminance >= 0.0f;
    }

    D2D1_COLOR_F cardFillForPage(PopupPage page) const {
        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        // 纯色卡片使用窗口底色，媒体页与快捷启动页保持一致。
        if (!frostedBackgroundActiveForPage(page))
            return p.windowBg;
        const bool dark = fluent::isDarkMode(fluent::ThemeTarget::Window);
        D2D1_COLOR_F cardFill = p.cardFillSolid;
        if (page == PopupPage::Idle) {
            // 纯色模式只跟随窗口深浅色；自定义颜色只参与磨砂 tint。
            cardFill = dark ? D2D1::ColorF(D2D1::ColorF::Black)
                            : D2D1::ColorF(D2D1::ColorF::White);
            if (frostedBackgroundActiveForPage(page)) {
                const float tintAlpha = dark ? 0.10f : 0.16f;
                if (dynamicBackgroundEnabledForPage(page)) {
                    cardFill = p.cardFill;
                    cardFill.a = tintAlpha;
                } else if (floatingCardBackgroundColorCustomized) {
                    cardFill = fluent::toD2D(floatingCardBackgroundColor, tintAlpha);
                } else {
                    cardFill = p.cardFill;
                }
            }
        } else if (frostedBackgroundActiveForPage(page)) {
            const float tintAlpha = dark ? 0.10f : 0.16f;
            if (dynamicBackgroundEnabledForPage(page)) {
                cardFill = p.cardFill;
                cardFill.a = tintAlpha;
            } else if (floatingCardBackgroundColorCustomized) {
                cardFill = fluent::toD2D(floatingCardBackgroundColor, tintAlpha);
            } else {
                // 未设置自定义颜色时保留系统磨砂卡片的默认颜色。
                cardFill = p.cardFill;
                cardFill.a = tintAlpha;
            }
        }
        return cardFill;
    }

    void releaseDynamicBackgroundResources() {
        releaseCom(brushDynamicGradient);
        releaseCom(brushDynamicGlow);
        dynamicBackgroundDirty = true;
    }

    void resetBackdropTextColors() {
        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        if (brushText)
            brushText->SetColor(p.text);
        if (brushSecondary)
            brushSecondary->SetColor(p.textSecondary);
        if (brushDisabled)
            brushDisabled->SetColor(p.disabled);
    }

    void applyBackdropTextContrastForPage(float luminance, PopupPage page) {
        if (!backdropTextContrastEnabledForPage(page)) {
            resetBackdropTextColors();
            return;
        }
        const float blackContrast = (luminance + 0.05f) / 0.05f;
        const float whiteContrast = 1.05f / (luminance + 0.05f);
        const bool useBlack = blackContrast >= whiteContrast;
        const D2D1_COLOR_F text = useBlack ? D2D1::ColorF(D2D1::ColorF::Black)
                                            : D2D1::ColorF(D2D1::ColorF::White);
        const D2D1_COLOR_F secondary =
            useBlack ? D2D1::ColorF(D2D1::ColorF::Black, 0.70f)
                     : D2D1::ColorF(D2D1::ColorF::White, 0.72f);
        const D2D1_COLOR_F disabled =
            useBlack ? D2D1::ColorF(D2D1::ColorF::Black, 0.42f)
                     : D2D1::ColorF(D2D1::ColorF::White, 0.46f);
        if (brushText)
            brushText->SetColor(text);
        if (brushSecondary)
            brushSecondary->SetColor(secondary);
        if (brushDisabled)
            brushDisabled->SetColor(disabled);
    }

    void applyBackdropTextContrast(float luminance) {
        applyBackdropTextContrastForPage(luminance, currentPage());
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

    float scale() const {
        return static_cast<float>(dpi) / 96.0f;
    }

    float dip(int px) const {
        return static_cast<float>(px) / scale();
    }

    float popupHeightDip() const {
        return popupContentHeightDip();
    }

    RECT popupCardScreenRect() const {
        return RECT{cardScreenX, cardScreenY, cardScreenX + cardWidthPx,
                    cardScreenY + cardHeightPx};
    }

    static RECT mergeScreenRects(const RECT& first, const RECT& second) {
        return RECT{std::min(first.left, second.left), std::min(first.top, second.top),
                    std::max(first.right, second.right), std::max(first.bottom, second.bottom)};
    }

    void killTimers() {
        idleScrollOffset = idleScrollView.offset();
        idleScrollView.stop();
        idleScrollView.setVisible(false);
        if (!hwnd)
            return;
        KillTimer(hwnd, kShowTimer);
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kCloseTimer);
        KillTimer(hwnd, kScrollTimer);
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kVolumeTimer);
        KillTimer(hwnd, kIdleQuickExpandTimer);
        KillTimer(hwnd, kCategoryTimer);
        KillTimer(hwnd, kCopyFeedbackTimer);
        scrollTimerRunning = false;
        visibility.finishOpen();
        deferredRender = false;
    }

    // 结束 DComp 层转场（页面互切或快速打开展开/收起）：撤掉合成层并停掉
    // 收尾定时器。多次调用安全；调用方负责在必要时把最终交换链帧提交后，
    // 再提交层的移除。两个转场共用同一组临时合成层，不会同时激活（切换按钮
    // 在页面互切期间禁点，setPage 也会先停掉另一个转场）。
    void stopLayerTransition(media_popup_detail::LayerTransition& transition, UINT_PTR timer) {
        const bool hadLayers = transition.hasLayers();
        transition.reset();
        deferredPositionMs = -1;
        if (hadLayers) {
            renderer.clearTransitionLayers();
        }
        if (hwnd)
            KillTimer(hwnd, timer);
    }

    void stopCategoryTransition() { stopLayerTransition(categoryTransition, kCategoryTimer); }

    void stopQuickExpandTransition() {
        stopLayerTransition(quickExpandTransition, kIdleQuickExpandTimer);
    }

    void releaseDeviceResources() {
        idleScrollOffset = idleScrollView.offset();
        idleScrollView.stop();
        idleScrollView.setVisible(false);
        stopCategoryTransition();
        stopQuickExpandTransition();
        releaseVisualResources();
        pet.discardDeviceResources();
        themeDirty = true;
        coverDirty = true;
        materialNeedsApply = true;
    }

    void releaseDrawingResources() {
        releaseDeviceResources();
        idleScrollView.discardDeviceResources();
        renderer.discard();
        deviceGeneration = 0;
    }

    void applyWindowMaterial() {
        if (!hwnd)
            return;

        // 这个窗口使用 WS_EX_NOREDIRECTIONBITMAP，卡片圆角、边框和背景全部
        // 在同一个 D2D 交换链里绘制。不要再给 HWND 设置 DWM 圆角/边框/材质，
        // 否则系统会保留一层窗口级底板，根视觉位移动画时就会和内容错开。
        // 这里只关闭系统自己的显示/隐藏过渡，动画由 DirectComposition 统一负责。
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions,
                              sizeof(disableTransitions));
    }

    void releaseVisualResources() {
        releaseCom(coverBmp);
        releaseCom(sourceIconBmp);
        sourceIconLoadedFor.clear();
        for (auto*& bitmap : idleIconBitmaps)
            releaseCom(bitmap);
        idleIconBitmaps.clear();
        idleIconsDirty = true;
        sourceIconDirty = true;
        releaseCom(backdropBmp);
        releaseCom(backdropBlur);
        lastBackdropLuminance = -1.0f;
        backdropDirty = true;
        releaseCom(brushBackground);
        releaseCom(brushStroke);
        releaseCom(brushText);
        releaseCom(brushSecondary);
        releaseCom(brushDisabled);
        releaseCom(brushProgressTrack);
        releaseCom(brushControl);
        releaseCom(brushControlHover);
        releaseCom(brushControlPressed);
        releaseCom(brushIdleCell);
        releaseCom(brushIdleCellHover);
        releaseCom(brushAccent);
        releaseCom(brushAccentHover);
        releaseCom(brushTextOnAccent);
        releaseCom(brushTaskPriorityHigh);
        releaseCom(brushTaskPriorityMedium);
        releaseCom(brushTaskPriorityLow);
        releaseCom(brushTaskPriorityNone);
        releaseCom(brushMediaTextEdgeFade);
        releaseCom(brushIdleTextEdgeFade);
        releaseCom(textEdgeFadeLayer);
        releaseDynamicBackgroundResources();
        releaseCom(fmtSource);
        releaseCom(fmtTimeRight);
        releaseCom(fmtTitle);
        releaseCom(fmtArtist);
        releaseCom(fmtIcon);
        releaseCom(fmtIdleHeader);
        releaseCom(fmtIdleQuote);
        releaseCom(fmtIdleSource);
        releaseCom(idleAppTrimmingSign);
        releaseCom(fmtIdleApp);
        releaseCom(titleLayout);
        releaseCom(artistLayout);
        releaseCom(idleQuoteLayout);
        releaseCom(idleQuoteSourceLayout);
        titleWidth = 0.0f;
        titleHeight = 0.0f;
        artistWidth = 0.0f;
        artistHeight = 0.0f;
        idleQuoteWidth = 0.0f;
        idleQuoteHeight = 0.0f;
        idleQuoteSourceWidth = 0.0f;
        idleQuoteSourceHeight = 0.0f;
        idleQuoteScrollOffset = 0.0f;
        idleQuoteSourceScrollOffset = 0.0f;
        textDirty = true;
        idleTextDirty = true;
        releaseCom(coverClip);
        media_control::release(controlGeometry);
        releaseCom(coverLayer);
        // 几何图层会随页面资源一起重建；页面互切时另外保留已解码的页面位图，
        // 避免来源页快照因资源重建而丢失图标。
        releaseCom(backdropClip);
        releaseCom(backdropLayer);
    }

    void releaseAll() {
        releaseDrawingResources();
        renderer.releaseAll();
    }

    bool createTextFormat(float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out,
                          DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                          bool wrap = false) {
        if (!out || !renderer.dwrite())
            return false;
        *out = nullptr;
        HRESULT hr = renderer.dwrite()->CreateTextFormat(
            fluent::uiFontFamily(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", out);
        if (FAILED(hr) || !*out)
            return false;
        (*out)->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                     : DWRITE_WORD_WRAPPING_NO_WRAP);
        (*out)->SetParagraphAlignment(paragraph);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        (*out)->SetTrimming(&trimming, nullptr);
        fluent::applyUiFontFallback(*out);
        return true;
    }

    bool createResources() {
        if (!themeDirty && brushBackground)
            return true;
        releaseVisualResources();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;

        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        const D2D1_COLOR_F cardFill = cardFillForPage(currentPage());
        if (FAILED(rt->CreateSolidColorBrush(cardFill, &brushBackground)) ||
            FAILED(rt->CreateSolidColorBrush(frostedBackgroundActive() ? p.cardStroke
                                                                      : p.controlStroke,
                                             &brushStroke)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.disabled, &brushDisabled)) ||
            FAILED(rt->CreateSolidColorBrush(p.separator, &brushProgressTrack)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlFill, &brushControl)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlHover, &brushControlHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlPressed, &brushControlPressed)) ||
            FAILED(rt->CreateSolidColorBrush(p.listHover, &brushIdleCell)) ||
            FAILED(rt->CreateSolidColorBrush(p.listSelected, &brushIdleCellHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.accent, &brushAccent)) ||
            FAILED(rt->CreateSolidColorBrush(p.accentHover, &brushAccentHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.textOnAccent, &brushTextOnAccent)) ||
            FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0xD13438, 0.96f),
                                             &brushTaskPriorityHigh)) ||
            FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0x2F78D0, 0.96f),
                                             &brushTaskPriorityMedium)) ||
            FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0xD99A00, 0.96f),
                                             &brushTaskPriorityLow)) ||
            FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0x8A8A8A, 0.82f),
                                             &brushTaskPriorityNone)) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtSource) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtTimeRight) ||
            !createTextFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtTitle) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtArtist) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtIcon) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtIdleHeader) ||
            !createTextFormat(14.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtIdleQuote,
                               DWRITE_PARAGRAPH_ALIGNMENT_NEAR) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtIdleSource) ||
            !createTextFormat(11.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtIdleApp)) {
            releaseDrawingResources();
            return false;
        }
        if (FAILED(fmtTimeRight->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING)) ||
            FAILED(fmtIdleQuote->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING)) ||
            FAILED(fmtIdleApp->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER))) {
            releaseDrawingResources();
            return false;
        }
        DWRITE_TRIMMING idleAppTrimming{
            DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        if (!renderer.dwrite() ||
            FAILED(renderer.dwrite()->CreateEllipsisTrimmingSign(
                fmtIdleApp, &idleAppTrimmingSign)) ||
            FAILED(fmtIdleApp->SetTrimming(&idleAppTrimming, idleAppTrimmingSign))) {
            releaseDrawingResources();
            return false;
        }

        auto* factory = renderer.d2d();
        if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(
                                 D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, kCoverSizeDip,
                                                               kCoverSizeDip),
                                                   10.0f, 10.0f),
                                 &coverClip)) ||
            FAILED(rt->CreateLayer(&coverLayer)) ||
            FAILED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, kPopupWidthDip, popupHeightDip()),
                                  kPopupCornerDip, kPopupCornerDip),
                &backdropClip)) ||
            FAILED(rt->CreateLayer(&backdropLayer)) ||
            !media_control::create(factory, controlGeometry)) {
            releaseDrawingResources();
            return false;
        }

        // 歌曲标题、艺术家和每日一言滚动时，左右各 8 DIP 逐渐淡出；
        // 使用图层透明度遮罩，不会用固定颜色覆盖磨砂背景。两类文本的区域宽度不同，
        // 因此分别创建渐变画刷，避免每日一言使用歌曲信息的渐变比例。
        auto createTextEdgeFade = [&](float left, float right,
                                      ID2D1LinearGradientBrush** out) {
            if (!out)
                return false;
            *out = nullptr;
            const float textAreaWidth = std::max(1.0f, right - left);
            const float fadeStop = std::min(0.5f, kPopupTextEdgeFadeDip / textAreaWidth);
            const D2D1_GRADIENT_STOP fadeStops[] = {
                {0.0f, D2D1::ColorF(D2D1::ColorF::White, 0.0f)},
                {fadeStop, D2D1::ColorF(D2D1::ColorF::White, 1.0f)},
                {1.0f - fadeStop, D2D1::ColorF(D2D1::ColorF::White, 1.0f)},
                {1.0f, D2D1::ColorF(D2D1::ColorF::White, 0.0f)},
            };
            ID2D1GradientStopCollection* collection = nullptr;
            if (FAILED(rt->CreateGradientStopCollection(fadeStops, _countof(fadeStops),
                                                        &collection)) ||
                !collection)
                return false;
            const auto gradient = D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(left, 0.0f), D2D1::Point2F(right, 0.0f));
            const HRESULT hr = rt->CreateLinearGradientBrush(gradient, collection, out);
            collection->Release();
            return SUCCEEDED(hr) && *out;
        };
        const bool mediaFadeReady = createTextEdgeFade(
            kPopupTextLeftDip, kPopupWidthDip - kMediaTextRightReserveDip,
            &brushMediaTextEdgeFade);
        const bool idleFadeReady = createTextEdgeFade(
            16.0f, kPopupWidthDip - 16.0f, &brushIdleTextEdgeFade);
        if ((mediaFadeReady || idleFadeReady) && FAILED(rt->CreateLayer(&textEdgeFadeLayer))) {
            releaseCom(brushMediaTextEdgeFade);
            releaseCom(brushIdleTextEdgeFade);
        }

        // 画笔按调色板重建后，如果背景快照仍然有效，沿用之前采样的亮度恢复
        // 文字对比色，避免为重新采样而隐藏窗口造成可见闪烁。
        if (backdropBmp && lastBackdropLuminance >= 0.0f)
            applyBackdropTextContrast(lastBackdropLuminance);
        themeDirty = false;
        return true;
    }

    bool createDynamicBackgroundResources(bool enabled) {
        if (!enabled)
            return false;
        if (!dynamicBackgroundDirty && brushDynamicGradient && brushDynamicGlow)
            return true;

        releaseCom(brushDynamicGradient);
        releaseCom(brushDynamicGlow);
        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;

        const COLORREF color = media.hasDominantColor ? media.dominantColor
                                                       : fluent::accentColor();
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, colorFromRef(color, 0.44f)},
            {0.56f, colorFromRef(color, 0.22f)},
            {1.0f, colorFromRef(color, 0.0f)},
        };
        ID2D1GradientStopCollection* collection = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, _countof(stops), &collection)) ||
            !collection) {
            return false;
        }

        const auto linear = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0.0f, 0.0f),
            D2D1::Point2F(kPopupWidthDip, popupHeightDip()));
        HRESULT hr = rt->CreateLinearGradientBrush(linear, collection, &brushDynamicGradient);
        if (SUCCEEDED(hr)) {
            const auto radial = D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(kPopupWidthDip * 0.74f, popupHeightDip() * 0.28f),
                D2D1::Point2F(0.0f, 0.0f), kPopupWidthDip * 0.72f,
                popupHeightDip() * 0.92f);
            hr = rt->CreateRadialGradientBrush(radial, collection, &brushDynamicGlow);
        }
        collection->Release();
        if (FAILED(hr) || !brushDynamicGradient || !brushDynamicGlow) {
            releaseCom(brushDynamicGradient);
            releaseCom(brushDynamicGlow);
            return false;
        }
        dynamicBackgroundDirty = false;
        return true;
    }

    bool createDynamicBackgroundResources() {
        return createDynamicBackgroundResources(dynamicBackgroundEnabled());
    }

    bool captureBackdrop(bool force = false) {
        if (!backdropDirty)
            return true;
        // 页面互切不改变窗口后方的场景；已有快照时直接复用，即使其他设置
        // 把 dirty 标记置上，也不能在可见转场中通过隐藏窗口重新抓屏。
        if (categoryTransition.active() && backdropBmp && lastBackdropLuminance >= 0.0f) {
            backdropDirty = false;
            return true;
        }
        backdropDirty = false;
        releaseCom(backdropBlur);
        releaseCom(backdropBmp);
        resetBackdropTextColors();
        if ((!force && !frostedBackgroundActive()) || !hwnd)
            return true;

        const int width = cardWidthPx;
        const int height = cardHeightPx;
        if (width <= 0 || height <= 0)
            return false;
        const RECT cardRect{cardScreenX, cardScreenY, cardScreenX + width,
                            cardScreenY + height};

        // 首次显示时窗口本来是隐藏的；切换设置或主题时可能已经可见，
        // 临时隐藏它再抓取，避免把自己的卡片拍进背景图形成递归灰层。
        const bool restoreVisible = IsWindowVisible(hwnd) != FALSE;
        if (restoreVisible) {
            ShowWindow(hwnd, SW_HIDE);
        }

        // 锚点就是任务栏内嵌歌词窗口。弹出卡片覆盖它时，如果直接抓屏，
        // 内嵌歌词的背景/强调色会被当作后方材质，形成底部第二块面板。
        RECT anchorRect{};
        const bool anchorOverlaps = anchor && anchor != hwnd && IsWindow(anchor) &&
                                    IsWindowVisible(anchor) && GetWindowRect(anchor, &anchorRect) &&
                                    anchorRect.left < cardRect.right &&
                                    anchorRect.right > cardRect.left &&
                                    anchorRect.top < cardRect.bottom &&
                                    anchorRect.bottom > cardRect.top;
        if (anchorOverlaps)
            ShowWindow(anchor, SW_HIDE);
        // 只有实际隐藏过窗口才需要等合成器移除旧画面。首次打开时通常没有
        // 窗口覆盖采样区域，无条件等待会把点击处理拖过一个或多个刷新周期。
        if (restoreVisible || anchorOverlaps)
            DwmFlush();

        auto restoreWindows = [&]() {
            if (anchorOverlaps)
                ShowWindow(anchor, SW_SHOWNA);
            if (restoreVisible)
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        };

        HDC screenDc = GetDC(nullptr);
        if (!screenDc) {
            restoreWindows();
            return false;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels,
                                       nullptr, 0);
        HDC memoryDc = dib ? CreateCompatibleDC(screenDc) : nullptr;
        HGDIOBJ oldBitmap = nullptr;
        bool copied = false;
        if (memoryDc && dib) {
            oldBitmap = SelectObject(memoryDc, dib);
            if (oldBitmap && oldBitmap != HGDI_ERROR)
                copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, cardRect.left,
                                cardRect.top, SRCCOPY | CAPTUREBLT) != FALSE;
        }
        if (oldBitmap && oldBitmap != HGDI_ERROR)
            SelectObject(memoryDc, oldBitmap);
        if (memoryDc)
            DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        restoreWindows();
        if (!copied || !pixels) {
            if (dib)
                DeleteObject(dib);
            return false;
        }

        const float sampledLuminance = backdropLuminance(pixels, width, height);
        lastBackdropLuminance = sampledLuminance;

        auto* rt = renderer.renderTarget();
        if (!rt) {
            DeleteObject(dib);
            return false;
        }
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(dpi), static_cast<float>(dpi));
        ID2D1Bitmap1* captured = nullptr;
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT>(width),
                                                static_cast<UINT>(height)),
                                    pixels, static_cast<UINT>(width * 4), &props, &captured)) ||
            !captured) {
            DeleteObject(dib);
            return false;
        }
        DeleteObject(dib);
        backdropBmp = captured;
        if (frostedBackgroundActive())
            applyBackdropTextContrast(sampledLuminance);

        ID2D1Effect* blur = nullptr;
        HRESULT hr = rt->CreateEffect(kGaussianBlurClsid, &blur);
        if (SUCCEEDED(hr))
            blur->SetInput(0, backdropBmp);
        if (SUCCEEDED(hr))
            hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 16.0f);
        if (SUCCEEDED(hr))
            hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                                D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        if (SUCCEEDED(hr))
            hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        if (SUCCEEDED(hr))
            backdropBlur = blur;
        else
            releaseCom(blur);
        return true;
    }

    void refreshClientAnimations() {
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        clientAnimations = animations != FALSE;
        if (!clientAnimations) {
            titleScrollOffset = 0.0f;
            artistScrollOffset = 0.0f;
            idleQuoteScrollOffset = 0.0f;
            idleQuoteSourceScrollOffset = 0.0f;
            scrollTickMs = 0;
            stopCategoryTransition();
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
            idleContentTransition.active = false;
            quickExpansion.progress = quickExpansion.expanded ? 1.0f : 0.0f;
            quickExpansion.opening = false;
            stopQuickExpandTransition();
        }
    }

    bool createMeasuredTextLayout(const std::wstring& text, IDWriteTextFormat* format,
                                  float layoutHeight, IDWriteTextLayout** out, float& width,
                                  float& height) {
        return media_popup_paint::createMeasuredTextLayout(renderer.dwrite(), text, format,
                                                           layoutHeight, out, width, height);
    }

    void buildTextLayouts() {
        if (!textDirty)
            return;
        textDirty = false;
        releaseCom(titleLayout);
        releaseCom(artistLayout);
        titleWidth = 0.0f;
        titleHeight = 0.0f;
        artistWidth = 0.0f;
        artistHeight = 0.0f;

        if (!renderer.dwrite() || !fmtTitle || !fmtArtist)
            return;

        createMeasuredTextLayout(media.title, fmtTitle, 40.0f, &titleLayout, titleWidth,
                                 titleHeight);
        createMeasuredTextLayout(media.artist, fmtArtist, 40.0f, &artistLayout, artistWidth,
                                 artistHeight);
    }

    void buildIdleTextLayout() {
        if (!idleTextDirty)
            return;
        idleTextDirty = false;
        releaseCom(idleQuoteLayout);
        releaseCom(idleQuoteSourceLayout);
        idleQuoteWidth = 0.0f;
        idleQuoteHeight = 0.0f;
        idleQuoteSourceWidth = 0.0f;
        idleQuoteSourceHeight = 0.0f;
        if (!renderer.dwrite())
            return;

        if (fmtIdleQuote && !idle.sentence.empty() &&
            createMeasuredTextLayout(idle.sentence, fmtIdleQuote, 42.0f, &idleQuoteLayout,
                                     idleQuoteWidth, idleQuoteHeight)) {
            idleQuoteHeight = std::min(42.0f, idleQuoteHeight);
        }
        if (fmtIdleSource && !idle.source.empty()) {
            createMeasuredTextLayout(idle.source, fmtIdleSource, 18.0f,
                                     &idleQuoteSourceLayout, idleQuoteSourceWidth,
                                     idleQuoteSourceHeight);
            idleQuoteSourceHeight = std::min(18.0f, idleQuoteSourceHeight);
        }
    }

    void decodeIdleIcons() {
        if (!idleIconsDirty)
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        // 先在临时数组中完成整批位图创建，最后再整体替换。不能先清空正在
        // 使用的数组，否则任务数据刷新恰好落在绘制交接处时，图标会短暂变成
        // 空白，表现为图标区域像被刷新了一次。
        std::vector<ID2D1Bitmap*> nextBitmaps;
        nextBitmaps.reserve(idle.apps.size());
        for (const auto& app : idle.apps) {
            ID2D1Bitmap* bitmap = nullptr;
            if (app.iconPixels && !app.iconPixels->empty() && app.iconWidth > 0 &&
                app.iconHeight > 0 &&
                SUCCEEDED(rt->CreateBitmap(
                    D2D1::SizeU(app.iconWidth, app.iconHeight), app.iconPixels->data(),
                    app.iconWidth * 4, &props, &bitmap))) {
                nextBitmaps.push_back(bitmap);
            } else {
                nextBitmaps.push_back(nullptr);
            }
        }
        for (auto*& bitmap : idleIconBitmaps)
            releaseCom(bitmap);
        idleIconBitmaps = std::move(nextBitmaps);
        idleIconsDirty = false;
    }


    float textAreaWidth(float popupWidth) const {
        return std::max(1.0f, popupWidth - kPopupTextLeftDip - kMediaTextRightReserveDip);
    }

    // 宠物状态由媒体快照驱动，弹窗隐藏期间也保持同步，暂停计时与 Dock 一致。
    void syncPetMode(bool trackChanged) {
        const ULONGLONG now = GetTickCount64();
        const DockPetMode mode = !available    ? DockPetMode::Hidden
                                 : media.playing ? DockPetMode::Listening
                                                 : DockPetMode::Paused;
        pet.setMode(mode, now);
        if (trackChanged && mode == DockPetMode::Listening)
            pet.hop(now);
    }

    void updateScrollTimer(float areaWidth) {
        const float idleAreaWidth = idleQuoteAreaWidth();
        const bool idleQuoteOverflow = idleQuoteWidth > idleAreaWidth;
        const bool idleSourceOverflow = idleQuoteSourceWidth > idleAreaWidth;
        const bool idleOverflow = idleQuoteOverflow || idleSourceOverflow;
        const bool mediaOverflow = titleWidth > areaWidth || artistWidth > areaWidth;
        const bool transitionDriving = idleContentTransition.active;
        const bool textDriving = transitionDriving || (idleMode && idleOverflow) ||
                                 (!idleMode && available && media.playing && mediaOverflow);
        const bool petDriving = !idleMode && available && pet.animating();
        const bool shouldRun = visibility.visible() && !visibility.animating() && enabled &&
                               clientAnimations && (textDriving || petDriving);
        if (shouldRun) {
            // 短转场由这个定时器逐帧驱动，需要 60fps 级别才平滑；
            // 纯文本跑马灯保持 32ms 即可。驱动目的变化时按新间隔重建定时器。
            // （卡片 ⇄ 空闲面板的互切转场由 DComp 合成层执行，不走这里。）
            const UINT interval = transitionDriving ? kTransitionFrameMs : kScrollTimerMs;
            if (!scrollTimerRunning || scrollTimerInterval != interval) {
                if (scrollTimerRunning)
                    KillTimer(hwnd, kScrollTimer);
                scrollTickMs = GetTickCount64();
                SetTimer(hwnd, kScrollTimer, interval, nullptr);
                scrollTimerRunning = true;
                scrollTimerInterval = interval;
            }
            if (textDriving)
                return;
        } else if (scrollTimerRunning) {
            KillTimer(hwnd, kScrollTimer);
            scrollTimerRunning = false;
            scrollTimerInterval = 0;
        }
        if (!idleMode || !idleOverflow || !clientAnimations)
            idleQuoteScrollOffset = 0.0f;
        if (!idleMode || !idleOverflow || !clientAnimations)
            idleQuoteSourceScrollOffset = 0.0f;
        if (idleMode || !mediaOverflow || !clientAnimations || !media.playing) {
            titleScrollOffset = 0.0f;
            artistScrollOffset = 0.0f;
        }
    }

    void advanceTextScroll() {
        const ULONGLONG now = GetTickCount64();
        if (scrollTickMs == 0) {
            scrollTickMs = now;
            return;
        }
        const float dt = static_cast<float>(now - scrollTickMs) / 1000.0f;
        scrollTickMs = now;
        const bool scrollingIdleQuote = idleMode;
        if (!clientAnimations || (!scrollingIdleQuote && !media.playing))
            return;

        const float mediaAreaWidth = textAreaWidth(kPopupWidthDip);
        const float idleAreaWidth = idleQuoteAreaWidth();
        auto marquee = [&](float textWidth, float areaWidth, float& offset) {
            if (textWidth <= areaWidth) {
                offset = 0.0f;
                return;
            }
            const float loopWidth = textWidth + kPopupTextPaddingDip * 2.0f;
            offset = std::fmod(offset + kPopupInfoScrollSpeed * std::max(dt, 0.0f),
                               loopWidth);
            if (offset < 0.0f)
                offset += loopWidth;
        };
        if (scrollingIdleQuote) {
            marquee(idleQuoteWidth, idleAreaWidth, idleQuoteScrollOffset);
            marquee(idleQuoteSourceWidth, idleAreaWidth, idleQuoteSourceScrollOffset);
        } else {
            marquee(titleWidth, mediaAreaWidth, titleScrollOffset);
            marquee(artistWidth, mediaAreaWidth, artistScrollOffset);
        }
    }

    void decodeCover() {
        coverDirty = false;
        releaseCom(coverBmp);
        auto* rt = renderer.renderTarget();
        if (!rt || !media.thumbnail || media.thumbnail->empty())
            return;

        HGLOBAL hglobal = GlobalAlloc(GHND, media.thumbnail->size());
        if (!hglobal)
            return;
        void* ptr = GlobalLock(hglobal);
        if (!ptr) {
            GlobalFree(hglobal);
            return;
        }
        std::memcpy(ptr, media.thumbnail->data(), media.thumbnail->size());
        GlobalUnlock(hglobal);

        IStream* stream = nullptr;
        HRESULT hr = CreateStreamOnHGlobal(hglobal, TRUE, &stream);
        if (FAILED(hr) || !stream) {
            GlobalFree(hglobal);
            return;
        }
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            stream->Release();
            return;
        }

        UINT w = bitmap.GetWidth();
        UINT h = bitmap.GetHeight();
        const UINT targetPx = std::max(
            1u, static_cast<UINT>(std::ceil(kCoverSizeDip * scale())));
        Gdiplus::Bitmap scaled(static_cast<INT>(targetPx), static_cast<INT>(targetPx),
                               PixelFormat32bppPARGB);
        Gdiplus::Bitmap* pixels = &bitmap;
        if ((w > targetPx || h > targetPx) && scaled.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics graphics(&scaled);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            if (graphics.DrawImage(&bitmap,
                                   Gdiplus::Rect(0, 0, static_cast<INT>(targetPx),
                                                 static_cast<INT>(targetPx)),
                                   0, 0, static_cast<INT>(w), static_cast<INT>(h),
                                   Gdiplus::UnitPixel) == Gdiplus::Ok) {
                pixels = &scaled;
                w = targetPx;
                h = targetPx;
            }
        }

        Gdiplus::BitmapData data{};
        Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
        if (pixels->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB,
                             &data) != Gdiplus::Ok) {
            stream->Release();
            return;
        }
        auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        ID2D1Bitmap1* decoded = nullptr;
        hr = rt->CreateBitmap(D2D1::SizeU(w, h), data.Scan0, data.Stride, &props, &decoded);
        if (SUCCEEDED(hr))
            coverBmp = decoded;
        pixels->UnlockBits(&data);
        stream->Release();
    }

    void decodeSourceIcon() {
        sourceIconDirty = false;
        auto* rt = renderer.renderTarget();
        if (media.sourceAppUserModelId.empty()) {
            releaseCom(sourceIconBmp);
            sourceIconLoadedFor.clear();
            return;
        }
        if (!rt)
            return;

        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!platform_icon::readSourceIconPixels(media.sourceAppUserModelId, pixels, width,
                                                 height))
            return;

        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        ID2D1Bitmap1* decoded = nullptr;
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4,
                                    &props, &decoded)))
            return;

        // 只有新位图创建成功后才替换旧位图。切换期间路径解析或进程图标读取
        // 出现一次性失败时，仍然保留上一帧可用的 QQ 音乐图标。
        releaseCom(sourceIconBmp);
        sourceIconBmp = decoded;
        sourceIconLoadedFor = media.sourceAppUserModelId;
    }

    void drawText(ID2D1DeviceContext* rt, const std::wstring& text, IDWriteTextFormat* format,
                  const D2D1_RECT_F& rect, ID2D1Brush* brush) {
        media_popup_paint::drawText(rt, text, format, rect, brush);
    }

    void drawScrollingText(ID2D1DeviceContext* rt, IDWriteTextLayout* layout, float textWidth,
                           float textHeight, const D2D1_RECT_F& rect, float offset,
                           ID2D1Brush* brush, ID2D1Brush* edgeFadeBrush) {
        media_popup_paint::drawScrollingText(pagePaintContext(), rt, layout, textWidth, textHeight,
                                             rect, offset, brush, edgeFadeBrush);
    }

    void drawChevron(ID2D1DeviceContext* rt, D2D1_POINT_2F center, float size,
                     ID2D1Brush* brush, bool right = true) {
        media_popup_paint::drawChevron(rt, center, size, brush, right);
    }

    void drawVerticalChevron(ID2D1DeviceContext* rt, D2D1_POINT_2F center, float size,
                             ID2D1Brush* brush, bool up) {
        media_popup_paint::drawVerticalChevron(rt, center, size, brush, up);
    }

    bool usesBlackTextForControls(PopupPage page) {
        return media_popup_paint::usesBlackTextForControls(pagePaintContext(), page);
    }

    D2D1_COLOR_F adaptiveControlFill(bool pressed, PopupPage page) {
        return media_popup_paint::adaptiveControlFill(pagePaintContext(), pressed, page);
    }

    D2D1_COLOR_F staticControlFill(bool pressed, PopupPage page) {
        return media_popup_paint::staticControlFill(pagePaintContext(), pressed, page);
    }

    void drawAdaptiveControlSurface(ID2D1DeviceContext* rt, const D2D1_RECT_F& rect,
                                    float radius, bool pressed, PopupPage page,
                                    bool hoverSurface = true) {
        media_popup_paint::drawAdaptiveControlSurface(pagePaintContext(), rt, rect, radius, pressed,
                                                      page, hoverSurface);
    }

    void drawCopyButton(ID2D1DeviceContext* rt, const D2D1_RECT_F& rect, bool hovered,
                        bool pressed, bool succeeded) {
        media_popup_paint::drawCopyButton(pagePaintContext(), rt, rect, hovered, pressed,
                                          succeeded);
    }

    float idleUnitHeight(const IdlePresentation& content, bool current) const {
        return media_popup_paint::idleUnitHeight(content, current, idle, idleQuoteLayout,
                                                 idleQuoteHeight);
    }

    void drawIdleQuoteText(ID2D1DeviceContext* rt, float w, const IdlePresentation& content,
                           float quoteHeight, IDWriteTextLayout* layout, float textWidth,
                           float textHeight, float scrollOffset,
                           IDWriteTextLayout* sourceLayout, float sourceWidth,
                           float sourceHeight, float sourceScrollOffset) {
        media_popup_paint::drawIdleQuoteText(
            pagePaintContext(), rt, w, content, quoteHeight, layout, textWidth, textHeight,
            scrollOffset, sourceLayout, sourceWidth, sourceHeight, sourceScrollOffset);
    }

    void drawIdleQuoteUnit(ID2D1DeviceContext* rt, float w, const IdlePresentation& content,
                           bool current) {
        media_popup_paint::drawIdleQuoteUnit(pagePaintContext(), rt, w, content, current);
    }

    bool idleCopyAvailable(const IdlePresentation& content) const {
        return media_popup_paint::idleCopyAvailable(content);
    }

    bool idleReturnArrowVisible(const IdlePresentation& content) const {
        return media_popup_paint::idleReturnArrowVisible(content, playbackScene, available,
                                                         idlePanelManual);
    }

    void drawIdleCopyButton(ID2D1DeviceContext* rt, float w,
                            const IdlePresentation& content, bool updateHitTest) {
        media_popup_paint::drawIdleCopyButton(pagePaintContext(), rt, w, content, updateHitTest);
    }

    void drawIdleQuickExpandButton(ID2D1DeviceContext* rt, float w, float headerTop,
                                   const IdlePresentation& content, bool updateHitTest) {
        media_popup_paint::drawIdleQuickExpandButton(pagePaintContext(), rt, w, headerTop, content,
                                                     updateHitTest);
    }

    void drawIdleQuickTabs(ID2D1DeviceContext* rt, float w, float top, float progress,
                           const IdlePresentation& content, bool updateHitTest) {
        media_popup_paint::drawIdleQuickTabs(pagePaintContext(), rt, w, top, progress, content,
                                             updateHitTest);
    }

    void drawIdleTodayTasks(ID2D1DeviceContext* rt, const IdlePresentation& content,
                            const D2D1_RECT_F* paintBounds = nullptr) {
        media_popup_paint::drawIdleTodayTasks(pagePaintContext(), rt, content, paintBounds);
    }

    void drawIdleQuickListContent(ID2D1DeviceContext* rt, float w,
                                  const IdlePresentation& content,
                                  const D2D1_RECT_F* paintBounds = nullptr) {
        media_popup_paint::drawIdleQuickListContent(pagePaintContext(), rt, w, content,
                                                    paintBounds);
    }

    void drawIdleQuickList(ID2D1DeviceContext* rt, float w, float top,
                           const IdlePresentation& content, bool updateHitTest) {
        if (!rt)
            return;
        layoutIdleList(w, top, &content);
        const float progress = idleQuickExpandProgress(content);
        drawIdleQuickTabs(rt, w, idleQuickTabTop(top, progress, content), progress,
                          content, updateHitTest);
        const bool composited = updateHitTest && visibility.visible() && idleMode &&
                                !visibility.animating() && !categoryTransition.active() &&
                                !quickExpandTransition.active();
        if (composited) {
            // 收尾预绘制时保持滚动内容透明，避免它叠在仍可见的转场快照上。
            idleScrollView.setContentVisible(!categoryTransition.hasLayers() &&
                                             !quickExpandTransition.hasLayers());
            idleScrollView.layout(
                D2D1::RectF(idleListRect.left, idleListRect.top + cardOriginDip,
                            idleListRect.right, idleListRect.bottom + cardOriginDip),
                idleQuickListContentHeight(content), dpi, false);
            if (!idleScrollView.animating())
                idleScrollView.setOffset(idleScrollOffset);
            idleScrollOffset = idleScrollView.offset();
            idleScrollView.paint([&](fluent::FluentDialogSurface::Painter& painter,
                                     const D2D1_RECT_F& bounds, float position) {
                auto* target = static_cast<ID2D1DeviceContext*>(painter.target());
                D2D1_MATRIX_3X2_F saved{};
                target->GetTransform(&saved);
                target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, cardOriginDip) * saved);
                const auto localBounds = D2D1::RectF(bounds.left, bounds.top - cardOriginDip,
                                                     bounds.right, bounds.bottom - cardOriginDip);
                const float previous = idleScrollOffset;
                idleScrollOffset = position;
                drawIdleQuickListContent(target, w, content, &localBounds);
                idleScrollOffset = previous;
                target->SetTransform(saved);
            });
            idleScrollView.setVisible(true);
            if (idleScrollMax > 0.0f) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(idleScrollTrackRect, 1.5f, 1.5f),
                                          brushProgressTrack);
                rt->FillRoundedRectangle(D2D1::RoundedRect(idleScrollThumbRect, 1.5f, 1.5f),
                                          brushSecondary);
            }
        } else {
            // 离屏快照只负责绘制，不能提前隐藏仍在屏幕上显示的滚动层。
            if (updateHitTest)
                idleScrollView.setVisible(false);
            drawIdleQuickListContent(rt, w, content);
        }
    }

    void drawIdle(ID2D1DeviceContext* rt, float w, bool updateHitTest) {
        if (!rt)
            return;
        if (updateHitTest) {
            copyRect = {};
            idleQuickExpandRect = {};
        }
        const IdleLayout layout = idleLayout(idle, idleUnitHeight(idle, true));

        if (idle.showText) {
            // 快速启动区域从底部向上覆盖每日一言；先裁掉被覆盖的旧内容，
            // 这样在半透明卡片或动态背景下也不会残留文字。
            rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, layout.expandButtonTop),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            drawText(rt, idle.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                     D2D1::RectF(16.0f, kIdleHeaderTopDip, w - 16.0f, kIdleHeaderBottomDip),
                     brushText);

            if (idleContentTransition.active && !categoryTransition.active()) {
                const float progress = idleContentTransitionProgress();
                if (progress >= 1.0f) {
                    idleContentTransition.active = false;
                    drawIdleQuoteUnit(rt, w, idle, true);
                } else {
                    const float travel = w + 24.0f;
                    const float oldOffset = -travel * progress;
                    const float newOffset = travel * (1.0f - progress);
                    const float transitionBottom = std::min(layout.sourceTop + 18.0f,
                                                            layout.expandButtonTop);
                    if (transitionBottom > kIdleHeaderBottomDip) {
                        rt->PushAxisAlignedClip(D2D1::RectF(8.0f, kIdleHeaderBottomDip,
                                                            w - 8.0f, transitionBottom),
                                                D2D1_ANTIALIAS_MODE_ALIASED);
                        rt->SetTransform(D2D1::Matrix3x2F::Translation(oldOffset, cardOriginDip));
                        drawIdleQuoteUnit(rt, w, idleContentTransition.from, false);
                        rt->SetTransform(D2D1::Matrix3x2F::Translation(newOffset, cardOriginDip));
                        drawIdleQuoteUnit(rt, w, idle, true);
                        rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, cardOriginDip));
                        rt->PopAxisAlignedClip();
                    }
                }
            } else {
                drawIdleQuoteUnit(rt, w, idle, true);
            }

            drawIdleCopyButton(rt, w, idle, updateHitTest);
            rt->PopAxisAlignedClip();

            if (updateHitTest && copyRect.right > copyRect.left &&
                copyRect.bottom > cardOriginDip + layout.expandButtonTop)
                copyRect = {};
        }

        drawIdleQuickDivider(rt, w, layout.headerTop, idle);
        drawIdleQuickHeader(rt, w, layout.headerTop, idle, updateHitTest);
        drawIdleQuickList(rt, w, layout.listTop, idle, updateHitTest);
    }

    // 标题/艺术家两行滚动文本：活页用缓存布局与当前滚动偏移，转场快照用
    // 临时布局与切换前保存的偏移，几何完全一致。
    void drawMediaTitles(ID2D1DeviceContext* rt, float w, IDWriteTextLayout* title,
                         float titleW, float titleH, float titleOffset,
                         IDWriteTextLayout* artist, float artistW, float artistH,
                         float artistOffset) {
        media_popup_paint::drawMediaTitles(pagePaintContext(), rt, w, title, titleW, titleH,
                                           titleOffset, artist, artistW, artistH, artistOffset);
    }

    void drawMediaPet(ID2D1DeviceContext* rt, float w) {
        media_popup_paint::drawMediaPet(pagePaintContext(), rt, w);
    }

    void drawMediaContent(ID2D1DeviceContext* rt, float w, bool useCachedLayers) {
        media_popup_paint::drawMediaContent(pagePaintContext(), rt, w, useCachedLayers);
    }

    void drawMediaSnapshot(ID2D1DeviceContext* rt, float w,
                           const OverlayMediaInfo& snapshot, bool useCachedLayers) {
        media_popup_paint::drawMediaSnapshot(pagePaintContext(), rt, w, snapshot, useCachedLayers);
    }

    void drawPageArrow(ID2D1DeviceContext* rt, float w, PopupPage page,
                       bool updateHitTest) {
        if (updateHitTest)
            pageArrowRect = {};
        if (!rt || !playbackScene || !available || !idlePageAllowed() ||
            (page != PopupPage::Media && !(page == PopupPage::Idle && idlePanelManual)))
            return;

        const D2D1_RECT_F local = D2D1::RectF(w - 44.0f, 7.0f, w - 12.0f, 39.0f);
        if (updateHitTest)
            pageArrowRect = D2D1::RectF(local.left, cardOriginDip + local.top,
                                        local.right, cardOriginDip + local.bottom);
        // 高亮跟随视觉状态而不是命中测试标志：合成层路径（updateHitTest=false）
        // 必须画出与交换链一致的高亮，否则点击箭头触发转场时，按下/悬停高亮
        // 会在层挂载与撤层瞬间凭空消失/出现。
        if (hoverPageArrow || pressedPageArrow)
            drawAdaptiveControlSurface(rt, local, 8.0f, pressedPageArrow, page);
        drawChevron(rt,
                    D2D1::Point2F((local.left + local.right) * 0.5f,
                                  (local.top + local.bottom) * 0.5f),
                    7.0f, brushSecondary, page == PopupPage::Media);
    }

    void drawBackdrop(ID2D1DeviceContext* rt, float w, float h, bool enabled,
                      bool useCachedLayer) {
        if (!enabled || !rt || (!backdropBlur && !backdropBmp))
            return;

        ID2D1Layer* temporaryLayer = nullptr;
        ID2D1Layer* clipLayer = useCachedLayer ? backdropLayer : nullptr;
        if (backdropClip && !clipLayer && FAILED(rt->CreateLayer(&temporaryLayer)))
            return;
        clipLayer = clipLayer ? clipLayer : temporaryLayer;
        const bool clipped = backdropClip && clipLayer;
        if (clipped) {
            rt->PushLayer(D2D1::LayerParameters1(
                              D2D1::InfiniteRect(), backdropClip,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                          clipLayer);
        }
        if (backdropBlur) {
            const D2D1_POINT_2F offset = D2D1::Point2F(0.0f, 0.0f);
            rt->DrawImage(backdropBlur, &offset, nullptr, D2D1_INTERPOLATION_MODE_LINEAR,
                          D2D1_COMPOSITE_MODE_SOURCE_OVER);
        } else {
            rt->DrawBitmap(backdropBmp, D2D1::RectF(0.0f, 0.0f, w, h), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (clipped)
            rt->PopLayer();
        if (temporaryLayer)
            temporaryLayer->Release();
    }

    void drawBackdrop(ID2D1DeviceContext* rt, float w, float h) {
        drawBackdrop(rt, w, h, frostedBackgroundActive(), true);
    }

    void drawDynamicBackground(ID2D1DeviceContext* rt, float w, float h, bool enabled) {
        if (!enabled || !rt || !brushDynamicGradient || !brushDynamicGlow)
            return;

        brushDynamicGradient->SetStartPoint(D2D1::Point2F(0.0f, 0.0f));
        brushDynamicGradient->SetEndPoint(D2D1::Point2F(w, h));
        brushDynamicGradient->SetOpacity(0.51f);
        brushDynamicGlow->SetCenter(D2D1::Point2F(w * 0.74f, h * 0.28f));
        brushDynamicGlow->SetGradientOriginOffset(D2D1::Point2F(-w * 0.05f, -h * 0.06f));
        brushDynamicGlow->SetRadiusX(w * 0.72f);
        brushDynamicGlow->SetRadiusY(h * 0.92f);
        brushDynamicGlow->SetOpacity(0.25f);

        const auto card = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, w, h),
                                             kPopupCornerDip, kPopupCornerDip);
        rt->FillRoundedRectangle(card, brushDynamicGradient);
        rt->FillRoundedRectangle(card, brushDynamicGlow);
    }

    void drawDynamicBackground(ID2D1DeviceContext* rt, float w, float h) {
        drawDynamicBackground(rt, w, h, dynamicBackgroundEnabled());
    }

    void drawCover(ID2D1DeviceContext* rt, bool useCachedLayer) {
        media_popup_paint::drawCover(pagePaintContext(), rt, useCachedLayer);
    }

    void drawSourceIcon(ID2D1DeviceContext* rt, const std::wstring& source) {
        media_popup_paint::drawSourceIcon(pagePaintContext(), rt, source);
    }

    void drawSource(ID2D1DeviceContext* rt) {
        media_popup_paint::drawSource(pagePaintContext(), rt);
    }

    void drawProgress(ID2D1DeviceContext* rt, float w, const OverlayMediaInfo& content) {
        media_popup_paint::drawProgress(pagePaintContext(), rt, w, content);
    }

    bool buttonEnabled(int index, const OverlayMediaInfo& content) const {
        return media_popup_paint::buttonEnabled(available, index, content);
    }

    void drawButton(ID2D1DeviceContext* rt, int index, const OverlayMediaInfo& content) {
        media_popup_paint::drawButton(pagePaintContext(), rt, index, content);
    }

    void drawVolumeControl(ID2D1DeviceContext* rt, float w, PopupPage page,
                           bool interactive) {
        // 图标固定在卡片右上角（来源行右端）；悬浮底是与播放键一致的 32px 圆形
        const float cy = 23.0f;
        const bool arrowVisible = playbackScene && available && idlePageAllowed() &&
                                  page == PopupPage::Media;
        const float volumeRight = arrowVisible ? w - 52.0f : w - 6.0f;
        const D2D1_RECT_F localIconRect =
            D2D1::RectF(volumeRight - 32.0f, cy - 16.0f, volumeRight, cy + 16.0f);
        volumeIconRect = D2D1::RectF(
            localIconRect.left, localIconRect.top + cardOriginDip,
            localIconRect.right, localIconRect.bottom + cardOriginDip);

        // 滑块轨道目标矩形：图标左侧 112 DIP（与内嵌控件音量浮窗宽度相当）
        const float trackRight = localIconRect.left - 10.0f;
        const float trackLeft = trackRight - 112.0f;
        const D2D1_RECT_F localSliderRect =
            D2D1::RectF(trackLeft, cy - 2.0f, trackRight, cy + 2.0f);
        volumeSliderRect = D2D1::RectF(
            localSliderRect.left, localSliderRect.top + cardOriginDip,
            localSliderRect.right, localSliderRect.bottom + cardOriginDip);

        // easeOutCubic：展开从图标处向左长出，收起反向缩回
        const float t = volumeSliderT <= 0.0f
                            ? 0.0f
                            : 1.0f - std::pow(1.0f - volumeSliderT, 3.0f);
        if (t > 0.0f && volume.available) {
            const float revealLeft = trackRight - (trackRight - trackLeft) * t;
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(revealLeft, cy - 2.0f, trackRight, cy + 2.0f),
                                  2.0f, 2.0f),
                brushProgressTrack);
            const float fraction = std::clamp(volume.percent, 0, 100) / 100.0f;
            const float fillRight = trackLeft + (trackRight - trackLeft) * fraction;
            const float fillLeft = std::max(trackLeft, revealLeft);
            if (fillRight > fillLeft)
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(fillLeft, cy - 2.0f, fillRight, cy + 2.0f),
                                      2.0f, 2.0f),
                    brushAccent);
            if (fillRight >= revealLeft && brushTextOnAccent) {
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillRight, cy), 7.0f, 7.0f),
                                brushAccent);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillRight, cy), 2.8f, 2.8f),
                                brushTextOnAccent);
            }
            // 数值随展开淡入，位于轨道左侧（右端预留超过旋钮半径的空隙，0% 时不贴 knob）
            const std::wstring expandedText = std::to_wstring(volume.percent) + L"%";
            brushSecondary->SetOpacity(t);
            drawText(rt, expandedText, fmtTimeRight,
                     D2D1::RectF(trackLeft - 58.0f, cy - 12.0f, trackLeft - 12.0f, cy + 12.0f),
                     brushSecondary);
            brushSecondary->SetOpacity(1.0f);
        } else {
            // 收起状态：数值在图标左侧
            const std::wstring text =
                volume.available ? std::to_wstring(volume.percent) + L"%" : L"--";
            drawText(rt, text, fmtTimeRight,
                     D2D1::RectF(volumeIconRect.left - 52.0f, cy - 12.0f,
                                 volumeIconRect.left - 6.0f, cy + 12.0f),
                     volume.available ? brushSecondary : brushDisabled);
        }

        // 图标悬浮底：与播放键一致的圆形
        if (volume.available &&
            ((interactive && (hoverVolume || pressedVolume)) || volumeSliderOn)) {
            drawAdaptiveControlSurface(rt, localIconRect, 16.0f, pressedVolume, page);
        }
        const float iconCx = (localIconRect.left + localIconRect.right) * 0.5f;
        const int level = !volume.available || volume.muted
                              ? 0
                              : volume.percent == 0 ? 1 : volume.percent < 50 ? 2 : 3;
        media_control::drawVolume(rt, D2D1::Point2F(iconCx, cy), 8.0f,
                                  volume.available ? brushText : brushDisabled, level);
    }

    void drawIdleSnapshot(ID2D1DeviceContext* rt, float w, const IdlePresentation& content) {
        IDWriteTextLayout* snapshotQuoteLayout = nullptr;
        IDWriteTextLayout* snapshotSourceLayout = nullptr;
        float snapshotQuoteWidth = 0.0f;
        float snapshotQuoteHeight = 0.0f;
        float snapshotSourceWidth = 0.0f;
        float snapshotSourceHeight = 0.0f;
        createMeasuredTextLayout(content.sentence, fmtIdleQuote, 42.0f, &snapshotQuoteLayout,
                                 snapshotQuoteWidth, snapshotQuoteHeight);
        snapshotQuoteHeight = std::min(42.0f, snapshotQuoteHeight);
        createMeasuredTextLayout(content.source, fmtIdleSource, 18.0f, &snapshotSourceLayout,
                                 snapshotSourceWidth, snapshotSourceHeight);
        snapshotSourceHeight = std::min(18.0f, snapshotSourceHeight);
        const float quoteHeight = snapshotQuoteLayout
                                       ? std::max(18.0f, snapshotQuoteHeight)
                                       : 18.0f;
        const IdleLayout layout = idleLayout(content, quoteHeight);

        if (content.showText) {
            rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, layout.expandButtonTop),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            drawText(rt, content.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                     D2D1::RectF(16.0f, kIdleHeaderTopDip, w - 16.0f, kIdleHeaderBottomDip),
                     brushText);
            drawIdleQuoteText(rt, w, content, quoteHeight, snapshotQuoteLayout, snapshotQuoteWidth,
                              snapshotQuoteHeight, categorySnapshot.idleQuoteScrollOffset,
                              snapshotSourceLayout, snapshotSourceWidth, snapshotSourceHeight,
                              categorySnapshot.idleQuoteSourceScrollOffset);
            drawIdleCopyButton(rt, w, content, false);
            rt->PopAxisAlignedClip();
        }

        drawIdleQuickDivider(rt, w, layout.headerTop, content);
        drawIdleQuickHeader(rt, w, layout.headerTop, content, false);
        const float savedScrollOffset = idleScrollOffset;
        idleScrollOffset = categorySnapshot.idleScrollOffset;
        drawIdleQuickList(rt, w, layout.listTop, content, false);
        idleScrollOffset = savedScrollOffset;
        releaseCom(snapshotQuoteLayout);
        releaseCom(snapshotSourceLayout);
    }

    void drawPageContent(ID2D1DeviceContext* rt, float w, PopupPage page, bool oldLayer,
                         bool updateHitTest, bool useCachedLayers) {
        if (page == PopupPage::Idle) {
            if (oldLayer && categorySnapshot.from == PopupPage::Idle)
                drawIdleSnapshot(rt, w, categorySnapshot.idle);
            else
                drawIdle(rt, w, updateHitTest);
        } else {
            if (oldLayer && categorySnapshot.from == PopupPage::Media)
                drawMediaSnapshot(rt, w, categorySnapshot.media, useCachedLayers);
            else
                drawMediaContent(rt, w, useCachedLayers);
        }
    }

    void drawPageLayer(ID2D1DeviceContext* rt, float w, PopupPage page, bool oldLayer,
                       bool updateHitTest, bool useCachedLayers) {
        drawPageContent(rt, w, page, oldLayer, updateHitTest, useCachedLayers);
        if (page == PopupPage::Media)
            drawVolumeControl(rt, w, page, updateHitTest);
        drawPageArrow(rt, w, page, updateHitTest);
    }

    void drawCardBackground(ID2D1DeviceContext* rt, float w, float h, PopupPage page,
                            bool useCachedLayer) {
        if (!rt)
            return;

        D2D1_COLOR_F previousFill{};
        if (brushBackground)
            previousFill = brushBackground->GetColor();
        if (brushBackground)
            brushBackground->SetColor(cardFillForPage(page));
        drawBackdrop(rt, w, h, frostedBackgroundActiveForPage(page), useCachedLayer);

        const auto card = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, w, h),
                                             kPopupCornerDip, kPopupCornerDip);
        rt->FillRoundedRectangle(card, brushBackground);
        drawDynamicBackground(rt, w, h, dynamicBackgroundEnabledForPage(page));
        rt->DrawRoundedRectangle(card, brushStroke, 1.0f);

        // 页面切换不再重建画笔；目标页的文字对比色要在普通交换链接管前
        // 由当前页面显式恢复，避免复用上一页画笔颜色。
        if (hasBackdropTextContrastForPage(page)) {
            applyBackdropTextContrastForPage(lastBackdropLuminance, page);
        } else {
            resetBackdropTextColors();
        }

        if (brushBackground)
            brushBackground->SetColor(previousFill);
    }

    void drawCategoryContentLayer(ID2D1DeviceContext* rt, float w, PopupPage page,
                                  bool oldLayer) {
        if (!rt)
            return;

        D2D1_COLOR_F previousText{};
        D2D1_COLOR_F previousSecondary{};
        D2D1_COLOR_F previousDisabled{};
        if (brushText)
            previousText = brushText->GetColor();
        if (brushSecondary)
            previousSecondary = brushSecondary->GetColor();
        if (brushDisabled)
            previousDisabled = brushDisabled->GetColor();

        if (hasBackdropTextContrastForPage(page)) {
            applyBackdropTextContrastForPage(lastBackdropLuminance, page);
        } else {
            resetBackdropTextColors();
        }

        // 内容层随卡片横向移动。内容不经过外层几何裁剪：各元素均够不到圆角
        // 区，新页卡片背景自身已是圆角绘制（模糊图有内层裁剪），与交换链稳态
        // 渲染保持一致——避免层中间纹理重采样导致文字在交接瞬间偏移。
        // 新页背景随内容一起滑入（旧页背景由固定背景层保留到转场结束），
        // 收尾时交换链终帧与层内终态一致，撤层不再出现背景换色。
        if (!oldLayer)
            drawCardBackground(rt, w, popupHeightDip(), page, false);
        drawPageLayer(rt, w, page, oldLayer, false, false);

        if (brushText)
            brushText->SetColor(previousText);
        if (brushSecondary)
            brushSecondary->SetColor(previousSecondary);
        if (brushDisabled)
            brushDisabled->SetColor(previousDisabled);
    }

    void drawCategoryContent(ID2D1DeviceContext* rt, float w, float cardH) {
        // 页面互切期间，两页内容承载在两个 DComp 合成层上，横向滑动由合成器
        // 按刷新率执行（BeginDraw 前统一启动）；交换链保留上一张完整卡片，不
        // 重绘或提交透明底，UI 线程不再逐帧刷新主卡片。
        if (categoryTransition.active())
            return;
        // 快速打开展开/收起期间，一言区、标题/Tab、列表主体承载在 DComp 合成层上，
        // 交换链只画卡片底与返回箭头，不重复绘制空闲页内容。
        if (quickExpandTransition.active() && currentPage() == PopupPage::Idle) {
            drawPageArrow(rt, w, PopupPage::Idle, false);
            return;
        }
        drawPageLayer(rt, w, currentPage(), false, true, true);
    }

    // 把旧页快照和新页内容分别画进两个 DComp 合成层并启动横向位移动画。
    // 必须在 createResources/文本布局/封面与图标解码之后调用，保证两页都按
    // 最新画笔和内容绘制。失败返回 false，调用方直接呈现最终页。
    bool startCategoryLayerTransition(float w) {
        if (!hwnd || cardWidthPx <= 0 || cardHeightPx <= 0 ||
            !renderer.ensureTransitionLayers(cardWidthPx, cardHeightPx, cardWidthPx,
                                                  cardHeightPx, kPopupCornerDip * scale()))
            return false;

        const float baseY = cardOriginDip * scale();
        if (!renderer.ensureTransitionBackdrop(cardWidthPx, cardHeightPx, baseY,
                                                    kPopupCornerDip * scale()))
            return false;
        if (auto* dc = renderer.beginTransitionBackdropDraw()) {
            drawCardBackground(dc, w, popupHeightDip(), categorySnapshot.from, false);
            if (!renderer.endTransitionBackdropDraw(dc))
                return false;
        } else {
            return false;
        }

        struct LayerJob {
            int index;
            PopupPage page;
            bool oldLayer;
        };
        const LayerJob jobs[2] = {
            {0, categorySnapshot.from, true},
            {1, currentPage(), false},
        };
        for (const auto& job : jobs) {
            auto* dc = renderer.beginTransitionLayerDraw(job.index);
            if (!dc)
                return false;
            drawCategoryContentLayer(dc, w, job.page, job.oldLayer);
            if (!renderer.endTransitionLayerDraw(job.index, dc))
                return false;
        }

        // 先以挂载初始状态（内容层创建即 0 透明度、背景层同为 0 透明度）
        // 提交并等待合成器处理完，让新表面先进入合成树；再启动动画并把背景层
        // 转可见，避免新表面首帧在合成器里显示空白底色。根交换链在整个页面
        // 互切期间保持上一张完整卡片，不再出现空白交换链帧。
        renderer.commit();
        renderer.waitForCommitCompletion();
        const float travel = kCategoryTransitionTravelDip * scale();
        const float dir = static_cast<float>(categorySnapshot.direction);
        const float durationSec = kCategoryTransitionMs / 1000.0f;
        if (!renderer.showTransitionBackdrop() ||
            !renderer.animateTransitionLayerX(0, 0.0f, -dir * travel, baseY, 1.0f, 0.0f,
                                         durationSec) ||
            !renderer.animateTransitionLayerX(1, dir * travel, 0.0f, baseY, 0.0f, 1.0f,
                                         durationSec))
            return false;
        categoryTransition.markComposited();
        // 滚动层与转场层共用合成设备，在同一事务中交接，避免重复显示。
        idleScrollView.setContentVisible(false);
        renderer.commit();
        // 合成器动画在 kCategoryTransitionMs 后停在终点；定时器稍加余量收尾：
        // 先预绘制目标页，再撤层提交。余量内层已静止在终点，交接无跳变。
        SetTimer(hwnd, kCategoryTimer, static_cast<UINT>(kCategoryTransitionMs) + 32,
                 nullptr);
        return true;
    }

    // 快速打开展开/收起：render() 先把只包含卡片背景和返回按钮的基准帧绘制到
    // 当前交换链后备缓冲，再把固定背景、一言区（含分隔线）和展开态的快速打开区
    // 交给 DComp。固定背景覆盖交换链，避免背景随 Present 交接单独闪烁；内容层只
    // 做合成器位移、裁剪和透明度。展开时快速打开区整体上移并淡入，收起时整体下移
    // 并淡出。列表视口高度通过层内矩形裁剪底边收放（该底边在层内坐标系中与位移
    // 无关，直接按两个端点值插值）。一言区原地不动，由裁剪底边扫过模拟被覆盖/显露。
    // 几何与 drawIdle 的逐帧版本在两个端点完全一致。
    bool startQuickExpandLayerTransition(float w) {
        if (!hwnd || cardWidthPx <= 0 || cardHeightPx <= 0)
            return false;

        const float s = scale();
        const bool opening = quickExpansion.expanded;
        const IdleLayout layout = idleLayout(idle, idleUnitHeight(idle, true));
        const float collapsedHeaderTop = layout.collapsedHeaderTop;
        const float expandedHeaderTop = kIdleQuickExpandedTopDip;
        const float delta = std::max(0.0f, collapsedHeaderTop - expandedHeaderTop);
        const float expandedButtonTop = idleQuickExpandButtonTop(expandedHeaderTop);
        const float collapsedButtonTop = idleQuickExpandButtonTop(collapsedHeaderTop);
        // 层 1（快速打开区）原点对齐展开态按钮顶；按钮与标题同体平移
        const float quickOriginY = expandedButtonTop;

        const float expandedListTop = idleQuickListTop(expandedHeaderTop) +
                                      idleQuickTabContentOffset(1.0f, idle);
        const float collapsedListTop = idleQuickListTop(collapsedHeaderTop) +
                                       idleQuickTabContentOffset(0.0f, idle);
        auto availableHeight = [&](float listTop) {
            return std::max(0.0f, kIdlePopupHeightDip - listTop -
                                      kIdleListBottomPaddingDip);
        };
        const float expandedListHeight =
            std::min(idleQuickListHeight(idle, 1.0f), availableHeight(expandedListTop));
        const float collapsedListHeight =
            std::min(idleQuickListHeight(idle, 0.0f), availableHeight(collapsedListTop));
        // 层内坐标（原点 = 卡片 y=quickOriginY）的列表可见底边端点
        const float clipBottomExpanded =
            (expandedListTop - quickOriginY) + expandedListHeight;
        const float clipBottomCollapsed =
            (expandedListTop - quickOriginY) + collapsedListHeight;
        const float expandedTabTop = idleQuickTabTop(expandedListTop, 1.0f, idle);

        const int layer0W = cardWidthPx;
        const int layer0H = static_cast<int>(std::lround(collapsedButtonTop * s)) + 1;
        const int layer1W = cardWidthPx;
        const int layer1H = static_cast<int>(std::lround(clipBottomExpanded * s)) + 1;
        const int layer2W = cardWidthPx;
        const int layer2H = static_cast<int>(std::lround(32.0f * s)) + 1;
        const int layer3W = cardWidthPx;
        const int layer3H = static_cast<int>(std::lround(expandedListHeight * s)) + 1;
        if (!renderer.ensureTransitionLayers(layer0W, layer0H, layer1W, layer1H,
                                                  layer2W, layer2H, layer3W, layer3H))
            return false;
        const float baseY = cardOriginDip * s;
        if (!renderer.ensureTransitionBackdrop(cardWidthPx, cardHeightPx, baseY,
                                                    kPopupCornerDip * s))
            return false;
        if (auto* dc = renderer.beginTransitionBackdropDraw()) {
            drawCardBackground(dc, w, popupHeightDip(), PopupPage::Idle, false);
            // 固定背景层位于交换链之上，因此把快速展开期间仍需可见的页面
            // 返回箭头一并画进稳定层，避免它被整卡片背景覆盖。
            drawPageArrow(dc, w, PopupPage::Idle, false);
            if (!renderer.endTransitionBackdropDraw(dc))
                return false;
        } else {
            return false;
        }

        // 层 0：一言区，原点即卡片原点；分隔线画在收起位置，随裁剪底边扫过
        // 被隐藏/显露。当前交换链后备缓冲已经绘制成无内容基准帧，不会与旧内容重影。
        if (auto* dc = renderer.beginTransitionLayerDraw(0)) {
            if (idle.showText) {
                drawText(dc, idle.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                         D2D1::RectF(16.0f, kIdleHeaderTopDip, w - 16.0f,
                                     kIdleHeaderBottomDip),
                         brushText);
                drawIdleQuoteUnit(dc, w, idle, true);
                drawIdleCopyButton(dc, w, idle, false);
                if (brushProgressTrack) {
                    const float dividerTop = collapsedButtonTop - 4.0f;
                    dc->FillRectangle(
                        D2D1::RectF(16.0f, dividerTop, w - 16.0f, dividerTop + 1.0f),
                        brushProgressTrack);
                }
            }
            if (!renderer.endTransitionLayerDraw(0, dc))
                return false;
        } else {
            return false;
        }

        // 层 1：快速打开区的标题和 Tab，按展开态绘制。展开时上移并淡入，
        // 收起时下移并淡出；列表主体单独放在层 3，避免图标跟随整层透明度
        // 在转场首尾闪烁。
        if (auto* dc = renderer.beginTransitionLayerDraw(1)) {
            const bool savedExpanded = quickExpansion.expanded;
            const float savedT = quickExpansion.progress;
            quickExpansion.expanded = true;
            quickExpansion.progress = 1.0f;
            // 保留 BeginDraw 的表面更新偏移，在其上叠加层原点平移
            D2D1_MATRIX_3X2_F baseTransform;
            dc->GetTransform(&baseTransform);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -quickOriginY) *
                             baseTransform);
            drawIdleQuickHeader(dc, w, expandedHeaderTop, idle, false);
            drawIdleQuickTabs(dc, w, expandedTabTop, 1.0f, idle, false);
            quickExpansion.expanded = savedExpanded;
            quickExpansion.progress = savedT;
            if (!renderer.endTransitionLayerDraw(1, dc))
                return false;
        } else {
            return false;
        }

        // 层 2：收起态标题/按钮。它与层 1 使用相同的垂直轨迹，但反向交叉淡入，
        // 使收起动画的最后一帧已经包含最终静态态的标题和按钮，避免撤层时补画闪烁。
        if (auto* dc = renderer.beginTransitionLayerDraw(2)) {
            const bool savedExpanded = quickExpansion.expanded;
            const float savedT = quickExpansion.progress;
            quickExpansion.expanded = false;
            quickExpansion.progress = 0.0f;
            D2D1_MATRIX_3X2_F baseTransform;
            dc->GetTransform(&baseTransform);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -collapsedButtonTop) *
                             baseTransform);
            drawIdleQuickHeader(dc, w, collapsedHeaderTop, idle, false);
            quickExpansion.expanded = savedExpanded;
            quickExpansion.progress = savedT;
            if (!renderer.endTransitionLayerDraw(2, dc))
                return false;
        } else {
            return false;
        }

        // 层 3：列表主体（应用图标/任务行）。只绘制展开态主体，但把它的局部
        // 原点放在列表顶部，因此视觉层可以从收起态列表顶平滑移动到展开态列表顶，
        // 且整个过程保持不透明，避免图标在交换链清空或层淡出时短暂消失。
        if (auto* dc = renderer.beginTransitionLayerDraw(3)) {
            const bool savedExpanded = quickExpansion.expanded;
            const float savedT = quickExpansion.progress;
            const D2D1_RECT_F savedListRect = idleListRect;
            const D2D1_RECT_F savedScrollTrackRect = idleScrollTrackRect;
            const D2D1_RECT_F savedScrollThumbRect = idleScrollThumbRect;
            const float savedScrollMax = idleScrollMax;
            const float savedScrollOffset = idleScrollOffset;
            quickExpansion.expanded = true;
            quickExpansion.progress = 1.0f;
            layoutIdleList(w, expandedListTop, &idle);
            D2D1_MATRIX_3X2_F baseTransform;
            dc->GetTransform(&baseTransform);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -expandedListTop) *
                             baseTransform);
            drawIdleQuickListContent(dc, w, idle);
            quickExpansion.expanded = savedExpanded;
            quickExpansion.progress = savedT;
            idleListRect = savedListRect;
            idleScrollTrackRect = savedScrollTrackRect;
            idleScrollThumbRect = savedScrollThumbRect;
            idleScrollMax = savedScrollMax;
            idleScrollOffset = savedScrollOffset;
            if (!renderer.endTransitionLayerDraw(3, dc))
                return false;
        } else {
            return false;
        }

        // 先挂载透明的背景/内容层并等待表面进入合成树，再转可见并设置动画。
        // 这样下面交换链的基准帧无论何时 Present，背景都已经由固定层覆盖。
        renderer.commit();
        renderer.waitForCommitCompletion();
        if (!renderer.showTransitionBackdrop())
            return false;

        const float quoteBaseY = baseY;
        const float quickBaseY = (cardOriginDip + quickOriginY) * s;
        const float collapsedHeaderY = (cardOriginDip + collapsedButtonTop) * s;
        const float expandedHeaderY = (cardOriginDip + expandedButtonTop) * s;
        const float collapsedBodyY = (cardOriginDip + collapsedListTop) * s;
        const float expandedBodyY = (cardOriginDip + expandedListTop) * s;
        const float travelPx = delta * s;
        const float durationSec = kIdleQuickExpandMs / 1000.0f;
        if (!renderer.animateTransitionLayerClipSlide(
                0, 0.0f, quoteBaseY, quoteBaseY,
                (opening ? collapsedButtonTop : expandedButtonTop) * s,
                (opening ? expandedButtonTop : collapsedButtonTop) * s, durationSec))
            return false;
        if (!renderer.animateTransitionLayerClipSlide(
                1, 0.0f, opening ? quickBaseY + travelPx : quickBaseY,
                opening ? quickBaseY : quickBaseY + travelPx,
                (opening ? clipBottomCollapsed : clipBottomExpanded) * s,
                (opening ? clipBottomExpanded : clipBottomCollapsed) * s, durationSec,
                opening ? 0.0f : 1.0f, opening ? 1.0f : 0.0f))
            return false;
        if (!renderer.animateTransitionLayerClipSlide(
                2, 0.0f, opening ? collapsedHeaderY : expandedHeaderY,
                opening ? expandedHeaderY : collapsedHeaderY, static_cast<float>(layer2H),
                static_cast<float>(layer2H), durationSec, opening ? 1.0f : 0.0f,
                opening ? 0.0f : 1.0f))
            return false;
        if (!renderer.animateTransitionLayerClipSlide(
                3, 0.0f, opening ? collapsedBodyY : expandedBodyY,
                opening ? expandedBodyY : collapsedBodyY,
                (opening ? collapsedListHeight : expandedListHeight) * s,
                (opening ? expandedListHeight : collapsedListHeight) * s, durationSec))
            return false;
        quickExpandTransition.markComposited();
        // 原列表不能留在移动快照上方；与动画启动一起提交隐藏操作。
        idleScrollView.setContentVisible(false);
        renderer.commit();
        return true;
    }

    bool render() {
        // 仅打开准备阶段和可见阶段允许创建设备，隐藏后的消息不能重建缓存。
        if (!hwnd || (!visibility.visible() && !visibility.opening()))
            return false;
        if ((!idleMode && !categoryTransition.active()) || !visibility.visible() ||
            visibility.animating()) {
            idleScrollOffset = idleScrollView.offset();
            idleScrollView.stop();
            idleScrollView.setVisible(false);
        }
        // 合成层已经覆盖卡片时，任何额外的交换链 Present 都可能把透明底单独
        // 推给合成器；状态会在转场收尾时统一预绘制并交接。
        if ((categoryTransition.active() && categoryTransition.hasLayers()) ||
            (quickExpandTransition.active() && quickExpandTransition.hasLayers()))
            return true;
        if (presentationUpdateDepth > 0) {
            presentationUpdatePending = true;
            return true;
        }
        const auto renderStarted = runtime_log::uiTimeMs();
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int pxW = rc.right - rc.left;
        const int pxH = rc.bottom - rc.top;
        if (pxW <= 0 || pxH <= 0)
            return false;
        if (!renderer.bind(hwnd, pxW, pxH))
            return false;
        const auto renderBound = runtime_log::uiTimeMs();
        if (deviceGeneration != renderer.deviceGeneration()) {
            if (deviceGeneration != 0)
                releaseDeviceResources();
            deviceGeneration = renderer.deviceGeneration();
        }
        if (materialNeedsApply) {
            // 必须在 DirectComposition target 绑定到 HWND 后再设置材质，
            // 否则绑定过程可能重建窗口合成表面并覆盖背景效果。
            applyWindowMaterial();
            materialNeedsApply = false;
        }
        renderer.setDpi(dpi);
        const float cardTopPx = cardOriginDip * scale();
        if (!renderer.setRootRoundedClip(cardTopPx, cardTopPx + static_cast<float>(cardHeightPx),
                                         kPopupCornerDip * scale()))
            return false;
        if (!createResources())
            return false;
        const auto resourcesReady = runtime_log::uiTimeMs();
        const bool categoryTransitionNeedsDynamicBackground =
            categoryTransition.active() &&
            (dynamicBackgroundEnabledForPage(categorySnapshot.from) ||
             dynamicBackgroundEnabledForPage(currentPage()));
        if (categoryTransitionNeedsDynamicBackground)
            createDynamicBackgroundResources(true);
        else if (dynamicBackgroundEnabled())
            createDynamicBackgroundResources();
        const bool categoryTransitionNeedsBackdrop =
            categoryTransition.active() && (frostedBackgroundActiveForPage(categorySnapshot.from) ||
                                            frostedBackgroundActiveForPage(currentPage()));
        const bool prewarmBackdrop =
            !visibility.visible() && (frostedBackgroundActiveForPage(PopupPage::Media) ||
                                      frostedBackgroundActiveForPage(PopupPage::Idle));
        if (frostedBackgroundActive() || categoryTransitionNeedsBackdrop || prewarmBackdrop)
            captureBackdrop(!frostedBackgroundActive() &&
                            (categoryTransitionNeedsBackdrop || prewarmBackdrop));
        const auto backdropReady = runtime_log::uiTimeMs();
        buildTextLayouts();
        if (coverDirty)
            decodeCover();
        if (sourceIconDirty)
            decodeSourceIcon();
        if (idleMode || (categoryTransition.active() && categorySnapshot.from == PopupPage::Idle)) {
            if (idleMode)
                buildIdleTextLayout();
            decodeIdleIcons();
        }
        const auto contentReady = runtime_log::uiTimeMs();

        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;
        const float w = dip(pxW);
        const float cardH = popupHeightDip();
        const float infoW = textAreaWidth(w);
        updateScrollTimer(infoW);
        mediaLayout_ = media_popup_detail::mediaPageLayout(w, cardOriginDip);

        // 页面互切：把固定圆角底板、两页内容和横向位移动画一次性提交到
        // DComp。主交换链保留上一张完整卡片，切换期间不再清空或 Present。
        if (categoryTransition.active() && !categoryTransition.hasLayers() &&
            !startCategoryLayerTransition(w)) {
            stopCategoryTransition();
        }
        const bool categoryLayersCoverCard =
            categoryTransition.active() && categoryTransition.hasLayers();
        const bool quickExpandLayersCoverCard =
            quickExpandTransition.active() && quickExpandTransition.hasLayers();
        if (categoryLayersCoverCard || quickExpandLayersCoverCard) {
            idleScrollView.setVisible(false);
            return true;
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, cardOriginDip));
        drawCardBackground(rt, w, cardH, currentPage(), true);
        drawCategoryContent(rt, w, cardH);

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const HRESULT hr = rt->EndDraw();
        const auto renderDrawn = runtime_log::uiTimeMs();
        if (FAILED(hr)) {
            runtime_log::writef(
                L"[media-popup] EndDraw failed: 0x%08X, releasing drawing resources",
                static_cast<unsigned>(hr));
            releaseDrawingResources();
            return false;
        }
        if (songTransitionPending) {
            songTransitionPending = false;
            renderer.resetRoot();
            const float travel = kSongTransitionTravelDip * scale();
            if (!renderer.animateRoot(travel, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                      kSongTransitionMs / 1000.0f))
                renderer.resetRoot();
        }

        // 快速展开的固定背景和内容层已在 startQuickExpandLayerTransition() 中完成
        // 两段提交并等待表面进入合成树；这里直接 Present 基准帧，避免再插入一批
        // 背景/内容交接命令。
        if (quickExpandTransition.active() && !quickExpandTransition.hasLayers() &&
            !startQuickExpandLayerTransition(w)) {
            stopQuickExpandTransition();
            return render();
        }
        if (!renderer.present()) {
            releaseDrawingResources();
            return false;
        }
        const auto renderPresented = runtime_log::uiTimeMs();
        if (renderPresented - renderStarted >= 24) {
            runtime_log::writef(
                L"[ui][media-popup-render] bind=%llums resources=%llums backdrop=%llums "
                L"content=%llums draw=%llums present=%llums",
                static_cast<unsigned long long>(renderBound - renderStarted),
                static_cast<unsigned long long>(resourcesReady - renderBound),
                static_cast<unsigned long long>(backdropReady - resourcesReady),
                static_cast<unsigned long long>(contentReady - backdropReady),
                static_cast<unsigned long long>(renderDrawn - contentReady),
                static_cast<unsigned long long>(renderPresented - renderDrawn));
        }
        return true;
    }

    void beginPresentationUpdate() {
        if (presentationUpdateDepth++ == 0)
            presentationUpdatePending = false;
    }

    void endPresentationUpdate() {
        if (presentationUpdateDepth <= 0)
            return;
        if (--presentationUpdateDepth > 0)
            return;

        const bool pending = presentationUpdatePending;
        presentationUpdatePending = false;
        if (!pending || !visibility.visible())
            return;
        if (visibility.animating()) {
            deferredRender = true;
            return;
        }
        render();
    }

    // 层转场收尾（页面互切与快速打开展开/收起共用）：先保留终点层并把最终页面
    // 送入交换链，等待目标帧进入合成器后再撤层，避免撤层与交换链帧交接时闪烁。
    void finishLayerTransition(media_popup_detail::LayerTransition& transition, UINT_PTR timer) {
        if (!transition.active())
            return;
        // 转场期间挂起的进度先取出来：预绘制必须用冻结的旧值，与层内快照
        // 逐像素一致；撤层交接完成后再应用新进度，表现为正常的一帧跳秒。
        const int64_t pendingPosition = deferredPositionMs;
        const bool canPrimeTarget = visibility.visible() && !visibility.animating();
        if (canPrimeTarget) {
            // 先保留终点层，只结束“转场中”状态，让 render() 把最终卡片画到
            // 交换链。转场层仍在合成树中时，Present/Commit 的交接不会露出空白
            // 的图标区域；等待目标帧真正进入合成器后再撤层。
            transition.retainFinal();
            if (render())
                renderer.waitForCommitCompletion();
            else
                renderer.commit();
            stopLayerTransition(transition, timer);
            if (idleMode)
                idleScrollView.setContentVisible(true);
            renderer.commit();
        } else {
            stopLayerTransition(transition, timer);
            renderer.commit();
        }
        if (pendingPosition >= 0 && positionMs != pendingPosition) {
            positionMs = pendingPosition;
            renderOrDefer();
        }
    }

    void reposition() {
        if (!hwnd || !anchor || !IsWindow(anchor))
            return;
        RECT anchorRect{};
        if (!GetWindowRect(anchor, &anchorRect))
            return;
        dpi = GetDpiForWindow(hwnd);
        if (dpi == 0)
            dpi = GetDpiForSystem();
        const float s = scale();
        const int popupW = static_cast<int>(std::lround(kPopupWidthDip * s));
        const int popupH = static_cast<int>(std::lround(popupHeightDip() * s));
        const int gap = static_cast<int>(std::lround(kPopupGapDip * s));

        HMONITOR monitor = MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return;
        const RECT work = info.rcWork;
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);
        // 侧边任务栏的宿主是窄竖栏，卡片应贴在它的外侧并以宿主中心对齐。
        // 这里把 HWND 直接放在卡片最终矩形上，卡片内部坐标保持从 (0, 0)
        // 开始，避免沿用上下弹出时仅支持 Y 方向的 cardOriginDip。
        const int anchorWidth = anchorRect.right - anchorRect.left;
        const int anchorHeight = anchorRect.bottom - anchorRect.top;
        // 直接使用歌词宿主自身的长宽比，而不是依赖 Shell_TrayWnd 的外框。
        // 某些 Explorer 版本会让外框覆盖整块屏幕，但宿主尺寸仍准确反映
        // 当前任务栏是横向还是侧边。
        const bool verticalTaskbar = anchorHeight > anchorWidth;
        sideAnchored = verticalTaskbar;
        if (verticalTaskbar) {
            const int monitorCenterX = (info.rcMonitor.left + info.rcMonitor.right) / 2;
            popupOnRightOfAnchor = (anchorRect.left + anchorRect.right) / 2 < monitorCenterX;

            int x = popupOnRightOfAnchor ? anchorRect.right + gap
                                         : anchorRect.left - gap - popupW;
            const int anchorCenterY = (anchorRect.top + anchorRect.bottom) / 2;
            int y = anchorCenterY - popupH / 2;
            y = std::clamp(y, workTop, std::max(workTop, workBottom - popupH));

            if (popupOnRightOfAnchor && x + popupW > workRight) {
                popupOnRightOfAnchor = false;
                x = anchorRect.left - gap - popupW;
            } else if (!popupOnRightOfAnchor && x < workLeft) {
                popupOnRightOfAnchor = true;
                x = anchorRect.right + gap;
            }
            x = std::clamp(x, workLeft, std::max(workLeft, workRight - popupW));

            cardScreenX = x;
            cardScreenY = y;
            cardWidthPx = popupW;
            cardHeightPx = popupH;
            cardOriginDip = 0.0f;
            // 初始状态让卡片外缘贴住宿主边缘，随后沿 X 轴滑入。
            animationTravelPx = static_cast<float>(popupW + gap);

            RECT windowRect{};
            const bool windowRectUnchanged =
                GetWindowRect(hwnd, &windowRect) && windowRect.left == x &&
                windowRect.top == y && windowRect.right - windowRect.left == popupW &&
                windowRect.bottom - windowRect.top == popupH;
            if (!windowRectUnchanged) {
                SetWindowPos(hwnd, HWND_TOPMOST, x, y, popupW, popupH,
                             SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            return;
        }

        sideAnchored = false;
        const int anchorCenterX = (anchorRect.left + anchorRect.right) / 2;
        int x = anchorCenterX - popupW / 2;
        x = std::clamp(x, workLeft, std::max(workLeft, workRight - popupW));

        const int monitorCenterY = (info.rcMonitor.top + info.rcMonitor.bottom) / 2;
        placedAbove = anchorRect.top >= workBottom ||
                      (anchorRect.bottom > workTop && anchorRect.top < workBottom &&
                       (anchorRect.top + anchorRect.bottom) / 2 >= monitorCenterY);
        int y = placedAbove ? anchorRect.top - gap - popupH : anchorRect.bottom + gap;
        if (placedAbove && y < workTop) {
            placedAbove = false;
            y = anchorRect.bottom + gap;
        } else if (!placedAbove && y + popupH > workBottom) {
            placedAbove = true;
            y = anchorRect.top - gap - popupH;
        }
        y = std::clamp(y, workTop, std::max(workTop, workBottom - popupH));

        // 承载窗口只覆盖“卡片终点到歌词宿主边缘”的可见路径，另一侧保持在
        // 宿主边缘之外。这样卡片会从歌词窗口边缘露出，而不是从屏幕边缘露出。
        const int anchorEdgeY = placedAbove ? anchorRect.top : anchorRect.bottom;
        const int hostY = placedAbove ? y : anchorEdgeY;
        const int cardLocalY = placedAbove ? 0 : y - anchorEdgeY;
        const int travel = placedAbove ? anchorEdgeY - y : cardLocalY + popupH;
        RECT windowRect{};
        const bool windowRectUnchanged =
            GetWindowRect(hwnd, &windowRect) && windowRect.left == x &&
            windowRect.top == hostY && windowRect.right - windowRect.left == popupW &&
            windowRect.bottom - windowRect.top == travel;
        cardScreenX = x;
        cardScreenY = y;
        cardWidthPx = popupW;
        cardHeightPx = popupH;
        animationTravelPx = static_cast<float>(travel);
        cardOriginDip = dip(cardLocalY);
        if (!windowRectUnchanged) {
            SetWindowPos(hwnd, HWND_TOPMOST, x, hostY, popupW, travel,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    int hitButton(float x, float y) const {
        if (categoryTransition.active())
            return -1;
        for (int i = 0; i < 3; ++i) {
            if (buttonEnabled(i, media) && contains(mediaLayout_.buttons[i], x, y))
                return i;
        }
        return -1;
    }

    bool hitVolumeIcon(float x, float y) const {
        return !categoryTransition.active() && currentPage() == PopupPage::Media &&
               volume.available && contains(volumeIconRect, x, y);
    }

    bool hitVolumeSlider(float x, float y) const {
        // 展开过半才允许拖动；命中区域比细轨道上下左右各扩一圈余量
        if (categoryTransition.active() || currentPage() != PopupPage::Media || !volume.available ||
            !volumeSliderOn || volumeSliderT < 0.6f)
            return false;
        return x >= volumeSliderRect.left - 7.0f && x <= volumeSliderRect.right + 7.0f &&
               y >= volumeSliderRect.top - 8.0f && y <= volumeSliderRect.bottom + 8.0f;
    }

    bool hitPageArrow(float x, float y) const {
        return !categoryTransition.active() && playbackScene && idlePageAllowed() &&
               (currentPage() == PopupPage::Media ||
                (currentPage() == PopupPage::Idle && idlePanelManual)) &&
               pageArrowRect.right > pageArrowRect.left && contains(pageArrowRect, x, y);
    }

    bool hitCopy(float x, float y) const {
        return !categoryTransition.active() && currentPage() == PopupPage::Idle &&
               idle.copyEnabled && !idle.loading && !idle.sentence.empty() &&
               copyRect.right > copyRect.left && contains(copyRect, x, y);
    }

    void copyIdleQuote() {
        if (!hwnd || !idle.copyEnabled || idle.loading || idle.sentence.empty())
            return;
        const SIZE_T bytes = (idle.sentence.size() + 1) * sizeof(wchar_t);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!handle)
            return;
        void* memory = GlobalLock(handle);
        if (!memory) {
            GlobalFree(handle);
            return;
        }
        std::memcpy(memory, idle.sentence.c_str(), bytes);
        GlobalUnlock(handle);
        if (!OpenClipboard(hwnd)) {
            GlobalFree(handle);
            return;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, handle)) {
            GlobalFree(handle);
            CloseClipboard();
            return;
        }
        CloseClipboard();
        KillTimer(hwnd, kCopyFeedbackTimer);
        copySucceeded = true;
        SetTimer(hwnd, kCopyFeedbackTimer, kCopyFeedbackMs, nullptr);
        runtime_log::writef(L"[action][media-popup] idle-quote-copied");
    }

    // 页面级交互元素（页面切换箭头、一言复制、快速启动展开和 Tab）的悬浮/按下状态。
    void resetPageInteractionState() {
        hoverPageArrow = false;
        pressedPageArrow = false;
        hoverCopy = false;
        pressedCopy = false;
        copySucceeded = false;
        hoverIdleQuickExpand = false;
        pressedIdleQuickExpand = false;
        hoverIdleTask = -1;
        hoverIdleTaskCheckbox = -1;
        pressedIdleTask = -1;
        pressedIdleTaskCheckbox = -1;
        idleQuickExpandRect = {};
        hoverIdleQuickTab = -1;
        pressedIdleQuickTab = -1;
        idleQuickTabRects[0] = {};
        idleQuickTabRects[1] = {};
    }

    void setPage(PopupPage target, bool userInitiated = false) {
        const PopupPage from = currentPage();
        if (from == target)
            return;

        if (from == PopupPage::Idle) {
            idleScrollOffset = idleScrollView.offset();
            idleScrollView.stop();
        }

        const RECT fromCardRect = popupCardScreenRect();
        const bool hasFromCardRect = visibility.visible() && cardWidthPx > 0 && cardHeightPx > 0;
        if (hwnd)
            KillTimer(hwnd, kHideTimer);
        if (userInitiated && from == PopupPage::Media && target == PopupPage::Idle)
            idlePanelManual = true;
        else if (target == PopupPage::Media)
            idlePanelManual = false;

        categoryHoverEnvelopeActive = false;
        categoryHoverEnvelope = {};
        // 打断可能仍在进行的转场：撤掉旧合成层；若本次也需要转场，接下来的
        // render() 会在同一帧内换上新层，不会闪空帧。
        stopCategoryTransition();
        stopQuickExpandTransition();
        if (visibility.visible() && clientAnimations && !visibility.animating())
            categoryTransition.begin();
        if (categoryTransition.active()) {
            categorySnapshot.from = from;
            categorySnapshot.idle =
                idleContentTransition.active ? idleContentTransition.from : idle;
            categorySnapshot.media = media;
            categorySnapshot.titleScrollOffset = titleScrollOffset;
            categorySnapshot.artistScrollOffset = artistScrollOffset;
            categorySnapshot.idleQuoteScrollOffset = idleQuoteScrollOffset;
            categorySnapshot.idleQuoteSourceScrollOffset = idleQuoteSourceScrollOffset;
            categorySnapshot.idleScrollOffset = idleScrollOffset;
            categorySnapshot.direction = categoryDirection(from, target);
        }
        idleContentTransition.active = false;
        idleScrollOffset = 0.0f;
        idleQuoteScrollOffset = 0.0f;
        idleQuoteSourceScrollOffset = 0.0f;
        scrollTickMs = 0;
        // 类别转场优先于歌曲横向转场；后续完整帧仍会按最新歌曲状态继续更新。
        songTransitionPending = false;
        if (hwnd)
            KillTimer(hwnd, kCopyFeedbackTimer);
        resetPageInteractionState();
        idleMode = target != PopupPage::Media;
        // 两页共用同一套尺寸和绘制资源；页面背景、文字对比色和内容差异由
        // drawCategoryContentLayer()/drawCardBackground() 按 page 参数处理，
        // 不在转场起点拆掉画笔、文字格式和裁剪层，避免动画开始前出现同步卡顿。
        renderer.resetRoot();
        // 两个页面的外层尺寸已经统一；切换时保持 HWND 的位置和大小，
        // 避免 SetWindowPos 在内容转场之外再触发一次窗口级重绘。
        if (categoryTransition.active() && hasFromCardRect) {
            categoryHoverEnvelope = mergeScreenRects(fromCardRect, popupCardScreenRect());
            categoryHoverEnvelopeActive = true;
        }
        if (visibility.visible()) {
            if (visibility.animating())
                deferredRender = true;
            else
                render();
        }
    }

    void togglePage() {
        if (!playbackScene || !idlePageAllowed())
            return;
        if (currentPage() == PopupPage::Media)
            setPage(PopupPage::Idle, true);
        else if (currentPage() == PopupPage::Idle && idlePanelManual)
            setPage(PopupPage::Media);
    }

    bool hitIdleQuickExpand(float x, float y) const {
        return !categoryTransition.active() && idleMode && idle.quickStartEnabled &&
               idle.showText && idleQuickExpandRect.right > idleQuickExpandRect.left &&
               contains(idleQuickExpandRect, x, y);
    }

    int hitIdleQuickTab(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransition.active() || quickExpandTransition.active() || !idleMode ||
            !idle.quickStartEnabled || !idle.todayTasksEnabled ||
            idleQuickExpandProgress(idle) < 0.99f)
            return -1;
        for (int i = 0; i < 2; ++i) {
            if (contains(idleQuickTabRects[i], x, y))
                return i;
        }
        return -1;
    }

    int hitIdleApp(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransition.active() || !idleMode ||
            (idle.todayTasksEnabled && idleQuickTab != IdleQuickTab::QuickStart) ||
            idle.apps.empty() || !contains(idleListRect, x, y))
            return -1;
        const float contentY = y - idleListRect.top + idleScrollOffset;
        const int rowIndex = static_cast<int>(contentY / kIdleQuickRowPitchDip);
        if (rowIndex < 0)
            return -1;
        const float rowOffset = std::fmod(contentY, kIdleQuickRowPitchDip);
        if (rowOffset < 0.0f || rowOffset >= kIdleQuickCellSizeDip)
            return -1;
        const float gridLeft = idleQuickGridLeft();
        const float gridRight = gridLeft + kIdleQuickGridWidthDip;
        if (x < gridLeft || x >= gridRight)
            return -1;
        const float columnPosition = x - gridLeft;
        const int column = static_cast<int>(columnPosition /
                                            (kIdleQuickCellSizeDip +
                                             kIdleQuickCellGapDip));
        if (column < 0 || static_cast<size_t>(column) >= kIdleQuickColumnCount)
            return -1;
        const float cellOffset = std::fmod(columnPosition,
                                           kIdleQuickCellSizeDip +
                                               kIdleQuickCellGapDip);
        if (cellOffset < 0.0f || cellOffset >= kIdleQuickCellSizeDip)
            return -1;
        const int index = rowIndex * static_cast<int>(kIdleQuickColumnCount) + column;
        if (static_cast<size_t>(index) >= idle.apps.size() ||
            !idle.apps[static_cast<size_t>(index)].pathValid)
            return -1;
        return index;
    }

    int hitIdleTask(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransition.active() || !idleMode || !idle.todayTasksEnabled ||
            idleQuickTab != IdleQuickTab::TodayTasks || idle.todayTasks.empty() ||
            !contains(idleListRect, x, y))
            return -1;

        const float contentY = y - idleListRect.top + idleScrollOffset;
        const float rowPitch = kIdleTaskRowHeightDip + kIdleTaskRowGapDip;
        const int rowIndex = static_cast<int>(contentY / rowPitch);
        if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= idle.todayTasks.size())
            return -1;
        const float rowOffset = std::fmod(contentY, rowPitch);
        if (rowOffset < 0.0f || rowOffset >= kIdleTaskRowHeightDip)
            return -1;

        const auto& task = idle.todayTasks[static_cast<size_t>(rowIndex)];
        return task.id.empty() || task.projectId.empty() ? -1 : rowIndex;
    }

    int hitIdleTaskCheckbox(float x, float y) const {
        const int rowIndex = hitIdleTask(x, y);
        if (rowIndex < 0)
            return -1;

        const float localY = y - cardOriginDip;
        const float rowTop = idleListRect.top +
                             static_cast<float>(rowIndex) *
                                 (kIdleTaskRowHeightDip + kIdleTaskRowGapDip) -
                             idleScrollOffset;
        // 可点击区域比视觉复选框稍大，降低精确点击成本，但仍限制在任务行左侧。
        const D2D1_RECT_F checkboxHit =
            D2D1::RectF(idleListRect.left + 6.0f, rowTop + 3.0f,
                        idleListRect.left + 32.0f, rowTop + 25.0f);
        return contains(checkboxHit, x, localY) ? rowIndex : -1;
    }

    bool hitIdleScrollBar(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransition.active() || !idleMode || idleScrollMax <= 0.0f)
            return false;
        const D2D1_RECT_F hit = D2D1::RectF(
            idleScrollTrackRect.left - (kIdleScrollBarHitWidthDip -
                                        kIdleScrollBarWidthDip) * 0.5f,
            idleScrollTrackRect.top - 4.0f,
            idleScrollTrackRect.right + (kIdleScrollBarHitWidthDip -
                                         kIdleScrollBarWidthDip) * 0.5f,
            idleScrollTrackRect.bottom + 4.0f);
        return contains(hit, x, y);
    }

    bool hitIdleScrollThumb(float x, float y) const {
        if (!hitIdleScrollBar(x, y))
            return false;
        return contains(idleScrollThumbRect, x, y);
    }

    void setIdleScrollFromPointer(float y) {
        y -= cardOriginDip;
        if (idleScrollMax <= 0.0f)
            return;
        const float trackHeight = idleScrollTrackRect.bottom - idleScrollTrackRect.top;
        const float thumbHeight = idleScrollThumbRect.bottom - idleScrollThumbRect.top;
        const float usable = std::max(0.0f, trackHeight - thumbHeight);
        if (usable <= 0.0f)
            return;
        const float thumbTop = std::clamp(
            y - idleScrollDragOffset, idleScrollTrackRect.top,
            idleScrollTrackRect.bottom - thumbHeight);
        idleScrollOffset = (thumbTop - idleScrollTrackRect.top) / usable * idleScrollMax;
        idleScrollView.setOffset(idleScrollOffset);
        renderOrDefer();
    }

    void scrollIdleBy(float delta) {
        if (!idleMode || idleScrollMax <= 0.0f)
            return;
        idleScrollView.scrollBy(delta);
    }

    // 拖动/点击滑块：按 x 坐标换算百分比，本地即时反馈并上报
    void applyVolumeAt(float x) {
        const float trackLeft = volumeSliderRect.left;
        const float trackRight = volumeSliderRect.right;
        const float fraction =
            std::clamp((x - trackLeft) / std::max(1.0f, trackRight - trackLeft), 0.0f, 1.0f);
        const int next = static_cast<int>(std::lround(fraction * 100.0f));
        if (next == volume.percent && !volume.muted)
            return;
        volume.percent = next;
        volume.muted = false; // 调整音量即解除静音（控制器侧同样处理）
        const auto request = AppVolumeRequest::create(next);
        volume.requestRevision = request.revision;
        renderOrDefer();
        if (onAppVolume)
            onAppVolume(request);
    }

    // 点击音量图标切换滑块；动画关闭时直接到位
    void toggleVolumeSlider() {
        volumeSliderOn = !volumeSliderOn;
        if (!clientAnimations) {
            volumeSliderT = volumeSliderOn ? 1.0f : 0.0f;
            renderOrDefer();
            return;
        }
        volumeSliderOpening = volumeSliderOn;
        SetTimer(hwnd, kVolumeTimer, 16, nullptr);
    }

    void toggleIdleQuickExpand() {
        if (!hwnd || !idleMode || !idle.quickStartEnabled || !idle.showText)
            return;
        // 再次点击时先完成上一目标的图层交接，再从该状态反向展开/收起。
        if (quickExpandTransition.active())
            finishLayerTransition(quickExpandTransition, kIdleQuickExpandTimer);
        idleScrollOffset = idleScrollView.offset();
        idleScrollView.stop();
        quickExpansion.expanded = !quickExpansion.expanded;
        quickExpansion.progress = quickExpansion.expanded ? 1.0f : 0.0f;
        quickExpansion.opening = false;
        if (!clientAnimations || !visibility.visible() || visibility.animating()) {
            KillTimer(hwnd, kIdleQuickExpandTimer);
            renderOrDefer();
            return;
        }
        // 展开/收起由 DComp 合成层执行（render() 里装层）；定时器在合成器
        // 动画到达终点并静止后收尾：先预绘制最终状态，再撤层完成交接。
        quickExpandTransition.begin();
        SetTimer(hwnd, kIdleQuickExpandTimer,
                 static_cast<UINT>(kIdleQuickExpandMs) + 32, nullptr);
        render();
    }

    void advanceVolumeSlider() {
        const float step = 16.0f / kVolumeSliderMs;
        volumeSliderT += volumeSliderOpening ? step : -step;
        if (volumeSliderT >= 1.0f) {
            volumeSliderT = 1.0f;
            KillTimer(hwnd, kVolumeTimer);
        } else if (volumeSliderT <= 0.0f) {
            volumeSliderT = 0.0f;
            KillTimer(hwnd, kVolumeTimer);
        }
        renderOrDefer();
    }

    bool hitSource(float x, float y) const {
        if (!available || categoryTransition.active() || currentPage() != PopupPage::Media ||
            media.sourceAppUserModelId.empty())
            return false;
        // 与 drawSource 的顶部来源行保持同一块可点击区域，给图标和名称都留出
        // 一点点击余量，但不侵入下面的封面和歌曲标题区域。
        return contains(mediaLayout_.sourceHit, x, y);
    }

    void trackMouseLeave() {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    bool cursorInsideScreenRect(const RECT& rect) const {
        if (rect.right <= rect.left || rect.bottom <= rect.top)
            return false;
        POINT point{};
        if (!GetCursorPos(&point))
            return false;
        return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
               point.y < rect.bottom;
    }

    bool cursorInsidePopupCard() const {
        return cursorInsideScreenRect(popupCardScreenRect());
    }

    void refreshPopupHoverAfterTransition() {
        if (!hwnd || !visibility.visible())
            return;

        if (cursorInsidePopupCard()) {
            popupHover = true;
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
            KillTimer(hwnd, kHideTimer);
            trackMouseLeave();
        } else if (categoryHoverEnvelopeActive &&
                   cursorInsideScreenRect(categoryHoverEnvelope)) {
            // 反向切换时，鼠标可能仍停在较高的旧组合卡片区域；该区域已经
            // 不属于缩短后的 HWND，但仍视为本次切换的连续悬浮范围。
            popupHover = false;
            KillTimer(hwnd, kHideTimer);
            SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
        } else {
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
            popupHover = false;
            if (!anchorHover)
                SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
        }
    }

    void scheduleHide() {
        if (!hwnd || !visibility.visible() || anchorHover || popupHover ||
            volumeDragging || categoryTransition.active() || quickExpandTransition.active())
            return;
        KillTimer(hwnd, kShowTimer);
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void renderOrDefer() {
        // DComp 正在独立驱动内容层时，交换链重绘既不能更新层内快照，
        // 又会制造额外 Present；最终收尾帧会统一提交最新状态。
        if (categoryTransition.active() || quickExpandTransition.active())
            return;
        if (visibility.animating()) {
            deferredRender = true;
            return;
        }
        render();
    }

    void showPopup() {
        if (!hwnd || !enabled || !available)
            return;
        const auto showStarted = runtime_log::uiTimeMs();
        const bool wasVisible = visibility.visible();
        KillTimer(hwnd, kShowTimer);
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kCloseTimer);
        KillTimer(hwnd, kScrollTimer);
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kVolumeTimer);
        KillTimer(hwnd, kIdleQuickExpandTimer);
        scrollTimerRunning = false;
        deferredRender = false;
        visibility.beginOpen();
        reposition();
        const auto positioned = runtime_log::uiTimeMs();
        const bool canUseFrostedBackdropOnEitherPage =
            frostedBackgroundActiveForPage(PopupPage::Media) ||
            frostedBackgroundActiveForPage(PopupPage::Idle);
        if (canUseFrostedBackdropOnEitherPage)
            backdropDirty = true;
        if (!render()) {
            hideImmediate();
            return;
        }
        const auto rendered = runtime_log::uiTimeMs();

        // 窗口本身保持固定位置，只让 DirectComposition 根视觉做位移动画。
        // 透明度始终为 1，避免淡入淡出时出现第二层背板。
        renderer.resetRoot();
        const float fromX = sideAnchored
                                ? (popupOnRightOfAnchor ? -animationTravelPx
                                                         : animationTravelPx)
                                : 0.0f;
        const float fromY = sideAnchored ? 0.0f
                                         : (placedAbove ? animationTravelPx
                                                        : -animationTravelPx);
        if (!renderer.animateRoot(fromX, 0.0f, fromY, 0.0f,
                                  1.0f, 1.0f,
                                  static_cast<float>(kOpenAnimationMs) / 1000.0f)) {
            renderer.resetRoot();
        }
        renderer.commit();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        visibility.markVisible();
        runtime_log::writef(L"[action][media-popup] shown trigger=%s",
                            opensOnHover() ? L"hover" : L"click");
        if (!wasVisible && onPanelOpened)
            onPanelOpened();
        SetTimer(hwnd, kEnterTimer, kOpenAnimationMs, nullptr);
        const auto shown = runtime_log::uiTimeMs();
        if (shown - showStarted >= 24) {
            runtime_log::writef(
                L"[ui][media-popup-show] prepare=%llums render=%llums reveal=%llums",
                static_cast<unsigned long long>(positioned - showStarted),
                static_cast<unsigned long long>(rendered - positioned),
                static_cast<unsigned long long>(shown - rendered));
        }
    }

    void hideAnimated() {
        if (!visibility.visible() || visibility.closing() || categoryTransition.active() ||
            quickExpandTransition.active())
            return;
        idleScrollOffset = idleScrollView.offset();
        idleScrollView.stop();
        idleScrollView.setVisible(false);
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kScrollTimer);
        KillTimer(hwnd, kVolumeTimer);
        KillTimer(hwnd, kIdleQuickExpandTimer);
        scrollTimerRunning = false;
        deferredRender = false;
        visibility.beginClose();
        if (!render()) {
            hideImmediate();
            return;
        }
        runtime_log::writef(L"[action][media-popup] hide");
        const float toX = sideAnchored
                              ? (popupOnRightOfAnchor ? -animationTravelPx
                                                       : animationTravelPx)
                              : 0.0f;
        const float toY = sideAnchored ? 0.0f
                                       : (placedAbove ? animationTravelPx
                                                      : -animationTravelPx);
        if (!renderer.animateRoot(0.0f, toX, 0.0f, toY,
                                  1.0f, 1.0f,
                                  static_cast<float>(kCloseAnimationMs) / 1000.0f)) {
            hideImmediate();
            return;
        }
        renderer.commit();
        SetTimer(hwnd, kCloseTimer, kCloseAnimationMs, nullptr);
    }

    void onPopupEnter() {
        popupHover = true;
        if (!categoryTransition.active()) {
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
        }
        KillTimer(hwnd, kHideTimer);
        trackMouseLeave();
        if (visibility.closing())
            showPopup();
    }

    void onPopupLeave() {
        popupHover = false;
        scheduleHide();
    }

    void onAnchorEnter() {
        anchorHover = true;
        if (!hwnd)
            return;
        KillTimer(hwnd, kHideTimer);
        if (enabled && available && !visibility.visible() && opensOnHover())
            SetTimer(hwnd, kShowTimer, kShowDelayMs, nullptr);
    }

    void onAnchorClick() {
        anchorHover = true;
        if (!hwnd || !enabled || !available || visibility.visible() || opensOnHover())
            return;
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kShowTimer);
        showPopup();
    }

    void onAnchorLeave() {
        anchorHover = false;
        KillTimer(hwnd, kShowTimer);
        scheduleHide();
    }

    void hideImmediate() {
        idleScrollOffset = idleScrollView.offset();
        idleScrollView.stop();
        idleScrollView.setVisible(false);
        if (playbackScene && idleMode && !inlineControls)
            idleMode = false;
        idlePanelManual = false;
        visibility.hide();
        stopCategoryTransition();
        stopQuickExpandTransition();
        categoryHoverEnvelopeActive = false;
        categoryHoverEnvelope = {};
        idleContentTransition = {};
        categorySnapshot = {};
        songTransitionPending = false;
        volumeSliderOn = false;
        volumeSliderT = 0.0f;
        volumeDragging = false;
        hoverVolume = false;
        pressedVolume = false;
        hoverIdleApp = -1;
        pressedIdleApp = -1;
        hoverIdleTask = -1;
        hoverIdleTaskCheckbox = -1;
        pressedIdleTask = -1;
        idleScrollDragging = false;
        resetPageInteractionState();
        quickExpansion.expanded = false;
        quickExpansion.opening = false;
        quickExpansion.progress = 0.0f;
        if (!hwnd)
            return;
        killTimers();
        ShowWindow(hwnd, SW_HIDE);
        renderer.resetRoot();
        renderer.commit();
        // 关闭动画已经结束；先隐藏窗口并撤除转场，再回收卡片专属资源。
        releaseAll();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT point{};
                if (GetCursorPos(&point) && ScreenToClient(hwnd, &point) &&
                    (hitSource(dip(point.x), dip(point.y)) ||
                     hitVolumeIcon(dip(point.x), dip(point.y)) ||
                     hitVolumeSlider(dip(point.x), dip(point.y)) ||
                     hitPageArrow(dip(point.x), dip(point.y)) ||
                     hitCopy(dip(point.x), dip(point.y)) ||
                     hitIdleQuickExpand(dip(point.x), dip(point.y)) ||
                     hitIdleQuickTab(dip(point.x), dip(point.y)) >= 0 ||
                     hitIdleApp(dip(point.x), dip(point.y)) >= 0 ||
                     hitIdleTask(dip(point.x), dip(point.y)) >= 0)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_MOUSEMOVE: {
            onPopupEnter();
            const float mx = dip(GET_X_LPARAM(lp));
            const float my = dip(GET_Y_LPARAM(lp));
            if (idleMode) {
                if (idleScrollDragging) {
                    setIdleScrollFromPointer(my);
                    return 0;
                }
                const int idleApp = hitIdleApp(mx, my);
                const int idleTask = hitIdleTask(mx, my);
                const int idleTaskCheckbox = hitIdleTaskCheckbox(mx, my);
                const bool arrow = hitPageArrow(mx, my);
                const bool copy = hitCopy(mx, my);
                const bool quickExpand = hitIdleQuickExpand(mx, my);
                const int quickTab = hitIdleQuickTab(mx, my);
                if (idleApp != hoverIdleApp || idleTask != hoverIdleTask ||
                    idleTaskCheckbox != hoverIdleTaskCheckbox ||
                    arrow != hoverPageArrow || copy != hoverCopy ||
                    quickExpand != hoverIdleQuickExpand || quickTab != hoverIdleQuickTab ||
                    hoverVolume) {
                    hoverIdleApp = idleApp;
                    hoverIdleTask = idleTask;
                    hoverIdleTaskCheckbox = idleTaskCheckbox;
                    hoverPageArrow = arrow;
                    hoverCopy = copy;
                    hoverIdleQuickExpand = quickExpand;
                    hoverIdleQuickTab = quickTab;
                    hoverVolume = false;
                    renderOrDefer();
                }
                return 0;
            }
            if (volumeDragging) {
                applyVolumeAt(mx);
                return 0;
            }
            const int button = hitButton(mx, my);
            const bool volHover = hitVolumeIcon(mx, my) || hitVolumeSlider(mx, my);
            const bool arrow = hitPageArrow(mx, my);
            if (button != hoverButton || volHover != hoverVolume || arrow != hoverPageArrow) {
                hoverButton = button;
                hoverVolume = volHover;
                hoverPageArrow = arrow;
                renderOrDefer();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverButton = -1;
            hoverVolume = false;
            hoverIdleApp = -1;
            hoverIdleTask = -1;
            hoverIdleTaskCheckbox = -1;
            hoverPageArrow = false;
            hoverCopy = false;
            hoverIdleQuickExpand = false;
            hoverIdleQuickTab = -1;
            onPopupLeave();
            renderOrDefer();
            return 0;
        case WM_LBUTTONDOWN: {
            idleScrollOffset = idleScrollView.offset();
            idleScrollView.stop();
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (idleMode) {
                if (hitPageArrow(x, y)) {
                    pressedPageArrow = true;
                    SetCapture(hwnd);
                    renderOrDefer();
                    return 0;
                }
                if (hitCopy(x, y)) {
                    pressedCopy = true;
                    SetCapture(hwnd);
                    renderOrDefer();
                    return 0;
                }
                if (hitIdleQuickExpand(x, y)) {
                    pressedIdleQuickExpand = true;
                    SetCapture(hwnd);
                    renderOrDefer();
                    return 0;
                }
                const int quickTab = hitIdleQuickTab(x, y);
                if (quickTab >= 0) {
                    pressedIdleQuickTab = quickTab;
                    SetCapture(hwnd);
                    renderOrDefer();
                    return 0;
                }
                if (hitIdleScrollThumb(x, y)) {
                    idleScrollDragging = true;
                    idleScrollDragOffset = y - cardOriginDip -
                                           idleScrollThumbRect.top;
                    SetCapture(hwnd);
                    return 0;
                }
                if (hitIdleScrollBar(x, y)) {
                    const float localY = y - cardOriginDip;
                    const float page = std::max(1.0f, idleListRect.bottom - idleListRect.top);
                    scrollIdleBy(localY < idleScrollThumbRect.top ? -page : page);
                    return 0;
                }
                pressedIdleApp = hitIdleApp(x, y);
                pressedIdleTask = hitIdleTask(x, y);
                pressedIdleTaskCheckbox = hitIdleTaskCheckbox(x, y);
                if (pressedIdleApp >= 0 || pressedIdleTask >= 0) {
                    SetCapture(hwnd);
                    renderOrDefer();
                }
                return 0;
            }
            if (hitPageArrow(x, y)) {
                pressedPageArrow = true;
                SetCapture(hwnd);
                renderOrDefer();
                return 0;
            }
            // 音量控件优先：图标按下待点击切换滑块；滑块按下直接进入拖动
            if (hitVolumeSlider(x, y)) {
                volumeDragging = true;
                SetCapture(hwnd);
                applyVolumeAt(x);
                return 0;
            }
            if (hitVolumeIcon(x, y)) {
                pressedVolume = true;
                SetCapture(hwnd);
                renderOrDefer();
                return 0;
            }
            pressedSource = hitSource(x, y);
            pressedSourceAppUserModelId = pressedSource ? media.sourceAppUserModelId : L"";
            const int button = pressedSource ? -1 : hitButton(x, y);
            if (pressedSource || button >= 0) {
                pressedButton = button;
                SetCapture(hwnd);
                renderOrDefer();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (idleMode) {
                const bool arrow = pressedPageArrow;
                const bool arrowHit = hitPageArrow(x, y);
                pressedPageArrow = false;
                const bool copy = pressedCopy;
                const bool copyHit = hitCopy(x, y);
                pressedCopy = false;
                const bool quickExpand = pressedIdleQuickExpand;
                const bool quickExpandHit = hitIdleQuickExpand(x, y);
                pressedIdleQuickExpand = false;
                if (arrow && arrowHit) {
                    if (GetCapture() == hwnd)
                        ReleaseCapture();
                    togglePage();
                    return 0;
                }
                if (copy && copyHit) {
                    if (GetCapture() == hwnd)
                        ReleaseCapture();
                    copyIdleQuote();
                    renderOrDefer();
                    return 0;
                }
                if (quickExpand && quickExpandHit) {
                    if (GetCapture() == hwnd)
                        ReleaseCapture();
                    toggleIdleQuickExpand();
                    return 0;
                }
                const int pressedTab = pressedIdleQuickTab;
                const int tabHit = hitIdleQuickTab(x, y);
                pressedIdleQuickTab = -1;
                if (pressedTab >= 0 && pressedTab == tabHit) {
                    if (GetCapture() == hwnd)
                        ReleaseCapture();
                    idleQuickTab = pressedTab == 0 ? IdleQuickTab::QuickStart
                                                   : IdleQuickTab::TodayTasks;
                    idleScrollOffset = 0.0f;
                    hoverIdleApp = -1;
                    hoverIdleTask = -1;
                    hoverIdleTaskCheckbox = -1;
                    renderOrDefer();
                    return 0;
                }
                if (idleScrollDragging) {
                    idleScrollDragging = false;
                    if (GetCapture() == hwnd)
                        ReleaseCapture();
                    return 0;
                }
                const int pressed = pressedIdleApp;
                const int hit = hitIdleApp(x, y);
                pressedIdleApp = -1;
                const int pressedTask = pressedIdleTask;
                const int hitTask = hitIdleTask(x, y);
                pressedIdleTask = -1;
                const int pressedTaskCheckbox = pressedIdleTaskCheckbox;
                const int hitTaskCheckbox = hitIdleTaskCheckbox(x, y);
                pressedIdleTaskCheckbox = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                renderOrDefer();
                if (pressed >= 0 && pressed == hit &&
                    static_cast<size_t>(pressed) < idle.apps.size() &&
                    onIdleAppOpen) {
                    const std::wstring path = idle.apps[static_cast<size_t>(pressed)].path;
                    hideImmediate();
                    anchorHover = false;
                    onIdleAppOpen(path);
                } else if (idle.todayTasksEnabled && pressedTaskCheckbox >= 0 &&
                           pressedTaskCheckbox == hitTaskCheckbox &&
                           static_cast<size_t>(pressedTaskCheckbox) < idle.todayTasks.size() &&
                           onIdleTaskComplete) {
                    const IdleTaskInfo task =
                        idle.todayTasks[static_cast<size_t>(pressedTaskCheckbox)];
                    onIdleTaskComplete(task);
                } else if (pressedTaskCheckbox < 0 && idle.todayTasksEnabled &&
                           pressedTask >= 0 &&
                           pressedTask == hitTask &&
                           static_cast<size_t>(pressedTask) < idle.todayTasks.size() &&
                           onIdleTaskOpen) {
                    const IdleTaskInfo task = idle.todayTasks[static_cast<size_t>(pressedTask)];
                    hideImmediate();
                    anchorHover = false;
                    onIdleTaskOpen(task);
                }
                return 0;
            }
            if (pressedPageArrow) {
                const bool hit = hitPageArrow(x, y);
                pressedPageArrow = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                if (hit)
                    togglePage();
                renderOrDefer();
                return 0;
            }
            if (volumeDragging) {
                applyVolumeAt(x);
                volumeDragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                popupHover = cursorInsidePopupCard();
                scheduleHide();
                return 0;
            }
            if (pressedVolume) {
                pressedVolume = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                if (hitVolumeIcon(x, y))
                    toggleVolumeSlider();
                renderOrDefer();
                return 0;
            }
            const bool source = hitSource(x, y);
            const int button = source ? -1 : hitButton(x, y);
            const int pressed = pressedButton;
            const bool sourcePressed = pressedSource;
            std::wstring sourceId = std::move(pressedSourceAppUserModelId);
            pressedButton = -1;
            pressedSource = false;
            pressedSourceAppUserModelId.clear();
            if (GetCapture() == hwnd)
                ReleaseCapture();
            renderOrDefer();
            if (sourcePressed && source && !sourceId.empty() &&
                sourceId == media.sourceAppUserModelId && onSourceOpen) {
                hideImmediate();
                onSourceOpen(sourceId);
            } else if (pressed >= 0 && pressed == button && onControl) {
                onControl(static_cast<MediaControl>(pressed));
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            // 音量图标/展开的滑块上滚动：直接调整应用音量（每格 ±2）
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float x = dip(point.x);
            const float y = dip(point.y);
            if (idleMode) {
                const float localY = y - cardOriginDip;
                if (contains(idleListRect, x, localY) || hitIdleScrollBar(x, y)) {
                    scrollIdleBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
                                 WHEEL_DELTA * 32.0f);
                }
                return 0;
            }
            if (onAppVolume && (hitVolumeIcon(x, y) || hitVolumeSlider(x, y))) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0) {
                    volume.percent = std::clamp(volume.percent + steps * 2, 0, 100);
                    volume.muted = false;
                    const auto request = AppVolumeRequest::create(volume.percent);
                    volume.requestRevision = request.revision;
                    renderOrDefer();
                    onAppVolume(request);
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (volumeDragging) {
                volumeDragging = false;
                popupHover = cursorInsidePopupCard();
                scheduleHide();
            }
            pressedButton = -1;
            pressedSource = false;
            pressedSourceAppUserModelId.clear();
            pressedVolume = false;
            pressedIdleApp = -1;
            pressedIdleTask = -1;
            pressedIdleTaskCheckbox = -1;
            idleScrollDragging = false;
            pressedPageArrow = false;
            pressedCopy = false;
            pressedIdleQuickExpand = false;
            pressedIdleQuickTab = -1;
            renderOrDefer();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
        {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float x = dip(point.x);
            const float y = dip(point.y);
            if (x < 0.0f || x > kPopupWidthDip || y < cardOriginDip ||
                y > cardOriginDip + popupHeightDip())
                return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_TIMER:
            if (idleScrollView.tick(wp))
                return 0;
            if (wp == kShowTimer) {
                KillTimer(hwnd, kShowTimer);
                if (anchorHover && opensOnHover())
                    showPopup();
            } else if (wp == kHideTimer) {
                KillTimer(hwnd, kHideTimer);
                if (volumeDragging || categoryTransition.active() || quickExpandTransition.active())
                    return 0;
                if (categoryHoverEnvelopeActive) {
                    if (cursorInsidePopupCard()) {
                        popupHover = true;
                        categoryHoverEnvelopeActive = false;
                        categoryHoverEnvelope = {};
                        trackMouseLeave();
                        return 0;
                    }
                    if (cursorInsideScreenRect(categoryHoverEnvelope)) {
                        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
                        return 0;
                    }
                    categoryHoverEnvelopeActive = false;
                    categoryHoverEnvelope = {};
                    popupHover = false;
                }
                if (!anchorHover && !popupHover)
                    hideAnimated();
            } else if (wp == kCloseTimer) {
                KillTimer(hwnd, kCloseTimer);
                if (visibility.closing() && !anchorHover && !popupHover)
                    hideImmediate();
                else if (visibility.closing())
                    showPopup();
            } else if (wp == kEnterTimer) {
                KillTimer(hwnd, kEnterTimer);
                visibility.finishOpen();
                if (visibility.visible() && !visibility.closing()) {
                    if (deferredRender || idleMode) {
                        deferredRender = false;
                        render();
                    }
                    updateScrollTimer(textAreaWidth(kPopupWidthDip));
                }
            } else if (wp == kScrollTimer) {
                if (visibility.animating() || !visibility.visible())
                    return 0;
                // 层转场期间内容承载在静态合成层快照上，滚动偏移必须保持冻结；
                // 否则收尾帧用推进后的偏移重绘，交接瞬间文字横向跳变。
                // 冻结只停偏移、计时基准照常推进，避免收尾后第一帧把转场
                // 时长一次性补进偏移造成二次跳动。
                if (categoryTransition.active() || quickExpandTransition.active()) {
                    scrollTickMs = GetTickCount64();
                    return 0;
                }
                advanceTextScroll();
                renderOrDefer();
            } else if (wp == kVolumeTimer) {
                if (visibility.animating() || !visibility.visible()) {
                    KillTimer(hwnd, kVolumeTimer);
                    return 0;
                }
                // 与文本滚动同理：层转场期间冻结滑块展开进度，保持与层内快照一致。
                if (categoryTransition.active() || quickExpandTransition.active())
                    return 0;
                advanceVolumeSlider();
            } else if (wp == kCopyFeedbackTimer) {
                KillTimer(hwnd, kCopyFeedbackTimer);
                copySucceeded = false;
                renderOrDefer();
            } else if (wp == kIdleQuickExpandTimer || wp == kCategoryTimer) {
                // 合成器动画已到终点并静止：先隐藏着预绘制最终页面，再撤层完成交接。
                KillTimer(hwnd, wp);
                const bool quickExpand = wp == kIdleQuickExpandTimer;
                auto& transition = quickExpand ? quickExpandTransition : categoryTransition;
                if (transition.active()) {
                    finishLayerTransition(transition, wp);
                    if (visibility.visible() && !visibility.animating()) {
                        refreshPopupHoverAfterTransition();
                    }
                }
            }
            return 0;
        case WM_DPICHANGED:
            dpi = GetDpiForWindow(hwnd);
            releaseDrawingResources();
            reposition();
            if (visibility.visible())
                render();
            return 0;
        case WM_DISPLAYCHANGE:
            reposition();
            return 0;
        case WM_SETTINGCHANGE:
            refreshClientAnimations();
            releaseDrawingResources();
            if (visibility.visible())
                render();
            return 0;
        case WM_THEMECHANGED:
            releaseDrawingResources();
            if (visibility.visible())
                render();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            renderOrDefer();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            hideImmediate();
            return 0;
        case WM_DESTROY:
            idleScrollView.reset();
            killTimers();
            releaseDrawingResources();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

MediaPopup::MediaPopup() : thread_(std::make_unique<UiThread>()) {
    thread_->invoke([this] {
        impl_ = std::make_unique<Impl>();
        runtime_log::writef(L"[ui][thread] role=media-popup thread=%lu", GetCurrentThreadId());
    });
}

MediaPopup::~MediaPopup() {
    destroy();
    thread_->invoke([this] { impl_.reset(); });
}

void MediaPopup::enqueue(std::function<void()> action) {
    if (updateDepth_ != 0) {
        updateActions_.push_back(std::move(action));
    } else if (!thread_->post(std::move(action))) {
        throw std::runtime_error("Media popup thread is closed");
    }
}

bool MediaPopup::create(HINSTANCE inst, HWND anchor) {
    return thread_->invoke([=, this]() mutable {
        if (impl_->hwnd) {
            impl_->anchor = anchor;
            impl_->reposition();
            return true;
        }
        impl_->inst = inst;
        impl_->anchor = anchor;
        impl_->refreshClientAnimations();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWndClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
        impl_->hwnd = CreateWindowExW(exStyle, kWndClassName, L"QQMusicLyricMediaPopup",
                                      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, impl_.get());
        if (!impl_->hwnd)
            return false;
        if (!impl_->idleScrollView.create(impl_->hwnd, kIdleSmoothScrollTimer,
                                          [impl = impl_.get()](float position, bool) {
            impl->idleScrollOffset = position;
            impl->renderOrDefer();
        }, &impl_->renderer)) {
            DestroyWindow(impl_->hwnd);
            return false;
        }
        impl_->dpi = GetDpiForWindow(impl_->hwnd);
        if (impl_->dpi == 0)
            impl_->dpi = GetDpiForSystem();
        impl_->reposition();
        return true;
    });
}

void MediaPopup::destroy() {
    thread_->invoke([=, this]() mutable {
        if (!impl_)
            return;
        impl_->hideImmediate();
        if (impl_->hwnd)
            DestroyWindow(impl_->hwnd);
        impl_->releaseAll();
    });
}

void MediaPopup::setControlCallback(std::function<void(MediaControl)> cb) {
    enqueue([=, this]() mutable {
        impl_->onControl = std::move(cb);
    });
}

void MediaPopup::setAppVolume(const AppVolumeState& state) {
    enqueue([=, this]() mutable {
        if (state.available && state.requestRevision < impl_->volume.requestRevision)
            return;
        const bool changed = state.available != impl_->volume.available ||
                             state.percent != impl_->volume.percent ||
                             state.muted != impl_->volume.muted;
        impl_->volume = state;
        if (!state.available && impl_->volumeSliderOn) {
            // 会话消失：直接收起滑块，不走过渡动画
            impl_->volumeSliderOn = false;
            impl_->volumeSliderT = 0.0f;
            const bool wasVolumeDragging = impl_->volumeDragging;
            impl_->volumeDragging = false;
            if (wasVolumeDragging && impl_->hwnd && GetCapture() == impl_->hwnd)
                ReleaseCapture();
            if (impl_->hwnd)
                KillTimer(impl_->hwnd, kVolumeTimer);
        }
        if (changed && impl_->visibility.visible())
            impl_->renderOrDefer();
    });
}

void MediaPopup::setAppVolumeCallback(std::function<void(AppVolumeRequest)> cb) {
    enqueue([=, this]() mutable {
        impl_->onAppVolume = std::move(cb);
    });
}

void MediaPopup::setSourceOpenCallback(std::function<void(const std::wstring&)> cb) {
    enqueue([=, this]() mutable {
        impl_->onSourceOpen = std::move(cb);
    });
}

void MediaPopup::setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb) {
    enqueue([=, this]() mutable {
        impl_->onIdleAppOpen = std::move(cb);
    });
}

void MediaPopup::setIdleTaskOpenCallback(std::function<void(const IdleTaskInfo&)> cb) {
    enqueue([=, this]() mutable {
        impl_->onIdleTaskOpen = std::move(cb);
    });
}

void MediaPopup::setIdleTaskCompleteCallback(
    std::function<void(const IdleTaskInfo&)> cb) {
    enqueue([=, this]() mutable {
        impl_->onIdleTaskComplete = std::move(cb);
    });
}

void MediaPopup::setPanelOpenedCallback(std::function<void()> cb) {
    enqueue([=, this]() mutable {
        impl_->onPanelOpened = std::move(cb);
    });
}

void MediaPopup::setEnabled(bool enabled) {
    enqueue([=, this]() mutable {
        impl_->enabled = enabled;
        if (!enabled) {
            impl_->hideImmediate();
            return;
        }
        if (!impl_->hwnd)
            return;
        if (impl_->opensOnHover() && impl_->anchorHover && impl_->available &&
            !impl_->visibility.visible())
            SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
    });
}

void MediaPopup::setTriggerOnHover(bool on) {
    enqueue([=, this]() mutable {
        if (impl_->triggerOnHover == on)
            return;
        impl_->triggerOnHover = on;
        if (!impl_->hwnd)
            return;
        if (!impl_->opensOnHover()) {
            KillTimer(impl_->hwnd, kShowTimer);
        } else if (impl_->enabled && impl_->anchorHover && impl_->available &&
                   !impl_->visibility.visible()) {
            SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
        }
    });
}

void MediaPopup::setBackgroundMode(MediaPopupBackground mode) {
    enqueue([=, this]() mutable {
        if (impl_->backgroundMode == mode)
            return;
        impl_->backgroundMode = mode;
        if (mode != MediaPopupBackground::Frosted)
            impl_->releaseDynamicBackgroundResources();
        if (!impl_->hwnd)
            return;
        impl_->releaseDrawingResources();
        if (!impl_->visibility.visible())
            return;
        if (impl_->visibility.animating())
            impl_->deferredRender = true;
        else
            impl_->render();
    });
}

void MediaPopup::setBackgroundColor(COLORREF color, bool customized) {
    enqueue([=, this]() mutable {
        if (impl_->floatingCardBackgroundColor == color &&
            impl_->floatingCardBackgroundColorCustomized == customized)
            return;
        impl_->floatingCardBackgroundColor = color;
        impl_->floatingCardBackgroundColorCustomized = customized;
        if (!impl_->hwnd)
            return;
        impl_->releaseDrawingResources();
        if (!impl_->visibility.visible())
            return;
        if (impl_->visibility.animating())
            impl_->deferredRender = true;
        else
            impl_->render();
    });
}

void MediaPopup::setFollowAlbumBackground(bool on) {
    enqueue([=, this]() mutable {
        if (impl_->followAlbumBackground == on)
            return;
        impl_->followAlbumBackground = on;
        if (!impl_->dynamicBackgroundEnabled())
            impl_->releaseDynamicBackgroundResources();
        else
            impl_->dynamicBackgroundDirty = true;
        if (!impl_->hwnd || !impl_->visibility.visible())
            return;
        if (impl_->visibility.animating())
            impl_->deferredRender = true;
        else
            impl_->render();
    });
}

void MediaPopup::setAutoTextContrast(bool on) {
    enqueue([=, this]() mutable {
        if (impl_->autoTextContrast == on)
            return;
        impl_->autoTextContrast = on;
        if (on)
            impl_->backdropDirty = true;
        else
            impl_->resetBackdropTextColors();
        if (!impl_->hwnd || !impl_->visibility.visible())
            return;
        if (impl_->visibility.animating())
            impl_->deferredRender = true;
        else
            impl_->render();
    });
}

void MediaPopup::refreshTheme() {
    enqueue([=, this]() mutable {
        impl_->releaseDrawingResources();
        if (!impl_->hwnd || !impl_->visibility.visible())
            return;
        if (impl_->visibility.animating())
            impl_->deferredRender = true;
        else
            impl_->render();
    });
}

void MediaPopup::setMedia(const OverlayMediaInfo& info, bool available,
                          bool animateSongTransition) {
    enqueue([=, this]() mutable {
        const bool coverChanged = info.thumbnail != impl_->media.thumbnail;
        const bool sourceChanged = info.sourceAppUserModelId != impl_->media.sourceAppUserModelId;
        const bool textChanged = info.title != impl_->media.title || info.artist != impl_->media.artist;
        const bool dominantColorChanged =
            info.hasDominantColor != impl_->media.hasDominantColor ||
            (info.hasDominantColor && info.dominantColor != impl_->media.dominantColor);
        const bool changed = coverChanged || info.title != impl_->media.title ||
                             info.artist != impl_->media.artist ||
                             sourceChanged ||
                             dominantColorChanged ||
                             info.durationMs != impl_->media.durationMs ||
                             info.canPrev != impl_->media.canPrev ||
                             info.canPlayPause != impl_->media.canPlayPause ||
                             info.canNext != impl_->media.canNext ||
                             info.playing != impl_->media.playing;
        impl_->songTransitionPending = false;
        impl_->media = info;
        impl_->available = available;
        impl_->syncPetMode(textChanged);
        if (coverChanged)
            impl_->coverDirty = true;
        if (sourceChanged)
            impl_->sourceIconDirty = true;
        if (dominantColorChanged)
            impl_->dynamicBackgroundDirty = true;
        if (textChanged) {
            impl_->textDirty = true;
            impl_->titleScrollOffset = 0.0f;
            impl_->artistScrollOffset = 0.0f;
            impl_->scrollTickMs = 0;
        }
        if (!available) {
            impl_->hideImmediate();
            return;
        }
        impl_->songTransitionPending = animateSongTransition && changed &&
                                       impl_->visibility.visible() && !impl_->visibility.animating() &&
                                       impl_->clientAnimations && !impl_->idleMode &&
                                       !impl_->categoryTransition.active();
        if (changed && impl_->visibility.visible() && !impl_->categoryTransition.active() &&
            !impl_->quickExpandTransition.active() && (!impl_->idleMode || dominantColorChanged)) {
            if (impl_->visibility.animating())
                impl_->deferredRender = true;
            else
                impl_->render();
        }
        if (impl_->hwnd && impl_->anchorHover && impl_->enabled && !impl_->visibility.visible() &&
            impl_->opensOnHover())
            SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
    });
}

void MediaPopup::setIdleContent(const IdlePresentation& content, bool available) {
    enqueue([=, this]() mutable {
        const bool textVisibilityChanged = content.showText != impl_->idle.showText;
        const bool idleAvailabilityChanged =
            content.quickStartEnabled != impl_->idle.quickStartEnabled;
        const bool quoteContentChanged = content.sentence != impl_->idle.sentence ||
                                         content.loading != impl_->idle.loading ||
                                         content.source != impl_->idle.source;
        const bool quoteChanged = quoteContentChanged ||
                                  content.showText != impl_->idle.showText ||
                                  content.showQuote != impl_->idle.showQuote ||
                                  content.copyEnabled != impl_->idle.copyEnabled ||
                                  content.quickStartEnabled != impl_->idle.quickStartEnabled;
        const bool appCountChanged = content.apps.size() != impl_->idle.apps.size();
        bool appsChanged = content.showAppNames != impl_->idle.showAppNames || appCountChanged;
        bool appIconsChanged = appCountChanged;
        const size_t appCount = std::min(content.apps.size(), impl_->idle.apps.size());
        for (size_t i = 0; i < appCount; ++i) {
            const auto& oldApp = impl_->idle.apps[i];
            const auto& newApp = content.apps[i];
            if (oldApp.path != newApp.path || oldApp.customName != newApp.customName ||
                oldApp.name != newApp.name || oldApp.pathValid != newApp.pathValid) {
                appsChanged = true;
            }
            if (oldApp.path != newApp.path || oldApp.iconPixels != newApp.iconPixels ||
                oldApp.iconWidth != newApp.iconWidth || oldApp.iconHeight != newApp.iconHeight ||
                oldApp.pathValid != newApp.pathValid) {
                appIconsChanged = true;
            }
        }

        const bool taskListStateChanged =
            content.todayTasksEnabled != impl_->idle.todayTasksEnabled ||
            content.todayTasks.size() != impl_->idle.todayTasks.size() ||
            content.todayTasksLoading != impl_->idle.todayTasksLoading ||
            content.todayTasksConnected != impl_->idle.todayTasksConnected ||
            content.todayTasksStatus != impl_->idle.todayTasksStatus;
        bool tasksChanged = taskListStateChanged;
        const size_t taskCount = std::min(content.todayTasks.size(), impl_->idle.todayTasks.size());
        for (size_t i = 0; i < taskCount; ++i) {
            const auto& oldTask = impl_->idle.todayTasks[i];
            const auto& newTask = content.todayTasks[i];
            if (oldTask.id != newTask.id || oldTask.projectId != newTask.projectId ||
                oldTask.title != newTask.title ||
                oldTask.dueText != newTask.dueText || oldTask.completed != newTask.completed ||
                oldTask.overdue != newTask.overdue || oldTask.priority != newTask.priority) {
                tasksChanged = true;
            }
        }

        const bool changed = quoteChanged || appsChanged || appIconsChanged || tasksChanged;
        if (appsChanged || appIconsChanged || tasksChanged) {
            impl_->idleScrollOffset = impl_->idleScrollView.offset();
            impl_->idleScrollView.stop();
        }

        if (quoteContentChanged || !content.showQuote || !content.copyEnabled ||
            content.loading || content.sentence.empty()) {
            if (impl_->hwnd)
                KillTimer(impl_->hwnd, kCopyFeedbackTimer);
            impl_->copySucceeded = false;
        }

        if (quoteContentChanged) {
            const bool canAnimateQuote = impl_->idleMode && impl_->visibility.visible() &&
                                         impl_->clientAnimations && !impl_->visibility.animating() &&
                                         !impl_->categoryTransition.active();
            if (canAnimateQuote) {
                if (!impl_->idleContentTransition.active) {
                    impl_->idleContentTransition.from = impl_->idle;
                    impl_->idleContentTransition.startMs = GetTickCount64();
                }
                impl_->idleContentTransition.active = true;
            } else {
                impl_->idleContentTransition.active = false;
            }
        }
        impl_->idle = content;
        impl_->available = available;
        impl_->syncPetMode(false);
        if (!content.todayTasksEnabled) {
            impl_->idleQuickTab = IdleQuickTab::QuickStart;
            impl_->hoverIdleQuickTab = -1;
            impl_->pressedIdleQuickTab = -1;
            impl_->idleQuickTabRects[0] = {};
            impl_->idleQuickTabRects[1] = {};
            impl_->hoverIdleTask = -1;
            impl_->hoverIdleTaskCheckbox = -1;
            impl_->pressedIdleTask = -1;
            impl_->pressedIdleTaskCheckbox = -1;
            impl_->idleScrollOffset = 0.0f;
            impl_->idleScrollView.setOffset(0.0f);
            impl_->idleScrollDragging = false;
        }
        if (!content.quickStartEnabled) {
            impl_->quickExpansion.expanded = false;
            impl_->quickExpansion.opening = false;
            impl_->quickExpansion.progress = 0.0f;
            impl_->stopQuickExpandTransition();
        } else if (!content.showText) {
            impl_->quickExpansion.expanded = true;
            impl_->quickExpansion.opening = false;
            impl_->quickExpansion.progress = 1.0f;
            impl_->stopQuickExpandTransition();
        } else if (textVisibilityChanged) {
            impl_->quickExpansion.expanded = false;
            impl_->quickExpansion.opening = false;
            impl_->quickExpansion.progress = 0.0f;
            impl_->stopQuickExpandTransition();
        }
        if (changed) {
            if (quoteChanged) {
                impl_->idleQuoteScrollOffset = 0.0f;
                impl_->idleQuoteSourceScrollOffset = 0.0f;
                impl_->scrollTickMs = 0;
            }
            if (quoteContentChanged)
                impl_->idleTextDirty = true;
            if (appIconsChanged)
                impl_->idleIconsDirty = true;
            if (impl_->visibility.visible() && impl_->idleMode)
                impl_->reposition();
        }
        if (!available) {
            impl_->hideImmediate();
            return;
        }
        if (changed && impl_->visibility.visible() && !impl_->categoryTransition.active() &&
            !impl_->quickExpandTransition.active() && (impl_->idleMode || idleAvailabilityChanged)) {
            if (impl_->visibility.animating())
                impl_->deferredRender = true;
            else
                impl_->render();
        }
        if (impl_->hwnd && impl_->anchorHover && impl_->enabled && !impl_->visibility.visible() &&
            impl_->opensOnHover())
            SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
    });
}

void MediaPopup::beginPresentationUpdate() {
    ++updateDepth_;
}

void MediaPopup::endPresentationUpdate() {
    if (updateDepth_ == 0 || --updateDepth_ != 0)
        return;
    auto actions = std::move(updateActions_);
    updateActions_.clear();
    enqueue([this, actions = std::move(actions)] {
        impl_->beginPresentationUpdate();
        for (const auto& action : actions)
            action();
        impl_->endPresentationUpdate();
    });
}

void MediaPopup::setPresentationMode(DisplayScene scene, bool available, bool inlineControls) {
    enqueue([=, this]() mutable {
        impl_->inlineControls = inlineControls;

        if (!available) {
            impl_->available = false;
            impl_->syncPetMode(false);
            impl_->playbackScene = false;
            impl_->idlePanelManual = false;
            impl_->idleContentTransition.active = false;
            impl_->hideImmediate();
            return;
        }

        impl_->available = true;
        impl_->syncPetMode(false);
        const bool noPlayback = scene == DisplayScene::Idle || scene == DisplayScene::NoPlayback;
        impl_->playbackScene = !noPlayback;
        if (noPlayback || inlineControls)
            impl_->idlePanelManual = false;
        const bool keepManualIdle = !inlineControls && impl_->playbackScene &&
                                    impl_->idlePanelManual &&
                                    impl_->currentPage() == MediaPopup::Impl::PopupPage::Idle &&
                                    impl_->idlePageAllowed();
        const MediaPopup::Impl::PopupPage target =
            noPlayback || keepManualIdle || inlineControls ? MediaPopup::Impl::PopupPage::Idle
                                                            : MediaPopup::Impl::PopupPage::Media;
        impl_->setPage(target);
    });
}

void MediaPopup::setProgress(int64_t positionMs) {
    enqueue([=, this]() mutable {
        const int64_t nextPositionMs = std::max<int64_t>(positionMs, 0);
        // 层转场期间新页内容承载在静态合成层快照上：进度只挂起不应用，
        // 由 finishLayerTransition 在撤层交接后再落到 positionMs 并重绘。
        if (impl_->categoryTransition.active() || impl_->quickExpandTransition.active()) {
            impl_->deferredPositionMs = nextPositionMs;
            return;
        }
        if (impl_->positionMs == nextPositionMs)
            return;
        impl_->positionMs = nextPositionMs;
        if (impl_->visibility.visible()) {
            if (impl_->presentationUpdateDepth > 0 || impl_->visibility.animating())
                impl_->renderOrDefer();
            else
                InvalidateRect(impl_->hwnd, nullptr, FALSE);
        }
    });
}

void MediaPopup::setAnchor(HWND anchor) {
    enqueue([=, this]() mutable {
        impl_->anchor = anchor;
        impl_->reposition();
    });
}

void MediaPopup::onAnchorEnter() {
    enqueue([=, this]() mutable {
        impl_->onAnchorEnter();
    });
}

void MediaPopup::onAnchorLeave() {
    enqueue([=, this]() mutable {
        impl_->onAnchorLeave();
    });
}

void MediaPopup::onAnchorClick() {
    enqueue([=, this]() mutable {
        impl_->onAnchorClick();
    });
}

void MediaPopup::hideImmediate() {
    enqueue([=, this]() mutable {
        impl_->hideImmediate();
    });
}
