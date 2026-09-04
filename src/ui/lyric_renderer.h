#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwrite.h>

#include <memory>

struct DCompSharedDevice;

// TaskbarHost 使用的 D2D/DWrite 基础设施：
// 工厂创建、DIB/MemoryDC 管理、DCRenderTarget 绑定、UpdateLayeredWindow 提交。
class LyricRenderer {
public:
    LyricRenderer();
    ~LyricRenderer();

    bool initialize();
    void setDpi(UINT dpi);

    ID2D1DCRenderTarget* renderTarget();
    ID2D1Factory* d2d() const;
    IDWriteFactory* dwrite() const;

    // 确保 MemoryDC / DIB 尺寸匹配并绑定到 RenderTarget。
    bool bindDC(int width, int height);

    // 将当前 MemoryDC 内容提交到分层窗口。
    bool present(HWND hwnd);

    // 将当前 MemoryDC 内容用 BitBlt 提交到普通窗口 DC（用于对话框预览等非分层窗口）
    bool copyToDC(HDC hdc, int width, int height);

    // 把当前帧按源 alpha 合成到外部 DC 的 (x,y)（设置窗口拖动时拼整窗快照用；
    // D2D 输出与 UpdateLayeredWindow 同为预乘 alpha，与 AC_SRC_ALPHA 语义一致）
    bool compositeTo(HDC dst, int x, int y);

    // 丢弃 RenderTarget（设备丢失时），保留工厂与 MemoryDC。
    void discard();

    // 释放全部资源。
    void releaseAll();

private:
    bool createFactories();
    bool createRenderTarget();

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    HDC memdc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int bmpW_ = 0;
    int bmpH_ = 0;
    UINT dpi_ = 96;
    bool rtBound_ = false;
};

// TaskbarHost 的 GPU 渲染管线：D2D 画到 DXGI 组合交换链，
// 由 DirectComposition 直接交给 DWM 合成，替代 DIB + UpdateLayeredWindow 的
// 每帧整图拷贝。对外接口与 LyricRenderer 对齐（renderTarget/bind/present/discard），
// 绘制代码无需感知差异。
class DCompRenderer {
public:
    DCompRenderer();
    ~DCompRenderer();

    bool initialize();
    void setDpi(UINT dpi);

    // 设备上下文，作为 ID2D1RenderTarget 使用；调用前必须先 bind()。
    ID2D1DeviceContext* renderTarget();
    ID2D1Factory* d2d() const;
    IDWriteFactory* dwrite() const;

    // 确保交换链/Visual 与窗口尺寸匹配并绑定后备缓冲（BeginDraw 前调用）。
    bool bind(HWND hwnd, int width, int height);

    // 提交交换链并经 DComposition 上屏；设备丢失返回 false，调用方走 discard。
    bool present();

    // 设备丢失：释放设备链全部对象，下次 bind() 惰性重建；保留工厂。
    void discard();

    // 单行歌词转场用的两个临时合成层（旧行/新行），内容只在转场开始时绘制一次，
    // 位移和透明度由 DirectComposition 按刷新率执行。两层可指定不同尺寸。
    // cornerRadius 为物理像素；默认为 0，保留歌词行/快速展开层的矩形边界。
    bool ensureLyricTransitionLayers(int width0, int height0, int width1, int height1,
                                     float cornerRadius = 0.0f);
    ID2D1DeviceContext* beginLyricLayerDraw(int index);
    bool endLyricLayerDraw(int index, ID2D1DeviceContext* dc);
    bool animateLyricLayer(int index, float fromY, float toY, float fromOpacity,
                           float toOpacity, float durationSec);
    // 页面横向滑动转场（媒体卡片 ⇄ 空闲面板）：内容已画入层，只让合成器
    // 按刷新率做 X 位移和透明度过渡；层定位在 (0, baseY)。
    bool animateLyricLayerX(int index, float fromX, float toX, float baseY,
                            float fromOpacity, float toOpacity, float durationSec);
    // 页面横向滑动和快速展开转场的固定背景层。背景与内容层一起提交，避免
    // 转场期间重绘/提交根交换链。创建/复用时透明度为 0，由调用方在动画提交
    // 阶段调 showLyricTransitionBackdrop() 转可见。
    bool ensureLyricTransitionBackdrop(int width, int height, float offsetY,
                                        float cornerRadius = 0.0f);
    ID2D1DeviceContext* beginLyricTransitionBackdropDraw();
    bool endLyricTransitionBackdropDraw(ID2D1DeviceContext* dc);
    // 两段提交的第二阶段：背景层从挂载时的 0 透明度转为可见。
    bool showLyricTransitionBackdrop();
    // 区域覆盖动画（快速打开展开/收起）：层固定在 (offsetX, fromY→toY)，
    // 同时矩形裁剪底边和层透明度分别从起点动画到终点（像素，层内坐标）。
    bool animateLyricLayerClipSlide(int index, float offsetX, float fromY, float toY,
                                    float clipFromBottom, float clipToBottom,
                                    float durationSec, float fromOpacity = 1.0f,
                                    float toOpacity = 1.0f);
    void clearLyricTransitionLayers();
    // 弹出式宿主的根视觉动画：只改变合成器位移和透明度，不触发布局。
    bool animateRoot(float fromX, float toX, float fromY, float toY,
                     float fromOpacity, float toOpacity, float durationSec);
    void resetRoot();
    // 给弹出卡片的根视觉设置物理像素圆角裁剪；未调用时根视觉保持矩形。
    bool setRootRoundedClip(float top, float bottom, float cornerRadius);
    void commit();
    // 等待上一批 DComposition 命令被合成器处理完，用于交换链与覆盖层交接。
    void waitForCommitCompletion();

    // 释放全部资源。
    void releaseAll();

private:
    struct LyricLayer {
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
    void releaseLyricTransitionLayers();
    bool addSmoothStep(IDCompositionAnimation* animation, double begin, double end, float from,
                       float to);

    ID2D1Factory1* d2d1_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID3D11Device* d3d_ = nullptr;
    IDXGIDevice* dxgiDevice_ = nullptr;
    // sharedDevice_ 持有 D3D11/DXGI 设备；窗口级 D2D 和合成资源仍由本实例独立管理。
    std::shared_ptr<DCompSharedDevice> sharedDevice_;
    ID2D1Device* d2dDevice_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
    IDCompositionDevice* dcomp_ = nullptr;
    IDCompositionTarget* target_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
    IDCompositionRectangleClip* rootClip_ = nullptr;
    IDCompositionEffectGroup* rootOpacity_ = nullptr;
    LyricLayer transitionBackdrop_;
    LyricLayer lyricLayers_[2];
    ID2D1Bitmap1* backBmp_ = nullptr;
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    UINT dpi_ = 96;
};
