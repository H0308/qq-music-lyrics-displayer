#include "settings_icons.h"

#include <algorithm>

namespace settings_icon {
namespace {

struct Canvas {
    ID2D1RenderTarget* target = nullptr;
    ID2D1Brush* brush = nullptr;
    float cx = 0.0f;
    float cy = 0.0f;
    float scale = 1.0f;
    float stroke = 1.25f;

    D2D1_POINT_2F point(float x, float y) const {
        return D2D1::Point2F(cx + (x - 10.0f) * scale, cy + (y - 10.0f) * scale);
    }

    D2D1_RECT_F rect(float left, float top, float right, float bottom) const {
        const auto a = point(left, top);
        const auto b = point(right, bottom);
        return D2D1::RectF(a.x, a.y, b.x, b.y);
    }

    void line(float x1, float y1, float x2, float y2, float width = -1.0f) const {
        target->DrawLine(point(x1, y1), point(x2, y2), brush,
                         width > 0.0f ? width * scale : stroke);
    }

    void ellipse(float left, float top, float right, float bottom, float width = -1.0f) const {
        const auto r = rect(left, top, right, bottom);
        target->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F((r.left + r.right) * 0.5f,
                                        (r.top + r.bottom) * 0.5f),
                          (r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f),
            brush, width > 0.0f ? width * scale : stroke);
    }

    void fillEllipse(float left, float top, float right, float bottom) const {
        const auto r = rect(left, top, right, bottom);
        target->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F((r.left + r.right) * 0.5f,
                                        (r.top + r.bottom) * 0.5f),
                          (r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f),
            brush);
    }

    void rounded(float left, float top, float right, float bottom, float radius,
                 float width = -1.0f) const {
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(rect(left, top, right, bottom), radius * scale, radius * scale),
            brush, width > 0.0f ? width * scale : stroke);
    }

    void fillRounded(float left, float top, float right, float bottom, float radius) const {
        target->FillRoundedRectangle(
            D2D1::RoundedRect(rect(left, top, right, bottom), radius * scale, radius * scale),
            brush);
    }
};

} // namespace

void draw(ID2D1RenderTarget* target, Kind kind, const D2D1_RECT_F& bounds, ID2D1Brush* brush,
          float strokeWidth) {
    if (!target || !brush || kind == Kind::None || bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top)
        return;

    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    Canvas c;
    c.target = target;
    c.brush = brush;
    c.cx = (bounds.left + bounds.right) * 0.5f;
    c.cy = (bounds.top + bounds.bottom) * 0.5f;
    c.scale = std::min(width, height) / 20.0f;
    c.stroke = strokeWidth * c.scale;

    switch (kind) {
    case Kind::Display:
        c.rounded(2.5f, 3.0f, 17.5f, 14.0f, 1.6f);
        c.line(7.0f, 17.0f, 13.0f, 17.0f);
        c.line(10.0f, 14.0f, 10.0f, 17.0f);
        c.line(6.0f, 17.0f, 14.0f, 17.0f);
        break;
    case Kind::Performance:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.line(10.0f, 10.0f, 13.7f, 6.6f, 1.35f);
        c.fillEllipse(8.6f, 8.6f, 11.4f, 11.4f);
        c.line(5.0f, 15.2f, 15.0f, 15.2f);
        break;
    case Kind::Card:
        c.rounded(2.0f, 3.0f, 17.0f, 17.0f, 2.0f);
        c.rounded(5.0f, 6.0f, 14.0f, 8.5f, 0.8f);
        c.line(5.0f, 11.0f, 14.0f, 11.0f);
        c.line(5.0f, 14.0f, 11.0f, 14.0f);
        break;
    case Kind::Media:
        c.rounded(2.0f, 3.0f, 18.0f, 17.0f, 2.0f);
        c.line(4.5f, 6.5f, 15.5f, 6.5f);
        c.line(6.0f, 11.0f, 8.3f, 9.5f);
        c.line(8.3f, 9.5f, 8.3f, 12.5f);
        c.line(8.3f, 12.5f, 6.0f, 11.0f);
        c.line(11.5f, 10.0f, 15.0f, 10.0f);
        c.line(11.5f, 12.5f, 15.0f, 12.5f);
        break;
    case Kind::Spectrum:
        c.fillRounded(3.0f, 10.0f, 5.5f, 16.0f, 1.0f);
        c.fillRounded(6.2f, 7.0f, 8.7f, 16.0f, 1.0f);
        c.fillRounded(9.4f, 4.0f, 11.9f, 16.0f, 1.0f);
        c.fillRounded(12.6f, 8.0f, 15.1f, 16.0f, 1.0f);
        c.fillRounded(15.8f, 6.0f, 18.0f, 16.0f, 1.0f);
        break;
    case Kind::Lyrics:
        c.line(5.0f, 5.0f, 17.0f, 5.0f);
        c.line(5.0f, 9.0f, 15.0f, 9.0f);
        c.line(5.0f, 13.0f, 12.0f, 13.0f);
        c.line(14.5f, 10.0f, 14.5f, 15.0f);
        c.line(14.5f, 10.0f, 17.0f, 9.2f);
        c.fillEllipse(12.5f, 14.0f, 15.0f, 16.2f);
        break;
    case Kind::Toast:
        c.line(5.0f, 13.5f, 15.0f, 13.5f);
        c.line(6.0f, 13.5f, 6.8f, 8.0f);
        c.line(6.8f, 8.0f, 8.0f, 5.8f);
        c.line(8.0f, 5.8f, 12.0f, 5.8f);
        c.line(12.0f, 5.8f, 13.2f, 8.0f);
        c.line(13.2f, 8.0f, 14.0f, 13.5f);
        c.fillEllipse(8.5f, 14.0f, 11.5f, 16.5f);
        break;
    case Kind::Idle:
        c.line(10.0f, 3.0f, 10.0f, 8.0f);
        c.line(7.5f, 5.5f, 12.5f, 5.5f);
        c.line(4.0f, 11.0f, 4.0f, 15.0f);
        c.line(2.0f, 13.0f, 6.0f, 13.0f);
        c.fillEllipse(13.0f, 12.0f, 17.0f, 16.0f);
        c.line(14.2f, 14.0f, 15.8f, 14.0f);
        break;
    case Kind::Theme:
        c.fillEllipse(7.0f, 7.0f, 13.0f, 13.0f);
        c.line(10.0f, 2.5f, 10.0f, 5.0f);
        c.line(10.0f, 15.0f, 10.0f, 17.5f);
        c.line(2.5f, 10.0f, 5.0f, 10.0f);
        c.line(15.0f, 10.0f, 17.5f, 10.0f);
        c.line(4.4f, 4.4f, 6.2f, 6.2f);
        c.line(13.8f, 13.8f, 15.6f, 15.6f);
        c.line(15.6f, 4.4f, 13.8f, 6.2f);
        c.line(6.2f, 13.8f, 4.4f, 15.6f);
        break;
    case Kind::Font:
        c.line(4.0f, 16.0f, 9.5f, 4.0f);
        c.line(9.5f, 4.0f, 15.0f, 16.0f);
        c.line(6.0f, 11.5f, 13.0f, 11.5f);
        c.line(3.0f, 16.0f, 7.0f, 16.0f);
        c.line(12.0f, 16.0f, 16.0f, 16.0f);
        break;
    case Kind::SongInfo:
        c.fillEllipse(3.0f, 5.0f, 5.5f, 7.5f);
        c.line(7.5f, 6.2f, 17.0f, 6.2f);
        c.fillEllipse(3.0f, 11.0f, 5.5f, 13.5f);
        c.line(7.5f, 12.2f, 15.0f, 12.2f);
        c.line(7.5f, 16.0f, 12.0f, 16.0f);
        break;
    case Kind::AlbumCover:
        c.rounded(3.0f, 3.0f, 17.0f, 17.0f, 1.5f);
        c.fillEllipse(5.0f, 5.0f, 8.0f, 8.0f);
        c.line(4.5f, 15.0f, 8.0f, 11.5f);
        c.line(8.0f, 11.5f, 10.5f, 14.0f);
        c.line(10.5f, 14.0f, 13.0f, 10.5f);
        c.line(13.0f, 10.5f, 16.0f, 15.0f);
        break;
    case Kind::Platform:
        c.rounded(3.0f, 3.0f, 17.0f, 17.0f, 3.0f);
        c.fillEllipse(7.0f, 7.0f, 13.0f, 13.0f);
        c.line(5.0f, 14.5f, 15.0f, 14.5f);
        break;
    case Kind::Vinyl:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.ellipse(6.0f, 6.0f, 14.0f, 14.0f, 0.9f);
        c.fillEllipse(8.7f, 8.7f, 11.3f, 11.3f);
        c.line(11.0f, 5.0f, 14.8f, 8.8f, 0.9f);
        break;
    case Kind::RenderMode:
        c.line(3.0f, 5.0f, 17.0f, 5.0f);
        c.line(3.0f, 10.0f, 17.0f, 10.0f);
        c.line(3.0f, 15.0f, 17.0f, 15.0f);
        c.fillEllipse(6.0f, 3.2f, 8.8f, 6.8f);
        c.fillEllipse(12.0f, 8.2f, 14.8f, 11.8f);
        c.fillEllipse(8.0f, 13.2f, 10.8f, 16.8f);
        break;
    case Kind::Hover:
        c.line(4.0f, 3.5f, 4.0f, 15.8f);
        c.line(4.0f, 3.5f, 14.5f, 12.0f);
        c.line(4.0f, 15.8f, 8.5f, 12.5f);
        c.line(8.5f, 12.5f, 11.0f, 17.0f);
        c.line(11.0f, 17.0f, 13.0f, 15.8f);
        c.line(13.0f, 15.8f, 10.7f, 11.8f);
        c.line(10.7f, 11.8f, 14.5f, 12.0f);
        break;
    case Kind::Control:
        c.rounded(2.5f, 4.0f, 17.5f, 16.0f, 2.0f);
        c.fillRounded(5.0f, 7.0f, 7.0f, 9.0f, 0.8f);
        c.fillRounded(9.0f, 7.0f, 11.0f, 9.0f, 0.8f);
        c.fillRounded(13.0f, 7.0f, 15.0f, 9.0f, 0.8f);
        c.line(5.0f, 12.5f, 15.0f, 12.5f);
        break;
    case Kind::Trigger:
        c.fillEllipse(8.0f, 8.0f, 12.0f, 12.0f);
        c.ellipse(5.0f, 5.0f, 15.0f, 15.0f, 0.9f);
        c.line(10.0f, 2.5f, 10.0f, 5.0f);
        c.line(15.0f, 10.0f, 17.5f, 10.0f);
        break;
    case Kind::Background:
        c.rounded(4.0f, 3.0f, 16.0f, 15.0f, 1.5f);
        c.line(5.0f, 12.5f, 8.0f, 9.5f);
        c.line(8.0f, 9.5f, 11.0f, 12.0f);
        c.line(11.0f, 12.0f, 14.5f, 8.0f);
        c.line(5.0f, 17.0f, 15.0f, 17.0f);
        break;
    case Kind::Color:
        c.fillEllipse(3.5f, 4.0f, 8.0f, 8.5f);
        c.fillEllipse(8.0f, 3.0f, 12.5f, 7.5f);
        c.fillEllipse(12.0f, 5.0f, 16.5f, 9.5f);
        c.rounded(4.0f, 7.0f, 16.0f, 16.5f, 4.0f);
        c.fillEllipse(7.0f, 10.0f, 9.0f, 12.0f);
        c.fillEllipse(11.0f, 13.0f, 13.0f, 15.0f);
        break;
    case Kind::Follow:
        c.ellipse(3.5f, 5.0f, 11.0f, 12.5f);
        c.ellipse(9.0f, 7.5f, 16.5f, 15.0f);
        c.line(8.0f, 8.0f, 12.0f, 12.0f);
        break;
    case Kind::Contrast:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.line(10.0f, 3.0f, 10.0f, 17.0f);
        c.line(6.0f, 6.0f, 6.0f, 14.0f, 0.9f);
        break;
    case Kind::Progress:
        c.line(3.0f, 10.0f, 17.0f, 10.0f);
        c.fillEllipse(8.0f, 7.0f, 13.0f, 13.0f);
        c.line(4.0f, 8.0f, 4.0f, 12.0f, 0.9f);
        c.line(16.0f, 8.0f, 16.0f, 12.0f, 0.9f);
        break;
    case Kind::Opacity:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.line(5.0f, 15.0f, 15.0f, 5.0f);
        c.fillEllipse(6.0f, 6.0f, 8.4f, 8.4f);
        c.fillEllipse(11.6f, 11.6f, 14.0f, 14.0f);
        break;
    case Kind::DynamicBackground:
        c.line(2.5f, 13.5f, 5.5f, 10.0f);
        c.line(5.5f, 10.0f, 8.5f, 13.0f);
        c.line(8.5f, 13.0f, 12.0f, 7.0f);
        c.line(12.0f, 7.0f, 17.5f, 11.0f);
        c.line(3.0f, 16.5f, 17.0f, 16.5f, 0.9f);
        c.fillEllipse(13.8f, 3.2f, 16.0f, 5.4f);
        break;
    case Kind::Scope:
        c.ellipse(4.0f, 4.0f, 16.0f, 16.0f);
        c.ellipse(7.0f, 7.0f, 13.0f, 13.0f, 0.9f);
        c.fillEllipse(9.0f, 9.0f, 11.0f, 11.0f);
        c.line(10.0f, 2.5f, 10.0f, 4.0f);
        c.line(10.0f, 16.0f, 10.0f, 17.5f);
        break;
    case Kind::Quote:
        c.fillEllipse(3.5f, 5.0f, 7.0f, 8.5f);
        c.line(5.2f, 8.0f, 4.0f, 11.5f);
        c.fillEllipse(11.5f, 5.0f, 15.0f, 8.5f);
        c.line(13.2f, 8.0f, 12.0f, 11.5f);
        c.line(4.0f, 15.0f, 16.0f, 15.0f);
        break;
    case Kind::Language:
        c.line(3.5f, 16.0f, 8.5f, 4.0f);
        c.line(8.5f, 4.0f, 13.5f, 16.0f);
        c.line(5.5f, 11.5f, 11.5f, 11.5f);
        c.line(14.0f, 6.0f, 17.0f, 6.0f);
        c.line(15.5f, 4.5f, 15.5f, 8.0f);
        c.line(14.0f, 8.0f, 17.0f, 11.0f);
        break;
    case Kind::Refresh:
        c.ellipse(4.0f, 4.0f, 16.0f, 16.0f);
        c.line(14.0f, 4.0f, 17.0f, 4.0f);
        c.line(17.0f, 4.0f, 17.0f, 7.0f);
        c.line(6.0f, 16.0f, 3.0f, 16.0f);
        c.line(3.0f, 16.0f, 3.0f, 13.0f);
        break;
    case Kind::Apps:
        c.fillRounded(3.0f, 3.0f, 8.0f, 8.0f, 1.0f);
        c.fillRounded(12.0f, 3.0f, 17.0f, 8.0f, 1.0f);
        c.fillRounded(3.0f, 12.0f, 8.0f, 17.0f, 1.0f);
        c.fillRounded(12.0f, 12.0f, 17.0f, 17.0f, 1.0f);
        break;
    case Kind::Tray:
        c.rounded(2.5f, 4.0f, 17.5f, 16.5f, 1.8f);
        c.line(2.5f, 8.0f, 17.5f, 8.0f);
        c.fillEllipse(6.0f, 10.5f, 7.8f, 12.3f);
        c.fillEllipse(9.1f, 10.5f, 10.9f, 12.3f);
        c.fillEllipse(12.2f, 10.5f, 14.0f, 12.3f);
        break;
    case Kind::Api:
        c.ellipse(3.0f, 6.0f, 9.0f, 12.0f);
        c.line(8.0f, 10.0f, 16.5f, 10.0f);
        c.line(13.0f, 10.0f, 13.0f, 13.0f);
        c.line(15.0f, 10.0f, 15.0f, 12.0f);
        break;
    case Kind::Connect:
        c.ellipse(3.0f, 7.0f, 9.0f, 13.0f);
        c.ellipse(11.0f, 7.0f, 17.0f, 13.0f);
        c.line(8.0f, 10.0f, 12.0f, 10.0f);
        c.line(7.0f, 6.0f, 13.0f, 14.0f, 0.9f);
        break;
    case Kind::Sync:
        c.line(4.0f, 7.0f, 15.5f, 7.0f);
        c.line(15.5f, 7.0f, 13.0f, 4.5f);
        c.line(15.5f, 7.0f, 13.0f, 9.5f);
        c.line(16.0f, 13.0f, 4.5f, 13.0f);
        c.line(4.5f, 13.0f, 7.0f, 10.5f);
        c.line(4.5f, 13.0f, 7.0f, 15.5f);
        break;
    case Kind::Disconnect:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.line(5.0f, 15.0f, 15.0f, 5.0f);
        break;
    case Kind::Duration:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.line(10.0f, 6.0f, 10.0f, 10.0f);
        c.line(10.0f, 10.0f, 13.0f, 12.0f);
        c.line(7.0f, 2.5f, 13.0f, 2.5f, 0.9f);
        break;
    case Kind::Fullscreen:
        c.line(3.0f, 7.0f, 3.0f, 3.0f);
        c.line(3.0f, 3.0f, 7.0f, 3.0f);
        c.line(13.0f, 3.0f, 17.0f, 3.0f);
        c.line(17.0f, 3.0f, 17.0f, 7.0f);
        c.line(3.0f, 13.0f, 3.0f, 17.0f);
        c.line(3.0f, 17.0f, 7.0f, 17.0f);
        c.line(13.0f, 17.0f, 17.0f, 17.0f);
        c.line(17.0f, 17.0f, 17.0f, 13.0f);
        break;
    case Kind::Position:
        c.line(3.0f, 5.0f, 17.0f, 5.0f);
        c.line(3.0f, 15.0f, 17.0f, 15.0f);
        c.line(10.0f, 7.0f, 10.0f, 13.0f);
        c.line(10.0f, 7.0f, 7.5f, 9.5f);
        c.line(10.0f, 7.0f, 12.5f, 9.5f);
        c.line(10.0f, 13.0f, 7.5f, 10.5f);
        c.line(10.0f, 13.0f, 12.5f, 10.5f);
        break;
    case Kind::DoubleLine:
        c.line(3.0f, 6.0f, 10.0f, 6.0f);
        c.line(3.0f, 9.0f, 17.0f, 9.0f);
        c.line(3.0f, 13.0f, 9.0f, 13.0f);
        c.line(3.0f, 16.0f, 14.0f, 16.0f);
        break;
    case Kind::Alignment:
        c.line(3.0f, 5.0f, 16.0f, 5.0f);
        c.line(3.0f, 9.0f, 13.0f, 9.0f);
        c.line(3.0f, 13.0f, 16.0f, 13.0f);
        c.line(3.0f, 17.0f, 11.0f, 17.0f);
        break;
    case Kind::LocalLyrics:
        c.rounded(4.0f, 3.0f, 16.0f, 17.0f, 1.2f);
        c.line(7.0f, 7.0f, 13.0f, 7.0f);
        c.line(7.0f, 10.0f, 13.0f, 10.0f);
        c.line(12.0f, 11.0f, 12.0f, 15.0f);
        c.line(12.0f, 11.0f, 14.5f, 10.3f);
        c.fillEllipse(10.0f, 14.0f, 12.7f, 16.2f);
        break;
    case Kind::Search:
        c.ellipse(3.5f, 3.5f, 12.5f, 12.5f);
        c.line(10.5f, 10.5f, 16.5f, 16.5f, 1.5f);
        break;
    case Kind::Log:
        c.rounded(4.0f, 2.5f, 16.0f, 17.5f, 1.5f);
        c.line(7.0f, 7.0f, 13.0f, 7.0f);
        c.line(7.0f, 10.0f, 13.0f, 10.0f);
        c.line(7.0f, 13.0f, 11.5f, 13.0f);
        break;
    case Kind::Settings:
        c.ellipse(5.0f, 5.0f, 15.0f, 15.0f);
        c.ellipse(8.0f, 8.0f, 12.0f, 12.0f, 1.0f);
        c.line(10.0f, 2.5f, 10.0f, 5.0f);
        c.line(10.0f, 15.0f, 10.0f, 17.5f);
        c.line(2.5f, 10.0f, 5.0f, 10.0f);
        c.line(15.0f, 10.0f, 17.5f, 10.0f);
        c.line(4.7f, 4.7f, 6.5f, 6.5f);
        c.line(13.5f, 13.5f, 15.3f, 15.3f);
        c.line(15.3f, 4.7f, 13.5f, 6.5f);
        c.line(6.5f, 13.5f, 4.7f, 15.3f);
        break;
    case Kind::Info:
        c.ellipse(3.0f, 3.0f, 17.0f, 17.0f);
        c.fillEllipse(9.0f, 5.5f, 11.0f, 7.5f);
        c.line(10.0f, 9.0f, 10.0f, 14.0f, 1.5f);
        break;
    case Kind::Exit:
        c.rounded(3.0f, 3.0f, 11.0f, 17.0f, 1.2f);
        c.line(9.0f, 10.0f, 17.0f, 10.0f);
        c.line(17.0f, 10.0f, 13.5f, 6.5f);
        c.line(17.0f, 10.0f, 13.5f, 13.5f);
        break;
    case Kind::Folder:
        c.line(2.5f, 6.0f, 7.0f, 6.0f);
        c.line(7.0f, 6.0f, 8.5f, 4.0f);
        c.line(8.5f, 4.0f, 14.0f, 4.0f);
        c.line(14.0f, 4.0f, 15.0f, 6.0f);
        c.rounded(2.5f, 6.0f, 17.5f, 16.5f, 1.5f);
        c.line(3.5f, 8.5f, 16.5f, 8.5f, 0.9f);
        break;
    case Kind::Persist:
        c.rounded(4.0f, 3.0f, 16.0f, 17.0f, 1.5f);
        c.rounded(7.0f, 4.5f, 13.0f, 8.0f, 0.7f);
        c.fillEllipse(7.0f, 11.0f, 13.0f, 17.0f);
        c.fillEllipse(9.0f, 13.0f, 11.0f, 15.0f);
        break;
    case Kind::Gradient:
        c.fillRounded(3.0f, 5.0f, 6.0f, 16.0f, 1.0f);
        c.fillRounded(8.0f, 3.0f, 11.0f, 16.0f, 1.0f);
        c.fillRounded(13.0f, 7.0f, 16.0f, 16.0f, 1.0f);
        c.line(3.0f, 17.5f, 16.0f, 17.5f, 0.9f);
        break;
    case Kind::Wave:
        c.line(2.5f, 11.0f, 5.0f, 7.0f);
        c.line(5.0f, 7.0f, 7.5f, 13.0f);
        c.line(7.5f, 13.0f, 10.0f, 5.0f);
        c.line(10.0f, 5.0f, 12.5f, 14.0f);
        c.line(12.5f, 14.0f, 15.0f, 8.0f);
        c.line(15.0f, 8.0f, 17.5f, 11.0f);
        break;
    case Kind::None:
        break;
    }
}

} // namespace settings_icon
