#pragma once
#include "ui/lyric_window.h"
#include "media/smtc_monitor.h"
#include "model/presentation_update.h"
struct AppSettings;
struct PlaybackState;

struct IdleFrameInput {
    IdlePresentation presentation;
    bool summaryActive = false;
    bool summaryPending = false;
    bool tasksEnabled = false;
    std::wstring summaryText;
};

// 完整帧与高频播放增量共用一个版本源，宿主不再反向修改业务状态。
class PresentationModel {
public:
    const PresentationFrame& frame() const { return frame_; }
    const PresentationFrame& update(const SmtcSnapshot& snap, const AppSettings& settings,
                                    const PlaybackState& playback, const IdleFrameInput& idle,
                                    PresentationUpdate update);
    const PresentationFrame& updateAlbumColor(COLORREF color);
    PlaybackPatch updatePlayback(const SmtcSnapshot& snap);

private:
    uint64_t revision_ = 0;
    PresentationFrame frame_;
};
