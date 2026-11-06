#include "ui/taskbar/cover_image.h"
#include <d2d1effects.h>
#include <gdiplus.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr float kCoverBlurStdDev = 6.0f;
// d2d1effects.h 只声明这些 GUID；当前工程的链接配置不提供其外部定义，
// 这里保留 Direct2D 标准 Gaussian Blur / Scale CLSID 的内部定义（与 media_popup.cpp 一致）。
constexpr CLSID kGaussianBlurClsid = {
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
constexpr CLSID kScaleClsid = {
    0x9daf9369, 0x3846, 0x4d0e, {0xa4, 0x4e, 0x0c, 0x60, 0x79, 0x34, 0xa5, 0xd7}};


} // namespace
void CoverImage::decode(ID2D1DeviceContext* rt, const std::vector<uint8_t>* bytes, UINT dpi,
                        UINT targetPx) {
    // 先解除模糊链对旧图的引用，再替换位图，避免旧封面延迟到下一次绘制才释放。
    if (blur_)
        blur_->SetInput(0, nullptr);
    bitmap_.Reset();
    blurInput_ = nullptr;
    if (!rt || !bytes || bytes->empty())
        return;

    HGLOBAL hglobal = GlobalAlloc(GHND, bytes->size());
    if (!hglobal)
        return;
    void* ptr = GlobalLock(hglobal);
    if (!ptr) {
        GlobalFree(hglobal);
        return;
    }
    memcpy(ptr, bytes->data(), bytes->size());
    GlobalUnlock(hglobal);
    Microsoft::WRL::ComPtr<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(hglobal, TRUE, stream.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !stream) {
        GlobalFree(hglobal);
        return;
    }
    Gdiplus::Bitmap bitmap(stream.Get());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return;
    }
    UINT w = bitmap.GetWidth();
    UINT h = bitmap.GetHeight();
    // 只按显示尺寸（封面槽 DIP × DPI 缩放）生成位图：原图常达 500~1000px，
    // 全尺寸 LockBits/CreateBitmap 会把 MB 级像素常驻显存，实际只显示约 30px。
    Gdiplus::Bitmap* pixels = &bitmap;
    Gdiplus::Bitmap scaled(static_cast<INT>(targetPx), static_cast<INT>(targetPx),
                           PixelFormat32bppPARGB);
    if ((w > targetPx || h > targetPx) && scaled.GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics g(&scaled);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // 目标为正方形，与绘制时拉伸到方形封面槽的行为一致
        if (g.DrawImage(&bitmap,
                        Gdiplus::Rect(0, 0, static_cast<INT>(targetPx), static_cast<INT>(targetPx)),
                        0, 0, static_cast<INT>(w), static_cast<INT>(h),
                        Gdiplus::UnitPixel) == Gdiplus::Ok) {
            pixels = &scaled;
            w = targetPx;
            h = targetPx;
        }
    }
    Gdiplus::BitmapData bitmapData{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    if (pixels->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bitmapData) !=
        Gdiplus::Ok) {
        return;
    }
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi), static_cast<float>(dpi));
    ID2D1Bitmap1* decoded = nullptr;
    hr = rt->CreateBitmap(D2D1::SizeU(w, h), bitmapData.Scan0, bitmapData.Stride, &props, &decoded);
    bitmap_.Attach(decoded); // ID2D1Bitmap1 派生自 ID2D1Bitmap
    pixels->UnlockBits(&bitmapData);
}

ID2D1Effect* CoverImage::blurred(ID2D1DeviceContext* rt, float w, float h) {
    if (!rt || !bitmap_.Get())
        return nullptr;
    if (!blur_.Get()) {
        if (FAILED(rt->CreateEffect(kGaussianBlurClsid, blur_.ReleaseAndGetAddressOf())))
            return nullptr;
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kCoverBlurStdDev);
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
        // SOFT（镜像）避免边缘模糊后透出黑边
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    }
    if (!scale_.Get()) {
        if (FAILED(rt->CreateEffect(kScaleClsid, scale_.ReleaseAndGetAddressOf())))
            return nullptr;
        scale_->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE,
                         D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        scale_->SetInputEffect(0, blur_.Get());
    }
    if (blurInput_ != bitmap_.Get()) {
        blur_->SetInput(0, bitmap_.Get());
        blurInput_ = bitmap_.Get();
    }
    const D2D1_SIZE_F size = bitmap_->GetSize();
    if (size.width <= 0.0f || size.height <= 0.0f)
        return nullptr;
    scale_->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(w / size.width, h / size.height));
    return scale_.Get();
}
