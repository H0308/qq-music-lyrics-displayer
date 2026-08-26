#pragma once

#include <windows.h>

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

struct IdleQuoteResult {
    bool ok = false;
    std::wstring content;
    std::wstring origin;
    std::wstring uuid;
    std::wstring token;
};

// 配置只持久化 path；name/icon 是启动或首次展示时读取的运行时资源。
struct IdleAppInfo {
    std::wstring path;
    std::wstring name;
    std::shared_ptr<const std::vector<BYTE>> iconPixels;
    UINT iconWidth = 0;
    UINT iconHeight = 0;
    bool pathValid = true;
};

struct IdlePresentation {
    std::wstring sentence;
    std::wstring source;
    bool loading = false;
    std::vector<IdleAppInfo> apps;
};
