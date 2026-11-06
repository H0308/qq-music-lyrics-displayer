#include "app/presentation_model.h"
#include "settings/app_settings.h"
#include "media/playback_controller.h"

namespace {
constexpr int64_t kLyricTransitionLeadMs = 100;
const wchar_t* notRunningStatus(const PlaybackState& playback) {
    return playback.lastPlayer_ == SmtcPlayerType::NetEase ? L"网易云音乐未运行" : L"QQ 音乐未运行";
}

OverlayMediaInfo makeMediaInfo(const SmtcSnapshot& snap, const PlaybackState& playback,
                               const PresentationFrame& previous) {
    OverlayMediaInfo mi;
    const bool retainPrevious = snap.sessionAlive && snap.title.empty() && snap.artist.empty() &&
                                previous.frameRevision != 0 &&
                                previous.trackKey == playback.currentKey;
    if (retainPrevious) {
        // SMTC 的过渡事件可能暂时没有完整文案，保留当前曲目的媒体字段，
        // 避免用一份不完整快照拼出“旧歌词 + 空标题”的中间画面。
        mi = previous.media;
    } else {
        mi.title = snap.title.empty() ? playback.currentTitle : snap.title;
        mi.artist = snap.artist.empty() ? playback.currentArtist : snap.artist;
        if (!snap.album.empty()) {
            mi.artist += L" · ";
            mi.artist += snap.album;
        }
        mi.sourceAppUserModelId = snap.sourceAppUserModelId;
    }
    if (snap.thumbnail && !snap.thumbnail->empty())
        mi.thumbnail = snap.thumbnail;
    else if (playback.lastSmtcThumbnail && !playback.lastSmtcThumbnail->empty())
        mi.thumbnail = playback.lastSmtcThumbnail;
    else
        mi.thumbnail = playback.lastCover_;
    mi.hasDominantColor = playback.hasAlbumColor_;
    mi.dominantColor = playback.albumColor_;
    // QQ 切歌时旧时间线会暂时残留，不能把上一首的总时长带进媒体卡片。
    mi.durationMs = !snap.timelineStale && snap.durationMs > 0 ? snap.durationMs : 0;
    mi.canPrev = snap.canPrev;
    mi.canPlayPause = snap.canPlayPause;
    mi.canNext = snap.canNext;
    mi.playing = snap.status == PlaybackStatus::Playing;
    return mi;
}

DisplayScene displaySceneFor(const SmtcSnapshot& snap, const AppSettings& settings,
                             const PlaybackState& playback) {
    if (!snap.sessionAlive)
        return settings.idleEntryEnabled_ ? DisplayScene::Idle : DisplayScene::NoPlayback;
    if (playback.lyricLoading_)
        return DisplayScene::Searching;
    if (!playback.currentLyrics_.empty())
        return DisplayScene::Lyrics;
    return settings.spectrumOn_ ? DisplayScene::Spectrum : DisplayScene::Message;
}

void fillPresentationFrame(PresentationFrame& frame, const SmtcSnapshot& snap,
                           bool animateTransition, bool copyLyrics, bool durationOnlyUpdate,
                           const AppSettings& settings, const PlaybackState& playback,
                           const IdleFrameInput& idle) {
    frame.requestGeneration = playback.requestGeneration_;
    frame.trackKey = playback.currentKey;
    frame.scene = displaySceneFor(snap, settings, playback);
    frame.media = makeMediaInfo(snap, playback, frame);
    frame.idle = idle.presentation;
    if (copyLyrics)
        frame.lyrics = playback.currentLyrics_;
    frame.actualPositionMs = snap.positionMs;
    frame.lineSelectionPositionMs = snap.positionMs + kLyricTransitionLeadMs;
    frame.currentLine = LyricProvider::findLine(frame.lyrics, frame.lineSelectionPositionMs);
    frame.visible = snap.sessionAlive || (settings.idleEntryEnabled_ && !snap.sessionAlive);
    frame.animateTransition = animateTransition;
    frame.durationOnlyUpdate = durationOnlyUpdate;
    const bool showStartupTaskSummary =
        idle.summaryActive && !snap.sessionAlive && frame.scene == DisplayScene::Idle;
    const bool showStartupTaskLoading = idle.summaryPending && idle.tasksEnabled &&
                                        !snap.sessionAlive && frame.scene == DisplayScene::Idle;
    frame.statusTextOneShot = showStartupTaskSummary;
    if (showStartupTaskSummary) {
        frame.statusText = idle.summaryText;
        frame.statusTextOneShot = true;
    } else if (showStartupTaskLoading) {
        frame.statusText = L"正在同步今日任务…";
    } else if (frame.scene == DisplayScene::Idle)
        frame.statusText = idle.presentation.showText ? frame.idle.sentence : L"";
    else if (!snap.sessionAlive)
        frame.statusText = notRunningStatus(playback);
    else if (playback.lyricLoading_)
        frame.statusText = L"歌词加载中…";
    else if (playback.currentLyrics_.empty())
        frame.statusText = playback.currentKey.empty() ? L"等待播放…" : L"暂无歌词";
    else
        frame.statusText.clear();
}
} // namespace

const PresentationFrame& PresentationModel::update(const SmtcSnapshot& snap,
                                                   const AppSettings& settings,
                                                   const PlaybackState& playback,
                                                   const IdleFrameInput& idle,
                                                   PresentationUpdate update) {
    fillPresentationFrame(frame_, snap, update.animateTransition,
                          revision_ == 0 || update.lyricsChanged, update.durationOnlyUpdate,
                          settings, playback, idle);
    frame_.frameRevision = ++revision_;
    return frame_;
}

PlaybackPatch PresentationModel::updatePlayback(const SmtcSnapshot& snap) {
    frame_.actualPositionMs = snap.positionMs;
    frame_.lineSelectionPositionMs = snap.positionMs + kLyricTransitionLeadMs;
    frame_.currentLine = LyricProvider::findLine(frame_.lyrics, frame_.lineSelectionPositionMs);
    frame_.media.playing = snap.status == PlaybackStatus::Playing;
    PlaybackPatch playback;
    playback.frameRevision = frame_.frameRevision;
    playback.requestGeneration = frame_.requestGeneration;
    playback.actualPositionMs = frame_.actualPositionMs;
    playback.lineSelectionPositionMs = frame_.lineSelectionPositionMs;
    playback.currentLine = frame_.currentLine;
    playback.playing = frame_.media.playing;
    return playback;
}

const PresentationFrame& PresentationModel::updateAlbumColor(COLORREF color) {
    frame_.media.hasDominantColor = true;
    frame_.media.dominantColor = color;
    frame_.frameRevision = ++revision_;
    frame_.animateTransition = false;
    return frame_;
}
