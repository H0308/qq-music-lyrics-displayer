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
void draw(ID2D1DeviceContext* target, const Geometry& geometry, int index, bool playing,
          const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush);

} // namespace media_control
