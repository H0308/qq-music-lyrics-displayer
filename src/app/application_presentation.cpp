#include "app/application_internal.h"
namespace lyrics_app {
void App::updateRuntimeLogState(const SmtcSnapshot& snap) {
    if (!snap.sessionAlive) {
        runtimeLogger_.setPlayback({}, {}, 0, false);
        runtimeLogger_.setLyricSource(L"未加载");
        runtimeLogger_.setCoverImage(nullptr);
        return;
    }

    const std::wstring title = snap.title.empty() ? playback_.state().currentTitle : snap.title;
    const std::wstring artist = snap.artist.empty() ? playback_.state().currentArtist : snap.artist;
    const int64_t durationMs =
        !snap.timelineStale
            ? (snap.durationMs > 0 ? snap.durationMs : playback_.state().currentDurationMs)
            : 0;
    runtimeLogger_.setPlayback(title, artist, durationMs, true);
    if (playback_.state().lyricLoading_)
        runtimeLogger_.setLyricSource(L"加载中…");
    else if (playback_.state().currentLyricsFromManual_)
        runtimeLogger_.setLyricSource(L"本地歌词（手动搜索）");
    else if (playback_.state().currentLyricsFromLocal_)
        runtimeLogger_.setLyricSource(L"本地歌词（QQ音乐本地）");
    else if (!playback_.state().currentLyrics_.empty())
        runtimeLogger_.setLyricSource(L"在线歌词");
    else
        runtimeLogger_.setLyricSource(L"暂无歌词");
    std::shared_ptr<const std::vector<uint8_t>> cover;
    CoverSource coverSource = CoverSource::None;
    if (playback_.state().lastSmtcThumbnail && !playback_.state().lastSmtcThumbnail->empty()) {
        cover = playback_.state().lastSmtcThumbnail;
        coverSource = CoverSource::Smtc;
    } else if (playback_.state().lastCover_ && !playback_.state().lastCover_->empty()) {
        cover = playback_.state().lastCover_;
        coverSource = playback_.state().lastCoverSource_;
    }
    runtimeLogger_.setCoverImage(cover, coverSourceDisplayName(coverSource));
}

void App::refreshIdleWelcome() {
    const SmtcSnapshot snap = monitor.snapshot();
    if (!settings_.idleEntryEnabled_ || settings_.idleQuoteEnabled_ || !idle_.idleTextVisible() ||
        snap.sessionAlive || !settings_.idleCustomWelcome_.empty())
        return;
    if (presentation_.frame().idle.sentence == idle_.defaultIdleWelcome())
        return;
    publishPresentationFrame(snap, false, true);
}

const PresentationFrame& App::preparePresentationFrame(const SmtcSnapshot& snap,
                                                       bool animateTransition, bool lyricsChanged,
                                                       bool durationOnlyUpdate) {
    IdleFrameInput input;
    input.presentation = idle_.currentIdlePresentation();
    input.summaryActive = idle_.state().startupTaskSummaryActive_;
    input.summaryPending = idle_.state().startupTaskSummaryPending_;
    input.tasksEnabled = idle_.tickTickEffectiveEnabled();
    if (input.summaryActive)
        input.summaryText = idle_.startupTaskSummaryText();
    return presentation_.update(snap, settings_, playback_.state(), input, animateTransition,
                                lyricsChanged, durationOnlyUpdate);
}

void App::publishPresentationFrame(const SmtcSnapshot& snap, bool animateTransition,
                                   bool lyricsChanged, bool durationOnlyUpdate) {
    // SMTC 状态/控制/封面事件只刷新帧字段；完整歌词仅在内容事务变化时复制。
    preparePresentationFrame(snap, animateTransition, lyricsChanged, durationOnlyUpdate);
    forEachHost([&](ILyricHost* host) { host->applyPresentationFrame(presentation_.frame()); });
    // 歌词内容事务也会改变辅助歌词的可用状态，同步刷新已打开的设置窗口。
    if (lyricsChanged)
        refreshSettingsDialog(true);
    // 弹窗可见期间异步补齐封面等字段；隐藏时只缓存不渲染
    if (songToast_)
        songToast_->setMedia(presentation_.frame().media);
    // 切歌时封面未到会短暂等待；封面在这帧补齐则立即弹出
    if (songToastCoverWaitArmed_ && presentation_.frame().media.thumbnail &&
        !presentation_.frame().media.thumbnail->empty()) {
        cancelSongToastCoverWait();
        notifySongToast();
    }
}

void App::syncHost(ILyricHost* host) {
    if (!host)
        return;
    if (presentation_.frame().frameRevision == 0) {
        preparePresentationFrame(monitor.snapshot(), false, true);
    }
    host->applyPresentationFrame(presentation_.frame());
}

void App::onControl(MediaControl c) {
    const wchar_t* name = L"unknown";
    switch (c) {
    case MediaControl::Prev:
        name = L"previous";
        monitor.skipPrevious();
        break;
    case MediaControl::PlayPause:
        name = L"play-pause";
        monitor.playPause();
        break;
    case MediaControl::Next:
        name = L"next";
        monitor.skipNext();
        break;
    }
    runtime_log::writef(L"[action][media-control] command=%s", name);
}

void App::pushAppVolume() {
    AppVolumeState state;
    state.available = appVolume_.query(state.percent, state.muted);
    if (state.available == appVolumeState_.available &&
        (!state.available ||
         (state.percent == appVolumeState_.percent && state.muted == appVolumeState_.muted)))
        return;
    appVolumeState_ = state;
    forEachHost([&](ILyricHost* host) { host->setAppVolume(state); });
}

void App::syncAppVolumeTarget(const SmtcSnapshot& snap) {
    appVolume_.setTargetProcessNames(
        volumeProcessNames(snap.sessionAlive ? snap.player : SmtcPlayerType::Unknown));
    pushAppVolume();
}

void App::onSmtcChanged() {
    SmtcSnapshot snap = monitor.snapshot();
    syncTaskbarView(snap);
    syncAppVolumeTarget(snap);
    const bool mediaSessionChanged = mediaSessionAlive_ != snap.sessionAlive;
    mediaSessionAlive_ = snap.sessionAlive;
    if (snap.sessionAlive) {
        // 媒体会话优先于启动任务播报；即使任务请求先返回，也不能覆盖已经在播放的歌词。
        idle_.cancelStartupSummary();
    }
    if (!snap.sessionAlive) {
        if (spectrumSessionAlive_)
            spectrum_.requestReconnect();
        spectrumSessionAlive_ = false;
        spectrumSessionKey_.clear();
        playback_.closeSession();
        idle_.refreshIdleQuote(false);
        publishPresentationFrame(snap, false, true);
        cancelSongToastCoverWait();
        if (songToast_)
            songToast_->hideImmediate();
        updateRuntimeLogState(snap);
        if (mediaSessionChanged)
            refreshSettingsDialog(true);
        return;
    }
    const wchar_t* spectrumProcess = spectrumProcessName(snap.player);
    if (*spectrumProcess) {
        spectrum_.setTargetProcessName(std::wstring(spectrumProcess));
        if (settings_.spectrumOn_ && isNormalRenderMode())
            spectrum_.start();
        // 把播放器源纳入频谱会话键：两个播放器播放同一首歌时也必须切换捕获目标。
        std::wstring spectrumKey = makeTrackKey(snap);
        if (!spectrumSessionAlive_ || spectrumSessionKey_ != spectrumKey)
            spectrum_.requestReconnect();
        spectrumSessionAlive_ = true;
        spectrumSessionKey_ = spectrumKey;
    } else {
        // 当前会话不是已适配的播放器时，不再保留旧播放器的捕获线程。
        spectrum_.stop();
        spectrumSessionAlive_ = false;
        spectrumSessionKey_.clear();
    }

    const std::wstring key = makeTrackKey(snap);
    if (manualSearchDialog && manualSearchDialog->isOpen() && !snap.title.empty()) {
        manualSearchDialog->setTargetSong(
            snap.title, snap.artist, snap.timelineStale ? 0 : snap.durationMs,
            snap.player == SmtcPlayerType::NetEase,
            snap.player == SmtcPlayerType::NetEase ? snap.neteaseSongId : L"");
        manualSearchTrackKey = key;
    }
    const TrackChange change =
        playback_.updateTrack(snap, presentation_.frame().media.sourceAppUserModelId);
    const bool trackChanged = change.trackChanged;
    const bool durationOnlyTrackUpdate = change.durationOnly;
    const bool songIdentityChanged = change.songIdentityChanged;
    publishPresentationFrame(snap, !trackChanged, trackChanged, durationOnlyTrackUpdate);
    if (trackChanged && songIdentityChanged && settings_.songToastEnabled_ &&
        !isMinimalRenderMode()) {
        cancelSongToastCoverWait();
        if (presentation_.frame().media.thumbnail &&
            !presentation_.frame().media.thumbnail->empty())
            notifySongToast();
        else if (trayHwnd)
            songToastCoverWaitArmed_ =
                SetTimer(trayHwnd, kTimerSongToastCover, kSongToastCoverWaitMs, nullptr) != 0;
    }
    tryExtractAlbumColor();
    updateRuntimeLogState(snap);
    if (mediaSessionChanged || trackChanged)
        refreshSettingsDialog(true);
}

void App::onFrame() {
    syncTaskbarOrientation();
    SmtcSnapshot snap = monitor.snapshot();
    if (snap.sessionAlive &&
        (idle_.state().startupTaskSummaryPending_ || idle_.state().startupTaskSummaryActive_)) {
        // 定时器也检查一次，避免媒体事件消息尚未出队时让启动播报多停留一帧。
        idle_.cancelStartupSummary();
        publishPresentationFrame(snap, false, true);
    }
    if (!snap.sessionAlive)
        return;
    if (!snapshotMatchesTrackKey(snap, playback_.state().currentKey))
        return;
    const PlaybackPatch playback = presentation_.updatePlayback(snap);
    forEachHost([&](ILyricHost* host) {
        // 提前切换显示行，让上下滚动动画在下一句真正开始前完成；
        // 补丁中的 actualPositionMs 仍是真实播放时间，避免逐字高亮跟着提前。
        host->applyPlaybackPatch(playback);
    });
    if (taskbarHost && settings_.spectrumOn_ && !taskbarVertical_) {
        SpectrumPatch spectrumPatch;
        spectrumPatch.frameRevision = presentation_.frame().frameRevision;
        spectrumPatch.requestGeneration = presentation_.frame().requestGeneration;
        spectrumPatch.bands = spectrum_.bands();
        forEachHost([&](TaskbarHost* host) { host->applySpectrumPatch(spectrumPatch); });
    }
    if (manualSearchDialog && manualSearchDialog->isOpen()) {
        manualSearchDialog->setPlaybackPosition(snap.positionMs);
    }
}

void App::tryExtractAlbumColor() {
    if (!playback_.extractAlbumColor())
        return;
    const auto& frame = presentation_.updateAlbumColor(playback_.state().albumColor_);
    forEachHost([&](ILyricHost* host) { host->applyPresentationFrame(frame); });
    applyFontColors();
}
} // namespace lyrics_app
