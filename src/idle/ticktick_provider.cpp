#include <windows.h>
#include <wincred.h>

#include "ticktick_provider.h"

#include "logging/runtime_logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr char kDidaApiBaseUrl[] = "https://api.dida365.com/open/v1";
constexpr char kUserAgent[] = "QQMusicLyric/2.2.0";

std::string toUtf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

std::wstring toWide(const std::string& value) {
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        length);
    return result;
}

size_t curlWrite(char* data, size_t size, size_t count, void* userdata) {
    auto* output = static_cast<std::string*>(userdata);
    const size_t total = size * count;
    output->append(data, total);
    return total;
}

struct CurlRequestContext {
    std::atomic<bool>* shutdown = nullptr;
};

int curlXferAbort(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* context = static_cast<const CurlRequestContext*>(userdata);
    return context && context->shutdown && context->shutdown->load() ? 1 : 0;
}

struct HttpResponse {
    CURLcode curlResult = CURLE_FAILED_INIT;
    long statusCode = 0;
    std::string body;
};

bool performHttp(const std::string& url, const std::vector<std::string>& requestHeaders,
                 std::atomic<bool>& shutdown, HttpResponse& response) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    curl_slist* headers = nullptr;
    for (const auto& value : requestHeaders)
        headers = curl_slist_append(headers, value.c_str());

    CurlRequestContext context{&shutdown};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferAbort);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);

    response.curlResult = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response.curlResult == CURLE_OK;
}

bool requestJson(const std::string& url, const std::vector<std::string>& headers,
                 std::atomic<bool>& shutdown, json& result, long& statusCode) {
    HttpResponse response;
    if (!performHttp(url, headers, shutdown, response)) {
        statusCode = response.statusCode;
        return false;
    }
    statusCode = response.statusCode;
    if (statusCode < 200 || statusCode >= 300 || response.body.empty())
        return false;
    result = json::parse(response.body, nullptr, false);
    return !result.is_discarded();
}

std::string percentEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() + 16);
    for (const unsigned char valueByte : value) {
        if ((valueByte >= 'a' && valueByte <= 'z') ||
            (valueByte >= 'A' && valueByte <= 'Z') ||
            (valueByte >= '0' && valueByte <= '9') || valueByte == '-' || valueByte == '_' ||
            valueByte == '.' || valueByte == '~') {
            result.push_back(static_cast<char>(valueByte));
        } else {
            result.push_back('%');
            result.push_back(kHex[valueByte >> 4]);
            result.push_back(kHex[valueByte & 0x0F]);
        }
    }
    return result;
}

std::wstring apiTokenTarget(TickTickService) {
    return L"QQMusicLyric.TickTick.Dida365";
}

bool readCredentialBlob(const std::wstring& target, std::string& blob) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) || !credential)
        return false;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0)
        blob.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                    credential->CredentialBlobSize);
    CredFree(credential);
    return !blob.empty();
}

bool writeCredentialBlob(const std::wstring& target, const std::string& blob) {
    if (blob.empty() || blob.size() > MAXDWORD)
        return false;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

void deleteCredential(const std::wstring& target) {
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

bool readApiToken(TickTickService service, std::wstring& apiToken) {
    std::string blob;
    if (!readCredentialBlob(apiTokenTarget(service), blob)) {
        apiToken.clear();
        return false;
    }

    // 兼容旧版 OAuth 令牌格式，但不把旧的 access_token JSON 当作 API 口令使用。
    const json legacy = json::parse(blob, nullptr, false);
    if (legacy.is_object() && legacy.contains("access_token")) {
        apiToken.clear();
        return false;
    }
    apiToken = toWide(blob);
    return !apiToken.empty();
}

bool writeApiToken(TickTickService service, const std::wstring& apiToken) {
    return writeCredentialBlob(apiTokenTarget(service), toUtf8(apiToken));
}

void clearApiToken(TickTickService service) {
    deleteCredential(apiTokenTarget(service));
}

bool requestApiJson(const std::wstring& apiToken, const std::string& url,
                   std::atomic<bool>& shutdown, json& result, bool& authRequired) {
    if (apiToken.empty()) {
        authRequired = true;
        return false;
    }

    const std::vector<std::string> headers = {
        "Accept: application/json", "Authorization: Bearer " + toUtf8(apiToken)};
    long status = 0;
    if (requestJson(url, headers, shutdown, result, status))
        return true;
    if (status == 401)
        authRequired = true;
    return false;
}

std::string stringField(const json& object, const char* name) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_string())
        return {};
    return object[name].get<std::string>();
}

int intField(const json& object, const char* name, int fallback = 0) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_number_integer())
        return fallback;
    return object[name].get<int>();
}

bool boolField(const json& object, const char* name, bool fallback = false) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_boolean())
        return fallback;
    return object[name].get<bool>();
}

std::string datePrefix(const std::string& value) {
    if (value.size() < 10 || value[4] != '-' || value[7] != '-')
        return {};
    return value.substr(0, 10);
}

bool parseIsoLocal(const std::string& value, std::tm& local, bool& hasTime) {
    if (value.size() < 10)
        return false;
    int year = 0;
    int month = 0;
    int day = 0;
    if (sscanf_s(value.c_str(), "%4d-%2d-%2d", &year, &month, &day) != 3)
        return false;
    local = {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    hasTime = value.size() >= 19 && value[10] == 'T';
    if (!hasTime)
        return true;

    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf_s(value.c_str() + 11, "%2d:%2d:%2d", &hour, &minute, &second) != 3)
        return false;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;

    const size_t zone = value.find_first_of("Zz+-", 19);
    if (zone == std::string::npos)
        return true;

    int offsetMinutes = 0;
    if (value[zone] == 'Z' || value[zone] == 'z') {
        offsetMinutes = 0;
    } else {
        int offsetHour = 0;
        int offsetMinute = 0;
        const char* offset = value.c_str() + zone + 1;
        if (sscanf_s(offset, "%2d:%2d", &offsetHour, &offsetMinute) < 1)
            return false;
        if (zone + 3 < value.size() && value[zone + 3] == ':')
            offsetMinute = std::atoi(value.substr(zone + 4, 2).c_str());
        else if (zone + 4 < value.size())
            offsetMinute = std::atoi(value.substr(zone + 3, 2).c_str());
        offsetMinutes = offsetHour * 60 + offsetMinute;
        if (value[zone] == '-')
            offsetMinutes = -offsetMinutes;
    }

    const __time64_t utcAsLocal = _mkgmtime64(&local);
    if (utcAsLocal == -1)
        return false;
    const __time64_t utc = utcAsLocal - static_cast<__time64_t>(offsetMinutes) * 60;
    return _localtime64_s(&local, &utc) == 0;
}

std::string localDate(const std::string& value) {
    std::tm local{};
    bool hasTime = false;
    if (!parseIsoLocal(value, local, hasTime))
        return datePrefix(value);
    char result[16]{};
    sprintf_s(result, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
              local.tm_mday);
    return result;
}

std::wstring dueText(const std::string& value, const std::string& today) {
    if (value.empty())
        return L"今天";
    std::tm local{};
    bool hasTime = false;
    if (!parseIsoLocal(value, local, hasTime))
        return L"今天";
    wchar_t text[32]{};
    if (localDate(value) == today) {
        if (!hasTime)
            return L"今天";
        swprintf_s(text, L"今天 %02d:%02d", local.tm_hour, local.tm_min);
    } else if (hasTime) {
        swprintf_s(text, L"%02d-%02d %02d:%02d", local.tm_mon + 1, local.tm_mday,
                   local.tm_hour, local.tm_min);
    } else {
        swprintf_s(text, L"截止 %02d-%02d", local.tm_mon + 1, local.tm_mday);
    }
    return text;
}

bool taskIsForToday(const json& task, const std::string& today) {
    const std::string start = localDate(stringField(task, "startDate"));
    const std::string due = localDate(stringField(task, "dueDate"));
    if (due == today)
        return true;
    if (!start.empty() && start <= today && (due.empty() || today <= due))
        return true;
    return due.empty() && start == today;
}

IdleTaskPriority taskPriority(int value) {
    if (value >= static_cast<int>(IdleTaskPriority::High))
        return IdleTaskPriority::High;
    if (value >= static_cast<int>(IdleTaskPriority::Medium))
        return IdleTaskPriority::Medium;
    if (value >= static_cast<int>(IdleTaskPriority::Low))
        return IdleTaskPriority::Low;
    return IdleTaskPriority::None;
}

int compareTaskTitles(const std::wstring& left, const std::wstring& right) {
    const int result = CompareStringEx(
        L"zh-CN", NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
        left.c_str(), static_cast<int>(left.size()), right.c_str(),
        static_cast<int>(right.size()), nullptr, nullptr, 0);
    if (result == CSTR_LESS_THAN)
        return -1;
    if (result == CSTR_GREATER_THAN)
        return 1;
    return left.compare(right);
}

IdleTaskInfo makeTask(const json& task, const std::string& today) {
    IdleTaskInfo result;
    result.id = toWide(stringField(task, "id"));
    result.title = toWide(stringField(task, "title"));
    const std::string due = stringField(task, "dueDate");
    result.dueText = dueText(due, today);
    result.completed = intField(task, "status") == 2;
    result.priority = taskPriority(intField(task, "priority"));

    std::tm local{};
    bool hasTime = false;
    if (parseIsoLocal(due, local, hasTime) && hasTime && localDate(due) == today) {
        const std::time_t dueTime = std::mktime(&local);
        result.overdue = dueTime != static_cast<std::time_t>(-1) &&
                         dueTime < std::time(nullptr) && !result.completed;
    }
    return result;
}

TickTickTasksResult fetchTodayTasks(TickTickService service, const std::wstring& apiToken,
                                    std::atomic<bool>& shutdown) {
    static_cast<void>(service);
    TickTickTasksResult result;
    if (apiToken.empty()) {
        result.authRequired = true;
        result.error = L"请先填写滴答清单 API 口令";
        return result;
    }

    const std::string today = [] {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char value[16]{};
        sprintf_s(value, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday);
        return std::string(value);
    }();

    json projects;
    bool authRequired = false;
    if (!requestApiJson(apiToken, std::string(kDidaApiBaseUrl) + "/project", shutdown, projects,
                        authRequired) ||
        !projects.is_array()) {
        result.authRequired = authRequired;
        result.error = authRequired ? L"滴答清单 API 口令无效，请重新填写"
                                   : L"滴答清单任务同步失败";
        return result;
    }

    std::unordered_set<std::string> seenTaskIds;
    bool projectRequestFailed = false;
    for (const auto& project : projects) {
        if (shutdown.load())
            return result;
        if (!project.is_object() || boolField(project, "closed"))
            continue;
        const std::string kind = stringField(project, "kind");
        if (!kind.empty() && kind != "TASK")
            continue;
        const std::string projectId = stringField(project, "id");
        if (projectId.empty())
            continue;

        json projectData;
        authRequired = false;
        const std::string url = std::string(kDidaApiBaseUrl) + "/project/" +
                                percentEncode(projectId) + "/data";
        if (!requestApiJson(apiToken, url, shutdown, projectData, authRequired)) {
            if (authRequired) {
                result.authRequired = true;
                result.error = L"滴答清单 API 口令无效，请重新填写";
                return result;
            }
            projectRequestFailed = true;
            continue;
        }
        if (!projectData.is_object() || !projectData.contains("tasks") ||
            !projectData["tasks"].is_array())
            continue;
        for (const auto& task : projectData["tasks"]) {
            if (!task.is_object() || intField(task, "status") == 2 ||
                !taskIsForToday(task, today))
                continue;
            const std::string taskId = stringField(task, "id");
            if (taskId.empty() || !seenTaskIds.insert(taskId).second)
                continue;
            IdleTaskInfo item = makeTask(task, today);
            item.projectId = toWide(projectId);
            if (boolField(task, "isAllDay"))
                item.dueText = L"今天";
            if (!item.title.empty())
                result.tasks.push_back(std::move(item));
        }
    }

    std::stable_sort(result.tasks.begin(), result.tasks.end(), [](const auto& left,
                                                                  const auto& right) {
        if (left.priority != right.priority)
            return static_cast<int>(left.priority) > static_cast<int>(right.priority);
        const int titleOrder = compareTaskTitles(left.title, right.title);
        if (titleOrder != 0)
            return titleOrder < 0;
        if (left.dueText != right.dueText)
            return left.dueText < right.dueText;
        return left.id < right.id;
    });
    if (projectRequestFailed) {
        result.error = L"滴答清单任务同步失败";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace

struct TickTickProvider::Impl {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };

    std::atomic<bool> shutdown{false};
    std::mutex mutex;
    std::vector<Worker> workers;

    ~Impl() {
        shutdown.store(true);
        for (auto& worker : workers) {
            if (worker.thread.joinable())
                worker.thread.join();
        }
    }

    void sweepFinished() {
        std::erase_if(workers, [](Worker& worker) {
            if (!worker.done->load())
                return false;
            if (worker.thread.joinable())
                worker.thread.join();
            return true;
        });
    }
};

TickTickProvider::TickTickProvider() : impl_(std::make_unique<Impl>()) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

TickTickProvider::~TickTickProvider() = default;

void TickTickProvider::requestTodayTasksAsync(TickTickService service,
                                              const std::wstring& apiToken,
                                              TasksReadyCallback cb) {
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, service, apiToken, cb = std::move(cb), done]() mutable {
        struct DoneFlag {
            std::shared_ptr<std::atomic<bool>> value;
            ~DoneFlag() { value->store(true); }
        } doneFlag{done};

        TickTickTasksResult result;
        try {
            result = fetchTodayTasks(service, apiToken, impl->shutdown);
        } catch (...) {
            result.error = L"滴答清单任务同步失败";
        }
        runtime_log::writef(L"[ticktick] tasks result=%s count=%llu",
                            result.ok ? L"success" : L"failed",
                            static_cast<unsigned long long>(result.tasks.size()));
        if (!impl->shutdown.load() && cb)
            cb(std::move(result));
    });

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

bool TickTickProvider::loadApiToken(TickTickService service, std::wstring& apiToken) const {
    return readApiToken(service, apiToken);
}

bool TickTickProvider::saveApiToken(TickTickService service, const std::wstring& apiToken) {
    if (apiToken.empty()) {
        clearApiToken(service);
        return true;
    }
    return writeApiToken(service, apiToken);
}

bool TickTickProvider::hasApiToken(TickTickService service) const {
    std::wstring apiToken;
    return readApiToken(service, apiToken);
}

void TickTickProvider::clearApiToken(TickTickService service) {
    ::clearApiToken(service);
}
