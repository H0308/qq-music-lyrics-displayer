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
constexpr UINT kShowDelayMs = 100;
constexpr UINT kHideDelayMs = 180;
constexpr UINT kOpenAnimationMs = 180;
constexpr UINT kCloseAnimationMs = 140;
constexpr float kSongTransitionMs = 220.0f;
constexpr float kSongTransitionTravelDip = 24.0f;
constexpr float kCategoryTransitionMs = 240.0f;
constexpr float kInnerContentTransitionMs = 220.0f;
constexpr float kIdleQuickExpandMs = 160.0f;
// 根卡片位移由 DirectComposition 按显示器刷新率执行；只有长文本内容需要
// 重绘，30fps 已足够平滑，也避免与任务栏歌词的高频提交长期争用 UI 线程。
constexpr UINT kScrollTimerMs = 32;
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
constexpr float kIdleListHeightDip = 132.0f;
constexpr float kIdleListExpandedHeightDip = 160.0f;
constexpr float kIdleListRowHeightDip = 40.0f;
constexpr float kIdleListGapDip = 8.0f;
constexpr float kIdleListBottomPaddingDip = 8.0f;
constexpr float kIdleQuickExpandedTopDip = 15.0f;
constexpr float kIdleQuickTitleLeftDip = 16.0f;
constexpr float kIdleQuickTitleWidthDip = 52.0f;
constexpr float kIdleQuickTitleButtonGapDip = 8.0f;
constexpr float kIdleQuickTriggerHorizontalPaddingDip = 8.0f;
constexpr float kIdleQuickTriggerVerticalPaddingDip = 4.0f;
constexpr float kIdleScrollBarWidthDip = 3.0f;
constexpr float kIdleScrollBarHitWidthDip = 12.0f;
// d2d1effects.h 只声明这个 GUID；当前工程的链接配置不提供其外部定义，
// 这里保留 Direct2D 标准 Gaussian Blur CLSID 的内部定义。
constexpr CLSID kGaussianBlurClsid = {
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

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
    bool idleTriggerOnHover = true;
    bool anchorHover = false;
    bool popupHover = false;
    bool popupVisible = false;
    bool closing = false;
    bool entering = false;
    bool deferredRender = false;
    bool placedAbove = true;
    bool themeDirty = true;
    MediaPopupBackground backgroundMode = MediaPopupBackground::Solid;
    MediaPopupBackground idleBackgroundMode = MediaPopupBackground::Solid;
    COLORREF idleBackgroundColor = RGB(255, 255, 255);
    bool idleBackgroundColorCustomized = false;
    bool idleFollowAlbumBackground = false;
    bool followAlbumBackground = false;
    bool autoTextContrast = false;
    bool materialNeedsApply = true;
    bool backdropDirty = true;
    bool preserveBackdropOnNextResourceCreate = false;
    // 最近一次背景采样得到的亮度（-1 表示无效）。页面切换时背景位图被保留，
    // 画笔重建后直接用它恢复文字对比色，无需再次隐藏窗口抓屏。
    float lastBackdropLuminance = -1.0f;
    bool coverDirty = true;
    bool sourceIconDirty = true;
    bool textDirty = true;
    bool scrollTimerRunning = false;
    bool clientAnimations = true;
    bool songTransitionPending = false;
    bool categoryTransitionActive = false;
    PopupPage categoryTransitionFrom = PopupPage::Media;
    IdlePresentation categoryTransitionIdle;
    OverlayMediaInfo categoryTransitionMedia;
    ULONGLONG categoryTransitionStartMs = 0;
    int categoryTransitionDirection = 1;
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
    std::function<void(MediaControl)> onControl;
    std::function<void(const std::wstring&)> onSourceOpen;
    std::function<void(const std::wstring&)> onIdleAppOpen;

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
    float idleScrollOffset = 0.0f;
    float idleScrollMax = 0.0f;
    int hoverIdleApp = -1;
    int pressedIdleApp = -1;
    bool hoverIdleQuickExpand = false;
    bool pressedIdleQuickExpand = false;
    bool idleScrollDragging = false;
    float idleScrollDragOffset = 0.0f;
    D2D1_RECT_F idleListRect{};
    D2D1_RECT_F idleScrollTrackRect{};
    D2D1_RECT_F idleScrollThumbRect{};
    D2D1_RECT_F idleQuickExpandRect{};
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
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* brushAccentHover = nullptr;
    ID2D1SolidColorBrush* brushTextOnAccent = nullptr;
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
        return idleMode ? idleTriggerOnHover : triggerOnHover;
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

    float categoryTransitionProgress() const {
        if (!categoryTransitionActive || categoryTransitionStartMs == 0)
            return 1.0f;
        const ULONGLONG now = GetTickCount64();
        const float linear = std::clamp(
            static_cast<float>(now >= categoryTransitionStartMs
                                   ? now - categoryTransitionStartMs
                                   : 0) /
                kCategoryTransitionMs,
            0.0f, 1.0f);
        return linear * linear * (3.0f - 2.0f * linear);
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

    D2D1_RECT_F idleQuickExpandLocalRect(float headerTop) const {
        const float left = kIdleQuickTitleLeftDip + kIdleQuickTitleWidthDip +
                           kIdleQuickTitleButtonGapDip;
        const float top = idleQuickExpandButtonTop(headerTop);
        return D2D1::RectF(left, top, left + 32.0f, top + 32.0f);
    }

    D2D1_RECT_F idleQuickTriggerLocalRect(float headerTop) const {
        const D2D1_RECT_F button = idleQuickExpandLocalRect(headerTop);
        return D2D1::RectF(
            kIdleQuickTitleLeftDip - kIdleQuickTriggerHorizontalPaddingDip,
            button.top - kIdleQuickTriggerVerticalPaddingDip,
            button.right + kIdleQuickTriggerHorizontalPaddingDip,
            button.bottom + kIdleQuickTriggerVerticalPaddingDip);
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

    MediaPopupBackground activeBackgroundMode() const {
        return idleMode ? idleBackgroundMode : backgroundMode;
    }

    bool frostedBackgroundActive() const {
        return activeBackgroundMode() == MediaPopupBackground::Frosted;
    }

    bool dynamicBackgroundEnabled() const {
        if (idleMode) {
            // 空闲入口只有在仍有播放场景且已提取专辑色时才切换，纯无播放入口
            // 保留用户设置的磨砂颜色。
            return idleFollowAlbumBackground && playbackScene && media.hasDominantColor &&
                   idleBackgroundMode == MediaPopupBackground::Frosted;
        }
        return followAlbumBackground && backgroundMode == MediaPopupBackground::Frosted;
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

    void applyBackdropTextContrast(float luminance) {
        // 无播放组合面板没有单独的字体颜色开关，磨砂模式始终根据后方内容
        // 选择黑/白文字；播放中的媒体卡片仍由设置项控制。
        if (!idleMode && !autoTextContrast) {
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
        scrollTimerRunning = false;
        entering = false;
        deferredRender = false;
    }

    void releaseDrawingResources() {
        preserveBackdropOnNextResourceCreate = false;
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

    void releaseVisualResources(bool preserveBackdrop = false) {
        releaseBitmap(coverBmp);
        releaseBitmap(sourceIconBmp);
        for (auto*& bitmap : idleIconBitmaps)
            releaseBitmap(bitmap);
        idleIconBitmaps.clear();
        idleIconsDirty = true;
        if (!preserveBackdrop) {
            releaseBitmap(backdropBmp);
            releaseCom(backdropBlur);
            lastBackdropLuminance = -1.0f;
        }
        sourceIconDirty = true;
        if (!preserveBackdrop)
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
        releaseBrush(brushAccent);
        releaseBrush(brushAccentHover);
        releaseBrush(brushTextOnAccent);
        releaseDynamicBackgroundResources();
        releaseFormat(fmtSource);
        releaseFormat(fmtTimeRight);
        releaseFormat(fmtTitle);
        releaseFormat(fmtArtist);
        releaseFormat(fmtIcon);
        releaseFormat(fmtIdleHeader);
        releaseFormat(fmtIdleQuote);
        releaseFormat(fmtIdleSource);
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
        // 几何图层会随页面资源一起重建；只保留背景快照和模糊效果，
        // 避免在 createResources() 中覆盖仍然存活的几何对象。
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
        const bool preserveBackdrop = preserveBackdropOnNextResourceCreate;
        preserveBackdropOnNextResourceCreate = false;
        releaseVisualResources(preserveBackdrop);
        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;

        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        const bool dark = fluent::isDarkMode(fluent::ThemeTarget::Window);
        D2D1_COLOR_F cardFill = p.cardFillSolid;
        if (idleMode) {
            // 无播放组合面板的纯色模式只跟随窗口深浅色，颜色设置只参与磨砂 tint。
            cardFill = dark ? D2D1::ColorF(D2D1::ColorF::Black)
                            : D2D1::ColorF(D2D1::ColorF::White);
            if (frostedBackgroundActive()) {
                const float tintAlpha = dark ? 0.10f : 0.16f;
                if (dynamicBackgroundEnabled()) {
                    cardFill = p.cardFill;
                    cardFill.a = tintAlpha;
                } else {
                    cardFill = idleBackgroundColorCustomized
                                   ? fluent::toD2D(idleBackgroundColor, tintAlpha)
                                   : p.cardFill;
                }
            }
        } else if (frostedBackgroundActive()) {
            // 保持播放中的音乐控件卡片原有磨砂逻辑；无播放组合面板的自定义颜色
            // 不参与这里的绘制。
            cardFill = p.cardFill;
            cardFill.a = dark ? 0.10f : 0.16f;
        }
        if (FAILED(rt->CreateSolidColorBrush(cardFill, &brushBackground)) ||
            FAILED(rt->CreateSolidColorBrush(p.cardStroke, &brushStroke)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.disabled, &brushDisabled)) ||
            FAILED(rt->CreateSolidColorBrush(p.separator, &brushProgressTrack)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlFill, &brushControl)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlHover, &brushControlHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlPressed, &brushControlPressed)) ||
            FAILED(rt->CreateSolidColorBrush(p.accent, &brushAccent)) ||
            FAILED(rt->CreateSolidColorBrush(p.accentHover, &brushAccentHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.textOnAccent, &brushTextOnAccent)) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtSource) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtTimeRight) ||
            !createTextFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtTitle) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtArtist) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtIcon) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtIdleHeader) ||
            !createTextFormat(14.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtIdleQuote,
                               DWRITE_PARAGRAPH_ALIGNMENT_NEAR) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtIdleSource) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtIdleApp)) {
            releaseDrawingResources();
            return false;
        }
        if (FAILED(fmtTimeRight->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING)) ||
            FAILED(fmtIdleQuote->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING))) {
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

        // 画笔按调色板重建后，如果背景快照被保留（页面互切场景），沿用之前
        // 采样的亮度恢复文字对比色，避免为重新采样而隐藏窗口造成可见闪烁。
        if (backdropBmp && lastBackdropLuminance >= 0.0f)
            applyBackdropTextContrast(lastBackdropLuminance);
        themeDirty = false;
        return true;
    }

    bool createDynamicBackgroundResources() {
        if (!dynamicBackgroundEnabled())
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

    bool captureBackdrop() {
        if (!backdropDirty)
            return true;
        backdropDirty = false;
        releaseCom(backdropBlur);
        releaseBitmap(backdropBmp);
        resetBackdropTextColors();
        if (!frostedBackgroundActive() || !hwnd)
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
            categoryTransitionActive = false;
            categoryHoverEnvelopeActive = false;
            categoryHoverEnvelope = {};
            idleContentTransitionActive = false;
            idleQuickExpandT = idleQuickExpanded ? 1.0f : 0.0f;
            idleQuickExpandOpening = false;
            if (hwnd)
                KillTimer(hwnd, kIdleQuickExpandTimer);
        }
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

        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle || !fmtArtist)
            return;

        auto build = [&](const std::wstring& text, IDWriteTextFormat* format,
                         IDWriteTextLayout** layout, float& width, float& height) {
            if (text.empty())
                return;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                format, 100000.0f, 40.0f, layout)) ||
                !*layout)
                return;
            // CreateTextLayout 的高度是 40 DIP；若继续沿用格式的居中段落，
            // DrawTextLayout 外层再做一次垂直居中会把文字推到裁剪区域之外。
            (*layout)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED((*layout)->GetMetrics(&metrics))) {
                width = metrics.width;
                height = metrics.height;
            }
        };
        build(media.title, fmtTitle, &titleLayout, titleWidth, titleHeight);
        build(media.artist, fmtArtist, &artistLayout, artistWidth, artistHeight);
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

        if (FAILED(renderer.dwrite()->CreateTextLayout(
                idle.sentence.c_str(), static_cast<UINT32>(idle.sentence.size()), fmtIdleQuote,
                100000.0f, 42.0f, &idleQuoteLayout)) ||
            !idleQuoteLayout)
            return;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(idleQuoteLayout->GetMetrics(&metrics))) {
            idleQuoteWidth = metrics.width;
            idleQuoteHeight = std::min(42.0f, metrics.height);
        }
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
        // 组合卡片高度固定为媒体卡片高度；应用数量较多时只在卡片内部滚动，
        // 不允许列表绘制或命中区域越过圆角卡片底部。
        const IdlePresentation& listContent = content ? *content : idle;
        const float expandProgress = idleQuickExpandProgress(listContent);
        const float maxListHeight =
            kIdleListHeightDip +
            (kIdleListExpandedHeightDip - kIdleListHeightDip) * expandProgress;
        const float availableHeight =
            std::max(0.0f, kIdlePopupHeightDip - top - kIdleListBottomPaddingDip);
        const float listHeight = std::min(maxListHeight, availableHeight);
        const auto& apps = listContent.apps;
        idleListRect = D2D1::RectF(16.0f, top,
                                   w - 16.0f - (idleScrollMax > 0.0f
                                                    ? kIdleScrollBarHitWidthDip
                                                    : 0.0f),
                                   top + listHeight);
        const float contentHeight =
            apps.empty() ? 0.0f : apps.size() * kIdleListRowHeightDip;
        idleScrollMax = std::max(0.0f, contentHeight - listHeight);
        if (idleScrollOffset > idleScrollMax)
            idleScrollOffset = idleScrollMax;

        idleListRect.right = w - 16.0f -
                             (idleScrollMax > 0.0f ? kIdleScrollBarHitWidthDip : 0.0f);
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
        const bool shouldRun = popupVisible && !entering && !closing && enabled &&
                               clientAnimations &&
                               (categoryTransitionActive || idleContentTransitionActive ||
                                (idleMode && idleOverflow) ||
                                (!idleMode && available && media.playing && mediaOverflow));
        if (shouldRun) {
            if (!scrollTimerRunning) {
                scrollTickMs = GetTickCount64();
                SetTimer(hwnd, kScrollTimer, kScrollTimerMs, nullptr);
                scrollTimerRunning = true;
            }
            return;
        }
        if (scrollTimerRunning) {
            KillTimer(hwnd, kScrollTimer);
            scrollTimerRunning = false;
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
        releaseBitmap(sourceIconBmp);
        auto* rt = renderer.renderTarget();
        if (!rt || media.sourceAppUserModelId.empty())
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
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4,
                                       &props, &decoded)))
            sourceIconBmp = decoded;
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

    void drawCopyButton(ID2D1DeviceContext* rt, const D2D1_RECT_F& rect, bool hovered,
                        bool pressed) {
        if (!rt)
            return;
        if (hovered || pressed)
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(rect, 8.0f, 8.0f),
                pressed ? brushControlPressed : brushControlHover);

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
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

    void drawIdleQuoteUnit(ID2D1DeviceContext* rt, float w, const IdlePresentation& content,
                           bool current) {
        if (!rt)
            return;
        const float quoteTop = 40.0f;
        const float quoteHeight = idleUnitHeight(content, current);
        const D2D1_RECT_F quoteRect =
            D2D1::RectF(16.0f, quoteTop, w - 16.0f, quoteTop + quoteHeight);
        if (current && idleQuoteLayout && content.sentence == idle.sentence) {
            drawScrollingText(rt, idleQuoteLayout, idleQuoteWidth, idleQuoteHeight, quoteRect,
                              idleQuoteScrollOffset, brushText);
        } else if (!content.sentence.empty()) {
            drawText(rt, content.sentence, fmtIdleQuote, quoteRect,
                     content.loading ? brushSecondary : brushText);
        } else if (content.loading) {
            drawText(rt, L"正在获取每日一言…", fmtIdleQuote, quoteRect, brushSecondary);
        } else if (content.showQuote) {
            drawText(rt, L"暂时无法获取每日一言", fmtIdleQuote, quoteRect, brushDisabled);
        }

        const float sourceTop = quoteTop + quoteHeight + 8.0f;
        if (!content.source.empty())
            drawText(rt, content.source, fmtIdleSource,
                     D2D1::RectF(16.0f, sourceTop, w - 16.0f, sourceTop + 18.0f),
                     brushSecondary);
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
        const float copyRight = idleReturnArrowVisible(content) ? w - 52.0f : w - 16.0f;
        const D2D1_RECT_F localCopy =
            D2D1::RectF(copyRight - 32.0f, 7.0f, copyRight, 39.0f);
        drawCopyButton(rt, localCopy, updateHitTest && hoverCopy,
                       updateHitTest && pressedCopy);
        if (updateHitTest)
            copyRect = D2D1::RectF(localCopy.left, cardOriginDip + localCopy.top,
                                   localCopy.right, cardOriginDip + localCopy.bottom);
    }

    void drawIdleQuickExpandButton(ID2D1DeviceContext* rt, float headerTop,
                                   const IdlePresentation& content, bool updateHitTest) {
        if (!rt || !content.quickStartEnabled) {
            if (updateHitTest)
                idleQuickExpandRect = {};
            return;
        }

        const D2D1_RECT_F local = idleQuickExpandLocalRect(headerTop);
        const D2D1_RECT_F trigger = idleQuickTriggerLocalRect(headerTop);
        if (updateHitTest) {
            idleQuickExpandRect = D2D1::RectF(
                trigger.left, cardOriginDip + trigger.top, trigger.right,
                cardOriginDip + trigger.bottom);
        }
        if (updateHitTest && (hoverIdleQuickExpand || pressedIdleQuickExpand)) {
            // 可见悬浮底与返回媒体按钮同为 32 DIP；命中区仍额外保留 4 DIP。
            const D2D1_RECT_F hoverRect =
                D2D1::RectF(trigger.left, local.top, trigger.right, local.bottom);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(hoverRect, 6.0f, 6.0f),
                pressedIdleQuickExpand ? brushControlPressed : brushControlHover);
        }

        // 收起状态提示向上展开，展开状态提示向下收起。
        drawVerticalChevron(
            rt,
            D2D1::Point2F((local.left + local.right) * 0.5f,
                          (local.top + local.bottom) * 0.5f),
            6.5f, brushSecondary, !idleQuickExpanded);
    }

    void drawIdleQuickList(ID2D1DeviceContext* rt, float w, float top,
                           const IdlePresentation& content) {
        if (!rt)
            return;
        layoutIdleList(w, top, &content);
        rt->PushAxisAlignedClip(idleListRect, D2D1_ANTIALIAS_MODE_ALIASED);
        if (content.apps.empty()) {
            drawText(rt, L"请在设置中添加应用", fmtIdleApp, idleListRect, brushDisabled);
        } else {
            for (size_t i = 0; i < content.apps.size(); ++i) {
                const float top = idleListRect.top +
                                  static_cast<float>(i) * kIdleListRowHeightDip -
                                  idleScrollOffset;
                const D2D1_RECT_F row = D2D1::RectF(
                    idleListRect.left, top, idleListRect.right, top + 36.0f);
                if (row.bottom < idleListRect.top || row.top > idleListRect.bottom)
                    continue;
                if (static_cast<int>(i) == hoverIdleApp)
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(row, 7.0f, 7.0f), brushControlHover);

                const D2D1_RECT_F iconRect =
                    D2D1::RectF(row.left + 8.0f, row.top + 6.0f, row.left + 32.0f,
                                row.top + 30.0f);
                if (i < idleIconBitmaps.size() && idleIconBitmaps[i])
                    rt->DrawBitmap(idleIconBitmaps[i], iconRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                const D2D1_RECT_F textRect =
                    D2D1::RectF(row.left + 42.0f, row.top, row.right - 20.0f, row.bottom);
                drawText(rt, content.apps[i].name.empty() ? L"未命名应用" : content.apps[i].name,
                         fmtIdleApp, textRect,
                         content.apps[i].pathValid ? brushText : brushDisabled);
                drawChevron(rt, D2D1::Point2F(row.right - 11.0f, row.top + 18.0f), 5.5f,
                            content.apps[i].pathValid ? brushSecondary : brushDisabled);
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
        const float quoteTop = 40.0f;
        const float quoteHeight = idleUnitHeight(idle, true);
        const float sourceTop = quoteTop + quoteHeight + 8.0f;
        const float collapsedQuickHeaderTop = sourceTop + 30.0f;
        const float quickHeaderTop = idleQuickHeaderTop(collapsedQuickHeaderTop, idle);
        const float quickHeaderBottom = quickHeaderTop + 16.0f;
        const float quickExpandButtonTop = idleQuickExpandButtonTop(quickHeaderTop);
        const float listTop = std::max(
            quickHeaderBottom + kIdleListGapDip,
            quickExpandButtonTop + 32.0f + kIdleListGapDip);

        // 快速启动区域从底部向上覆盖每日一言；先裁掉被覆盖的旧内容，
        // 这样在半透明卡片或动态背景下也不会残留文字。
        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, quickExpandButtonTop),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        drawText(rt, idle.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                 D2D1::RectF(16.0f, 14.0f, w - 16.0f, 36.0f), brushText);

        if (idleContentTransitionActive && !categoryTransitionActive) {
            const float progress = idleContentTransitionProgress();
            if (progress >= 1.0f) {
                idleContentTransitionActive = false;
                drawIdleQuoteUnit(rt, w, idle, true);
            } else {
                const float travel = w + 24.0f;
                const float oldOffset = -travel * progress;
                const float newOffset = travel * (1.0f - progress);
                const float transitionBottom = std::min(sourceTop + 18.0f,
                                                        quickExpandButtonTop);
                if (transitionBottom > 36.0f) {
                    rt->PushAxisAlignedClip(D2D1::RectF(8.0f, 36.0f, w - 8.0f,
                                                         transitionBottom),
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
            copyRect.bottom > cardOriginDip + quickExpandButtonTop)
            copyRect = {};

        drawIdleQuickDivider(rt, w, quickHeaderTop, idle);
        const D2D1_RECT_F expandButton =
            idleQuickExpandLocalRect(quickHeaderTop);
        const float quickHeaderRight = idle.quickStartEnabled
                                           ? expandButton.left - 8.0f
                                           : w - 16.0f;
        drawIdleQuickExpandButton(rt, quickHeaderTop, idle, updateHitTest);
        drawText(rt, L"快速打开", fmtIdleHeader,
                 D2D1::RectF(16.0f, quickHeaderTop, quickHeaderRight, quickHeaderBottom),
                 brushSecondary);
        drawIdleQuickList(rt, w, listTop, idle);
    }

    void drawMediaContent(ID2D1DeviceContext* rt, float w) {
        if (!rt)
            return;
        drawSource(rt);
        drawCover(rt);
        drawScrollingText(rt, titleLayout, titleWidth, titleHeight,
                          D2D1::RectF(kPopupTextLeftDip, 48.0f,
                                      w - kPopupTextRightPaddingDip, 76.0f),
                          titleScrollOffset, brushText);
        drawScrollingText(rt, artistLayout, artistWidth, artistHeight,
                          D2D1::RectF(kPopupTextLeftDip, 78.0f,
                                      w - kPopupTextRightPaddingDip, 102.0f),
                          artistScrollOffset, brushSecondary);
        drawProgress(rt, w);
        for (int i = 0; i < 3; ++i)
            drawButton(rt, i);
    }

    void drawMediaSnapshot(ID2D1DeviceContext* rt, float w,
                           const OverlayMediaInfo& snapshot) {
        if (!rt)
            return;
        // 类别转场开始前保存的媒体字段可能已经被最新 SMTC 帧替换；
        // 旧层只需要一次静态快照，不参与新的布局和滚动状态。
        if (!snapshot.sourceAppUserModelId.empty()) {
            const D2D1_RECT_F iconRect = D2D1::RectF(16.0f, 14.0f, 34.0f, 32.0f);
            rt->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 5.0f, 5.0f), brushAccent);
            drawText(rt, L"♪", fmtIcon, iconRect, brushTextOnAccent);
            drawText(rt, sourceLabel(snapshot.sourceAppUserModelId), fmtSource,
                     D2D1::RectF(42.0f, 12.0f, 220.0f, 34.0f), brushText);
        }
        drawCover(rt);
        drawText(rt, snapshot.title, fmtTitle,
                 D2D1::RectF(kPopupTextLeftDip, 48.0f,
                             w - kPopupTextRightPaddingDip, 76.0f),
                 brushText);
        drawText(rt, snapshot.artist, fmtArtist,
                 D2D1::RectF(kPopupTextLeftDip, 78.0f,
                             w - kPopupTextRightPaddingDip, 102.0f),
                 brushSecondary);

        const OverlayMediaInfo current = media;
        media = snapshot;
        drawProgress(rt, w);
        for (int i = 0; i < 3; ++i)
            drawButton(rt, i);
        media = current;
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
        if (updateHitTest && (hoverPageArrow || pressedPageArrow))
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(local, 8.0f, 8.0f),
                pressedPageArrow ? brushControlPressed : brushControlHover);
        drawChevron(rt,
                    D2D1::Point2F((local.left + local.right) * 0.5f,
                                  (local.top + local.bottom) * 0.5f),
                    7.0f, brushSecondary, page == PopupPage::Media);
    }

    void drawBackdrop(ID2D1DeviceContext* rt, float w, float h) {
        if (!frostedBackgroundActive() || (!backdropBlur && !backdropBmp))
            return;

        const bool clipped = backdropClip && backdropLayer;
        if (clipped) {
            rt->PushLayer(D2D1::LayerParameters1(
                              D2D1::InfiniteRect(), backdropClip,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                          backdropLayer);
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
    }

    void drawDynamicBackground(ID2D1DeviceContext* rt, float w, float h) {
        if (!dynamicBackgroundEnabled() || !rt || !brushDynamicGradient || !brushDynamicGlow)
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

    void drawCover(ID2D1DeviceContext* rt) {
        const D2D1_RECT_F rect = D2D1::RectF(16.0f, 44.0f, 16.0f + kCoverSizeDip,
                                             44.0f + kCoverSizeDip);
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, 10.0f, 10.0f), brushControl);
        if (coverBmp && coverClip && coverLayer) {
            rt->PushLayer(D2D1::LayerParameters1(
                              D2D1::InfiniteRect(), coverClip,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                              D2D1::Matrix3x2F::Translation(rect.left, rect.top)),
                          coverLayer);
            rt->DrawBitmap(coverBmp, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
            return;
        }
        drawText(rt, L"♪", fmtIcon, rect, brushSecondary);
    }

    void drawSource(ID2D1DeviceContext* rt) {
        const D2D1_RECT_F iconRect = D2D1::RectF(16.0f, 14.0f, 34.0f, 32.0f);
        if (sourceIconBmp) {
            rt->DrawBitmap(sourceIconBmp, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            rt->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 5.0f, 5.0f), brushAccent);
            drawText(rt, L"♪", fmtIcon, iconRect, brushTextOnAccent);
        }
        drawText(rt, sourceLabel(media.sourceAppUserModelId), fmtSource,
                 D2D1::RectF(42.0f, 12.0f, 200.0f, 34.0f), brushText);
    }

    void drawProgress(ID2D1DeviceContext* rt, float w) {
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
            media.durationMs > 0
                ? std::clamp(positionMs, int64_t{0}, media.durationMs)
                : std::max<int64_t>(positionMs, 0);
        if (media.durationMs > 0) {
            const float fraction = static_cast<float>(
                static_cast<double>(displayPositionMs) / static_cast<double>(media.durationMs));
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
        drawText(rt, formatPlaybackDuration(media.durationMs), fmtTimeRight,
                 D2D1::RectF(right - kTimeLabelWidth, timeTop, right, timeBottom),
                 brushSecondary);
    }

    bool buttonEnabled(int index) const {
        if (!available)
            return false;
        // 某些播放器的 SMTC 快照会把前后曲目能力标成 false，但对应的
        // TrySkipPrevious/Next 仍然是可用操作；媒体卡片保留明确的操作入口。
        return index == 1 ? media.canPlayPause : true;
    }

    void drawButton(ID2D1DeviceContext* rt, int index) {
        const bool enabled = buttonEnabled(index);
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
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(rect, 18.0f, 18.0f),
                pressedButton == index ? brushControlPressed : brushControlHover);
        }

        ID2D1Brush* iconBrush = enabled ? brushText : brushDisabled;
        if (index == 1 && enabled)
            iconBrush = brushTextOnAccent;
        const float radius = index == 1 ? 11.0f
                                        : (rect.bottom - rect.top) * 0.26f;
        media_control::draw(rt, controlGeometry, index, media.playing,
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
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(localIconRect, 16.0f, 16.0f),
                pressedVolume ? brushControlPressed : brushControlHover);
        }
        const float iconCx = (localIconRect.left + localIconRect.right) * 0.5f;
        const int level = !volume.available || volume.muted
                              ? 0
                              : volume.percent == 0 ? 1 : volume.percent < 50 ? 2 : 3;
        media_control::drawVolume(rt, D2D1::Point2F(iconCx, cy), 8.0f,
                                  volume.available ? brushText : brushDisabled, level);
    }

    void drawIdleSnapshot(ID2D1DeviceContext* rt, float w, const IdlePresentation& content) {
        const float quoteTop = 40.0f;
        const float quoteHeight = idleUnitHeight(content, false);
        const float sourceTop = quoteTop + quoteHeight + 8.0f;
        const float collapsedQuickHeaderTop = sourceTop + 30.0f;
        const float quickHeaderTop = idleQuickHeaderTop(collapsedQuickHeaderTop, content);
        const float quickHeaderBottom = quickHeaderTop + 16.0f;
        const float quickExpandButtonTop = idleQuickExpandButtonTop(quickHeaderTop);
        const float listTop = std::max(
            quickHeaderBottom + kIdleListGapDip,
            quickExpandButtonTop + 32.0f + kIdleListGapDip);

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, quickExpandButtonTop),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        drawText(rt, content.showQuote ? L"每日一言" : L"欢迎", fmtIdleHeader,
                 D2D1::RectF(16.0f, 14.0f, w - 16.0f, 36.0f), brushText);
        drawIdleQuoteUnit(rt, w, content, false);
        drawIdleCopyButton(rt, w, content, false);
        rt->PopAxisAlignedClip();

        drawIdleQuickDivider(rt, w, quickHeaderTop, content);
        const D2D1_RECT_F expandButton =
            idleQuickExpandLocalRect(quickHeaderTop);
        const float quickHeaderRight = content.quickStartEnabled
                                           ? expandButton.left - 8.0f
                                           : w - 16.0f;
        drawIdleQuickExpandButton(rt, quickHeaderTop, content, false);
        drawText(rt, L"快速打开", fmtIdleHeader,
                 D2D1::RectF(16.0f, quickHeaderTop, quickHeaderRight, quickHeaderBottom),
                 brushSecondary);
        drawIdleQuickList(rt, w, listTop, content);
    }

    void drawPageContent(ID2D1DeviceContext* rt, float w, PopupPage page, bool oldLayer,
                         bool updateHitTest) {
        if (page == PopupPage::Idle) {
            if (oldLayer && categoryTransitionFrom == PopupPage::Idle)
                drawIdleSnapshot(rt, w, categoryTransitionIdle);
            else
                drawIdle(rt, w, updateHitTest);
        } else {
            if (oldLayer && categoryTransitionFrom == PopupPage::Media)
                drawMediaSnapshot(rt, w, categoryTransitionMedia);
            else
                drawMediaContent(rt, w);
        }
    }

    void drawPageLayer(ID2D1DeviceContext* rt, float w, PopupPage page, bool oldLayer,
                       bool updateHitTest) {
        drawPageContent(rt, w, page, oldLayer, updateHitTest);
        if (page == PopupPage::Media)
            drawVolumeControl(rt, w, page, updateHitTest);
        drawPageArrow(rt, w, page, updateHitTest);
    }

    void drawCategoryContent(ID2D1DeviceContext* rt, float w, float cardH) {
        if (!categoryTransitionActive) {
            drawPageLayer(rt, w, currentPage(), false, true);
            return;
        }

        const float progress = categoryTransitionProgress();
        if (progress >= 1.0f) {
            categoryTransitionActive = false;
            drawPageLayer(rt, w, currentPage(), false, true);
            refreshPopupHoverAfterTransition();
            return;
        }

        const float travel = w;
        const float oldOffset = -static_cast<float>(categoryTransitionDirection) * travel * progress;
        const float newOffset = static_cast<float>(categoryTransitionDirection) * travel *
                                (1.0f - progress);
        const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(0.0f, cardOriginDip);
        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, cardH),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(oldOffset, cardOriginDip));
        drawPageLayer(rt, w, categoryTransitionFrom, true, false);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(newOffset, cardOriginDip));
        drawPageLayer(rt, w, currentPage(), false, false);
        rt->SetTransform(base);
        rt->PopAxisAlignedClip();
    }

    bool render() {
        if (!hwnd)
            return false;
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
        if (!createResources())
            return false;
        if (dynamicBackgroundEnabled())
            createDynamicBackgroundResources();
        if (frostedBackgroundActive())
            captureBackdrop();
        buildTextLayouts();
        if (coverDirty)
            decodeCover();
        if (sourceIconDirty)
            decodeSourceIcon();
        if (idleMode) {
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

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, cardOriginDip));
        drawBackdrop(rt, w, cardH);

        const D2D1_RECT_F card = D2D1::RectF(0.0f, 0.0f, w, cardH);
        rt->FillRoundedRectangle(D2D1::RoundedRect(card, kPopupCornerDip, kPopupCornerDip),
                                 brushBackground);
        drawDynamicBackground(rt, w, cardH);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(card, kPopupCornerDip, kPopupCornerDip),
                                 brushStroke, 1.0f);

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
        cardScreenX = x;
        cardScreenY = y;
        cardWidthPx = popupW;
        cardHeightPx = popupH;
        animationTravelPx = static_cast<float>(travel);
        cardOriginDip = dip(cardLocalY);
        SetWindowPos(hwnd, HWND_TOPMOST, x, hostY, popupW, travel,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    int hitButton(float x, float y) const {
        if (categoryTransitionActive)
            return -1;
        for (int i = 0; i < 3; ++i) {
            if (buttonEnabled(i) && contains(buttonRects[i], x, y))
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
        runtime_log::writef(L"[action][media-popup] idle-quote-copied");
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
        categoryTransitionActive = popupVisible && clientAnimations && !entering && !closing;
        if (categoryTransitionActive) {
            categoryTransitionFrom = from;
            categoryTransitionIdle = idleContentTransitionActive
                                         ? idleContentTransitionFrom
                                         : idle;
            categoryTransitionMedia = media;
            categoryTransitionStartMs = GetTickCount64();
            categoryTransitionDirection = categoryDirection(from, target);
        } else {
            categoryTransitionStartMs = 0;
        }
        idleContentTransitionActive = false;
        idleScrollOffset = 0.0f;
        idleQuoteScrollOffset = 0.0f;
        scrollTickMs = 0;
        // 类别转场优先于歌曲横向转场；后续完整帧仍会按最新歌曲状态继续更新。
        songTransitionPending = false;
        hoverPageArrow = false;
        pressedPageArrow = false;
        hoverCopy = false;
        pressedCopy = false;
        hoverIdleQuickExpand = false;
        pressedIdleQuickExpand = false;
        idleQuickExpandRect = {};
        if (hwnd)
            KillTimer(hwnd, kIdleQuickExpandTimer);
        idleMode = target != PopupPage::Media;
        // 页面互切只需要按新的页面状态重建画笔、文本和内容位图；保留
        // DirectComposition 的交换链与视觉树，避免切换期间出现透明空帧闪烁。
        preserveBackdropOnNextResourceCreate = true;
        themeDirty = true;
        coverDirty = true;
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

    int hitIdleApp(float x, float y) const {
        y -= cardOriginDip;
        if (categoryTransitionActive || !idleMode || idle.apps.empty() ||
            !contains(idleListRect, x, y))
            return -1;
        const float contentY = y - idleListRect.top + idleScrollOffset;
        const int index = static_cast<int>(contentY / kIdleListRowHeightDip);
        if (index < 0 || static_cast<size_t>(index) >= idle.apps.size())
            return -1;
        if (!idle.apps[static_cast<size_t>(index)].pathValid)
            return -1;
        const float rowOffset = std::fmod(contentY, kIdleListRowHeightDip);
        return rowOffset <= 36.0f ? index : -1;
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
        idleQuickExpanded = !idleQuickExpanded;
        if (!clientAnimations) {
            idleQuickExpandT = idleQuickExpanded ? 1.0f : 0.0f;
            idleQuickExpandOpening = false;
            KillTimer(hwnd, kIdleQuickExpandTimer);
            renderOrDefer();
            return;
        }
        idleQuickExpandOpening = idleQuickExpanded;
        SetTimer(hwnd, kIdleQuickExpandTimer, 16, nullptr);
        renderOrDefer();
    }

    void advanceIdleQuickExpand() {
        if (entering || closing || !popupVisible || !idleMode ||
            !idle.quickStartEnabled) {
            KillTimer(hwnd, kIdleQuickExpandTimer);
            return;
        }
        const float step = 16.0f / kIdleQuickExpandMs;
        idleQuickExpandT += idleQuickExpandOpening ? step : -step;
        if (idleQuickExpandT >= 1.0f) {
            idleQuickExpandT = 1.0f;
            KillTimer(hwnd, kIdleQuickExpandTimer);
        } else if (idleQuickExpandT <= 0.0f) {
            idleQuickExpandT = 0.0f;
            KillTimer(hwnd, kIdleQuickExpandTimer);
        }
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
        render();
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
        if (!hwnd || !popupVisible || anchorHover || popupHover || categoryTransitionActive)
            return;
        KillTimer(hwnd, kShowTimer);
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void renderOrDefer() {
        if (entering || closing) {
            deferredRender = true;
            return;
        }
        render();
    }

    void showPopup() {
        if (!hwnd || !enabled || !available)
            return;
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
        if (frostedBackgroundActive())
            backdropDirty = true;
        if (!render()) {
            entering = false;
            return;
        }

        // 窗口本身保持固定位置，只让 DirectComposition 根视觉做位移动画。
        // 透明度始终为 1，避免淡入淡出时出现第二层背板。
        renderer.resetRoot();
        const float fromY = placedAbove ? animationTravelPx : -animationTravelPx;
        if (!renderer.animateRoot(0.0f, 0.0f, fromY, 0.0f,
                                  1.0f, 1.0f,
                                  static_cast<float>(kOpenAnimationMs) / 1000.0f)) {
            renderer.resetRoot();
        }
        renderer.commit();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        popupVisible = true;
        runtime_log::writef(L"[action][media-popup] shown trigger=%s",
                            opensOnHover() ? L"hover" : L"click");
        SetTimer(hwnd, kEnterTimer, kOpenAnimationMs, nullptr);
    }

    void hideAnimated() {
        if (!popupVisible || closing || categoryTransitionActive)
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
        const float toY = placedAbove ? animationTravelPx : -animationTravelPx;
        if (!renderer.animateRoot(0.0f, 0.0f, 0.0f, toY,
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
        categoryTransitionActive = false;
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
        hoverPageArrow = false;
        pressedPageArrow = false;
        hoverCopy = false;
        pressedCopy = false;
        hoverIdleQuickExpand = false;
        pressedIdleQuickExpand = false;
        idleQuickExpandRect = {};
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
                if (idleApp != hoverIdleApp || arrow != hoverPageArrow || copy != hoverCopy ||
                    quickExpand != hoverIdleQuickExpand || hoverVolume) {
                    hoverIdleApp = idleApp;
                    hoverPageArrow = arrow;
                    hoverCopy = copy;
                    hoverIdleQuickExpand = quickExpand;
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
                if (categoryTransitionActive)
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
                advanceTextScroll();
                render();
            } else if (wp == kVolumeTimer) {
                if (entering || closing || !popupVisible) {
                    KillTimer(hwnd, kVolumeTimer);
                    return 0;
                }
                advanceVolumeSlider();
            } else if (wp == kIdleQuickExpandTimer) {
                advanceIdleQuickExpand();
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

void MediaPopup::setIdleTriggerOnHover(bool on) {
    if (impl_->idleTriggerOnHover == on)
        return;
    impl_->idleTriggerOnHover = on;
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
    if (!impl_->hwnd || impl_->idleMode)
        return;
    impl_->releaseDrawingResources();
    if (!impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setIdleBackgroundMode(MediaPopupBackground mode) {
    if (impl_->idleBackgroundMode == mode)
        return;
    impl_->idleBackgroundMode = mode;
    if (!impl_->hwnd || !impl_->idleMode)
        return;
    impl_->releaseDrawingResources();
    if (!impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setIdleBackgroundColor(COLORREF color, bool customized) {
    if (impl_->idleBackgroundColor == color &&
        impl_->idleBackgroundColorCustomized == customized)
        return;
    impl_->idleBackgroundColor = color;
    impl_->idleBackgroundColorCustomized = customized;
    if (!impl_->hwnd || !impl_->idleMode)
        return;
    impl_->releaseDrawingResources();
    if (!impl_->popupVisible)
        return;
    if (impl_->entering || impl_->closing)
        impl_->deferredRender = true;
    else
        impl_->render();
}

void MediaPopup::setIdleFollowAlbumBackground(bool on) {
    if (impl_->idleFollowAlbumBackground == on)
        return;
    impl_->idleFollowAlbumBackground = on;
    if (!impl_->dynamicBackgroundEnabled())
        impl_->releaseDynamicBackgroundResources();
    else
        impl_->dynamicBackgroundDirty = true;
    if (!impl_->hwnd || !impl_->popupVisible || !impl_->idleMode)
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
    if (!impl_->hwnd || !impl_->popupVisible || impl_->idleMode)
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
    else if (!impl_->idleMode)
        impl_->resetBackdropTextColors();
    if (!impl_->hwnd || !impl_->popupVisible || impl_->idleMode)
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
                   content.apps.size() != impl_->idle.apps.size();
    if (!changed) {
        for (size_t i = 0; i < content.apps.size(); ++i) {
            const auto& oldApp = impl_->idle.apps[i];
            const auto& newApp = content.apps[i];
            if (oldApp.path != newApp.path || oldApp.name != newApp.name ||
                oldApp.iconPixels != newApp.iconPixels ||
                oldApp.iconWidth != newApp.iconWidth || oldApp.iconHeight != newApp.iconHeight ||
                oldApp.pathValid != newApp.pathValid) {
                changed = true;
                break;
            }
        }
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
        if (impl_->hwnd)
            KillTimer(impl_->hwnd, kIdleQuickExpandTimer);
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
