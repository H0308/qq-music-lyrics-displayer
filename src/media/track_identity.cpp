#include "media/track_identity.h"
#include "lyric/lyric_provider.h"

std::wstring makeTrackKey(const SmtcSnapshot& snap) {
    if (snap.player == SmtcPlayerType::NetEase && !snap.neteaseSongId.empty())
        return L"netease|" + snap.neteaseSongId;
    // QQ 切歌窗口可能把上一首的时间线残留带进当前快照；它对展示和歌词匹配
    // 都是不可信的，不能让这段时长参与曲目身份，否则真实时长到达时会被误判为换歌。
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    return L"qq|" + LyricProvider::makeKey(snap.title, snap.artist, durationMs);
}

bool snapshotMatchesTrackKey(const SmtcSnapshot& snap, const std::wstring& key) {
    if (!snap.sessionAlive)
        return false;
    // SMTC 的 Changing/Opened 过渡可能暂时没有完整文案；此时保留当前会话，
    // 待下一份完整快照再做曲目身份判断。
    if (snap.title.empty() && snap.neteaseSongId.empty())
        return true;
    return key.empty() || makeTrackKey(snap) == key;
}
