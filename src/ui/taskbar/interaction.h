#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

class TaskbarPresence {
public:
    enum class Phase { Shown, Hiding, Hidden, Showing };
    enum class Action { None, Hide, Show, StopTimer };
    static constexpr UINT kHideMs = 100, kShowMs = 140;
    Phase phase() const { return phase_; }
    ULONGLONG startMs() const { return startMs_; }
    float opacity(ULONGLONG now) const;
    void beginHide(ULONGLONG now, float from) { begin(Phase::Hiding, now, from); }
    void beginShow(ULONGLONG now, float from) { begin(Phase::Showing, now, from); }
    void prepareShow() { begin(Phase::Showing, 0, 0.0f); }
    void startShow(ULONGLONG now) { startMs_ = now; }
    void hide() {
        phase_ = Phase::Hidden;
        awaySince_ = 0;
    }
    void reset() {
        phase_ = Phase::Shown;
        awaySince_ = 0;
    }
    Action advance(ULONGLONG now, bool cursorAway);

private:
    void begin(Phase phase, ULONGLONG now, float from) {
        phase_ = phase;
        startMs_ = now;
        from_ = from;
        awaySince_ = 0;
    }
    Phase phase_ = Phase::Shown;
    ULONGLONG startMs_ = 0, awaySince_ = 0;
    float from_ = 1.0f;
};

// 按下与拖动共用一次手势，只有越过阈值后才进入拖动。
class TaskbarDragGesture {
public:
    bool pressed() const { return pressed_; }
    bool dragging() const { return dragging_; }
    POINT cursor() const { return cursor_; }
    int candidate() const { return candidate_; }
    int previewMajor() const { return previewMajor_; }
    void press(POINT point, int mode) {
        pressed_ = true;
        start_ = cursor_ = point;
        candidate_ = mode;
    }
    bool move(POINT point, int thresholdX, int thresholdY) {
        cursor_ = point;
        return !dragging_ && (std::abs(point.x - start_.x) >= thresholdX ||
                              std::abs(point.y - start_.y) >= thresholdY);
    }
    void begin(int previewMajor, int mode) {
        dragging_ = true;
        previewMajor_ = previewMajor;
        candidate_ = mode;
    }
    void end() { pressed_ = dragging_ = false; }
    void clearPreview() { previewMajor_ = 0; }
    void setCandidate(int mode) { candidate_ = mode; }

private:
    bool pressed_ = false, dragging_ = false;
    POINT start_{}, cursor_{};
    int candidate_ = 0, previewMajor_ = 0;
};
