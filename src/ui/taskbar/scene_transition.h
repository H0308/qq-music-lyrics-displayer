#pragma once
#include "ui/taskbar/placement.h"
#include <optional>

namespace taskbar_detail {
// 只拥有场景快照和尺寸时间轴；窗口移动与绘制提交仍由宿主执行。
class SceneTransition {
public:
    static constexpr float kDurationMs = 240.0f;
    static bool isMediaScene(DisplayScene scene);
    static bool switchesPage(DisplayScene from, DisplayScene to);

    void beginContent(DisplayScene outgoing, int width, int height);
    void resetContent();
    DisplayScene outgoingScene() const { return outgoingScene_; }
    int fromWidth() const { return fromWidth_; }
    int fromHeight() const { return fromHeight_; }
    bool needsRelayout() const { return needsRelayout_; }
    void setNeedsRelayout(bool value) { needsRelayout_ = value; }

    bool beginResize(const WindowPlacement& from, const WindowPlacement& to, ULONGLONG now);
    void cancelResize() { resize_.reset(); }
    bool resizing() const { return resize_.has_value(); }
    bool targets(const WindowPlacement& placement) const;
    void synchronizeResizeStart(ULONGLONG now);
    std::optional<WindowPlacement> advanceResize(ULONGLONG now);

private:
    struct Resize {
        WindowPlacement from, to, lastApplied;
        ULONGLONG startMs;
    };
    std::optional<Resize> resize_;
    DisplayScene outgoingScene_ = DisplayScene::NoPlayback;
    int fromWidth_ = 0;
    int fromHeight_ = 0;
    bool needsRelayout_ = false;
};
} // namespace taskbar_detail
