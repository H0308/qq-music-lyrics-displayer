#include "idle_quote_provider.h"

#include "logging/runtime_logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

std::string toUtf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
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

bool getJson(const std::string& url, const std::string& token, std::atomic<bool>& shutdown,
             json& result) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    std::string response;
    curl_slist* headers = nullptr;
    if (!token.empty())
        headers = curl_slist_append(headers, ("X-User-Token: " + token).c_str());

    CurlRequestContext context{&shutdown};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferAbort);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);

    const CURLcode curlResult = curl_easy_perform(curl);
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curlResult != CURLE_OK || statusCode != 200 || response.empty())
        return false;
    result = json::parse(response, nullptr, false);
    return !result.is_discarded() && result.is_object();
}

std::wstring jrsOrigin(const json& data) {
    if (!data.contains("origin"))
        return {};
    if (data["origin"].is_string())
        return toWide(data["origin"].get<std::string>());
    if (!data["origin"].is_object())
        return {};

    std::wstring result;
    const auto appendPart = [&result](const std::string& value) {
        if (value.empty())
            return;
        if (!result.empty())
            result += L" · ";
        result += toWide(value);
    };
    appendPart(data["origin"].value("title", std::string()));
    appendPart(data["origin"].value("dynasty", std::string()));
    appendPart(data["origin"].value("author", std::string()));
    return result;
}

IdleQuoteResult requestQuote(IdleQuoteSource source, const std::wstring& token,
                             std::atomic<bool>& shutdown) {
    IdleQuoteResult result;
    if (source == IdleQuoteSource::Hitokoto) {
        json data;
        if (!getJson("https://v1.hitokoto.cn/?encode=json", {}, shutdown, data))
            return result;
        const std::string content = data.value("hitokoto", std::string());
        if (content.empty())
            return result;
        result.ok = true;
        result.content = toWide(content);
        result.origin = toWide(data.value("from", std::string()));
        result.uuid = toWide(data.value("uuid", std::string()));
        return result;
    }

    std::string tokenUtf8 = toUtf8(token);
    if (tokenUtf8.empty()) {
        json tokenData;
        if (!getJson("https://v2.jinrishici.com/token", {}, shutdown, tokenData))
            return result;
        tokenUtf8 = tokenData.value("data", std::string());
        if (tokenUtf8.empty())
            return result;
    }

    json data;
    if (!getJson("https://v2.jinrishici.com/sentence", tokenUtf8, shutdown, data)) {
        result.token = toWide(tokenUtf8);
        return result;
    }
    if (data.value("status", std::string()) != "success" || !data.contains("data") ||
        !data["data"].is_object()) {
        result.token = toWide(tokenUtf8);
        return result;
    }
    const json& sentence = data["data"];
    const std::string content = sentence.value("content", std::string());
    result.token = toWide(tokenUtf8);
    if (content.empty())
        return result;
    result.ok = true;
    result.content = toWide(content);
    result.origin = jrsOrigin(sentence);
    return result;
}

std::string fullDateForYear(int year, const std::string& value) {
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-')
        return value.substr(0, 10);

    int month = 0;
    int day = 0;
    if (sscanf_s(value.c_str(), "%d-%d", &month, &day) != 2 || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return {};
    char date[16]{};
    sprintf_s(date, "%04d-%02d-%02d", year, month, day);
    return date;
}

std::string shortDateOf(const std::string& fullDate) {
    return fullDate.size() >= 10 ? fullDate.substr(5, 5) : fullDate;
}

HolidayCalendarResult requestHolidayYear(int year, std::atomic<bool>& shutdown) {
    HolidayCalendarResult result;
    result.year = year;
    if (year < 2000 || year > 3000)
        return result;

    json data;
    const std::string url = "https://timor.tech/api/holiday/year/" + std::to_string(year) +
                            "/?type=Y&week=Y";
    if (!getJson(url, {}, shutdown, data) || data.value("code", -1) != 0 ||
        !data.contains("type") || !data["type"].is_object())
        return result;

    const json* holidays = nullptr;
    if (data.contains("holiday") && data["holiday"].is_object())
        holidays = &data["holiday"];

    for (const auto& item : data["type"].items()) {
        if (!item.value().is_object())
            continue;
        const std::string date = fullDateForYear(year, item.key());
        const int type = item.value().value("type", -1);
        if (date.empty() || type < 0 || type > 3)
            continue;

        std::wstring name = toWide(item.value().value("name", std::string()));
        if (holidays) {
            const std::string shortDate = shortDateOf(date);
            auto holiday = holidays->find(item.key());
            if (holiday == holidays->end())
                holiday = holidays->find(shortDate);
            if (holiday != holidays->end() && holiday->is_object()) {
                const std::string holidayName = holiday->value("name", std::string());
                if (!holidayName.empty())
                    name = toWide(holidayName);
            }
        }
        result.days.push_back({date, type, std::move(name)});
    }

    std::sort(result.days.begin(), result.days.end(),
              [](const HolidayDayInfo& left, const HolidayDayInfo& right) {
                  return left.date < right.date;
              });
    result.ok = !result.days.empty();
    return result;
}

} // namespace

struct IdleQuoteProvider::Impl {
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

IdleQuoteProvider::IdleQuoteProvider() : impl_(std::make_unique<Impl>()) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

IdleQuoteProvider::~IdleQuoteProvider() = default;

void IdleQuoteProvider::requestAsync(IdleQuoteSource source, const std::wstring& token,
                                     ReadyCallback cb) {
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, source, token, cb = std::move(cb), done]() mutable {
        struct DoneFlag {
            std::shared_ptr<std::atomic<bool>> value;
            ~DoneFlag() { value->store(true); }
        } doneFlag{done};

        IdleQuoteResult result = requestQuote(source, token, impl->shutdown);
        runtime_log::writef(L"[idle-quote] source=%s result=%s",
                            source == IdleQuoteSource::Hitokoto ? L"hitokoto" : L"jinrishici",
                            result.ok ? L"success" : L"failed");
        if (!impl->shutdown.load() && cb)
            cb(std::move(result));
    });

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void IdleQuoteProvider::requestHolidayYearAsync(int year, HolidayReadyCallback cb) {
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, year, cb = std::move(cb), done]() mutable {
        struct DoneFlag {
            std::shared_ptr<std::atomic<bool>> value;
            ~DoneFlag() { value->store(true); }
        } doneFlag{done};

        HolidayCalendarResult result = requestHolidayYear(year, impl->shutdown);
        runtime_log::writef(L"[holiday] year=%d result=%s", year,
                            result.ok ? L"success" : L"failed");
        if (!impl->shutdown.load() && cb)
            cb(std::move(result));
    });

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}
