#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace settings_store {

enum class LoadStatus { Loaded, Missing, OpenError, ReadError, ParseError };

struct LoadResult {
    LoadStatus status = LoadStatus::Missing;
    nlohmann::json document;
};

// 只负责文件读写及 JSON 解析；设置含义、迁移和错误提示由应用层处理。
LoadResult load(const std::filesystem::path& path);

// 先完整写入同目录临时文件并刷新、关闭，再替换目标文件；失败抛出异常。
void save(const std::filesystem::path& path, const nlohmann::json& document);

} // namespace settings_store
