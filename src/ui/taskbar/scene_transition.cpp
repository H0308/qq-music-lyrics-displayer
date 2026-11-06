#include "ui/taskbar/scene_transition.h"
#include <algorithm>
#include <cmath>

namespace taskbar_detail {
namespace {
bool sameGeometry(const WindowPlacement& a, const WindowPlacement& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
} // namespace

bool SceneTransition::isMediaScene(DisplayScene scene) {
    return scene == DisplayScene::Searching || scene == DisplayScene::Lyrics ||
           scene == DisplayScene::Spectrum || scene == DisplayScene::Message;
}

bool SceneTransition::switchesPage(DisplayScene from, DisplayScene to) {
    return (from == DisplayScene::Idle && isMediaScene(to)) ||
           (isMediaScene(from) && to == DisplayScene::Idle);
}

void SceneTransition::beginContent(DisplayScene outgoing, int width, int height) {
    outgoingScene_ = outgoing;
    fromWidth_ = width;
    fromHeight_ = height;
    needsRelayout_ = false;
}

void SceneTransition::resetContent() {
    outgoingScene_ = DisplayScene::NoPlayback;
    fromWidth_ = 0;
    fromHeight_ = 0;
    needsRelayout_ = false;
}

bool SceneTransition::beginResize(const WindowPlacement& from, const WindowPlacement& to,
                                  ULONGLONG now) {
    if (sameGeometry(from, to)) {
        cancelResize();
        return false;
    }
    resize_ = Resize{from, to, from, now};
    return true;
}

bool SceneTransition::targets(const WindowPlacement& placement) const {
    return resize_ && sameGeometry(resize_->to, placement);
}

void SceneTransition::synchronizeResizeStart(ULONGLONG now) {
    if (resize_)
        resize_->startMs = now;
}

std::optional<WindowPlacement> SceneTransition::advanceResize(ULONGLONG now) {
    if (!resize_)
        return std::nullopt;
    auto& resize = *resize_;
    const auto elapsed = now > resize.startMs ? now - resize.startMs : 0;
    const float progress = std::clamp(static_cast<float>(elapsed) / kDurationMs, 0.0f, 1.0f);
    const float t = progress * progress * (3.0f - 2.0f * progress);
    auto interpolate = [t](int from, int to) {
        return static_cast<int>(std::lround(from + (to - from) * t));
    };
    WindowPlacement placement;
    placement.x = interpolate(resize.from.x, resize.to.x);
    placement.y = interpolate(resize.from.y, resize.to.y);
    placement.width = interpolate(resize.from.width, resize.to.width);
    placement.height = interpolate(resize.from.height, resize.to.height);
    const bool changed = !sameGeometry(placement, resize.lastApplied);
    resize.lastApplied = placement;
    if (progress >= 1.0f)
        cancelResize();
    return changed ? std::optional<WindowPlacement>(placement) : std::nullopt;
}
} // namespace taskbar_detail
