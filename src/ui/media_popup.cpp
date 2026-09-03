#include "media_popup.h"

#include "fluent_theme.h"
#include "lyric_renderer.h"
#include "media_control_icons.h"
#include "platform_icon.h"
#include "logging/runtime_logger.h"

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

namespace {

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
constexpr float kIdleQuickExpandMs = 160.0f;
// 根卡片位移由 DirectComposition 按显示器刷新率执行；只有长文本内容需要
// 重绘，30fps 已足够平滑，也避免与任务栏歌词的高频提交长期争用 UI 线程。
constexpr UINT kScrollTimerMs = 32;
// 页面滑动转场只有 240ms，按 32ms 驱动只有 7-8 帧，肉眼明显卡顿；
// 转场期间改用 16ms（60fps 级别），由 updateScrollTimer 按驱动目的切换间隔。
constexpr UINT kTransitionFrameMs = 16;
// 音量滑块展开/收起过渡时长
constexpr float kVolumeSliderMs = 140.0f;

constexpr float kPopupWidthDip = 384.0f;
constexpr float kPopupHeightDip = 208.0f;
// 每日一言 + 快速启动组合面板与播放中的媒体控件卡片保持同一外层高度。
constexpr float kIdlePopupHeightDip = kPopupHeightDip;
constexpr float kPopupGapDip = 8.0f;
constexpr float kPopupCornerDip = 12.0f;
constexpr float kCoverSizeDip = 80.0f;
constexpr float kPopupTextLeftDip = 112.0f;
constexpr float kPopupTextRightPaddingDip = 16.0f;
constexpr float kPopupTextPaddingDip = 8.0f;
constexpr float kPopupInfoScrollSpeed = 10.0f;
constexpr std::size_t kIdleQuickColumnCount = 5;
constexpr float kIdleQuickCellSizeDip = 62.0f;
constexpr float kIdleQuickCellGapDip = 6.0f;
constexpr float kIdleQuickRowPitchDip = kIdleQuickCellSizeDip + kIdleQuickCellGapDip;
constexpr float kIdleQuickGridWidthDip =
    kIdleQuickCellSizeDip * static_cast<float>(kIdleQuickColumnCount) +
    kIdleQuickCellGapDip * static_cast<float>(kIdleQuickColumnCount - 1);
constexpr float kIdleQuickIconSizeDip = 24.0f;
constexpr float kIdleQuickNamedIconTopDip = 7.0f;
constexpr float kIdleQuickNamedTextTopInsetDip = 23.0f;
constexpr float kIdleQuickNamedTextBottomInsetDip = 4.0f;
constexpr float kIdleListHeightDip = kIdleQuickCellSizeDip;
constexpr float kIdleListGapDip = 8.0f;
constexpr float kIdleListBottomPaddingDip = 8.0f;
// 展开态把卡片底部的可用空间尽量交给列表；实际高度仍由列表顶部和卡片
// 底部内边距共同限制，内容超过可视区时才启用滚动。
constexpr float kIdleListExpandedHeightDip =
    kIdlePopupHeightDip - kIdleListBottomPaddingDip;
constexpr float kIdleQuickVisualClipPaddingDip = 1.0f;
constexpr float kIdleQuickExpandedTopDip = 15.0f;
constexpr float kIdleQuickTitleLeftDip = 16.0f;
constexpr float kIdleQuickTriggerHorizontalPaddingDip = 8.0f;
constexpr float kIdleQuickTriggerVerticalPaddingDip = 4.0f;
constexpr float kIdleQuickExpandButtonRightPaddingDip = 16.0f;
constexpr float kIdleQuickTabWidthDip = 112.0f;
constexpr float kIdleQuickTabGapDip = 6.0f;
constexpr float kIdleQuickTabHeightDip = 28.0f;
constexpr float kIdleQuickTabContentGapDip = 8.0f;
constexpr float kIdleTaskRowHeightDip = 28.0f;
constexpr float kIdleTaskRowGapDip = 6.0f;
constexpr float kIdleScrollBarWidthDip = 3.0f;
constexpr float kIdleScrollBarHitWidthDip = 12.0f;
// 两个页面的内容布局基准：活页绘制、转场快照与合成层转场共用同一套纵向
// 坐标，避免每条路径各算一份导致几何漂移。
constexpr float kIdleHeaderTopDip = 14.0f;
constexpr float kIdleHeaderBottomDip = 36.0f;
constexpr float kIdleQuoteTopDip = 40.0f;
constexpr float kIdleQuoteSourceGapDip = 8.0f;
constexpr float kIdleQuickHeaderGapDip = 30.0f;
constexpr float kIdleQuickHeaderHeightDip = 16.0f;
constexpr float kMediaSourceRowTopDip = 12.0f;
constexpr float kMediaSourceRowBottomDip = 34.0f;
constexpr float kMediaSourceTextLeftDip = 42.0f;
constexpr float kMediaSourceTextRightDip = 220.0f;
constexpr float kMediaTitleTopDip = 48.0f;
constexpr float kMediaTitleBottomDip = 76.0f;
constexpr float kMediaArtistTopDip = 78.0f;
constexpr float kMediaArtistBottomDip = 102.0f;
// d2d1effects.h 只声明这个 GUID；当前工程的链接配置不提供其外部定义，
// 这里保留 Direct2D 标准 Gaussian Blur CLSID 的内部定义。
constexpr CLSID kGaussianBlurClsid = {
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

enum class IdleQuickTab {
    QuickStart,
    TodayTasks,
};

class GdiplusInit {
public:
    GdiplusInit() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartupOutput output;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, &output);
        token_ = token;
    }

    ~GdiplusInit() {
        if (token_)
            Gdiplus::GdiplusShutdown(token_);
    }

private:
    ULONG_PTR token_ = 0;
};

GdiplusInit g_gdiplusInit;

bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

void releaseBitmap(ID2D1Bitmap*& bitmap) {
    if (bitmap) {
        bitmap->Release();
        bitmap = nullptr;
    }
}

void releaseBrush(ID2D1SolidColorBrush*& brush) {
    if (brush) {
        brush->Release();
        brush = nullptr;
    }
}

void releaseFormat(IDWriteTextFormat*& format) {
    if (format) {
        format->Release();
        format = nullptr;
    }
}

template <typename T>
void releaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring sourceLabel(const std::wstring& source) {
    if (source.find(L"QQMusic") != std::wstring::npos ||
        source.find(L"qqmusic") != std::wstring::npos)
        return L"QQ音乐";
    if (source.find(L"cloudmusic") != std::wstring::npos ||
        source.find(L"Netease") != std::wstring::npos ||
        source.find(L"netease") != std::wstring::npos)
        return L"网易云音乐";
    return L"音乐播放器";
}

std::wstring formatPlaybackTime(int64_t milliseconds) {
    const int64_t totalSeconds = std::max<int64_t>(milliseconds, 0) / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t seconds = totalSeconds % 60;
    auto twoDigits = [](int64_t value) {
        std::wstring text = std::to_wstring(value);
        if (text.size() < 2)
            text.insert(text.begin(), L'0');
        return text;
    };

    if (hours > 0)
        return std::to_wstring(hours) + L":" + twoDigits(minutes) + L":" +
               twoDigits(seconds);
    return twoDigits(totalSeconds / 60) + L":" + twoDigits(seconds);
}

std::wstring formatPlaybackDuration(int64_t milliseconds) {
    return milliseconds > 0 ? formatPlaybackTime(milliseconds) : L"--:--";
}

D2D1_COLOR_F colorFromRef(COLORREF color, float alpha) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                        GetBValue(color) / 255.0f, alpha);
}

float srgbChannelToLinear(BYTE value) {
    const float channel = static_cast<float>(value) / 255.0f;
    return channel <= 0.04045f
               ? channel / 12.92f
               : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

float backdropLuminance(const void* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0)
        return 0.0f;

    const auto* bytes = static_cast<const BYTE*>(pixels);
    const int step = std::max(1, std::min(width, height) / 96);
    double total = 0.0;
    int samples = 0;
    for (int y = 0; y < height; y += step) {
        for (int x = 0; x < width; x += step) {
            const BYTE* pixel = bytes + (static_cast<size_t>(y) * width + x) * 4;
            const float blue = srgbChannelToLinear(pixel[0]);
            const float green = srgbChannelToLinear(pixel[1]);
            const float red = srgbChannelToLinear(pixel[2]);
            total += 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            ++samples;
        }
    }
    return samples > 0 ? static_cast<float>(total / samples) : 0.0f;
}

} // namespace

struct MediaPopup::Impl {
    enum class PopupPage {
        Media,
        Idle,
    };

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
    bool popupVisible = false;
    bool closing = false;
    bool entering = false;
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
    bool categoryTransitionActive = false;
    PopupPage categoryTransitionFrom = PopupPage::Media;
    IdlePresentation categoryTransitionIdle;
    OverlayMediaInfo categoryTransitionMedia;
    float categoryTransitionTitleScrollOffset = 0.0f;
    float categoryTransitionArtistScrollOffset = 0.0f;
    float categoryTransitionIdleQuoteScrollOffset = 0.0f;
    int categoryTransitionDirection = 1;
    // 转场内容是否已承载到两个 DComp 合成层上；承载后横向滑动由合成器
    // 按刷新率执行（与面板滑出动画同一机制），UI 线程不再逐帧重绘。
    bool categoryLayersActive = false;
    // 快速打开展开/收起是否正由 DComp 合成层呈现（一言区 + 快速打开区两层，
    // 位移与裁剪由合成器执行）；进行中交换链不再绘制空闲页内容。
    bool quickExpandTransitionActive = false;
    bool quickExpandLayersBuilt = false;
    // 卡片高度变化时，旧卡片和新卡片的屏幕位置可能不同。这个区域只用于
    // 转场后的鼠标连续性判断，不参与绘制，也不会扩大媒体卡片的可见外观。
    bool categoryHoverEnvelopeActive = false;
    RECT categoryHoverEnvelope{};
    bool idleContentTransitionActive = false;
    IdlePresentation idleContentTransitionFrom;
    ULONGLONG idleContentTransitionStartMs = 0;
    bool idleQuickExpanded = false;
    bool idleQuickExpandOpening = false;
    float idleQuickExpandT = 0.0f;
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
    std::function<void()> onPanelOpened;

    // 应用音量控件：图标+数值固定在右上角；点击图标从图标处向左过渡展开卡内滑块
    AppVolumeState volume;
    bool volumeSliderOn = false;       // 展开目标状态
    float volumeSliderT = 0.0f;        // 当前展开进度 0-1（过渡动画）
    bool volumeSliderOpening = false;  // 过渡方向
    bool volumeDragging = false;
    bool hoverVolume = false;
    bool pressedVolume = false;
    std::function<void(int)> onAppVolume;
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

    DCompRenderer renderer;
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
    float idleQuoteWidth = 0.0f;
    float idleQuoteHeight = 0.0f;
    float titleWidth = 0.0f;
    float titleHeight = 0.0f;
    float artistWidth = 0.0f;
    float artistHeight = 0.0f;
    float titleScrollOffset = 0.0f;
    float artistScrollOffset = 0.0f;
    float idleQuoteScrollOffset = 0.0f;
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
    D2D1_RECT_F buttonRects[3]{};

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
        if (!idleContentTransitionActive || idleContentTransitionStartMs == 0)
            return 1.0f;
        const ULONGLONG now = GetTickCount64();
        const float linear = std::clamp(
            static_cast<float>(now >= idleContentTransitionStartMs
                                   ? now - idleContentTransitionStartMs
                                   : 0) /
                kInnerContentTransitionMs,
            0.0f, 1.0f);
        return linear * linear * (3.0f - 2.0f * linear);
    }

    float idleQuickExpandProgress(const IdlePresentation& content) const {
        if (!content.quickStartEnabled)
            return 0.0f;
        const float linear = std::clamp(idleQuickExpandT, 0.0f, 1.0f);
        return linear * linear * (3.0f - 2.0f * linear);
    }

    float idleQuickHeaderTop(float collapsedTop, const IdlePresentation& content) const {
        const float progress = idleQuickExpandProgress(content);
        return collapsedTop + (kIdleQuickExpandedTopDip - collapsedTop) * progress;
    }

    float idleQuickExpandButtonTop(float headerTop) const {
        // 保持按钮与卡片顶栏一致；展开后顶边仍留出圆角内侧的安全间距。
        return std::max(7.0f, headerTop - 8.0f);
    }

    std::size_t idleQuickRowCount(const IdlePresentation& content) const {
        if (content.apps.empty())
            return 0;
        return (content.apps.size() + kIdleQuickColumnCount - 1) /
               kIdleQuickColumnCount;
    }

    float idleQuickGridContentHeight(const IdlePresentation& content) const {
        const std::size_t rows = idleQuickRowCount(content);
        if (rows == 0)
            return 0.0f;
        return kIdleQuickCellSizeDip +
               static_cast<float>(rows - 1) * kIdleQuickRowPitchDip;
    }

    float idleQuickTaskContentHeight(const IdlePresentation& content) const {
        if (content.todayTasks.empty())
            return 0.0f;
        return static_cast<float>(content.todayTasks.size()) * kIdleTaskRowHeightDip +
               static_cast<float>(content.todayTasks.size() - 1) * kIdleTaskRowGapDip;
    }

    float idleQuickListContentHeight(const IdlePresentation& content) const {
        return idleQuickTab == IdleQuickTab::TodayTasks
                   ? idleQuickTaskContentHeight(content)
                   : idleQuickGridContentHeight(content);
    }

    float idleQuickGridLeft() const {
        const float availableWidth = idleListRect.right - idleListRect.left;
        return idleListRect.left +
               std::max(0.0f, (availableWidth - kIdleQuickGridWidthDip) * 0.5f);
    }

    float idleQuickListHeight(const IdlePresentation& content, float progress) const {
        const float expandedHeight = std::min(
            kIdleListExpandedHeightDip,
            std::max(kIdleQuickCellSizeDip, idleQuickListContentHeight(content)));
        return kIdleListHeightDip +
               (expandedHeight - kIdleListHeightDip) * std::clamp(progress, 0.0f, 1.0f);
    }

    float idleQuickListTop(float headerTop) const {
        const float headerBottom = headerTop + kIdleQuickHeaderHeightDip;
        const float expandButtonTop = idleQuickExpandButtonTop(headerTop);
        return std::max(headerBottom + kIdleListGapDip,
                        expandButtonTop + 32.0f + kIdleListGapDip);
    }

    float idleQuickTabContentOffset(float progress) const {
        return (kIdleQuickTabHeightDip + kIdleQuickTabContentGapDip) *
               std::clamp(progress, 0.0f, 1.0f);
    }

    float idleQuickTabTop(float contentTop, float progress) const {
        return contentTop - idleQuickTabContentOffset(progress);
    }

    D2D1_RECT_F idleQuickExpandLocalRect(float w, float headerTop) const {
        const float left = std::max(
            kIdleQuickTitleLeftDip,
            w - kIdleQuickExpandButtonRightPaddingDip - 32.0f);
        const float top = idleQuickExpandButtonTop(headerTop);
        return D2D1::RectF(left, top, left + 32.0f, top + 32.0f);
    }

    D2D1_RECT_F idleQuickTriggerLocalRect(float w, float headerTop) const {
        const D2D1_RECT_F button = idleQuickExpandLocalRect(w, headerTop);
        return D2D1::RectF(
            kIdleQuickTitleLeftDip - kIdleQuickTriggerHorizontalPaddingDip,
            button.top - kIdleQuickTriggerVerticalPaddingDip,
            button.right + kIdleQuickTriggerHorizontalPaddingDip,
            button.bottom + kIdleQuickTriggerVerticalPaddingDip);
    }

    // 空闲页（每日一言 + 快速打开）的纵向布局量测：活页绘制、转场快照和
    // 快速展开层转场共用同一份几何，保证三条路径的端点完全一致。
    struct IdleLayout {
        float quoteTop = 0.0f;           // 一言正文顶
        float quoteHeight = 0.0f;
        float sourceTop = 0.0f;          // 来源行顶
        float collapsedHeaderTop = 0.0f; // 快速打开标题行顶（收起态）
        float headerTop = 0.0f;          // 按当前展开进度插值后的标题行顶
        float expandButtonTop = 0.0f;
        float tabTop = 0.0f;             // 快捷启动/今日任务 Tab 顶
        float listTop = 0.0f;            // 当前 Tab 内容可视区顶
    };

    IdleLayout idleLayout(const IdlePresentation& content, float quoteHeight) const {
        IdleLayout layout;
        layout.quoteTop = kIdleQuoteTopDip;
        layout.quoteHeight = quoteHeight;
        layout.sourceTop = layout.quoteTop + quoteHeight + kIdleQuoteSourceGapDip;
        layout.collapsedHeaderTop = layout.sourceTop + kIdleQuickHeaderGapDip;
        layout.headerTop = idleQuickHeaderTop(layout.collapsedHeaderTop, content);
        layout.expandButtonTop = idleQuickExpandButtonTop(layout.headerTop);
        const float progress = idleQuickExpandProgress(content);
        layout.tabTop = idleQuickListTop(layout.headerTop);
        layout.listTop = layout.tabTop + idleQuickTabContentOffset(progress);
        return layout;
    }

    // “快捷启动与今日任务”标题行：展开按钮 + 标题文本；活页、快照与展开层
    // 转场共用同一套几何。
    void drawIdleQuickHeader(ID2D1DeviceContext* rt, float w, float headerTop,
                             const IdlePresentation& content, bool updateHitTest) {
        drawIdleQuickExpandButton(rt, w, headerTop, content, updateHitTest);
        const D2D1_RECT_F expandButton = idleQuickExpandLocalRect(w, headerTop);
        const float headerRight =
            content.quickStartEnabled ? expandButton.left - 8.0f : w - 16.0f;
        drawText(rt, L"快捷启动与今日任务", fmtIdleHeader,
                 D2D1::RectF(16.0f, headerTop, headerRight,
                              headerTop + kIdleQuickHeaderHeightDip),
                 brushSecondary);
    }

    void drawIdleQuickDivider(ID2D1DeviceContext* rt, float w, float headerTop,
                              const IdlePresentation& content) {
        if (!rt || !brushProgressTrack)
            return;
        const float opacity = 1.0f - idleQuickExpandProgress(content);
        if (opacity <= 0.0f)
            return;
        const float dividerTop = idleQuickExpandButtonTop(headerTop) - 4.0f;
        brushProgressTrack->SetOpacity(opacity);
        rt->FillRectangle(D2D1::RectF(16.0f, dividerTop, w - 16.0f,
                                      dividerTop + 1.0f),
                          brushProgressTrack);
        brushProgressTrack->SetOpacity(1.0f);
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
        entering = false;
        deferredRender = false;
    }

    // 结束 DComp 层转场（页面互切或快速打开展开/收起）：撤掉合成层并停掉
    // 收尾定时器。多次调用安全；调用方负责在必要时把最终交换链帧提交后，
    // 再提交层的移除。两个转场共用同一对合成层，不会同时激活（切换按钮在
    // 页面互切期间禁点，setPage 也会先停掉另一个转场）。
    void stopLayerTransition(bool& transitionActive, bool& layersActive, UINT_PTR timer) {
        transitionActive = false;
        deferredPositionMs = -1;
        if (layersActive) {
            layersActive = false;
            renderer.clearLyricTransitionLayers();
        }
        if (hwnd)
            KillTimer(hwnd, timer);
    }

    void stopCategoryTransition() {
        stopLayerTransition(categoryTransitionActive, categoryLayersActive, kCategoryTimer);
    }

    void stopQuickExpandTransition() {
        stopLayerTransition(quickExpandTransitionActive, quickExpandLayersBuilt,
                            kIdleQuickExpandTimer);
    }

    void releaseDrawingResources() {
        stopCategoryTransition();
        stopQuickExpandTransition();
        releaseVisualResources();
        renderer.discard();
        themeDirty = true;
        coverDirty = true;
        materialNeedsApply = true;
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
        releaseBitmap(coverBmp);
        releaseBitmap(sourceIconBmp);
        sourceIconLoadedFor.clear();
        for (auto*& bitmap : idleIconBitmaps)
            releaseBitmap(bitmap);
        idleIconBitmaps.clear();
        idleIconsDirty = true;
        sourceIconDirty = true;
        releaseBitmap(backdropBmp);
        releaseCom(backdropBlur);
        lastBackdropLuminance = -1.0f;
        backdropDirty = true;
        releaseBrush(brushBackground);
        releaseBrush(brushStroke);
        releaseBrush(brushText);
        releaseBrush(brushSecondary);
        releaseBrush(brushDisabled);
        releaseBrush(brushProgressTrack);
        releaseBrush(brushControl);
        releaseBrush(brushControlHover);
        releaseBrush(brushControlPressed);
        releaseBrush(brushIdleCell);
        releaseBrush(brushIdleCellHover);
        releaseBrush(brushAccent);
        releaseBrush(brushAccentHover);
        releaseBrush(brushTextOnAccent);
        releaseBrush(brushTaskPriorityHigh);
        releaseBrush(brushTaskPriorityMedium);
        releaseBrush(brushTaskPriorityLow);
        releaseBrush(brushTaskPriorityNone);
        releaseDynamicBackgroundResources();
        releaseFormat(fmtSource);
        releaseFormat(fmtTimeRight);
        releaseFormat(fmtTitle);
        releaseFormat(fmtArtist);
        releaseFormat(fmtIcon);
        releaseFormat(fmtIdleHeader);
        releaseFormat(fmtIdleQuote);
        releaseFormat(fmtIdleSource);
        releaseCom(idleAppTrimmingSign);
        releaseFormat(fmtIdleApp);
        releaseCom(titleLayout);
        releaseCom(artistLayout);
        releaseCom(idleQuoteLayout);
        titleWidth = 0.0f;
        titleHeight = 0.0f;
        artistWidth = 0.0f;
        artistHeight = 0.0f;
        idleQuoteWidth = 0.0f;
        idleQuoteHeight = 0.0f;
        idleQuoteScrollOffset = 0.0f;
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
            FAILED(rt->CreateSolidColorBrush(p.cardStroke, &brushStroke)) ||
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
        if (categoryTransitionActive && backdropBmp && lastBackdropLuminance >= 0.0f) {
            backdropDirty = false;
            return true;
        }
        backdropDirty = false;
        releaseCom(backdropBlur);
        releaseBitmap(backdropBmp);
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
            scrollTickMs = 0;
            stopCategoryTransition();
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
            idleContentTransitionActive = false;
            idleQuickExpandT = idleQuickExpanded ? 1.0f : 0.0f;
            idleQuickExpandOpening = false;
            stopQuickExpandTransition();
        }
    }

    bool createMeasuredTextLayout(const std::wstring& text, IDWriteTextFormat* format,
                                  float layoutHeight, IDWriteTextLayout** out, float& width,
                                  float& height) {
        width = 0.0f;
        height = 0.0f;
        if (!out)
            return false;
        *out = nullptr;
        if (!renderer.dwrite() || !format || text.empty())
            return false;
        if (FAILED(renderer.dwrite()->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), format, 100000.0f,
                layoutHeight, out)) ||
            !*out)
            return false;

        // 正常页和转场快照都用同一个行布局基准；外层绘制再按实际 metrics 垂直居中。
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED((*out)->GetMetrics(&metrics))) {
            releaseCom(*out);
            return false;
        }
        width = metrics.width;
        height = metrics.height;
        return true;
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
        idleQuoteWidth = 0.0f;
        idleQuoteHeight = 0.0f;
        if (!renderer.dwrite() || !fmtIdleQuote || idle.sentence.empty())
            return;

        if (!createMeasuredTextLayout(idle.sentence, fmtIdleQuote, 42.0f, &idleQuoteLayout,
                                      idleQuoteWidth, idleQuoteHeight))
            return;
        idleQuoteHeight = std::min(42.0f, idleQuoteHeight);
    }

    void decodeIdleIcons() {
        if (!idleIconsDirty)
            return;
        idleIconsDirty = false;
        for (auto*& bitmap : idleIconBitmaps)
            releaseBitmap(bitmap);
        idleIconBitmaps.clear();

        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        idleIconBitmaps.reserve(idle.apps.size());
        for (const auto& app : idle.apps) {
            ID2D1Bitmap* bitmap = nullptr;
            if (app.iconPixels && !app.iconPixels->empty() && app.iconWidth > 0 &&
                app.iconHeight > 0 &&
                SUCCEEDED(rt->CreateBitmap(
                    D2D1::SizeU(app.iconWidth, app.iconHeight), app.iconPixels->data(),
                    app.iconWidth * 4, &props, &bitmap))) {
                idleIconBitmaps.push_back(bitmap);
            } else {
                idleIconBitmaps.push_back(nullptr);
            }
        }
    }

    void layoutIdleList(float w, float top, const IdlePresentation* content = nullptr) {
        // 收起时只展示第一排；展开时尽量扩大网格可视区，内容超出后在
        // 列表内部滚动，始终不让内容越过卡片底部。
        const IdlePresentation& listContent = content ? *content : idle;
        const float expandProgress = idleQuickExpandProgress(listContent);
        const float maxListHeight = idleQuickListHeight(listContent, expandProgress);
        const float availableHeight =
            std::max(0.0f, kIdlePopupHeightDip - top - kIdleListBottomPaddingDip);
        const float listHeight = std::min(maxListHeight, availableHeight);
        // 始终为滚动条预留固定槽位，避免滚动条出现或消失时网格横向跳动。
        const float listRight = w - 16.0f - kIdleScrollBarHitWidthDip;
        idleListRect = D2D1::RectF(16.0f, top, listRight, top + listHeight);
        const float contentHeight = idleQuickListContentHeight(listContent);
        // 当前可视区放不下完整内容时就允许滚动；展开按钮只扩大可视区，
        // 如果扩大后已经能容纳全部内容，滚动条会自动消失。
        idleScrollMax = std::max(0.0f, contentHeight - listHeight);
        if (idleScrollMax <= 0.0f)
            idleScrollOffset = 0.0f;
        if (idleScrollOffset > idleScrollMax)
            idleScrollOffset = idleScrollMax;

        idleScrollTrackRect = {};
        idleScrollThumbRect = {};
        if (idleScrollMax <= 0.0f)
            return;

        const float trackTop = idleListRect.top;
        const float trackBottom = idleListRect.bottom;
        const float trackHeight = trackBottom - trackTop;
        const float thumbHeight = std::max(
            24.0f, trackHeight * listHeight / std::max(listHeight, contentHeight));
        const float usable = std::max(0.0f, trackHeight - thumbHeight);
        const float thumbTop = trackTop + idleScrollOffset / idleScrollMax * usable;
        const float trackLeft = w - 12.0f;
        idleScrollTrackRect =
            D2D1::RectF(trackLeft, trackTop, trackLeft + kIdleScrollBarWidthDip, trackBottom);
        idleScrollThumbRect =
            D2D1::RectF(trackLeft, thumbTop, trackLeft + kIdleScrollBarWidthDip,
                        thumbTop + thumbHeight);
    }

    float textAreaWidth(float popupWidth) const {
        return std::max(1.0f, popupWidth - kPopupTextLeftDip - kPopupTextRightPaddingDip);
    }

    void updateScrollTimer(float areaWidth) {
        const float idleAreaWidth = idleQuoteAreaWidth();
        const bool idleOverflow = idleQuoteWidth > idleAreaWidth;
        const bool mediaOverflow = titleWidth > areaWidth || artistWidth > areaWidth;
        const bool transitionDriving = idleContentTransitionActive;
        const bool shouldRun = popupVisible && !entering && !closing && enabled &&
                               clientAnimations &&
                               (transitionDriving ||
                                (idleMode && idleOverflow) ||
                                (!idleMode && available && media.playing && mediaOverflow));
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
            return;
        }
        if (scrollTimerRunning) {
            KillTimer(hwnd, kScrollTimer);
            scrollTimerRunning = false;
            scrollTimerInterval = 0;
        }
        if (!idleMode || !idleOverflow || !clientAnimations)
            idleQuoteScrollOffset = 0.0f;
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
        } else {
            marquee(titleWidth, mediaAreaWidth, titleScrollOffset);
            marquee(artistWidth, mediaAreaWidth, artistScrollOffset);
        }
    }

    void decodeCover() {
        coverDirty = false;
        releaseBitmap(coverBmp);
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
            releaseBitmap(sourceIconBmp);
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
        releaseBitmap(sourceIconBmp);
        sourceIconBmp = decoded;
        sourceIconLoadedFor = media.sourceAppUserModelId;
    }

    void drawText(ID2D1DeviceContext* rt, const std::wstring& text, IDWriteTextFormat* format,
                  const D2D1_RECT_F& rect, ID2D1Brush* brush) {
        if (text.empty() || !format || !brush)
            return;
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void drawScrollingText(ID2D1DeviceContext* rt, IDWriteTextLayout* layout, float textWidth,
                           float textHeight, const D2D1_RECT_F& rect, float offset,
                           ID2D1Brush* brush) {
        if (!rt || !layout || !brush)
            return;
        const float areaWidth = std::max(0.0f, rect.right - rect.left);
        const float y = rect.top +
                        std::max(0.0f, (rect.bottom - rect.top - textHeight) * 0.5f);
        const bool scrolling = textWidth > areaWidth;
        const float loopWidth = textWidth + kPopupTextPaddingDip * 2.0f;
        const float bases[2] = {rect.left - offset, rect.left - offset + loopWidth};
        const int count = scrolling ? 2 : 1;

        rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < count; ++i) {
            const float x = scrolling ? bases[i] : rect.left;
            rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
        }
        rt->PopAxisAlignedClip();
    }

    void drawChevron(ID2D1DeviceContext* rt, D2D1_POINT_2F center, float size,
                     ID2D1Brush* brush, bool right = true) {
        if (!rt || !brush)
            return;
        const float direction = right ? 1.0f : -1.0f;
        const D2D1_POINT_2F top =
            D2D1::Point2F(center.x - direction * size * 0.45f,
                          center.y - size * 0.62f);
        const D2D1_POINT_2F middle =
            D2D1::Point2F(center.x + direction * size * 0.35f, center.y);
        const D2D1_POINT_2F bottom =
            D2D1::Point2F(center.x - direction * size * 0.45f,
                          center.y + size * 0.62f);
        rt->DrawLine(top, middle, brush, 1.35f);
        rt->DrawLine(middle, bottom, brush, 1.35f);
    }

    void drawVerticalChevron(ID2D1DeviceContext* rt, D2D1_POINT_2F center, float size,
                             ID2D1Brush* brush, bool up) {
        if (!rt || !brush)
            return;
        const float direction = up ? -1.0f : 1.0f;
        const float halfWidth = size * 0.52f;
        const float shoulderOffset = size * 0.18f;
        const float tipOffset = size * 0.42f;
        // 尖端比两侧端点伸得更远，按实际包围盒中心校正视觉位置。
        const float iconCenterY =
            center.y - direction * (tipOffset - shoulderOffset) * 0.5f;
        const D2D1_POINT_2F left =
            D2D1::Point2F(center.x - halfWidth, iconCenterY - direction * shoulderOffset);
        const D2D1_POINT_2F middle =
            D2D1::Point2F(center.x, iconCenterY + direction * tipOffset);
        const D2D1_POINT_2F right =
            D2D1::Point2F(center.x + halfWidth, iconCenterY - direction * shoulderOffset);
        constexpr float kStrokeWidth = 1.5f;
        rt->DrawLine(left, middle, brush, kStrokeWidth);
        rt->DrawLine(middle, right, brush, kStrokeWidth);

        // DrawLine 默认端点是平的，用圆点补齐端帽和折点，形成更柔和的 Fluent 风格。
        const float capRadius = kStrokeWidth * 0.5f;
        rt->FillEllipse(D2D1::Ellipse(left, capRadius, capRadius), brush);
        rt->FillEllipse(D2D1::Ellipse(middle, capRadius, capRadius), brush);
        rt->FillEllipse(D2D1::Ellipse(right, capRadius, capRadius), brush);
    }

    bool usesBlackTextForControls(PopupPage page) const {
        if (!hasBackdropTextContrastForPage(page))
            return !fluent::isDarkMode(fluent::ThemeTarget::Window);
        if (!brushText)
            return !fluent::isDarkMode(fluent::ThemeTarget::Window);
        const D2D1_COLOR_F textColor = brushText->GetColor();
        return textColor.r + textColor.g + textColor.b < 1.5f;
    }

    D2D1_COLOR_F adaptiveControlFill(bool pressed, PopupPage page) const {
        if (usesBlackTextForControls(page))
            return pressed ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f)
                           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.82f);
        return pressed ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f)
                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.38f);
    }

    D2D1_COLOR_F staticControlFill(bool pressed, PopupPage page) const {
        const D2D1_COLOR_F card = cardFillForPage(page);
        const float luminance = 0.2126f * card.r + 0.7152f * card.g + 0.0722f * card.b;
        const bool darkCard = luminance < 0.5f;
        if (darkCard)
            return pressed ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)
                           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);
        return pressed ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f)
                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f);
    }

    void drawAdaptiveControlSurface(ID2D1DeviceContext* rt, const D2D1_RECT_F& rect,
                                    float radius, bool pressed, PopupPage page,
                                    bool hoverSurface = true) {
        if (!rt)
            return;
        ID2D1SolidColorBrush* brush = hoverSurface
                                          ? (pressed ? brushControlPressed : brushControlHover)
                                          : brushControl;
        if (!brush)
            return;
        if (!hasBackdropTextContrastForPage(page)) {
            if (!frostedBackgroundActiveForPage(page) && hoverSurface) {
                // 纯色模式不读取背后应用亮度，但悬浮/按下仍使用固定的反色表面。
                const D2D1_COLOR_F previousColor = brush->GetColor();
                brush->SetColor(staticControlFill(pressed, page));
                rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
                brush->SetColor(previousColor);
                return;
            }
            // 磨砂动态适配关闭时，保持原有主题画刷，不读取背后应用亮度。
            rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
            return;
        }
        const D2D1_COLOR_F previousColor = brush->GetColor();
        brush->SetColor(adaptiveControlFill(pressed, page));
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
        brush->SetColor(previousColor);
    }

    void drawCopyButton(ID2D1DeviceContext* rt, const D2D1_RECT_F& rect, bool hovered,
                        bool pressed, bool succeeded) {
        if (!rt)
            return;
        if (hovered || pressed)
            drawAdaptiveControlSurface(rt, rect, 8.0f, pressed, PopupPage::Idle);

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        if (succeeded) {
            if (brushSecondary) {
                constexpr float kStrokeWidth = 1.9f;
                const D2D1_POINT_2F left = D2D1::Point2F(cx - 7.0f, cy + 0.2f);
                const D2D1_POINT_2F middle = D2D1::Point2F(cx - 2.0f, cy + 5.2f);
                const D2D1_POINT_2F right = D2D1::Point2F(cx + 7.0f, cy - 5.0f);
                rt->DrawLine(left, middle, brushSecondary, kStrokeWidth);
                rt->DrawLine(middle, right, brushSecondary, kStrokeWidth);

                // DrawLine 默认端点是平的，用圆点补齐端帽和折点，让成功态与复制图标
                // 保持一致的柔和线条。
                const float capRadius = kStrokeWidth * 0.5f;
                rt->FillEllipse(D2D1::Ellipse(left, capRadius, capRadius), brushSecondary);
                rt->FillEllipse(D2D1::Ellipse(middle, capRadius, capRadius), brushSecondary);
                rt->FillEllipse(D2D1::Ellipse(right, capRadius, capRadius), brushSecondary);
            }
            return;
        }

        const D2D1_RECT_F back =
            D2D1::RectF(cx - 6.0f, cy - 7.0f, cx + 4.0f, cy + 6.0f);
        const D2D1_RECT_F front =
            D2D1::RectF(cx - 3.0f, cy - 5.0f, cx + 7.0f, cy + 8.0f);
        if (brushSecondary) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(back, 2.0f, 2.0f), brushSecondary,
                                     1.15f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(front, 2.0f, 2.0f), brushSecondary,
                                     1.15f);
        }
    }

    float idleUnitHeight(const IdlePresentation& content, bool current) const {
        if (current && idleQuoteLayout && content.sentence == idle.sentence)
            return std::max(18.0f, idleQuoteHeight);
        return 18.0f;
    }

    void drawIdleQuoteText(ID2D1DeviceContext* rt, float w, const IdlePresentation& content,
                           float quoteHeight, IDWriteTextLayout* layout, float textWidth,
                           float textHeight, float scrollOffset) {
        if (!rt)
            return;
        const float quoteTop = kIdleQuoteTopDip;
        const D2D1_RECT_F quoteRect =
            D2D1::RectF(16.0f, quoteTop, w - 16.0f, quoteTop + quoteHeight);
        if (layout && !content.sentence.empty()) {
            drawScrollingText(rt, layout, textWidth, textHeight, quoteRect, scrollOffset,
                              brushText);
        } else if (!content.sentence.empty()) {
            drawText(rt, content.sentence, fmtIdleQuote, quoteRect,
                     content.loading ? brushSecondary : brushText);
        } else if (content.loading) {
            drawText(rt, L"正在获取每日一言…", fmtIdleQuote, quoteRect, brushSecondary);
        } else if (content.showQuote) {
            drawText(rt, L"暂时无法获取每日一言", fmtIdleQuote, quoteRect, brushDisabled);
        }

        const float sourceTop = quoteTop + quoteHeight + kIdleQuoteSourceGapDip;
        if (!content.source.empty())
            drawText(rt, content.source, fmtIdleSource,
                     D2D1::RectF(16.0f, sourceTop, w - 16.0f, sourceTop + 18.0f),
                     brushSecondary);
    }

    void drawIdleQuoteUnit(ID2D1DeviceContext* rt, float w, const IdlePresentation& content,
                           bool current) {
        const float quoteHeight = idleUnitHeight(content, current);
        const bool useCurrentLayout =
            current && idleQuoteLayout && content.sentence == idle.sentence;
        drawIdleQuoteText(rt, w, content, quoteHeight,
                          useCurrentLayout ? idleQuoteLayout : nullptr,
                          useCurrentLayout ? idleQuoteWidth : 0.0f,
                          useCurrentLayout ? idleQuoteHeight : 0.0f,
                          useCurrentLayout ? idleQuoteScrollOffset : 0.0f);
    }

    bool idleCopyAvailable(const IdlePresentation& content) const {
        return content.showQuote && content.copyEnabled && !content.loading &&
               !content.sentence.empty();
    }

    bool idleReturnArrowVisible(const IdlePresentation& content) const {
        return playbackScene && available && content.quickStartEnabled && idlePanelManual;
    }

    void drawIdleCopyButton(ID2D1DeviceContext* rt, float w,
                            const IdlePresentation& content, bool updateHitTest) {
        if (!rt || !idleCopyAvailable(content)) {
            if (updateHitTest)
                copyRect = {};
            return;
        }

        // 复制和返回操作放在同一排，二者之间固定留出 8 DIP，避免悬浮底互相覆盖。
        // 页面互切时箭头可能暂时不绘制，但它的槽位仍要保留，避免复制按钮横向跳动。
        const bool reservePageArrowSlot = idleReturnArrowVisible(content) ||
                                          categoryTransitionActive;
        const float copyRight = reservePageArrowSlot ? w - 52.0f : w - 16.0f;
        const D2D1_RECT_F localCopy =
            D2D1::RectF(copyRight - 32.0f, 7.0f, copyRight, 39.0f);
        drawCopyButton(rt, localCopy, updateHitTest && hoverCopy,
                       updateHitTest && pressedCopy, copySucceeded);
        if (updateHitTest && hoverCopy && !copySucceeded) {
            // 提示与按钮同高并放在左侧，避免遮住一言正文，也不会改变按钮占位。
            constexpr float kTooltipWidthDip = 104.0f;
            const float tooltipRight = localCopy.left - 8.0f;
            const D2D1_RECT_F tooltip = D2D1::RectF(
                std::max(8.0f, tooltipRight - kTooltipWidthDip), 7.0f, tooltipRight, 39.0f);

            // 提示文字和气泡底沿用同一套动态对比逻辑，避免深色背景下出现白字
            // 落在浅色控件底上的低对比组合。
            drawAdaptiveControlSurface(rt, tooltip, 8.0f, false, PopupPage::Idle, false);
            if (brushStroke) {
                brushStroke->SetOpacity(0.7f);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(tooltip, 8.0f, 8.0f),
                                         brushStroke, 0.8f);
                brushStroke->SetOpacity(1.0f);
            }
            drawText(rt, L"复制每日一言", fmtIdleSource,
                     D2D1::RectF(tooltip.left + 8.0f, tooltip.top + 7.0f,
                                 tooltip.right - 8.0f, tooltip.bottom - 5.0f),
                     brushText);
        }
        if (updateHitTest)
            copyRect = D2D1::RectF(localCopy.left, cardOriginDip + localCopy.top,
                                   localCopy.right, cardOriginDip + localCopy.bottom);
    }

    void drawIdleQuickExpandButton(ID2D1DeviceContext* rt, float w, float headerTop,
                                   const IdlePresentation& content, bool updateHitTest) {
        if (!rt || !content.quickStartEnabled) {
            if (updateHitTest)
                idleQuickExpandRect = {};
            return;
        }

        const D2D1_RECT_F local = idleQuickExpandLocalRect(w, headerTop);
        const D2D1_RECT_F trigger = idleQuickTriggerLocalRect(w, headerTop);
        if (updateHitTest) {
            idleQuickExpandRect = D2D1::RectF(
                trigger.left, cardOriginDip + trigger.top, trigger.right,
                cardOriginDip + trigger.bottom);
        }
        // 高亮与命中测试解耦：理由同 drawPageArrow——快速展开按钮本身就是
        // 转场触发器，层路径不画高亮会在挂载/撤层瞬间闪变。
        if (hoverIdleQuickExpand || pressedIdleQuickExpand) {
            // 可见悬浮底与返回媒体按钮同为 32 DIP；命中区仍额外保留 4 DIP。
            const D2D1_RECT_F hoverRect =
                D2D1::RectF(trigger.left, local.top, trigger.right, local.bottom);
            drawAdaptiveControlSurface(rt, hoverRect, 6.0f, pressedIdleQuickExpand,
                                       PopupPage::Idle);
        }

        // 收起状态提示向上展开，展开状态提示向下收起。
        drawVerticalChevron(
            rt,
            D2D1::Point2F((local.left + local.right) * 0.5f,
                          (local.top + local.bottom) * 0.5f),
            6.5f, brushSecondary, !idleQuickExpanded);
    }

    void drawIdleQuickTabs(ID2D1DeviceContext* rt, float w, float top, float progress,
                           bool updateHitTest) {
        if (!rt)
            return;
        if (progress <= 0.01f) {
            if (updateHitTest) {
                idleQuickTabRects[0] = {};
                idleQuickTabRects[1] = {};
            }
            return;
        }

        const float tabWidth = std::min(
            kIdleQuickTabWidthDip,
            std::max(1.0f, (w - 32.0f - kIdleQuickTabGapDip) * 0.5f));
        const float left = 16.0f;
        const D2D1_RECT_F tabs[2] = {
            D2D1::RectF(left, top, left + tabWidth, top + kIdleQuickTabHeightDip),
            D2D1::RectF(left + tabWidth + kIdleQuickTabGapDip, top,
                        left + tabWidth * 2.0f + kIdleQuickTabGapDip,
                        top + kIdleQuickTabHeightDip),
        };
        if (updateHitTest) {
            idleQuickTabRects[0] = tabs[0];
            idleQuickTabRects[1] = tabs[1];
        }

        const float opacity = std::clamp(progress, 0.0f, 1.0f);
        const wchar_t* labels[2] = {L"快速启动", L"今日任务"};
        for (int i = 0; i < 2; ++i) {
            const bool selected =
                (i == 0 && idleQuickTab == IdleQuickTab::QuickStart) ||
                (i == 1 && idleQuickTab == IdleQuickTab::TodayTasks);
            const bool hovered = i == hoverIdleQuickTab;
            const bool pressed = i == pressedIdleQuickTab;
            if (selected && brushAccent) {
                const float previousOpacity = brushAccent->GetOpacity();
                brushAccent->SetOpacity(previousOpacity * 0.16f * opacity);
                rt->FillRoundedRectangle(D2D1::RoundedRect(tabs[i], 8.0f, 8.0f),
                                         brushAccent);
                brushAccent->SetOpacity(previousOpacity);
            }
            if (hovered || pressed)
                drawAdaptiveControlSurface(rt, tabs[i], 8.0f, pressed, PopupPage::Idle);
            if (selected && brushAccent) {
                const float previousOpacity = brushAccent->GetOpacity();
                brushAccent->SetOpacity(previousOpacity * opacity);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(tabs[i].left + 12.0f, tabs[i].bottom - 2.0f,
                                    tabs[i].right - 12.0f, tabs[i].bottom),
                        1.0f, 1.0f),
                    brushAccent);
                brushAccent->SetOpacity(previousOpacity);
            }
            ID2D1SolidColorBrush* textBrush = selected ? brushText : brushSecondary;
            if (textBrush) {
                const float previousOpacity = textBrush->GetOpacity();
                textBrush->SetOpacity(previousOpacity * opacity);
                drawText(rt, labels[i], fmtIdleApp, tabs[i], textBrush);
                textBrush->SetOpacity(previousOpacity);
            }
        }
    }

    void drawIdleTodayTasks(ID2D1DeviceContext* rt, const IdlePresentation& content) {
        if (!rt)
            return;
        if (content.todayTasks.empty()) {
            const std::wstring message =
                content.todayTasksLoading
                    ? L"正在同步今日任务…"
                    : !content.todayTasksStatus.empty()
                          ? content.todayTasksStatus
                          : !content.todayTasksConnected ? L"请在设置中连接滴答清单"
                                                         : L"今天没有待办任务";
            drawText(rt, message, fmtIdleApp, idleListRect,
                     content.todayTasksLoading ? brushSecondary : brushDisabled);
            return;
        }

        for (size_t i = 0; i < content.todayTasks.size(); ++i) {
            const auto& task = content.todayTasks[i];
            const float rowTop = idleListRect.top +
                                 static_cast<float>(i) *
                                     (kIdleTaskRowHeightDip + kIdleTaskRowGapDip) -
                                 idleScrollOffset;
            const D2D1_RECT_F row =
                D2D1::RectF(idleListRect.left, rowTop, idleListRect.right,
                            rowTop + kIdleTaskRowHeightDip);
            if (row.bottom < idleListRect.top || row.top > idleListRect.bottom)
                continue;

            rt->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), brushIdleCell);
            if (brushStroke) {
                brushStroke->SetOpacity(0.45f);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), brushStroke,
                                         0.75f);
                brushStroke->SetOpacity(1.0f);
            }
            ID2D1SolidColorBrush* priorityBrush = nullptr;
            switch (task.priority) {
            case IdleTaskPriority::High:
                priorityBrush = brushTaskPriorityHigh;
                break;
            case IdleTaskPriority::Medium:
                priorityBrush = brushTaskPriorityMedium;
                break;
            case IdleTaskPriority::Low:
                priorityBrush = brushTaskPriorityLow;
                break;
            case IdleTaskPriority::None:
            default:
                priorityBrush = brushTaskPriorityNone;
                break;
            }
            if (priorityBrush) {
                const float previousOpacity = priorityBrush->GetOpacity();
                priorityBrush->SetOpacity(previousOpacity * 0.95f);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(row.left, row.top + 6.0f, row.left + 4.0f,
                                    row.bottom - 7.0f),
                        1.5f, 1.5f),
                    priorityBrush);
                priorityBrush->SetOpacity(previousOpacity);
            }

            const D2D1_RECT_F checkbox =
                D2D1::RectF(row.left + 12.0f, row.top + 7.0f, row.left + 26.0f,
                            row.top + 21.0f);
            if (task.completed && brushAccent) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(checkbox, 4.0f, 4.0f),
                                         brushAccent);
                ID2D1Brush* checkBrush = brushTextOnAccent ? brushTextOnAccent : brushText;
                if (checkBrush) {
                    rt->DrawLine(D2D1::Point2F(checkbox.left + 3.0f, checkbox.top + 7.0f),
                                 D2D1::Point2F(checkbox.left + 6.0f, checkbox.top + 10.0f),
                                 checkBrush, 1.4f);
                    rt->DrawLine(D2D1::Point2F(checkbox.left + 6.0f, checkbox.top + 10.0f),
                                 D2D1::Point2F(checkbox.right - 3.0f, checkbox.top + 4.0f),
                                 checkBrush, 1.4f);
                }
            } else if (brushSecondary) {
                brushSecondary->SetOpacity(0.82f);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(checkbox, 4.0f, 4.0f),
                                         brushSecondary, 1.1f);
                brushSecondary->SetOpacity(1.0f);
            }

            ID2D1Brush* titleBrush = task.completed ? brushDisabled : brushText;
            ID2D1Brush* dueBrush = task.completed
                                       ? brushDisabled
                                       : task.overdue ? brushAccent : brushSecondary;
            drawText(rt, task.title, fmtSource,
                     D2D1::RectF(row.left + 38.0f, row.top + 3.0f,
                                 row.right - 96.0f, row.bottom - 3.0f),
                     titleBrush);
            drawText(rt, task.dueText, fmtTimeRight,
                     D2D1::RectF(row.right - 92.0f, row.top + 3.0f, row.right - 8.0f,
                                 row.bottom - 3.0f),
                     dueBrush);
        }
    }

    void drawIdleQuickList(ID2D1DeviceContext* rt, float w, float top,
                           const IdlePresentation& content, bool updateHitTest) {
        if (!rt)
            return;
        layoutIdleList(w, top, &content);
        const float progress = idleQuickExpandProgress(content);
        drawIdleQuickTabs(rt, w, idleQuickTabTop(top, progress), progress, updateHitTest);
        // 方块边框以几何边界为中心绘制；给可视裁剪区留出 1 DIP，避免首行
        // 的上边框被裁掉，同时不改变列表的布局和命中区域。
        const D2D1_RECT_F visualClip =
            D2D1::RectF(idleListRect.left,
                        idleListRect.top - kIdleQuickVisualClipPaddingDip,
                        idleListRect.right,
                        idleListRect.bottom + kIdleQuickVisualClipPaddingDip);
        rt->PushAxisAlignedClip(visualClip, D2D1_ANTIALIAS_MODE_ALIASED);
        if (idleQuickTab == IdleQuickTab::TodayTasks) {
            drawIdleTodayTasks(rt, content);
        } else if (content.apps.empty()) {
            drawText(rt, L"请在设置中添加应用", fmtIdleApp, idleListRect, brushDisabled);
        } else {
            const float columnGap = kIdleQuickCellGapDip;
            const float gridLeft = idleQuickGridLeft();
            for (size_t i = 0; i < content.apps.size(); ++i) {
                const size_t column = i % kIdleQuickColumnCount;
                const size_t rowIndex = i / kIdleQuickColumnCount;
                const float cellTop = idleListRect.top +
                                      static_cast<float>(rowIndex) * kIdleQuickRowPitchDip -
                                      idleScrollOffset;
                const float cellLeft = gridLeft +
                                       static_cast<float>(column) *
                                           (kIdleQuickCellSizeDip + columnGap);
                const D2D1_RECT_F cell =
                    D2D1::RectF(cellLeft, cellTop, cellLeft + kIdleQuickCellSizeDip,
                                cellTop + kIdleQuickCellSizeDip);
                if (cell.bottom < idleListRect.top || cell.top > idleListRect.bottom)
                    continue;
                const bool hovered = static_cast<int>(i) == hoverIdleApp;
                const bool valid = content.apps[i].pathValid;
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(cell, 9.0f, 9.0f),
                    hovered ? brushIdleCellHover : brushIdleCell);
                if (brushStroke) {
                    brushStroke->SetOpacity(hovered ? 0.85f : 0.55f);
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(cell, 9.0f, 9.0f),
                                             brushStroke, hovered ? 1.0f : 0.75f);
                    brushStroke->SetOpacity(1.0f);
                }

                const bool showName = content.showAppNames;
                const float iconTop = showName
                                          ? cell.top + kIdleQuickNamedIconTopDip
                                          : cell.top + (kIdleQuickCellSizeDip -
                                                        kIdleQuickIconSizeDip) * 0.5f;
                const D2D1_RECT_F iconRect =
                    D2D1::RectF(cell.left + (kIdleQuickCellSizeDip - kIdleQuickIconSizeDip) *
                                             0.5f,
                                iconTop,
                                cell.left + (kIdleQuickCellSizeDip + kIdleQuickIconSizeDip) *
                                             0.5f,
                                iconTop + kIdleQuickIconSizeDip);
                if (i < idleIconBitmaps.size() && idleIconBitmaps[i])
                    rt->DrawBitmap(idleIconBitmaps[i], iconRect, valid ? 1.0f : 0.45f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                if (showName) {
                    const D2D1_RECT_F textRect =
                        D2D1::RectF(cell.left + 4.0f,
                                    cell.bottom - kIdleQuickNamedTextTopInsetDip,
                                    cell.right - 4.0f,
                                    cell.bottom - kIdleQuickNamedTextBottomInsetDip);
                    drawText(rt,
                             content.apps[i].name.empty() ? L"未命名应用" : content.apps[i].name,
                             fmtIdleApp, textRect, valid ? brushText : brushDisabled);
                }
            }
        }
        rt->PopAxisAlignedClip();
        if (idleScrollMax > 0.0f) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(idleScrollTrackRect, 1.5f, 1.5f), brushProgressTrack);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(idleScrollThumbRect, 1.5f, 1.5f), brushSecondary);
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

        // 快速启动区域从底部向上覆盖每日一言；先裁掉被覆盖的旧内容，
        // 这样在半透明卡片或动态背景下也不会残留文字。
        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, layout.expandButtonTop),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        drawText(rt, idle.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                 D2D1::RectF(16.0f, kIdleHeaderTopDip, w - 16.0f, kIdleHeaderBottomDip),
                 brushText);

        if (idleContentTransitionActive && !categoryTransitionActive) {
            const float progress = idleContentTransitionProgress();
            if (progress >= 1.0f) {
                idleContentTransitionActive = false;
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
                    drawIdleQuoteUnit(rt, w, idleContentTransitionFrom, false);
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
        drawScrollingText(rt, title, titleW, titleH,
                          D2D1::RectF(kPopupTextLeftDip, kMediaTitleTopDip,
                                      w - kPopupTextRightPaddingDip, kMediaTitleBottomDip),
                          titleOffset, brushText);
        drawScrollingText(rt, artist, artistW, artistH,
                          D2D1::RectF(kPopupTextLeftDip, kMediaArtistTopDip,
                                      w - kPopupTextRightPaddingDip, kMediaArtistBottomDip),
                          artistOffset, brushSecondary);
    }

    void drawMediaContent(ID2D1DeviceContext* rt, float w, bool useCachedLayers) {
        if (!rt)
            return;
        drawSource(rt);
        drawCover(rt, useCachedLayers);
        drawMediaTitles(rt, w, titleLayout, titleWidth, titleHeight, titleScrollOffset,
                        artistLayout, artistWidth, artistHeight, artistScrollOffset);
        drawProgress(rt, w, media);
        for (int i = 0; i < 3; ++i)
            drawButton(rt, i, media);
    }

    void drawMediaSnapshot(ID2D1DeviceContext* rt, float w,
                           const OverlayMediaInfo& snapshot, bool useCachedLayers) {
        if (!rt)
            return;
        // 类别转场开始前保存的媒体字段可能已经被最新 SMTC 帧替换；
        // 旧层只需要一次静态快照，不参与新的布局和滚动状态。
        if (!snapshot.sourceAppUserModelId.empty()) {
            drawSourceIcon(rt, snapshot.sourceAppUserModelId);
            drawText(rt, sourceLabel(snapshot.sourceAppUserModelId), fmtSource,
                     D2D1::RectF(kMediaSourceTextLeftDip, kMediaSourceRowTopDip,
                                 kMediaSourceTextRightDip, kMediaSourceRowBottomDip),
                     brushText);
        }
        drawCover(rt, useCachedLayers);
        IDWriteTextLayout* snapshotTitleLayout = nullptr;
        IDWriteTextLayout* snapshotArtistLayout = nullptr;
        float snapshotTitleWidth = 0.0f;
        float snapshotTitleHeight = 0.0f;
        float snapshotArtistWidth = 0.0f;
        float snapshotArtistHeight = 0.0f;
        createMeasuredTextLayout(snapshot.title, fmtTitle, 40.0f, &snapshotTitleLayout,
                                 snapshotTitleWidth, snapshotTitleHeight);
        createMeasuredTextLayout(snapshot.artist, fmtArtist, 40.0f, &snapshotArtistLayout,
                                 snapshotArtistWidth, snapshotArtistHeight);
        drawMediaTitles(rt, w, snapshotTitleLayout, snapshotTitleWidth, snapshotTitleHeight,
                        categoryTransitionTitleScrollOffset, snapshotArtistLayout,
                        snapshotArtistWidth, snapshotArtistHeight,
                        categoryTransitionArtistScrollOffset);
        drawProgress(rt, w, snapshot);
        for (int i = 0; i < 3; ++i)
            drawButton(rt, i, snapshot);
        releaseCom(snapshotTitleLayout);
        releaseCom(snapshotArtistLayout);
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
        const D2D1_RECT_F rect = D2D1::RectF(16.0f, 44.0f, 16.0f + kCoverSizeDip,
                                             44.0f + kCoverSizeDip);
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, 10.0f, 10.0f), brushControl);
        ID2D1Layer* temporaryLayer = nullptr;
        ID2D1Layer* clipLayer = useCachedLayer ? coverLayer : nullptr;
        if (coverBmp && coverClip && !clipLayer && FAILED(rt->CreateLayer(&temporaryLayer)))
            clipLayer = nullptr;
        else
            clipLayer = clipLayer ? clipLayer : temporaryLayer;
        if (coverBmp && coverClip && clipLayer) {
            rt->PushLayer(D2D1::LayerParameters1(
                              D2D1::InfiniteRect(), coverClip,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                              D2D1::Matrix3x2F::Translation(rect.left, rect.top)),
                          clipLayer);
            rt->DrawBitmap(coverBmp, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
            if (temporaryLayer)
                temporaryLayer->Release();
            return;
        }
        if (temporaryLayer)
            temporaryLayer->Release();
        drawText(rt, L"♪", fmtIcon, rect, brushSecondary);
    }

    void drawSourceIcon(ID2D1DeviceContext* rt, const std::wstring& source) {
        const D2D1_RECT_F iconRect = D2D1::RectF(16.0f, 14.0f, 34.0f, 32.0f);
        if (sourceIconBmp && sourceIconLoadedFor == source) {
            rt->DrawBitmap(sourceIconBmp, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            rt->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 5.0f, 5.0f), brushAccent);
            drawText(rt, L"♪", fmtIcon, iconRect, brushTextOnAccent);
        }
    }

    void drawSource(ID2D1DeviceContext* rt) {
        drawSourceIcon(rt, media.sourceAppUserModelId);
        drawText(rt, sourceLabel(media.sourceAppUserModelId), fmtSource,
                 D2D1::RectF(kMediaSourceTextLeftDip, kMediaSourceRowTopDip,
                             kMediaSourceTextRightDip, kMediaSourceRowBottomDip),
                 brushText);
    }

    void drawProgress(ID2D1DeviceContext* rt, float w, const OverlayMediaInfo& content) {
        if (!rt || !brushProgressTrack || !brushAccent || !brushSecondary || !fmtSource ||
            !fmtTimeRight)
            return;

        const float left = 16.0f;
        const float right = std::max(left + 1.0f, w - 16.0f);
        const float trackTop = 148.0f;
        const float trackBottom = 152.0f;
        const D2D1_RECT_F track = D2D1::RectF(left, trackTop, right, trackBottom);
        rt->FillRoundedRectangle(D2D1::RoundedRect(track, 2.0f, 2.0f), brushProgressTrack);

        const int64_t displayPositionMs =
            content.durationMs > 0
                ? std::clamp(positionMs, int64_t{0}, content.durationMs)
                : std::max<int64_t>(positionMs, 0);
        if (content.durationMs > 0) {
            const float fraction = static_cast<float>(
                static_cast<double>(displayPositionMs) / static_cast<double>(content.durationMs));
            const float fillRight = left + (right - left) * fraction;
            if (fillRight > left)
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(left, trackTop, fillRight, trackBottom),
                                      2.0f, 2.0f),
                    brushAccent);
        }

        const float timeTop = 126.0f;
        const float timeBottom = 144.0f;
        constexpr float kTimeLabelWidth = 72.0f;
        drawText(rt, formatPlaybackTime(displayPositionMs), fmtSource,
                 D2D1::RectF(left, timeTop, left + kTimeLabelWidth, timeBottom),
                 brushSecondary);
        drawText(rt, formatPlaybackDuration(content.durationMs), fmtTimeRight,
                 D2D1::RectF(right - kTimeLabelWidth, timeTop, right, timeBottom),
                 brushSecondary);
    }

    bool buttonEnabled(int index, const OverlayMediaInfo& content) const {
        if (!available)
            return false;
        // 某些播放器的 SMTC 快照会把前后曲目能力标成 false，但对应的
        // TrySkipPrevious/Next 仍然是可用操作；媒体卡片保留明确的操作入口。
        return index == 1 ? content.canPlayPause : true;
    }

    void drawButton(ID2D1DeviceContext* rt, int index, const OverlayMediaInfo& content) {
        const bool enabled = buttonEnabled(index, content);
        // buttonRects 保存的是窗口客户区命中坐标；绘制层已经通过
        // cardOriginDip 把卡片移到窗口中的实际位置，因此这里还原为卡片内部坐标，
        // 避免上下方弹出或横向转场时把垂直偏移叠加一次。
        const D2D1_RECT_F hitRect = buttonRects[index];
        const D2D1_RECT_F rect = D2D1::RectF(
            hitRect.left, hitRect.top - cardOriginDip,
            hitRect.right, hitRect.bottom - cardOriginDip);
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        if (index == 1) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(rect, 18.0f, 18.0f),
                enabled ? (pressedButton == index ? brushAccent : hoverButton == index
                                                        ? brushAccentHover
                                                        : brushAccent)
                        : brushControl);
        } else if (hoverButton == index || pressedButton == index) {
            drawAdaptiveControlSurface(rt, rect, 18.0f, pressedButton == index,
                                       PopupPage::Media);
        }

        ID2D1Brush* iconBrush = enabled ? brushText : brushDisabled;
        if (index == 1 && enabled)
            iconBrush = brushTextOnAccent;
        const float radius = index == 1 ? 11.0f
                                        : (rect.bottom - rect.top) * 0.26f;
        media_control::draw(rt, controlGeometry, index, content.playing,
                             D2D1::Point2F(cx, cy), radius, iconBrush);
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
        float snapshotQuoteWidth = 0.0f;
        float snapshotQuoteHeight = 0.0f;
        createMeasuredTextLayout(content.sentence, fmtIdleQuote, 42.0f, &snapshotQuoteLayout,
                                 snapshotQuoteWidth, snapshotQuoteHeight);
        snapshotQuoteHeight = std::min(42.0f, snapshotQuoteHeight);
        const float quoteHeight = snapshotQuoteLayout
                                       ? std::max(18.0f, snapshotQuoteHeight)
                                       : 18.0f;
        const IdleLayout layout = idleLayout(content, quoteHeight);

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, layout.expandButtonTop),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        drawText(rt, content.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                 D2D1::RectF(16.0f, kIdleHeaderTopDip, w - 16.0f, kIdleHeaderBottomDip),
                 brushText);
        drawIdleQuoteText(rt, w, content, quoteHeight, snapshotQuoteLayout,
                          snapshotQuoteWidth, snapshotQuoteHeight,
                          categoryTransitionIdleQuoteScrollOffset);
        drawIdleCopyButton(rt, w, content, false);
        rt->PopAxisAlignedClip();

        drawIdleQuickDivider(rt, w, layout.headerTop, content);
        drawIdleQuickHeader(rt, w, layout.headerTop, content, false);
        drawIdleQuickList(rt, w, layout.listTop, content, false);
        releaseCom(snapshotQuoteLayout);
    }

    void drawPageContent(ID2D1DeviceContext* rt, float w, PopupPage page, bool oldLayer,
                         bool updateHitTest, bool useCachedLayers) {
        if (page == PopupPage::Idle) {
            if (oldLayer && categoryTransitionFrom == PopupPage::Idle)
                drawIdleSnapshot(rt, w, categoryTransitionIdle);
            else
                drawIdle(rt, w, updateHitTest);
        } else {
            if (oldLayer && categoryTransitionFrom == PopupPage::Media)
                drawMediaSnapshot(rt, w, categoryTransitionMedia, useCachedLayers);
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
        if (categoryTransitionActive)
            return;
        // 快速打开展开/收起期间，一言区与快速打开区承载在两个 DComp 合成层上，
        // 交换链只画卡片底与返回箭头，不重复绘制空闲页内容。
        if (quickExpandTransitionActive && currentPage() == PopupPage::Idle) {
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
            !renderer.ensureLyricTransitionLayers(cardWidthPx, cardHeightPx, cardWidthPx,
                                                  cardHeightPx, kPopupCornerDip * scale()))
            return false;

        const float baseY = cardOriginDip * scale();
        if (!renderer.ensureLyricTransitionBackdrop(cardWidthPx, cardHeightPx, baseY,
                                                    kPopupCornerDip * scale()))
            return false;
        if (auto* dc = renderer.beginLyricTransitionBackdropDraw()) {
            drawCardBackground(dc, w, popupHeightDip(), categoryTransitionFrom, false);
            if (!renderer.endLyricTransitionBackdropDraw(dc))
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
            {0, categoryTransitionFrom, true},
            {1, currentPage(), false},
        };
        for (const auto& job : jobs) {
            auto* dc = renderer.beginLyricLayerDraw(job.index);
            if (!dc)
                return false;
            drawCategoryContentLayer(dc, w, job.page, job.oldLayer);
            if (!renderer.endLyricLayerDraw(job.index, dc))
                return false;
        }

        // 先以挂载初始状态（内容层创建即 0 透明度、背景层同为 0 透明度）
        // 提交并等待合成器处理完，让新表面先进入合成树；再启动动画并把背景层
        // 转可见，避免新表面首帧在合成器里显示空白底色。根交换链在整个页面
        // 互切期间保持上一张完整卡片，不再出现空白交换链帧。
        renderer.commit();
        renderer.waitForCommitCompletion();
        const float travel = kCategoryTransitionTravelDip * scale();
        const float dir = static_cast<float>(categoryTransitionDirection);
        const float durationSec = kCategoryTransitionMs / 1000.0f;
        if (!renderer.showLyricTransitionBackdrop() ||
            !renderer.animateLyricLayerX(0, 0.0f, -dir * travel, baseY, 1.0f, 0.0f,
                                         durationSec) ||
            !renderer.animateLyricLayerX(1, dir * travel, 0.0f, baseY, 0.0f, 1.0f,
                                         durationSec))
            return false;
        categoryLayersActive = true;
        renderer.commit();
        // 合成器动画在 kCategoryTransitionMs 后停在终点；定时器稍加余量收尾：
        // 先预绘制目标页，再撤层提交。余量内层已静止在终点，交接无跳变。
        SetTimer(hwnd, kCategoryTimer, static_cast<UINT>(kCategoryTransitionMs) + 32,
                 nullptr);
        return true;
    }

    // 快速打开展开/收起：把一言区（含分隔线）和展开态的快速打开区分别画进
    // 两个 DComp 合成层。快速打开区整体上滑/下滑（OffsetY 动画），列表视口
    // 高度收放（层内矩形裁剪底边动画——推导后可证该底边在层内坐标系中与
    // 位移无关，直接按两个端点值插值）；一言区原地不动，由裁剪底边扫过
    // 模拟被覆盖/显露。几何与 drawIdle 的逐帧版本在两个端点完全一致。
    bool startQuickExpandLayerTransition(float w) {
        if (!hwnd || cardWidthPx <= 0 || cardHeightPx <= 0)
            return false;

        const float s = scale();
        const bool opening = idleQuickExpanded;
        const IdleLayout layout = idleLayout(idle, idleUnitHeight(idle, true));
        const float collapsedHeaderTop = layout.collapsedHeaderTop;
        const float expandedHeaderTop = kIdleQuickExpandedTopDip;
        const float delta = std::max(0.0f, collapsedHeaderTop - expandedHeaderTop);
        const float expandedButtonTop = idleQuickExpandButtonTop(expandedHeaderTop);
        const float collapsedButtonTop = idleQuickExpandButtonTop(collapsedHeaderTop);
        // 层 1（快速打开区）原点对齐展开态按钮顶；按钮与标题同体平移
        const float quickOriginY = expandedButtonTop;

        const float expandedListTop = idleQuickListTop(expandedHeaderTop) +
                                      idleQuickTabContentOffset(1.0f);
        const float collapsedListTop = idleQuickListTop(collapsedHeaderTop) +
                                       idleQuickTabContentOffset(0.0f);
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

        const int layer0W = cardWidthPx;
        const int layer0H = static_cast<int>(std::lround(collapsedButtonTop * s)) + 1;
        const int layer1W = cardWidthPx;
        const int layer1H = static_cast<int>(std::lround(clipBottomExpanded * s)) + 1;
        if (!renderer.ensureLyricTransitionLayers(layer0W, layer0H, layer1W, layer1H))
            return false;

        // 层 0：一言区，原点即卡片原点；分隔线画在收起位置，随裁剪底边扫过
        // 被隐藏/显露（与原逐帧版本的淡出/淡入近似，端点一致）。
        if (auto* dc = renderer.beginLyricLayerDraw(0)) {
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
            if (!renderer.endLyricLayerDraw(0, dc))
                return false;
        } else {
            return false;
        }

        // 层 1：快速打开区，按展开态完整绘制（列表布局依赖展开进度，临时固定
        // 为展开态；收起过程的收缩由合成层裁剪动画完成）。层原点 =
        // 卡片 y=quickOriginY，用平移变换把卡片坐标映射进层内坐标。
        if (auto* dc = renderer.beginLyricLayerDraw(1)) {
            const float savedT = idleQuickExpandT;
            idleQuickExpandT = 1.0f;
            // 保留 BeginDraw 的表面更新偏移，在其上叠加层原点平移
            D2D1_MATRIX_3X2_F baseTransform;
            dc->GetTransform(&baseTransform);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -quickOriginY) *
                             baseTransform);
            drawIdleQuickHeader(dc, w, expandedHeaderTop, idle, false);
            drawIdleQuickList(dc, w, expandedListTop, idle, false);
            idleQuickExpandT = savedT;
            if (!renderer.endLyricLayerDraw(1, dc))
                return false;
        } else {
            return false;
        }

        // 同页面转场：先提交 0 透明度的内容层，再和交换链的目标帧一起提交
        // 动画状态，避免合成器在首次挂载子视觉时显示一帧空白底色。
        renderer.commit();

        const float quoteBaseY = cardOriginDip * s;
        const float quickBaseY = (cardOriginDip + quickOriginY) * s;
        const float travelPx = delta * s;
        const float durationSec = kIdleQuickExpandMs / 1000.0f;
        if (!renderer.animateLyricLayerClipSlide(
                0, 0.0f, quoteBaseY, quoteBaseY,
                (opening ? collapsedButtonTop : expandedButtonTop) * s,
                (opening ? expandedButtonTop : collapsedButtonTop) * s, durationSec))
            return false;
        if (!renderer.animateLyricLayerClipSlide(
                1, 0.0f, opening ? quickBaseY + travelPx : quickBaseY,
                opening ? quickBaseY : quickBaseY + travelPx,
                (opening ? clipBottomCollapsed : clipBottomExpanded) * s,
                (opening ? clipBottomExpanded : clipBottomCollapsed) * s, durationSec))
            return false;
        quickExpandLayersBuilt = true;
        return true;
    }

    bool render() {
        if (!hwnd)
            return false;
        // 合成层已经覆盖卡片时，任何额外的交换链 Present 都可能把透明底单独
        // 推给合成器；状态会在转场收尾时统一预绘制并交接。
        if ((categoryTransitionActive && categoryLayersActive) ||
            (quickExpandTransitionActive && quickExpandLayersBuilt))
            return true;
        if (presentationUpdateDepth > 0) {
            presentationUpdatePending = true;
            return true;
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int pxW = rc.right - rc.left;
        const int pxH = rc.bottom - rc.top;
        if (pxW <= 0 || pxH <= 0)
            return false;
        if (!renderer.bind(hwnd, pxW, pxH))
            return false;
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
        const bool categoryTransitionNeedsDynamicBackground =
            categoryTransitionActive &&
            (dynamicBackgroundEnabledForPage(categoryTransitionFrom) ||
             dynamicBackgroundEnabledForPage(currentPage()));
        if (categoryTransitionNeedsDynamicBackground)
            createDynamicBackgroundResources(true);
        else if (dynamicBackgroundEnabled())
            createDynamicBackgroundResources();
        const bool categoryTransitionNeedsBackdrop =
            categoryTransitionActive &&
            (frostedBackgroundActiveForPage(categoryTransitionFrom) ||
             frostedBackgroundActiveForPage(currentPage()));
        const bool prewarmBackdrop =
            !popupVisible &&
            (frostedBackgroundActiveForPage(PopupPage::Media) ||
             frostedBackgroundActiveForPage(PopupPage::Idle));
        if (frostedBackgroundActive() || categoryTransitionNeedsBackdrop || prewarmBackdrop)
            captureBackdrop(!frostedBackgroundActive() &&
                            (categoryTransitionNeedsBackdrop || prewarmBackdrop));
        buildTextLayouts();
        if (coverDirty)
            decodeCover();
        if (sourceIconDirty)
            decodeSourceIcon();
        if (idleMode ||
            (categoryTransitionActive && categoryTransitionFrom == PopupPage::Idle)) {
            if (idleMode)
                buildIdleTextLayout();
            decodeIdleIcons();
        }

        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;
        const float w = dip(pxW);
        const float cardH = popupHeightDip();
        const float infoW = textAreaWidth(w);
        updateScrollTimer(infoW);
        buttonRects[0] = D2D1::RectF(w * 0.5f - 102.0f, cardOriginDip + 164.0f,
                                     w * 0.5f - 66.0f, cardOriginDip + 200.0f);
        buttonRects[1] = D2D1::RectF(w * 0.5f - 20.0f, cardOriginDip + 162.0f,
                                     w * 0.5f + 20.0f, cardOriginDip + 202.0f);
        buttonRects[2] = D2D1::RectF(w * 0.5f + 66.0f, cardOriginDip + 164.0f,
                                     w * 0.5f + 102.0f, cardOriginDip + 200.0f);

        // 页面互切：把固定圆角底板、两页内容和横向位移动画一次性提交到
        // DComp。主交换链保留上一张完整卡片，切换期间不再清空或 Present。
        if (categoryTransitionActive && !categoryLayersActive &&
            !startCategoryLayerTransition(w)) {
            stopCategoryTransition();
        }
        // 快速打开展开/收起：同理，装层失败则本帧直接呈现目标状态。
        if (quickExpandTransitionActive && !quickExpandLayersBuilt &&
            !startQuickExpandLayerTransition(w)) {
            stopQuickExpandTransition();
        }

        const bool categoryLayersCoverCard = categoryTransitionActive && categoryLayersActive;
        if (categoryLayersCoverCard)
            return true;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, cardOriginDip));
        drawCardBackground(rt, w, cardH, currentPage(), true);
        drawCategoryContent(rt, w, cardH);

        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            releaseDrawingResources();
            return false;
        }
        if (FAILED(hr)) {
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
        if (!renderer.present()) {
            releaseDrawingResources();
            return false;
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
        if (!pending || !popupVisible)
            return;
        if (entering || closing) {
            deferredRender = true;
            return;
        }
        render();
    }

    // 层转场收尾（页面互切与快速打开展开/收起共用）：先在转场层仍覆盖着
    // 交换链时提交最终页面，并等待该批命令被合成器处理；随后再提交撤层，
    // 确保撤层时交换链已经是最终页面。
    void finishLayerTransition(bool& transitionActive, bool& layersActive, UINT_PTR timer) {
        if (!transitionActive)
            return;
        // 转场期间挂起的进度先取出来：预绘制必须用冻结的旧值，与层内快照
        // 逐像素一致；撤层交接完成后再应用新进度，表现为正常的一帧跳秒。
        const int64_t pendingPosition = deferredPositionMs;
        const bool canPrimeTarget = popupVisible && !entering && !closing;
        if (canPrimeTarget) {
            transitionActive = false;
            // 目标页先正常送入交换链并提交，但转场层仍覆盖在上面；等待这一批
            // 被合成器处理完后再撤层，避免撤层先于目标交换链帧进入可见合成帧。
            render();
            renderer.waitForCommitCompletion();
        }
        stopLayerTransition(transitionActive, layersActive, timer);
        renderer.commit();
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
        if (categoryTransitionActive)
            return -1;
        for (int i = 0; i < 3; ++i) {
            if (buttonEnabled(i, media) && contains(buttonRects[i], x, y))
                return i;
        }
        return -1;
    }

    bool hitVolumeIcon(float x, float y) const {
        return !categoryTransitionActive &&
               currentPage() == PopupPage::Media &&
               volume.available && contains(volumeIconRect, x, y);
    }

    bool hitVolumeSlider(float x, float y) const {
        // 展开过半才允许拖动；命中区域比细轨道上下左右各扩一圈余量
        if (categoryTransitionActive ||
            currentPage() != PopupPage::Media ||
            !volume.available || !volumeSliderOn ||
            volumeSliderT < 0.6f)
            return false;
        return x >= volumeSliderRect.left - 7.0f && x <= volumeSliderRect.right + 7.0f &&
               y >= volumeSliderRect.top - 8.0f && y <= volumeSliderRect.bottom + 8.0f;
    }

    bool hitPageArrow(float x, float y) const {
        return !categoryTransitionActive && playbackScene && idlePageAllowed() &&
               (currentPage() == PopupPage::Media ||
                (currentPage() == PopupPage::Idle && idlePanelManual)) &&
               pageArrowRect.right > pageArrowRect.left &&
               contains(pageArrowRect, x, y);
    }

    bool hitCopy(float x, float y) const {
        return !categoryTransitionActive && currentPage() == PopupPage::Idle &&
               idle.copyEnabled && !idle.loading &&
               !idle.sentence.empty() && copyRect.right > copyRect.left &&
               contains(copyRect, x, y);
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

        const RECT fromCardRect = popupCardScreenRect();
        const bool hasFromCardRect = popupVisible && cardWidthPx > 0 && cardHeightPx > 0;
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
        categoryTransitionActive = popupVisible && clientAnimations && !entering && !closing;
        if (categoryTransitionActive) {
            categoryTransitionFrom = from;
            categoryTransitionIdle = idleContentTransitionActive
                                         ? idleContentTransitionFrom
                                         : idle;
            categoryTransitionMedia = media;
            categoryTransitionTitleScrollOffset = titleScrollOffset;
            categoryTransitionArtistScrollOffset = artistScrollOffset;
            categoryTransitionIdleQuoteScrollOffset = idleQuoteScrollOffset;
            categoryTransitionDirection = categoryDirection(from, target);
        }
        idleContentTransitionActive = false;
        idleScrollOffset = 0.0f;
        idleQuoteScrollOffset = 0.0f;
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
        if (categoryTransitionActive && hasFromCardRect) {
            categoryHoverEnvelope = mergeScreenRects(fromCardRect, popupCardScreenRect());
            categoryHoverEnvelopeActive = true;
        }
        if (popupVisible) {
            if (entering || closing)
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
        return !categoryTransitionActive && idleMode && idle.quickStartEnabled &&
               idleQuickExpandRect.right > idleQuickExpandRect.left &&
               contains(idleQuickExpandRect, x, y);
    }

    int hitIdleQuickTab(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransitionActive || quickExpandTransitionActive || !idleMode ||
            !idle.quickStartEnabled || !idleQuickExpanded || idleQuickExpandT < 0.99f)
            return -1;
        for (int i = 0; i < 2; ++i) {
            if (contains(idleQuickTabRects[i], x, y))
                return i;
        }
        return -1;
    }

    int hitIdleApp(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransitionActive || !idleMode || idleQuickTab != IdleQuickTab::QuickStart ||
            idle.apps.empty() ||
            !contains(idleListRect, x, y))
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

    bool hitIdleScrollBar(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransitionActive || !idleMode || idleScrollMax <= 0.0f)
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
        renderOrDefer();
    }

    void scrollIdleBy(float delta) {
        if (!idleMode || idleScrollMax <= 0.0f)
            return;
        idleScrollOffset = std::clamp(idleScrollOffset + delta, 0.0f, idleScrollMax);
        renderOrDefer();
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
        renderOrDefer();
        if (onAppVolume)
            onAppVolume(next);
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
        if (!hwnd || !idleMode || !idle.quickStartEnabled)
            return;
        // 上一次层动画还在进行时再次点击：合成器动画无法中途取值反向，
        // 直接停掉（瞬时落到当前目标状态），再从新目标重新起步。
        if (quickExpandTransitionActive)
            stopQuickExpandTransition();
        idleQuickExpanded = !idleQuickExpanded;
        idleQuickExpandT = idleQuickExpanded ? 1.0f : 0.0f;
        idleQuickExpandOpening = false;
        if (!clientAnimations || !popupVisible || entering || closing) {
            KillTimer(hwnd, kIdleQuickExpandTimer);
            renderOrDefer();
            return;
        }
        // 展开/收起由 DComp 合成层执行（render() 里装层）；定时器在合成器
        // 动画到达终点并静止后收尾：先预绘制最终状态，再撤层完成交接。
        quickExpandTransitionActive = true;
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
        if (!available || categoryTransitionActive || currentPage() != PopupPage::Media ||
            media.sourceAppUserModelId.empty())
            return false;
        // 与 drawSource 的顶部来源行保持同一块可点击区域，给图标和名称都留出
        // 一点点击余量，但不侵入下面的封面和歌曲标题区域。
        return contains(D2D1::RectF(8.0f, cardOriginDip + 8.0f, 220.0f,
                                     cardOriginDip + 40.0f),
                        x, y);
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
        if (!hwnd || !popupVisible)
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
        if (!hwnd || !popupVisible || anchorHover || popupHover ||
            categoryTransitionActive || quickExpandTransitionActive)
            return;
        KillTimer(hwnd, kShowTimer);
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void renderOrDefer() {
        // DComp 正在独立驱动内容层时，交换链重绘既不能更新层内快照，
        // 又会制造额外 Present；最终收尾帧会统一提交最新状态。
        if (categoryTransitionActive || quickExpandTransitionActive)
            return;
        if (entering || closing) {
            deferredRender = true;
            return;
        }
        render();
    }

    void showPopup() {
        if (!hwnd || !enabled || !available)
            return;
        const bool wasVisible = popupVisible;
        KillTimer(hwnd, kShowTimer);
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kCloseTimer);
        KillTimer(hwnd, kScrollTimer);
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kVolumeTimer);
        KillTimer(hwnd, kIdleQuickExpandTimer);
        scrollTimerRunning = false;
        deferredRender = false;
        entering = true;
        closing = false;
        reposition();
        const bool canUseFrostedBackdropOnEitherPage =
            frostedBackgroundActiveForPage(PopupPage::Media) ||
            frostedBackgroundActiveForPage(PopupPage::Idle);
        if (canUseFrostedBackdropOnEitherPage)
            backdropDirty = true;
        if (!render()) {
            entering = false;
            return;
        }

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
        popupVisible = true;
        runtime_log::writef(L"[action][media-popup] shown trigger=%s",
                            opensOnHover() ? L"hover" : L"click");
        if (!wasVisible && onPanelOpened)
            onPanelOpened();
        SetTimer(hwnd, kEnterTimer, kOpenAnimationMs, nullptr);
    }

    void hideAnimated() {
        if (!popupVisible || closing || categoryTransitionActive ||
            quickExpandTransitionActive)
            return;
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kScrollTimer);
        KillTimer(hwnd, kVolumeTimer);
        KillTimer(hwnd, kIdleQuickExpandTimer);
        scrollTimerRunning = false;
        entering = false;
        deferredRender = false;
        closing = true;
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
        if (!categoryTransitionActive) {
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
        }
        KillTimer(hwnd, kHideTimer);
        trackMouseLeave();
        if (closing)
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
        if (enabled && available && !popupVisible && opensOnHover())
            SetTimer(hwnd, kShowTimer, kShowDelayMs, nullptr);
    }

    void onAnchorClick() {
        anchorHover = true;
        if (!hwnd || !enabled || !available || popupVisible || opensOnHover())
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
        if (playbackScene && idleMode && !inlineControls)
            idleMode = false;
        idlePanelManual = false;
        popupVisible = false;
        closing = false;
        stopCategoryTransition();
        stopQuickExpandTransition();
        categoryHoverEnvelopeActive = false;
        categoryHoverEnvelope = {};
        idleContentTransitionActive = false;
        volumeSliderOn = false;
        volumeSliderT = 0.0f;
        volumeDragging = false;
        hoverVolume = false;
        pressedVolume = false;
        hoverIdleApp = -1;
        pressedIdleApp = -1;
        idleScrollDragging = false;
        resetPageInteractionState();
        idleQuickExpanded = false;
        idleQuickExpandOpening = false;
        idleQuickExpandT = 0.0f;
        if (!hwnd)
            return;
        killTimers();
        ShowWindow(hwnd, SW_HIDE);
        renderer.resetRoot();
        renderer.commit();
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
                     hitIdleApp(dip(point.x), dip(point.y)) >= 0)) {
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
                const bool arrow = hitPageArrow(mx, my);
                const bool copy = hitCopy(mx, my);
                const bool quickExpand = hitIdleQuickExpand(mx, my);
                const int quickTab = hitIdleQuickTab(mx, my);
                if (idleApp != hoverIdleApp || arrow != hoverPageArrow || copy != hoverCopy ||
                    quickExpand != hoverIdleQuickExpand || quickTab != hoverIdleQuickTab ||
                    hoverVolume) {
                    hoverIdleApp = idleApp;
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
            hoverPageArrow = false;
            hoverCopy = false;
            hoverIdleQuickExpand = false;
            hoverIdleQuickTab = -1;
            onPopupLeave();
            renderOrDefer();
            return 0;
        case WM_LBUTTONDOWN: {
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
                if (pressedIdleApp >= 0) {
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
                volumeDragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
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
                    const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                    if (steps != 0)
                        scrollIdleBy(-static_cast<float>(steps) * 32.0f);
                }
                return 0;
            }
            if (onAppVolume && (hitVolumeIcon(x, y) || hitVolumeSlider(x, y))) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0) {
                    volume.percent = std::clamp(volume.percent + steps * 2, 0, 100);
                    volume.muted = false;
                    renderOrDefer();
                    onAppVolume(volume.percent);
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressedButton = -1;
            pressedSource = false;
            pressedSourceAppUserModelId.clear();
            pressedVolume = false;
            volumeDragging = false;
            pressedIdleApp = -1;
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
            if (wp == kShowTimer) {
                KillTimer(hwnd, kShowTimer);
                if (anchorHover && opensOnHover())
                    showPopup();
            } else if (wp == kHideTimer) {
                KillTimer(hwnd, kHideTimer);
                if (categoryTransitionActive || quickExpandTransitionActive)
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
                if (closing && !anchorHover && !popupHover)
                    hideImmediate();
                else if (closing)
                    showPopup();
            } else if (wp == kEnterTimer) {
                KillTimer(hwnd, kEnterTimer);
                entering = false;
                if (popupVisible && !closing) {
                    if (deferredRender) {
                        deferredRender = false;
                        render();
                    }
                    updateScrollTimer(textAreaWidth(kPopupWidthDip));
                }
            } else if (wp == kScrollTimer) {
                if (entering || closing || !popupVisible)
                    return 0;
                // 层转场期间内容承载在静态合成层快照上，滚动偏移必须保持冻结；
                // 否则收尾帧用推进后的偏移重绘，交接瞬间文字横向跳变。
                // 冻结只停偏移、计时基准照常推进，避免收尾后第一帧把转场
                // 时长一次性补进偏移造成二次跳动。
                if (categoryTransitionActive || quickExpandTransitionActive) {
                    scrollTickMs = GetTickCount64();
                    return 0;
                }
                advanceTextScroll();
                renderOrDefer();
            } else if (wp == kVolumeTimer) {
                if (entering || closing || !popupVisible) {
                    KillTimer(hwnd, kVolumeTimer);
                    return 0;
                }
                // 与文本滚动同理：层转场期间冻结滑块展开进度，保持与层内快照一致。
                if (categoryTransitionActive || quickExpandTransitionActive)
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
                bool& transitionActive =
                    quickExpand ? quickExpandTransitionActive : categoryTransitionActive;
                if (transitionActive) {
                    finishLayerTransition(transitionActive,
                                          quickExpand ? quickExpandLayersBuilt
                                                      : categoryLayersActive,
                                          wp);
                    if (popupVisible && !entering && !closing) {
                        refreshPopupHoverAfterTransition();
                    }
                }
            }
            return 0;
        case WM_DPICHANGED:
            dpi = GetDpiForWindow(hwnd);
            releaseDrawingResources();
            reposition();
            if (popupVisible)
                render();
            return 0;
        case WM_DISPLAYCHANGE:
            reposition();
            return 0;
        case WM_SETTINGCHANGE:
            refreshClientAnimations();
            releaseDrawingResources();
            if (popupVisible)
                render();
            return 0;
        case WM_THEMECHANGED:
            releaseDrawingResources();
            if (popupVisible)
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
            killTimers();
            releaseDrawingResources();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

MediaPopup::MediaPopup() : impl_(std::make_unique<Impl>()) {}

MediaPopup::~MediaPopup() {
    destroy();
}

bool MediaPopup::create(HINSTANCE inst, HWND anchor) {
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
    impl_->dpi = GetDpiForWindow(impl_->hwnd);
    if (impl_->dpi == 0)
        impl_->dpi = GetDpiForSystem();
    impl_->reposition();
    return true;
}

void MediaPopup::destroy() {
    if (!impl_)
        return;
    impl_->hideImmediate();
    if (impl_->hwnd)
        DestroyWindow(impl_->hwnd);
    impl_->releaseAll();
}

void MediaPopup::setControlCallback(std::function<void(MediaControl)> cb) {
    impl_->onControl = std::move(cb);
}

void MediaPopup::setAppVolume(const AppVolumeState& state) {
    const bool changed = state.available != impl_->volume.available ||
                         state.percent != impl_->volume.percent ||
                         state.muted != impl_->volume.muted;
    impl_->volume = state;
    if (!state.available && impl_->volumeSliderOn) {
        // 会话消失：直接收起滑块，不走过渡动画
        impl_->volumeSliderOn = false;
        impl_->volumeSliderT = 0.0f;
        impl_->volumeDragging = false;
        if (impl_->hwnd)
            KillTimer(impl_->hwnd, kVolumeTimer);
    }
    if (changed && impl_->popupVisible)
        impl_->renderOrDefer();
}

void MediaPopup::setAppVolumeCallback(std::function<void(int)> cb) {
    impl_->onAppVolume = std::move(cb);
}

void MediaPopup::setSourceOpenCallback(std::function<void(const std::wstring&)> cb) {
    impl_->onSourceOpen = std::move(cb);
}

void MediaPopup::setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb) {
    impl_->onIdleAppOpen = std::move(cb);
}

void MediaPopup::setPanelOpenedCallback(std::function<void()> cb) {
    impl_->onPanelOpened = std::move(cb);
}

void MediaPopup::setEnabled(bool enabled) {
    impl_->enabled = enabled;
    if (!enabled) {
        impl_->hideImmediate();
        // 媒体卡片是极简模式明确关闭的附加窗口；禁用时连同 DComp/D2D 资源一起释放，
        // 下次重新启用时由现有 bind/render 路径惰性重建。
        impl_->releaseAll();
        return;
    }
    if (!impl_->hwnd)
        return;
    if (impl_->opensOnHover() && impl_->anchorHover && impl_->available &&
        !impl_->popupVisible)
        SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
}

void MediaPopup::setTriggerOnHover(bool on) {
    if (impl_->triggerOnHover == on)
        return;
    impl_->triggerOnHover = on;
    if (!impl_->hwnd)
        return;
    if (!impl_->opensOnHover()) {
        KillTimer(impl_->hwnd, kShowTimer);
    } else if (impl_->enabled && impl_->anchorHover && impl_->available &&
               !impl_->popupVisible) {
        SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
    }
}

void MediaPopup::setBackgroundMode(MediaPopupBackground mode) {
    if (impl_->backgroundMode == mode)
        return;
    impl_->backgroundMode = mode;
    if (mode != MediaPopupBackground::Frosted)
        impl_->releaseDynamicBackgroundResources();
    if (!impl_->hwnd)
        return;
    impl_->releaseDrawingResources();
    if (!impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setBackgroundColor(COLORREF color, bool customized) {
    if (impl_->floatingCardBackgroundColor == color &&
        impl_->floatingCardBackgroundColorCustomized == customized)
        return;
    impl_->floatingCardBackgroundColor = color;
    impl_->floatingCardBackgroundColorCustomized = customized;
    if (!impl_->hwnd)
        return;
    impl_->releaseDrawingResources();
    if (!impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setFollowAlbumBackground(bool on) {
    if (impl_->followAlbumBackground == on)
        return;
    impl_->followAlbumBackground = on;
    if (!impl_->dynamicBackgroundEnabled())
        impl_->releaseDynamicBackgroundResources();
    else
        impl_->dynamicBackgroundDirty = true;
    if (!impl_->hwnd || !impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setAutoTextContrast(bool on) {
    if (impl_->autoTextContrast == on)
        return;
    impl_->autoTextContrast = on;
    if (on)
        impl_->backdropDirty = true;
    else
        impl_->resetBackdropTextColors();
    if (!impl_->hwnd || !impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::refreshTheme() {
    impl_->releaseDrawingResources();
    if (!impl_->hwnd || !impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setMedia(const OverlayMediaInfo& info, bool available,
                          bool animateSongTransition) {
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
    impl_->songTransitionPending = animateSongTransition && changed && impl_->popupVisible &&
                                   !impl_->entering && !impl_->closing &&
                                   impl_->clientAnimations && !impl_->idleMode &&
                                   !impl_->categoryTransitionActive;
    if (changed && impl_->popupVisible &&
        !impl_->categoryTransitionActive && !impl_->quickExpandTransitionActive &&
        (!impl_->idleMode || dominantColorChanged)) {
        if (impl_->entering || impl_->closing)
            impl_->deferredRender = true;
        else
            impl_->render();
    }
    if (impl_->hwnd && impl_->anchorHover && impl_->enabled && !impl_->popupVisible &&
        impl_->opensOnHover())
        SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
}

void MediaPopup::setIdleContent(const IdlePresentation& content, bool available) {
    const bool idleAvailabilityChanged =
        content.quickStartEnabled != impl_->idle.quickStartEnabled;
    const bool quoteContentChanged = content.sentence != impl_->idle.sentence ||
                                     content.loading != impl_->idle.loading ||
                                     content.source != impl_->idle.source;
    const bool quoteChanged = quoteContentChanged ||
                              content.showQuote != impl_->idle.showQuote ||
                              content.copyEnabled != impl_->idle.copyEnabled ||
                              content.quickStartEnabled != impl_->idle.quickStartEnabled;
    bool changed = quoteChanged ||
                   content.showAppNames != impl_->idle.showAppNames ||
                   content.apps.size() != impl_->idle.apps.size() ||
                   content.todayTasks.size() != impl_->idle.todayTasks.size() ||
                   content.todayTasksLoading != impl_->idle.todayTasksLoading ||
                   content.todayTasksConnected != impl_->idle.todayTasksConnected ||
                   content.todayTasksStatus != impl_->idle.todayTasksStatus;
    if (!changed) {
        for (size_t i = 0; i < content.apps.size(); ++i) {
            const auto& oldApp = impl_->idle.apps[i];
            const auto& newApp = content.apps[i];
            if (oldApp.path != newApp.path || oldApp.customName != newApp.customName ||
                oldApp.name != newApp.name ||
                oldApp.iconPixels != newApp.iconPixels ||
                oldApp.iconWidth != newApp.iconWidth || oldApp.iconHeight != newApp.iconHeight ||
                oldApp.pathValid != newApp.pathValid) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        for (size_t i = 0; i < content.todayTasks.size(); ++i) {
            const auto& oldTask = impl_->idle.todayTasks[i];
            const auto& newTask = content.todayTasks[i];
            if (oldTask.id != newTask.id || oldTask.title != newTask.title ||
                oldTask.dueText != newTask.dueText || oldTask.completed != newTask.completed ||
                oldTask.overdue != newTask.overdue || oldTask.priority != newTask.priority) {
                changed = true;
                break;
            }
        }
    }

    if (quoteContentChanged || !content.showQuote || !content.copyEnabled ||
        content.loading || content.sentence.empty()) {
        if (impl_->hwnd)
            KillTimer(impl_->hwnd, kCopyFeedbackTimer);
        impl_->copySucceeded = false;
    }

    if (quoteContentChanged) {
        const bool canAnimateQuote = impl_->idleMode &&
                                     impl_->popupVisible && impl_->clientAnimations &&
                                     !impl_->entering && !impl_->closing &&
                                     !impl_->categoryTransitionActive;
        if (canAnimateQuote) {
            if (!impl_->idleContentTransitionActive) {
                impl_->idleContentTransitionFrom = impl_->idle;
                impl_->idleContentTransitionStartMs = GetTickCount64();
            }
            impl_->idleContentTransitionActive = true;
        } else {
            impl_->idleContentTransitionActive = false;
        }
    }
    impl_->idle = content;
    impl_->available = available;
    if (!content.quickStartEnabled) {
        impl_->idleQuickExpanded = false;
        impl_->idleQuickExpandOpening = false;
        impl_->idleQuickExpandT = 0.0f;
        impl_->stopQuickExpandTransition();
    }
    if (changed) {
        if (quoteChanged) {
            impl_->idleQuoteScrollOffset = 0.0f;
            impl_->scrollTickMs = 0;
        }
        impl_->idleTextDirty = true;
        impl_->idleIconsDirty = true;
        if (impl_->popupVisible && impl_->idleMode)
            impl_->reposition();
    }
    if (!available) {
        impl_->hideImmediate();
        return;
    }
    if (changed && impl_->popupVisible &&
        !impl_->categoryTransitionActive && !impl_->quickExpandTransitionActive &&
        (impl_->idleMode || idleAvailabilityChanged)) {
        if (impl_->entering || impl_->closing)
            impl_->deferredRender = true;
        else
            impl_->render();
    }
    if (impl_->hwnd && impl_->anchorHover && impl_->enabled && !impl_->popupVisible &&
        impl_->opensOnHover())
        SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
}

void MediaPopup::beginPresentationUpdate() {
    impl_->beginPresentationUpdate();
}

void MediaPopup::endPresentationUpdate() {
    impl_->endPresentationUpdate();
}

void MediaPopup::setPresentationMode(DisplayScene scene, bool available, bool inlineControls) {
    impl_->inlineControls = inlineControls;

    if (!available) {
        impl_->available = false;
        impl_->playbackScene = false;
        impl_->idlePanelManual = false;
        impl_->categoryTransitionActive = false;
        impl_->idleContentTransitionActive = false;
        impl_->hideImmediate();
        return;
    }

    impl_->available = true;
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
}

void MediaPopup::setProgress(int64_t positionMs) {
    const int64_t nextPositionMs = std::max<int64_t>(positionMs, 0);
    // 层转场期间新页内容承载在静态合成层快照上：进度只挂起不应用，
    // 由 finishLayerTransition 在撤层交接后再落到 positionMs 并重绘。
    if (impl_->categoryTransitionActive || impl_->quickExpandTransitionActive) {
        impl_->deferredPositionMs = nextPositionMs;
        return;
    }
    if (impl_->positionMs == nextPositionMs)
        return;
    impl_->positionMs = nextPositionMs;
    if (impl_->popupVisible)
        impl_->renderOrDefer();
}

void MediaPopup::setAnchor(HWND anchor) {
    impl_->anchor = anchor;
    impl_->reposition();
}

void MediaPopup::onAnchorEnter() {
    impl_->onAnchorEnter();
}

void MediaPopup::onAnchorLeave() {
    impl_->onAnchorLeave();
}

void MediaPopup::onAnchorClick() {
    impl_->onAnchorClick();
}

void MediaPopup::hideImmediate() {
    impl_->hideImmediate();
}
