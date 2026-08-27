#pragma once

#include "idle_types.h"

#include <functional>
#include <memory>
#include <string>

class IdleQuoteProvider {
public:
    using ReadyCallback = std::function<void(IdleQuoteResult)>;
    using HolidayReadyCallback = std::function<void(HolidayCalendarResult)>;

    IdleQuoteProvider();
    ~IdleQuoteProvider();

    IdleQuoteProvider(const IdleQuoteProvider&) = delete;
    IdleQuoteProvider& operator=(const IdleQuoteProvider&) = delete;

    void requestAsync(IdleQuoteSource source, const std::wstring& token, ReadyCallback cb);
    void requestHolidayYearAsync(int year, HolidayReadyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
