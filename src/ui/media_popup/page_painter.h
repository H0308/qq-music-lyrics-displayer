#pragma once
#include "ui/media_popup/page_layout.h"
#include "ui/media_popup/transitions.h"
#include "ui/media_control_icons.h"
#include "ui/dock_pet.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <array>
#include <span>

namespace media_popup_paint {
using PopupPage = media_popup_detail::Page;
using namespace media_popup_detail;
// 本次绘制所借用的页面数据与公共资源；不保存窗口或拥有者指针。
struct PagePaintResources {
    std::span<ID2D1Bitmap* const> idleIconBitmaps;
    DockPet& pet;
    ID2D1SolidColorBrush* brushStroke;
    ID2D1SolidColorBrush* brushText;
    ID2D1SolidColorBrush* brushSecondary;
    ID2D1SolidColorBrush* brushDisabled;
    ID2D1SolidColorBrush* brushProgressTrack;
    ID2D1SolidColorBrush* brushControl;
    ID2D1SolidColorBrush* brushControlHover;
    ID2D1SolidColorBrush* brushControlPressed;
    ID2D1SolidColorBrush* brushIdleCell;
    ID2D1SolidColorBrush* brushIdleCellHover;
    ID2D1SolidColorBrush* brushAccent;
    ID2D1SolidColorBrush* brushAccentHover;
    ID2D1SolidColorBrush* brushTextOnAccent;
    ID2D1SolidColorBrush* brushTaskPriorityHigh;
    ID2D1SolidColorBrush* brushTaskPriorityMedium;
    ID2D1SolidColorBrush* brushTaskPriorityLow;
    ID2D1SolidColorBrush* brushTaskPriorityNone;
    ID2D1LinearGradientBrush* brushMediaTextEdgeFade;
    ID2D1LinearGradientBrush* brushIdleTextEdgeFade;
    ID2D1Layer* textEdgeFadeLayer;
    IDWriteTextFormat* fmtSource;
    IDWriteTextFormat* fmtTimeRight;
    IDWriteTextFormat* fmtTitle;
    IDWriteTextFormat* fmtArtist;
    IDWriteTextFormat* fmtIcon;
    IDWriteTextFormat* fmtIdleHeader;
    IDWriteTextFormat* fmtIdleQuote;
    IDWriteTextFormat* fmtIdleSource;
    IDWriteTextFormat* fmtIdleApp;
    IDWriteTextLayout* titleLayout;
    IDWriteTextLayout* artistLayout;
    IDWriteTextLayout* idleQuoteLayout;
    IDWriteTextLayout* idleQuoteSourceLayout;
    ID2D1Bitmap* coverBmp;
    ID2D1Bitmap* sourceIconBmp;
    const std::wstring& sourceIconLoadedFor;
    ID2D1RoundedRectangleGeometry* coverClip;
    ID2D1Layer* coverLayer;
    const media_control::Geometry& controlGeometry;
};
struct PagePaintMedia {
    const media_popup_detail::PageSnapshot& categorySnapshot;
    const OverlayMediaInfo& media;
    int64_t positionMs;
    float titleWidth;
    float titleHeight;
    float artistWidth;
    float artistHeight;
    float titleScrollOffset;
    float artistScrollOffset;
};
struct PagePaintIdle {
    const media_popup_detail::QuickExpansion& quickExpansion;
    const IdlePresentation& idle;
    float idleScrollOffset;
    float idleScrollMax;
    IdleQuickTab idleQuickTab;
    float idleQuoteWidth;
    float idleQuoteHeight;
    float idleQuoteSourceWidth;
    float idleQuoteSourceHeight;
    float idleQuoteScrollOffset;
    float idleQuoteSourceScrollOffset;
};
struct PagePaintInteraction {
    int hoverButton;
    int pressedButton;
    bool hoverCopy;
    bool pressedCopy;
    bool copySucceeded;
    int hoverIdleApp;
    int hoverIdleTask;
    int hoverIdleTaskCheckbox;
    int pressedIdleTask;
    int pressedIdleTaskCheckbox;
    int hoverIdleQuickTab;
    int pressedIdleQuickTab;
    bool hoverIdleQuickExpand;
    bool pressedIdleQuickExpand;
};
struct PagePaintGeometry {
    float cardOriginDip;
    D2D1_RECT_F& copyRect;
    D2D1_RECT_F idleListRect;
    D2D1_RECT_F idleScrollTrackRect;
    D2D1_RECT_F idleScrollThumbRect;
    D2D1_RECT_F& idleQuickExpandRect;
    D2D1_RECT_F (&idleQuickTabRects)[2];
    const MediaPageLayout& mediaLayout_;
};
struct PagePaintState {
    bool available;
    bool playbackScene;
    bool idlePanelManual;
    const media_popup_detail::LayerTransition& categoryTransition;
};
struct PagePaintContext {
    IDWriteFactory* factory;
    std::array<bool, 2> contrast, frosted;
    std::array<D2D1_COLOR_F, 2> cardFills;
    PagePaintResources resources;
    PagePaintMedia media;
    PagePaintIdle idle;
    PagePaintInteraction interaction;
    PagePaintGeometry geometry;
    PagePaintState state;
};
IdlePageLayout layoutFor(const PagePaintContext& context, const IdlePresentation& content);
std::wstring sourceLabel(const std::wstring& source);
std::wstring formatPlaybackTime(int64_t milliseconds);
std::wstring formatPlaybackDuration(int64_t milliseconds);
bool buttonEnabled(bool available, int index, const OverlayMediaInfo& content);
bool idleCopyAvailable(const IdlePresentation& content);
bool idleReturnArrowVisible(const IdlePresentation& content, bool playbackScene, bool available,
                            bool manual);
float idleUnitHeight(const IdlePresentation& content, bool current, const IdlePresentation& idle,
                     IDWriteTextLayout* layout, float height);
void drawText(ID2D1DeviceContext* rt, const std::wstring& text, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, ID2D1Brush* brush);
void drawScrollingText(const PagePaintContext& context, ID2D1DeviceContext* rt,
                       IDWriteTextLayout* layout, float textWidth, float textHeight,
                       const D2D1_RECT_F& rect, float offset, ID2D1Brush* brush,
                       ID2D1Brush* edgeFadeBrush);
void drawChevron(ID2D1DeviceContext* rt, D2D1_POINT_2F center, float size,
                 ID2D1Brush* brush, bool right = true);
void drawVerticalChevron(ID2D1DeviceContext* rt, D2D1_POINT_2F center, float size,
                         ID2D1Brush* brush, bool up);
bool usesBlackTextForControls(const PagePaintContext& context, PopupPage page);
D2D1_COLOR_F adaptiveControlFill(const PagePaintContext& context, bool pressed, PopupPage page);
D2D1_COLOR_F staticControlFill(const PagePaintContext& context, bool pressed, PopupPage page);
void drawAdaptiveControlSurface(const PagePaintContext& context, ID2D1DeviceContext* rt,
                                const D2D1_RECT_F& rect, float radius, bool pressed, PopupPage page,
                                bool hoverSurface = true);
void drawCopyButton(const PagePaintContext& context, ID2D1DeviceContext* rt,
                    const D2D1_RECT_F& rect, bool hovered, bool pressed, bool succeeded);
void drawIdleQuickHeader(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                         float headerTop, const IdlePresentation& content, bool updateHitTest);
void drawIdleQuickDivider(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                          float headerTop, const IdlePresentation& content);
void drawIdleQuoteText(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                       const IdlePresentation& content, float quoteHeight,
                       IDWriteTextLayout* layout, float textWidth, float textHeight,
                       float scrollOffset, IDWriteTextLayout* sourceLayout, float sourceWidth,
                       float sourceHeight, float sourceScrollOffset);
void drawIdleQuoteUnit(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                       const IdlePresentation& content, bool current);
void drawIdleCopyButton(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                        const IdlePresentation& content, bool updateHitTest);
void drawIdleQuickExpandButton(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                               float headerTop, const IdlePresentation& content,
                               bool updateHitTest);
void drawIdleQuickTabs(const PagePaintContext& context, ID2D1DeviceContext* rt, float w, float top,
                       float progress, const IdlePresentation& content, bool updateHitTest);
void drawIdleTodayTasks(const PagePaintContext& context, ID2D1DeviceContext* rt,
                        const IdlePresentation& content, const D2D1_RECT_F* paintBounds = nullptr);
void drawIdleQuickListContent(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                              const IdlePresentation& content,
                              const D2D1_RECT_F* paintBounds = nullptr);
void drawMediaTitles(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                     IDWriteTextLayout* title, float titleW, float titleH, float titleOffset,
                     IDWriteTextLayout* artist, float artistW, float artistH, float artistOffset);
void drawMediaPet(const PagePaintContext& context, ID2D1DeviceContext* rt, float w);
void drawMediaContent(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                      bool useCachedLayers);
void drawMediaSnapshot(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                       const OverlayMediaInfo& snapshot, bool useCachedLayers);
void drawSourceIcon(const PagePaintContext& context, ID2D1DeviceContext* rt,
                    const std::wstring& source);
void drawSource(const PagePaintContext& context, ID2D1DeviceContext* rt);
void drawProgress(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                  const OverlayMediaInfo& content);
void drawButton(const PagePaintContext& context, ID2D1DeviceContext* rt, int index,
                const OverlayMediaInfo& content);
void drawCover(const PagePaintContext& context, ID2D1DeviceContext* rt, bool useCachedLayer);
bool createMeasuredTextLayout(IDWriteFactory* factory, const std::wstring& text,
                              IDWriteTextFormat* format, float layoutHeight,
                              IDWriteTextLayout** out, float& width, float& height);

} // namespace media_popup_paint
