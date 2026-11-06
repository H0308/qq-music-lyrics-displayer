#include "ui/media_popup/page_painter.h"
#include "ui/fluent_theme.h"
#include "util/com_release.h"
#include <algorithm>
#include <cmath>

namespace media_popup_paint {
void drawMediaTitles(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                     IDWriteTextLayout* title, float titleW, float titleH, float titleOffset,
                     IDWriteTextLayout* artist, float artistW, float artistH, float artistOffset) {
    auto& brushText = context.resources.brushText;
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushMediaTextEdgeFade = context.resources.brushMediaTextEdgeFade;

    drawScrollingText(context, rt, title, titleW, titleH, context.geometry.mediaLayout_.title,
                      titleOffset, brushText, brushMediaTextEdgeFade);
    drawScrollingText(context, rt, artist, artistW, artistH, context.geometry.mediaLayout_.artist,
                      artistOffset, brushSecondary, brushMediaTextEdgeFade);
}

void drawMediaPet(const PagePaintContext& context, ID2D1DeviceContext* rt, float w) {
    auto& pet = context.resources.pet;
    auto& brushText = context.resources.brushText;

    if (!rt)
        return;
    const float laneRight = w - kPopupTextRightPaddingDip;
    pet.setLane(laneRight - kMediaPetZoneWidthDip, laneRight, kMediaPetLaneHeightDip);
    pet.tick(GetTickCount64());
    // Zzz 颜色跟随卡片当前文字色（含专辑色背景下的自动对比度）。
    if (brushText) {
        const D2D1_COLOR_F text = brushText->GetColor();
        pet.setLightTheme(0.2126f * text.r + 0.7152f * text.g + 0.0722f * text.b < 0.5f);
    }
    D2D1_MATRIX_3X2_F saved{};
    rt->GetTransform(&saved);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, kMediaPetLaneTopDip) * saved);
    pet.draw(rt);
    rt->SetTransform(saved);
}

void drawMediaContent(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                      bool useCachedLayers) {
    auto& media = context.media.media;
    auto& titleLayout = context.resources.titleLayout;
    auto& artistLayout = context.resources.artistLayout;
    auto& titleWidth = context.media.titleWidth;
    auto& titleHeight = context.media.titleHeight;
    auto& artistWidth = context.media.artistWidth;
    auto& artistHeight = context.media.artistHeight;
    auto& titleScrollOffset = context.media.titleScrollOffset;
    auto& artistScrollOffset = context.media.artistScrollOffset;

    if (!rt)
        return;
    drawSource(context, rt);
    drawCover(context, rt, useCachedLayers);
    drawMediaTitles(context, rt, w, titleLayout, titleWidth, titleHeight, titleScrollOffset,
                    artistLayout, artistWidth, artistHeight, artistScrollOffset);
    drawMediaPet(context, rt, w);
    drawProgress(context, rt, w, media);
    for (int i = 0; i < 3; ++i)
        drawButton(context, rt, i, media);
}

void drawMediaSnapshot(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                       const OverlayMediaInfo& snapshot, bool useCachedLayers) {
    auto& categorySnapshot = context.media.categorySnapshot;
    auto& brushText = context.resources.brushText;
    auto& fmtSource = context.resources.fmtSource;
    auto& fmtTitle = context.resources.fmtTitle;
    auto& fmtArtist = context.resources.fmtArtist;
    auto& titleScrollOffset = context.media.titleScrollOffset;
    auto& artistScrollOffset = context.media.artistScrollOffset;

    if (!rt)
        return;
    // 类别转场开始前保存的媒体字段可能已经被最新 SMTC 帧替换；
    // 旧层只需要一次静态快照，不参与新的布局和滚动状态。
    if (!snapshot.sourceAppUserModelId.empty()) {
        drawSourceIcon(context, rt, snapshot.sourceAppUserModelId);
        drawText(context, rt, sourceLabel(snapshot.sourceAppUserModelId), fmtSource,
                 context.geometry.mediaLayout_.source, brushText);
    }
    drawCover(context, rt, useCachedLayers);
    IDWriteTextLayout* snapshotTitleLayout = nullptr;
    IDWriteTextLayout* snapshotArtistLayout = nullptr;
    float snapshotTitleWidth = 0.0f;
    float snapshotTitleHeight = 0.0f;
    float snapshotArtistWidth = 0.0f;
    float snapshotArtistHeight = 0.0f;
    createMeasuredTextLayout(context, snapshot.title, fmtTitle, 40.0f, &snapshotTitleLayout,
                             snapshotTitleWidth, snapshotTitleHeight);
    createMeasuredTextLayout(context, snapshot.artist, fmtArtist, 40.0f, &snapshotArtistLayout,
                             snapshotArtistWidth, snapshotArtistHeight);
    drawMediaTitles(context, rt, w, snapshotTitleLayout, snapshotTitleWidth, snapshotTitleHeight,
                    categorySnapshot.titleScrollOffset, snapshotArtistLayout, snapshotArtistWidth,
                    snapshotArtistHeight, categorySnapshot.artistScrollOffset);
    drawMediaPet(context, rt, w);
    drawProgress(context, rt, w, snapshot);
    for (int i = 0; i < 3; ++i)
        drawButton(context, rt, i, snapshot);
    releaseCom(snapshotTitleLayout);
    releaseCom(snapshotArtistLayout);
}

void drawSourceIcon(const PagePaintContext& context, ID2D1DeviceContext* rt,
                    const std::wstring& source) {
    auto& brushAccent = context.resources.brushAccent;
    auto& brushTextOnAccent = context.resources.brushTextOnAccent;
    auto& fmtIcon = context.resources.fmtIcon;
    auto& sourceIconBmp = context.resources.sourceIconBmp;
    auto& sourceIconLoadedFor = context.resources.sourceIconLoadedFor;

    const D2D1_RECT_F iconRect = D2D1::RectF(16.0f, 14.0f, 34.0f, 32.0f);
    if (sourceIconBmp && sourceIconLoadedFor == source) {
        rt->DrawBitmap(sourceIconBmp, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        rt->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 5.0f, 5.0f), brushAccent);
        drawText(context, rt, L"♪", fmtIcon, iconRect, brushTextOnAccent);
    }
}

void drawSource(const PagePaintContext& context, ID2D1DeviceContext* rt) {
    auto& media = context.media.media;
    auto& brushText = context.resources.brushText;
    auto& fmtSource = context.resources.fmtSource;

    drawSourceIcon(context, rt, media.sourceAppUserModelId);
    drawText(context, rt, sourceLabel(media.sourceAppUserModelId), fmtSource,
             context.geometry.mediaLayout_.source, brushText);
}

void drawProgress(const PagePaintContext& context, ID2D1DeviceContext* rt, float w,
                  const OverlayMediaInfo& content) {
    auto& positionMs = context.media.positionMs;
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushProgressTrack = context.resources.brushProgressTrack;
    auto& brushAccent = context.resources.brushAccent;
    auto& fmtSource = context.resources.fmtSource;
    auto& fmtTimeRight = context.resources.fmtTimeRight;

    if (!rt || !brushProgressTrack || !brushAccent || !brushSecondary || !fmtSource ||
        !fmtTimeRight)
        return;

    const auto track = context.geometry.mediaLayout_.track;
    const float left = track.left, right = track.right;
    const float trackTop = track.top, trackBottom = track.bottom;
    rt->FillRoundedRectangle(D2D1::RoundedRect(track, 2.0f, 2.0f), brushProgressTrack);

    const int64_t displayPositionMs = content.durationMs > 0
                                          ? std::clamp(positionMs, int64_t{0}, content.durationMs)
                                          : std::max<int64_t>(positionMs, 0);
    if (content.durationMs > 0) {
        const float fraction = static_cast<float>(static_cast<double>(displayPositionMs) /
                                                  static_cast<double>(content.durationMs));
        const float fillRight = left + (right - left) * fraction;
        if (fillRight > left)
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(left, trackTop, fillRight, trackBottom), 2.0f, 2.0f),
                brushAccent);
    }

    drawText(context, rt, formatPlaybackTime(displayPositionMs), fmtSource,
             context.geometry.mediaLayout_.timeLeft, brushSecondary);
    drawText(context, rt, formatPlaybackDuration(content.durationMs), fmtTimeRight,
             context.geometry.mediaLayout_.timeRight, brushSecondary);
}

void drawButton(const PagePaintContext& context, ID2D1DeviceContext* rt, int index,
                const OverlayMediaInfo& content) {
    auto& cardOriginDip = context.geometry.cardOriginDip;
    auto& hoverButton = context.interaction.hoverButton;
    auto& pressedButton = context.interaction.pressedButton;
    auto& brushText = context.resources.brushText;
    auto& brushDisabled = context.resources.brushDisabled;
    auto& brushControl = context.resources.brushControl;
    auto& brushAccent = context.resources.brushAccent;
    auto& brushAccentHover = context.resources.brushAccentHover;
    auto& brushTextOnAccent = context.resources.brushTextOnAccent;
    auto& controlGeometry = context.resources.controlGeometry;
    auto& mediaLayout_ = context.geometry.mediaLayout_;

    const bool enabled = buttonEnabled(context.state.available, index, content);
    // mediaLayout_.buttons 保存的是窗口客户区命中坐标；绘制层已经通过
    // cardOriginDip 把卡片移到窗口中的实际位置，因此这里还原为卡片内部坐标，
    // 避免上下方弹出或横向转场时把垂直偏移叠加一次。
    const D2D1_RECT_F hitRect = mediaLayout_.buttons[index];
    const D2D1_RECT_F rect = D2D1::RectF(hitRect.left, hitRect.top - cardOriginDip, hitRect.right,
                                         hitRect.bottom - cardOriginDip);
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    if (index == 1) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, 18.0f, 18.0f),
                                 enabled ? (pressedButton == index ? brushAccent
                                            : hoverButton == index ? brushAccentHover
                                                                   : brushAccent)
                                         : brushControl);
    } else if (hoverButton == index || pressedButton == index) {
        drawAdaptiveControlSurface(context, rt, rect, 18.0f, pressedButton == index,
                                   PopupPage::Media);
    }

    ID2D1Brush* iconBrush = enabled ? brushText : brushDisabled;
    if (index == 1 && enabled)
        iconBrush = brushTextOnAccent;
    const float radius = index == 1 ? 11.0f : (rect.bottom - rect.top) * 0.26f;
    media_control::draw(rt, controlGeometry, index, content.playing, D2D1::Point2F(cx, cy), radius,
                        iconBrush);
}

void drawCover(const PagePaintContext& context, ID2D1DeviceContext* rt, bool useCachedLayer) {
    auto& brushSecondary = context.resources.brushSecondary;
    auto& brushControl = context.resources.brushControl;
    auto& fmtIcon = context.resources.fmtIcon;
    auto& coverBmp = context.resources.coverBmp;
    auto& coverClip = context.resources.coverClip;
    auto& coverLayer = context.resources.coverLayer;

    const D2D1_RECT_F rect = context.geometry.mediaLayout_.cover;
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, 10.0f, 10.0f), brushControl);
    ID2D1Layer* temporaryLayer = nullptr;
    ID2D1Layer* clipLayer = useCachedLayer ? coverLayer : nullptr;
    if (coverBmp && coverClip && !clipLayer && FAILED(rt->CreateLayer(&temporaryLayer)))
        clipLayer = nullptr;
    else
        clipLayer = clipLayer ? clipLayer : temporaryLayer;
    if (coverBmp && coverClip && clipLayer) {
        rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), coverClip,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Translation(rect.left, rect.top)),
                      clipLayer);
        rt->DrawBitmap(coverBmp, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt->PopLayer();
        if (temporaryLayer)
            temporaryLayer->Release();
        return;
    }
    if (temporaryLayer)
        temporaryLayer->Release();
    drawText(context, rt, L"♪", fmtIcon, rect, brushSecondary);
}

} // namespace media_popup_paint
