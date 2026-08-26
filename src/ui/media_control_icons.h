#pragma once

#include <d2d1_1.h>

namespace media_control {

// 任务栏内嵌控件与媒体弹窗共用的播放控制图标几何。
struct Geometry {
    ID2D1PathGeometry* play = nullptr;
    ID2D1PathGeometry* previous = nullptr;
    ID2D1PathGeometry* next = nullptr;
};

void release(Geometry& geometry);
bool create(ID2D1Factory* factory, Geometry& geometry);

// index: 0=上一首，1=播放/暂停，2=下一首。
void draw(ID2D1RenderTarget* target, const Geometry& geometry, int index, bool playing,
          const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush);

// 音量图标（不依赖 Geometry，按 level 现画；接受 ID2D1RenderTarget，
// DCRenderTarget（LyricRenderer）与 DeviceContext（DCompRenderer）都可传入）：
// level 0=静音（喇叭+叉），1=无声（仅喇叭），2=小声（一道波），3=大声（两道波）。
void drawVolume(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float radius,
                ID2D1Brush* brush, int level);

} // namespace media_control
