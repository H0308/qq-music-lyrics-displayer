#include "graphics/bitmap_renderer.h"

#pragma comment(lib, "msimg32.lib") // AlphaBlend（compositeTo）

BitmapRenderer::BitmapRenderer() = default;

BitmapRenderer::~BitmapRenderer() {
    releaseAll();
}

bool BitmapRenderer::initialize() {
    return createFactories();
}

void BitmapRenderer::setDpi(UINT dpi) {
    dpi_ = dpi;
    if (rt_)
        rt_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
}

bool BitmapRenderer::createFactories() {
    // D2D/DWrite 工厂创建昂贵（DWrite 要初始化字体子系统，单个可达数十毫秒），
    // 对话框里每个分层控件各持一个 BitmapRenderer，逐个创建会拖慢窗口打开。
    // 工厂对象不可变，进程内共享同一份，退出时随系统回收。
    static ID2D1Factory* sharedD2d = [] {
        D2D1_FACTORY_OPTIONS opts{};
        ID2D1Factory* f = nullptr;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory),
                                     &opts, reinterpret_cast<void**>(&f))))
            return static_cast<ID2D1Factory*>(nullptr);
        return f;
    }();
    static IDWriteFactory* sharedDwrite = [] {
        IDWriteFactory* f = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&f))))
            return static_cast<IDWriteFactory*>(nullptr);
        return f;
    }();
    if (!sharedD2d || !sharedDwrite)
        return false;
    d2d_ = sharedD2d;
    dwrite_ = sharedDwrite;
    return true;
}

bool BitmapRenderer::createRenderTarget() {
    if (rt_)
        return true;
    if (!d2d_)
        return false;
    // DC 渲染目标必须用软件光栅：DEFAULT（硬件优先）会让 D2D 为窗口所在显示器创建
    // 内部 D3D 设备，在无渲染硬件的虚拟屏（远程桌面虚拟适配器等）上回退 WARP，
    // 进程内拉起按核数扩张的线程池和与窗口像素尺寸成正比的软件表面，内存可飙至
    // 数百 MB。这些界面只有圆角矩形和文字，CPU 光栅足够，还省掉 GPU→CPU 回传。
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = d2d_->CreateDCRenderTarget(&props, &rt_);
    if (FAILED(hr)) {
        rt_ = nullptr;
        return false;
    }
    rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    rtBound_ = false;
    return true;
}

ID2D1DCRenderTarget* BitmapRenderer::renderTarget() {
    if (!createRenderTarget())
        return nullptr;
    return rt_;
}

bool BitmapRenderer::bindDC(int width, int height) {
    if (!createRenderTarget())
        return false;

    if (width == bmpW_ && height == bmpH_ && memdc_) {
        if (!rtBound_) {
            RECT rc{0, 0, width, height};
            HRESULT hr = rt_->BindDC(memdc_, &rc);
            if (FAILED(hr)) {
                discard();
                return false;
            }
            rtBound_ = true;
        }
        return true;
    }

    if (memdc_) {
        if (oldBmp_)
            SelectObject(memdc_, oldBmp_);
        if (dib_)
            DeleteObject(dib_);
        DeleteDC(memdc_);
        memdc_ = nullptr;
        dib_ = nullptr;
        oldBmp_ = nullptr;
    }
    bmpW_ = 0;
    bmpH_ = 0;

    HDC screen = GetDC(nullptr);
    memdc_ = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dib_ = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib_) {
        DeleteDC(memdc_);
        memdc_ = nullptr;
        return false;
    }
    oldBmp_ = SelectObject(memdc_, dib_);

    bmpW_ = width;
    bmpH_ = height;

    RECT rc{0, 0, width, height};
    HRESULT hr = rt_->BindDC(memdc_, &rc);
    if (FAILED(hr)) {
        discard();
        return false;
    }
    rtBound_ = true;
    return true;
}

bool BitmapRenderer::present(HWND hwnd, BYTE alpha) {
    if (!hwnd || !memdc_)
        return false;
    POINT ptSrc{0, 0};
    SIZE sz{bmpW_, bmpH_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    return !!UpdateLayeredWindow(hwnd, nullptr, nullptr, &sz, memdc_, &ptSrc, 0, &blend,
                                 ULW_ALPHA);
}

bool BitmapRenderer::copyToDC(HDC hdc, int width, int height) {
    if (!memdc_ || width <= 0 || height <= 0)
        return false;
    return !!BitBlt(hdc, 0, 0, width, height, memdc_, 0, 0, SRCCOPY);
}

bool BitmapRenderer::compositeTo(HDC dst, int x, int y) {
    if (!memdc_ || bmpW_ <= 0 || bmpH_ <= 0)
        return false;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return !!AlphaBlend(dst, x, y, bmpW_, bmpH_, memdc_, 0, 0, bmpW_, bmpH_, blend);
}

void BitmapRenderer::discard() {
    rtBound_ = false;
    if (rt_) {
        rt_->Release();
        rt_ = nullptr;
    }
}

void BitmapRenderer::releaseAll() {
    discard();
    if (memdc_) {
        if (oldBmp_)
            SelectObject(memdc_, oldBmp_);
        if (dib_)
            DeleteObject(dib_);
        DeleteDC(memdc_);
        memdc_ = nullptr;
        dib_ = nullptr;
        oldBmp_ = nullptr;
    }
    bmpW_ = 0;
    bmpH_ = 0;
    // 工厂是进程级共享的，不随实例释放
    dwrite_ = nullptr;
    d2d_ = nullptr;
}

ID2D1Factory* BitmapRenderer::d2d() const {
    return d2d_;
}

IDWriteFactory* BitmapRenderer::dwrite() const {
    return dwrite_;
}
