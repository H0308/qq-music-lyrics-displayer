#pragma once

#include "idle_types.h"

#include <functional>
#include <memory>
#include <string>

// 滴答清单（Dida365）普通用户 API 口令访问。
// API 口令由用户在滴答清单「账号与安全」中生成，并由实现写入 Windows 凭据管理器。
enum class TickTickService {
    Dida365,
};

struct TickTickTasksResult {
    bool ok = false;
    bool authRequired = false;
    std::wstring error;
    std::vector<IdleTaskInfo> tasks;
};

class TickTickProvider {
public:
    using TasksReadyCallback = std::function<void(TickTickTasksResult)>;

    TickTickProvider();
    ~TickTickProvider();

    TickTickProvider(const TickTickProvider&) = delete;
    TickTickProvider& operator=(const TickTickProvider&) = delete;

    void requestTodayTasksAsync(TickTickService service, const std::wstring& apiToken,
                                TasksReadyCallback cb);

    bool loadApiToken(TickTickService service, std::wstring& apiToken) const;
    bool saveApiToken(TickTickService service, const std::wstring& apiToken);
    bool hasApiToken(TickTickService service) const;
    void clearApiToken(TickTickService service);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
