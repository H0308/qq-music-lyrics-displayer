#include "settings_store.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace settings_store {
namespace {

[[noreturn]] void throwFileError(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

// 只拥有本次保存创建的临时文件；任何失败路径都不删除或截断正式配置。
class PendingFile {
public:
    explicit PendingFile(const std::filesystem::path& target) {
        GUID id{};
        const HRESULT hr = CoCreateGuid(&id);
        if (FAILED(hr))
            throw std::system_error(static_cast<int>(hr), std::system_category(), "CoCreateGuid");
        wchar_t name[39]{};
        StringFromGUID2(id, name, static_cast<int>(std::size(name)));
        path_ = target;
        path_ += std::wstring(L".") + name + L".tmp";
        handle_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throwFileError("CreateFileW(settings temporary file)");
    }

    ~PendingFile() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        if (!path_.empty())
            DeleteFileW(path_.c_str());
    }

    PendingFile(const PendingFile&) = delete;
    PendingFile& operator=(const PendingFile&) = delete;

    void writeAndReplace(const std::string& bytes, const std::filesystem::path& target) {
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD count = static_cast<DWORD>(std::min<size_t>(
                bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data() + offset, count, &written, nullptr))
                throwFileError("WriteFile(settings)");
            if (written == 0)
                throw std::system_error(ERROR_WRITE_FAULT, std::system_category(), "WriteFile(settings)");
            offset += written;
        }
        if (!FlushFileBuffers(handle_))
            throwFileError("FlushFileBuffers(settings)");
        if (!CloseHandle(handle_))
            throwFileError("CloseHandle(settings)");
        handle_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(path_.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throwFileError("MoveFileExW(settings)");
        path_.clear();
    }

private:
    std::filesystem::path path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace

LoadResult load(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {exists || error ? LoadStatus::OpenError : LoadStatus::Missing, {}};
    auto document = nlohmann::json::parse(file, nullptr, false);
    if (file.bad())
        return {LoadStatus::ReadError, {}};
    if (document.is_discarded())
        return {LoadStatus::ParseError, {}};
    return {LoadStatus::Loaded, std::move(document)};
}

void save(const std::filesystem::path& path, const nlohmann::json& document) {
    const std::string bytes = document.dump();
    PendingFile pending(path);
    pending.writeAndReplace(bytes, path);
}

} // namespace settings_store
