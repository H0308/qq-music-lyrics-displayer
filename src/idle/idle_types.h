#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class IdleQuoteSource {
    Hitokoto,
    Jinrishici,
};

enum class IdleQuoteRefreshInterval {
    Daily,
    HalfDay,
    Hourly,
};

// Timor 年度接口返回的日期类型：0 工作日、1 周末、2 节日、3 调休/补班。
enum class IdleDayType {
    Workday = 0,
    Weekend = 1,
    Holiday = 2,
    MakeupWorkday = 3,
};

struct HolidayDayInfo {
    std::string date; // YYYY-MM-DD
    int type = static_cast<int>(IdleDayType::Workday);
    std::wstring name;
};

struct HolidayCalendarResult {
    bool ok = false;
    int year = 0;
    std::vector<HolidayDayInfo> days;
};

struct IdleQuoteResult {
    bool ok = false;
    std::wstring content;
    std::wstring origin;
    std::wstring uuid;
    std::wstring token;
};

// path、customName 是配置；name/icon 是启动或首次展示时读取的运行时资源。
struct IdleAppInfo {
    std::wstring path;
    std::wstring customName;
    std::wstring name;
    std::shared_ptr<const std::vector<BYTE>> iconPixels;
    UINT iconWidth = 0;
    UINT iconHeight = 0;
    bool pathValid = true;
};

constexpr std::size_t kMaxIdleApps = 10;

enum class IdleTaskPriority : int {
    None = 0,
    Low = 1,
    Medium = 3,
    High = 5,
};

struct IdleTaskInfo {
    std::wstring id;
    std::wstring projectId;
    std::wstring title;
    std::wstring dueText;
    bool completed = false;
    bool overdue = false;
    IdleTaskPriority priority = IdleTaskPriority::None;
};

struct IdlePresentation {
    std::wstring sentence;
    std::wstring source;
    bool loading = false;
    bool showQuote = true;
    bool copyEnabled = false;
    bool quickStartEnabled = true;
    bool showAppNames = true;
    std::vector<IdleAppInfo> apps;
    std::vector<IdleTaskInfo> todayTasks;
    bool todayTasksLoading = false;
    bool todayTasksConnected = false;
    std::wstring todayTasksStatus;
};
