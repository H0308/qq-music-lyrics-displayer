#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

// 基于内存位图的 D2D/DWrite 绘图基础设施：
// 管理工厂、DIB/MemoryDC、DC 绘图目标，以及位图复制和分层窗口提交。
class BitmapRenderer {
public:
    BitmapRenderer();
    ~BitmapRenderer();

    bool initialize();
    void setDpi(UINT dpi);

    ID2D1DCRenderTarget* renderTarget();
    ID2D1Factory* d2d() const;
    IDWriteFactory* dwrite() const;

    // 确保 MemoryDC / DIB 尺寸匹配并绑定到 RenderTarget。
    bool bindDC(int width, int height);

    // 将当前 MemoryDC 内容提交到分层窗口。
    bool present(HWND hwnd, BYTE alpha = 255);

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
