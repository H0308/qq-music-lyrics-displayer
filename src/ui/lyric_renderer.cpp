#include "lyric_renderer.h"

#include "logging/runtime_logger.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <memory>
#include <mutex>

struct DCompSharedDevice {
    ID3D11Device* d3d = nullptr;
    IDXGIDevice* dxgi = nullptr;
    bool invalid = false;

    ~DCompSharedDevice() {
        if (dxgi)
            dxgi->Release();
        if (d3d)
            d3d->Release();
    }
};

namespace {

std::shared_ptr<DCompSharedDevice> acquireSharedDevice() {
    static std::weak_ptr<DCompSharedDevice> weak;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (auto shared = weak.lock(); shared && !shared->invalid)
        return shared;

    auto shared = std::make_shared<DCompSharedDevice>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   &shared->d3d, nullptr, nullptr);
    if (SUCCEEDED(hr))
        hr = shared->d3d->QueryInterface(&shared->dxgi);
    if (FAILED(hr))
        return nullptr;

    weak = shared;
    runtime_log::writef(L"[dcomp] shared device created");
    return shared;
}

bool isDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

HRESULT configureRectangleClip(IDCompositionRectangleClip* clip, float left, float top,
                               float right, float bottom, float cornerRadius) {
    if (!clip || right <= left || bottom <= top)
        return E_INVALIDARG;

    float radius = std::max(0.0f, cornerRadius);
    radius = std::min(radius, std::min(right - left, bottom - top) * 0.5f);
    HRESULT hr = clip->SetLeft(left);
    if (SUCCEEDED(hr))
        hr = clip->SetTop(top);
    if (SUCCEEDED(hr))
        hr = clip->SetRight(right);
    if (SUCCEEDED(hr))
        hr = clip->SetBottom(bottom);
    if (SUCCEEDED(hr))
        hr = clip->SetTopLeftRadiusX(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetTopLeftRadiusY(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetTopRightRadiusX(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetTopRightRadiusY(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetBottomLeftRadiusX(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetBottomLeftRadiusY(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetBottomRightRadiusX(radius);
    if (SUCCEEDED(hr))
        hr = clip->SetBottomRightRadiusY(radius);
    return hr;
}

} // namespace

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
    if (dc_ && sharedDevice_ && !sharedDevice_->invalid)
        return true;
    if (dc_)
        discard();
    if (!d2d1_)
        return false;

    sharedDevice_ = acquireSharedDevice();
    if (!sharedDevice_)
        return false;

    // 共享设备只由 sharedDevice_ 持有；以下两个指针仅供现有绘制代码访问。
    d3d_ = sharedDevice_->d3d;
    dxgiDevice_ = sharedDevice_->dxgi;

    // D2D 设备与上下文保持窗口级隔离，避免不同弹窗的绘制状态互相影响。
    HRESULT hr = d2d1_->CreateDevice(dxgiDevice_, &d2dDevice_);
    if (SUCCEEDED(hr))
        hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (SUCCEEDED(hr))
        dc_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (SUCCEEDED(hr))
        hr = DCompositionCreateDevice2(d2dDevice_, __uuidof(IDCompositionDevice),
                                       reinterpret_cast<void**>(&dcomp_));
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
        if (rootClip_) {
            if (visual_)
                visual_->SetClip(nullptr);
            rootClip_->Release();
            rootClip_ = nullptr;
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
        runtime_log::writef(L"[dcomp] device create failed");
        return false;
    }
    if (!ensureSwapchain(hwnd, width, height)) {
        runtime_log::writef(L"[dcomp] swapchain bind failed (hwnd=%p %dx%d)", hwnd, width, height);
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
        runtime_log::writef(L"[dcomp] Present failed: 0x%08X", static_cast<unsigned>(hr));
        if (sharedDevice_ && isDeviceLost(hr))
            sharedDevice_->invalid = true;
        return false;
    }
    if (dcomp_)
        dcomp_->Commit();
    return true;
}

void DCompRenderer::releaseLyricTransitionLayers() {
    auto releaseLayer = [&](LyricLayer& layer) {
        if (visual_ && layer.visual)
            visual_->RemoveVisual(layer.visual);
        if (layer.visual) {
            layer.visual->SetContent(nullptr);
            layer.visual->SetEffect(nullptr);
            layer.visual->SetClip(nullptr);
            layer.visual->Release();
            layer.visual = nullptr;
        }
        if (layer.opacity) {
            layer.opacity->Release();
            layer.opacity = nullptr;
        }
        if (layer.clip) {
            layer.clip->Release();
            layer.clip = nullptr;
        }
        if (layer.surface) {
            layer.surface->Release();
            layer.surface = nullptr;
        }
        layer.width = 0;
        layer.height = 0;
    };

    releaseLayer(transitionBackdrop_);
    for (auto& layer : lyricLayers_) {
        releaseLayer(layer);
    }
}

bool DCompRenderer::ensureLyricTransitionLayers(int width0, int height0, int width1,
                                                 int height1, float cornerRadius) {
    return ensureLyricTransitionLayers(width0, height0, width1, height1, 0, 0, 0, 0,
                                       cornerRadius);
}

bool DCompRenderer::ensureLyricTransitionLayers(int width0, int height0, int width1,
                                                 int height1, int width2, int height2,
                                                 float cornerRadius) {
    return ensureLyricTransitionLayers(width0, height0, width1, height1, width2, height2,
                                       0, 0, cornerRadius);
}

bool DCompRenderer::ensureLyricTransitionLayers(int width0, int height0, int width1,
                                                 int height1, int width2, int height2,
                                                 int width3, int height3, float cornerRadius) {
    if (!dcomp_ || !visual_ || width0 <= 0 || height0 <= 0 || width1 <= 0 || height1 <= 0)
        return false;

    const auto validOptionalLayer = [](int width, int height) {
        return (width == 0 && height == 0) || (width > 0 && height > 0);
    };
    if (!validOptionalLayer(width2, height2) || !validOptionalLayer(width3, height3))
        return false;
    const bool hasThirdLayer = width2 > 0 && height2 > 0;
    const bool hasFourthLayer = width3 > 0 && height3 > 0;
    if (hasFourthLayer && !hasThirdLayer)
        return false;

    const int widths[4] = {width0, width1, width2, width3};
    const int heights[4] = {height0, height1, height2, height3};
    const int layerCount = hasFourthLayer ? 4 : hasThirdLayer ? 3 : 2;
    bool reusable = true;
    for (int i = 0; i < 4; ++i) {
        if (i < layerCount) {
            reusable = reusable && lyricLayers_[i].surface &&
                       lyricLayers_[i].width == widths[i] &&
                       lyricLayers_[i].height == heights[i];
        } else {
            reusable = reusable && !lyricLayers_[i].surface;
        }
    }
    if (reusable) {
        // 复用已有层：裁剪、位移和透明度可能被上一次动画改过，统一恢复到
        // 挂载初始状态，避免新一轮转场首帧继承旧动画的终点。
        for (int i = 0; i < layerCount; ++i) {
            auto& layer = lyricLayers_[i];
            if (FAILED(configureRectangleClip(layer.clip, 0.0f, 0.0f,
                                              static_cast<float>(widths[i]),
                                              static_cast<float>(heights[i]), cornerRadius)))
                return false;
            if (FAILED(layer.visual->SetOffsetX(0.0f)) ||
                FAILED(layer.visual->SetOffsetY(0.0f)) ||
                FAILED(layer.opacity->SetOpacity(0.0f)))
                return false;
        }
        return true;
    }

    releaseLyricTransitionLayers();
    for (int i = 0; i < layerCount; ++i) {
        auto& layer = lyricLayers_[i];
        HRESULT hr = dcomp_->CreateVisual(&layer.visual);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateSurface(static_cast<UINT>(widths[i]),
                                       static_cast<UINT>(heights[i]),
                                       DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                       &layer.surface);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateEffectGroup(&layer.opacity);
        if (SUCCEEDED(hr))
            hr = layer.opacity->SetOpacity(0.0f);
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateRectangleClip(&layer.clip);
        if (SUCCEEDED(hr))
            hr = configureRectangleClip(layer.clip, 0.0f, 0.0f,
                                        static_cast<float>(widths[i]),
                                        static_cast<float>(heights[i]), cornerRadius);
        if (SUCCEEDED(hr))
            hr = layer.visual->SetClip(layer.clip);
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
        layer.width = widths[i];
        layer.height = heights[i];
    }
    return true;
}

bool DCompRenderer::ensureLyricTransitionBackdrop(int width, int height, float offsetY,
                                                   float cornerRadius) {
    if (!dcomp_ || !visual_ || !lyricLayers_[0].visual || !lyricLayers_[1].visual ||
        width <= 0 || height <= 0)
        return false;

    if (transitionBackdrop_.surface && transitionBackdrop_.visual &&
        transitionBackdrop_.opacity && transitionBackdrop_.width == width &&
        transitionBackdrop_.height == height && transitionBackdrop_.clip) {
        return SUCCEEDED(transitionBackdrop_.visual->SetOffsetX(0.0f)) &&
               SUCCEEDED(transitionBackdrop_.visual->SetOffsetY(offsetY)) &&
               SUCCEEDED(transitionBackdrop_.opacity->SetOpacity(0.0f)) &&
               SUCCEEDED(configureRectangleClip(transitionBackdrop_.clip, 0.0f, 0.0f,
                                                static_cast<float>(width),
                                                static_cast<float>(height), cornerRadius));
    }

    auto releaseBackdrop = [&]() {
        if (visual_ && transitionBackdrop_.visual)
            visual_->RemoveVisual(transitionBackdrop_.visual);
        if (transitionBackdrop_.visual) {
            transitionBackdrop_.visual->SetContent(nullptr);
            transitionBackdrop_.visual->SetEffect(nullptr);
            transitionBackdrop_.visual->SetClip(nullptr);
            transitionBackdrop_.visual->Release();
            transitionBackdrop_.visual = nullptr;
        }
        if (transitionBackdrop_.clip) {
            transitionBackdrop_.clip->Release();
            transitionBackdrop_.clip = nullptr;
        }
        if (transitionBackdrop_.opacity) {
            transitionBackdrop_.opacity->Release();
            transitionBackdrop_.opacity = nullptr;
        }
        if (transitionBackdrop_.surface) {
            transitionBackdrop_.surface->Release();
            transitionBackdrop_.surface = nullptr;
        }
        transitionBackdrop_.width = 0;
        transitionBackdrop_.height = 0;
    };

    releaseBackdrop();
    HRESULT hr = dcomp_->CreateVisual(&transitionBackdrop_.visual);
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height),
                                   DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                   &transitionBackdrop_.surface);
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateRectangleClip(&transitionBackdrop_.clip);
    if (SUCCEEDED(hr))
        hr = configureRectangleClip(transitionBackdrop_.clip, 0.0f, 0.0f,
                                    static_cast<float>(width), static_cast<float>(height),
                                    cornerRadius);
    // 透明度初始为 0：挂载提交阶段背景层不可见，待动画提交阶段再转可见，
    // 避免新表面首帧在合成器里显示空白底色（与内容层的两段提交一致）。
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateEffectGroup(&transitionBackdrop_.opacity);
    if (SUCCEEDED(hr))
        hr = transitionBackdrop_.opacity->SetOpacity(0.0f);
    if (SUCCEEDED(hr))
        hr = transitionBackdrop_.visual->SetContent(transitionBackdrop_.surface);
    if (SUCCEEDED(hr))
        hr = transitionBackdrop_.visual->SetEffect(transitionBackdrop_.opacity);
    if (SUCCEEDED(hr))
        hr = transitionBackdrop_.visual->SetClip(transitionBackdrop_.clip);
    if (SUCCEEDED(hr))
        hr = transitionBackdrop_.visual->SetOffsetX(0.0f);
    if (SUCCEEDED(hr))
        hr = transitionBackdrop_.visual->SetOffsetY(offsetY);
    // 内容层以 AddVisual(TRUE, nullptr) 逐个插到所有兄弟层的最底部，因此当前
    // 活动内容层中索引最高的一层才是 z 序最底的一层。固定背景必须以它为参考
    // 插到它后面（insertAbove=FALSE），才能只覆盖根交换链、而不盖住转场内容；
    // 以顶部内容层为参考会把背景压在部分内容之上，收尾时就会出现内容弹出。
    if (SUCCEEDED(hr)) {
        IDCompositionVisual* bottomContent = lyricLayers_[3].visual
                                                  ? lyricLayers_[3].visual
                                                  : lyricLayers_[1].visual;
        hr = visual_->AddVisual(transitionBackdrop_.visual, FALSE, bottomContent);
    }
    if (FAILED(hr)) {
        releaseBackdrop();
        return false;
    }
    transitionBackdrop_.width = width;
    transitionBackdrop_.height = height;
    return true;
}

bool DCompRenderer::showLyricTransitionBackdrop() {
    if (!transitionBackdrop_.opacity)
        return false;
    return SUCCEEDED(transitionBackdrop_.opacity->SetOpacity(1.0f));
}

ID2D1DeviceContext* DCompRenderer::beginLyricTransitionBackdropDraw() {
    if (!transitionBackdrop_.surface)
        return nullptr;
    POINT offset{};
    ID2D1DeviceContext* dc = nullptr;
    HRESULT hr = transitionBackdrop_.surface->BeginDraw(
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

bool DCompRenderer::endLyricTransitionBackdropDraw(ID2D1DeviceContext* dc) {
    if (!transitionBackdrop_.surface || !dc)
        return false;
    HRESULT hr = transitionBackdrop_.surface->EndDraw();
    dc->Release();
    return SUCCEEDED(hr);
}

ID2D1DeviceContext* DCompRenderer::beginLyricLayerDraw(int index) {
    if (index < 0 || index >= 4 || !lyricLayers_[index].surface)
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
    if (index < 0 || index >= 4 || !lyricLayers_[index].surface || !dc)
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
    if (index < 0 || index >= 4 || !dcomp_ || !lyricLayers_[index].visual ||
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

bool DCompRenderer::animateLyricLayerX(int index, float fromX, float toX, float baseY,
                                       float fromOpacity, float toOpacity,
                                       float durationSec) {
    if (index < 0 || index >= 4 || !dcomp_ || !lyricLayers_[index].visual ||
        !lyricLayers_[index].opacity || durationSec <= 0.0f)
        return false;

    const double duration = static_cast<double>(durationSec);
    IDCompositionAnimation* offsetAnim = nullptr;
    IDCompositionAnimation* opacityAnim = nullptr;
    HRESULT hr = dcomp_->CreateAnimation(&offsetAnim);
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateAnimation(&opacityAnim);
    if (SUCCEEDED(hr))
        hr = addSmoothStep(offsetAnim, 0.0, duration, fromX, toX) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = offsetAnim->End(duration, toX);
    if (SUCCEEDED(hr))
        hr = addSmoothStep(opacityAnim, 0.0, duration, fromOpacity, toOpacity)
                     ? S_OK
                     : E_FAIL;
    if (SUCCEEDED(hr))
        hr = opacityAnim->End(duration, toOpacity);
    if (SUCCEEDED(hr))
        hr = lyricLayers_[index].visual->SetOffsetX(offsetAnim);
    if (SUCCEEDED(hr))
        hr = lyricLayers_[index].visual->SetOffsetY(baseY);
    if (SUCCEEDED(hr))
        hr = lyricLayers_[index].opacity->SetOpacity(opacityAnim);

    if (offsetAnim)
        offsetAnim->Release();
    if (opacityAnim)
        opacityAnim->Release();
    return SUCCEEDED(hr);
}

bool DCompRenderer::animateLyricLayerClipSlide(int index, float offsetX, float fromY,
                                               float toY, float clipFromBottom,
                                               float clipToBottom, float durationSec,
                                               float fromOpacity, float toOpacity) {
    if (index < 0 || index >= 4 || !dcomp_ || !lyricLayers_[index].visual ||
        !lyricLayers_[index].opacity || !lyricLayers_[index].clip || durationSec <= 0.0f)
        return false;

    auto& layer = lyricLayers_[index];
    const double duration = static_cast<double>(durationSec);
    IDCompositionAnimation* opacityAnim = nullptr;
    // 显式写入动画起点。尤其是收起时，层通常刚从隐藏的挂载状态进入动画，
    // 不能依赖上一批合成命令的基础透明度，否则 tab 可能在首帧短暂闪现。
    HRESULT hr = layer.opacity->SetOpacity(fromOpacity);
    if (SUCCEEDED(hr))
        hr = layer.visual->SetOffsetX(offsetX);
    if (fromY == toY) {
        if (SUCCEEDED(hr))
            hr = layer.visual->SetOffsetY(toY);
    } else {
        IDCompositionAnimation* offsetAnim = nullptr;
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateAnimation(&offsetAnim);
        if (SUCCEEDED(hr))
            hr = addSmoothStep(offsetAnim, 0.0, duration, fromY, toY) ? S_OK : E_FAIL;
        if (SUCCEEDED(hr))
            hr = offsetAnim->End(duration, toY);
        if (SUCCEEDED(hr))
            hr = layer.visual->SetOffsetY(offsetAnim);
        if (offsetAnim)
            offsetAnim->Release();
    }
    if (SUCCEEDED(hr))
        hr = layer.clip->SetLeft(0.0f);
    if (SUCCEEDED(hr))
        hr = layer.clip->SetTop(0.0f);
    if (SUCCEEDED(hr))
        hr = layer.clip->SetRight(static_cast<float>(layer.width));
    if (clipFromBottom == clipToBottom) {
        if (SUCCEEDED(hr))
            hr = layer.clip->SetBottom(clipToBottom);
    } else {
        IDCompositionAnimation* clipAnim = nullptr;
        if (SUCCEEDED(hr))
            hr = dcomp_->CreateAnimation(&clipAnim);
        if (SUCCEEDED(hr))
            hr = addSmoothStep(clipAnim, 0.0, duration, clipFromBottom, clipToBottom)
                     ? S_OK
                     : E_FAIL;
        if (SUCCEEDED(hr))
            hr = clipAnim->End(duration, clipToBottom);
        if (SUCCEEDED(hr))
            hr = layer.clip->SetBottom(clipAnim);
        if (clipAnim)
            clipAnim->Release();
    }
    if (SUCCEEDED(hr)) {
        if (fromOpacity == toOpacity) {
            hr = layer.opacity->SetOpacity(toOpacity);
        } else {
            hr = dcomp_->CreateAnimation(&opacityAnim);
            if (SUCCEEDED(hr)) {
                // 展开和收起使用同一条透明度曲线，并与位移、裁剪贯穿相同的
                // 动画时长。这样内容不会在展开时延迟出现，也不会在收起时提前消失，
                // 视觉上的淡入淡出速度保持一致。
                if (!addSmoothStep(opacityAnim, 0.0, duration, fromOpacity, toOpacity))
                    hr = E_FAIL;
            }
            if (SUCCEEDED(hr))
                hr = opacityAnim->End(duration, toOpacity);
            if (SUCCEEDED(hr))
                hr = layer.opacity->SetOpacity(opacityAnim);
        }
    }
    if (opacityAnim)
        opacityAnim->Release();
    return SUCCEEDED(hr);
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

void DCompRenderer::waitForCommitCompletion() {
    if (dcomp_)
        dcomp_->WaitForCommitCompletion();
}

bool DCompRenderer::setRootRoundedClip(float top, float bottom, float cornerRadius) {
    if (!dcomp_ || !visual_ || width_ <= 0 || height_ <= 0)
        return false;
    if (!rootClip_ && FAILED(dcomp_->CreateRectangleClip(&rootClip_)))
        return false;
    if (FAILED(configureRectangleClip(rootClip_, 0.0f, top, static_cast<float>(width_), bottom,
                                      cornerRadius)))
        return false;
    return SUCCEEDED(visual_->SetClip(rootClip_));
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
    if (visual_ && rootClip_)
        visual_->SetClip(nullptr);
    release(rootOpacity_);
    release(rootClip_);
    release(visual_);
    release(target_);
    release(swapchain_);
    release(dcomp_);
    release(dc_);
    release(d2dDevice_);
    dxgiDevice_ = nullptr;
    d3d_ = nullptr;
    sharedDevice_.reset();
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
