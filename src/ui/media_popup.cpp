#include "media_popup.h"

#include "fluent_theme.h"
#include "lyric_renderer.h"
#include "media_control_icons.h"
#include "platform_icon.h"

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
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
constexpr UINT kShowDelayMs = 100;
constexpr UINT kHideDelayMs = 180;
constexpr UINT kCloseAnimationMs = 140;
constexpr UINT kScrollTimerMs = 16;

constexpr float kPopupWidthDip = 384.0f;
constexpr float kPopupHeightDip = 176.0f;
constexpr float kPopupGapDip = 8.0f;
constexpr float kPopupCornerDip = 12.0f;
constexpr float kCoverSizeDip = 80.0f;
constexpr float kPopupEnterOffsetDip = 8.0f;
constexpr float kPopupTextLeftDip = 112.0f;
constexpr float kPopupTextRightPaddingDip = 16.0f;
constexpr float kPopupTextPaddingDip = 8.0f;
constexpr float kPopupInfoScrollSpeed = 10.0f;

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

} // namespace

struct MediaPopup::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND anchor = nullptr;
    UINT dpi = 96;

    bool enabled = false;
    bool available = false;
    bool anchorHover = false;
    bool popupHover = false;
    bool popupVisible = false;
    bool closing = false;
    bool placedAbove = true;
    bool themeDirty = true;
    bool coverDirty = true;
    bool sourceIconDirty = true;
    bool textDirty = true;
    bool scrollTimerRunning = false;
    bool clientAnimations = true;

    int hoverButton = -1;
    int pressedButton = -1;
    std::function<void(MediaControl)> onControl;
    OverlayMediaInfo media;

    DCompRenderer renderer;
    ID2D1SolidColorBrush* brushShadow = nullptr;
    ID2D1SolidColorBrush* brushBackground = nullptr;
    ID2D1SolidColorBrush* brushStroke = nullptr;
    ID2D1SolidColorBrush* brushText = nullptr;
    ID2D1SolidColorBrush* brushSecondary = nullptr;
    ID2D1SolidColorBrush* brushDisabled = nullptr;
    ID2D1SolidColorBrush* brushControl = nullptr;
    ID2D1SolidColorBrush* brushControlHover = nullptr;
    ID2D1SolidColorBrush* brushControlPressed = nullptr;
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* brushAccentHover = nullptr;
    ID2D1SolidColorBrush* brushTextOnAccent = nullptr;

    IDWriteTextFormat* fmtSource = nullptr;
    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtArtist = nullptr;
    IDWriteTextFormat* fmtIcon = nullptr;
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
    ID2D1RoundedRectangleGeometry* coverClip = nullptr;
    ID2D1Layer* coverLayer = nullptr;
    media_control::Geometry controlGeometry;
    D2D1_RECT_F buttonRects[3]{};

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

    void killTimers() {
        if (!hwnd)
            return;
        KillTimer(hwnd, kShowTimer);
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kCloseTimer);
        KillTimer(hwnd, kScrollTimer);
        scrollTimerRunning = false;
    }

    void releaseDrawingResources() {
        releaseVisualResources();
        renderer.discard();
        themeDirty = true;
        coverDirty = true;
    }

    void releaseVisualResources() {
        releaseBitmap(coverBmp);
        releaseBitmap(sourceIconBmp);
        sourceIconDirty = true;
        releaseBrush(brushShadow);
        releaseBrush(brushBackground);
        releaseBrush(brushStroke);
        releaseBrush(brushText);
        releaseBrush(brushSecondary);
        releaseBrush(brushDisabled);
        releaseBrush(brushControl);
        releaseBrush(brushControlHover);
        releaseBrush(brushControlPressed);
        releaseBrush(brushAccent);
        releaseBrush(brushAccentHover);
        releaseBrush(brushTextOnAccent);
        releaseFormat(fmtSource);
        releaseFormat(fmtTitle);
        releaseFormat(fmtArtist);
        releaseFormat(fmtIcon);
        releaseCom(titleLayout);
        releaseCom(artistLayout);
        titleWidth = 0.0f;
        titleHeight = 0.0f;
        artistWidth = 0.0f;
        artistHeight = 0.0f;
        textDirty = true;
        releaseCom(coverClip);
        media_control::release(controlGeometry);
        releaseCom(coverLayer);
    }

    void releaseAll() {
        releaseDrawingResources();
        renderer.releaseAll();
    }

    bool createTextFormat(float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out,
                          DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (!out || !renderer.dwrite())
            return false;
        *out = nullptr;
        HRESULT hr = renderer.dwrite()->CreateTextFormat(
            fluent::uiFontFamily(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", out);
        if (FAILED(hr) || !*out)
            return false;
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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

        const auto& p = fluent::palette();
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f),
                                              &brushShadow)) ||
            FAILED(rt->CreateSolidColorBrush(p.cardFillSolid, &brushBackground)) ||
            FAILED(rt->CreateSolidColorBrush(p.cardStroke, &brushStroke)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.disabled, &brushDisabled)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlFill, &brushControl)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlHover, &brushControlHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.controlPressed, &brushControlPressed)) ||
            FAILED(rt->CreateSolidColorBrush(p.accent, &brushAccent)) ||
            FAILED(rt->CreateSolidColorBrush(p.accentHover, &brushAccentHover)) ||
            FAILED(rt->CreateSolidColorBrush(p.textOnAccent, &brushTextOnAccent)) ||
            !createTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtSource) ||
            !createTextFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtTitle) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtArtist) ||
            !createTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtIcon)) {
            releaseDrawingResources();
            return false;
        }

        auto* factory = renderer.d2d();
        if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(
                                 D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, kCoverSizeDip,
                                                               kCoverSizeDip),
                                                   10.0f, 10.0f),
                                 &coverClip)) ||
            FAILED(rt->CreateLayer(&coverLayer)) || !media_control::create(factory, controlGeometry)) {
            releaseDrawingResources();
            return false;
        }

        themeDirty = false;
        return true;
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

    float textAreaWidth(float popupWidth) const {
        return std::max(1.0f, popupWidth - kPopupTextLeftDip - kPopupTextRightPaddingDip);
    }

    void updateScrollTimer(float areaWidth) {
        const bool overflow = titleWidth > areaWidth || artistWidth > areaWidth;
        const bool shouldRun = popupVisible && enabled && available && clientAnimations &&
                               media.playing && overflow;
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
        if (!clientAnimations || !media.playing)
            return;

        const float areaWidth = textAreaWidth(kPopupWidthDip);
        auto marquee = [&](float textWidth, float& offset) {
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
        marquee(titleWidth, titleScrollOffset);
        marquee(artistWidth, artistScrollOffset);
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

    bool buttonEnabled(int index) const {
        if (!available)
            return false;
        // 某些播放器的 SMTC 快照会把前后曲目能力标成 false，但对应的
        // TrySkipPrevious/Next 仍然是可用操作；媒体卡片保留明确的操作入口。
        return index == 1 ? media.canPlayPause : true;
    }

    void drawButton(ID2D1DeviceContext* rt, int index) {
        const bool enabled = buttonEnabled(index);
        const D2D1_RECT_F rect = buttonRects[index];
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
        renderer.setDpi(dpi);
        if (!createResources())
            return false;
        buildTextLayouts();
        if (coverDirty)
            decodeCover();
        if (sourceIconDirty)
            decodeSourceIcon();

        auto* rt = renderer.renderTarget();
        if (!rt)
            return false;
        const float w = dip(pxW);
        const float h = dip(pxH);
        const float infoW = textAreaWidth(w);
        updateScrollTimer(infoW);
        buttonRects[0] = D2D1::RectF(w * 0.5f - 102.0f, 126.0f, w * 0.5f - 66.0f, 162.0f);
        buttonRects[1] = D2D1::RectF(w * 0.5f - 20.0f, 124.0f, w * 0.5f + 20.0f, 164.0f);
        buttonRects[2] = D2D1::RectF(w * 0.5f + 66.0f, 126.0f, w * 0.5f + 102.0f, 162.0f);

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const D2D1_RECT_F card = D2D1::RectF(1.0f, 1.0f, w - 1.0f, h - 3.0f);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(card.left, card.top + 3.0f, card.right,
                                          card.bottom + 3.0f),
                              kPopupCornerDip, kPopupCornerDip),
            brushShadow);
        rt->FillRoundedRectangle(D2D1::RoundedRect(card, kPopupCornerDip, kPopupCornerDip),
                                 brushBackground);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(card, kPopupCornerDip, kPopupCornerDip),
                                 brushStroke, 1.0f);

        drawSource(rt);
        drawCover(rt);
        drawScrollingText(rt, titleLayout, titleWidth, titleHeight,
                          D2D1::RectF(kPopupTextLeftDip, 48.0f, w - kPopupTextRightPaddingDip,
                                      76.0f),
                          titleScrollOffset, brushText);
        drawScrollingText(rt, artistLayout, artistWidth, artistHeight,
                          D2D1::RectF(kPopupTextLeftDip, 78.0f, w - kPopupTextRightPaddingDip,
                                      102.0f),
                          artistScrollOffset, brushSecondary);
        for (int i = 0; i < 3; ++i)
            drawButton(rt, i);

        const HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            releaseDrawingResources();
            return false;
        }
        if (FAILED(hr) || !renderer.present()) {
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
        const int popupH = static_cast<int>(std::lround(kPopupHeightDip * s));
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

        SetWindowPos(hwnd, HWND_TOPMOST, x, y, popupW, popupH,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    int hitButton(float x, float y) const {
        for (int i = 0; i < 3; ++i) {
            if (buttonEnabled(i) && contains(buttonRects[i], x, y))
                return i;
        }
        return -1;
    }

    void trackMouseLeave() {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    void scheduleHide() {
        if (!hwnd || !popupVisible || anchorHover || popupHover)
            return;
        KillTimer(hwnd, kShowTimer);
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void showPopup() {
        if (!hwnd || !enabled || !available)
            return;
        KillTimer(hwnd, kShowTimer);
        KillTimer(hwnd, kHideTimer);
        KillTimer(hwnd, kCloseTimer);
        reposition();
        if (!render())
            return;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        popupVisible = true;
        closing = false;
        updateScrollTimer(textAreaWidth(kPopupWidthDip));
        renderer.resetRoot();
        const float offset = kPopupEnterOffsetDip * scale();
        const float fromY = placedAbove ? offset : -offset;
        if (!renderer.animateRoot(0.0f, 0.0f, fromY, 0.0f, 0.0f, 1.0f, 0.18f))
            renderer.resetRoot();
        renderer.commit();
    }

    void hideAnimated() {
        if (!popupVisible || closing)
            return;
        KillTimer(hwnd, kHideTimer);
        closing = true;
        const float offset = kPopupEnterOffsetDip * scale();
        const float toY = placedAbove ? offset : -offset;
        if (!renderer.animateRoot(0.0f, 0.0f, 0.0f, toY, 1.0f, 0.0f,
                                  kCloseAnimationMs / 1000.0f)) {
            hideImmediate();
            return;
        }
        renderer.commit();
        SetTimer(hwnd, kCloseTimer, kCloseAnimationMs, nullptr);
    }

    void onPopupEnter() {
        popupHover = true;
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
        if (enabled && available && !popupVisible)
            SetTimer(hwnd, kShowTimer, kShowDelayMs, nullptr);
    }

    void onAnchorLeave() {
        anchorHover = false;
        KillTimer(hwnd, kShowTimer);
        scheduleHide();
    }

    void hideImmediate() {
        if (!hwnd)
            return;
        killTimers();
        popupVisible = false;
        closing = false;
        ShowWindow(hwnd, SW_HIDE);
        renderer.resetRoot();
        renderer.commit();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_MOUSEMOVE: {
            onPopupEnter();
            const int button = hitButton(dip(GET_X_LPARAM(lp)), dip(GET_Y_LPARAM(lp)));
            if (button != hoverButton) {
                hoverButton = button;
                render();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverButton = -1;
            onPopupLeave();
            render();
            return 0;
        case WM_LBUTTONDOWN: {
            const int button = hitButton(dip(GET_X_LPARAM(lp)), dip(GET_Y_LPARAM(lp)));
            if (button >= 0) {
                pressedButton = button;
                SetCapture(hwnd);
                render();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const int button = hitButton(dip(GET_X_LPARAM(lp)), dip(GET_Y_LPARAM(lp)));
            const int pressed = pressedButton;
            pressedButton = -1;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            render();
            if (pressed >= 0 && pressed == button && onControl)
                onControl(static_cast<MediaControl>(pressed));
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressedButton = -1;
            render();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_TIMER:
            if (wp == kShowTimer) {
                KillTimer(hwnd, kShowTimer);
                if (anchorHover)
                    showPopup();
            } else if (wp == kHideTimer) {
                KillTimer(hwnd, kHideTimer);
                if (!anchorHover && !popupHover)
                    hideAnimated();
            } else if (wp == kCloseTimer) {
                KillTimer(hwnd, kCloseTimer);
                if (closing && !anchorHover && !popupHover)
                    hideImmediate();
                else if (closing)
                    showPopup();
            } else if (wp == kScrollTimer) {
                advanceTextScroll();
                render();
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

    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
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

void MediaPopup::setEnabled(bool enabled) {
    impl_->enabled = enabled;
    if (!impl_->hwnd)
        return;
    if (!enabled)
        impl_->hideImmediate();
    else if (impl_->anchorHover && impl_->available && !impl_->popupVisible)
        SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
}

void MediaPopup::setMedia(const OverlayMediaInfo& info, bool available) {
    const bool coverChanged = info.thumbnail != impl_->media.thumbnail;
    const bool sourceChanged = info.sourceAppUserModelId != impl_->media.sourceAppUserModelId;
    const bool textChanged = info.title != impl_->media.title || info.artist != impl_->media.artist;
    const bool changed = coverChanged || info.title != impl_->media.title ||
                         info.artist != impl_->media.artist ||
                         sourceChanged ||
                         info.canPrev != impl_->media.canPrev ||
                         info.canPlayPause != impl_->media.canPlayPause ||
                         info.canNext != impl_->media.canNext ||
                         info.playing != impl_->media.playing;
    impl_->media = info;
    impl_->available = available;
    if (coverChanged)
        impl_->coverDirty = true;
    if (sourceChanged)
        impl_->sourceIconDirty = true;
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
    if (changed && impl_->popupVisible)
        impl_->render();
    if (impl_->hwnd && impl_->anchorHover && impl_->enabled && !impl_->popupVisible)
        SetTimer(impl_->hwnd, kShowTimer, kShowDelayMs, nullptr);
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

void MediaPopup::hideImmediate() {
    impl_->hideImmediate();
}
