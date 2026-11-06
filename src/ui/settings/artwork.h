#pragma once
#include "ui/fluent_dialog_surface.h"
#include "ui/lyric_star.h"
#include "ui/spectrum_effects.h"
#include "ui/lyric_effects.h"

// 设置卡片的视觉示例。仅依赖绘制参数，不读取或修改窗口与业务状态。
class SettingsArtwork {
public:
    void drawSpectrumStyleArtwork(fluent::FluentDialogSurface::Painter& painter,
                                  const D2D1_RECT_F& bounds, int style, bool enabled);
    void drawSpectrumEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                   const D2D1_RECT_F& bounds, SpectrumEffect effect, bool enabled,
                                   bool gradient = false);
    void drawLyricEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                const D2D1_RECT_F& bounds, LyricEffect effect, bool enabled);
    void drawSpectrumBackgroundArtwork(fluent::FluentDialogSurface::Painter& painter,
                                       const D2D1_RECT_F& bounds, bool enabled);
    void drawCoverEffectArtwork(fluent::FluentDialogSurface::Painter& painter,
                                const D2D1_RECT_F& bounds, bool vinyl, bool enabled);
    void drawSongToastPositionArtwork(fluent::FluentDialogSurface::Painter& painter,
                                      const D2D1_RECT_F& bounds, bool top, bool enabled);
    void drawHoverControlStyleArtwork(fluent::FluentDialogSurface::Painter& painter,
                                      const D2D1_RECT_F& bounds, bool mediaCard, bool enabled);
    void drawIdleQuoteBackgroundArtwork(fluent::FluentDialogSurface::Painter& painter,
                                        const D2D1_RECT_F& bounds, int style, bool enabled);

private:
    LyricStarShape lyricStarArtwork_;
};
