#include "lyric_renderer.h"

#include <cstdio>
#include <iterator>

#pragma comment(lib, "msimg32.lib") // AlphaBlend（compositeTo）

LyricRenderer::LyricRenderer() = default;

LyricRenderer::~LyricRenderer() {
    releaseAll();
}

bool LyricRenderer::initialize() {
    return createFactories();
}

void LyricRenderer::setDpi(UINT dpi) {
    dpi_ = dpi;
    if (rt_)
        rt_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
}

bool LyricRenderer::createFactories() {
    // D2D/DWrite 工厂创建昂贵（DWrite 要初始化字体子系统，单个可达数十毫秒），
    // 对话框里每个分层控件各持一个 LyricRenderer，逐个创建会拖慢窗口打开。
    // 工厂对象不可变，进程内共享同一份，退出时随系统回收。
    static ID2D1Factory* sharedD2d = [] {
        D2D1_FACTORY_OPTIONS opts{};
        ID2D1Factory* f = nullptr;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
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

bool LyricRenderer::createRenderTarget() {
    if (rt_)
        return true;
    if (!d2d_)
        return false;
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
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

ID2D1DCRenderTarget* LyricRenderer::renderTarget() {
    if (!createRenderTarget())
        return nullptr;
    return rt_;
}

bool LyricRenderer::bindDC(int width, int height) {
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

bool LyricRenderer::present(HWND hwnd) {
    if (!hwnd || !memdc_)
        return false;
    POINT ptSrc{0, 0};
    SIZE sz{bmpW_, bmpH_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return !!UpdateLayeredWindow(hwnd, nullptr, nullptr, &sz, memdc_, &ptSrc, 0, &blend,
                                 ULW_ALPHA);
}

bool LyricRenderer::copyToDC(HDC hdc, int width, int height) {
    if (!memdc_ || width <= 0 || height <= 0)
        return false;
    return !!BitBlt(hdc, 0, 0, width, height, memdc_, 0, 0, SRCCOPY);
}

bool LyricRenderer::compositeTo(HDC dst, int x, int y) {
    if (!memdc_ || bmpW_ <= 0 || bmpH_ <= 0)
        return false;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return !!AlphaBlend(dst, x, y, bmpW_, bmpH_, memdc_, 0, 0, bmpW_, bmpH_, blend);
}

void LyricRenderer::discard() {
    rtBound_ = false;
    if (rt_) {
        rt_->Release();
        rt_ = nullptr;
    }
}

void LyricRenderer::releaseAll() {
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

ID2D1Factory* LyricRenderer::d2d() const {
    return d2d_;
}

IDWriteFactory* LyricRenderer::dwrite() const {
    return dwrite_;
}

// ---------- DCompRenderer ----------

DCompRenderer::DCompRenderer() = default;

DCompRenderer::~DCompRenderer() {
    releaseAll();
}

bool DCompRenderer::initialize() {
    return createFactories();
}

void DCompRenderer::setDpi(UINT dpi) {
    dpi_ = dpi;
    if (dc_)
        dc_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
}

ID2D1DeviceContext* DCompRenderer::renderTarget() {
    return dc_;
}

ID2D1Factory* DCompRenderer::d2d() const {
    return d2d1_;
}

IDWriteFactory* DCompRenderer::dwrite() const {
    return dwrite_;
}

bool DCompRenderer::createFactories() {
    if (!d2d1_) {
        D2D1_FACTORY_OPTIONS opts{};
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory1), &opts,
                                       reinterpret_cast<void**>(&d2d1_));
        if (FAILED(hr))
            return false;
    }
    if (!dwrite_) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&dwrite_));
        if (FAILED(hr))
            return false;
    }
    return true;
}

bool DCompRenderer::createDevice() {
    if (dc_)
        return true;
    if (!d2d1_)
        return false;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   &d3d_, nullptr, nullptr);
    if (SUCCEEDED(hr))
        hr = d3d_->QueryInterface(&dxgiDevice_);
    if (SUCCEEDED(hr))
        hr = d2d1_->CreateDevice(dxgiDevice_, &d2dDevice_);
    if (SUCCEEDED(hr))
        hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (SUCCEEDED(hr)) {
        dc_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        hr = DCompositionCreateDevice2(d2dDevice_, __uuidof(IDCompositionDevice),
                                       reinterpret_cast<void**>(&dcomp_));
    }
    if (FAILED(hr)) {
        discard();
        return false;
    }
    return true;
}

bool DCompRenderer::ensureSwapchain(HWND hwnd, int width, int height) {
    if (swapchain_ && (width != width_ || height != height_)) {
        releaseBackBuffer();
        HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                               static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            discard();
            return false;
        }
        width_ = width;
        height_ = height;
    }
    if (swapchain_ && hwnd != hwnd_) {
        // 窗口重建（Explorer 重启后 createWindow）：DComp 目标绑在原 HWND 上，必须重建
        releaseLyricTransitionLayers();
        if (rootOpacity_) {
            if (visual_)
                visual_->SetEffect(nullptr);
            rootOpacity_->Release();
            rootOpacity_ = nullptr;
        }
        if (visual_) {
            visual_->Release();
            visual_ = nullptr;
        }
        if (target_) {
            target_->Release();
            target_ = nullptr;
        }
        swapchain_->Release();
        swapchain_ = nullptr;
        releaseBackBuffer();
    }
    if (!swapchain_) {
        IDXGIFactory2* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                        reinterpret_cast<void**>(&factory));
        if (FAILED(hr))
            return false;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = {1, 0};
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForComposition(dxgiDevice_, &desc, nullptr, &swapchain_);
        factory->Release();
        if (FAILED(hr))
            return false;

        hr = dcomp_->CreateTargetForHwnd(hwnd, TRUE, &target_);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateVisual(&visual_);
        if (SUCCEEDED(hr))
            hr = visual_->SetContent(swapchain_);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateEffectGroup(&rootOpacity_);
        if (SUCCEEDED(hr))
            hr = rootOpacity_->SetOpacity(1.0f);
        if (SUCCEEDED(hr))
            hr = visual_->SetEffect(rootOpacity_);
        if (SUCCEEDED(hr))
            hr = target_->SetRoot(visual_);
        if (SUCCEEDED(hr))
            hr = dcomp_->Commit();
        if (FAILED(hr))
            return false;
        hwnd_ = hwnd;
        width_ = width;
        height_ = height;
    }
    if (!backBmp_) {
        IDXGISurface* surface = nullptr;
        HRESULT hr = swapchain_->GetBuffer(0, __uuidof(IDXGISurface),
                                           reinterpret_cast<void**>(&surface));
        if (FAILED(hr))
            return false;
        auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        hr = dc_->CreateBitmapFromDxgiSurface(surface, &props, &backBmp_);
        surface->Release();
        if (FAILED(hr))
            return false;
        dc_->SetTarget(backBmp_);
    }
    return true;
}

void DCompRenderer::releaseBackBuffer() {
    if (dc_)
        dc_->SetTarget(nullptr);
    if (backBmp_) {
        backBmp_->Release();
        backBmp_ = nullptr;
    }
}

bool DCompRenderer::bind(HWND hwnd, int width, int height) {
    if (!createFactories() || !createDevice()) {
        std::wprintf(L"[dcomp] device create failed\n");
        return false;
    }
    if (!ensureSwapchain(hwnd, width, height)) {
        std::wprintf(L"[dcomp] swapchain bind failed (hwnd=%p %dx%d)\n", hwnd, width, height);
        return false;
    }
    return true;
}

bool DCompRenderer::present() {
    if (!swapchain_)
        return false;
    releaseBackBuffer(); // Present 前解绑后备缓冲
    HRESULT hr = swapchain_->Present(0, 0);
    if (FAILED(hr)) {
        std::wprintf(L"[dcomp] Present failed: 0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }
    if (dcomp_)
        dcomp_->Commit();
    return true;
}

void DCompRenderer::releaseLyricTransitionLayers() {
    for (auto& layer : lyricLayers_) {
        if (visual_ && layer.visual)
            visual_->RemoveVisual(layer.visual);
        if (layer.visual) {
            layer.visual->SetContent(nullptr);
            layer.visual->SetEffect(nullptr);
            layer.visual->Release();
            layer.visual = nullptr;
        }
        if (layer.opacity) {
            layer.opacity->Release();
            layer.opacity = nullptr;
        }
        if (layer.surface) {
            layer.surface->Release();
            layer.surface = nullptr;
        }
        layer.width = 0;
        layer.height = 0;
    }
}

bool DCompRenderer::ensureLyricTransitionLayers(int width, int height) {
    if (!dcomp_ || !visual_ || width <= 0 || height <= 0)
        return false;
    if (lyricLayers_[0].surface && lyricLayers_[1].surface &&
        lyricLayers_[0].width == width && lyricLayers_[0].height == height)
        return true;

    releaseLyricTransitionLayers();
    for (int i = 0; i < 2; ++i) {
        auto& layer = lyricLayers_[i];
        HRESULT hr = dcomp_->CreateVisual(&layer.visual);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height),
                                       DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                       &layer.surface);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateEffectGroup(&layer.opacity);
        if (SUCCEEDED(hr))
            hr = layer.opacity->SetOpacity(0.0f);
        if (SUCCEEDED(hr))
            hr = layer.visual->SetContent(layer.surface);
        if (SUCCEEDED(hr))
            hr = layer.visual->SetEffect(layer.opacity);
        if (SUCCEEDED(hr))
            hr = layer.visual->SetOffsetX(0.0f);
        if (SUCCEEDED(hr))
            hr = layer.visual->SetOffsetY(0.0f);
        if (SUCCEEDED(hr))
            hr = visual_->AddVisual(layer.visual, TRUE, nullptr);
        if (FAILED(hr)) {
            releaseLyricTransitionLayers();
            return false;
        }
        layer.width = width;
        layer.height = height;
    }
    return true;
}

ID2D1DeviceContext* DCompRenderer::beginLyricLayerDraw(int index) {
    if (index < 0 || index >= 2 || !lyricLayers_[index].surface)
        return nullptr;
    POINT offset{};
    ID2D1DeviceContext* dc = nullptr;
    HRESULT hr = lyricLayers_[index].surface->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&dc), &offset);
    if (FAILED(hr) || !dc) {
        if (dc)
            dc->Release();
        return nullptr;
    }
    const float dipX = static_cast<float>(offset.x) * 96.0f / static_cast<float>(dpi_);
    const float dipY = static_cast<float>(offset.y) * 96.0f / static_cast<float>(dpi_);
    dc->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    dc->SetTransform(D2D1::Matrix3x2F::Translation(dipX, dipY));
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    return dc;
}

bool DCompRenderer::endLyricLayerDraw(int index, ID2D1DeviceContext* dc) {
    if (index < 0 || index >= 2 || !lyricLayers_[index].surface || !dc)
        return false;
    HRESULT hr = lyricLayers_[index].surface->EndDraw();
    dc->Release();
    return SUCCEEDED(hr);
}

bool DCompRenderer::addSmoothStep(IDCompositionAnimation* animation, double begin, double end,
                                  float from, float to) {
    if (!animation || end <= begin)
        return false;
    if (from == to)
        return SUCCEEDED(animation->AddCubic(begin, from, 0.0f, 0.0f, 0.0f));

    const double a = begin;
    const double d = end - begin;
    const double delta = static_cast<double>(to) - from;
    const double cubic = -2.0 * delta / (d * d * d);
    const double quadratic = delta * (3.0 / (d * d) + 6.0 * a / (d * d * d));
    const double linear = delta * (-6.0 * a / (d * d) - 6.0 * a * a / (d * d * d));
    const double constant =
        static_cast<double>(from) + delta * (3.0 * a * a / (d * d) + 2.0 * a * a * a / (d * d * d));
    return SUCCEEDED(animation->AddCubic(begin, static_cast<float>(constant),
                                         static_cast<float>(linear), static_cast<float>(quadratic),
                                         static_cast<float>(cubic)));
}

bool DCompRenderer::animateLyricLayer(int index, float fromY, float toY, float fromOpacity,
                                      float toOpacity, float durationSec) {
    if (index < 0 || index >= 2 || !dcomp_ || !lyricLayers_[index].visual ||
        !lyricLayers_[index].opacity || durationSec <= 0.0f)
        return false;

    const double duration = static_cast<double>(durationSec);
    IDCompositionAnimation* offsetAnim = nullptr;
    IDCompositionAnimation* opacityAnim = nullptr;
    HRESULT hr = dcomp_->CreateAnimation(&offsetAnim);
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateAnimation(&opacityAnim);
    if (SUCCEEDED(hr))
        hr = addSmoothStep(offsetAnim, 0.0, duration, fromY, toY) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = offsetAnim->End(duration, toY);
    if (SUCCEEDED(hr)) {
        if (fromOpacity > toOpacity) {
            if (!addSmoothStep(opacityAnim, 0.0, duration * 0.08, fromOpacity, fromOpacity) ||
                !addSmoothStep(opacityAnim, duration * 0.08, duration * 0.90, fromOpacity,
                               toOpacity))
                hr = E_FAIL;
        } else if (fromOpacity < toOpacity) {
            if (!addSmoothStep(opacityAnim, 0.0, duration * 0.14, fromOpacity, fromOpacity) ||
                !addSmoothStep(opacityAnim, duration * 0.14, duration, fromOpacity, toOpacity))
                hr = E_FAIL;
        } else if (!addSmoothStep(opacityAnim, 0.0, duration, fromOpacity, toOpacity)) {
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr))
        hr = opacityAnim->End(duration, toOpacity);
    if (SUCCEEDED(hr))
        hr = lyricLayers_[index].visual->SetOffsetX(0.0f);
    if (SUCCEEDED(hr))
        hr = lyricLayers_[index].visual->SetOffsetY(offsetAnim);
    if (SUCCEEDED(hr))
        hr = lyricLayers_[index].opacity->SetOpacity(opacityAnim);

    if (offsetAnim)
        offsetAnim->Release();
    if (opacityAnim)
        opacityAnim->Release();
    return SUCCEEDED(hr);
}

void DCompRenderer::clearLyricTransitionLayers() {
    releaseLyricTransitionLayers();
}

bool DCompRenderer::animateRoot(float fromX, float toX, float fromY, float toY,
                                float fromOpacity, float toOpacity, float durationSec) {
    if (!dcomp_ || !visual_ || !rootOpacity_ || durationSec <= 0.0f)
        return false;

    const double duration = static_cast<double>(durationSec);
    IDCompositionAnimation* xAnim = nullptr;
    IDCompositionAnimation* yAnim = nullptr;
    IDCompositionAnimation* opacityAnim = nullptr;
    HRESULT hr = dcomp_->CreateAnimation(&xAnim);
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateAnimation(&yAnim);
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateAnimation(&opacityAnim);
    if (SUCCEEDED(hr))
        hr = addSmoothStep(xAnim, 0.0, duration, fromX, toX) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = addSmoothStep(yAnim, 0.0, duration, fromY, toY) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = addSmoothStep(opacityAnim, 0.0, duration, fromOpacity, toOpacity) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = xAnim->End(duration, toX);
    if (SUCCEEDED(hr))
        hr = yAnim->End(duration, toY);
    if (SUCCEEDED(hr))
        hr = opacityAnim->End(duration, toOpacity);
    if (SUCCEEDED(hr))
        hr = visual_->SetOffsetX(xAnim);
    if (SUCCEEDED(hr))
        hr = visual_->SetOffsetY(yAnim);
    if (SUCCEEDED(hr))
        hr = rootOpacity_->SetOpacity(opacityAnim);

    if (xAnim)
        xAnim->Release();
    if (yAnim)
        yAnim->Release();
    if (opacityAnim)
        opacityAnim->Release();
    return SUCCEEDED(hr);
}

void DCompRenderer::resetRoot() {
    if (!visual_)
        return;
    visual_->SetOffsetX(0.0f);
    visual_->SetOffsetY(0.0f);
    if (rootOpacity_)
        rootOpacity_->SetOpacity(1.0f);
}

void DCompRenderer::commit() {
    if (dcomp_)
        dcomp_->Commit();
}

void DCompRenderer::discard() {
    releaseBackBuffer();
    releaseLyricTransitionLayers();
    auto release = [](auto*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    if (visual_ && rootOpacity_)
        visual_->SetEffect(nullptr);
    release(rootOpacity_);
    release(visual_);
    release(target_);
    release(swapchain_);
    release(dcomp_);
    release(dc_);
    release(d2dDevice_);
    release(dxgiDevice_);
    release(d3d_);
    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
}

void DCompRenderer::releaseAll() {
    discard();
    if (dwrite_) {
        dwrite_->Release();
        dwrite_ = nullptr;
    }
    if (d2d1_) {
        d2d1_->Release();
        d2d1_ = nullptr;
    }
}
