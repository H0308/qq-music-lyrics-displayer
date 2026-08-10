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

// 歌词获取：QQ 音乐公开接口 搜索 -> 三重匹配 -> 下载 -> base64 解码 -> 解析 LRC
class LyricProvider {
public:
    using ReadyCallback = std::function<void(bool ok)>; // 在工作线程触发（缓存命中时同步触发）

    LyricProvider();
    ~LyricProvider();

    LyricProvider(const LyricProvider&) = delete;
    LyricProvider& operator=(const LyricProvider&) = delete;

    // 异步请求歌词；同名歌曲按"标题完全匹配 + 歌手包含 + |时长差|<=2s"匹配，失败取第一条
    void requestAsync(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
                      ReadyCallback cb);

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
