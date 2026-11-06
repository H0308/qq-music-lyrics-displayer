#include "ui/taskbar/platform.h"
#include <shellapi.h>
#include <algorithm>
#include <cwchar>
#include <unordered_map>
#include <iterator>
#include <utility>
namespace taskbar_detail {

template <class T> T regDword(HKEY root, const wchar_t* path, const wchar_t* name, T defaultValue) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return defaultValue;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    LONG err = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (err != ERROR_SUCCESS || type != REG_DWORD)
        return defaultValue;
    return static_cast<T>(value);
}

bool isTaskbarCenterAlign() {
    // 0 = 左对齐，1 = 居中（默认）
    return regDword(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                    L"TaskbarAl", 1) != 0;
}

bool isValidTaskbarEdge(UINT edge) {
    return edge == ABE_LEFT || edge == ABE_TOP || edge == ABE_RIGHT || edge == ABE_BOTTOM;
}

UINT queryTaskbarEdge(HWND taskbar, const RECT& taskbarRect) {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &data) != 0 && isValidTaskbarEdge(data.uEdge) &&
        data.rc.left == taskbarRect.left && data.rc.top == taskbarRect.top &&
        data.rc.right == taskbarRect.right && data.rc.bottom == taskbarRect.bottom)
        return data.uEdge;

    // ABM_GETTASKBARPOS 是首选；旧版 Shell 或第三方任务栏替代实现不可用时，
    // 用任务栏矩形相对显示器边界的位置兜底，避免把侧边任务栏误判成横向。
    HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        const int width = taskbarRect.right - taskbarRect.left;
        const int height = taskbarRect.bottom - taskbarRect.top;
        if (height > width) {
            if (taskbarRect.left <= info.rcMonitor.left)
                return ABE_LEFT;
            if (taskbarRect.right >= info.rcMonitor.right)
                return ABE_RIGHT;
        } else {
            if (taskbarRect.top <= info.rcMonitor.top)
                return ABE_TOP;
            if (taskbarRect.bottom >= info.rcMonitor.bottom)
                return ABE_BOTTOM;
        }
    }
    return (taskbarRect.bottom - taskbarRect.top) > (taskbarRect.right - taskbarRect.left)
               ? ABE_LEFT
               : ABE_BOTTOM;
}

struct DisplayEnumeration {
    std::vector<TaskbarDisplayInfo>* displays = nullptr;
};

BOOL CALLBACK collectDisplayMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* enumeration = reinterpret_cast<DisplayEnumeration*>(parameter);
    if (!enumeration || !enumeration->displays)
        return FALSE;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;
    if (info.rcMonitor.right <= info.rcMonitor.left || info.rcMonitor.bottom <= info.rcMonitor.top)
        return TRUE;

    TaskbarDisplayInfo display;
    display.monitor = monitor;
    display.deviceName = info.szDevice;
    display.displayName = display.deviceName;
    display.monitorRect = info.rcMonitor;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    std::wstring activeMonitorLabel;
    std::wstring fallbackMonitorLabel;
    for (DWORD monitorIndex = 0;; ++monitorIndex) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(info.szDevice, monitorIndex, &device, 0))
            break;
        if (device.DeviceString[0] == L'\0')
            continue;
        if (fallbackMonitorLabel.empty())
            fallbackMonitorLabel = device.DeviceString;
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0)
            continue;
        if (activeMonitorLabel.empty()) {
            activeMonitorLabel = device.DeviceString;
        } else if (activeMonitorLabel.find(device.DeviceString) == std::wstring::npos) {
            activeMonitorLabel += L" / ";
            activeMonitorLabel += device.DeviceString;
        }
    }
    if (!activeMonitorLabel.empty())
        display.displayName = std::move(activeMonitorLabel);
    else if (!fallbackMonitorLabel.empty())
        display.displayName = std::move(fallbackMonitorLabel);

    enumeration->displays->push_back(std::move(display));
    return TRUE;
}

struct TaskbarWindowEnumeration {
    std::vector<TaskbarDisplayInfo>* displays = nullptr;
    DWORD shellProcessId = 0;
};

BOOL CALLBACK findTaskbarForDisplay(HWND window, LPARAM parameter) {
    auto* enumeration = reinterpret_cast<TaskbarWindowEnumeration*>(parameter);
    if (!enumeration || !enumeration->displays)
        return FALSE;

    wchar_t className[128]{};
    const int length = GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (length <= 0 || (wcscmp(className, L"Shell_TrayWnd") != 0 &&
                        wcscmp(className, L"Shell_SecondaryTrayWnd") != 0))
        return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (enumeration->shellProcessId != 0 && processId != enumeration->shellProcessId)
        return TRUE;

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    auto it = std::find_if(
        enumeration->displays->begin(), enumeration->displays->end(),
        [monitor](const TaskbarDisplayInfo& display) { return display.monitor == monitor; });
    if (it == enumeration->displays->end())
        return TRUE;

    it->taskbarWindow = window;
    RECT taskbarRect{};
    if (GetWindowRect(window, &taskbarRect)) {
        const UINT edge = queryTaskbarEdge(window, taskbarRect);
        it->horizontalTaskbar = edge == ABE_TOP || edge == ABE_BOTTOM;
    }
    return TRUE;
}

bool isVerticalTaskbarEdge(UINT edge) { return edge == ABE_LEFT || edge == ABE_RIGHT; }

int taskbarCrossPixels(const RECT& rect, UINT edge) {
    return isVerticalTaskbarEdge(edge) ? rect.right - rect.left : rect.bottom - rect.top;
}

// 避让探测统一在工作线程执行；其中 UIA 会阻塞等待 Explorer。
// UI 线程不能调用 UIA，也不能同步等待探测线程退出，否则 Explorer 同时
// 等待任务栏子窗口处理消息时会形成循环等待（WER: AppHangXProcB1）。

HWND findTrafficMonitorWnd(HWND taskbar) {
    if (!taskbar)
        return nullptr;
    HWND child = nullptr;
    while ((child = FindWindowExW(taskbar, child, nullptr, nullptr)) != nullptr) {
        wchar_t text[256] = {};
        GetWindowTextW(child, text, 256);
        if (wcscmp(text, L"TrafficMonitorTaskbarWindow") == 0)
            return child;
    }
    return nullptr;
}

// 酷狗官方任务栏歌词可能是酷狗进程创建的任务栏子窗口，也可能是叠在任务栏上的独立窗口。
// 不能依赖固定窗口类名：不同版本的酷狗客户端可能使用不同的 UI 框架和类名；这里按进程
// 镜像名识别，再只保留与当前任务栏相交的可见窗口。所有调用都在探测工作线程执行。
struct KugouWindowProbeContext {
    RECT taskbarRect{};
    std::vector<RECT>* out = nullptr;
    std::unordered_map<DWORD, bool> processCache;
};

bool isKugouProcess(DWORD processId, std::unordered_map<DWORD, bool>& cache) {
    const auto cached = cache.find(processId);
    if (cached != cache.end())
        return cached->second;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        cache.emplace(processId, false);
        return false;
    }

    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!queried) {
        cache.emplace(processId, false);
        return false;
    }

    const wchar_t* fileName = wcsrchr(path, L'\\');
    fileName = fileName ? fileName + 1 : path;
    const bool match = _wcsicmp(fileName, L"KuGou.exe") == 0;
    cache.emplace(processId, match);
    return match;
}

void collectKugouTaskbarWindow(HWND window, KugouWindowProbeContext& context) {
    if (!window || !IsWindowVisible(window))
        return;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!isKugouProcess(processId, context.processCache))
        return;

    RECT windowRect{};
    RECT intersection{};
    if (GetWindowRect(window, &windowRect) &&
        IntersectRect(&intersection, &windowRect, &context.taskbarRect))
        context.out->push_back(intersection);
}

BOOL CALLBACK collectKugouTopLevelWindow(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<KugouWindowProbeContext*>(parameter);
    if (context)
        collectKugouTaskbarWindow(window, *context);
    return TRUE;
}

BOOL CALLBACK collectKugouTaskbarChildWindow(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<KugouWindowProbeContext*>(parameter);
    if (context)
        collectKugouTaskbarWindow(window, *context);
    return TRUE;
}

void queryKugouTaskbarWindows(HWND taskbar, std::vector<RECT>& out) {
    out.clear();
    if (!taskbar)
        return;

    KugouWindowProbeContext context;
    if (!GetWindowRect(taskbar, &context.taskbarRect))
        return;
    context.out = &out;

    // 顶层枚举覆盖独立置顶/覆盖窗口；任务栏子树枚举覆盖被 Explorer 承载的嵌入窗口。
    EnumWindows(collectKugouTopLevelWindow, reinterpret_cast<LPARAM>(&context));
    EnumChildWindows(taskbar, collectKugouTaskbarChildWindow, reinterpret_cast<LPARAM>(&context));

    std::sort(out.begin(), out.end(), [](const RECT& a, const RECT& b) {
        if (a.left != b.left)
            return a.left < b.left;
        if (a.top != b.top)
            return a.top < b.top;
        if (a.right != b.right)
            return a.right < b.right;
        return a.bottom < b.bottom;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }),
              out.end());
}

// 通过 UI Automation 取任务栏 XAML 部件（开始/搜索/任务视图/小组件/固定与运行中的
// 应用图标）的屏幕包围矩形。这些按钮是 XAML 元素而非窗口，HWND 枚举看不到，
// 但 UIA 的 TaskbarFrame 子树完整暴露；小组件等后续新增的按钮同样作为其子元素出现。
// uia 实例由探测工作线程创建与持有
void queryTaskbarButtonsUia(IUIAutomation* uia, HWND taskbar, std::vector<RECT>& out) {
    out.clear();
    if (!taskbar || !uia)
        return;
    IUIAutomationElement* tb = nullptr;
    if (FAILED(uia->ElementFromHandle(taskbar, &tb)) || !tb)
        return;
    VARIANT aid{};
    aid.vt = VT_BSTR;
    aid.bstrVal = SysAllocString(L"TaskbarFrame");
    IUIAutomationCondition* cond = nullptr;
    uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, aid, &cond);
    VariantClear(&aid);
    IUIAutomationElement* frame = nullptr;
    if (cond) {
        tb->FindFirst(TreeScope_Descendants, cond, &frame);
        cond->Release();
    }
    tb->Release();
    if (!frame)
        return;
    IUIAutomationCondition* trueCond = nullptr;
    uia->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray* arr = nullptr;
    if (trueCond) {
        frame->FindAll(TreeScope_Children, trueCond, &arr);
        trueCond->Release();
    }
    frame->Release();
    if (!arr)
        return;
    int n = 0;
    arr->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* el = nullptr;
        arr->GetElement(i, &el);
        if (!el)
            continue;
        BOOL offscreen = FALSE;
        RECT rc{};
        el->get_CurrentIsOffscreen(&offscreen);
        el->get_CurrentBoundingRectangle(&rc);
        el->Release();
        if (!offscreen && rc.right > rc.left && rc.bottom > rc.top)
            out.push_back(rc);
    }
    arr->Release();
}

std::vector<TaskbarDisplayInfo> enumerateDisplays() {
    std::vector<TaskbarDisplayInfo> displays;
    DisplayEnumeration displayEnumeration{&displays};
    EnumDisplayMonitors(nullptr, nullptr, collectDisplayMonitor,
                        reinterpret_cast<LPARAM>(&displayEnumeration));

    DWORD shellProcessId = 0;
    if (const HWND primaryTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
        GetWindowThreadProcessId(primaryTaskbar, &shellProcessId);
    TaskbarWindowEnumeration taskbarEnumeration{&displays, shellProcessId};
    EnumWindows(findTaskbarForDisplay, reinterpret_cast<LPARAM>(&taskbarEnumeration));

    std::stable_sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
        return left.primary && !right.primary;
    });
    return displays;
}

} // namespace taskbar_detail

std::vector<TaskbarDisplayInfo> enumerateTaskbarDisplays() {
    return taskbar_detail::enumerateDisplays();
}
