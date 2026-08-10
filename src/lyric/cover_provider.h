#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 专辑封面下载：当 SMTC 没有提供缩略图时，通过 QQ 音乐 albummid 兜底
class CoverProvider {
public:
    using ReadyCallback = std::function<void(std::shared_ptr<const std::vector<uint8_t>>)>;

    CoverProvider();
    ~CoverProvider();

    CoverProvider(const CoverProvider&) = delete;
    CoverProvider& operator=(const CoverProvider&) = delete;

    // 异步下载封面；callback 在工作线程触发（失败时返回 nullptr）
    void requestAsync(const std::wstring& albummid, ReadyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
