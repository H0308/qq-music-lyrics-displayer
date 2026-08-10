#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

// OverlayHost 与 TaskbarHost 共用的 D2D/DWrite 基础设施：
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
