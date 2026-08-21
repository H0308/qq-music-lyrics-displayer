#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 逐字时间轴（QRC/KRC/YRC 解析结果）；text 为歌词时间轴 token，英文常为整词
struct LyricChar {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::wstring text;
};

struct LyricLine {
    int64_t ms = 0;
    std::wstring text;
    std::vector<LyricChar> chars; // 非空表示有逐字时间轴；text 为 chars 拼接
    std::wstring translation;     // 与本行时间轴对齐的翻译（可空）
    std::wstring romanization;    // 与本行时间轴对齐的罗马音（可空）
};

// 搜索匹配到的歌曲信息（用于歌词下载，也用于封面兜底）
struct SongInfo {
    std::wstring songmid;
    std::wstring albummid;
    std::wstring neteaseSongId;
};

enum class LyricSource {
    Qrc, // QQ 音乐原生逐字歌词
    Krc, // 酷狗逐字歌词
    Lrc, // QQ 音乐整行歌词
    Yrc  // 网易云音乐逐字歌词
};

// 手动搜索返回的候选歌曲
struct SearchCandidate {
    std::wstring songmid;   // QQ 歌曲 mid（LRC 候选使用）
    std::wstring songid;    // QQ 数字歌曲 ID（QRC 候选使用）
    std::wstring albummid;
    std::wstring kugouHash; // 酷狗歌曲 hash（KRC 候选使用）
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
    LyricSource source = LyricSource::Lrc;
    std::wstring neteaseSongId; // 网易云歌曲 ID（YRC 候选使用）
};

// 歌词获取：QQ 使用现有 QRC/KRC/LRC 链路；网易云按歌曲 ID 获取 YRC，失败后回退 LRC。
class LyricProvider {
public:
    using ReadyCallback = std::function<void(bool ok)>; // 在工作线程触发（缓存命中时同步触发）
    using SearchCallback = std::function<void(const std::vector<SearchCandidate>&)>;
    using FetchCallback = std::function<void(bool ok, const std::vector<LyricLine>& lines,
                                             const SongInfo& info)>;

    LyricProvider();
    ~LyricProvider();

    LyricProvider(const LyricProvider&) = delete;
    LyricProvider& operator=(const LyricProvider&) = delete;

    // 异步请求歌词；优先按版本标注匹配（本地有版本则候选必须有相同版本，
    // 本地无版本则候选优先不带版本），无合适结果时回退到加权模糊匹配
    // bypassLocal 仅用于“获取在线版歌词”操作；手动保存的歌词仍然优先。
    void requestAsync(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
                      ReadyCallback cb, bool bypassLocal = false);

    // 按网易云歌曲 ID 异步取歌词：优先 YRC，失败后回退网易云 LRC。
    // 同时传入当前元数据，用于读取按歌曲 ID 保存的手动歌词覆盖。
    void requestNeteaseAsync(const std::wstring& songId, const std::wstring& title,
                             const std::wstring& artist, int64_t durationMs,
                             ReadyCallback cb);

    // 手动搜索候选（网易云 YRC/LRC、QQ QRC/KRC/LRC 各最多 5 条），结果在 UI 线程回调
    void searchCandidatesAsync(const std::wstring& title, const std::wstring& artist,
                               SearchCallback cb);

    // 按 songmid 异步取歌词（不修改当前播放歌词），结果在 UI 线程回调
    void fetchLyricAsync(const SearchCandidate& cand, FetchCallback cb);

    // 设置手动选择：写入缓存并作为当前歌词；QQ 按标题/歌手/时长复用，
    // 网易云按歌曲 ID 复用。
    void setManualOverride(const std::wstring& title, const std::wstring& artist,
                           int64_t durationMs, std::vector<LyricLine> lines, SongInfo info);

    // 设置手动歌词的持久化目录；每首歌保存为独立文件，请求歌词时按 key 查找对应文件
    void setManualOverrideDir(const std::wstring& dir);

    // 设置 QQ 音乐本地 QRC 来源；仅影响 QQ 请求，未启用或目录为空时保持原有在线链路。
    void setQqLocalLyricsConfig(bool enabled, const std::wstring& dir);

    // 最近一次应用的歌词（仅在 ReadyCallback(true) 之后于 UI 线程读取）
    const std::vector<LyricLine>& lines() const;

    // 最近一次匹配到的歌曲信息（仅在 ReadyCallback(true) 之后于 UI 线程读取）
    const SongInfo& songInfo() const;

    // 最近一次成功加载的歌词是否来自 QQ 音乐本地 QRC（仅在 ReadyCallback(true) 后读取）
    bool lastLoadWasLocal() const;

    // 最近一次成功加载的歌词是否来自手动保存（仅在 ReadyCallback(true) 后读取）
    bool lastLoadWasManual() const;

    // 缓存/覆盖键：标题 + 歌手 + 时长（5 秒分桶），区分同名不同版本
    static std::wstring makeKey(const std::wstring& title, const std::wstring& artist,
                                int64_t durationMs);
    // 二分定位：最后一个 ms <= positionMs 的行号；无则 -1
    static int findLine(const std::vector<LyricLine>& lines, int64_t positionMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
