#pragma once
#include "media/smtc_monitor.h"
#include <string>

// 曲目身份判断供请求校验、手动搜索与展示链路共用。
std::wstring makeTrackKey(const SmtcSnapshot& snap);
bool snapshotMatchesTrackKey(const SmtcSnapshot& snap, const std::wstring& key);
