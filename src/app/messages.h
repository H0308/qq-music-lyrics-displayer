#pragma once
#include <windows.h>
constexpr UINT kMsgSmtcChanged = WM_APP + 1;
constexpr UINT kMsgLyricReady = WM_APP + 2;
constexpr UINT kMsgCoverReady = WM_APP + 3;
constexpr UINT kMsgQqLocalFolderReady = WM_APP + 4;
// 应用音频会话的音量/静音/可用性变化（AppVolumeController 从 COM 回调线程投递）
constexpr UINT kMsgAppVolumeChanged = WM_APP + 5;
constexpr UINT kMsgIdleQuoteReady = WM_APP + 6;
constexpr UINT kMsgIdleAppReady = WM_APP + 7;
constexpr UINT kMsgHolidayReady = WM_APP + 8;
constexpr UINT kMsgTickTickTasksReady = WM_APP + 9;
constexpr UINT kMsgTickTickTaskCompleteReady = WM_APP + 10;
constexpr UINT kMsgTaskbarNoSpace = WM_APP + 11;
constexpr UINT kMsgQqCoverInfoReady = WM_APP + 12;
constexpr UINT kMsgTaskbarRebuild = WM_APP + 13;
constexpr UINT kMsgRequestQuit = WM_APP + 14;

constexpr UINT_PTR kTimerTickTickStartupRetry = 6;
