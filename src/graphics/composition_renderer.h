#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwrite.h>

#include <memory>
#include <cstdint>

struct CompositionSharedDevice;

// 窗口的 GPU 渲染与合成基础设施：D2D 画到 DXGI 组合交换链或合成表面，
// 由 DirectComposition 提交到 DWM，并管理合成图层、动画与设备资源。
class CompositionRenderer {
public:
    explicit CompositionRenderer(bool contentAboveChildren = true);
    ~CompositionRenderer();

    bool initialize();
    void setDrawingDeviceSource(CompositionRenderer* source);
    void setDpi(UINT dpi);

    // 设备上下文，作为 ID2D1RenderTarget 使用；调用前必须先 bind()。
    ID2D1DeviceContext* renderTarget();
    ID2D1Factory* d2d() const;
    IDWriteFactory* dwrite() const;

    // 确保交换链/Visual 与窗口尺寸匹配并绑定后备缓冲（BeginDraw 前调用）。
    struct BindTimings {
        uint64_t factoriesMs = 0;
        uint64_t deviceMs = 0;
        uint64_t surfaceMs = 0;
    };
    bool bind(HWND hwnd, int width, int height, BindTimings* timings = nullptr);

    // 合成表面绘制：返回的上下文已开始绘制，初始变换包含表面分配偏移。
    // 与 bind()/present() 二选一；结束绘制后调用方更新视觉属性，再统一 commit()。
    ID2D1DeviceContext* beginSurfaceDraw(HWND hwnd, int width, int height);
    bool endSurfaceDraw(ID2D1DeviceContext* dc);

    // 每次成功创建窗口级设备链后递增；调用方据此重建设备相关的画刷、位图与图层。
    uint64_t deviceGeneration() const { return deviceGeneration_; }

    // 提交交换链并经 DComposition 上屏；设备丢失返回 false，调用方走 discard。
    bool present();

    // 用原生分层窗口的实时表面替换根内容，保留子控件绘制，由根视觉统一转场。
    bool setWindowContent(HWND source);

    // 隐藏后释放窗口表面和转场层，保留所属线程的设备及工厂供下次显示复用。
    // 合成树变更由调用方通过 commit() 随当前事务提交。
    void releaseWindowResources();

    // 完整释放设备链；健康合成设备的拥有者先提交撤层，下次 bind() 惰性重建。
    // 保留工厂；设备丢失时标记共享设备失效，不继续提交或复用旧设备。
    void discard();

    // 临时合成层支持两到四层，内容只在转场开始时绘制一次。
    // 位移和透明度由 DirectComposition 按刷新率执行，各层可指定不同尺寸。
    // cornerRadius 为物理像素；默认为 0，保留图层的矩形边界。
    bool ensureTransitionLayers(int width0, int height0, int width1, int height1,
                                     float cornerRadius = 0.0f);
    bool ensureTransitionLayers(int width0, int height0, int width1, int height1,
                                     int width2, int height2, float cornerRadius = 0.0f);
    bool ensureTransitionLayers(int width0, int height0, int width1, int height1,
                                     int width2, int height2, int width3, int height3,
                                     float cornerRadius = 0.0f);
    ID2D1DeviceContext* beginTransitionLayerDraw(int index);
    bool endTransitionLayerDraw(int index, ID2D1DeviceContext* dc);
    bool animateTransitionLayer(int index, float fromY, float toY, float fromOpacity,
                           float toOpacity, float durationSec);
    // 横向滑动转场：内容已画入层，只让合成器
    // 按刷新率做 X 位移和透明度过渡；层定位在 (0, baseY)。
    bool animateTransitionLayerX(int index, float fromX, float toX, float baseY,
                            float fromOpacity, float toOpacity, float durationSec,
                            float fixedClipRight = 0.0f);
    // 页面横向滑动转场的固定背景层。背景与内容层一起提交，避免转场期间
    // 重绘/提交根交换链。创建/复用时透明度为 0，由调用方在动画提交阶段
    // 调 showTransitionBackdrop() 转可见。
    bool ensureTransitionBackdrop(int width, int height, float offsetY,
                                        float cornerRadius = 0.0f);
    ID2D1DeviceContext* beginTransitionBackdropDraw();
    bool endTransitionBackdropDraw(ID2D1DeviceContext* dc);
    // 两段提交的第二阶段：背景层从挂载时的 0 透明度转为可见。
    bool showTransitionBackdrop();
    // 区域覆盖动画：层固定在 (offsetX, fromY→toY)，
    // 同时矩形裁剪底边和层透明度分别从起点动画到终点（像素，层内坐标）。
    bool animateTransitionLayerClipSlide(int index, float offsetX, float fromY, float toY,
                                    float clipFromBottom, float clipToBottom,
                                    float durationSec, float fromOpacity = 1.0f,
                                    float toOpacity = 1.0f);
    void clearTransitionLayers();
    // 弹出式宿主的根视觉动画：只改变合成器位移和透明度，不触发布局。
    enum class RootAnimationCurve { SmoothStep, EaseOutQuadratic, EaseOutCubic };
    bool animateRoot(float fromX, float toX, float fromY, float toY,
                     float fromOpacity, float toOpacity, float durationSec,
                     RootAnimationCurve curve = RootAnimationCurve::SmoothStep);
    void resetRoot();
    // 直接设置根视觉透明度（取消其上的透明度动画），不改变位移。
    void setRootOpacity(float opacity);
    void setRootOffsetY(float offset);
    bool animateRootScroll(float from, float to, float velocity, float frequency);
    // 给窗口的根视觉设置物理像素圆角裁剪；未调用时根视觉保持矩形。
    bool setRootRoundedClip(float top, float bottom, float cornerRadius);
    bool commit();
    // 等待上一批 DComposition 命令被合成器处理完，用于交换链与覆盖层交接。
    void waitForCommitCompletion();

    // 释放全部资源。
    void releaseAll();

private:
    struct TransitionLayer {
        IDCompositionVisual* visual = nullptr;
        IDCompositionSurface* surface = nullptr;
        IDCompositionEffectGroup* opacity = nullptr;
        IDCompositionRectangleClip* clip = nullptr;
        int width = 0;
        int height = 0;
    };

    bool createFactories();
    bool createDevice();
    bool ensureSwapchain(HWND hwnd, int width, int height);
    void releaseBackBuffer();
    void releaseTransitionLayers();
    bool addSmoothStep(IDCompositionAnimation* animation, double begin, double end, float from,
                       float to);

    ID2D1Factory1* d2d1_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID3D11Device* d3d_ = nullptr;
    IDXGIDevice* dxgiDevice_ = nullptr;
    // sharedDevice_ 持有 D3D11/DXGI 设备；窗口级 D2D 和合成资源仍由本实例独立管理。
    std::shared_ptr<CompositionSharedDevice> sharedDevice_;
    uint64_t deviceGeneration_ = 0;
    ID2D1Device* d2dDevice_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
    IDCompositionSurface* surface_ = nullptr;
    IDCompositionDevice* dcomp_ = nullptr;
    IDCompositionTarget* target_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
    IDCompositionRectangleClip* rootClip_ = nullptr;
    IDCompositionEffectGroup* rootOpacity_ = nullptr;
    TransitionLayer transitionBackdrop_;
    TransitionLayer transitionLayers_[4];
    ID2D1Bitmap1* backBmp_ = nullptr;
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    UINT dpi_ = 96;
    bool contentAboveChildren_ = true;
    CompositionRenderer* drawingDeviceSource_ = nullptr;
};
