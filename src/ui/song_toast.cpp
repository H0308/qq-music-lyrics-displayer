#include "song_toast.h"

#include "fluent_theme.h"
#include "lyric_renderer.h"
#include "platform_icon.h"

#include <d2d1effects.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr wchar_t kWndClassName[] = L"QQMusicLyricSongToast";
constexpr UINT_PTR kEnterTimer = 1;
constexpr UINT_PTR kAutoHideTimer = 2;
constexpr UINT_PTR kCloseTimer = 3;
constexpr UINT_PTR kScrollTimer = 4;
constexpr UINT kEnterAnimationMs = 240;
constexpr UINT kCloseAnimationMs = 180;
// 超长文字滚动：与媒体卡片一致约 30fps 重绘，位移由计时器步进
constexpr UINT kScrollTimerMs = 32;
constexpr float kScrollSpeedDip = 24.0f;

constexpr float kToastHeightDip = 48.0f;
constexpr float kToastWidthDip = 168.0f;
constexpr float kCoverSizeDip = 32.0f;
// 上下左右内边距一致，封面圆与浮窗胶囊左角同心，弧度衔接自然
constexpr float kCoverLeftDip = 8.0f;
constexpr float kCoverTopDip = 8.0f;
constexpr float kTextLeftDip = 50.0f;
constexpr float kTextRightPaddingDip = 16.0f;
constexpr float kTextGapDip = 2.0f;
// 封面右下角的来源应用角标：跨出封面边缘 2px，固定显示不提供开关
constexpr float kBadgeSizeDip = 14.0f;
constexpr float kBadgeOverhangDip = 2.0f;
// 滚动文本首尾相接时两份文字之间的间隔
constexpr float kTextScrollPaddingDip = 24.0f;
constexpr float kBottomMarginDip = 20.0f;
constexpr float kEnterTravelDip = 12.0f;
constexpr float kExitTravelDip = 6.0f;
// 胶囊外形：圆角恒为高度一半
constexpr float kTitleFontSize = 13.0f;
constexpr float kArtistFontSize = 11.5f;

// d2d1effects.h 只声明这个 GUID；与 media_popup.cpp 相同，这里保留内部定义。
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

template <typename T>
void releaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
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

struct SongToast::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT dpi = 96;

    bool enabled = false;
    int durationSec = 4;
    bool placementTop = false; // true 中上，false 中下
    bool visible = false;
    bool entering = false;
    bool closing = false;
    bool themeDirty = true;
    bool materialNeedsApply = true;
    bool backdropDirty = true;
    bool coverDirty = true;
    bool sourceIconDirty = true;
    bool textDirty = true;
    bool scrollTimerRunning = false;
    bool clientAnimations = true;

    OverlayMediaInfo media;
    int cardScreenX = 0;
    int cardScreenY = 0;
    int cardWidthPx = 0;
    int cardHeightPx = 0;

    DCompRenderer renderer;
    ID2D1SolidColorBrush* brushBackground = nullptr;
    ID2D1SolidColorBrush* brushStroke = nullptr;
    ID2D1SolidColorBrush* brushText = nullptr;
    ID2D1SolidColorBrush* brushSecondary = nullptr;
    ID2D1SolidColorBrush* brushControl = nullptr;

    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtArtist = nullptr;
    IDWriteTextLayout* titleLayout = nullptr;
    IDWriteTextLayout* artistLayout = nullptr;
    float titleWidth = 0.0f;
    float titleHeight = 0.0f;
    float artistWidth = 0.0f;
    float artistHeight = 0.0f;
    float titleScrollOffset = 0.0f;
    float artistScrollOffset = 0.0f;
    ULONGLONG scrollTickMs = 0;
    ID2D1Bitmap* coverBmp = nullptr;
    ID2D1Bitmap* sourceIconBmp = nullptr;
    ID2D1EllipseGeometry* coverClip = nullptr;
    ID2D1Layer* coverLayer = nullptr;
    ID2D1Bitmap* backdropBmp = nullptr;
    ID2D1Effect* backdropBlur = nullptr;

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

    void killTimers() {
        if (!hwnd)
            return;
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kAutoHideTimer);
        KillTimer(hwnd, kCloseTimer);
        KillTimer(hwnd, kScrollTimer);
        scrollTimerRunning = false;
        entering = false;
        closing = false;
    }

    void refreshClientAnimations() {
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        clientAnimations = animations != FALSE;
        if (!clientAnimations) {
            titleScrollOffset = 0.0f;
            artistScrollOffset = 0.0f;
            scrollTickMs = 0;
        }
    }

    float textAreaWidth() const {
        return std::max(1.0f, kToastWidthDip - kTextLeftDip - kTextRightPaddingDip);
    }

    void updateScrollTimer() {
        const float areaWidth = textAreaWidth();
        const bool overflow = titleWidth > areaWidth || artistWidth > areaWidth;
        const bool shouldRun = visible && !entering && !closing && enabled &&
                               clientAnimations && overflow;
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
        if (!overflow || !clientAnimations) {
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
        if (!clientAnimations)
            return;

        const float areaWidth = textAreaWidth();
        auto marquee = [&](float textWidth, float& offset) {
            if (textWidth <= areaWidth) {
                offset = 0.0f;
                return;
            }
            const float loopWidth = textWidth + kTextScrollPaddingDip * 2.0f;
            offset = std::fmod(offset + kScrollSpeedDip * std::max(dt, 0.0f), loopWidth);
            if (offset < 0.0f)
                offset += loopWidth;
        };
        marquee(titleWidth, titleScrollOffset);
        marquee(artistWidth, artistScrollOffset);
    }

    bool createTextFormats() {
        if (fmtTitle && fmtArtist)
            return true;
        auto* dw = renderer.dwrite();
        if (!dw)
            return false;
        auto create = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            if (FAILED(dw->CreateTextFormat(fluent::uiFontFamily(), nullptr, weight,
                                            DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"", out)) ||
                !*out)
                return false;
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            (*out)->SetTrimming(&trimming, nullptr);
            fluent::applyUiFontFallback(*out);
            return true;
        };
        return create(kTitleFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtTitle) &&
               create(kArtistFontSize, DWRITE_FONT_WEIGHT_NORMAL, &fmtArtist);
    }

    void releaseVisualResources() {
        releaseCom(coverBmp);
        releaseCom(sourceIconBmp);
        sourceIconDirty = true;
        releaseCom(backdropBmp);
        releaseCom(backdropBlur);
        backdropDirty = true;
        releaseCom(brushBackground);
        releaseCom(brushStroke);
        releaseCom(brushText);
        releaseCom(brushSecondary);
        releaseCom(brushControl);
        releaseCom(titleLayout);
        releaseCom(artistLayout);
        titleWidth = 0.0f;
        titleHeight = 0.0f;
        artistWidth = 0.0f;
        artistHeight = 0.0f;
        textDirty = true;
        releaseCom(coverClip);
        releaseCom(coverLayer);
    }

    void releaseDrawingResources() {
        releaseVisualResources();
        renderer.discard();
        themeDirty = true;
        coverDirty = true;
        materialNeedsApply = true;
    }

    void releaseAll() {
        releaseDrawingResources();
        renderer.releaseAll();
        releaseCom(fmtTitle);
        releaseCom(fmtArtist);
    }

    void applyWindowMaterial() {
        if (!hwnd)
            return;
        // 与媒体卡片一致：内容全部在 D2D 交换链里绘制，只关闭系统显示/隐藏过渡，
        // 位移动画由 DirectComposition 负责。
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions,
                              sizeof(disableTransitions));
    }

    bool createResources() {
        if (!themeDirty && brushBackground)
            return true;
        releaseVisualResources();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;

        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        // 截图模糊已经是弹窗底图，前景只保留一层很薄的 tint，避免再次变成灰蒙蒙。
        D2D1_COLOR_F cardFill = p.cardFill;
        cardFill.a = fluent::isDarkMode(fluent::ThemeTarget::Window) ? 0.10f : 0.16f;
        if (FAILED(rt->CreateSolidColorBrush(cardFill, &brushBackground)) ||
            FAILED(rt->CreateSolidColorBrush(p.cardStroke, &brushStroke)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlFill, &brushControl))) {
            releaseDrawingResources();
            return false;
        }

        auto* factory = renderer.d2d();
        if (!factory ||
            FAILED(factory->CreateEllipseGeometry(
                D2D1::Ellipse(D2D1::Point2F(kCoverSizeDip * 0.5f, kCoverSizeDip * 0.5f),
                              kCoverSizeDip * 0.5f, kCoverSizeDip * 0.5f),
                &coverClip)) ||
            FAILED(rt->CreateLayer(&coverLayer))) {
            releaseDrawingResources();
            return false;
        }

        themeDirty = false;
        return true;
    }

    // 文字颜色固定跟随背景明暗，不提供关闭选项。
    void applyBackdropTextContrast(float luminance) {
        const float blackContrast = (luminance + 0.05f) / 0.05f;
        const float whiteContrast = 1.05f / (luminance + 0.05f);
        const bool useBlack = blackContrast >= whiteContrast;
        const D2D1_COLOR_F text = useBlack ? D2D1::ColorF(D2D1::ColorF::Black)
                                           : D2D1::ColorF(D2D1::ColorF::White);
        const D2D1_COLOR_F secondary =
            useBlack ? D2D1::ColorF(D2D1::ColorF::Black, 0.70f)
                     : D2D1::ColorF(D2D1::ColorF::White, 0.72f);
        if (brushText)
            brushText->SetColor(text);
        if (brushSecondary)
            brushSecondary->SetColor(secondary);
    }

    bool captureBackdrop() {
        if (!backdropDirty)
            return true;
        backdropDirty = false;
        releaseCom(backdropBlur);
        releaseCom(backdropBmp);
        if (!hwnd)
            return true;

        const int width = cardWidthPx;
        const int height = cardHeightPx;
        if (width <= 0 || height <= 0)
            return false;

        // 窗口已可见时先临时隐藏再抓取，避免把自己拍进背景图。
        const bool restoreVisible = IsWindowVisible(hwnd) != FALSE;
        if (restoreVisible)
            ShowWindow(hwnd, SW_HIDE);
        DwmFlush();

        HDC screenDc = GetDC(nullptr);
        if (!screenDc) {
            if (restoreVisible)
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
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
                copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, cardScreenX,
                                cardScreenY, SRCCOPY | CAPTUREBLT) != FALSE;
        }
        if (oldBitmap && oldBitmap != HGDI_ERROR)
            SelectObject(memoryDc, oldBitmap);
        if (memoryDc)
            DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        if (restoreVisible)
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (!copied || !pixels) {
            if (dib)
                DeleteObject(dib);
            return false;
        }

        const float sampledLuminance = backdropLuminance(pixels, width, height);

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
                                    pixels, static_cast<UINT>(width * 4), &props,
                                    &captured)) ||
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
        titleScrollOffset = 0.0f;
        artistScrollOffset = 0.0f;
        scrollTickMs = 0;

        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle || !fmtArtist)
            return;

        auto build = [&](const std::wstring& text, IDWriteTextFormat* format,
                         IDWriteTextLayout** layout, float& width, float& height) {
            if (text.empty())
                return;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(),
                                                static_cast<UINT32>(text.size()), format,
                                                100000.0f, 64.0f, layout)) ||
                !*layout)
                return;
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

    // 固定宽度定位到主屏幕工作区中下方（不覆盖任务栏），超长文字截断。
    bool layoutAndPosition() {
        if (!hwnd)
            return false;
        buildTextLayouts();
        dpi = GetDpiForWindow(hwnd);
        if (dpi == 0)
            dpi = GetDpiForSystem();
        const float s = scale();
        const int widthPx = static_cast<int>(std::lround(kToastWidthDip * s));
        const int heightPx = static_cast<int>(std::lround(kToastHeightDip * s));
        const int margin = static_cast<int>(std::lround(kBottomMarginDip * s));

        HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return false;
        const RECT work = info.rcWork;
        const int workWidth = static_cast<int>(work.right - work.left);
        const int x = static_cast<int>(work.left) + (workWidth - widthPx) / 2;
        const int y = placementTop ? static_cast<int>(work.top) + margin
                                   : static_cast<int>(work.bottom) - margin - heightPx;

        cardScreenX = x;
        cardScreenY = y;
        cardWidthPx = widthPx;
        cardHeightPx = heightPx;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, widthPx, heightPx,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return true;
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
        releaseCom(sourceIconBmp);
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

    void drawText(ID2D1DeviceContext* rt, const std::wstring& text,
                  IDWriteTextFormat* format, const D2D1_RECT_F& rect, ID2D1Brush* brush) {
        if (text.empty() || !format || !brush)
            return;
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // 超出区域时首尾相接滚动；未超出时静止显示
    void drawScrollingText(ID2D1DeviceContext* rt, IDWriteTextLayout* layout,
                           float textWidth, float textHeight, const D2D1_RECT_F& rect,
                           float offset, ID2D1Brush* brush) {
        if (!rt || !layout || !brush)
            return;
        const float areaWidth = std::max(0.0f, rect.right - rect.left);
        const float y = rect.top;
        const bool scrolling = textWidth > areaWidth;
        const float loopWidth = textWidth + kTextScrollPaddingDip * 2.0f;
        const float bases[2] = {rect.left - offset, rect.left - offset + loopWidth};
        const int count = scrolling ? 2 : 1;

        rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < count; ++i) {
            const float x = scrolling ? bases[i] : rect.left;
            rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
        }
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
            applyWindowMaterial();
            materialNeedsApply = false;
        }
        renderer.setDpi(dpi);
        if (!createResources())
            return false;
        captureBackdrop();
        buildTextLayouts();
        if (coverDirty)
            decodeCover();
        if (sourceIconDirty)
            decodeSourceIcon();
        updateScrollTimer();

        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;
        const float w = kToastWidthDip;
        const float h = kToastHeightDip;
        const float radius = h * 0.5f;

        // 胶囊裁剪几何随宽度变化，按帧创建并在 EndDraw 后释放。
        ID2D1RoundedRectangleGeometry* capsule = nullptr;
        ID2D1Layer* capsuleLayer = nullptr;
        if (auto* factory = renderer.d2d()) {
            factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, w, h), radius, radius), &capsule);
        }
        if (capsule)
            rt->CreateLayer(&capsuleLayer);

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const bool clipped = capsule && capsuleLayer;
        if (clipped) {
            rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), capsule,
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                          capsuleLayer);
        }
        if (backdropBlur) {
            const D2D1_POINT_2F offset = D2D1::Point2F(0.0f, 0.0f);
            rt->DrawImage(backdropBlur, &offset, nullptr, D2D1_INTERPOLATION_MODE_LINEAR,
                          D2D1_COMPOSITE_MODE_SOURCE_OVER);
        } else if (backdropBmp) {
            rt->DrawBitmap(backdropBmp, D2D1::RectF(0.0f, 0.0f, w, h), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (clipped)
            rt->PopLayer();

        const auto card = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, w, h), radius, radius);
        rt->FillRoundedRectangle(card, brushBackground);
        rt->DrawRoundedRectangle(card, brushStroke, 1.0f);

        // 封面
        const D2D1_RECT_F coverRect =
            D2D1::RectF(kCoverLeftDip, kCoverTopDip, kCoverLeftDip + kCoverSizeDip,
                        kCoverTopDip + kCoverSizeDip);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((coverRect.left + coverRect.right) * 0.5f,
                                                    (coverRect.top + coverRect.bottom) * 0.5f),
                                      kCoverSizeDip * 0.5f, kCoverSizeDip * 0.5f),
                        brushControl);
        if (coverBmp && coverClip && coverLayer) {
            rt->PushLayer(D2D1::LayerParameters1(
                              D2D1::InfiniteRect(), coverClip,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                              D2D1::Matrix3x2F::Translation(coverRect.left, coverRect.top)),
                          coverLayer);
            rt->DrawBitmap(coverBmp, coverRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
        } else {
            drawText(rt, L"♪", fmtTitle, coverRect, brushSecondary);
        }

        // 封面右下角的来源应用角标
        if (sourceIconBmp) {
            const float badgeRight = coverRect.right + kBadgeOverhangDip;
            const float badgeBottom = coverRect.bottom + kBadgeOverhangDip;
            rt->DrawBitmap(sourceIconBmp,
                           D2D1::RectF(badgeRight - kBadgeSizeDip,
                                       badgeBottom - kBadgeSizeDip, badgeRight, badgeBottom),
                           1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        // 标题 + 艺术家：整体垂直居中；无艺术家时标题单独居中
        const float textRight = std::max(kTextLeftDip + 1.0f, w - kTextRightPaddingDip);
        float titleTop = 0.0f;
        float artistTop = 0.0f;
        if (artistHeight > 0.0f && titleHeight > 0.0f) {
            const float total = titleHeight + kTextGapDip + artistHeight;
            titleTop = std::max(0.0f, (h - total) * 0.5f);
            artistTop = titleTop + titleHeight + kTextGapDip;
        } else if (titleHeight > 0.0f) {
            titleTop = std::max(0.0f, (h - titleHeight) * 0.5f);
        } else if (artistHeight > 0.0f) {
            artistTop = std::max(0.0f, (h - artistHeight) * 0.5f);
        }
        if (titleLayout) {
            drawScrollingText(rt, titleLayout, titleWidth, titleHeight,
                              D2D1::RectF(kTextLeftDip, titleTop, textRight,
                                          titleTop + titleHeight),
                              titleScrollOffset, brushText);
        }
        if (artistLayout) {
            drawScrollingText(rt, artistLayout, artistWidth, artistHeight,
                              D2D1::RectF(kTextLeftDip, artistTop, textRight,
                                          artistTop + artistHeight),
                              artistScrollOffset, brushSecondary);
        }

        const HRESULT hr = rt->EndDraw();
        releaseCom(capsule);
        releaseCom(capsuleLayer);
        if (FAILED(hr)) {
            releaseDrawingResources();
            return false;
        }
        if (!renderer.present()) {
            releaseDrawingResources();
            return false;
        }
        return true;
    }

    void showSong() {
        if (!hwnd || !enabled || media.title.empty())
            return;
        killTimers();
        if (visible)
            hideImmediate();
        textDirty = true;
        coverDirty = true;
        sourceIconDirty = true;
        backdropDirty = true;
        entering = true;
        closing = false;
        if (!layoutAndPosition() || !render()) {
            entering = false;
            return;
        }

        // 窗口保持固定位置，只让 DirectComposition 根视觉滑入 + 淡入；
        // 中上时从上方滑入，与中下方向相反。
        renderer.resetRoot();
        const float travel = (placementTop ? -kEnterTravelDip : kEnterTravelDip) * scale();
        if (!renderer.animateRoot(0.0f, 0.0f, travel, 0.0f, 0.0f, 1.0f,
                                  static_cast<float>(kEnterAnimationMs) / 1000.0f)) {
            renderer.resetRoot();
        }
        renderer.commit();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        visible = true;
        SetTimer(hwnd, kEnterTimer, kEnterAnimationMs, nullptr);
        SetTimer(hwnd, kAutoHideTimer,
                 static_cast<UINT>(std::clamp(durationSec, 1, 10)) * 1000, nullptr);
    }

    void hideAnimated() {
        if (!visible || closing || !hwnd)
            return;
        KillTimer(hwnd, kEnterTimer);
        KillTimer(hwnd, kAutoHideTimer);
        KillTimer(hwnd, kScrollTimer);
        scrollTimerRunning = false;
        entering = false;
        closing = true;
        // 原地轻微偏移 + 淡出；中上时向上收起，方向与入场一致。
        const float travel = (placementTop ? -kExitTravelDip : kExitTravelDip) * scale();
        if (!renderer.animateRoot(0.0f, 0.0f, 0.0f, travel, 1.0f, 0.0f,
                                  static_cast<float>(kCloseAnimationMs) / 1000.0f)) {
            hideImmediate();
            return;
        }
        renderer.commit();
        SetTimer(hwnd, kCloseTimer, kCloseAnimationMs, nullptr);
    }

    void hideImmediate() {
        visible = false;
        if (!hwnd)
            return;
        killTimers();
        ShowWindow(hwnd, SW_HIDE);
        renderer.resetRoot();
        renderer.commit();
    }

    void setMedia(const OverlayMediaInfo& info) {
        if (info.title.empty()) {
            media = info;
            hideImmediate();
            return;
        }
        const bool textChanged = info.title != media.title || info.artist != media.artist;
        const bool coverChanged = info.thumbnail != media.thumbnail;
        const bool sourceChanged = info.sourceAppUserModelId != media.sourceAppUserModelId;
        media = info;
        if (coverChanged)
            coverDirty = true;
        if (sourceChanged)
            sourceIconDirty = true;
        if (textChanged)
            textDirty = true;
        if (!visible || entering || closing)
            return;
        render();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT; // 固定鼠标穿透
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wp == kEnterTimer) {
                KillTimer(hwnd, kEnterTimer);
                entering = false;
                updateScrollTimer();
            } else if (wp == kAutoHideTimer) {
                KillTimer(hwnd, kAutoHideTimer);
                hideAnimated();
            } else if (wp == kCloseTimer) {
                KillTimer(hwnd, kCloseTimer);
                if (closing)
                    hideImmediate();
            } else if (wp == kScrollTimer) {
                if (entering || closing || !visible)
                    return 0;
                advanceTextScroll();
                render();
            }
            return 0;
        case WM_DPICHANGED:
            dpi = GetDpiForWindow(hwnd);
            releaseDrawingResources();
            if (visible) {
                backdropDirty = true;
                layoutAndPosition();
                render();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            if (visible) {
                backdropDirty = true;
                layoutAndPosition();
                render();
            }
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            refreshClientAnimations();
            releaseDrawingResources();
            if (visible) {
                backdropDirty = true;
                render();
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            if (visible && !entering && !closing)
                render();
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
            releaseAll();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

SongToast::SongToast() : impl_(std::make_unique<Impl>()) {}

SongToast::~SongToast() {
    destroy();
}

bool SongToast::create(HINSTANCE inst) {
    if (impl_->hwnd)
        return true;
    impl_->inst = inst;
    impl_->refreshClientAnimations();
    if (!impl_->renderer.initialize())
        return false;
    if (!impl_->createTextFormats())
        return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // WS_EX_TRANSPARENT + WM_NCHITTEST=HTTRANSPARENT：双重保证鼠标事件完全穿透
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                          WS_EX_NOREDIRECTIONBITMAP;
    impl_->hwnd = CreateWindowExW(exStyle, kWndClassName, L"QQMusicLyricSongToast", WS_POPUP,
                                  0, 0, 1, 1, nullptr, nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;
    impl_->dpi = GetDpiForWindow(impl_->hwnd);
    if (impl_->dpi == 0)
        impl_->dpi = GetDpiForSystem();
    return true;
}

void SongToast::destroy() {
    if (impl_ && impl_->hwnd)
        DestroyWindow(impl_->hwnd);
}

void SongToast::setEnabled(bool enabled) {
    impl_->enabled = enabled;
    if (!enabled)
        impl_->hideImmediate();
}

void SongToast::setDurationSec(int seconds) {
    impl_->durationSec = std::clamp(seconds, 1, 10);
}

void SongToast::setPlacementTop(bool top) {
    if (impl_->placementTop == top)
        return;
    impl_->placementTop = top;
    if (impl_->visible) {
        // 位置改变需要重新抓背后内容
        impl_->backdropDirty = true;
        impl_->layoutAndPosition();
        impl_->render();
    }
}

void SongToast::showSong(const OverlayMediaInfo& info) {
    impl_->media = info;
    impl_->showSong();
}

void SongToast::setMedia(const OverlayMediaInfo& info) {
    impl_->setMedia(info);
}

void SongToast::hideImmediate() {
    impl_->hideImmediate();
}
