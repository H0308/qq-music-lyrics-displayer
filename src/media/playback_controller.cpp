#include "media/playback_controller.h"
#include "media/track_identity.h"
#include "logging/runtime_logger.h"
#include "util/dominant_color.h"
#include <utility>

namespace {
constexpr UINT_PTR kTimerLyricDebounce = 3;
constexpr UINT kLyricDebounceMs = 300;

const wchar_t* coverSourceName(CoverSource source) {
    switch (source) {
    case CoverSource::None:
        return L"none";
    case CoverSource::Smtc:
        return L"smtc";
    case CoverSource::QqApi:
        return L"qq-api";
    case CoverSource::NeteaseApi:
        return L"netease-api";
    default:
        return L"unknown";
    }
}

const wchar_t* playerName(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return L"QQ Music";
    case SmtcPlayerType::NetEase:
        return L"NetEase Music";
    default:
        return L"Unknown";
    }
}

const wchar_t* statusName(PlaybackStatus s) {
    switch (s) {
    case PlaybackStatus::Playing:
        return L"Playing";
    case PlaybackStatus::Paused:
        return L"Paused";
    case PlaybackStatus::Stopped:
        return L"Stopped";
    default:
        return L"Other";
    }
}
} // namespace

PlaybackController::PlaybackController(std::function<SmtcSnapshot()> snapshot,
                                       std::function<void(const SmtcSnapshot&, bool, bool)> publish,
                                       std::function<void(const SmtcSnapshot&)> logState,
                                       std::function<void()> coverChanged)
    : snapshot_(std::move(snapshot)), publish_(std::move(publish)), logState_(std::move(logState)),
      coverChanged_(std::move(coverChanged)) {}

void PlaybackController::closeSession() {
    if (!state_.currentKey.empty() || state_.lastStatus != PlaybackStatus::Stopped)
        runtime_log::writef(L"[smtc] %s session closed", playerName(state_.lastPlayer_));
    state_.currentKey.clear();
    state_.currentTitle.clear();
    state_.currentArtist.clear();
    state_.currentDurationMs = 0;
    state_.lastStatus = PlaybackStatus::Stopped;
    state_.lyricLoading_ = false;
    cancelLyricDebounce();
    ++state_.requestGeneration_;
    releaseCurrentResources();
    state_.hasAlbumColor_ = false;
}

TrackChange PlaybackController::updateTrack(const SmtcSnapshot& snap,
                                            const std::wstring& previousSourceAppId) {
    const SmtcPlayerType previousPlayer = state_.lastPlayer_;
    state_.lastPlayer_ = snap.player;
    const std::wstring key = makeTrackKey(snap);
    const bool trackChanged = key != state_.currentKey && !snap.title.empty();
    // QQ 的时间线可能晚于媒体属性到达：同一首歌先以未知时长（0）进入，
    // 随后补上真实时长会让包含时长的 trackKey 变化。这个补齐不是换歌，
    // 不能因此清掉当前封面/专辑色；歌词仍按新时长重新建立匹配键。
    const bool durationOnlyTrackUpdate =
        trackChanged && snap.player == previousPlayer && !snap.timelineStale &&
        state_.currentDurationMs <= 0 && snap.durationMs > 0 && snap.title == state_.currentTitle &&
        snap.artist == state_.currentArtist && snap.sourceAppUserModelId == previousSourceAppId;
    // QQ 的曲目键包含时长，切歌后时间线更新会让 trackChanged 再触发一次；
    // 切歌弹窗只认标题/艺术家身份变化，避免同一首歌弹两次。
    const bool songIdentityChanged = !snap.title.empty() && (snap.title != state_.currentTitle ||
                                                             snap.artist != state_.currentArtist);
    const bool newSmtcThumbnail =
        snap.thumbnail && !snap.thumbnail->empty() &&
        (!state_.lastSmtcThumbnail || state_.lastSmtcThumbnail != snap.thumbnail);
    if (snap.status != state_.lastStatus) {
        runtime_log::writef(L"[smtc] %s status: %s", playerName(snap.player),
                            statusName(snap.status));
        state_.lastStatus = snap.status;
    }
    if (trackChanged) {
        // 时长补齐仍需重建歌词请求，但媒体卡片继续沿用当前封面和专辑色，
        // 避免 0 → 有效时长的那一帧出现空卡片或背景闪回。
        releaseCurrentLyrics();
        if (!durationOnlyTrackUpdate)
            releaseCurrentCover();
        state_.currentKey = key;
        state_.currentTitle = snap.title;
        state_.currentArtist = snap.artist;
        state_.currentDurationMs = snap.timelineStale ? 0 : snap.durationMs;
        if (snap.player == SmtcPlayerType::NetEase) {
            runtime_log::writef(L"[smtc] %s track: %s - %s [%s] (%lld ms)", playerName(snap.player),
                                snap.title.c_str(), snap.artist.c_str(), snap.neteaseSongId.c_str(),
                                snap.durationMs);
        } else {
            runtime_log::writef(L"[smtc] %s track: %s - %s (%lld ms)", playerName(snap.player),
                                snap.title.c_str(), snap.artist.c_str(), snap.durationMs);
        }
        if (snap.thumbnail && !snap.thumbnail->empty()) {
            state_.lastCover_ = snap.thumbnail;
            state_.lastCoverSource_ = CoverSource::Smtc;
            logCoverCreated(CoverSource::Smtc, state_.lastCover_);
        }
        if (!durationOnlyTrackUpdate)
            state_.hasAlbumColor_ = false;
        // 跟随专辑开启时不回退配置色：沿用上一首的专辑色直到新封面取色完成，
        // 避免歌词加载窗口期配置色闪一下再切回专辑色
        state_.lyricLoading_ = true;
        if (snap.player == SmtcPlayerType::NetEase) {
            cancelLyricDebounce();
            startLyricRequest(snap);
            if (!snap.thumbnail || snap.thumbnail->empty())
                requestNeteaseCoverFallback(snap.neteaseSongId);
        } else if (timerWindow_) {
            // QQ：等 SMTC 切歌事件批合并后再请求；期间 state_.lyricLoading_ 保持 true，
            // 界面停留在「歌词加载中…」而不是闪「暂无歌词」。
            if (state_.lyricDebounceArmed_)
                KillTimer(timerWindow_, kTimerLyricDebounce);
            state_.lyricDebounceDeadline_ = GetTickCount64() + 8000;
            state_.lyricDebounceArmed_ =
                SetTimer(timerWindow_, kTimerLyricDebounce, kLyricDebounceMs, nullptr) != 0;
            if (!state_.lyricDebounceArmed_)
                startLyricRequest(snap); // 定时器创建失败时按原逻辑立即请求
        } else {
            startLyricRequest(snap);
        }
    } else {
        if (!snap.timelineStale && snap.durationMs > 0)
            state_.currentDurationMs = snap.durationMs;
        if (newSmtcThumbnail) {
            if (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty()) {
                runtime_log::writef(L"[resource][event] cover-released source=smtc bytes=%zu",
                                    state_.lastSmtcThumbnail->size());
            } else if (state_.lastCover_ && !state_.lastCover_->empty() &&
                       state_.lastCover_ != snap.thumbnail) {
                runtime_log::writef(L"[resource][event] cover-released source=%s bytes=%zu",
                                    coverSourceName(state_.lastCoverSource_),
                                    state_.lastCover_->size());
            }
            logCoverCreated(CoverSource::Smtc, snap.thumbnail);
        }
        if (snap.thumbnail && !snap.thumbnail->empty()) {
            state_.lastCover_ = snap.thumbnail;
            state_.lastCoverSource_ = CoverSource::Smtc;
        }
    }
    state_.lastSmtcThumbnail = snap.thumbnail;
    // 时间线从残留恢复可信（stale 1→0）：若当前歌词请求是在不可信期间发出的
    // （等待超时被迫发出），用可信时长立即重发，覆盖可能已被时长过滤淘汰的结果。
    if (!trackChanged && snap.player == SmtcPlayerType::QQMusic && state_.lyricRequestStale_ &&
        !snap.timelineStale && !state_.lyricDebounceArmed_ && state_.lyricLoading_ &&
        state_.currentLyrics_.empty()) {
        startLyricRequest(snap);
    }

    return {trackChanged, durationOnlyTrackUpdate, songIdentityChanged};
}

void PlaybackController::applyManualLyrics(LyricResult result, const SmtcSnapshot& snap) {
    // 手动覆盖是新的内容事务，使仍在队列中的自动歌词/封面结果失效。
    ++state_.requestGeneration_;
    cancelLyricDebounce(); // 防止挂起的切歌防抖请求到点后覆盖手动选择
    releaseCurrentLyrics();
    state_.currentLyrics_ = std::move(result.lines);
    state_.currentLyricsFromLocal_ = false;
    state_.currentLyricsFromManual_ = true;
    state_.lyricLoading_ = false;
    updateLyricCapabilities(state_.currentLyrics_);
    logLyricsCreated(L"manual");
    publish_(snap, true, true);
    runtime_log::writef(L"[lyric] manual override applied: %s", state_.currentKey.c_str());
    logState_(snap);
    // 手动选择后同样兜底封面；网易云不依赖手动候选，仍使用当前播放歌曲 ID。
    if (!state_.lastSmtcThumbnail || state_.lastSmtcThumbnail->empty()) {
        if (snap.player == SmtcPlayerType::NetEase)
            requestNeteaseCoverFallback(snap.neteaseSongId);
        else if (!result.info.albummid.empty())
            requestQqCoverFallback(result.info.albummid);
        else
            requestQqCoverInfoFallback(snap);
    }
}

bool PlaybackController::extractAlbumColor() {
    if (state_.hasAlbumColor_ || !state_.lastCover_ || state_.lastCover_->empty())
        return false;
    auto color = extractDominantColor(*state_.lastCover_);
    if (!color)
        return false;
    state_.albumColor_ = *color;
    state_.hasAlbumColor_ = true;
    runtime_log::writef(L"[color] album dominant: #%02X%02X%02X : %s", GetRValue(*color),
                        GetGValue(*color), GetBValue(*color), state_.currentKey.c_str());
    return true;
}

void PlaybackController::logLyricsCreated(const wchar_t* source) {
    if (!state_.currentLyrics_.empty())
        runtime_log::writef(L"[resource][event] lyrics-created source=%s lines=%zu", source,
                            state_.currentLyrics_.size());
}

void PlaybackController::releaseCurrentLyrics() {
    if (!state_.currentLyrics_.empty()) {
        const wchar_t* source = state_.currentLyricsFromManual_  ? L"manual"
                                : state_.currentLyricsFromLocal_ ? L"local"
                                                                 : L"online";
        runtime_log::writef(L"[resource][event] lyrics-released source=%s lines=%zu", source,
                            state_.currentLyrics_.size());
    }
    state_.currentLyrics_.clear();
    state_.currentLyricsFromLocal_ = false;
    state_.currentLyricsFromManual_ = false;
    updateLyricCapabilities({});
}

void PlaybackController::logCoverCreated(CoverSource source,
                                         const std::shared_ptr<const std::vector<uint8_t>>& cover) {
    if (!cover || cover->empty())
        return;
    const wchar_t* sourceName = coverSourceName(source);
    runtime_log::writef(L"[resource][event] cover-created source=%s bytes=%zu", sourceName,
                        cover->size());
    runtime_log::writef(L"[cover] selected source=%s track=%s bytes=%zu", sourceName,
                        state_.currentKey.c_str(), cover->size());
}

CoverProvider::ReadyCallback PlaybackController::coverReadyCallback(CoverSource source) {
    const std::wstring key = state_.currentKey;
    const uint64_t generation = state_.requestGeneration_;
    return [this, key, generation, source](std::shared_ptr<const std::vector<uint8_t>> cover) {
        if (!cover || cover->empty())
            return;
        auto* payload = new CoverPayload{key, generation, source, std::move(cover)};
        if (!PostThreadMessageW(mainThread_, kMsgCoverReady, 1, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    };
}

void PlaybackController::requestQqCoverFallback(const std::wstring& albummid) {
    if (!albummid.empty())
        coverProvider.requestAsync(albummid, coverReadyCallback(CoverSource::QqApi));
}

void PlaybackController::requestQqCoverInfoFallback(const SmtcSnapshot& snap) {
    const std::wstring key = state_.currentKey;
    const uint64_t generation = state_.requestGeneration_;
    const DWORD targetThread = mainThread_;
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    provider.requestQqSongInfoAsync(
        snap.title, snap.artist, durationMs, [targetThread, key, generation](const SongInfo& info) {
            auto* payload = new QqCoverInfoPayload{key, generation, info.albummid};
            if (!PostThreadMessageW(targetThread, kMsgQqCoverInfoReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

void PlaybackController::requestNeteaseCoverFallback(const std::wstring& songId) {
    if (!songId.empty())
        coverProvider.requestNeteaseAsync(songId, coverReadyCallback(CoverSource::NeteaseApi));
}

void PlaybackController::releaseCurrentCover() {
    if (state_.lastCover_ && !state_.lastCover_->empty())
        runtime_log::writef(L"[resource][event] cover-released source=%s bytes=%zu",
                            coverSourceName(state_.lastCoverSource_), state_.lastCover_->size());
    else if (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty())
        runtime_log::writef(L"[resource][event] cover-released source=smtc bytes=%zu",
                            state_.lastSmtcThumbnail->size());
    state_.lastCover_.reset();
    state_.lastCoverSource_ = CoverSource::None;
    state_.lastSmtcThumbnail.reset();
}

void PlaybackController::releaseCurrentResources() {
    releaseCurrentLyrics();
    releaseCurrentCover();
}

void PlaybackController::updateLyricCapabilities(const std::vector<LyricLine>& lines) {
    state_.currentHasTranslation_ = false;
    state_.currentHasRomanization_ = false;
    for (const auto& line : lines) {
        state_.currentHasTranslation_ = state_.currentHasTranslation_ || !line.translation.empty();
        state_.currentHasRomanization_ =
            state_.currentHasRomanization_ || !line.romanization.empty();
    }
}

void PlaybackController::startLyricRequest(const SmtcSnapshot& snap, bool forceOnline,
                                           bool forceLocal, bool persistOrder) {
    state_.lyricLoading_ = true;
    state_.currentLyricsFromLocal_ = false;
    state_.currentLyricsFromManual_ = false;
    state_.lyricRequestStale_ = snap.timelineStale;
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    ++state_.requestGeneration_;
    logState_(snap);
    const std::wstring requestKey = state_.currentKey;
    const uint64_t generation = state_.requestGeneration_;
    auto postLyricResult = [threadId = mainThread_, requestKey, generation](LyricResult result) {
        auto* payload = new LyricPayload{requestKey, generation, std::move(result)};
        if (!PostThreadMessageW(threadId, kMsgLyricReady, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    };
    if (snap.player == SmtcPlayerType::NetEase) {
        provider.requestNeteaseAsync(snap.neteaseSongId, snap.title, snap.artist, durationMs,
                                     postLyricResult);
    } else {
        provider.requestAsync(snap.title, snap.artist, durationMs, postLyricResult, forceOnline,
                              forceLocal, persistOrder);
    }
}

void PlaybackController::cancelLyricDebounce() {
    if (state_.lyricDebounceArmed_ && timerWindow_)
        KillTimer(timerWindow_, kTimerLyricDebounce);
    state_.lyricDebounceArmed_ = false;
}

void PlaybackController::onLyricDebounce() {
    state_.lyricDebounceArmed_ = false;
    SmtcSnapshot snap = snapshot_();
    if (!snap.sessionAlive || snap.player != SmtcPlayerType::QQMusic || snap.title.empty())
        return;
    if (makeTrackKey(snap) != state_.currentKey)
        return;
    if (snap.timelineStale && GetTickCount64() < state_.lyricDebounceDeadline_) {
        // 时间线仍是旧歌残留：durationMs 不可信， lyric 请求的时长过滤会
        // 把正确候选全部淘汰（失败会先闪「暂无歌词」）。继续等新时间线。
        if (timerWindow_)
            state_.lyricDebounceArmed_ =
                SetTimer(timerWindow_, kTimerLyricDebounce, kLyricDebounceMs, nullptr) != 0;
        return;
    }
    startLyricRequest(snap);
}

void PlaybackController::onLyricReady(std::unique_ptr<LyricPayload> payload) {
    if (!payload || payload->key != state_.currentKey ||
        payload->requestGeneration != state_.requestGeneration_)
        return;
    SmtcSnapshot snap = snapshot_();
    if (!snapshotMatchesTrackKey(snap, state_.currentKey))
        return;

    state_.lyricLoading_ = false;
    std::wstring coverAlbummid;
    if (payload->result.ok) {
        state_.currentLyrics_ = std::move(payload->result.lines);
        state_.currentLyricsFromLocal_ =
            snap.player == SmtcPlayerType::QQMusic && payload->result.fromLocal;
        state_.currentLyricsFromManual_ = payload->result.fromManual;
        updateLyricCapabilities(state_.currentLyrics_);
        logLyricsCreated(state_.currentLyricsFromManual_  ? L"manual"
                         : state_.currentLyricsFromLocal_ ? L"local"
                                                          : L"online");
        runtime_log::writef(L"[lyric] loaded %zu lines: %s", state_.currentLyrics_.size(),
                            state_.currentKey.c_str());
        coverAlbummid = std::move(payload->result.info.albummid);
    } else {
        if (state_.lyricRequestStale_) {
            // 时间线不可信期间发出的请求：失败大概率是旧时长触发候选的 15 秒
            // 时长过滤，不能据此判定「暂无歌词」。保持「加载中」，等新时间线
            // 可信后由 onSmtcChanged 的重试分支用正确时长重发。
            state_.lyricLoading_ = true;
            publish_(snap, false, false);
            logState_(snap);
            return;
        }
        releaseCurrentLyrics();
        runtime_log::writef(L"[lyric] not found: %s", state_.currentKey.c_str());
    }
    // QQ 封面标识与歌词来源解耦：在线歌词已带 albummid 时直接使用；本地歌词或
    // 歌词未命中时单独复用 QQ 匹配规则查询，不替换当前歌词。
    if (snap.player == SmtcPlayerType::QQMusic &&
        (!state_.lastSmtcThumbnail || state_.lastSmtcThumbnail->empty()) &&
        (!state_.lastCover_ || state_.lastCover_->empty())) {
        if (!coverAlbummid.empty())
            requestQqCoverFallback(coverAlbummid);
        else
            requestQqCoverInfoFallback(snap);
    }
    publish_(snap, true, true);
    logState_(snap);
}

void PlaybackController::onQqCoverInfoReady(std::unique_ptr<QqCoverInfoPayload> payload) {
    if (!payload || payload->key != state_.currentKey ||
        payload->requestGeneration != state_.requestGeneration_)
        return;
    SmtcSnapshot snap = snapshot_();
    if (!snapshotMatchesTrackKey(snap, state_.currentKey) ||
        (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty()) ||
        (state_.lastCover_ && !state_.lastCover_->empty()))
        return;
    if (payload->albummid.empty()) {
        runtime_log::writef(L"[cover][QQ] no albummid from song-info: %s",
                            state_.currentKey.c_str());
        return;
    }
    requestQqCoverFallback(payload->albummid);
}

void PlaybackController::onCoverReady(std::unique_ptr<CoverPayload> payload) {
    if (!payload || !payload->cover || payload->key != state_.currentKey ||
        payload->requestGeneration != state_.requestGeneration_)
        return; // 已切歌或重新加载，丢弃过期封面
    SmtcSnapshot snap = snapshot_();
    if (!snapshotMatchesTrackKey(snap, state_.currentKey))
        return;
    if (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty())
        return; // SMTC 已提供有效封面，优先使用
    state_.lastCover_ = payload->cover;
    state_.lastCoverSource_ = payload->source;
    logCoverCreated(payload->source, state_.lastCover_);
    publish_(snap, true, false);
    coverChanged_();
    logState_(snap);
}

void PlaybackController::reloadCurrentQqLyrics(bool forceOnline, bool forceLocal,
                                               bool persistOrder) {
    const SmtcSnapshot snap = snapshot_();
    if (!snap.sessionAlive || snap.player != SmtcPlayerType::QQMusic || state_.currentKey.empty() ||
        snap.title.empty() || snap.timelineStale || makeTrackKey(snap) != state_.currentKey ||
        (forceOnline && !state_.currentLyricsFromLocal_) ||
        (forceLocal && (state_.currentLyricsFromLocal_ || state_.currentLyricsFromManual_)))
        return;

    runtime_log::writef(L"[action][lyrics] reload force-online=%d force-local=%d persist-order=%d",
                        forceOnline ? 1 : 0, forceLocal ? 1 : 0, persistOrder ? 1 : 0);
    cancelLyricDebounce();
    releaseCurrentLyrics();
    state_.lyricLoading_ = true;
    startLyricRequest(snap, forceOnline, forceLocal, persistOrder);
    publish_(snap, false, true);
}
