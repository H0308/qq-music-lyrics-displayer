#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct LyricLine {
    int64_t ms = 0;
    std::wstring text;
};

// 搜索匹配到的歌曲信息（用于歌词下载，也用于封面兜底）
struct SongInfo {
    std::wstring songmid;
    std::wstring albummid;
};

// 手动搜索返回的候选歌曲
struct SearchCandidate {
    std::wstring songmid;
    std::wstring albummid;
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
};

// 歌词获取：QQ 音乐公开接口 搜索 -> 三重匹配 -> 下载 -> base64 解码 -> 解析 LRC
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
    void requestAsync(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
                      ReadyCallback cb);

    // 手动搜索候选（最多 5 条），结果在 UI 线程回调
    void searchCandidatesAsync(const std::wstring& title, const std::wstring& artist,
                               SearchCallback cb);

    // 按 songmid 异步取歌词（不修改当前播放歌词），结果在 UI 线程回调
    void fetchLyricAsync(const SearchCandidate& cand, FetchCallback cb);

    // 设置手动选择：写入缓存并作为当前歌词，以后同一首歌优先使用
    void setManualOverride(const std::wstring& title, const std::wstring& artist,
                           std::vector<LyricLine> lines, SongInfo info);

    // 最近一次应用的歌词（仅在 ReadyCallback(true) 之后于 UI 线程读取）
    const std::vector<LyricLine>& lines() const;

    // 最近一次匹配到的歌曲信息（仅在 ReadyCallback(true) 之后于 UI 线程读取）
    const SongInfo& songInfo() const;

    static std::wstring makeKey(const std::wstring& title, const std::wstring& artist);
    // 二分定位：最后一个 ms <= positionMs 的行号；无则 -1
    static int findLine(const std::vector<LyricLine>& lines, int64_t positionMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
