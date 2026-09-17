#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 专辑封面下载：当 SMTC 没有提供有效缩略图时，通过播放器接口兜底
class CoverProvider {
public:
    using ReadyCallback = std::function<void(std::shared_ptr<const std::vector<uint8_t>>)>;

    CoverProvider();
    ~CoverProvider();

    CoverProvider(const CoverProvider&) = delete;
    CoverProvider& operator=(const CoverProvider&) = delete;

    // QQ 音乐：按 albummid 异步下载封面；callback 在工作线程触发（失败时返回 nullptr）
    void requestAsync(const std::wstring& albummid, ReadyCallback cb);

    // 网易云：按增强 SMTC 提供的歌曲 ID 查询歌曲详情并异步下载专辑封面
    void requestNeteaseAsync(const std::wstring& songId, ReadyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
