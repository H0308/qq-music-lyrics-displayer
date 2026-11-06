#pragma once
#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

// 封面解码与模糊链共用同一资源所有者；调用者只借用 bitmap()/blurred() 的结果。
class CoverImage {
public:
    ID2D1Bitmap* bitmap() const { return bitmap_.Get(); }
    void decode(ID2D1DeviceContext* target, const std::vector<uint8_t>* bytes, UINT dpi,
                UINT targetPixels);
    ID2D1Effect* blurred(ID2D1DeviceContext* target, float width, float height);
    void releaseBackground() {
        scale_.Reset();
        blur_.Reset();
        blurInput_ = nullptr;
    }
    void discardDeviceResources() {
        releaseBackground();
        bitmap_.Reset();
    }

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    Microsoft::WRL::ComPtr<ID2D1Effect> blur_, scale_;
    ID2D1Bitmap* blurInput_ = nullptr; // bitmap_ 持有，只用于判断是否需要重新绑定。
};
