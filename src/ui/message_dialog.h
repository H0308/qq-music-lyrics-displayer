#pragma once

#include <windows.h>

#include <string>

namespace message_dialog {

enum class Result {
    Primary,
    Secondary,
    Closed,
};

// 显示一个紧凑的 Fluent 模态提示框。
// secondaryLabel 为空时只显示 primaryLabel；关闭窗口返回 Secondary 或 Closed。
Result showModal(HINSTANCE inst, HWND owner, const std::wstring& title,
                 const std::wstring& message, const wchar_t* primaryLabel,
                 const wchar_t* secondaryLabel = nullptr,
                 bool focusPrimary = true);

} // namespace message_dialog
