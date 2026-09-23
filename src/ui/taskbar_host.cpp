#include "taskbar_host.h"
#include "logging/runtime_logger.h"
#include "ui/app_icon.h"
#include "fluent_theme.h"
#include "dock_pet.h"
#include "lyric_renderer.h"
#include "media_control_icons.h"
#include "media_popup.h"
#include "monitor/resource_monitor.h"
#include "dock_backdrop.h"
#include "platform_icon.h"
#include "settings_icons.h"
#include "volume_popup.h"

#include <d2d1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId = 2;
constexpr UINT_PTR kTaskbarAttachTimerId = 3;
constexpr UINT kTimerMs = 16;         // 活动帧最大间隔：60Hz 基准
constexpr UINT kTimerMinMs = 8;       // 活动帧最小间隔：高刷封顶 ~125Hz，避免过度唤醒
constexpr UINT kTimerPausedMs = 33;   // 暂停时 ~30fps：超长文本继续滚动，任务栏合成开销减半
constexpr UINT kTaskbarAttachRetryMs = 250;
constexpr UINT_PTR kPlacementTimerId = 4;
constexpr UINT kAppBarCallbackMessage = WM_APP + 40;
constexpr UINT kPlacementTimerMs = 500; // 隐藏时只拾取避让结果，不运行完整渲染帧
constexpr UINT kProbeIntervalMs = 3000;
constexpr UINT kNoSpaceProbeIntervalMs = 1000;
constexpr int kSlowTickInterval = 15; // 慢速分支（任务栏位置跟踪等）每 15 帧一次，约 250ms
constexpr wchar_t kWndClassName[] = L"QQMusicLyricTaskbar";

constexpr float kMinWidthDip = 160.0f;
constexpr float kMaxWidthDip = 280.0f;
constexpr float kCompressedMinWidthRatio = 0.5f;
constexpr float kLeftRatio = 0.38f;
constexpr float kCoverPadding = 4.0f;
constexpr float kTextPadding = 8.0f;
constexpr wchar_t kDragPreviewText[] = L"松手固定到这里";
constexpr float kLyricDragThresholdDip = 6.0f;
// 滚动文本左缘（滚出侧）的渐隐宽度
constexpr float kLyricEdgeFadeDip = 18.0f;
constexpr float kSongInfoLyricGap = 8.0f; // 歌曲信息与歌词之间的分隔间距
constexpr float kSongInfoDividerWidth = 1.0f;
constexpr float kSongInfoDividerInset = 5.0f;
constexpr float kCornerRadius = 8.0f;
constexpr float kVerticalMinLengthDip = 220.0f;
constexpr float kVerticalMaxLengthDip = 320.0f;
constexpr float kVerticalRailPadding = 4.0f;
constexpr float kVerticalCoverGap = 6.0f;
constexpr float kVerticalControlsGap = 4.0f;
constexpr float kVerticalLyricGap = 6.0f;
constexpr float kInfoScrollSpeed = 10.0f;  // 歌名/歌手滚动速度（DIP/s）
constexpr float kLyricScrollSpeed = 15.0f; // 歌词滚动速度（DIP/s）
constexpr ULONGLONG kOneShotStatusTextHoldMs = 5000; // 不足以滚动时保留完整文案的时长
constexpr int kOneShotStatusTextRounds = 3; // 启动任务概览超长文案完整滚动三轮后结束
constexpr float kLyricTransitionMs = 280.0f; // 相邻歌词上下切换时长
constexpr float kSceneTransitionMs = 240.0f; // 每日一言与歌词内容块上下翻页时长
constexpr float kSongTransitionMs = 220.0f; // 切歌时新内容滑入时长
constexpr float kSongTransitionTravelDip = 24.0f; // 切歌时新内容的水平入场距离
constexpr float kLyricPreviewGap = 3.0f; // 普通双行模式的核心行与下一行间距
constexpr float kLyricPreviewOpacity = 0.90f; // 下一行预览透明度
constexpr float kKaraokeScrollFollowMs = 100.0f; // 逐字歌词横向跟随时间常数
constexpr int kImmersiveControlPrevious = 0;
constexpr int kImmersiveControlPlayPause = 1;
constexpr int kImmersiveControlNext = 2;
constexpr int kImmersiveControlVolume = 3;
constexpr int kImmersiveControlExit = 4;
// Dock 的快捷应用入口使用独立图标和命中项；不能复用沉浸模式的应用收纳入口。
constexpr int kDockControlQuickApps = 5;
constexpr int kImmersiveControlApps = 6;
constexpr int kImmersiveControlMenu = 7;
constexpr int kImmersiveControlTray = 8;
constexpr int kImmersiveControlCount = 8; // 沉浸模式显示 8 个，索引 5 保留给 Dock 入口
constexpr int kExpandedControlCount = 9;
constexpr int kAppBarControlCount = 7;
constexpr float kAppBarHeightDip = 48.0f;
constexpr float kImmersiveControlRadiusFactor = 0.24f;
constexpr float kImmersiveControlMinRadius = 8.0f;
constexpr float kImmersiveControlMaxRadius = 12.0f;
constexpr float kImmersiveControlPitchFactor = 3.0f;
constexpr float kLyricMainFontScale = 1.18f;
constexpr float kLyricPreviewFontScale = 0.86f;
constexpr float kLyricPreviewScale = kLyricPreviewFontScale / kLyricMainFontScale;
constexpr float kMinFont = 9.0f;
constexpr float kMaxFont = 18.0f;
constexpr float kBaseFontSize = 12.0f;
constexpr float kSpectrumBarW = 5.0f;  // 频谱柱宽
constexpr float kSpectrumGap = 3.0f;   // 频谱柱间隙
constexpr float kSpectrumBottomPadding = 1.0f;
constexpr float kSpectrumBarRadius = 2.0f; // 轻微圆角，保持柱状感
constexpr int kImmersiveSpectrumBarCount = 24;
constexpr int kImmersiveSpectrumWideBarCount = 32;
constexpr float kImmersiveSpectrumZoneRatio = 0.22f;
constexpr float kImmersiveSpectrumZoneMinW = 220.0f;
constexpr float kImmersiveSpectrumZoneMaxW = 440.0f;
constexpr float kImmersiveSpectrumWideZoneThreshold = 340.0f;
constexpr float kImmersiveClockZoneRatio = 0.045f;
constexpr float kImmersiveClockZoneMinW = 80.0f;
constexpr float kImmersiveClockZoneMaxW = 96.0f;
constexpr float kImmersiveSpectrumClockGap = 10.0f;
constexpr float kDockResourceZoneRatio = 0.12f;
constexpr float kDockResourceZoneMinW = 172.0f;
constexpr float kDockResourceZoneMaxW = 180.0f;
constexpr float kDockResourceZoneGap = 10.0f;
constexpr float kDockResourceMetricColumnW = 72.0f;
constexpr float kDockResourceRightPadding = 3.0f;
constexpr float kDockPetSeatW = 42.0f;
constexpr float kPetLaneMinWidthDip = 44.0f;
constexpr UINT kResourceSnapshotPollMs = 250;
constexpr float kSpectrumBarGradientDarkFactor = 0.78f;
constexpr float kSpectrumBarGradientLightMix = 0.22f;
constexpr float kVinylRotationDegPerSecond = 30.0f; // 黑胶唱片转速：12 秒一圈，保持视觉克制
constexpr float kVinylHaloWidth = 2.5f;
constexpr float kVinylInnerRatio = 0.30f; // 圆形专辑封面半径 / 封面槽边长
constexpr float kCoverBlurStdDev = 6.0f;  // 封面模糊背景的高斯模糊强度（封面按显示尺寸解码，拉伸后等效更强）

// d2d1effects.h 只声明这些 GUID；当前工程的链接配置不提供其外部定义，
// 这里保留 Direct2D 标准 Gaussian Blur / Scale CLSID 的内部定义（与 media_popup.cpp 一致）。
constexpr CLSID kGaussianBlurClsid = {
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
constexpr CLSID kScaleClsid = {
    0x9daf9369, 0x3846, 0x4d0e, {0xa4, 0x4e, 0x0c, 0x60, 0x79, 0x34, 0xa5, 0xd7}};

ULONGLONG monotonicNowMs() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const LONGLONG wholeMs = counter.QuadPart / frequency.QuadPart * 1000;
    const LONGLONG remainderMs =
        (counter.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart;
    return static_cast<ULONGLONG>(wholeMs + remainderMs);
}

// GDI+ 一次性初始化（封面解码）
class GdiplusInit {
public:
    GdiplusInit() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartupOutput output;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, &output);
        token_ = token;
    }
    ~GdiplusInit() {
        if (token_)
            Gdiplus::GdiplusShutdown(token_);
    }

private:
    ULONG_PTR token_ = 0;
};
GdiplusInit g_gdiplusInit;

// 读取注册表 DWORD，失败返回默认值
template <class T>
T regDword(HKEY root, const wchar_t* path, const wchar_t* name, T defaultValue) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return defaultValue;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    LONG err = RegQueryValueExW(key, name, nullptr, &type,
                                reinterpret_cast<LPBYTE>(&value), &size);
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
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &data) != 0 && isValidTaskbarEdge(data.uEdge))
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

bool isVerticalTaskbarEdge(UINT edge) {
    return edge == ABE_LEFT || edge == ABE_RIGHT;
}

int taskbarCrossPixels(const RECT& rect, UINT edge) {
    return isVerticalTaskbarEdge(edge) ? rect.right - rect.left : rect.bottom - rect.top;
}

// 以下两个函数都是【阻塞型跨进程调用】（GetWindowText = 同步 SendMessage，UIA = 阻塞 COM），
// 只允许在探测工作线程上调用。若放在 UI 线程，explorer 同时向我们的任务栏子窗口
// 发消息时会形成双向死等（WER: AppHangXProcB1，explorer 挂起、对方是本进程）

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
    EnumChildWindows(taskbar, collectKugouTaskbarChildWindow,
                     reinterpret_cast<LPARAM>(&context));

    std::sort(out.begin(), out.end(), [](const RECT& a, const RECT& b) {
        if (a.left != b.left)
            return a.left < b.left;
        if (a.top != b.top)
            return a.top < b.top;
        if (a.right != b.right)
            return a.right < b.right;
        return a.bottom < b.bottom;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const RECT& a, const RECT& b) {
                  return EqualRect(&a, &b) != FALSE;
              }),
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

bool containsCaseInsensitive(const std::wstring& value, const wchar_t* token) {
    if (value.empty() || !token || !*token)
        return false;

    std::wstring normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    });
    std::wstring normalizedToken = token;
    std::transform(normalizedToken.begin(), normalizedToken.end(), normalizedToken.begin(),
                   [](wchar_t ch) {
                       return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
                   });
    return normalized.find(normalizedToken) != std::wstring::npos;
}

bool isTaskbarTrayOverflowElement(IUIAutomationElement* element) {
    if (!element)
        return false;

    BSTR automationIdValue = nullptr;
    BSTR nameValue = nullptr;
    CONTROLTYPEID controlType = 0;
    element->get_CurrentAutomationId(&automationIdValue);
    element->get_CurrentName(&nameValue);
    element->get_CurrentControlType(&controlType);
    const std::wstring automationId = automationIdValue ? automationIdValue : L"";
    const std::wstring name = nameValue ? nameValue : L"";
    SysFreeString(automationIdValue);
    SysFreeString(nameValue);

    const bool byAutomationId = containsCaseInsensitive(automationId, L"OverflowTrayIconsList") ||
                                containsCaseInsensitive(automationId, L"overflow");
    const bool isButton = controlType == UIA_ButtonControlTypeId;
    const bool byName = isButton &&
                        (containsCaseInsensitive(name, L"show hidden icon") ||
                         containsCaseInsensitive(name, L"hidden icon") ||
                         containsCaseInsensitive(name, L"overflow") ||
                         containsCaseInsensitive(name, L"隐藏图标") ||
                         containsCaseInsensitive(name, L"溢出"));
    if (!byAutomationId && !byName)
        return false;

    BOOL offscreen = FALSE;
    if (SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && offscreen)
        return false;
    BOOL enabled = TRUE;
    if (SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && !enabled)
        return false;
    RECT bounds{};
    if (SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds)) &&
        (bounds.right <= bounds.left || bounds.bottom <= bounds.top))
        return false;
    return true;
}

IUIAutomationElement* findTaskbarTrayOverflowElement(IUIAutomation* uia, HWND taskbar) {
    if (!uia || !taskbar)
        return nullptr;

    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(taskbar, &root)) || !root)
        return nullptr;

    IUIAutomationCondition* condition = nullptr;
    uia->CreateTrueCondition(&condition);
    IUIAutomationElementArray* elements = nullptr;
    if (condition) {
        root->FindAll(TreeScope_Descendants, condition, &elements);
        condition->Release();
    }
    root->Release();
    if (!elements)
        return nullptr;

    IUIAutomationElement* result = nullptr;
    int count = 0;
    elements->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement* element = nullptr;
        elements->GetElement(i, &element);
        if (!element)
            continue;
        if (isTaskbarTrayOverflowElement(element)) {
            result = element;
            break;
        }
        element->Release();
    }
    elements->Release();
    return result;
}

bool invokeTaskbarTrayOverflowElement(IUIAutomationElement* element) {
    if (!element)
        return false;

    IUnknown* unknown = nullptr;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_InvokePatternId, &unknown)) && unknown) {
        IUIAutomationInvokePattern* pattern = nullptr;
        const HRESULT query = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
        unknown->Release();
        if (SUCCEEDED(query) && pattern) {
            const HRESULT result = pattern->Invoke();
            pattern->Release();
            if (SUCCEEDED(result))
                return true;
        }
    }

    unknown = nullptr;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_TogglePatternId, &unknown)) && unknown) {
        IUIAutomationTogglePattern* pattern = nullptr;
        const HRESULT query = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
        unknown->Release();
        if (SUCCEEDED(query) && pattern) {
            const HRESULT result = pattern->Toggle();
            pattern->Release();
            if (SUCCEEDED(result))
                return true;
        }
    }

    unknown = nullptr;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_ExpandCollapsePatternId, &unknown)) &&
        unknown) {
        IUIAutomationExpandCollapsePattern* pattern = nullptr;
        const HRESULT query = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
        unknown->Release();
        if (SUCCEEDED(query) && pattern) {
            const HRESULT result = pattern->Expand();
            pattern->Release();
            if (SUCCEEDED(result))
                return true;
        }
    }

    unknown = nullptr;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_LegacyIAccessiblePatternId, &unknown)) &&
        unknown) {
        IUIAutomationLegacyIAccessiblePattern* pattern = nullptr;
        const HRESULT query = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
        unknown->Release();
        if (SUCCEEDED(query) && pattern) {
            const HRESULT result = pattern->DoDefaultAction();
            pattern->Release();
            if (SUCCEEDED(result))
                return true;
        }
    }
    return false;
}

struct TrayOverflowWindowSearch {
    DWORD explorerProcessId = 0;
    HWND result = nullptr;
};

BOOL CALLBACK findTrayOverflowWindowProc(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<TrayOverflowWindowSearch*>(parameter);
    if (!search || !IsWindowVisible(window))
        return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->explorerProcessId)
        return TRUE;

    wchar_t className[128] = {};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (containsCaseInsensitive(className, L"overflow"))
        search->result = window;
    return search->result ? FALSE : TRUE;
}

HWND findTrayOverflowWindow(HWND taskbar) {
    if (!taskbar)
        return nullptr;

    TrayOverflowWindowSearch search;
    GetWindowThreadProcessId(taskbar, &search.explorerProcessId);
    if (!search.explorerProcessId)
        return nullptr;
    EnumWindows(findTrayOverflowWindowProc, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

void positionTrayOverflowWindow(HWND overflow, HWND taskbar, POINT anchor, UINT taskbarEdge) {
    if (!overflow || !taskbar)
        return;

    RECT panel{};
    RECT taskbarRect{};
    if (!GetWindowRect(overflow, &panel) || !GetWindowRect(taskbar, &taskbarRect))
        return;
    const int width = panel.right - panel.left;
    const int height = panel.bottom - panel.top;
    if (width <= 0 || height <= 0)
        return;

    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(taskbar));
    const int gap = std::max(4, MulDiv(6, static_cast<int>(dpi), 96));
    int x = panel.left;
    int y = panel.top;
    switch (taskbarEdge) {
    case ABE_TOP:
        x = anchor.x - width / 2;
        y = taskbarRect.bottom + gap;
        break;
    case ABE_LEFT:
        x = taskbarRect.right + gap;
        y = anchor.y - height / 2;
        break;
    case ABE_RIGHT:
        x = taskbarRect.left - width - gap;
        y = anchor.y - height / 2;
        break;
    case ABE_BOTTOM:
    default:
        x = anchor.x - width / 2;
        y = taskbarRect.top - height - gap;
        break;
    }

    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        const RECT& work = monitorInfo.rcWork;
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);
        x = std::clamp(x, workLeft, std::max(workLeft, workRight - width));
        y = std::clamp(y, workTop, std::max(workTop, workBottom - height));
    }

    SetWindowPos(overflow, nullptr, x, y, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE | SWP_SHOWWINDOW);
}

bool invokeTaskbarTrayOverflow(HWND taskbar, POINT anchor, UINT taskbarEdge) {
    if (!taskbar || !IsWindow(taskbar))
        return false;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return false;

    bool invoked = false;
    IUIAutomation* uia = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&uia)))) {
        IUIAutomationElement* element = findTaskbarTrayOverflowElement(uia, taskbar);
        if (element) {
            invoked = invokeTaskbarTrayOverflowElement(element);
            element->Release();
        }
        uia->Release();
    }

    if (invoked) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (HWND overflow = findTrayOverflowWindow(taskbar)) {
                positionTrayOverflowWindow(overflow, taskbar, anchor, taskbarEdge);
                break;
            }
            Sleep(10);
        }
    }

    if (SUCCEEDED(init))
        CoUninitialize();
    return invoked;
}

bool sameLyricLine(const LyricLine& a, const LyricLine& b) {
    if (a.ms != b.ms || a.text != b.text || a.translation != b.translation ||
        a.romanization != b.romanization || a.chars.size() != b.chars.size())
        return false;
    for (size_t i = 0; i < a.chars.size(); ++i) {
        const LyricChar& ac = a.chars[i];
        const LyricChar& bc = b.chars[i];
        if (ac.startMs != bc.startMs || ac.endMs != bc.endMs || ac.text != bc.text)
            return false;
    }
    return true;
}

bool sameLyrics(const std::vector<LyricLine>& a, const std::vector<LyricLine>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!sameLyricLine(a[i], b[i]))
            return false;
    }
    return true;
}

// 这些位表示“需要重新处理什么”，不再分别由多个布尔量表达。它们不是
// 生命周期阶段：阶段使用下面 RenderState 中的枚举表示，失效位只描述待处理工作。
enum class RenderInvalidation : uint32_t {
    Paint = 1u << 0,
    Text = 1u << 1,
    SongInfo = 1u << 2,
    Geometry = 1u << 3,
    Layout = 1u << 4,
    Cover = 1u << 5,
    PlatformIcon = 1u << 6,
};

using RenderInvalidationMask = uint32_t;

constexpr RenderInvalidationMask toMask(RenderInvalidation value) {
    return static_cast<RenderInvalidationMask>(value);
}

} // namespace

struct TaskbarHost::Impl {
    struct WindowPlacement {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int availableMajor = 0;
    };

    struct InvalidationSnapshot {
        RenderInvalidationMask mask = 0;
        std::array<uint64_t, 7> generations{};
    };

    class InvalidationState {
    public:
        InvalidationState()
            : pending_(toMask(RenderInvalidation::Paint) |
                       toMask(RenderInvalidation::Text) |
                       toMask(RenderInvalidation::SongInfo) |
                       toMask(RenderInvalidation::Geometry) |
                       toMask(RenderInvalidation::Layout) |
                       toMask(RenderInvalidation::Cover) |
                       toMask(RenderInvalidation::PlatformIcon)) {}

        void request(RenderInvalidation value) {
            request(toMask(value));
        }

        void request(RenderInvalidationMask mask) {
            pending_ |= mask;
            for (size_t i = 0; i < generations_.size(); ++i) {
                if (mask & (1u << i))
                    ++generations_[i];
            }
        }

        bool contains(RenderInvalidation value) const {
            return (pending_ & toMask(value)) != 0;
        }

        bool any() const { return pending_ != 0; }

        InvalidationSnapshot begin() const {
            return InvalidationSnapshot{pending_, generations_};
        }

        void commit(const InvalidationSnapshot& snapshot) {
            for (size_t i = 0; i < generations_.size(); ++i) {
                const RenderInvalidationMask bit = 1u << i;
                if ((snapshot.mask & bit) && generations_[i] == snapshot.generations[i])
                    pending_ &= ~bit;
            }
        }

    private:
        RenderInvalidationMask pending_ = 0;
        std::array<uint64_t, 7> generations_{};
    };

    // 渲染阶段集中在一个对象内：外部只看到枚举/查询，窗口、转场和资源阶段
    // 不再由宿主成员函数各自拼接多个布尔量表达。
    class RenderState {
    public:
        enum class WindowPhase {
            Hidden,
            Visible,
        };
        enum class LyricTransitionPhase {
            Idle,
            Pending,
            Running,
        };
        enum class DCompTransitionPhase {
            Inactive,
            Active,
            EndRequested,
        };
        enum class SongTransitionPhase {
            Idle,
            Pending,
        };
        enum class DeviceResourcePhase {
            Uninitialized,
            Ready,
        };

        RenderMode mode() const { return mode_; }
        WindowPhase windowPhase() const { return window_; }
        LyricTransitionPhase lyricTransitionPhase() const { return lyricTransition_; }
        DCompTransitionPhase dcompTransitionPhase() const { return dcompTransition_; }
        DeviceResourcePhase deviceResourcePhase() const { return deviceResources_; }
        bool sessionVisible() const { return sessionVisible_; }
        bool visibilitySuppressed() const { return visibilitySuppressed_; }
        bool songTransitionPending() const {
            return songTransition_ == SongTransitionPhase::Pending;
        }

        void setMode(RenderMode mode) { mode_ = mode; }
        void setWindowPhase(WindowPhase phase) { window_ = phase; }
        void setSessionVisible(bool visible) { sessionVisible_ = visible; }
        void setVisibilitySuppressed(bool suppressed) {
            visibilitySuppressed_ = suppressed;
        }
        void setLyricTransitionPhase(LyricTransitionPhase phase) {
            lyricTransition_ = phase;
        }
        void setDCompTransitionPhase(DCompTransitionPhase phase) {
            dcompTransition_ = phase;
        }
        void setDeviceResourcePhase(DeviceResourcePhase phase) {
            deviceResources_ = phase;
        }
        void setSongTransitionPending(bool pending) {
            songTransition_ = pending ? SongTransitionPhase::Pending
                                       : SongTransitionPhase::Idle;
        }

        void requestInvalidation(RenderInvalidation value) { invalidation_.request(value); }
        void requestInvalidation(RenderInvalidationMask mask) { invalidation_.request(mask); }
        bool isInvalidated(RenderInvalidation value) const {
            return invalidation_.contains(value);
        }
        bool hasInvalidation() const { return invalidation_.any(); }
        InvalidationSnapshot beginInvalidation() const { return invalidation_.begin(); }
        void commitInvalidation(const InvalidationSnapshot& snapshot) {
            invalidation_.commit(snapshot);
        }

    private:
        RenderMode mode_ = RenderMode::Normal;
        WindowPhase window_ = WindowPhase::Hidden;
        LyricTransitionPhase lyricTransition_ = LyricTransitionPhase::Idle;
        DCompTransitionPhase dcompTransition_ = DCompTransitionPhase::Inactive;
        DeviceResourcePhase deviceResources_ = DeviceResourcePhase::Uninitialized;
        bool sessionVisible_ = false;
        bool visibilitySuppressed_ = false;
        SongTransitionPhase songTransition_ = SongTransitionPhase::Idle;
        InvalidationState invalidation_;
    };

    RenderState renderState_;

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool timerRunning_ = false;
    UINT timerMs_ = 0; // 当前定时器实际间隔（活动/暂停档位切换用）
    bool placementTimerRunning_ = false;
    UINT displayRefreshHz_ = 60;
    bool allowOverlap_ = false;
    bool probeReady_ = false;
    TaskbarPlacementStatus placementStatus_ = TaskbarPlacementStatus::Unavailable;
    std::function<void(TaskbarPlacementStatus)> onPlacementStatusChanged_;

    // 任务栏句柄与子部件
    HWND taskbar_ = nullptr;
    bool taskbarEmbedded_ = false;
    bool appBarRegistered_ = false;
    bool appBarFullscreenOpen_ = false;
    AppBarEdge appBarEdge_ = AppBarEdge::Top;
    HWND notify_ = nullptr;
    HWND start_ = nullptr;
    RECT rcTaskbar_{};   // 任务栏屏幕坐标（缓存）
    RECT rcNotify_{};    // 通知区屏幕坐标（缓存）
    RECT rcStart_{};     // 开始按钮屏幕坐标（缓存，找不到时为空）
    RECT rcTrafficMonitor_{}; // TrafficMonitor 屏幕坐标（缓存，未运行时为 empty）
    // 任务栏 XAML 按钮（开始/搜索/任务视图/小组件/应用图标）包围矩形缓存（屏幕坐标）。
    // 以上探测数据由工作线程产出、UI 线程拾取（见 probeOut_）
    std::vector<RECT> uiaButtons_;
    // 酷狗进程中与任务栏相交的可见窗口（屏幕坐标）。
    std::vector<RECT> kugouTaskbarWindows_;
    UINT dpi_ = 96;
    bool centerAlign_ = true;
    UINT taskbarEdge_ = ABE_BOTTOM;
    bool lightTheme_ = false;
    int positionMode_ = 0; // 0 = 通知区域前；1 = 任务栏起始端

    // 避让探测工作线程：UIA / GetWindowText 等阻塞型跨进程调用全部在这里执行，
    // UI 线程永不阻塞，从根上避免与 explorer 互相死等。结果经原子指针交付
    struct ProbeResult {
        RECT rcTm{};
        std::vector<RECT> buttons;
        std::vector<RECT> kugouWindows;
    };
    std::thread probeThread_;
    std::atomic<bool> probeStop_{false};
    std::atomic<bool> probeFast_{false};
    std::atomic<ProbeResult*> probeOut_{nullptr};
    std::atomic<HWND> taskbarAtomic_{nullptr}; // taskbar_ 的线程安全副本（探测线程读）
    // 系统托盘溢出按钮的 UIA 查询放到独立线程，避免 Explorer 的跨进程调用阻塞歌词 UI。
    std::shared_ptr<std::atomic<bool>> trayOverflowOpening_ =
        std::make_shared<std::atomic<bool>>(false);

    // 歌词状态
    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    std::function<void()> onStatusTextCycleCompleted_;
    bool statusTextOneShot_ = false;
    ULONGLONG statusTextOneShotStartMs_ = 0;
    int statusTextOneShotRounds_ = 0;
    bool statusTextCycleCallbackPending_ = false;
    int currentLine = -1;
    int64_t positionMs_ = 0; // 播放进度（每帧更新），驱动逐字高亮
    DisplayScene scene_ = DisplayScene::NoPlayback;
    std::wstring trackKey_;
    uint64_t requestGeneration_ = 0;
    uint64_t frameRevision_ = 0;

    // 字体：默认字体族与软件普通窗口一致（fluent::uiFontFamily）
    float fontSize_ = kBaseFontSize;
    std::wstring fontFamily_ = fluent::uiFontFamily();
    LyricFontStyle fontStyle_ = LyricFontStyle::Normal;

    // 媒体信息
    OverlayMediaInfo media;
    IdlePresentation idle;
    ID2D1Bitmap* coverBmp = nullptr;
    ID2D1Bitmap* platformIconBmp = nullptr;

    // 交互
    std::function<void()> tick;
    std::function<void(MediaControl)> onControl;
    std::function<void()> onImmersiveExit;
    std::function<void(POINT)> onContextMenu;
    std::function<void(POINT)> onImmersiveMenu;
    std::function<void(POINT)> onAppCollection;
    std::function<void(POINT)> onDockQuickApps;
    std::function<void(int)> onPositionModeChanged;
    bool mouseOver_ = false;
    bool trackingLeave_ = false;
    int immersiveControlHover_ = -1;
    int immersiveControlPressed_ = -1;
    bool dragPress_ = false;
    bool lyricDragging_ = false;
    POINT dragPressScreen_{};
    POINT dragCursorScreen_{};
    int dragCandidateMode_ = 0;
    int dragPreviewMajorPx_ = 0;
    bool controlsOnHover_ = true;
    bool contextMenuEnabled_ = true;
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    MediaPopupTrigger floatingCardTrigger_ = MediaPopupTrigger::Hover;
    MediaPopup mediaPopup;
    bool mediaPopupEnabled_ = false;
    bool quitting = false;

    // 应用音量（内嵌控件音量图标 + 悬停滑块浮窗）
    AppVolumeState appVolume_;
    VolumePopup volumePopup_;
    std::function<void(int)> onAppVolume;
    bool volumeHover_ = false;

    // 逐字填充进度（布局像素坐标）：目标值 + 平滑值。
    // SMTC 进度是锚点插值的，每次锚点校正都会阶跃一次；平滑值按当前字时长
    // 决定的时间常数指数趋近目标，消除阶跃闪烁且同步误差有界
    float karaokeProgX_ = 0.0f;      // 本帧实际使用的填充进度（平滑后）
    float karaokeSmoothX_ = 0.0f;    // 平滑状态
    int karaokeSmoothLine_ = -1;     // 平滑状态所属行号
    bool karaokeEnteringLine_ = false; // 自然转场中的新行从首字平滑追赶真实位置
    ULONGLONG karaokeTick_ = 0;      // 上次平滑步进的时刻

    struct KaraokeSpan {
        int64_t startMs = 0;
        int64_t endMs = 0;
        float startX = 0.0f;
        float endX = 0.0f;
    };
    std::vector<KaraokeSpan> karaokeSpans_;
    int karaokeGeometryLine_ = -1;
    const IDWriteTextLayout* karaokeGeometryLayout_ = nullptr;

    // 滚动字幕（歌名、歌手、歌词独立滚动）
    float titleWidth_ = 0.0f;
    float titleHeight_ = 0.0f;
    float titleScrollOffset_ = 0.0f;
    float artistWidth_ = 0.0f;
    float artistHeight_ = 0.0f;
    float artistScrollOffset_ = 0.0f;
    float lyricWidth_ = 0.0f;
    float lyricHeight_ = 0.0f;
    float lyricScrollOffset_ = 0.0f;
    float lyricScrollSpeed_ = kLyricScrollSpeed; // 动态速度：随当前行时长变化，最慢 kLyricScrollSpeed
    float secondaryWidth_ = 0.0f;
    float secondaryHeight_ = 0.0f;
    float secondaryScrollOffset_ = 0.0f;
    std::wstring lastTitle_;
    std::wstring lastArtist_;
    std::wstring lastLyric_;
    std::wstring lastSecondary_;
    std::wstring clockTimeText_;
    std::wstring clockDateText_;
    uint64_t clockMinuteKey_ = 0;
    system_monitor::ResourceMonitor resourceMonitor_;
    uint64_t resourceSnapshotRevision_ = 0;
    ULONGLONG nextResourceSnapshotPollMs_ = 0;
    std::wstring resourceCpuText_ = L"—";
    std::wstring resourceMemoryText_ = L"—";
    std::wstring resourceDownloadText_ = L"—";
    std::wstring resourceUploadText_ = L"—";
    std::wstring resourceGpuText_ = L"—";
    std::wstring resourceCpuFrequencyText_ = L"—";
    DockResourceVisibility dockResourceVisibility_{};
    struct DockResourceRow {
        std::wstring left;
        std::wstring right;
    };
    size_t dockResourcePage_ = 0;
    ULONGLONG lastTickMs_ = 0;
    int slowTick_ = 0; // 慢速分支计数器

    // 渲染
    struct VerticalLyricPart {
        IDWriteTextLayout* layout = nullptr;
        float width = 0.0f;
        float height = 0.0f;
        bool rotated = false;
    };

    DCompRenderer renderer;
    // 沉浸模式切歌时，歌曲内容会先绘制到独立的 DirectComposition 表面。
    // 绘制辅助函数统一从这里取目标，普通帧保持使用交换链上下文。
    ID2D1DeviceContext* drawTargetOverride_ = nullptr;
    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtArtist_ = nullptr;
    IDWriteTextFormat* fmtLyric_ = nullptr;
    IDWriteTextFormat* fmtNextLyric_ = nullptr;
    IDWriteTextFormat* fmtSecondary_ = nullptr;
    IDWriteTextFormat* fmtDragPreview_ = nullptr;
    IDWriteTextFormat* fmtClockTime_ = nullptr;
    IDWriteTextFormat* fmtClockDate_ = nullptr;
    IDWriteTextLayout* titleLayout_ = nullptr;
    IDWriteTextLayout* artistLayout_ = nullptr;
    IDWriteTextLayout* lyricLayout_ = nullptr;
    // 侧边任务栏使用独立的竖排布局；中文/符号逐字排列，连续英文片段整组旋转。
    // 横向布局仍保留给上下任务栏和逐字时间轴计算，避免两种排版互相改变测量结果。
    IDWriteTextLayout* verticalLyricLayout_ = nullptr;
    std::vector<VerticalLyricPart> verticalLyricParts_;
    IDWriteTextLayout* nextLyricLayout_ = nullptr;
    IDWriteTextLayout* secondaryLayout_ = nullptr;
    // 行切换动画保留上一帧的布局，避免新行直接替换导致文字瞬移。
    IDWriteTextLayout* outgoingLyricLayout_ = nullptr;
    IDWriteTextLayout* outgoingVerticalLyricLayout_ = nullptr;
    IDWriteTextLayout* outgoingSecondaryLayout_ = nullptr;
    IDWriteTextLayout* outgoingNextLyricLayout_ = nullptr;
    bool lyricLayoutDoubleLine_ = false;
    float outgoingLyricWidth_ = 0.0f;
    float outgoingLyricHeight_ = 0.0f;
    float verticalLyricWidth_ = 0.0f;
    float verticalLyricHeight_ = 0.0f;
    bool verticalLyricRotated_ = false;
    std::vector<VerticalLyricPart> outgoingVerticalLyricParts_;
    float outgoingVerticalLyricWidth_ = 0.0f;
    float outgoingVerticalLyricHeight_ = 0.0f;
    bool outgoingVerticalLyricRotated_ = false;
    float outgoingLyricBlockHeight_ = 0.0f;
    float outgoingLyricScrollOffset_ = 0.0f;
    float outgoingSecondaryWidth_ = 0.0f;
    float outgoingSecondaryHeight_ = 0.0f;
    float outgoingSecondaryScrollOffset_ = 0.0f;
    float outgoingNextLyricWidth_ = 0.0f;
    float outgoingNextLyricHeight_ = 0.0f;
    float nextLyricWidth_ = 0.0f;
    float nextLyricHeight_ = 0.0f;
    enum class LyricTransitionKind {
        None,
        Line,
        Scene,
    };
    LyricTransitionKind lyricTransitionKind_ = LyricTransitionKind::None;
    DisplayScene outgoingScene_ = DisplayScene::NoPlayback;
    bool outgoingDoubleLine_ = false;
    bool sceneTransitionNeedsRelayout_ = false;
    int sceneTransitionFromPxW_ = 0;
    int sceneTransitionFromPxH_ = 0;
    ULONGLONG lyricTransitionStartMs_ = 0;
    int lyricTransitionDirection_ = 1; // 1: 新行从下方进入，-1: 从上方进入
    uint64_t lyricTransitionRevision_ = 0;
    // 行过渡目标：currentLine 是逻辑当前行；transitionTarget_ 是本次动画要进入的行，
    // pendingTarget_ 是动画期间收到的最新目标（latest-frame-wins，只覆盖不累积），
    // 动画结束收尾时统一消费，不让旧的结束逻辑覆盖新行。
    struct LyricTransitionTarget {
        int lineIndex = -1;
        int64_t actualPositionMs = 0;
        int direction = 1;
        uint64_t frameRevision = 0;
    };
    std::optional<LyricTransitionTarget> transitionTarget_;
    std::optional<LyricTransitionTarget> pendingTarget_;
    ULONGLONG frameNowMs_ = 0;
    ID2D1SolidColorBrush* brushBg_ = nullptr;
    ID2D1SolidColorBrush* brushHover_ = nullptr;
    ID2D1SolidColorBrush* brushText_ = nullptr;
    ID2D1SolidColorBrush* brushDim_ = nullptr;
    ID2D1SolidColorBrush* brushBtn_ = nullptr;
    ID2D1SolidColorBrush* brushBtnDisabled_ = nullptr;
    ID2D1SolidColorBrush* brushLyric_ = nullptr;        // 已播放歌词颜色（用户可配）
    ID2D1SolidColorBrush* brushLyricDim_ = nullptr;     // 逐字歌词未唱部分（独立颜色+不透明度）
    ID2D1SolidColorBrush* brushLyricGlow_ = nullptr;    // 歌词光晕（主色低透明度）
    ID2D1SolidColorBrush* brushLyricOutline_ = nullptr; // 歌词深色描边
    ID2D1SolidColorBrush* brushCoverHalo_ = nullptr;    // 黑胶外圈光环（复用已播放色）
    ID2D1SolidColorBrush* brushVinylBase_ = nullptr;    // 黑胶唱片底色
    ID2D1SolidColorBrush* brushVinylGroove_ = nullptr;  // 黑胶纹理线
    COLORREF lyricColor_ = RGB(49, 194, 124);           // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124);   // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                    // 逐字未播放不透明度（%）
    COLORREF lyricGlowColor_ = RGB(49, 194, 124);       // 光晕颜色
    COLORREF lyricOutlineColor_ = RGB(0, 0, 0);         // 描边颜色
    bool lyricGlow_ = false;                            // 光晕开关
    bool lyricOutline_ = false;                         // 描边开关
    bool translationEnabled_ = true;
    bool romanizationEnabled_ = false;
    bool doubleLineLyricsEnabled_ = false;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    LyricAlignment idleQuoteAlignment_ = LyricAlignment::Left;
    IdleQuoteBackground idleQuoteBackground_ = IdleQuoteBackground::None;
    IdleQuoteBackgroundScope idleQuoteBackgroundScope_ = IdleQuoteBackgroundScope::DailyQuote;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;
    bool clientAnimations_ = true;
    // 渲染模式：0 正常；1 低渲染（播放中也固定 ~30fps）；2 完全停止（窗口隐藏、
    // 帧定时器停止、GPU 设备释放，数据状态保留在内存）；3 极简（保留歌词刷新率，
    // 仅关闭附加视觉、媒体卡片和切歌弹窗）
    float vinylAngleDeg_ = 0.0f;
    ULONGLONG vinylTickMs_ = 0;
    // 频谱：基色可跟随已播放色、专辑主题色或自定义色（createLyricBrushes），
    // bands 由 UI 线程每帧写入
    ID2D1SolidColorBrush* brushSpectrum_ = nullptr;
    ID2D1LinearGradientBrush* brushSpectrumBarGradient_ = nullptr;
    SpectrumStyle spectrumStyle_ = SpectrumStyle::Default;
    COLORREF spectrumColor_ = RGB(49, 194, 124);
    bool spectrumCustomColor_ = false;
    bool spectrumFollowAlbum_ = false;
    COLORREF spectrumAlbumColor_ = RGB(49, 194, 124);
    bool spectrumAlbumColorAvailable_ = false;
    bool spectrumGradient_ = false;
    bool spectrumBackground_ = false;
    int spectrumOpacityPct_ = 40;
    // 播放进度背景：与背景波浪互斥，颜色取专辑主色（无封面时回退系统强调色）
    bool progressBackground_ = false;
    int progressBackgroundOpacityPct_ = 25;
    ID2D1SolidColorBrush* brushProgressBg_ = nullptr;
    // 任务栏歌词背景：封面模糊（GaussianBlur → Scale 效果链 + 主题遮罩）或纯色；
    // 画在最底层，可与进度背景、背景波浪叠加
    TaskbarBackground background_ = TaskbarBackground::None;
    int coverBackgroundOpacityPct_ = 60;
    ID2D1SolidColorBrush* brushBackground_ = nullptr; // 纯色填充与模糊遮罩共用（每帧 SetColor）
    // 沉浸模式使用完整任务栏客户区；AppBar 使用独立的 Shell 工作区协议。
    TaskbarViewMode viewMode_ = TaskbarViewMode::Embedded;
    int immersiveBackgroundBlurPct_ = 88;
    int dockBackgroundBlurPct_ = 88;
    int immersiveBackgroundAdjustment_ = 0;
    int dockBackgroundAdjustment_ = 0;
    bool backgroundBlurSupported_ = true;
    DockBackdrop dockBackdrop_;
    ID2D1SolidColorBrush* brushIdleWarm_ = nullptr;
    ID2D1SolidColorBrush* brushIdleCool_ = nullptr;
    ID2D1SolidColorBrush* brushIdleAccent_ = nullptr;
    DockPet dockPet_;
    // 伴听宠物已见到的曲目（title|artist），用于检测"播放中切歌"并触发雀跃。
    std::wstring dockPetTrack_;
    ID2D1StrokeStyle* dragPreviewStroke_ = nullptr;
    ID2D1Effect* coverBlurFx_ = nullptr;
    ID2D1Effect* coverScaleFx_ = nullptr;
    ID2D1Bitmap* coverBlurInput_ = nullptr; // 模糊链当前绑定的封面（不持有引用，仅用于比较）
    bool spectrumVisible_ = false;
    std::array<float, TaskbarHost::kSpectrumBands> spectrumBands_{};
    ID2D1RoundedRectangleGeometry* coverClip_ = nullptr;
    ID2D1EllipseGeometry* vinylCoverClip_ = nullptr;
    ID2D1Layer* coverLayer_ = nullptr;
    // 滚动歌词左缘渐隐：固定两停止点，渐变轴每帧按滚动偏移更新
    ID2D1LinearGradientBrush* lyricEdgeFadeBrush_ = nullptr;
    ID2D1Layer* lyricEdgeFadeLayer_ = nullptr;
    // 右缘渐隐带：完全位于歌词区右缘之外的留白（到频谱/窗口边缘的间隙），
    // 不遮挡可读区域内的文字
    ID2D1LinearGradientBrush* lyricRightFadeBrush_ = nullptr;
    ID2D1Layer* lyricRightFadeLayer_ = nullptr;
    // 歌曲信息与歌词之间的悬浮分隔线，使用上下渐隐的主题色画刷
    ID2D1LinearGradientBrush* songInfoDividerBrush_ = nullptr;
    media_control::Geometry controlGeometry;
    struct SceneResizeAnimation {
        WindowPlacement from{};
        WindowPlacement to{};
        WindowPlacement lastApplied{};
        ULONGLONG startMs = 0;
    };
    std::optional<SceneResizeAnimation> sceneResize_;
    struct SongContentTransition {
        ULONGLONG startMs = 0;
        bool compositorLayer = false;
        bool pendingConsumed = false;
    };
    std::optional<SongContentTransition> songContentTransition_;
    int lastPxW_ = 0;
    int lastPxH_ = 0;
    int lastLogicalPxW_ = 0;
    int lastLogicalPxH_ = 0;
    // 静止跳帧：updateScroll 每帧重算 scrollAnimating_（跑马灯/跟随滚动/转场收尾是否在动），
    // karaokeSettled_ 表示逐字平滑已收敛；两者都静止且无脏状态时可跳过整帧重绘。
    bool scrollAnimating_ = true;
    bool karaokeSettled_ = true;
    // 光晕/描边离屏缓存：glow+outline+本体三层只合成一次，滚动/淡变时做 DrawImage 平移，
    // 避免每帧对主歌词重复 16+ 次 DrawTextLayout。双行转场同时绘制旧/新布局，保留两个
    // 缓存槽，避免两套布局在同一帧互相驱逐。布局、画刷或效果组合变化时经 textFxGen_ 失效。
    struct TextFxCacheEntry {
        ID2D1BitmapRenderTarget* target = nullptr;
        const IDWriteTextLayout* layout = nullptr;
        ID2D1Brush* brush = nullptr;
        ID2D1Brush* outline = nullptr;
        ID2D1Brush* glow = nullptr;
        uint64_t generation = 0;
        uint64_t lastUse = 0;
        UINT dpi = 0;
        float w = 0.0f;
        float h = 0.0f;
        float pad = 0.0f;
    };
    std::array<TextFxCacheEntry, 2> textFxCaches_{};
    uint64_t textFxUse_ = 0;
    uint64_t textFxGen_ = 0;

    bool isWindowVisible() const {
        return renderState_.windowPhase() == RenderState::WindowPhase::Visible;
    }

    bool isSessionVisible() const { return renderState_.sessionVisible(); }

    bool isRenderMode(RenderMode mode) const { return renderState_.mode() == mode; }

    bool isStoppedMode() const { return isRenderMode(RenderMode::Stopped); }

    bool isLyricTransitionPending() const {
        return renderState_.lyricTransitionPhase() ==
               RenderState::LyricTransitionPhase::Pending;
    }

    bool isLyricTransitionActive() const {
        return renderState_.lyricTransitionPhase() ==
               RenderState::LyricTransitionPhase::Running;
    }

    bool isLyricTransitionInProgress() const {
        return renderState_.lyricTransitionPhase() !=
               RenderState::LyricTransitionPhase::Idle;
    }

    void setLyricTransitionPending() {
        renderState_.setLyricTransitionPhase(RenderState::LyricTransitionPhase::Pending);
    }

    void setLyricTransitionActive() {
        renderState_.setLyricTransitionPhase(RenderState::LyricTransitionPhase::Running);
    }

    void clearLyricTransitionPhase() {
        renderState_.setLyricTransitionPhase(RenderState::LyricTransitionPhase::Idle);
    }

    bool isLyricDCompActive() const {
        return renderState_.dcompTransitionPhase() ==
               RenderState::DCompTransitionPhase::Active;
    }

    bool isLyricDCompEndRequested() const {
        return renderState_.dcompTransitionPhase() ==
               RenderState::DCompTransitionPhase::EndRequested;
    }

    void requestLyricDCompEnd() {
        if (isLyricDCompActive())
            renderState_.setDCompTransitionPhase(
                RenderState::DCompTransitionPhase::EndRequested);
    }

    void clearLyricDCompState() {
        renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::Inactive);
    }

    bool isSongTransitionPending() const {
        return renderState_.songTransitionPending();
    }

    void setSongTransitionPending(bool pending) {
        renderState_.setSongTransitionPending(pending);
    }

    bool immersiveSongContentLayerActive() const {
        return isExpandedView() && songContentTransition_ &&
               songContentTransition_->compositorLayer;
    }

    bool isSceneResizeActive() const { return sceneResize_.has_value(); }

    void requestInvalidation(RenderInvalidation value) {
        renderState_.requestInvalidation(value);
    }

    void requestInvalidation(RenderInvalidationMask mask) {
        renderState_.requestInvalidation(mask);
    }

    bool isInvalidated(RenderInvalidation value) const {
        return renderState_.isInvalidated(value);
    }

    void requestFrame() { requestInvalidation(RenderInvalidation::Paint); }

    void flushRenderRequest() {
        if (isWindowVisible() && hwnd)
            render();
    }

    void requestFrameAndFlush() {
        requestFrame();
        flushRenderRequest();
    }

    bool isExpandedView() const {
        return viewMode_ == TaskbarViewMode::Immersive ||
               viewMode_ == TaskbarViewMode::AppBar;
    }

    bool isAppBarView() const { return viewMode_ == TaskbarViewMode::AppBar; }

    int expandedControlCount() const {
        return isAppBarView() ? kAppBarControlCount : kImmersiveControlCount;
    }

    bool expandedControlVisible(int index) const {
        if (!isAppBarView())
            return index >= 0 && index < kExpandedControlCount &&
                   index != kDockControlQuickApps;
        return (index >= kImmersiveControlPrevious && index <= kDockControlQuickApps) ||
               index == kImmersiveControlMenu;
    }

    bool shouldShowWindow() const {
        if (!hwnd || isStoppedMode() || !isSessionVisible() ||
            renderState_.visibilitySuppressed())
            return false;
        if (!allowOverlap_ && (placementStatus_ == TaskbarPlacementStatus::NoSpace ||
                               placementStatus_ == TaskbarPlacementStatus::Unavailable))
            return false;
        return true;
    }

    bool reconcileWindowVisibility(bool prewarmBeforeShow = false) {
        const bool wantVisible = shouldShowWindow();
        const bool nativeVisible = hwnd && IsWindowVisible(hwnd);
        const bool appBarRegistrationMismatch =
            isAppBarView() && (wantVisible != appBarRegistered_);
        if (wantVisible == isWindowVisible() && wantVisible == nativeVisible &&
            !appBarRegistrationMismatch)
            return false;

        renderState_.setWindowPhase(wantVisible ? RenderState::WindowPhase::Visible
                                                : RenderState::WindowPhase::Hidden);
        if (wantVisible) {
            if (isAppBarView() && !registerAppBar()) {
                renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
                ShowWindow(hwnd, SW_HIDE);
                setPlacementStatus(TaskbarPlacementStatus::Unavailable);
                return false;
            }
            // 完全停止模式会释放 DComp 设备链。先在原生窗口仍隐藏时完成首帧，
            // 让设备/交换链/字体和位图资源的重建成本不落到用户可见的第一帧。
            if (prewarmBeforeShow)
                requestFrameAndFlush();
            ShowWindow(hwnd, SW_SHOWNA);
            ensureImmersiveZOrder();
            startFrameTimer();
        } else {
            stopFrameTimer();
            clearImmersiveSongContentTransition();
            if (hwnd)
                ShowWindow(hwnd, SW_HIDE);
            if (isAppBarView())
                unregisterAppBar();
        }
        return true;
    }

    float scale() const { return static_cast<float>(dpi_) / 96.0f; }

    ID2D1DeviceContext* drawTarget() {
        return drawTargetOverride_ ? drawTargetOverride_ : renderer.renderTarget();
    }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    bool isVerticalTaskbar() const {
        return !isAppBarView() && isVerticalTaskbarEdge(taskbarEdge_);
    }

    void clientPixelSize(int& width, int& height) const {
        width = 0;
        height = 0;
        if (!hwnd)
            return;
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;
        }
    }

    void logicalClientPixelSize(int& width, int& height) const {
        clientPixelSize(width, height);
    }

    void clientPointToLogicalDip(float clientX, float clientY, float& xDip,
                                 float& yDip) const {
        xDip = clientX / scale();
        yDip = clientY / scale();
    }

    POINT logicalDipToClientPoint(float xDip, float yDip) const {
        return POINT{static_cast<LONG>(std::lround(xDip * scale())),
                     static_cast<LONG>(std::lround(yDip * scale()))};
    }

    bool updateMediaInfo(const OverlayMediaInfo& info) {
        bool thumbChanged = info.thumbnail != media.thumbnail;
        bool textChanged = info.title != media.title || info.artist != media.artist;
        bool controlsChanged = info.canPrev != media.canPrev ||
                               info.canPlayPause != media.canPlayPause ||
                               info.canNext != media.canNext;
        bool playingChanged = info.playing != media.playing;
        bool platformChanged = info.sourceAppUserModelId != media.sourceAppUserModelId;
        bool dominantColorChanged =
            info.hasDominantColor != media.hasDominantColor ||
            (info.hasDominantColor && info.dominantColor != media.dominantColor);
        bool durationChanged = info.durationMs != media.durationMs;
        media = info;
        if (thumbChanged)
            requestInvalidation(RenderInvalidation::Cover);
        if (platformChanged)
            requestInvalidation(RenderInvalidation::PlatformIcon);
        if (thumbChanged || textChanged)
            vinylAngleDeg_ = 0.0f;
        if (textChanged)
            requestInvalidation(RenderInvalidation::SongInfo);
        if (thumbChanged || textChanged || playingChanged)
            vinylTickMs_ = monotonicNowMs();
        return thumbChanged || textChanged || controlsChanged || playingChanged || platformChanged ||
               dominantColorChanged || durationChanged;
    }

    // ---------- 窗口创建与定位 ----------

    bool findTaskbar() {
        taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
        taskbarAtomic_ = taskbar_; // 同步给探测工作线程
        if (!taskbar_)
            return false;
        notify_ = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr);
        start_ = FindWindowExW(taskbar_, nullptr, L"Start", nullptr);
        dpi_ = GetDpiForWindow(taskbar_);
        centerAlign_ = isTaskbarCenterAlign();
        lightTheme_ = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        updateRects();
        taskbarEdge_ = queryTaskbarEdge(taskbar_, rcTaskbar_);
        return true;
    }

    void updateRects() {
        if (taskbar_)
            GetWindowRect(taskbar_, &rcTaskbar_);
        if (notify_)
            GetWindowRect(notify_, &rcNotify_);
        if (start_)
            GetWindowRect(start_, &rcStart_);
    }

    void scheduleTaskbarAttachRetry() {
        if (hwnd)
            SetTimer(hwnd, kTaskbarAttachTimerId, kTaskbarAttachRetryMs, nullptr);
    }

    void cancelTaskbarAttachRetry() {
        if (hwnd)
            KillTimer(hwnd, kTaskbarAttachTimerId);
    }

    bool attachToTaskbar(HWND window) {
        taskbarEmbedded_ = false;
        if (!window || !taskbar_ || !IsWindow(taskbar_))
            return false;

        SetLastError(ERROR_SUCCESS);
        const LONG_PTR originalStyle = GetWindowLongPtrW(window, GWL_STYLE);
        if (originalStyle == 0 && GetLastError() != ERROR_SUCCESS)
            return false;

        // SetParent 不会替窗口切换 WS_POPUP/WS_CHILD；先切成真正的子窗口，
        // 否则后续 SetWindowPos 仍可能按顶层窗口的屏幕坐标解释。
        const LONG_PTR childStyle =
            (originalStyle & ~static_cast<LONG_PTR>(WS_POPUP)) |
            static_cast<LONG_PTR>(WS_CHILD);
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWL_STYLE, childStyle) == 0 &&
            GetLastError() != ERROR_SUCCESS)
            return false;

        // 初始窗口没有父窗口时，SetParent 成功也会返回 nullptr（返回的是旧父窗口），
        // 必须结合 GetLastError 和实际父窗口判断，不能直接判断返回值。
        SetLastError(ERROR_SUCCESS);
        const HWND previousParent = SetParent(window, taskbar_);
        const DWORD error = GetLastError();
        if (!previousParent && error != ERROR_SUCCESS) {
            SetParent(window, nullptr);
            SetWindowLongPtrW(window, GWL_STYLE, originalStyle);
            runtime_log::writef(L"[taskbar] SetParent failed: error=%lu hwnd=%p parent=%p",
                                static_cast<unsigned long>(error), window, taskbar_);
            return false;
        }

        if (GetParent(window) != taskbar_) {
            SetParent(window, nullptr);
            SetWindowLongPtrW(window, GWL_STYLE, originalStyle);
            runtime_log::writef(L"[taskbar] SetParent parent mismatch: hwnd=%p parent=%p",
                                window, taskbar_);
            return false;
        }

        taskbarEmbedded_ = true;
        return true;
    }

    bool detachFromTaskbar() {
        if (!hwnd)
            return false;
        cancelTaskbarAttachRetry();
        const HWND originalParent = GetParent(hwnd);
        const LONG_PTR originalStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (originalParent) {
            SetLastError(ERROR_SUCCESS);
            const HWND previousParent = SetParent(hwnd, nullptr);
            const DWORD error = GetLastError();
            if (!previousParent && error != ERROR_SUCCESS) {
                runtime_log::writef(L"[appbar] detach failed: error=%lu",
                                    static_cast<unsigned long>(error));
                return false;
            }
        }

        // SetParent 不会自动切换 WS_CHILD/WS_POPUP。仍带 WS_CHILD 时，桌面窗口会被
        // GetParent 返回为父窗口，因此必须先完成顶层样式切换，再校验是否真正脱离。
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR popupStyle =
            (originalStyle & ~static_cast<LONG_PTR>(WS_CHILD)) |
            static_cast<LONG_PTR>(WS_POPUP);
        if (SetWindowLongPtrW(hwnd, GWL_STYLE, popupStyle) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            runtime_log::writef(L"[appbar] popup style failed: error=%lu",
                                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
        if (GetParent(hwnd) != nullptr) {
            runtime_log::writef(L"[appbar] detach verification failed: parent=%p",
                                GetParent(hwnd));
            return false;
        }
        taskbarEmbedded_ = false;
        return true;
    }

    HMONITOR appBarMonitor() const {
        if (taskbar_ && IsWindow(taskbar_))
            return MonitorFromWindow(taskbar_, MONITOR_DEFAULTTOPRIMARY);
        if (hwnd)
            return MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    void updateAppBarZOrder() {
        if (!hwnd || !appBarRegistered_)
            return;
        // Shell 只会通过 ABN_FULLSCREENAPP 报告真正的全屏打开/关闭；最大化窗口
        // 不进入该状态。全屏期间按 AppBar 规范降到 Z 序底部，退出后恢复置顶。
        SetWindowPos(hwnd, appBarFullscreenOpen_ ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    bool updateAppBarPosition() {
        if (!hwnd || !appBarRegistered_)
            return false;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(appBarMonitor(), &monitor))
            return false;

        UINT nextDpi = GetDpiForWindow(hwnd);
        if (nextDpi == 0 && taskbar_)
            nextDpi = GetDpiForWindow(taskbar_);
        if (nextDpi == 0)
            nextDpi = 96;
        if (dpi_ != nextDpi) {
            dpi_ = nextDpi;
            renderer.setDpi(dpi_);
            requestInvalidation(toMask(RenderInvalidation::Layout) |
                                toMask(RenderInvalidation::Text) |
                                toMask(RenderInvalidation::Cover));
        }

        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uEdge = appBarEdge_ == AppBarEdge::Top ? ABE_TOP : ABE_BOTTOM;
        data.rc = monitor.rcMonitor;
        const LONG height = std::max<LONG>(1, static_cast<LONG>(std::lround(
                                                   kAppBarHeightDip * dpi_ / 96.0f)));
        if (data.uEdge == ABE_TOP)
            data.rc.bottom = data.rc.top + height;
        else
            data.rc.top = data.rc.bottom - height;
        SHAppBarMessage(ABM_QUERYPOS, &data);

        if (data.uEdge == ABE_TOP)
            data.rc.bottom = data.rc.top + height;
        else
            data.rc.top = data.rc.bottom - height;
        SHAppBarMessage(ABM_SETPOS, &data);
        if (data.rc.right <= data.rc.left || data.rc.bottom <= data.rc.top)
            return false;

        SetWindowPos(hwnd, appBarFullscreenOpen_ ? HWND_BOTTOM : HWND_TOPMOST,
                     data.rc.left, data.rc.top,
                     data.rc.right - data.rc.left, data.rc.bottom - data.rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        mediaPopup.setAnchor(hwnd);
        setPlacementStatus(TaskbarPlacementStatus::Safe);
        return true;
    }

    bool registerAppBar() {
        if (!isAppBarView() || !hwnd)
            return false;
        if (appBarRegistered_)
            return updateAppBarPosition();
        if (!detachFromTaskbar())
            return false;
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uCallbackMessage = kAppBarCallbackMessage;
        if (!SHAppBarMessage(ABM_NEW, &data)) {
            runtime_log::writef(L"[appbar] ABM_NEW failed");
            return false;
        }
        appBarRegistered_ = true;
        if (!updateAppBarPosition()) {
            unregisterAppBar();
            return false;
        }
        runtime_log::writef(L"[appbar] registered edge=%s",
                            appBarEdge_ == AppBarEdge::Top ? L"top" : L"bottom");
        return true;
    }

    void unregisterAppBar() {
        if (!appBarRegistered_ || !hwnd)
            return;
        appBarRegistered_ = false;
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        SHAppBarMessage(ABM_REMOVE, &data);
        runtime_log::writef(L"[appbar] unregistered");
    }

    void retryTaskbarAttach() {
        if (!hwnd)
            return;
        if (!findTaskbar())
            return;

        const bool immersiveBackdropPrepared =
            viewMode_ == TaskbarViewMode::Immersive && !dockBackdrop_.available();
        if (immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (!attachToTaskbar(hwnd)) {
            // 附着失败时仍保持顶层窗口，但必须使用屏幕坐标，避免落到屏幕顶部。
            adjustPosition();
            return;
        }

        cancelTaskbarAttachRetry();
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        adjustPosition();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    bool createWindow(HINSTANCE inst) {
        this->inst = inst;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        clientAnimations_ = animations != FALSE;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWndClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const HICON icon = app_icon::taskbarIcon();
        wc.hIcon = icon;
        wc.hIconSm = icon;
        RegisterClassExW(&wc);

        if (!findTaskbar())
            return false;

        // 内容完全由 DirectComposition visual 提供；禁用窗口自身的不透明重定向位图，
        // 避免登录或解锁后重建的底面透过半透明遮罩显示为灰底。
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
        HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyricTaskbar", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, inst, this);
        if (!h)
            return false;

        hwnd = h;
        app_icon::applyTaskbarIcon(hwnd);
        const bool immersiveBackdropPrepared =
            viewMode_ == TaskbarViewMode::Immersive;
        if (immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (isAppBarView()) {
            taskbarEmbedded_ = false;
            renderState_.setVisibilitySuppressed(false);
            setPlacementStatus(TaskbarPlacementStatus::Safe);
        } else if (!attachToTaskbar(h)) {
            scheduleTaskbarAttachRetry();
        }
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (!mediaPopup.create(inst, hwnd))
            runtime_log::writef(L"[taskbar] media popup creation failed");
        if (!volumePopup_.create(inst))
            runtime_log::writef(L"[taskbar] volume popup creation failed");
        adjustPosition();
        startProbe(); // 避让探测（阻塞型跨进程调用）全程在工作线程执行
        if (isAppBarView() && !isStoppedMode())
            startResourceMonitoring();
        if (renderState_.visibilitySuppressed() && !isStoppedMode())
            startPlacementTimer();
        return true;
    }

    // 任务栏主轴上的占用区间（屏幕坐标，已合并）：横向任务栏取 [left, right)，
    // 纵向任务栏取 [top, bottom)。来源为 UIA 按钮 + 通知区 + TrafficMonitor + 酷狗窗口。
    // 开始按钮的 HWND 矩形与 UIA StartButton 重复，合并后无害，留着可在 UIA
    // 不可用时兜底。
    std::vector<std::pair<int, int>> occupiedIntervals() const {
        std::vector<std::pair<int, int>> v;
        const bool vertical = isVerticalTaskbar();
        auto add = [&v, vertical](const RECT& r) {
            const int start = vertical ? r.top : r.left;
            const int end = vertical ? r.bottom : r.right;
            if (end > start)
                v.emplace_back(start, end);
        };
        for (const RECT& r : uiaButtons_)
            add(r);
        if (notify_)
            add(rcNotify_);
        if (start_)
            add(rcStart_);
        add(rcTrafficMonitor_);
        for (const RECT& r : kugouTaskbarWindows_)
            add(r);
        std::sort(v.begin(), v.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto& p : v) {
            if (!merged.empty() && p.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, p.second);
            else
                merged.push_back(p);
        }
        return merged;
    }

    void startPlacementTimer() {
        if (placementTimerRunning_ || !hwnd ||
            isStoppedMode())
            return;
        if (SetTimer(hwnd, kPlacementTimerId, kPlacementTimerMs, nullptr))
            placementTimerRunning_ = true;
    }

    void stopPlacementTimer() {
        if (placementTimerRunning_ && hwnd)
            KillTimer(hwnd, kPlacementTimerId);
        placementTimerRunning_ = false;
    }

    void resumeAfterPlacementAvailable() {
        if ((!probeReady_ && !allowOverlap_) ||
            placementStatus_ == TaskbarPlacementStatus::NoSpace ||
            placementStatus_ == TaskbarPlacementStatus::Unavailable)
            return;

        renderState_.setVisibilitySuppressed(false);
        probeFast_ = false;
        stopPlacementTimer();
        if (isSessionVisible() && !isStoppedMode()) {
            // 隐藏期间不运行完整帧定时器；恢复前补一次播放进度/当前行，
            // 避免窗口重新出现时先显示旧歌词，等下一帧才追上。
            if (tick)
                tick();
            const bool visibilityChanged = reconcileWindowVisibility(true);
            if (isWindowVisible() && !visibilityChanged)
                requestFrameAndFlush();
        } else if (!isWindowVisible()) {
            stopFrameTimer();
        }
    }

    TaskbarPlacementStatus setPlacementStatus(TaskbarPlacementStatus status) {
        if (placementStatus_ == status)
            return status;
        const TaskbarPlacementStatus previous = placementStatus_;
        placementStatus_ = status;
        if (status == TaskbarPlacementStatus::NoSpace ||
            status == TaskbarPlacementStatus::Unavailable) {
            probeFast_ = status == TaskbarPlacementStatus::NoSpace;
            startPlacementTimer();
            reconcileWindowVisibility();
            stopFrameTimer();
        } else if (previous == TaskbarPlacementStatus::NoSpace ||
                   previous == TaskbarPlacementStatus::Unavailable) {
            resumeAfterPlacementAvailable();
        }
        if (onPlacementStatusChanged_)
            onPlacementStatusChanged_(status);
        return status;
    }

    void releaseVisibilitySuppression() {
        if (!renderState_.visibilitySuppressed() || !probeReady_)
            return;

        if (placementStatus_ == TaskbarPlacementStatus::NoSpace) {
            // 首次真实探测已经完成：解除“等待首次结果”的抑制，但仍保持隐藏，
            // 后续由低频避让计时器等待空间恢复。
            renderState_.setVisibilitySuppressed(false);
            probeFast_ = true;
            startPlacementTimer();
            return;
        }
        if (placementStatus_ == TaskbarPlacementStatus::Unavailable)
            return;
        resumeAfterPlacementAvailable();
    }

    void setVisibilitySuppressed(bool on) {
        if (on) {
            renderState_.setVisibilitySuppressed(true);
            reconcileWindowVisibility();
            if (hwnd && !isStoppedMode())
                startPlacementTimer();
            return;
        }

        if (!probeReady_)
            return;
        releaseVisibilitySuppression();
    }

    void show() {
        if (isAppBarView()) {
            renderState_.setVisibilitySuppressed(false);
            setPlacementStatus(TaskbarPlacementStatus::Safe);
            renderState_.setSessionVisible(true);
            reconcileWindowVisibility();
            requestFrameAndFlush();
            return;
        }
        if (renderState_.visibilitySuppressed() && !allowOverlap_)
            return;
        if ((placementStatus_ == TaskbarPlacementStatus::NoSpace ||
             placementStatus_ == TaskbarPlacementStatus::Unavailable) &&
            !allowOverlap_)
            return;
        if (allowOverlap_) {
            renderState_.setVisibilitySuppressed(false);
            probeFast_ = false;
            stopPlacementTimer();
        }
        renderState_.setSessionVisible(true);
        reconcileWindowVisibility();
        requestFrameAndFlush();
    }

    void hide() {
        renderState_.setSessionVisible(false);
        reconcileWindowVisibility();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.hide();
        mediaPopup.hideImmediate();
    }

    TaskbarPlacementStatus calculateWindowPlacement(WindowPlacement& placement,
                                                     int positionMode = -1,
                                                     bool updateStatus = true) {
        auto finishStatus = [&](TaskbarPlacementStatus status) {
            return updateStatus ? setPlacementStatus(status) : status;
        };
        if (!hwnd || !taskbar_)
            return finishStatus(TaskbarPlacementStatus::Unavailable);

        updateRects();

        const bool vertical = isVerticalTaskbar();
        if (viewMode_ == TaskbarViewMode::Immersive) {
            RECT client{};
            POINT origin{0, 0};
            if (!GetClientRect(taskbar_, &client) || client.right <= client.left ||
                client.bottom <= client.top || !ClientToScreen(taskbar_, &origin))
                return finishStatus(TaskbarPlacementStatus::Unavailable);

            placement.x = origin.x;
            placement.y = origin.y;
            placement.width = client.right - client.left;
            placement.height = client.bottom - client.top;
            placement.availableMajor = vertical ? placement.height : placement.width;
            return finishStatus(TaskbarPlacementStatus::Safe);
        }

        const int taskbarCross = taskbarCrossPixels(rcTaskbar_, taskbarEdge_);
        const int crossMargin = std::max(2, (int)std::lround(2.0f * scale()));
        int crossPx = taskbarCross - crossMargin * 2;
        if (crossPx < 16)
            crossPx = taskbarCross;
        if (crossPx <= 0)
            return finishStatus(TaskbarPlacementStatus::Unavailable);

        const int effectivePositionMode = positionMode < 0 ? positionMode_ : positionMode;

        int gap = std::max(4, (int)std::lround(4.0f * scale()));
        float minWidthDip = vertical ? kVerticalMinLengthDip : kMinWidthDip;
        float maxWidthDip = vertical ? kVerticalMaxLengthDip : kMaxWidthDip;
        float compressedMinWidthDip = minWidthDip * kCompressedMinWidthRatio;
        if (!vertical && !songInfoVisible_ && scene_ != DisplayScene::Idle) {
            // 保留原歌词区宽度，只扣除歌曲信息区；左侧压缩为可见的封面区域。
            const float compactLeftDip = albumCoverVisible_ ? coverSlotWidth(dip(crossPx)) : 0.0f;
            const float flexibleMinDip = kMinWidthDip * (1.0f - kLeftRatio);
            minWidthDip = flexibleMinDip + compactLeftDip;
            compressedMinWidthDip = flexibleMinDip * kCompressedMinWidthRatio + compactLeftDip;
            maxWidthDip = kMaxWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
        }
        int minW = (int)std::lround(minWidthDip * scale());
        int compressedMinW = (int)std::lround(compressedMinWidthDip * scale());
        int maxW = (int)std::lround(maxWidthDip * scale());
        // 仅播放场景的独立频谱区域需要整体加宽；背景波浪复用内容区，不再占用额外宽度。
        const float spectrumExtra = spectrumExtraForScene(scene_);
        if (!vertical && spectrumExtra > 0.0f) {
            int extra = (int)std::lround(spectrumExtra * scale());
            minW += extra;
            compressedMinW += extra;
            maxW += extra;
        }

        // 空闲区间 = 任务栏主轴减去占用区间
        struct Span {
            int l, r;
        };
        std::vector<Span> spans;
        const int majorStart = vertical ? rcTaskbar_.top : rcTaskbar_.left;
        const int majorEnd = vertical ? rcTaskbar_.bottom : rcTaskbar_.right;
        int cursor = majorStart;
        const auto occupied = occupiedIntervals();
        for (const auto& o : occupied) {
            if (o.first > cursor)
                spans.push_back({cursor, std::min(o.first, majorEnd)});
            cursor = std::max(cursor, std::min(o.second, majorEnd));
        }
        if (cursor < majorEnd)
            spans.push_back({cursor, majorEnd});
        // 只有确实没有探测到任何占用区间时，才把整条任务栏视为可用；
        // 如果占用区间已经覆盖整个主轴，必须保留“无空闲位置”的结果。
        if (spans.empty() && occupied.empty())
            spans.push_back({majorStart, majorEnd});

        auto usableMajor = [gap](const Span& s) { return s.r - s.l - gap * 2; };

        int pxMajor = 0;
        int availableMajor = 0;
        int x = 0;
        int y = 0;
        auto place = [&](const Span& s, int w) {
            availableMajor = std::max(1, usableMajor(s));
            pxMajor = std::min(w, std::max(1, majorEnd - majorStart));
            int major = effectivePositionMode == 1 ? s.l + gap : s.r - gap - pxMajor;
            if (pxMajor <= majorEnd - majorStart)
                major = std::clamp(major, majorStart, majorEnd - pxMajor);
            else
                major = majorStart;
            if (vertical) {
                x = taskbarEdge_ == ABE_LEFT ? rcTaskbar_.left + crossMargin
                                             : rcTaskbar_.right - crossMargin - crossPx;
                y = major;
            } else {
                x = major;
                y = rcTaskbar_.top + crossMargin;
            }
        };
        TaskbarPlacementStatus result = TaskbarPlacementStatus::Unavailable;
        if (spans.empty()) {
            if (!allowOverlap_)
                return finishStatus(TaskbarPlacementStatus::NoSpace);
            const Span full{majorStart, majorEnd};
            // 用户明确选择继续开启时，也沿用压缩后的最小主轴尺寸，
            // 不再恢复为标准最小尺寸，尽量降低对任务栏的遮挡。
            place(full, compressedMinW);
            result = TaskbarPlacementStatus::ForcedOverlap;
        } else {
            // 原位优先：模式 0 锚定通知区域之前的主轴末端空闲区，模式 1 锚定
            // 任务栏起始端空闲区。横向对应右/左，纵向对应下/上。
            const Span& pref = effectivePositionMode == 1 ? spans.front() : spans.back();
            if (usableMajor(pref) >= compressedMinW) {
                place(pref, std::min(usableMajor(pref), maxW)); // 原位优先：被挤压先收缩长度
                result = usableMajor(pref) < minW ? TaskbarPlacementStatus::Compressed
                                                  : TaskbarPlacementStatus::Safe;
            } else {
                // 原位压到安全最小长度仍放不下：换到容得下的最大空闲区。
                const Span* best = nullptr;
                for (const auto& s : spans) {
                    if (usableMajor(s) >= compressedMinW &&
                        (!best || s.r - s.l > best->r - best->l))
                        best = &s;
                }
                if (best) {
                    place(*best, std::min(usableMajor(*best), maxW));
                    result = TaskbarPlacementStatus::Relocated;
                } else if (allowOverlap_) {
                    // 用户已明确允许重叠时，仍把窗口限制在任务栏主轴范围内，
                    // 只允许与已探测到的控件相交，不让窗口越出任务栏；尺寸使用
                    // 压缩后的最小值，避免强制开启时又恢复为标准尺寸。
                    place(pref, compressedMinW);
                    result = TaskbarPlacementStatus::ForcedOverlap;
                } else {
                    return finishStatus(TaskbarPlacementStatus::NoSpace);
                }
            }
        }
        if (vertical) {
            if (y < rcTaskbar_.top)
                y = rcTaskbar_.top;
            if (pxMajor <= majorEnd - majorStart && y + pxMajor > rcTaskbar_.bottom)
                y = rcTaskbar_.bottom - pxMajor;
        } else if (x < rcTaskbar_.left) {
            x = rcTaskbar_.left;
        }

        placement.x = x;
        placement.y = y;
        placement.width = vertical ? crossPx : pxMajor;
        placement.height = vertical ? pxMajor : crossPx;
        placement.availableMajor = availableMajor;
        if (placement.width <= 0 || placement.height <= 0)
            return finishStatus(TaskbarPlacementStatus::Unavailable);
        return finishStatus(result);
    }

    void applyWindowPlacement(const WindowPlacement& placement, bool bringToFront = true,
                              bool noRedraw = false) {
        if (!hwnd || placement.width <= 0 || placement.height <= 0)
            return;

        POINT pt{placement.x, placement.y};
        if (taskbarEmbedded_)
            ScreenToClient(taskbar_, &pt);

        // 把窗口提到任务栏子窗口最前面，避免被其他任务栏子窗口盖住。
        // 尺寸转场期间保持 Z 序且禁止系统自动重绘，画面由后续 render() 统一提交。
        UINT flags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
        if (!bringToFront)
            flags |= SWP_NOZORDER;
        if (noRedraw)
            flags |= SWP_NOREDRAW;
        SetWindowPos(hwnd, bringToFront ? HWND_TOP : nullptr, pt.x, pt.y, placement.width,
                     placement.height, flags);
        mediaPopup.setAnchor(hwnd);
    }

    // TrafficMonitor 是任务栏的外部子窗口，刷新自身内容时可能重新进入任务栏
    // 子窗口 Z 序的顶部。嵌入模式需要和它互相避让，沉浸模式则必须始终覆盖它；
    // 这里只在发现宿主不在顶部时修正，并明确禁止激活，避免伪装成“跑到前台”。
    void ensureImmersiveZOrder() {
        if (viewMode_ != TaskbarViewMode::Immersive || !hwnd || !taskbar_ ||
            !taskbarEmbedded_ || GetParent(hwnd) != taskbar_)
            return;
        if (GetTopWindow(taskbar_) == hwnd)
            return;
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void adjustPosition() {
        if (lyricDragging_)
            return;
        cancelSceneWindowResize();
        if (isAppBarView()) {
            if (appBarRegistered_)
                updateAppBarPosition();
            return;
        }
        WindowPlacement placement;
        const TaskbarPlacementStatus status = calculateWindowPlacement(placement);
        if (status != TaskbarPlacementStatus::Unavailable &&
            status != TaskbarPlacementStatus::NoSpace)
            applyWindowPlacement(placement);
    }

    TaskbarPlacementStatus refreshPlacement() {
        adjustPosition();
        return placementStatus_;
    }

    bool currentWindowPlacement(WindowPlacement& placement) const {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
            return false;

        placement.x = rect.left;
        placement.y = rect.top;
        placement.width = rect.right - rect.left;
        placement.height = rect.bottom - rect.top;
        return true;
    }

    static bool usableDragPlacement(TaskbarPlacementStatus status) {
        return status != TaskbarPlacementStatus::Unavailable &&
               status != TaskbarPlacementStatus::NoSpace;
    }

    int dragPreviewMajorPixels(const WindowPlacement& anchor) const {
        const int anchorMajor = anchor.availableMajor > 0
                                    ? anchor.availableMajor
                                    : (isVerticalTaskbar() ? anchor.height : anchor.width);
        const int currentMajor = dragPreviewMajorPx_ > 0
                                     ? dragPreviewMajorPx_
                                     : (isVerticalTaskbar() ? anchor.height : anchor.width);
        return std::clamp(currentMajor, 1, std::max(1, anchorMajor));
    }

    WindowPlacement dragPreviewFromAnchor(const WindowPlacement& anchor, int mode) const {
        WindowPlacement preview = anchor;
        const int previewMajor = dragPreviewMajorPixels(anchor);
        if (isVerticalTaskbar()) {
            preview.height = previewMajor;
            if (mode == 0)
                preview.y = anchor.y + anchor.height - preview.height;
        } else {
            preview.width = previewMajor;
            if (mode == 0)
                preview.x = anchor.x + anchor.width - preview.width;
        }
        return preview;
    }

    bool dragPreviewPlacementAt(POINT cursorScreen, WindowPlacement& placement, int& mode) {
        bool found = false;
        long long bestDistance = 0;
        const int cursorMajor = isVerticalTaskbar() ? cursorScreen.y : cursorScreen.x;
        for (int candidateMode = 0; candidateMode <= 1; ++candidateMode) {
            WindowPlacement anchor;
            const TaskbarPlacementStatus status =
                calculateWindowPlacement(anchor, candidateMode, false);
            if (!usableDragPlacement(status))
                continue;

            const WindowPlacement preview = dragPreviewFromAnchor(anchor, candidateMode);
            const int previewStart = isVerticalTaskbar() ? preview.y : preview.x;
            const int previewLength = isVerticalTaskbar() ? preview.height : preview.width;
            const long long distance =
                std::llabs(static_cast<long long>(cursorMajor) * 2 -
                           (static_cast<long long>(previewStart) * 2 + previewLength));
            if (!found || distance < bestDistance ||
                (distance == bestDistance && candidateMode == positionMode_)) {
                found = true;
                bestDistance = distance;
                placement = preview;
                mode = candidateMode;
            }
        }
        return found;
    }

    bool updateDragPreviewPlacement() {
        if (!lyricDragging_)
            return false;
        WindowPlacement placement;
        int mode = positionMode_;
        if (!dragPreviewPlacementAt(dragCursorScreen_, placement, mode))
            return false;

        dragCandidateMode_ = mode;
        WindowPlacement current;
        if (currentWindowPlacement(current) && current.x == placement.x &&
            current.y == placement.y && current.width == placement.width &&
            current.height == placement.height)
            return false;
        applyWindowPlacement(placement, true, true);
        return true;
    }

    void beginLyricDrag() {
        if (isExpandedView() || lyricDragging_)
            return;
        WindowPlacement current;
        // 预览表达的是用户此刻看到的整个任务栏歌词宿主，而非单独歌词文本：
        // 当前 HWND 主轴尺寸已包含封面、歌曲信息、歌词区和独立频谱。
        dragPreviewMajorPx_ = currentWindowPlacement(current)
                                  ? (isVerticalTaskbar() ? current.height : current.width)
                                  : 0;
        lyricDragging_ = true;
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        dragCandidateMode_ = positionMode_;
        cancelSceneWindowResize();
        setSongTransitionPending(false);
        renderer.resetRoot();
        clearImmersiveSongContentTransition();
        renderer.clearLyricTransitionLayers();
        clearLyricDCompState();
        mouseOver_ = false;
        trackingLeave_ = false;
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.hide();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        updateDragPreviewPlacement();
        requestFrameAndFlush();
    }

    void finishLyricDrag(bool commit) {
        const bool wasDragging = lyricDragging_;
        const int committedMode = dragCandidateMode_;
        dragPress_ = false;
        lyricDragging_ = false;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));

        if (!wasDragging)
            return;
        trackingLeave_ = false;
        mouseOver_ = false;
        immersiveControlHover_ = -1;
        if (commit)
            positionMode_ = committedMode;
        requestInvalidation(RenderInvalidation::Layout);
        adjustPosition();
        requestFrameAndFlush();
        if (commit && onPositionModeChanged)
            onPositionModeChanged(positionMode_);
        dragPreviewMajorPx_ = 0;
    }

    bool detectChanges() {
        if (!taskbar_ || !IsWindow(taskbar_))
            return false;

        RECT rcTaskbar{}, rcNotify{};
        GetWindowRect(taskbar_, &rcTaskbar);
        if (notify_)
            GetWindowRect(notify_, &rcNotify);
        UINT dpi = GetDpiForWindow(taskbar_);
        bool center = isTaskbarCenterAlign();
        bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        const UINT edge = queryTaskbarEdge(taskbar_, rcTaskbar);
        const bool edgeChanged = edge != taskbarEdge_;
        bool themeChanged = light != lightTheme_;

        RECT rcStart{};
        if (start_)
            GetWindowRect(start_, &rcStart);

        // TrafficMonitor / 酷狗 / UIA 按钮矩形由探测工作线程提供（pickProbeResult），
        // 这里只做非阻塞检查，UI 线程不允许出现阻塞型跨进程调用
        bool changed = dpi != dpi_ || center != centerAlign_ || edgeChanged ||
                       themeChanged ||
                       !EqualRect(&rcTaskbar, &rcTaskbar_) ||
                       !EqualRect(&rcNotify, &rcNotify_) ||
                       !EqualRect(&rcStart, &rcStart_);
        if (changed) {
            // 封面位图按显示尺寸解码（decodeCover），DPI/任务栏厚度或方向变化时按新尺寸重解码
            if (dpi != dpi_ || edge != taskbarEdge_ ||
                taskbarCrossPixels(rcTaskbar, edge) != taskbarCrossPixels(rcTaskbar_, taskbarEdge_))
                requestInvalidation(RenderInvalidation::Cover);
            dpi_ = dpi;
            centerAlign_ = center;
            taskbarEdge_ = edge;
            if (edgeChanged)
                renderer.resetRoot();
            if (edgeChanged) {
                // 左右侧的竖排文字方向相反；任务栏方向变化时必须重建逐字布局，
                // 同时丢弃沿用自另一方向的滚动偏移。
                requestInvalidation(RenderInvalidation::Text);
                lyricScrollOffset_ = 0.0f;
            }
            rcTaskbar_ = rcTaskbar;
            rcNotify_ = rcNotify;
            rcStart_ = rcStart;
            renderer.setDpi(dpi_);
            requestInvalidation(RenderInvalidation::Layout);
            if (themeChanged) {
                lightTheme_ = light;
                updateBackdropSolidColor();
                discardDeviceResources();
            }
        }
        return changed;
    }

    // ---------- 资源 ----------

    void createDeviceResources() {
        if (brushBg_) {
            renderState_.setDeviceResourcePhase(RenderState::DeviceResourcePhase::Ready);
            return;
        }
        renderer.initialize();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);

        // 根据任务栏深浅主题选择配色，贴近 Windows 11 原生媒体控件
        // 背景使用极低的 alpha，视觉上透明但保证分层窗口命中测试覆盖整个区域
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.01f), &brushBg_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f), &brushHover_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.95f), &brushText_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.30f, 0.30f, 0.75f), &brushDim_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.90f), &brushBtn_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.30f), &brushBtnDisabled_);
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.01f), &brushBg_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f), &brushHover_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.95f), &brushText_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.65f), &brushDim_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.90f), &brushBtn_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.35f), &brushBtnDisabled_);
        }
        // 歌词画刷与主题无关，单独创建（用户可换色，换色时只重建这四个）
        createLyricBrushes();
        // 进度背景颜色每帧经 SetColor 写入，这里只建空画刷
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &brushProgressBg_);
        // 纯色背景/封面模糊遮罩同样每帧 SetColor
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &brushBackground_);
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.32f, 0.15f, 1.0f),
                                      &brushIdleWarm_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.40f, 0.64f, 1.0f),
                                      &brushIdleCool_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.47f, 0.34f, 1.0f),
                                      &brushIdleAccent_);
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 0.56f, 0.24f, 1.0f),
                                      &brushIdleWarm_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.56f, 0.80f, 1.00f, 1.0f),
                                      &brushIdleCool_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.95f, 0.70f, 1.0f),
                                      &brushIdleAccent_);
        }
        rt->CreateLayer(&coverLayer_);
        rt->CreateLayer(&lyricEdgeFadeLayer_);
        rt->CreateLayer(&lyricRightFadeLayer_);
        recreateFormats();
        if (auto* dwrite = renderer.dwrite()) {
            dwrite->CreateTextFormat(
                fluent::uiFontFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn",
                &fmtDragPreview_);
            if (fmtDragPreview_) {
                fmtDragPreview_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmtDragPreview_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                fmtDragPreview_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                fluent::applyUiFontFallback(fmtDragPreview_);
            }

        }
        if (auto* factory = renderer.d2d()) {
            D2D1_STROKE_STYLE_PROPERTIES props{};
            props.startCap = D2D1_CAP_STYLE_FLAT;
            props.endCap = D2D1_CAP_STYLE_FLAT;
            props.dashCap = D2D1_CAP_STYLE_FLAT;
            props.lineJoin = D2D1_LINE_JOIN_ROUND;
            props.miterLimit = 10.0f;
            props.dashStyle = D2D1_DASH_STYLE_DASH;
            props.dashOffset = 0.0f;
            factory->CreateStrokeStyle(props, nullptr, 0, &dragPreviewStroke_);
        }
        renderState_.setDeviceResourcePhase(
            brushBg_ ? RenderState::DeviceResourcePhase::Ready
                     : RenderState::DeviceResourcePhase::Uninitialized);
    }

    // 黑胶效果画刷：光环/纹理沿用当前已播放色，保证专辑取色后两处同步变化。
    void createAlbumCoverBrushes() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        auto release = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        release(brushCoverHalo_);
        release(brushVinylBase_);
        release(brushVinylGroove_);
        auto rgb = [](COLORREF c, float a) {
            return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                                GetBValue(c) / 255.0f, a);
        };
        rt->CreateSolidColorBrush(rgb(lyricColor_, 0.42f), &brushCoverHalo_);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.018f, 0.018f, 0.022f, 0.96f),
                                  &brushVinylBase_);
        rt->CreateSolidColorBrush(rgb(lyricColor_, 0.22f), &brushVinylGroove_);
    }

    void releaseAlbumCoverEffectResources() {
        auto release = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        release(brushCoverHalo_);
        release(brushVinylBase_);
        release(brushVinylGroove_);
        release(vinylCoverClip_);
    }

    // 歌词与黑胶画刷：随用户颜色重建，与主题画刷解耦
    void createLyricBrushes() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        ++textFxGen_; // 颜色变化使离屏缓存失效
        if (brushLyric_) {
            brushLyric_->Release();
            brushLyric_ = nullptr;
        }
        if (brushLyricDim_) {
            brushLyricDim_->Release();
            brushLyricDim_ = nullptr;
        }
        if (brushLyricGlow_) {
            brushLyricGlow_->Release();
            brushLyricGlow_ = nullptr;
        }
        if (brushLyricOutline_) {
            brushLyricOutline_->Release();
            brushLyricOutline_ = nullptr;
        }
        if (brushSpectrum_) {
            brushSpectrum_->Release();
            brushSpectrum_ = nullptr;
        }
        if (brushSpectrumBarGradient_) {
            brushSpectrumBarGradient_->Release();
            brushSpectrumBarGradient_ = nullptr;
        }
        auto rgb = [](COLORREF c, float a) {
            return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                                GetBValue(c) / 255.0f, a);
        };
        rt->CreateSolidColorBrush(rgb(lyricColor_, 1.00f), &brushLyric_);
        rt->CreateSolidColorBrush(rgb(lyricUnplayedColor_, lyricUnplayedAlphaPct_ / 100.0f),
                                  &brushLyricDim_);
        rt->CreateSolidColorBrush(rgb(lyricGlowColor_, 0.28f), &brushLyricGlow_);
        rt->CreateSolidColorBrush(rgb(lyricOutlineColor_, 0.50f), &brushLyricOutline_);
        // 频谱颜色模式独立于歌词颜色设置：默认跟随已播放色，也可跟随专辑主题色或使用自定义色。
        // 尚未提取到当前专辑主色时，专辑模式沿用歌词模式的当前有效已播放色。
        const COLORREF spectrumBaseColor =
            spectrumCustomColor_
                ? spectrumColor_
                : spectrumFollowAlbum_ && spectrumAlbumColorAvailable_ ? spectrumAlbumColor_
                                                                         : lyricColor_;
        const D2D1_COLOR_F spectrumBase = rgb(spectrumBaseColor, 0.60f);
        rt->CreateSolidColorBrush(spectrumBase, &brushSpectrum_);

        // 柱状频谱开启渐变时，沿每根柱子的高度固定分成三段：下深、中间基色、上浅。
        const D2D1_COLOR_F spectrumDark = D2D1::ColorF(
            spectrumBase.r * kSpectrumBarGradientDarkFactor,
            spectrumBase.g * kSpectrumBarGradientDarkFactor,
            spectrumBase.b * kSpectrumBarGradientDarkFactor, spectrumBase.a);
        const D2D1_COLOR_F spectrumLight = D2D1::ColorF(
            spectrumBase.r + (1.0f - spectrumBase.r) * kSpectrumBarGradientLightMix,
            spectrumBase.g + (1.0f - spectrumBase.g) * kSpectrumBarGradientLightMix,
            spectrumBase.b + (1.0f - spectrumBase.b) * kSpectrumBarGradientLightMix,
            spectrumBase.a);
        const D2D1_GRADIENT_STOP barGradientStops[] = {
            {0.000f, spectrumDark},
            {0.333f, spectrumDark},
            {0.334f, spectrumBase},
            {0.666f, spectrumBase},
            {0.667f, spectrumLight},
            {1.000f, spectrumLight},
        };
        ID2D1GradientStopCollection* barGradientStopCollection = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(
                barGradientStops, _countof(barGradientStops), &barGradientStopCollection)) &&
            barGradientStopCollection) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 1.0f),
                                                     D2D1::Point2F(0.0f, 0.0f)),
                barGradientStopCollection, &brushSpectrumBarGradient_);
            barGradientStopCollection->Release();
        }
        if (albumCoverEffect_ == AlbumCoverEffect::Vinyl)
            createAlbumCoverBrushes();
        else
            releaseAlbumCoverEffectResources();
    }

    void setFontGlowColors(COLORREF glow, COLORREF outline) {
        if (lyricGlowColor_ == glow && lyricOutlineColor_ == outline)
            return;
        lyricGlowColor_ = glow;
        lyricOutlineColor_ = outline;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) {
        if (lyricColor_ == played && lyricUnplayedColor_ == unplayed &&
            lyricUnplayedAlphaPct_ == unplayedAlphaPct)
            return;
        lyricColor_ = played;
        lyricUnplayedColor_ = unplayed;
        lyricUnplayedAlphaPct_ = unplayedAlphaPct;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setFontGlow(bool on) {
        if (lyricGlow_ == on)
            return;
        lyricGlow_ = on;
        ++textFxGen_; // 效果组合变化，不能复用旧的离屏缓存
        requestFrameAndFlush();
    }

    void setFontOutline(bool on) {
        if (lyricOutline_ == on)
            return;
        lyricOutline_ = on;
        ++textFxGen_; // 效果组合变化，不能复用旧的离屏缓存
        requestFrameAndFlush();
    }

    void setSecondaryLyricMode(bool translation, bool romanization) {
        // 调用方保证互斥，这里再收紧一次，避免异常状态绘制三行。
        if (translation && romanization)
            romanization = false;
        if (translationEnabled_ == translation && romanizationEnabled_ == romanization)
            return;
        translationEnabled_ = translation;
        romanizationEnabled_ = romanization;
        resetLyricTransition();
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void setDoubleLineLyrics(bool on) {
        if (doubleLineLyricsEnabled_ == on)
            return;
        doubleLineLyricsEnabled_ = on;
        resetLyricTransition();
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void setLyricAlignment(LyricAlignment alignment) {
        if (lyricAlignment_ == alignment)
            return;
        lyricAlignment_ = alignment;
        lyricScrollOffset_ = 0.0f;
        secondaryScrollOffset_ = 0.0f;
        requestFrameAndFlush();
    }

    void setIdleQuoteAlignment(LyricAlignment alignment) {
        if (idleQuoteAlignment_ == alignment)
            return;
        idleQuoteAlignment_ = alignment;
        lyricScrollOffset_ = 0.0f;
        secondaryScrollOffset_ = 0.0f;
        requestFrameAndFlush();
    }

    void setIdleQuoteBackground(IdleQuoteBackground background) {
        if (idleQuoteBackground_ == background)
            return;
        idleQuoteBackground_ = background;
        refreshFrameTimer();
        requestFrameAndFlush();
    }

    void setIdleQuoteBackgroundScope(IdleQuoteBackgroundScope scope) {
        if (idleQuoteBackgroundScope_ == scope)
            return;
        idleQuoteBackgroundScope_ = scope;
        refreshFrameTimer();
        requestFrameAndFlush();
    }

    bool isMinimalMode() const {
        return isRenderMode(RenderMode::Minimal);
    }

    bool mediaPopupAvailable(bool sessionVisible) const {
        return sessionVisible && !isStoppedMode() && !isMinimalMode();
    }

    static bool isTaskbarMediaScene(DisplayScene scene) {
        return scene == DisplayScene::Searching || scene == DisplayScene::Lyrics ||
               scene == DisplayScene::Spectrum || scene == DisplayScene::Message;
    }

    static bool isTaskbarSceneTransition(DisplayScene from, DisplayScene to) {
        return (from == DisplayScene::Idle && isTaskbarMediaScene(to)) ||
               (isTaskbarMediaScene(from) && to == DisplayScene::Idle);
    }

    bool mediaPopupEnabledForScene() const {
        if (isExpandedView() || isMinimalMode() || isStoppedMode())
            return false;
        if (scene_ == DisplayScene::Idle)
            return isSessionVisible() && idle.quickStartEnabled;
        if (hoverControlStyle_ == HoverControlStyle::Popup)
            return controlsOnHover_;
        return isSessionVisible() && idle.quickStartEnabled;
    }

    void syncMediaPopupEnabled() {
        const bool enabled = mediaPopupEnabledForScene();
        const bool wasEnabled = mediaPopupEnabled_;
        mediaPopupEnabled_ = enabled;
        mediaPopup.setEnabled(enabled);
        if (enabled && !wasEnabled) {
            // 弹窗样式可能在媒体会话已经存在时才开启；补送当前完整快照，
            // 让弹窗的可用状态和展示类别不依赖下一次 SMTC 事件。
            const bool available = mediaPopupAvailable(isSessionVisible());
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setIdleContent(idle, available);
            mediaPopup.setPresentationMode(scene_, available,
                                            hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.setMedia(media, available);
            mediaPopup.endPresentationUpdate();
        } else if (enabled) {
            const bool available = mediaPopupAvailable(isSessionVisible());
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setPresentationMode(scene_, available,
                                            hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.endPresentationUpdate();
        }
        mediaPopup.setTriggerOnHover(floatingCardTrigger_ == MediaPopupTrigger::Hover);
        if (enabled && mouseOver_)
            mediaPopup.onAnchorEnter();
    }

    void releaseCoverBackgroundResources() {
        if (coverBlurFx_) {
            coverBlurFx_->Release();
            coverBlurFx_ = nullptr;
        }
        if (coverScaleFx_) {
            coverScaleFx_->Release();
            coverScaleFx_ = nullptr;
        }
        coverBlurInput_ = nullptr;
    }

    void setControlsOnHover(bool on) {
        if (controlsOnHover_ == on)
            return;
        controlsOnHover_ = on;
        volumeHover_ = false;
        volumePopup_.hide();
        syncMediaPopupEnabled();
        requestFrameAndFlush();
    }

    void setHoverControlStyle(HoverControlStyle style) {
        if (hoverControlStyle_ == style)
            return;
        hoverControlStyle_ = style;
        volumeHover_ = false;
        volumePopup_.hide();
        syncMediaPopupEnabled();
        requestFrameAndFlush();
    }

    void setFloatingCardTrigger(MediaPopupTrigger trigger) {
        if (floatingCardTrigger_ == trigger)
            return;
        floatingCardTrigger_ = trigger;
        syncMediaPopupEnabled();
    }

    void setFloatingCardBackground(MediaPopupBackground mode) {
        mediaPopup.setBackgroundMode(mode);
    }

    void setFloatingCardBackgroundColor(COLORREF color, bool customized) {
        mediaPopup.setBackgroundColor(color, customized);
    }

    void setFloatingCardFollowAlbum(bool on) {
        mediaPopup.setFollowAlbumBackground(on);
    }

    void setFloatingCardAutoTextContrast(bool on) {
        mediaPopup.setAutoTextContrast(on);
    }

    bool backgroundWaveEnabled() const {
        return spectrumBackground_ && spectrumStyle_ == SpectrumStyle::DreamyWave;
    }

    void setSpectrumStyle(SpectrumStyle style) {
        if (spectrumStyle_ == style)
            return;
        const bool wasBackground = backgroundWaveEnabled();
        spectrumStyle_ = style;
        if (spectrumVisible_ && wasBackground != backgroundWaveEnabled())
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumColor(COLORREF color, bool customized, bool followAlbum) {
        if (spectrumColor_ == color && spectrumCustomColor_ == customized &&
            spectrumFollowAlbum_ == followAlbum)
            return;
        spectrumColor_ = color;
        spectrumCustomColor_ = customized;
        spectrumFollowAlbum_ = followAlbum && !customized;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setSpectrumAlbumColor(COLORREF color, bool available) {
        if (spectrumAlbumColor_ == color && spectrumAlbumColorAvailable_ == available)
            return;
        spectrumAlbumColor_ = color;
        spectrumAlbumColorAvailable_ = available;
        if (spectrumFollowAlbum_ && !spectrumCustomColor_)
            createLyricBrushes();
        requestFrameAndFlush();
    }

    void setSpectrumGradient(bool on) {
        if (spectrumGradient_ == on)
            return;
        spectrumGradient_ = on;
        requestFrameAndFlush();
    }

    void setSpectrumBackground(bool on) {
        if (spectrumBackground_ == on)
            return;
        const bool wasBackground = backgroundWaveEnabled();
        spectrumBackground_ = on;
        if (spectrumVisible_ && wasBackground != backgroundWaveEnabled())
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (spectrumOpacityPct_ == next)
            return;
        spectrumOpacityPct_ = next;
        if (backgroundWaveEnabled())
            requestFrameAndFlush();
    }

    // 进度背景实际生效条件：用户开启 && 背景波浪未占用背景 && 当前歌曲有时长
    bool progressBackgroundActive() const {
        return progressBackground_ && !backgroundWaveEnabled() && media.durationMs > 0;
    }

    void setProgressBackground(bool on) {
        if (progressBackground_ == on)
            return;
        progressBackground_ = on;
        requestFrameAndFlush();
    }

    void setProgressBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (progressBackgroundOpacityPct_ == next)
            return;
        progressBackgroundOpacityPct_ = next;
        if (progressBackgroundActive())
            requestFrameAndFlush();
    }

    void setBackground(TaskbarBackground mode) {
        if (background_ == mode)
            return;
        background_ = mode;
        if (mode != TaskbarBackground::CoverBlur)
            releaseCoverBackgroundResources();
        requestFrameAndFlush();
    }

    void setCoverBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (coverBackgroundOpacityPct_ == next)
            return;
        coverBackgroundOpacityPct_ = next;
        if (background_ == TaskbarBackground::CoverBlur)
            requestFrameAndFlush();
    }

    void setViewMode(TaskbarViewMode mode) {
        if (viewMode_ == mode)
            return;

        const TaskbarViewMode previousMode = viewMode_;
        if (hwnd && IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_HIDE);
        const bool hadImmersivePress = immersiveControlPressed_ >= 0;
        if (dragPress_ || lyricDragging_)
            finishLyricDrag(false);
        immersiveControlPressed_ = -1;
        if (hadImmersivePress && GetCapture() == hwnd)
            ReleaseCapture();
        renderer.resetRoot();
        clearImmersiveSongContentTransition();
        if (previousMode == TaskbarViewMode::AppBar)
            unregisterAppBar();
        if (previousMode == TaskbarViewMode::AppBar)
            stopResourceMonitoring();
        viewMode_ = mode;
        bool immersiveBackdropPrepared = false;
        if (mode == TaskbarViewMode::Immersive && !dockBackdrop_.available()) {
            if (!taskbarEmbedded_ || detachFromTaskbar())
                initializeBackdropForCurrentViewMode();
            else
                backgroundBlurSupported_ = false;
            immersiveBackdropPrepared = true;
        }
        dockPet_.setMode(DockPetMode::Hidden, monotonicNowMs());
        dockResourcePage_ = 0;
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.hide();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        syncMediaPopupEnabled();
        cancelSceneWindowResize();
        if (mode == TaskbarViewMode::AppBar) {
            detachFromTaskbar();
            renderState_.setVisibilitySuppressed(false);
            stopPlacementTimer();
            setPlacementStatus(TaskbarPlacementStatus::Safe);
            if (!isStoppedMode())
                startResourceMonitoring();
        } else {
            findTaskbar();
            if (!attachToTaskbar(hwnd))
                scheduleTaskbarAttachRetry();
        }
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        // 沉浸模式仍需要完成首次真实探测，才能解除创建阶段的显示抑制；
        // 探测完成且窗口已可见后才停止避让定时器。否则重启时会一直隐藏。
        if (mode == TaskbarViewMode::Immersive && probeReady_ &&
            !renderState_.visibilitySuppressed())
            stopPlacementTimer();
        else if (mode != TaskbarViewMode::AppBar && !isStoppedMode())
            startPlacementTimer();
        requestInvalidation(toMask(RenderInvalidation::Layout) |
                            toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        adjustPosition();
        reconcileWindowVisibility();
        requestFrameAndFlush();
    }

    void setAppBarEdge(AppBarEdge edge) {
        if (appBarEdge_ == edge)
            return;
        appBarEdge_ = edge;
        if (isAppBarView() && appBarRegistered_)
            updateAppBarPosition();
        requestInvalidation(RenderInvalidation::Layout);
        requestFrameAndFlush();
    }

    void setImmersiveBackgroundBlur(int percent) {
        const int nextBlur = std::clamp(percent, 0, 100);
        if (immersiveBackgroundBlurPct_ == nextBlur)
            return;
        immersiveBackgroundBlurPct_ = nextBlur;
        if (viewMode_ == TaskbarViewMode::Immersive) {
            dockBackdrop_.setBlurPercent(immersiveBackgroundBlurPct_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrame();
        }
    }

    void setDockBackgroundBlur(int percent) {
        const int nextBlur = std::clamp(percent, 0, 100);
        if (dockBackgroundBlurPct_ == nextBlur)
            return;
        dockBackgroundBlurPct_ = nextBlur;
        if (viewMode_ == TaskbarViewMode::AppBar) {
            dockBackdrop_.setBlurPercent(dockBackgroundBlurPct_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrame();
        }
    }

    void setImmersiveBackgroundAdjustment(int mode) {
        const int nextMode = std::clamp(mode, 0, 1);
        if (immersiveBackgroundAdjustment_ == nextMode)
            return;
        immersiveBackgroundAdjustment_ = nextMode;
        if (viewMode_ == TaskbarViewMode::Immersive) {
            updateBackdropSolidColor();
            dockBackdrop_.setAdjustmentMode(immersiveBackgroundAdjustment_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrameAndFlush();
        }
    }

    void setDockBackgroundAdjustment(int mode) {
        const int nextMode = std::clamp(mode, 0, 1);
        if (dockBackgroundAdjustment_ == nextMode)
            return;
        dockBackgroundAdjustment_ = nextMode;
        if (viewMode_ == TaskbarViewMode::AppBar) {
            updateBackdropSolidColor();
            dockBackdrop_.setAdjustmentMode(dockBackgroundAdjustment_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrameAndFlush();
        }
    }

    bool backgroundBlurAvailable() const { return backgroundBlurSupported_; }

    void updateBackdropSolidColor() {
        if (viewMode_ == TaskbarViewMode::Embedded)
            return;
        const COLORREF color = fluent::isWindowsAppDarkMode() ? RGB(32, 32, 32)
                                                              : RGB(243, 243, 243);
        dockBackdrop_.setSolidColor(color);
        if (!dockBackdrop_.available())
            backgroundBlurSupported_ = false;
    }

    void initializeBackdropForCurrentViewMode() {
        if (viewMode_ == TaskbarViewMode::Embedded) {
            dockBackdrop_.setVisible(false);
            backgroundBlurSupported_ = true;
            return;
        }

        if (dockBackdrop_.available()) {
            backgroundBlurSupported_ = true;
            runtime_log::writef(
                L"[dock-backdrop] init result=reused view=%s hwnd=%p",
                viewMode_ == TaskbarViewMode::Immersive ? L"immersive" : L"dock", hwnd);
        } else {
            backgroundBlurSupported_ = dockBackdrop_.initialize(hwnd);
        }
        updateBackdropSolidColor();
        dockBackdrop_.setAdjustmentMode(viewMode_ == TaskbarViewMode::Immersive
                                            ? immersiveBackgroundAdjustment_
                                            : dockBackgroundAdjustment_);
        dockBackdrop_.setBlurPercent(viewMode_ == TaskbarViewMode::Immersive
                                         ? immersiveBackgroundBlurPct_
                                         : dockBackgroundBlurPct_);
        dockBackdrop_.setVisible(true);
        if (!dockBackdrop_.available())
            backgroundBlurSupported_ = false;
    }

    void setDockResourceVisibility(const DockResourceVisibility& visibility) {
        if (dockResourceVisibility_.gpuUsage == visibility.gpuUsage &&
            dockResourceVisibility_.cpuFrequency == visibility.cpuFrequency)
            return;
        dockResourceVisibility_ = visibility;
        dockResourcePage_ = 0;
        requestInvalidation(toMask(RenderInvalidation::Layout) |
                            toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        if (isAppBarView())
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumVisible(bool on) {
        if (spectrumVisible_ == on) {
            if (!on)
                spectrumBands_.fill(0.0f);
            return;
        }
        spectrumVisible_ = on;
        if (!on)
            spectrumBands_.fill(0.0f);
        // 先改窗口宽度再渲染：若在 render 内经 Layout 失效改大小，
        // render 结尾的 present 会用旧尺寸位图把窗口尺寸拽回去（与 setPositionMode 同序）
        adjustPosition();
        requestFrameAndFlush();
    }

    void setSongInfoVisible(bool on) {
        if (songInfoVisible_ == on)
            return;
        songInfoVisible_ = on;
        titleScrollOffset_ = 0.0f;
        artistScrollOffset_ = 0.0f;
        requestInvalidation(RenderInvalidation::SongInfo);
        adjustPosition();
        requestFrameAndFlush();
    }

    void setAlbumCoverVisible(bool on) {
        if (albumCoverVisible_ == on)
            return;
        albumCoverVisible_ = on;
        if (!on) {
            requestInvalidation(RenderInvalidation::PlatformIcon);
            if (platformIconBmp) {
                platformIconBmp->Release();
                platformIconBmp = nullptr;
            }
        } else if (platformIconVisible_) {
            requestInvalidation(RenderInvalidation::PlatformIcon);
        }
        requestInvalidation(RenderInvalidation::Text);
        adjustPosition();
        requestFrameAndFlush();
    }

    void setPlatformIconVisible(bool on) {
        if (platformIconVisible_ == on)
            return;
        platformIconVisible_ = on;
        requestInvalidation(RenderInvalidation::PlatformIcon);
        requestFrameAndFlush();
    }

    void setAlbumCoverEffect(AlbumCoverEffect effect) {
        if (albumCoverEffect_ == effect)
            return;
        albumCoverEffect_ = effect;
        vinylAngleDeg_ = 0.0f;
        vinylTickMs_ = monotonicNowMs();
        requestInvalidation(RenderInvalidation::Geometry);
        if (effect == AlbumCoverEffect::Vinyl)
            createAlbumCoverBrushes();
        else
            releaseAlbumCoverEffectResources();
        requestFrameAndFlush();
    }

    void setSpectrumBands(const std::array<float, TaskbarHost::kSpectrumBands>& bands) {
        spectrumBands_ = bands;
    }

    void applyPlaybackPatch(const PlaybackPatch& patch) {
        // 高频补丁必须属于当前已经应用的完整帧；旧曲目或旧帧的位置不能回写。
        if (patch.frameRevision != frameRevision_ ||
            patch.requestGeneration != requestGeneration_)
            return;

        if (patch.actualPositionMs != positionMs_) {
            positionMs_ = patch.actualPositionMs;
            if (!patch.playing)
                karaokeSettled_ = false; // 暂停中 seek：逐字高亮需要重新收敛
        }
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setProgress(patch.actualPositionMs);
        const bool playingChanged = media.playing != patch.playing;
        if (playingChanged) {
            media.playing = patch.playing;
            vinylTickMs_ = monotonicNowMs();
            mediaPopup.setMedia(media, mediaPopupAvailable(isSessionVisible()));
            mediaPopup.setPresentationMode(
                scene_, mediaPopupAvailable(isSessionVisible()),
                hoverControlStyle_ == HoverControlStyle::Inline);
        }
        mediaPopup.endPresentationUpdate();
        if (playingChanged) {
            // 悬浮控制按钮的播放/暂停图标随状态变化，直接提交一帧；弹窗自身
            // 的完整展示帧已在上面的批量更新中提交。
            // 这里仅保留宿主歌词的刷新，不再重复触发弹窗绘制。
            requestFrameAndFlush();
        }

        if (patch.currentLine == currentLine)
            return;

        if (lyricTransitionKind_ == LyricTransitionKind::Scene &&
            isLyricTransitionInProgress()) {
            // 场景翻页期间不重排目标层，先记下最新歌词行，待翻页完成后
            // 统一提交，避免目标层重建把上下翻页打断成瞬移。
            currentLine = patch.currentLine;
            sceneTransitionNeedsRelayout_ = true;
            lyricTransitionRevision_ = frameRevision_;
            return;
        }

        onLyricLineTargetChanged(patch.currentLine, patch.actualPositionMs, frameRevision_,
                                 true);
    }

    void applySpectrumPatch(const SpectrumPatch& patch) {
        if (patch.frameRevision != frameRevision_ ||
            patch.requestGeneration != requestGeneration_)
            return;
        spectrumBands_ = patch.bands;
    }

    void applyPresentationFrame(const PresentationFrame& frame) {
        // 完整帧只允许按版本向前提交；高频播放补丁不改变这个版本边界。
        if (frame.frameRevision < frameRevision_ ||
            (frame.frameRevision == frameRevision_ && frame.frameRevision != 0 &&
             frame.trackKey != trackKey_))
            return;

        const bool trackChanged = frame.trackKey != trackKey_;
        const bool lyricsChanged = !sameLyrics(lines, frame.lyrics);
        const bool lineChanged = frame.currentLine != currentLine;
        const bool statusChanged = frame.statusText != statusText;
        const bool statusOneShotChanged = frame.statusTextOneShot != statusTextOneShot_;
        const bool sceneChanged = frame.scene != scene_;
        const bool sceneWidthChanged = sceneWidthPolicyDiffers(scene_, frame.scene);
        const bool idleChanged = frame.idle.sentence != idle.sentence ||
                                 frame.idle.source != idle.source ||
                                 frame.idle.loading != idle.loading ||
                                 frame.idle.showQuote != idle.showQuote ||
                                 frame.idle.copyEnabled != idle.copyEnabled ||
                                 frame.idle.quickStartEnabled != idle.quickStartEnabled ||
                                 frame.idle.apps.size() != idle.apps.size();
        const bool wasVisible = isWindowVisible();
        const bool shouldAnimateScene =
            isWindowVisible() && frame.visible && clientAnimations_ && !isMinimalMode() &&
            !isStoppedMode() &&
            isTaskbarSceneTransition(scene_, frame.scene) && lyricLayout_;
        const bool continueSceneTransition =
            sceneChanged && lyricTransitionKind_ == LyricTransitionKind::Scene &&
            isLyricTransitionInProgress() &&
            isTaskbarMediaScene(scene_) && isTaskbarMediaScene(frame.scene);
        const bool mediaIdentityChanged =
            frame.media.title != media.title || frame.media.artist != media.artist ||
            frame.media.sourceAppUserModelId != media.sourceAppUserModelId ||
            frame.media.thumbnail != media.thumbnail;
        // QQ 的 trackKey 包含时长；同一首歌从未知时长补齐到有效时长时，
        // 即使封面、专辑字段也在同一批事件中更新，也不能启动整卡切歌动画。
        const bool durationOnlyMediaUpdate =
            trackChanged && frame.durationOnlyUpdate && media.durationMs <= 0 &&
            frame.media.durationMs > 0;
        const bool confirmedDurationChange = media.durationMs > 0 &&
                                             frame.media.durationMs > 0 &&
                                             media.durationMs != frame.media.durationMs;
        const bool songChanged = trackChanged && !trackKey_.empty() && !frame.trackKey.empty() &&
                                 !durationOnlyMediaUpdate &&
                                 (mediaIdentityChanged || confirmedDurationChange);
        const bool mediaChanged = updateMediaInfo(frame.media);
        const bool popupAvailable = mediaPopupAvailable(frame.visible);
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setIdleContent(frame.idle, popupAvailable);
        mediaPopup.setMedia(frame.media, popupAvailable, songChanged);
        // 先同步完整媒体数据，再建立页面转场层，避免 Idle → Media 时目标层
        // 先绘制旧歌曲、随后才收到本帧最新标题/歌手而在转场结束时跳变。
        mediaPopup.setPresentationMode(frame.scene, popupAvailable,
                                       hoverControlStyle_ == HoverControlStyle::Inline);
        mediaPopup.setProgress(frame.actualPositionMs);
        mediaPopup.endPresentationUpdate();

        frameRevision_ = frame.frameRevision;
        requestGeneration_ = frame.requestGeneration;
        trackKey_ = frame.trackKey;
        if (sceneChanged) {
            // 类别切换拥有最高优先级：先结束可能正在进行的歌词行转场，
            // 再把旧场景布局交给新的内容块上下翻页。
            if (continueSceneTransition) {
                // Searching/Spectrum/Message 都属于播放侧内容。歌词加载完成时
                // 继续同一页翻转，不能把中间场景当成一次新的类别切换。
                sceneTransitionNeedsRelayout_ = true;
                lyricTransitionRevision_ = frame.frameRevision;
            } else {
                resetLyricTransition();
                if (shouldAnimateScene) {
                    outgoingScene_ = scene_;
                    outgoingDoubleLine_ = lyricLayoutDoubleLine_;
                    sceneTransitionFromPxW_ = lastLogicalPxW_;
                    sceneTransitionFromPxH_ = lastLogicalPxH_;
                    lyricTransitionKind_ = LyricTransitionKind::Scene;
                    lyricTransitionDirection_ = frame.scene == DisplayScene::Lyrics ? 1 : -1;
                    setLyricTransitionPending();
                    lyricTransitionRevision_ = frame.frameRevision;
                }
            }
        }
        scene_ = frame.scene;
        if (sceneWidthChanged) {
            if (shouldAnimateScene) {
                // 与内容转场使用同一段平滑时间曲线，窗口宽度/锚点连续变化，避免
                // 在转场开始或结束时一次性收窄/撑开并触发位图重绑闪烁。
                if (beginSceneWindowResize()) {
                    // 窗口尺寸动画接管本次布局失效；render() 在动画期间不再执行
                    // 一次性的 adjustPosition，成功提交后由失效快照统一确认。
                } else {
                    requestInvalidation(RenderInvalidation::Layout);
                }
            } else {
                cancelSceneWindowResize();
                requestInvalidation(RenderInvalidation::Layout);
            }
        }
        idle = frame.idle;
        if (frame.actualPositionMs != positionMs_ && !media.playing)
            karaokeSettled_ = false; // 暂停中 seek：逐字高亮需要重新收敛
        positionMs_ = frame.actualPositionMs;
        if (statusOneShotChanged || (statusChanged && frame.statusTextOneShot)) {
            statusTextOneShot_ = frame.statusTextOneShot;
            statusTextOneShotStartMs_ = statusTextOneShot_ ? monotonicNowMs() : 0;
            statusTextOneShotRounds_ = 0;
            lyricScrollOffset_ = 0.0f;
            lastTickMs_ = 0;
        }
        statusText = frame.statusText;

        // 只对已有曲目之间的切换做入场动画；首次显示、同曲刷新和会话关闭保持即时提交。
        setSongTransitionPending(songChanged && frame.visible && !isMinimalMode() &&
                                 !isStoppedMode() && clientAnimations_);

        const bool sceneTransitionInProgress =
            lyricTransitionKind_ == LyricTransitionKind::Scene &&
            isLyricTransitionInProgress();
        if (trackChanged || lyricsChanged) {
            lines = frame.lyrics;
            currentLine = frame.currentLine;
            if (!sceneTransitionInProgress) {
                resetLyricTransition();
                if (nextLyricLayout_) {
                    nextLyricLayout_->Release();
                    nextLyricLayout_ = nullptr;
                }
                nextLyricWidth_ = 0.0f;
                nextLyricHeight_ = 0.0f;
                requestInvalidation(RenderInvalidation::Text);
            } else if (!sceneChanged || continueSceneTransition) {
                sceneTransitionNeedsRelayout_ = true;
                lyricTransitionRevision_ = frame.frameRevision;
            }
        } else if (lineChanged) {
            if (sceneTransitionInProgress) {
                currentLine = frame.currentLine;
                sceneTransitionNeedsRelayout_ = true;
                lyricTransitionRevision_ = frame.frameRevision;
            } else {
                onLyricLineTargetChanged(frame.currentLine, frame.actualPositionMs,
                                         frame.frameRevision, frame.animateTransition);
            }
        } else if (isLyricTransitionInProgress()) {
            // 同一目标行的低频媒体/场景更新不应打断动画，但动画版本要跟随最新完整帧。
            lyricTransitionRevision_ = frame.frameRevision;
        }
        if (statusChanged || statusOneShotChanged || sceneChanged) {
            if (sceneTransitionInProgress && (!sceneChanged || continueSceneTransition)) {
                sceneTransitionNeedsRelayout_ = true;
                lyricTransitionRevision_ = frame.frameRevision;
            } else {
                requestInvalidation(RenderInvalidation::Text);
            }
        }

        renderState_.setSessionVisible(frame.visible);
        if (isSessionVisible() && placementStatus_ == TaskbarPlacementStatus::NoSpace)
            startPlacementTimer();
        else if (!isSessionVisible())
            stopPlacementTimer();
        reconcileWindowVisibility();
        syncMediaPopupEnabled();

        if (isWindowVisible() && (wasVisible != isWindowVisible() || trackChanged || mediaChanged || lyricsChanged ||
                        lineChanged || statusChanged || statusOneShotChanged || sceneChanged ||
                        idleChanged))
            requestFrameAndFlush();
    }

    // 频谱簇总宽（含柱间间隙）
    float spectrumClusterW() const {
        return TaskbarHost::kSpectrumBands * kSpectrumBarW +
               (TaskbarHost::kSpectrumBands - 1) * kSpectrumGap;
    }

    float spectrumVisualClusterW(int barCount) const {
        const int count = std::max(1, barCount);
        return count * kSpectrumBarW + (count - 1) * kSpectrumGap;
    }

    bool horizontalImmersiveMode() const {
        return isExpandedView() && !isVerticalTaskbar();
    }

    // 沉浸模式把频谱提升为右侧固定视觉区，长任务栏自动给它更多空间。
    float immersiveSpectrumZoneW(float hostW) const {
        if (!horizontalImmersiveMode() || hostW <= 0.0f)
            return 0.0f;
        return std::clamp(hostW * kImmersiveSpectrumZoneRatio,
                          kImmersiveSpectrumZoneMinW, kImmersiveSpectrumZoneMaxW);
    }

    float immersiveClockZoneW(float hostW) const {
        if (!horizontalImmersiveMode() || hostW <= 0.0f)
            return 0.0f;
        return std::clamp(hostW * kImmersiveClockZoneRatio,
                          kImmersiveClockZoneMinW, kImmersiveClockZoneMaxW);
    }

    bool dockResourceExtrasVisible() const {
        return dockResourceVisibility_.gpuUsage || dockResourceVisibility_.cpuFrequency;
    }

    std::vector<DockResourceRow> dockResourceExtraRows() const {
        std::vector<std::wstring> cells;
        if (dockResourceVisibility_.gpuUsage)
            cells.push_back(L"GPU: " + resourceGpuText_);
        if (dockResourceVisibility_.cpuFrequency)
            cells.push_back(L"频率: " + resourceCpuFrequencyText_);

        std::vector<DockResourceRow> rows;
        for (size_t i = 0; i < cells.size(); i += 2)
            rows.push_back({cells[i], i + 1 < cells.size() ? cells[i + 1] : L""});
        return rows;
    }

    size_t dockResourcePageCount() const {
        const auto rows = dockResourceExtraRows();
        return 1 + (rows.size() + 1) / 2;
    }

    float dockResourceZoneW(float hostW) const {
        if (!isAppBarView() || hostW <= 0.0f)
            return 0.0f;
        return std::clamp(hostW * kDockResourceZoneRatio,
                          kDockResourceZoneMinW, kDockResourceZoneMaxW);
    }

    bool hitDockResourceArea(float clientX, float clientY) const {
        if (!isAppBarView() || !horizontalImmersiveMode() || !dockResourceExtrasVisible())
            return false;

        float x = 0.0f;
        float y = 0.0f;
        clientPointToLogicalDip(clientX, clientY, x, y);
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const float width = dip(pxW);
        const float height = dip(pxH);
        const float resourceW = dockResourceZoneW(width);
        const float resourceX = width - resourceW - kDockResourceRightPadding;
        return x >= resourceX && x <= width - kDockResourceRightPadding && y >= 0.0f &&
               y <= height;
    }

    void advanceDockResourcePage(int steps) {
        const size_t pageCount = dockResourcePageCount();
        if (pageCount <= 1 || steps == 0)
            return;

        const long long count = static_cast<long long>(pageCount);
        long long next = static_cast<long long>(dockResourcePage_) + steps;
        next %= count;
        if (next < 0)
            next += count;
        dockResourcePage_ = static_cast<size_t>(next);
        requestInvalidation(toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        requestFrameAndFlush();
    }

    int immersiveSpectrumBarCount(float visualW) const {
        const int target = visualW >= kImmersiveSpectrumWideZoneThreshold
                               ? kImmersiveSpectrumWideBarCount
                               : kImmersiveSpectrumBarCount;
        const int capacity = std::max(
            1, static_cast<int>(std::floor((visualW + kSpectrumGap) /
                                           (kSpectrumBarW + kSpectrumGap))));
        return std::min(target, capacity);
    }

    float spectrumContentWForScene(DisplayScene scene, float hostW) const {
        if (scene == DisplayScene::Idle || !spectrumVisible_ || backgroundWaveEnabled())
            return 0.0f;
        return horizontalImmersiveMode() ? immersiveSpectrumZoneW(hostW) : spectrumClusterW();
    }

    // 嵌入模式只在独立频谱开启时预留 12 柱宽度；沉浸模式始终保留右侧
    // “频谱 + 时钟/资源状态”固定区，避免开关频谱时歌词安全区发生变化。
    float spectrumExtraForScene(DisplayScene scene, float hostW = -1.0f) const {
        if (hostW <= 0.0f) {
            int pxW = 0;
            int pxH = 0;
            logicalClientPixelSize(pxW, pxH);
            hostW = dip(pxW);
        }
        // 沉浸模式的最右侧始终保留给频谱和时钟；关闭频谱只隐藏其可视内容，
        // 不让歌词安全区或居中效果跟着变化。
        if (horizontalImmersiveMode()) {
            const float rightPadding = isAppBarView() ? kDockResourceRightPadding : kTextPadding;
            float extra = immersiveSpectrumZoneW(hostW) + rightPadding;
            if (isAppBarView())
                extra += dockResourceZoneW(hostW) + kDockResourceZoneGap;
            return extra;
        }
        if (scene == DisplayScene::Idle || !spectrumVisible_ || backgroundWaveEnabled())
            return 0.0f;
        return spectrumContentWForScene(scene, hostW) + kTextPadding;
    }

    bool sceneUsesCompactWidth(DisplayScene scene) const {
        return !isVerticalTaskbar() && !songInfoVisible_ && scene != DisplayScene::Idle;
    }

    bool sceneWidthPolicyDiffers(DisplayScene from, DisplayScene to) const {
        if (isVerticalTaskbar() || isExpandedView())
            return false;
        return sceneUsesCompactWidth(from) != sceneUsesCompactWidth(to) ||
               spectrumExtraForScene(from) != spectrumExtraForScene(to);
    }

    void cancelSceneWindowResize() {
        sceneResize_.reset();
    }

    bool beginSceneWindowResize() {
        if (isExpandedView())
            return false;
        WindowPlacement from;
        WindowPlacement to;
        if (!currentWindowPlacement(from))
            return false;
        const TaskbarPlacementStatus placementStatus = calculateWindowPlacement(to);
        if (placementStatus == TaskbarPlacementStatus::Unavailable ||
            placementStatus == TaskbarPlacementStatus::NoSpace)
            return false;
        if (from.x == to.x && from.y == to.y && from.width == to.width &&
            from.height == to.height) {
            cancelSceneWindowResize();
            return false;
        }

        sceneResize_ = SceneResizeAnimation{from, to, from, monotonicNowMs()};
        return true;
    }

    bool updateSceneWindowResize(ULONGLONG now) {
        if (!sceneResize_)
            return false;
        SceneResizeAnimation& resize = *sceneResize_;
        if (now < resize.startMs)
            now = resize.startMs;

        const float progress = std::clamp(
            static_cast<float>(now - resize.startMs) / kSceneTransitionMs, 0.0f,
            1.0f);
        const float t = smoothStep(progress);
        WindowPlacement placement;
        placement.x = static_cast<int>(std::lround(
            resize.from.x + (resize.to.x - resize.from.x) * t));
        placement.y = static_cast<int>(std::lround(
            resize.from.y + (resize.to.y - resize.from.y) * t));
        placement.width = static_cast<int>(std::lround(
            resize.from.width + (resize.to.width - resize.from.width) * t));
        placement.height = static_cast<int>(std::lround(
            resize.from.height + (resize.to.height - resize.from.height) * t));

        const bool changed = placement.x != resize.lastApplied.x ||
                             placement.y != resize.lastApplied.y ||
                             placement.width != resize.lastApplied.width ||
                             placement.height != resize.lastApplied.height;
        if (changed) {
            applyWindowPlacement(placement, false, true);
            resize.lastApplied = placement;
        }
        if (progress >= 1.0f) {
            sceneResize_.reset();
        }
        return true;
    }

    float spectrumLevel(int index) const {
        return std::clamp(spectrumBands_[index], 0.0f, 1.0f);
    }

    void fillSpectrumBar(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& bar) {
        if (!rt || !brushSpectrum_)
            return;
        if (spectrumGradient_ && brushSpectrumBarGradient_) {
            const float centerX = (bar.rect.left + bar.rect.right) * 0.5f;
            brushSpectrumBarGradient_->SetStartPoint(
                D2D1::Point2F(centerX, bar.rect.bottom));
            brushSpectrumBarGradient_->SetEndPoint(D2D1::Point2F(centerX, bar.rect.top));
            rt->FillRoundedRectangle(bar, brushSpectrumBarGradient_);
            return;
        }
        rt->FillRoundedRectangle(bar, brushSpectrum_);
    }

    D2D1_COLOR_F spectrumColorForLevel(float level, float alphaScale = 1.0f) const {
        const D2D1_COLOR_F base = brushSpectrum_->GetColor();
        if (!spectrumGradient_)
            return D2D1::ColorF(base.r, base.g, base.b, base.a * alphaScale);

        // 只在当前频谱基色的基础上向白色轻微提亮，波动越高提亮越多，保持同色系关系。
        const float lift = std::clamp(level, 0.0f, 1.0f) * 0.30f;
        return D2D1::ColorF(base.r + (1.0f - base.r) * lift,
                            base.g + (1.0f - base.g) * lift,
                            base.b + (1.0f - base.b) * lift,
                            base.a * alphaScale);
    }

    float spectrumVisualLevel(int index, int visualCount) const {
        if (visualCount <= 1)
            return spectrumLevel(0);
        const float position = static_cast<float>(index) /
                               static_cast<float>(visualCount - 1) *
                               static_cast<float>(TaskbarHost::kSpectrumBands - 1);
        const int leftBand = std::min(TaskbarHost::kSpectrumBands - 2,
                                      static_cast<int>(position));
        const float local = position - static_cast<float>(leftBand);
        const float smoothLocal = local * local * (3.0f - 2.0f * local);
        return spectrumLevel(leftBand) +
               (spectrumLevel(leftBand + 1) - spectrumLevel(leftBand)) * smoothLocal;
    }

    void drawDefaultSpectrum(float x, float h, int visualCount) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushSpectrum_)
            return;
        const int n = std::max(1, visualCount);
        const float cy = h * 0.5f;
        const float maxH = h * 0.74f;
        constexpr float minH = 4.0f; // 静音时也保留小柱，不消失
        for (int i = 0; i < n; ++i) {
            const float level = spectrumVisualLevel(i, n);
            const float bh = minH + level * (maxH - minH);
            D2D1_ROUNDED_RECT rr{
                D2D1::RectF(x, cy - bh * 0.5f, x + kSpectrumBarW, cy + bh * 0.5f),
                kSpectrumBarW * 0.5f, kSpectrumBarW * 0.5f};
            fillSpectrumBar(rt, rr);
            x += kSpectrumBarW + kSpectrumGap;
        }
    }

    // 新增样式：普通柱状图，柱底贴近歌词窗口下边缘，电平只向上增长。
    void drawBarSpectrum(float x, float h, int visualCount) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushSpectrum_)
            return;
        const int n = std::max(1, visualCount);
        const float baseY = h - kSpectrumBottomPadding;
        const float maxH = h * 0.82f;
        constexpr float minH = 3.0f;
        for (int i = 0; i < n; ++i) {
            const float level = spectrumVisualLevel(i, n);
            const float bh = minH + level * (maxH - minH);
            const D2D1_ROUNDED_RECT bar{
                D2D1::RectF(x, baseY - bh, x + kSpectrumBarW, baseY),
                kSpectrumBarRadius, kSpectrumBarRadius};
            fillSpectrumBar(rt, bar);
            x += kSpectrumBarW + kSpectrumGap;
        }
    }

    // 填充波浪：以 baseY 为底边，width 可覆盖独立频谱区或歌曲内容背景区。
    void drawWaveSpectrum(float x, float h, float width, float opacityScale) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushSpectrum_)
            return;

        constexpr int n = TaskbarHost::kSpectrumBands;
        constexpr int samplesPerBand = 6;
        constexpr int sampleCount = (n - 1) * samplesPerBand + 1;
        constexpr float twoPi = 6.28318530718f;
        const float baseY = h - kSpectrumBottomPadding;
        const float minH = 2.0f;
        const float maxH = std::max(minH + 1.0f, h * 0.76f);
        const float phase = clientAnimations_
                                ? static_cast<float>(frameNowMs_ % 60000ULL) / 1000.0f
                                : 0.0f;

        std::array<D2D1_POINT_2F, sampleCount> backWave{};
        std::array<D2D1_POINT_2F, sampleCount> middleWave{};
        std::array<D2D1_POINT_2F, sampleCount> frontWave{};
        for (int i = 0; i < sampleCount; ++i) {
            const float normalized = static_cast<float>(i) / (sampleCount - 1);
            const float bandPosition = normalized * static_cast<float>(n - 1);
            const int leftBand = std::min(n - 2, static_cast<int>(bandPosition));
            const float local = bandPosition - static_cast<float>(leftBand);
            const float smoothLocal = local * local * (3.0f - 2.0f * local);
            const float leftLevel = spectrumLevel(leftBand);
            const float rightLevel = spectrumLevel(leftBand + 1);
            const float level = leftLevel + (rightLevel - leftLevel) * smoothLocal;

            // 频段电平控制浪高，低频正弦只负责制造宽阔的海浪峰谷；每层使用
            // 不同波长和相位，避免三层变成同一条线的缩放副本。
            const float audio = std::clamp(level, 0.0f, 1.0f);
            const float backTide = std::clamp(
                0.5f + 0.5f * std::sin(twoPi * normalized * 0.92f + phase * 0.30f + 1.15f) +
                    0.12f * std::sin(twoPi * normalized * 0.44f + phase * 0.16f - 0.4f),
                0.0f, 1.0f);
            const float middleTide = std::clamp(
                0.5f + 0.5f * std::sin(twoPi * normalized * 1.08f + phase * 0.38f + 0.35f) +
                    0.10f * std::sin(twoPi * normalized * 0.52f + phase * 0.20f + 1.0f),
                0.0f, 1.0f);
            const float frontTide = std::clamp(
                0.5f + 0.5f * std::sin(twoPi * normalized * 1.24f + phase * 0.46f - 0.55f) +
                    0.08f * std::sin(twoPi * normalized * 0.60f + phase * 0.24f + 0.7f),
                0.0f, 1.0f);
            const float backProfile = std::clamp(0.16f + audio * 0.34f + backTide * 0.50f,
                                                 0.0f, 1.0f);
            const float middleProfile =
                std::clamp(0.10f + audio * 0.46f + middleTide * 0.42f, 0.0f, 1.0f);
            const float frontProfile =
                std::clamp(0.06f + audio * 0.62f + frontTide * 0.34f, 0.0f, 1.0f);
            const float backAmplitude = minH + backProfile * (maxH * 0.88f - minH);
            const float middleAmplitude = minH + middleProfile * (maxH * 0.76f - minH);
            const float frontAmplitude = minH + frontProfile * (maxH - minH);
            const float pointX = x + width * normalized;
            backWave[i] = D2D1::Point2F(pointX, baseY - 1.6f - backAmplitude);
            middleWave[i] = D2D1::Point2F(pointX, baseY - 0.8f - middleAmplitude);
            frontWave[i] = D2D1::Point2F(pointX, baseY - frontAmplitude);
        }

        const float originalOpacity = brushSpectrum_->GetOpacity();
        std::array<D2D1_GRADIENT_STOP, TaskbarHost::kSpectrumBands + 6> fadeStops{};
        size_t fadeStopCount = 0;
        auto addFadeStop = [&](float position, float level, float alphaScale) {
            if (fadeStopCount >= fadeStops.size())
                return;
            fadeStops[fadeStopCount++] =
                {position, spectrumColorForLevel(level, alphaScale)};
        };
        auto levelAt = [&](float normalized) {
            const float bandPosition =
                std::clamp(normalized, 0.0f, 1.0f) * static_cast<float>(n - 1);
            const int leftBand = std::min(n - 2, static_cast<int>(bandPosition));
            const float local = bandPosition - static_cast<float>(leftBand);
            const float smoothLocal = local * local * (3.0f - 2.0f * local);
            return spectrumLevel(leftBand) +
                   (spectrumLevel(leftBand + 1) - spectrumLevel(leftBand)) * smoothLocal;
        };
        if (spectrumGradient_) {
            addFadeStop(0.00f, levelAt(0.0f), 0.00f);
            addFadeStop(0.017f, levelAt(0.0f), 0.38f);
            addFadeStop(0.033f, levelAt(0.0f), 0.78f);
            for (int i = 0; i < n; ++i) {
                const float position = 0.05f + 0.90f * static_cast<float>(i) /
                                                         static_cast<float>(n - 1);
                addFadeStop(position, spectrumLevel(i), 1.0f);
            }
            addFadeStop(0.967f, levelAt(1.0f), 0.78f);
            addFadeStop(0.983f, levelAt(1.0f), 0.38f);
            addFadeStop(1.00f, levelAt(1.0f), 0.00f);
        } else {
            addFadeStop(0.00f, 0.0f, 0.00f);
            addFadeStop(0.017f, 0.0f, 0.38f);
            addFadeStop(0.033f, 0.0f, 0.78f);
            addFadeStop(0.05f, 0.0f, 1.0f);
            addFadeStop(0.95f, 0.0f, 1.0f);
            addFadeStop(0.967f, 0.0f, 0.78f);
            addFadeStop(0.983f, 0.0f, 0.38f);
            addFadeStop(1.00f, 0.0f, 0.00f);
        }
        ID2D1GradientStopCollection* fadeStopCollection = nullptr;
        ID2D1LinearGradientBrush* fadeBrush = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(fadeStops.data(),
                                                        static_cast<UINT32>(fadeStopCount),
                                                        &fadeStopCollection)) &&
            fadeStopCollection) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, 0.0f),
                                                     D2D1::Point2F(x + width, 0.0f)),
                fadeStopCollection, &fadeBrush);
        }
        if (fadeStopCollection)
            fadeStopCollection->Release();

        ID2D1Brush* waveBrush = fadeBrush ? static_cast<ID2D1Brush*>(fadeBrush)
                                          : static_cast<ID2D1Brush*>(brushSpectrum_);
        auto setWaveOpacity = [&](float opacity) {
            waveBrush->SetOpacity(originalOpacity * opacityScale * opacity);
        };
        auto drawFilledWave = [&](const auto& points, float opacity) {
            auto* d2d = renderer.d2d();
            if (!d2d)
                return;

            ID2D1PathGeometry* geometry = nullptr;
            if (FAILED(d2d->CreatePathGeometry(&geometry)) || !geometry)
                return;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED(geometry->Open(&sink)) || !sink) {
                geometry->Release();
                return;
            }

            sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < sampleCount; ++i)
                sink->AddLine(points[i]);
            sink->AddLine(D2D1::Point2F(points[sampleCount - 1].x, baseY));
            sink->AddLine(D2D1::Point2F(points[0].x, baseY));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            const HRESULT closeHr = sink->Close();
            sink->Release();
            if (SUCCEEDED(closeHr)) {
                setWaveOpacity(opacity);
                rt->FillGeometry(geometry, waveBrush);
            }
            geometry->Release();
        };
        auto drawCurve = [&](const auto& points, float opacity, float strokeWidth) {
            setWaveOpacity(opacity);
            for (int i = 1; i < sampleCount; ++i)
                rt->DrawLine(points[i - 1], points[i], waveBrush, strokeWidth);
        };

        // 先铺后景到前景的连续波面，避免波峰下方出现大片空白。
        drawFilledWave(backWave, 0.32f);
        drawFilledWave(middleWave, 0.46f);
        drawFilledWave(frontWave, 0.76f);

        // 这条线与柱状图的柱底严格共用 baseY，切换样式时视觉基准不跳动。
        setWaveOpacity(0.16f);
        rt->DrawLine(D2D1::Point2F(x, baseY), D2D1::Point2F(x + width, baseY),
                     waveBrush, 0.8f);
        drawCurve(frontWave, 0.07f, 3.8f);
        drawCurve(frontWave, 0.36f, 1.0f);
        brushSpectrum_->SetOpacity(originalOpacity);
        if (fadeBrush)
            fadeBrush->Release();
    }

    void drawDreamyWaveSpectrum(float x, float h, float width) {
        drawWaveSpectrum(x, h, width, 1.0f);
    }

    void drawBackgroundWaveSpectrum(float x, float h, float width) {
        drawWaveSpectrum(x, h, width,
                         static_cast<float>(spectrumOpacityPct_) / 100.0f);
    }

    // 默认保留原有的中线对称频谱效果；柱状图样式从窗口下边缘向上增长。
    // 沉浸模式中，柱状频谱在右侧宽频谱区内居中，梦幻波浪则铺满整个区域。
    void drawSpectrum(float x, float h, float width) {
        if (width <= 0.0f)
            return;
        switch (spectrumStyle_) {
        case SpectrumStyle::Bars:
        {
            const int visualCount = horizontalImmersiveMode()
                                        ? immersiveSpectrumBarCount(width)
                                        : TaskbarHost::kSpectrumBands;
            const float clusterW = spectrumVisualClusterW(visualCount);
            const float clusterX = x + std::max(0.0f, (width - clusterW) * 0.5f);
            drawBarSpectrum(clusterX, h, visualCount);
            break;
        }
        case SpectrumStyle::DreamyWave:
            drawDreamyWaveSpectrum(x, h, width);
            break;
        case SpectrumStyle::Default:
        default:
        {
            const int visualCount = horizontalImmersiveMode()
                                        ? immersiveSpectrumBarCount(width)
                                        : TaskbarHost::kSpectrumBands;
            const float clusterW = spectrumVisualClusterW(visualCount);
            const float clusterX = x + std::max(0.0f, (width - clusterW) * 0.5f);
            drawDefaultSpectrum(clusterX, h, visualCount);
            break;
        }
        }
    }

    bool refreshImmersiveClockText() {
        if (!horizontalImmersiveMode())
            return false;

        SYSTEMTIME now{};
        GetLocalTime(&now);
        const uint64_t dateKey =
            (static_cast<uint64_t>(now.wYear) * 10000ULL) +
            (static_cast<uint64_t>(now.wMonth) * 100ULL) + now.wDay;
        const uint64_t minuteKey =
            (dateKey * 10000ULL) + (static_cast<uint64_t>(now.wHour) * 100ULL) +
            now.wMinute;
        if (clockMinuteKey_ == minuteKey && !clockTimeText_.empty() &&
            !clockDateText_.empty())
            return false;

        wchar_t timeText[64]{};
        wchar_t dateText[64]{};
        if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &now, nullptr,
                             timeText, static_cast<int>(_countof(timeText))))
            swprintf_s(timeText, L"%02u:%02u", now.wHour, now.wMinute);
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &now, nullptr,
                             dateText, static_cast<int>(_countof(dateText)), nullptr))
            swprintf_s(dateText, L"%04u/%02u/%02u", now.wYear, now.wMonth, now.wDay);

        const bool changed = clockTimeText_ != timeText || clockDateText_ != dateText;
        clockTimeText_ = timeText;
        clockDateText_ = dateText;
        clockMinuteKey_ = minuteKey;
        return changed;
    }

    static std::wstring formatResourcePercent(double percent) {
        wchar_t text[16]{};
        swprintf_s(text, L"%.0f%%", std::clamp(percent, 0.0, 100.0));
        return text;
    }

    static std::wstring formatNetworkRate(uint64_t bytesPerSecond) {
        static constexpr const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
        double value = static_cast<double>(bytesPerSecond);
        size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < std::size(units)) {
            value /= 1024.0;
            ++unit;
        }

        wchar_t text[32]{};
        if (unit == 0 || value >= 100.0)
            swprintf_s(text, L"%.0f%ls", value, units[unit]);
        else
            swprintf_s(text, L"%.1f%ls", value, units[unit]);
        return text;
    }

    static std::wstring formatFrequency(bool available, double gigahertz) {
        if (!available)
            return L"—";
        wchar_t text[24]{};
        swprintf_s(text, L"%.1fGHz", std::clamp(gigahertz, 0.0, 99.0));
        return text;
    }

    void startResourceMonitoring() {
        resourceCpuText_ = L"—";
        resourceMemoryText_ = L"—";
        resourceDownloadText_ = L"—";
        resourceUploadText_ = L"—";
        resourceGpuText_ = L"—";
        resourceCpuFrequencyText_ = L"—";
        resourceSnapshotRevision_ = UINT64_MAX;
        nextResourceSnapshotPollMs_ = 0;
        resourceMonitor_.start();
    }

    void stopResourceMonitoring() {
        resourceMonitor_.stop();
        nextResourceSnapshotPollMs_ = 0;
    }

    bool refreshResourceSnapshot() {
        if (!isAppBarView())
            return false;

        const ULONGLONG now = monotonicNowMs();
        if (now < nextResourceSnapshotPollMs_)
            return false;
        nextResourceSnapshotPollMs_ = now + kResourceSnapshotPollMs;

        const system_monitor::ResourceSnapshot snapshot = resourceMonitor_.snapshot();
        if (snapshot.revision == resourceSnapshotRevision_)
            return false;
        resourceSnapshotRevision_ = snapshot.revision;
        resourceCpuText_ = snapshot.cpuAvailable ? formatResourcePercent(snapshot.cpuPercent)
                                                  : L"—";
        resourceMemoryText_ = snapshot.memoryAvailable
                                  ? formatResourcePercent(snapshot.memoryPercent)
                                  : L"—";
        resourceDownloadText_ = snapshot.networkAvailable
                                    ? formatNetworkRate(snapshot.downloadBytesPerSecond)
                                    : L"—";
        resourceUploadText_ = snapshot.networkAvailable
                                  ? formatNetworkRate(snapshot.uploadBytesPerSecond)
                                  : L"—";
        resourceGpuText_ = snapshot.gpuAvailable ? formatResourcePercent(snapshot.gpuPercent)
                                                  : L"—";
        resourceCpuFrequencyText_ =
            formatFrequency(snapshot.cpuFrequencyAvailable, snapshot.cpuFrequencyGHz);
        return true;
    }

    void drawImmersiveClock(float x, float h, float width) {
        auto* rt = renderer.renderTarget();
        if (!rt || width <= 0.0f || !fmtClockTime_ || !fmtClockDate_ || !brushText_ ||
            !brushDim_)
            return;
        fmtClockTime_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        const float blockH = std::min(35.0f, std::max(1.0f, h - 2.0f));
        const float top = std::max(0.0f, (h - blockH) * 0.5f);
        const float timeH = blockH * 0.56f;
        rt->DrawTextW(clockTimeText_.c_str(), static_cast<UINT32>(clockTimeText_.size()),
                      fmtClockTime_, D2D1::RectF(x, top, x + width, top + timeH), brushText_,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        rt->DrawTextW(clockDateText_.c_str(), static_cast<UINT32>(clockDateText_.size()),
                      fmtClockDate_, D2D1::RectF(x, top + timeH, x + width, top + blockH),
                      brushDim_, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
    }

    void drawDockResourceStatus(float x, float h, float width) {
        auto* rt = renderer.renderTarget();
        if (!rt || !isAppBarView() || width <= 0.0f || !fmtClockTime_ || !brushText_)
            return;

        IDWriteTextFormat* resourceFormat = fmtClockTime_;
        resourceFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const std::wstring baseCells[] = {L"CPU: " + resourceCpuText_,
                                          L"内存: " + resourceMemoryText_,
                                          L"下行: " + resourceDownloadText_,
                                          L"上行: " + resourceUploadText_};
        const auto extraRows = dockResourceExtraRows();
        const size_t pageCount = 1 + (extraRows.size() + 1) / 2;
        const size_t page = std::min(dockResourcePage_, pageCount - 1);

        std::array<DockResourceRow, 2> visibleRows{};
        if (page == 0) {
            visibleRows[0] = {baseCells[0], baseCells[2]};
            visibleRows[1] = {baseCells[1], baseCells[3]};
        } else {
            const size_t start = (page - 1) * 2;
            if (start < extraRows.size())
                visibleRows[0] = extraRows[start];
            if (start + 1 < extraRows.size())
                visibleRows[1] = extraRows[start + 1];
        }

        const float blockH = std::min(38.0f, std::max(1.0f, h - 2.0f));
        const float top = std::max(0.0f, (h - blockH) * 0.5f);
        const float rowH = blockH * 0.5f;
        const float leftColumnW = std::min(kDockResourceMetricColumnW,
                                           std::max(1.0f, width - kDockResourceZoneGap));
        const float rightColumnX = x + leftColumnW + kDockResourceZoneGap;
        const float rightColumnW =
            std::max(1.0f, width - leftColumnW - kDockResourceZoneGap);

        IDWriteFactory* dwrite = renderer.dwrite();
        auto drawResourceCell = [&](const std::wstring& text, float left, float rowTop,
                                    float cellW) {
            if (!dwrite || cellW <= 0.0f)
                return;
            IDWriteTextLayout* layout = nullptr;
            const D2D1_RECT_F rect = D2D1::RectF(left, rowTop, left + cellW, rowTop + rowH);
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                resourceFormat, cellW, rowH, &layout)) ||
                !layout)
                return;
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, nullptr);
            rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brushText_,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            layout->Release();
        };
        for (size_t row = 0; row < visibleRows.size(); ++row) {
            const auto& current = visibleRows[row];
            const float rowTop = top + row * rowH;
            drawResourceCell(current.left, x, rowTop, leftColumnW);
            if (!current.right.empty())
                drawResourceCell(current.right, rightColumnX, rowTop, rightColumnW);
        }
    }

    DockPetMode dockPetMode() const {
        if (!isAppBarView() || !isSessionVisible() || isMinimalMode() || isStoppedMode())
            return DockPetMode::Hidden;
        if (scene_ == DisplayScene::Idle)
            return DockPetMode::Roaming;
        return media.playing ? DockPetMode::Listening : DockPetMode::Paused;
    }

    void drawDockPet(float w, float h, float leftW) {
        const ULONGLONG now = frameNowMs_ != 0 ? frameNowMs_ : monotonicNowMs();
        const DockPetMode mode = dockPetMode();
        if (mode == DockPetMode::Hidden) {
            dockPet_.setMode(mode, now);
            return;
        }

        const float resourceW = dockResourceZoneW(w);
        const float resourceX = w - resourceW - kDockResourceRightPadding;
        const float spectrumW = immersiveSpectrumZoneW(w);
        const float spectrumX = resourceX - kDockResourceZoneGap - spectrumW;
        const float start = songInfoVisible_ && scene_ != DisplayScene::Idle
                                ? kSongInfoLyricGap
                                : kTextPadding;
        const float laneLeft = leftW + start + immersiveControlsWidth(h);
        const float laneRight = mode == DockPetMode::Roaming
                                    ? spectrumX - kTextPadding
                                    : std::min(spectrumX - kTextPadding,
                                               laneLeft + kDockPetSeatW);
        const float minLaneWidth = mode == DockPetMode::Roaming ? kPetLaneMinWidthDip : 28.0f;
        if (laneRight <= laneLeft + minLaneWidth) {
            dockPet_.setMode(DockPetMode::Hidden, now);
            return;
        }

        dockPet_.setLane(laneLeft, laneRight, h);
        dockPet_.setMode(mode, now);
        dockPet_.setLightTheme(lightTheme_);
        // 播放中切歌（曲目变化但始终处于 Listening）时补一次雀跃；
        // 暂停/恢复与开始播放的雀跃由 DockPet::setMode 内部处理。
        if (mode == DockPetMode::Listening) {
            const std::wstring track = media.title + L"|" + media.artist;
            if (track != dockPetTrack_) {
                if (!dockPetTrack_.empty())
                    dockPet_.hop(now);
                dockPetTrack_ = track;
            }
        }
        dockPet_.draw(drawTarget());
    }

    void drawImmersiveRightSide(float w, float h, bool showSpectrum) {
        if (!horizontalImmersiveMode())
            return;

        const float zoneW = immersiveSpectrumZoneW(w);
        const bool dock = isAppBarView();
        const float resourceW = dock ? dockResourceZoneW(w) : 0.0f;
        const float resourceX = w - resourceW -
                                (dock ? kDockResourceRightPadding : kTextPadding);
        const float zoneX = dock ? resourceX - kDockResourceZoneGap - zoneW
                                 : w - zoneW - kTextPadding;
        const float clockW = dock ? 0.0f : std::min(zoneW, immersiveClockZoneW(w));
        const float clockX = w - clockW - kTextPadding;
        const float spectrumRight = dock ? resourceX - kDockResourceZoneGap
                                         : clockX - kImmersiveSpectrumClockGap;
        const float spectrumW = std::max(0.0f, spectrumRight - zoneX);
        if (showSpectrum && spectrumW > 0.0f) {
            if (spectrumStyle_ == SpectrumStyle::DreamyWave) {
                drawDreamyWaveSpectrum(zoneX, h, spectrumW);
            } else {
                // 先按原始频谱区确定柱数；Dock 贴右对齐，普通沉浸模式仍按原逻辑居中。
                const int availableCount = std::max(
                    1, static_cast<int>(std::floor((spectrumW + kSpectrumGap) /
                                                   (kSpectrumBarW + kSpectrumGap))));
                const int visualCount =
                    std::min(immersiveSpectrumBarCount(zoneW), availableCount);
                const float clusterW = spectrumVisualClusterW(visualCount);
                const float naturalX = zoneX + std::max(0.0f, (zoneW - clusterW) * 0.5f);
                // Dock 模式将频谱簇贴到右边界，保证它与资源区之间只保留
                // kDockResourceZoneGap；普通沉浸模式继续保持原有居中位置。
                const float clusterX = dock
                                           ? std::max(zoneX, spectrumRight - clusterW)
                                           : std::max(zoneX, std::min(naturalX,
                                                                       spectrumRight - clusterW));
                if (spectrumStyle_ == SpectrumStyle::Bars)
                    drawBarSpectrum(clusterX, h, visualCount);
                else
                    drawDefaultSpectrum(clusterX, h, visualCount);
            }
        }
        if (dock)
            drawDockResourceStatus(resourceX, h, resourceW);
        else
            drawImmersiveClock(clockX, h, clockW);
    }

    float backgroundWaveRight(float w) const {
        if (!horizontalImmersiveMode())
            return w - kTextPadding;
        if (isAppBarView()) {
            return w - dockResourceZoneW(w) - kDockResourceRightPadding -
                   kDockResourceZoneGap;
        }
        return w - immersiveClockZoneW(w) - kImmersiveSpectrumClockGap - kTextPadding;
    }

    bool taskbarDynamicBackgroundVisible() const {
        if (scene_ == DisplayScene::NoPlayback ||
            idleQuoteBackground_ == IdleQuoteBackground::None || isMinimalMode())
            return false;

        switch (idleQuoteBackgroundScope_) {
        case IdleQuoteBackgroundScope::DailyQuote:
            // “每日一言”范围覆盖整个待机内容场景：关闭每日一言后
            // 显示的默认欢迎语同样属于待机内容，不随开关丢失动态背景。
            return scene_ == DisplayScene::Idle;
        case IdleQuoteBackgroundScope::Lyrics:
            return scene_ == DisplayScene::Lyrics;
        case IdleQuoteBackgroundScope::All:
            return scene_ == DisplayScene::Idle || scene_ == DisplayScene::Lyrics;
        case IdleQuoteBackgroundScope::None:
        default:
            return false;
        }
    }

    bool taskbarDynamicBackgroundAnimating() const {
        return taskbarDynamicBackgroundVisible() && clientAnimations_;
    }

    void drawIdleQuoteBackground(float w, float h, float contentW) {
        auto* rt = renderer.renderTarget();
        if (!rt || w <= 0.0f || h <= 0.0f || !taskbarDynamicBackgroundVisible())
            return;

        const float effectW = std::clamp(contentW, 1.0f, w);

        const float time = static_cast<float>(frameNowMs_ % 600000ULL) / 1000.0f;
        auto* warm = brushIdleWarm_ ? brushIdleWarm_ : brushText_;
        auto* cool = brushIdleCool_ ? brushIdleCool_ : brushText_;
        auto* accent = brushIdleAccent_ ? brushIdleAccent_ : brushText_;
        if (!warm && !cool && !accent)
            return;

        auto drawWithOpacity = [](ID2D1SolidColorBrush* brush, float opacity,
                                  const auto& draw) {
            if (!brush)
                return;
            const float previous = brush->GetOpacity();
            brush->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
            draw();
            brush->SetOpacity(previous);
        };

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, effectW, h),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        switch (idleQuoteBackground_) {
        case IdleQuoteBackground::FallingLeaves: {
            struct Leaf {
                float x;
                float y;
                float size;
                float fallSpeed;
                float drift;
                float phase;
                float angle;
            };
            static constexpr Leaf leaves[] = {
                {0.03f, -0.22f, 3.6f, 9.4f, 0.12f, 0.4f, -24.0f},
                {0.10f, 0.46f, 3.0f, 7.2f, 0.16f, 2.1f, 18.0f},
                {0.17f, 0.06f, 4.3f, 12.0f, 0.10f, 4.0f, 42.0f},
                {0.24f, 0.78f, 2.8f, 8.6f, 0.14f, 1.2f, -12.0f},
                {0.31f, -0.04f, 3.8f, 10.8f, 0.17f, 3.5f, 28.0f},
                {0.38f, 0.30f, 3.1f, 7.8f, 0.11f, 5.1f, -36.0f},
                {0.45f, 0.93f, 4.6f, 11.2f, 0.15f, 0.9f, 12.0f},
                {0.52f, 0.14f, 2.9f, 8.2f, 0.18f, 2.8f, -48.0f},
                {0.59f, 0.61f, 3.7f, 10.0f, 0.12f, 4.6f, 34.0f},
                {0.66f, -0.28f, 3.2f, 7.0f, 0.16f, 1.6f, -8.0f},
                {0.73f, 0.38f, 4.1f, 12.6f, 0.10f, 3.9f, 52.0f},
                {0.80f, 0.84f, 2.7f, 8.8f, 0.14f, 5.4f, -30.0f},
                {0.87f, 0.18f, 3.5f, 9.8f, 0.17f, 2.6f, 20.0f},
                {0.95f, 0.56f, 4.4f, 11.8f, 0.11f, 0.7f, -42.0f},
            };
            const float loopH = h + 16.0f;
            for (size_t i = 0; i < _countof(leaves); ++i) {
                float y = std::fmod(leaves[i].y * h + time * leaves[i].fallSpeed, loopH);
                if (y < 0.0f)
                    y += loopH;
                y -= 8.0f;
                float x = leaves[i].x * effectW +
                          std::sin(time * 0.62f + leaves[i].phase) * leaves[i].drift * effectW;
                x = std::fmod(x + effectW, effectW);
                if (x < 0.0f)
                    x += effectW;
                const D2D1_POINT_2F center = D2D1::Point2F(x, y);
                const float angle = leaves[i].angle +
                                    std::sin(time * 0.88f + leaves[i].phase) * 24.0f;
                drawWithOpacity(warm, 0.22f + (i % 4) * 0.045f, [&] {
                    D2D1_MATRIX_3X2_F previous{};
                    rt->GetTransform(&previous);
                    rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle, center));
                    rt->FillEllipse(D2D1::Ellipse(center, leaves[i].size,
                                                   leaves[i].size * 0.46f),
                                    warm);
                    rt->DrawLine(D2D1::Point2F(x - leaves[i].size * 0.52f, y),
                                 D2D1::Point2F(x + leaves[i].size * 0.52f, y), warm, 0.70f);
                    rt->SetTransform(previous);
                });
            }
            break;
        }
        case IdleQuoteBackground::TwinklingStars: {
            struct Star {
                float x;
                float y;
                float radius;
                float phase;
                float speed;
            };
            static constexpr Star stars[] = {
                {0.03f, 0.18f, 0.92f, 0.2f, 1.25f},  {0.07f, 0.68f, 0.78f, 2.4f, 0.92f},
                {0.12f, 0.42f, 1.18f, 4.1f, 1.46f},   {0.16f, 0.86f, 0.84f, 1.1f, 0.78f},
                {0.21f, 0.08f, 0.76f, 3.2f, 1.12f},   {0.25f, 0.56f, 1.06f, 5.0f, 1.36f},
                {0.30f, 0.30f, 0.88f, 0.6f, 0.86f},   {0.34f, 0.76f, 0.74f, 2.9f, 1.58f},
                {0.39f, 0.16f, 1.10f, 4.7f, 1.04f},   {0.43f, 0.92f, 0.82f, 1.8f, 1.32f},
                {0.48f, 0.46f, 0.96f, 3.7f, 0.74f},   {0.52f, 0.12f, 0.76f, 5.6f, 1.18f},
                {0.56f, 0.70f, 1.24f, 1.5f, 1.42f},   {0.60f, 0.34f, 0.80f, 3.0f, 0.88f},
                {0.64f, 0.84f, 1.02f, 4.4f, 1.24f},   {0.68f, 0.22f, 0.72f, 0.9f, 1.60f},
                {0.72f, 0.52f, 1.14f, 2.1f, 1.02f},   {0.76f, 0.06f, 0.86f, 5.2f, 1.38f},
                {0.80f, 0.76f, 1.00f, 3.4f, 0.80f},   {0.84f, 0.38f, 0.78f, 1.7f, 1.50f},
                {0.88f, 0.16f, 1.20f, 4.8f, 1.14f},   {0.92f, 0.62f, 0.82f, 2.6f, 0.96f},
                {0.96f, 0.30f, 1.08f, 0.7f, 1.28f},   {0.985f, 0.90f, 0.74f, 5.8f, 0.72f},
            };
            for (size_t i = 0; i < _countof(stars); ++i) {
                const float twinkle =
                    0.5f + 0.5f * std::sin(time * stars[i].speed + stars[i].phase);
                const D2D1_POINT_2F center =
                    D2D1::Point2F(stars[i].x * effectW, stars[i].y * h);
                const float radius = stars[i].radius * (0.72f + twinkle * 0.52f);
                drawWithOpacity(cool, 0.14f + twinkle * 0.30f, [&] {
                    rt->FillEllipse(D2D1::Ellipse(center, radius, radius), cool);
                    if (i % 4 == 0 && twinkle > 0.52f) {
                        const float ray = radius * (2.4f + twinkle * 1.5f);
                        rt->DrawLine(D2D1::Point2F(center.x - ray, center.y),
                                     D2D1::Point2F(center.x + ray, center.y), cool, 0.70f);
                        rt->DrawLine(D2D1::Point2F(center.x, center.y - ray),
                                     D2D1::Point2F(center.x, center.y + ray), cool, 0.70f);
                    }
                });
            }
            break;
        }
        case IdleQuoteBackground::BinaryRain: {
            if (fmtSecondary_) {
                const int columns = std::clamp(static_cast<int>(effectW / 23.0f), 5, 10);
                const float columnW = effectW / static_cast<float>(columns);
                const float rowGap = std::max(9.0f, h / 3.6f);
                const ULONGLONG bitTick = frameNowMs_ / 190ULL;
                for (int col = 0; col < columns; ++col) {
                    const float head = std::fmod(
                        time * (7.0f + static_cast<float>(col % 3) * 2.0f) +
                            static_cast<float>(col) * 8.0f,
                        h + rowGap * 4.0f) - rowGap * 2.0f;
                    for (int row = -1; row < 5; ++row) {
                        const float y = head + static_cast<float>(row) * rowGap;
                        if (y < -rowGap || y > h)
                            continue;
                        const bool one = ((bitTick + static_cast<ULONGLONG>(col * 13 +
                                                                             row * 7 +
                                                                             col * row * 3)) &
                                          1ULL) != 0;
                        wchar_t digit[2] = {one ? L'1' : L'0', L'\0'};
                        const float trail = std::clamp(1.0f - std::fabs(y - head) /
                                                                 (rowGap * 3.5f),
                                                       0.0f, 1.0f);
                        const float opacity = 0.045f + trail * 0.16f;
                        const D2D1_RECT_F rect = D2D1::RectF(
                            columnW * (static_cast<float>(col) + 0.5f) - 4.0f, y,
                            columnW * (static_cast<float>(col) + 0.5f) + 4.0f,
                            y + rowGap + 2.0f);
                        drawWithOpacity(accent, opacity, [&] {
                            rt->DrawTextW(digit, 1, fmtSecondary_, rect, accent,
                                          D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                          DWRITE_MEASURING_MODE_NATURAL);
                        });
                    }
                }
            }
            break;
        }
        case IdleQuoteBackground::FloatingParticles: {
            struct Particle {
                float x;
                float y;
                float radius;
                float speed;
                float amplitude;
                float phase;
            };
            static constexpr Particle particles[] = {
                {0.02f, 0.28f, 1.18f, 4.0f, 0.18f, 0.4f},  {0.07f, 0.74f, 0.92f, 5.5f, 0.14f, 2.3f},
                {0.12f, 0.48f, 1.48f, 2.8f, 0.20f, 4.2f},  {0.17f, 0.12f, 1.00f, 4.8f, 0.13f, 1.2f},
                {0.22f, 0.86f, 1.26f, 3.3f, 0.17f, 3.5f},  {0.27f, 0.38f, 0.86f, 6.2f, 0.22f, 5.0f},
                {0.32f, 0.66f, 1.38f, 3.7f, 0.16f, 0.9f},  {0.37f, 0.22f, 0.96f, 5.2f, 0.19f, 2.8f},
                {0.42f, 0.80f, 1.10f, 3.1f, 0.14f, 4.8f},  {0.47f, 0.52f, 1.56f, 4.6f, 0.18f, 1.7f},
                {0.52f, 0.08f, 0.90f, 5.8f, 0.21f, 3.9f},  {0.57f, 0.92f, 1.32f, 2.6f, 0.15f, 5.4f},
                {0.62f, 0.34f, 1.16f, 4.3f, 0.20f, 1.5f},  {0.67f, 0.70f, 0.88f, 6.0f, 0.13f, 3.1f},
                {0.72f, 0.18f, 1.44f, 3.4f, 0.17f, 4.5f},  {0.77f, 0.84f, 1.02f, 4.9f, 0.22f, 0.7f},
                {0.82f, 0.44f, 1.30f, 2.9f, 0.16f, 2.0f},  {0.87f, 0.06f, 0.94f, 5.6f, 0.19f, 3.6f},
                {0.92f, 0.62f, 1.50f, 3.9f, 0.14f, 5.1f},  {0.97f, 0.30f, 1.08f, 4.5f, 0.20f, 1.0f},
            };
            for (size_t i = 0; i < _countof(particles); ++i) {
                float x = std::fmod(particles[i].x * effectW + time * particles[i].speed, effectW);
                if (x < 0.0f)
                    x += effectW;
                const float y = particles[i].y * h +
                                std::sin(time * 0.72f + particles[i].phase) *
                                    particles[i].amplitude * h;
                const float pulse =
                    0.5f + 0.5f * std::sin(time * (1.1f + i * 0.04f) + particles[i].phase);
                auto* brush = i % 3 == 0 ? accent : cool;
                drawWithOpacity(brush, 0.15f + pulse * 0.26f, [&] {
                    const float radius = particles[i].radius * (0.72f + pulse * 0.48f);
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush);
                    if (i % 2 == 0) {
                        rt->DrawLine(D2D1::Point2F(x - particles[i].speed * 0.7f, y),
                                     D2D1::Point2F(x - radius, y), brush, 0.65f);
                    }
                });
            }
            break;
        }
        case IdleQuoteBackground::None:
        default:
            break;
        }
        rt->PopAxisAlignedClip();
    }

    void drawVinylCover(float coverX, float coverY, float s) {
        auto* rt = drawTarget();
        if (!rt || s <= 0.0f)
            return;

        D2D1_POINT_2F center = D2D1::Point2F(coverX + s * 0.5f, coverY + s * 0.5f);
        float outerRadius = std::max(1.0f, s * 0.5f - 0.5f);
        float haloWidth = std::min(kVinylHaloWidth, outerRadius * 0.25f);
        float recordRadius = std::max(0.5f, outerRadius - haloWidth);
        float innerRadius = vinylInnerRadius(s);
        D2D1_ELLIPSE outer{center, outerRadius, outerRadius};
        D2D1_ELLIPSE record{center, recordRadius, recordRadius};
        D2D1_ELLIPSE inner{center, innerRadius, innerRadius};

        // Direct2D 的正角度就是顺时针旋转；整个唱片组以中心为轴同步旋转。
        D2D1_MATRIX_3X2_F previous{};
        rt->GetTransform(&previous);
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(vinylAngleDeg_, center) * previous);
        if (brushCoverHalo_)
            rt->FillEllipse(outer, brushCoverHalo_);
        if (brushVinylBase_)
            rt->FillEllipse(record, brushVinylBase_);

        if (brushVinylGroove_) {
            // 细密同心纹理和一条短径向高光，让纯色封面也能看出唱片正在转动。
            for (float radius = innerRadius + 1.5f; radius < recordRadius - 0.5f;
                 radius += 2.0f) {
                rt->DrawEllipse(D2D1_ELLIPSE{center, radius, radius}, brushVinylGroove_, 0.55f);
            }
            rt->DrawLine(
                D2D1::Point2F(center.x - recordRadius * 0.82f,
                               center.y - recordRadius * 0.18f),
                D2D1::Point2F(center.x - innerRadius - 1.0f,
                               center.y - recordRadius * 0.18f),
                brushVinylGroove_, 0.8f);
        }

        D2D1_RECT_F innerRect = D2D1::RectF(center.x - innerRadius, center.y - innerRadius,
                                            center.x + innerRadius, center.y + innerRadius);
        if (coverBmp && vinylCoverClip_ && coverLayer_) {
            rt->PushLayer(D2D1::LayerParameters1(
                              D2D1::InfiniteRect(), vinylCoverClip_,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                              D2D1::Matrix3x2F::Translation(coverX, coverY)),
                          coverLayer_);
            rt->DrawBitmap(coverBmp, innerRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
        } else if (brushDim_) {
            rt->FillEllipse(inner, brushDim_);
        }
        if (brushVinylGroove_)
            rt->DrawEllipse(inner, brushVinylGroove_, 0.8f);
        if (brushVinylBase_)
            rt->FillEllipse(D2D1_ELLIPSE{center, std::max(0.8f, s * 0.025f),
                                         std::max(0.8f, s * 0.025f)},
                            brushVinylBase_);
        rt->SetTransform(previous);
    }

    void drawPlatformIcon(float coverX, float coverY, float s) {
        auto* rt = drawTarget();
        if (!rt || !albumCoverVisible_ || !platformIconVisible_ || !platformIconBmp ||
            s <= 0.0f)
            return;

        const float iconSize = std::max(1.0f, std::min(s * 0.50f, 13.0f));
        const float inset = std::min(1.5f, s * 0.06f);
        D2D1_RECT_F iconRect = D2D1::RectF(
            coverX + s - iconSize - inset, coverY + s - iconSize - inset,
            coverX + s - inset, coverY + s - inset);
        rt->DrawBitmap(platformIconBmp, iconRect, 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void setPositionMode(int mode) {
        mode = mode == 1 ? 1 : 0;
        if (positionMode_ == mode)
            return;
        positionMode_ = mode;
        adjustPosition();
        requestFrameAndFlush();
    }

    // Explorer 重启后由托盘主窗口调用（TaskbarCreated 广播只发给顶层窗口）。
    // 作为 Shell_TrayWnd 的子窗口，歌词窗在 explorer 退出时会随任务栏一起被销毁，
    // 此时只剩野句柄，必须整体重建；若幸存（崩溃非正常退出）则重新附着即可。
    // 歌词/媒体/字体等状态都保存在 Impl 成员里，重建窗口后原样恢复
    void onTaskbarCreated() {
        runtime_log::writef(L"[taskbar] TaskbarCreated: hwnd=%p alive=%d visible=%d timer=%d",
                            hwnd, (hwnd && IsWindow(hwnd)) ? 1 : 0, isWindowVisible() ? 1 : 0,
                            timerRunning_ ? 1 : 0);
        if (hwnd && IsWindow(hwnd)) {
            if (findTaskbar()) {
                const bool immersiveBackdropPrepared =
                    viewMode_ == TaskbarViewMode::Immersive && !dockBackdrop_.available();
                if (immersiveBackdropPrepared) {
                    if (!taskbarEmbedded_ || detachFromTaskbar())
                        initializeBackdropForCurrentViewMode();
                    else
                        backgroundBlurSupported_ = false;
                }
                if (isAppBarView()) {
                    // Explorer 重启后 Shell 的 AppBar 注册表已丢失，本地标记不再可信。
                    appBarRegistered_ = false;
                    cancelTaskbarAttachRetry();
                    if (isSessionVisible() && !isStoppedMode())
                        registerAppBar();
                } else if (!attachToTaskbar(hwnd)) {
                    scheduleTaskbarAttachRetry();
                } else {
                    cancelTaskbarAttachRetry();
                }
                if (!immersiveBackdropPrepared)
                    initializeBackdropForCurrentViewMode();
                adjustPosition();
                reconcileWindowVisibility();
                requestFrameAndFlush();
            } else {
                taskbarEmbedded_ = false;
                if (!isAppBarView())
                    scheduleTaskbarAttachRetry();
            }
            return;
        }
        dockBackdrop_.reset();
        hwnd = nullptr;
        taskbarEmbedded_ = false;
        // Explorer 强制退出时旧窗口可能没有走 WM_DESTROY，Shell 侧与本地的
        // AppBar 注册状态都必须按已失效处理，避免重建窗口跳过 ABM_NEW。
        appBarRegistered_ = false;
        // 旧窗口被系统侧销毁时不一定投递 WM_DESTROY（Explorer 被强杀），
        // timerRunning_ 可能残留为 true，但定时器已随旧窗口消失；重建时同样
        // 等待新任务栏的首个探测结果，避免按旧矩形先显示一帧。
        timerRunning_ = false;
        timerMs_ = 0;
        renderState_.setVisibilitySuppressed(true);
        renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
        probeReady_ = false;
        delete probeOut_.exchange(nullptr);
        if (createWindow(inst)) {
            if (isAppBarView()) {
                renderState_.setVisibilitySuppressed(false);
                setPlacementStatus(TaskbarPlacementStatus::Safe);
            }
            reconcileWindowVisibility();
            if (isWindowVisible())
                requestFrameAndFlush();
        }
    }

    void recreateClockFormats() {
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite)
            return;
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            if (*out) {
                (*out)->Release();
                *out = nullptr;
            }
            DWRITE_FONT_WEIGHT effectiveWeight =
                isBoldFontStyle(fontStyle_) ? DWRITE_FONT_WEIGHT_BOLD : weight;
            dwrite->CreateTextFormat(fontFamily_.c_str(), nullptr, effectiveWeight,
                                     dwriteStyleOf(fontStyle_), DWRITE_FONT_STRETCH_NORMAL, size,
                                     L"", out);
            if (*out) {
                (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                fluent::applyUiFontFallback(*out);
            }
        };
        make(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtClockTime_);
        make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtClockDate_);
    }

    void recreateFormats() {
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite)
            return;
        recreateClockFormats();
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_PARAGRAPH_ALIGNMENT pa,
                        IDWriteTextFormat** out) {
            if (*out) {
                (*out)->Release();
                *out = nullptr;
            }
            DWRITE_FONT_WEIGHT effectiveWeight =
                isBoldFontStyle(fontStyle_) ? DWRITE_FONT_WEIGHT_BOLD : weight;
            dwrite->CreateTextFormat(fontFamily_.c_str(), nullptr, effectiveWeight,
                                     dwriteStyleOf(fontStyle_), DWRITE_FONT_STRETCH_NORMAL, size,
                                     L"zh-cn", out);
            if (*out) {
                (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                (*out)->SetParagraphAlignment(pa);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                // 不裁剪，超长时由 drawScrollingText 滚动显示
                fluent::applyUiFontFallback(*out);
            }
        };
        // 歌名/歌手固定字号，不随歌词字号调整；只有歌词跟随 fontSize_
        make(kBaseFontSize * 1.05f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtTitle_);
        make(kBaseFontSize * 0.92f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtArtist_);
        make(fontSize_ * kLyricMainFontScale, DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtLyric_);
        // 双行模式的下一行：比翻译字号（0.78）略大，但明显小于核心歌词（1.18）。
        make(fontSize_ * kLyricPreviewFontScale, DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtNextLyric_);
        make(fontSize_ * 0.78f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtSecondary_);
        requestInvalidation(RenderInvalidation::Text);
        requestInvalidation(RenderInvalidation::SongInfo);
    }

    void changeFont(float delta) {
        setFont(fontFamily_, fontSize_ + delta, fontStyle_);
    }

    void setFont(const std::wstring& family, float size, LyricFontStyle style) {
        fontFamily_ = family;
        fontSize_ = std::clamp(size, kMinFont, kMaxFont);
        fontStyle_ = style;
        recreateFormats();
        requestInvalidation(RenderInvalidation::Layout);
        requestFrameAndFlush();
    }

    void discardDeviceResources() {
        cancelSceneWindowResize();
        auto r = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        r(fmtTitle_);
        r(fmtArtist_);
        r(fmtLyric_);
        r(fmtNextLyric_);
        r(fmtSecondary_);
        r(fmtDragPreview_);
        r(fmtClockTime_);
        r(fmtClockDate_);
        r(titleLayout_);
        r(artistLayout_);
        r(lyricLayout_);
        r(verticalLyricLayout_);
        releaseVerticalLyricParts(verticalLyricParts_);
        r(nextLyricLayout_);
        r(secondaryLayout_);
        r(outgoingLyricLayout_);
        r(outgoingVerticalLyricLayout_);
        releaseVerticalLyricParts(outgoingVerticalLyricParts_);
        r(outgoingSecondaryLayout_);
        r(outgoingNextLyricLayout_);
        r(brushBg_);
        r(brushHover_);
        r(brushText_);
        r(brushDim_);
        r(brushBtn_);
        r(brushBtnDisabled_);
        r(brushLyric_);
        r(brushLyricDim_);
        r(brushLyricGlow_);
        r(brushLyricOutline_);
        r(brushCoverHalo_);
        r(brushVinylBase_);
        r(brushVinylGroove_);
        r(brushSpectrum_);
        r(brushSpectrumBarGradient_);
        r(brushProgressBg_);
        r(brushBackground_);
        r(brushIdleWarm_);
        r(brushIdleCool_);
        r(brushIdleAccent_);
        dockPet_.discardDeviceResources();
        r(dragPreviewStroke_);
        r(coverBlurFx_);
        r(coverScaleFx_);
        coverBlurInput_ = nullptr;
        for (auto& cache : textFxCaches_) {
            r(cache.target);
            cache = {};
        }
        textFxUse_ = 0;
        karaokeSpans_.clear();
        karaokeGeometryLine_ = -1;
        karaokeGeometryLayout_ = nullptr;
        r(coverClip_);
        r(vinylCoverClip_);
        r(coverLayer_);
        r(lyricEdgeFadeBrush_);
        r(lyricEdgeFadeLayer_);
        r(lyricRightFadeBrush_);
        r(lyricRightFadeLayer_);
        r(songInfoDividerBrush_);
        media_control::release(controlGeometry);
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
        }
        if (platformIconBmp) {
            platformIconBmp->Release();
            platformIconBmp = nullptr;
        }
        drawTargetOverride_ = nullptr;
        renderer.discard();
        renderState_.setDeviceResourcePhase(RenderState::DeviceResourcePhase::Uninitialized);
        songContentTransition_.reset();
        clearLyricDCompState();
        requestInvalidation(toMask(RenderInvalidation::Text) |
                            toMask(RenderInvalidation::SongInfo) |
                            toMask(RenderInvalidation::Geometry) |
                            toMask(RenderInvalidation::Layout) |
                            toMask(RenderInvalidation::Cover) |
                            toMask(RenderInvalidation::PlatformIcon));
    }

    void refreshTheme() {
        app_icon::applyTaskbarIcon(hwnd);
        updateBackdropSolidColor();
        const bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        if (light != lightTheme_) {
            lightTheme_ = light;
            discardDeviceResources();
        }
        // 媒体卡片属于普通悬浮窗，使用 Window 主题，而不是任务栏主题。
        mediaPopup.refreshTheme();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    void releaseAll() {
        discardDeviceResources();
        renderer.releaseAll();
    }

    // ---------- 避让探测工作线程 ----------

    // 正常每 3 秒探测一次：TrafficMonitor、酷狗窗口矩形 + UIA 任务栏按钮矩形；
    // 空间不足隐藏时缩短到 1 秒，尽快发现可恢复空间。UIA 属性查询由 explorer
    // 的 UI 线程执行，不能跟随歌词帧率高频调用。
    // 每次循环都重新发布结果（即使没变化），变化比较在 UI 线程拾取时做
    void probeMain() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IUIAutomation* uia = nullptr;
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&uia));
        while (!probeStop_.load()) {
            if (HWND tb = taskbarAtomic_.load()) {
                auto* p = new ProbeResult;
                if (HWND tm = findTrafficMonitorWnd(tb))
                    GetWindowRect(tm, &p->rcTm);
                queryTaskbarButtonsUia(uia, tb, p->buttons);
                queryKugouTaskbarWindows(tb, p->kugouWindows);
                delete probeOut_.exchange(p); // 上一份未被拾取则丢弃
            }
            int elapsed = 0;
            while (!probeStop_.load()) {
                const UINT interval = probeFast_.load() ? kNoSpaceProbeIntervalMs
                                                        : kProbeIntervalMs;
                if (elapsed >= static_cast<int>(interval))
                    break;
                Sleep(100);
                elapsed += 100;
            }
        }
        if (uia)
            uia->Release();
        CoUninitialize();
    }

    void startProbe() {
        if (probeThread_.joinable())
            return;
        probeStop_ = false;
        probeThread_ = std::thread([this] { probeMain(); });
    }

    void stopProbe() {
        probeStop_ = true;
        probeFast_ = false;
        if (probeThread_.joinable())
            probeThread_.join();
        delete probeOut_.exchange(nullptr);
    }

    // UI 线程慢速分支：拾取探测结果，与缓存比较有变化才更新。返回是否有变化
    bool pickProbeResult() {
        std::unique_ptr<ProbeResult> p(probeOut_.exchange(nullptr));
        if (!p)
            return false;
        const bool firstProbe = !probeReady_;
        probeReady_ = true;
        bool changed = !EqualRect(&p->rcTm, &rcTrafficMonitor_);
        if (!changed) {
            if (p->buttons.size() != uiaButtons_.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < p->buttons.size(); ++i) {
                    if (!EqualRect(&p->buttons[i], &uiaButtons_[i])) {
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (!changed) {
            if (p->kugouWindows.size() != kugouTaskbarWindows_.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < p->kugouWindows.size(); ++i) {
                    if (!EqualRect(&p->kugouWindows[i], &kugouTaskbarWindows_[i])) {
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (changed) {
            rcTrafficMonitor_ = p->rcTm;
            uiaButtons_ = std::move(p->buttons);
            kugouTaskbarWindows_ = std::move(p->kugouWindows);
        }
        return changed || firstProbe;
    }

    // ---------- 封面解码 ----------

    void decodeCover() {
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
            // 旧封面可能仍被模糊链引用，强制下帧重新绑定输入
            coverBlurInput_ = nullptr;
        }
        auto* rt = renderer.renderTarget();
        if (!rt || !media.thumbnail || media.thumbnail->empty())
            return;

        HGLOBAL hglobal = GlobalAlloc(GHND, media.thumbnail->size());
        if (!hglobal)
            return;
        void* ptr = GlobalLock(hglobal);
        if (ptr) {
            memcpy(ptr, media.thumbnail->data(), media.thumbnail->size());
            GlobalUnlock(hglobal);
        }
        IStream* stream = nullptr;
        HRESULT hr = CreateStreamOnHGlobal(hglobal, TRUE, &stream);
        if (FAILED(hr) || !stream) {
            GlobalFree(hglobal);
            return;
        }
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            stream->Release();
            return;
        }
        UINT w = bitmap.GetWidth();
        UINT h = bitmap.GetHeight();
        // 只按显示尺寸（封面槽 DIP × DPI 缩放）生成位图：原图常达 500~1000px，
        // 全尺寸 LockBits/CreateBitmap 会把 MB 级像素常驻显存，实际只显示约 30px。
        const UINT targetPx =
            std::max(1u, static_cast<UINT>(std::ceil(coverSize() * scale())));
        Gdiplus::Bitmap* pixels = &bitmap;
        Gdiplus::Bitmap scaled(static_cast<INT>(targetPx), static_cast<INT>(targetPx),
                               PixelFormat32bppPARGB);
        if ((w > targetPx || h > targetPx) && scaled.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics g(&scaled);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            // 目标为正方形，与绘制时拉伸到方形封面槽的行为一致
            if (g.DrawImage(&bitmap, Gdiplus::Rect(0, 0, static_cast<INT>(targetPx),
                                                   static_cast<INT>(targetPx)),
                            0, 0, static_cast<INT>(w), static_cast<INT>(h),
                            Gdiplus::UnitPixel) == Gdiplus::Ok) {
                pixels = &scaled;
                w = targetPx;
                h = targetPx;
            }
        }
        Gdiplus::BitmapData bitmapData{};
        Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
        if (pixels->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB,
                             &bitmapData) != Gdiplus::Ok) {
            stream->Release();
            return;
        }
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        ID2D1Bitmap1* decoded = nullptr;
        hr = rt->CreateBitmap(D2D1::SizeU(w, h), bitmapData.Scan0, bitmapData.Stride, &props,
                              &decoded);
        coverBmp = decoded; // ID2D1Bitmap1 派生自 ID2D1Bitmap
        pixels->UnlockBits(&bitmapData);
        stream->Release();
    }

    // 封面模糊链：GaussianBlur → Scale 铺满全窗。效果与设备同生命周期，
    // 封面更换或设备重建后经 coverBlurInput_ 惰性重绑；窗口尺寸每帧可能变化，
    // Scale 参数便宜，直接按帧写入
    ID2D1Effect* ensureCoverBlurChain(float w, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !coverBmp)
            return nullptr;
        if (!coverBlurFx_) {
            if (FAILED(rt->CreateEffect(kGaussianBlurClsid, &coverBlurFx_)))
                return nullptr;
            coverBlurFx_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                                   kCoverBlurStdDev);
            coverBlurFx_->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                                   D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
            // SOFT（镜像）避免边缘模糊后透出黑边
            coverBlurFx_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        }
        if (!coverScaleFx_) {
            if (FAILED(rt->CreateEffect(kScaleClsid, &coverScaleFx_)))
                return nullptr;
            coverScaleFx_->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE,
                                    D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            coverScaleFx_->SetInputEffect(0, coverBlurFx_);
        }
        if (coverBlurInput_ != coverBmp) {
            coverBlurFx_->SetInput(0, coverBmp);
            coverBlurInput_ = coverBmp;
        }
        const D2D1_SIZE_F size = coverBmp->GetSize();
        if (size.width <= 0.0f || size.height <= 0.0f)
            return nullptr;
        coverScaleFx_->SetValue(D2D1_SCALE_PROP_SCALE,
                                D2D1::Vector2F(w / size.width, h / size.height));
        return coverScaleFx_;
    }

    void decodePlatformIcon() {
        if (platformIconBmp) {
            platformIconBmp->Release();
            platformIconBmp = nullptr;
        }
        if (!albumCoverVisible_ || !platformIconVisible_ || media.sourceAppUserModelId.empty())
            return;

        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!platform_icon::readSourceIconPixels(media.sourceAppUserModelId, pixels, width,
                                                 height))
            return;

        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        ID2D1Bitmap1* decoded = nullptr;
        rt->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4, &props,
                         &decoded);
        platformIconBmp = decoded;
    }

    int displayLyricLine() const {
        if (scene_ == DisplayScene::NoPlayback || scene_ == DisplayScene::Idle ||
            scene_ == DisplayScene::Searching ||
            scene_ == DisplayScene::Message)
            return -1;
        if (currentLine >= 0 && (size_t)currentLine < lines.size())
            return currentLine;
        return lines.empty() ? -1 : 0;
    }

    std::wstring selectedSecondary(const LyricLine& line) const {
        if (translationEnabled_)
            return line.translation;
        if (romanizationEnabled_)
            return line.romanization;
        return {};
    }

    bool useDoubleLineLyricsForLine(int lineIndex) const {
        return doubleLineLyricsEnabled_ && lineIndex >= 0 &&
               static_cast<size_t>(lineIndex) < lines.size() &&
               selectedSecondary(lines[static_cast<size_t>(lineIndex)]).empty();
    }

    bool useDoubleLineLyrics() const {
        if (isLyricTransitionActive() && lyricLayout_)
            return lyricLayoutDoubleLine_;
        return useDoubleLineLyricsForLine(displayLyricLine());
    }

    // ---------- 行过渡状态 ----------

    void releaseOutgoingLyricLayouts() {
        if (outgoingLyricLayout_) {
            outgoingLyricLayout_->Release();
            outgoingLyricLayout_ = nullptr;
        }
        if (outgoingVerticalLyricLayout_) {
            outgoingVerticalLyricLayout_->Release();
            outgoingVerticalLyricLayout_ = nullptr;
        }
        releaseVerticalLyricParts(outgoingVerticalLyricParts_);
        if (outgoingSecondaryLayout_) {
            outgoingSecondaryLayout_->Release();
            outgoingSecondaryLayout_ = nullptr;
        }
        if (outgoingNextLyricLayout_) {
            outgoingNextLyricLayout_->Release();
            outgoingNextLyricLayout_ = nullptr;
        }
        outgoingLyricWidth_ = 0.0f;
        outgoingLyricHeight_ = 0.0f;
        outgoingVerticalLyricWidth_ = 0.0f;
        outgoingVerticalLyricHeight_ = 0.0f;
        outgoingVerticalLyricRotated_ = false;
        outgoingSecondaryWidth_ = 0.0f;
        outgoingSecondaryHeight_ = 0.0f;
        outgoingLyricBlockHeight_ = 0.0f;
        outgoingLyricScrollOffset_ = 0.0f;
        outgoingSecondaryScrollOffset_ = 0.0f;
        outgoingNextLyricWidth_ = 0.0f;
        outgoingNextLyricHeight_ = 0.0f;
    }

    // 丢弃当前行过渡：清空待启动/进行中状态并释放旧布局，下一次排版直接提交最终行。
    void resetLyricTransition() {
        if (isSceneResizeActive()) {
            cancelSceneWindowResize();
            requestInvalidation(RenderInvalidation::Layout);
        }
        clearLyricTransitionPhase();
        lyricTransitionKind_ = LyricTransitionKind::None;
        outgoingScene_ = DisplayScene::NoPlayback;
        outgoingDoubleLine_ = false;
        sceneTransitionNeedsRelayout_ = false;
        lyricTransitionStartMs_ = 0;
        lyricTransitionRevision_ = 0;
        transitionTarget_.reset();
        pendingTarget_.reset();
        requestLyricDCompEnd();
        releaseOutgoingLyricLayouts();
        karaokeEnteringLine_ = false;
    }

    // 行目标变化统一入口：动画进行中只记录最新目标（不重启当前动画），
    // 待启动的过渡直接改打最新目标，空闲时发起新过渡；不允许动画时立即提交最终状态。
    void onLyricLineTargetChanged(int newLine, int64_t actualPositionMs, uint64_t revision,
                                  bool allowAnimate) {
        const int previous = currentLine;
        currentLine = newLine;
        if (!allowAnimate || !clientAnimations_ || previous < 0 || newLine < 0) {
            resetLyricTransition();
            requestInvalidation(RenderInvalidation::Text);
            return;
        }
        // 只有相邻自然换行才做空间转场。seek、快速切行或反向跳转不能把多次
        // 280ms 动画串起来，否则画面会长期追不上真实歌词。
        if (std::abs(newLine - previous) > 1) {
            resetLyricTransition();
            requestInvalidation(RenderInvalidation::Text);
            return;
        }
        LyricTransitionTarget target{newLine, actualPositionMs,
                                     newLine > previous ? 1 : -1, revision};
        if (isLyricTransitionActive()) {
            if (target.direction != lyricTransitionDirection_) {
                resetLyricTransition();
                requestInvalidation(RenderInvalidation::Text);
                return;
            }
            pendingTarget_ = target;
            // 过渡版本跟随最新帧，避免被 updateScroll 的过期检查丢弃。
            lyricTransitionRevision_ = revision;
            return;
        }
        transitionTarget_ = target;
        lyricTransitionKind_ = LyricTransitionKind::Line;
        lyricTransitionDirection_ = target.direction;
        setLyricTransitionPending();
        lyricTransitionRevision_ = revision;
        requestInvalidation(RenderInvalidation::Text);
    }

    // 行过渡收尾：先冻结动画再交换状态。释放旧布局、消费动画期间记录的最新目标；
    // 自然转场中的新行保留逐字追赶过程，避免收尾时又跳到真实位置。
    void finalizeLyricTransition(ULONGLONG now) {
        const bool sceneTransition = lyricTransitionKind_ == LyricTransitionKind::Scene;
        const bool sceneNeedsRelayout = sceneTransitionNeedsRelayout_;
        requestLyricDCompEnd();
        releaseOutgoingLyricLayouts();
        clearLyricTransitionPhase();
        lyricTransitionStartMs_ = 0;

        if (sceneTransition) {
            // 类别转场不消费歌词行的 pendingTarget；场景稳定后用最新完整帧
            // 再做一次排版，避免把转场期间收到的歌词行状态写回旧布局。
            lyricTransitionKind_ = LyricTransitionKind::None;
            outgoingScene_ = DisplayScene::NoPlayback;
            outgoingDoubleLine_ = false;
            sceneTransitionNeedsRelayout_ = false;
            transitionTarget_.reset();
            pendingTarget_.reset();
            lyricTransitionRevision_ = 0;
            if (sceneNeedsRelayout)
                requestInvalidation(RenderInvalidation::Text);
            return;
        }

        if (pendingTarget_) {
            // 动画期间收到了更新的目标行：当前布局仍是旧目标，以它为旧行立即
            // 发起向最新目标的过渡，不经过稳定的中间帧。
            transitionTarget_ = pendingTarget_;
            pendingTarget_.reset();
            lyricTransitionDirection_ = transitionTarget_->direction;
            setLyricTransitionPending();
            lyricTransitionRevision_ = frameRevision_;
            requestInvalidation(RenderInvalidation::Text);
            // 布局尚未对应新行：解绑逐字平滑状态，排版完成后直接对齐真实目标。
            karaokeSmoothLine_ = -1;
            karaokeSettled_ = false;
            karaokeTick_ = now;
            karaokeEnteringLine_ = false;
            return;
        }
        transitionTarget_.reset();
        lyricTransitionRevision_ = 0;

        karaokeTick_ = now;
        const bool preserveKaraokeEntry =
            karaokeEnteringLine_ && karaokeSmoothLine_ == currentLine;
        karaokeSmoothLine_ = currentLine;
        if (const LyricLine* line = karaokeLine()) {
            int64_t charDur = 0;
            const float target = karaokeTargetX(*line, charDur);
            if (preserveKaraokeEntry) {
                karaokeProgX_ = karaokeSmoothX_;
                karaokeSettled_ = std::fabs(target - karaokeSmoothX_) < 0.5f;
            } else {
                karaokeSmoothX_ = target;
                karaokeProgX_ = target;
                karaokeSettled_ = true; // 非自然切换直接对齐真实目标
                karaokeEnteringLine_ = false;
            }
        } else {
            karaokeSmoothX_ = 0.0f;
            karaokeProgX_ = 0.0f;
            karaokeSettled_ = true;
            karaokeEnteringLine_ = false;
        }
        clearLyricTransitionPhase();
        lyricTransitionKind_ = LyricTransitionKind::None;
        outgoingScene_ = DisplayScene::NoPlayback;
        outgoingDoubleLine_ = false;
    }

    // ---------- 排版 ----------

    static bool isCjkVerticalCharacter(wchar_t ch) {
        return (ch >= 0x2E80 && ch <= 0x2FFF) ||
               (ch >= 0x3000 && ch <= 0x30FF) ||
               (ch >= 0x3400 && ch <= 0x9FFF) ||
               (ch >= 0xAC00 && ch <= 0xD7AF);
    }

    static bool isLatinVerticalCharacter(wchar_t ch) {
        return (ch >= L'A' && ch <= L'Z') ||
               (ch >= L'a' && ch <= L'z') ||
               (ch >= 0x00C0 && ch <= 0x024F) ||
               (ch >= 0x1E00 && ch <= 0x1EFF) ||
               (ch >= 0xFF21 && ch <= 0xFF3A) ||
               (ch >= 0xFF41 && ch <= 0xFF5A);
    }

    static bool isLatinRunConnector(wchar_t ch) {
        switch (ch) {
        case L' ':
        case L'\t':
        case L'\'':
        case L'’':
        case L'-':
        case L'‐':
        case L'‑':
        case L'_':
        case L'&':
        case L'+':
        case L'/':
        case L'.':
        case L',':
        case L'!':
        case L'?':
        case L':':
        case L';':
        case L'%':
        case L'#':
        case L'@':
        case L'=':
        case L'(':
        case L')':
        case L'[':
        case L']':
        case L'{':
        case L'}':
            return true;
        default:
            return false;
        }
    }

    static bool isLatinRunCharacter(wchar_t ch) {
        return isLatinVerticalCharacter(ch) || (ch >= L'0' && ch <= L'9') ||
               (ch >= 0xFF10 && ch <= 0xFF19) || isLatinRunConnector(ch);
    }

    static bool isLatinRunStartCharacter(wchar_t ch) {
        return isLatinRunCharacter(ch) && !isLatinRunConnector(ch);
    }

    static wchar_t verticalPunctuationGlyph(wchar_t ch) {
        // 普通括号在逐字竖排时会保持“竖着的原字形”，改用 Unicode 竖排字形，
        // 让括号曲线横向展开。中英文及常见全角/中文成对括号统一处理。
        switch (ch) {
        case L'(':
        case L'（':
            return L'︵';
        case L')':
        case L'）':
            return L'︶';
        case L'[':
        case L'［':
            return L'﹇';
        case L']':
        case L'］':
            return L'﹈';
        case L'{':
        case L'｛':
            return L'︷';
        case L'}':
        case L'｝':
            return L'︸';
        case L'<':
        case L'〈':
        case L'＜':
            return L'︿';
        case L'>':
        case L'〉':
        case L'＞':
            return L'﹀';
        case L'〔':
            return L'︹';
        case L'〕':
            return L'︺';
        case L'【':
            return L'︻';
        case L'】':
            return L'︼';
        case L'《':
            return L'︽';
        case L'》':
            return L'︾';
        case L'「':
            return L'﹁';
        case L'」':
            return L'﹂';
        case L'『':
            return L'﹃';
        case L'』':
            return L'﹄';
        default:
            return ch;
        }
    }

    // 把一行歌词拆成逐字换行的布局。中文和符号按 Unicode 码点处理，
    // 常见括号转换为竖排字形；避免 UTF-16 代理项被拆开后出现半个字符。
    // 纯拉丁文本走整句旋转布局。
    std::wstring makeVerticalLyricText(const std::wstring& text, bool topToBottom) const {
        std::vector<std::wstring> glyphs;
        for (size_t i = 0; i < text.size();) {
            if (text[i] == L'\r' || text[i] == L'\n') {
                ++i;
                continue;
            }
            size_t units = 1;
            const wchar_t first = text[i];
            if (first >= 0xD800 && first <= 0xDBFF && i + 1 < text.size() &&
                text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                units = 2;
            }
            std::wstring glyph = text.substr(i, units);
            if (units == 1)
                glyph[0] = verticalPunctuationGlyph(glyph[0]);
            glyphs.emplace_back(std::move(glyph));
            i += units;
        }
        if (!topToBottom)
            std::reverse(glyphs.begin(), glyphs.end());

        std::wstring result;
        for (const auto& glyph : glyphs) {
            if (!result.empty())
                result.push_back(L'\n');
            result += glyph;
        }
        return result;
    }

    bool useRotatedVerticalLyric(const std::wstring& text) const {
        bool hasLatin = false;
        for (size_t i = 0; i < text.size();) {
            const wchar_t c = text[i];
            if (c == L'\r' || c == L'\n') {
                ++i;
                continue;
            }
            if (c >= 0xD800 && c <= 0xDFFF) {
                // 代理项和 CJK 之外的复杂文字仍走逐字布局，避免旋转后破坏字形组合。
                if (c <= 0xDBFF && i + 1 < text.size() &&
                    text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
                    i += 2;
                else
                    ++i;
                continue;
            }

            if (isCjkVerticalCharacter(c))
                return false;
            hasLatin = hasLatin || isLatinVerticalCharacter(c);
            ++i;
        }
        return hasLatin;
    }

    bool createVerticalLyricLayout(const std::wstring& text, float layoutWidth,
                                   IDWriteTextLayout** out, float& width, float& height,
                                   bool& rotated) {
        width = 0.0f;
        height = 0.0f;
        rotated = false;
        if (!out)
            return false;
        *out = nullptr;
        if (!renderer.dwrite() || !fmtLyric_ || text.empty() || layoutWidth <= 0.0f)
            return false;

        const bool rotateText = useRotatedVerticalLyric(text);
        // 竖向英文统一使用截图中的顺时针 90° 角度；原文本顺序无需反转，
        // 这样视觉上仍按原句从上向下阅读。
        const std::wstring layoutText = rotateText ? text
                                                   : makeVerticalLyricText(text, true);
        if (layoutText.empty() ||
            FAILED(renderer.dwrite()->CreateTextLayout(
                layoutText.c_str(), static_cast<UINT32>(layoutText.size()), fmtLyric_,
                rotateText ? 100000.0f : layoutWidth, 100000.0f, out)) ||
            !*out) {
            return false;
        }

        (*out)->SetTextAlignment(rotateText ? DWRITE_TEXT_ALIGNMENT_LEADING
                                             : DWRITE_TEXT_ALIGNMENT_CENTER);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED((*out)->GetMetrics(&metrics))) {
            (*out)->Release();
            *out = nullptr;
            return false;
        }
        if (rotateText) {
            // 横向整句旋转 90° 后，原文本高度成为竖栏宽度，原文本宽度成为滚动长度。
            width = metrics.height;
            height = metrics.width;
            rotated = true;
        } else {
            // metrics.width 是字符墨宽，而不是布局槽宽；绘制/命中需要保留完整的
            // 任务栏横向槽宽，才能让每个字在窄栏中保持居中。
            width = layoutWidth;
            height = metrics.height;
        }
        return height > 0.0f;
    }

    void releaseVerticalLyricParts(std::vector<VerticalLyricPart>& parts) {
        for (auto& part : parts) {
            if (part.layout)
                part.layout->Release();
        }
        parts.clear();
    }

    bool hasMixedVerticalLyric(const std::wstring& text) const {
        bool hasLatin = false;
        bool hasCjk = false;
        for (size_t i = 0; i < text.size();) {
            const wchar_t c = text[i];
            if (c == L'\r' || c == L'\n') {
                ++i;
                continue;
            }
            if (c >= 0xD800 && c <= 0xDFFF) {
                if (c <= 0xDBFF && i + 1 < text.size() &&
                    text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
                    i += 2;
                else
                    ++i;
                continue;
            }
            hasLatin = hasLatin || isLatinVerticalCharacter(c);
            hasCjk = hasCjk || isCjkVerticalCharacter(c);
            if (hasLatin && hasCjk)
                return true;
            ++i;
        }
        return false;
    }

    bool createMixedVerticalLyricLayout(const std::wstring& text, float layoutWidth,
                                        std::vector<VerticalLyricPart>& parts, float& width,
                                        float& height) {
        releaseVerticalLyricParts(parts);
        width = layoutWidth;
        height = 0.0f;
        if (!renderer.dwrite() || !fmtLyric_ || text.empty() || layoutWidth <= 0.0f)
            return false;

        IDWriteFactory* dwrite = renderer.dwrite();
        auto appendPart = [&](const std::wstring& partText, bool rotated) {
            if (partText.empty())
                return true;

            IDWriteTextLayout* layout = nullptr;
            const HRESULT hr = dwrite->CreateTextLayout(
                partText.c_str(), static_cast<UINT32>(partText.size()), fmtLyric_,
                rotated ? 100000.0f : layoutWidth, 100000.0f, &layout);
            if (FAILED(hr) || !layout) {
                if (layout)
                    layout->Release();
                return false;
            }
            layout->SetTextAlignment(rotated ? DWRITE_TEXT_ALIGNMENT_LEADING
                                              : DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(layout->GetMetrics(&metrics))) {
                layout->Release();
                return false;
            }
            const float partWidth = rotated ? metrics.height : layoutWidth;
            const float partHeight = rotated ? metrics.width : metrics.height;
            if (partHeight <= 0.0f) {
                layout->Release();
                return true;
            }
            parts.push_back({layout, partWidth, partHeight, rotated});
            height += partHeight;
            return true;
        };

        bool created = true;
        for (size_t i = 0; i < text.size();) {
            if (text[i] == L'\r' || text[i] == L'\n') {
                ++i;
                continue;
            }

            if (isLatinRunStartCharacter(text[i])) {
                const size_t start = i;
                size_t end = i;
                while (end < text.size()) {
                    if (text[end] == L'\r' || text[end] == L'\n')
                        break;
                    if (text[end] >= 0xD800 && text[end] <= 0xDFFF)
                        break;
                    if (!isLatinRunCharacter(text[end]))
                        break;
                    ++end;
                }
                size_t partEnd = end;
                while (partEnd > start &&
                       (text[partEnd - 1] == L' ' || text[partEnd - 1] == L'\t'))
                    --partEnd;
                if (partEnd > start && !appendPart(text.substr(start, partEnd - start), true)) {
                    created = false;
                    break;
                }
                i = end;
                continue;
            }

            size_t units = 1;
            const wchar_t first = text[i];
            if (first >= 0xD800 && first <= 0xDBFF && i + 1 < text.size() &&
                text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                units = 2;
            }
            std::wstring glyph = text.substr(i, units);
            if (units == 1)
                glyph[0] = verticalPunctuationGlyph(glyph[0]);
            if (!appendPart(glyph, false)) {
                created = false;
                break;
            }
            i += units;
        }

        if (!created || parts.empty() || height <= 0.0f) {
            releaseVerticalLyricParts(parts);
            width = 0.0f;
            height = 0.0f;
            return false;
        }
        return true;
    }

    void buildTextLayouts(float leftW, float rightW) {
        // 文本布局以超宽无换行创建，度量与区域宽度无关，leftW/rightW 仅保留签名兼容
        (void)leftW;
        (void)rightW;
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle_ || !fmtArtist_ || !fmtLyric_)
            return;

        // 歌曲信息（标题/歌手）独立重建：换行只走下面的歌词分支，
        // 歌曲信息变化也不触碰歌词布局与行过渡状态
        if (isInvalidated(RenderInvalidation::SongInfo)) {
            if (titleLayout_) {
                titleLayout_->Release();
                titleLayout_ = nullptr;
            }
            if (artistLayout_) {
                artistLayout_->Release();
                artistLayout_ = nullptr;
            }
            titleWidth_ = 0.0f;
            titleHeight_ = 0.0f;
            artistWidth_ = 0.0f;
            artistHeight_ = 0.0f;
            if (songInfoVisible_ && !media.title.empty()) {
                dwrite->CreateTextLayout(media.title.c_str(), (UINT32)media.title.size(),
                                         fmtTitle_, 100000.0f, 40.0f, &titleLayout_);
                if (titleLayout_) {
                    DWRITE_TEXT_METRICS m{};
                    titleLayout_->GetMetrics(&m);
                    titleWidth_ = m.width;
                    titleHeight_ = m.height;
                }
            }
            if (songInfoVisible_ && !media.artist.empty()) {
                dwrite->CreateTextLayout(media.artist.c_str(), (UINT32)media.artist.size(),
                                         fmtArtist_, 100000.0f, 40.0f, &artistLayout_);
                if (artistLayout_) {
                    DWRITE_TEXT_METRICS m{};
                    artistLayout_->GetMetrics(&m);
                    artistWidth_ = m.width;
                    artistHeight_ = m.height;
                }
            }
            const bool titleChanged = media.title != lastTitle_;
            const bool artistChanged = media.artist != lastArtist_;
            if (titleChanged) {
                titleScrollOffset_ = 0.0f;
                lastTitle_ = media.title;
            }
            if (artistChanged) {
                artistScrollOffset_ = 0.0f;
                lastArtist_ = media.artist;
            }
            if (titleChanged || artistChanged)
                lastTickMs_ = 0;
        }
        if (!isInvalidated(RenderInvalidation::Text))
            return;
        ++textFxGen_; // 布局指针重建，离屏缓存全部失效
        karaokeSpans_.clear();
        karaokeGeometryLine_ = -1;
        karaokeGeometryLayout_ = nullptr;

        const bool doubleLineLyrics = useDoubleLineLyricsForLine(displayLyricLine());
        // 准备阶段：先把当前布局移交为旧行，目标行布局构建完成后才记录动画起点，
        // 避免“新布局已替换但动画初始位置还没准备好”导致的文字瞬移。
        bool preparedTransition = false;
        if (isLyricTransitionPending() && lyricLayout_) {
            // 保留旧行离场前的滚动位置。新布局后面会把 lyricScrollOffset_ 重置为 0，
            // 不能让旧的超长歌词因此在转场第一帧跳回开头。
            const float outgoingLyricOffset = lyricScrollOffset_;
            const float outgoingSecondaryOffset = secondaryScrollOffset_;
            const bool outgoingDoubleLine = lyricLayoutDoubleLine_;
            if (outgoingLyricLayout_)
                outgoingLyricLayout_->Release();
            outgoingLyricLayout_ = lyricLayout_;
            lyricLayout_ = nullptr;
            outgoingLyricWidth_ = lyricWidth_;
            outgoingLyricHeight_ = lyricHeight_;
            if (outgoingVerticalLyricLayout_)
                outgoingVerticalLyricLayout_->Release();
            releaseVerticalLyricParts(outgoingVerticalLyricParts_);
            outgoingVerticalLyricLayout_ = verticalLyricLayout_;
            verticalLyricLayout_ = nullptr;
            outgoingVerticalLyricParts_.swap(verticalLyricParts_);
            outgoingVerticalLyricWidth_ = verticalLyricWidth_;
            outgoingVerticalLyricHeight_ = verticalLyricHeight_;
            outgoingVerticalLyricRotated_ = verticalLyricRotated_;
            outgoingLyricScrollOffset_ = outgoingLyricOffset;
            if (outgoingSecondaryLayout_)
                outgoingSecondaryLayout_->Release();
            if (outgoingNextLyricLayout_)
                outgoingNextLyricLayout_->Release();
            outgoingSecondaryLayout_ = nullptr;
            outgoingNextLyricLayout_ = nullptr;
            outgoingSecondaryWidth_ = 0.0f;
            outgoingSecondaryHeight_ = 0.0f;
            outgoingNextLyricWidth_ = 0.0f;
            outgoingNextLyricHeight_ = 0.0f;
            outgoingDoubleLine_ = outgoingDoubleLine;
            if (outgoingDoubleLine) {
                // 双行模式的旧层需要保留下一句预览，才能在切到辅助歌词时
                // 继续绘制完整的离场块。
                outgoingNextLyricLayout_ = nextLyricLayout_;
                nextLyricLayout_ = nullptr;
                outgoingNextLyricWidth_ = nextLyricWidth_;
                outgoingNextLyricHeight_ = nextLyricHeight_;
                if (secondaryLayout_)
                    secondaryLayout_->Release();
                secondaryLayout_ = nullptr;
            } else {
                if (nextLyricLayout_)
                    nextLyricLayout_->Release();
                nextLyricLayout_ = nullptr;
                outgoingSecondaryLayout_ = secondaryLayout_;
                secondaryLayout_ = nullptr;
                outgoingSecondaryWidth_ = secondaryWidth_;
                outgoingSecondaryHeight_ = secondaryHeight_;
                outgoingSecondaryScrollOffset_ = outgoingSecondaryOffset;
            }
            const float outgoingPreviewH =
                outgoingDoubleLine
                    ? outgoingNextLyricLayout_ ? kLyricPreviewGap + outgoingNextLyricHeight_
                                                : 0.0f
                    : outgoingSecondaryLayout_ ? 1.0f + outgoingSecondaryHeight_ : 0.0f;
            outgoingLyricBlockHeight_ = outgoingLyricHeight_ + outgoingPreviewH;
            preparedTransition = true;
        } else {
            resetLyricTransition();
            if (lyricLayout_)
                lyricLayout_->Release();
            if (verticalLyricLayout_)
                verticalLyricLayout_->Release();
            releaseVerticalLyricParts(verticalLyricParts_);
            if (secondaryLayout_)
                secondaryLayout_->Release();
            lyricLayout_ = nullptr;
            secondaryLayout_ = nullptr;
        }
        // 排版已接管待启动转场；成功构建目标布局后会转换为 Running。

        if (nextLyricLayout_) {
            nextLyricLayout_->Release();
            nextLyricLayout_ = nullptr;
        }
        nextLyricWidth_ = 0.0f;
        nextLyricHeight_ = 0.0f;

        int displayLine = displayLyricLine();
        std::wstring lyric;
        std::wstring secondary;
        if (displayLine >= 0) {
            lyric = lines[(size_t)displayLine].text;
            secondary = selectedSecondary(lines[(size_t)displayLine]);
        } else {
            lyric = statusText;
        }
        lyricLayout_ = nullptr;
        lyricWidth_ = 0.0f;
        lyricHeight_ = 0.0f;
        verticalLyricLayout_ = nullptr;
        verticalLyricParts_.clear();
        verticalLyricWidth_ = 0.0f;
        verticalLyricHeight_ = 0.0f;
        verticalLyricRotated_ = false;
        secondaryWidth_ = 0.0f;
        secondaryHeight_ = 0.0f;
        if (!lyric.empty()) {
            // 用足够大的宽度创建布局以准确测量文本宽度
            dwrite->CreateTextLayout(lyric.c_str(), (UINT32)lyric.size(), fmtLyric_, 100000.0f,
                                     100.0f, &lyricLayout_);
            if (lyricLayout_) {
                lyricLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TEXT_METRICS m{};
                lyricLayout_->GetMetrics(&m);
                lyricWidth_ = m.width;
                lyricHeight_ = m.height;
            }

            if (isVerticalTaskbar()) {
                int verticalPxW = 0;
                int verticalPxH = 0;
                clientPixelSize(verticalPxW, verticalPxH);
                const float verticalWidth = dip(verticalPxW);
                if (hasMixedVerticalLyric(lyric)) {
                    createMixedVerticalLyricLayout(lyric, verticalWidth, verticalLyricParts_,
                                                   verticalLyricWidth_, verticalLyricHeight_);
                } else {
                    createVerticalLyricLayout(lyric, verticalWidth, &verticalLyricLayout_,
                                              verticalLyricWidth_, verticalLyricHeight_,
                                              verticalLyricRotated_);
                }
            }
        }
        if (!doubleLineLyrics && !secondary.empty() && fmtSecondary_) {
            dwrite->CreateTextLayout(secondary.c_str(), (UINT32)secondary.size(), fmtSecondary_,
                                     100000.0f, 100.0f, &secondaryLayout_);
            if (secondaryLayout_) {
                secondaryLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TEXT_METRICS m{};
                secondaryLayout_->GetMetrics(&m);
                secondaryWidth_ = m.width;
                secondaryHeight_ = m.height;
            }
        }
        if (doubleLineLyrics && fmtNextLyric_ && displayLine >= 0 &&
            static_cast<size_t>(displayLine + 1) < lines.size() &&
            !lines[(size_t)displayLine + 1].text.empty()) {
            int nextLine = displayLine + 1;
            const std::wstring& nextText = lines[(size_t)nextLine].text;
            dwrite->CreateTextLayout(nextText.c_str(), (UINT32)nextText.size(), fmtNextLyric_,
                                     100000.0f, 100.0f, &nextLyricLayout_);
            if (nextLyricLayout_) {
                nextLyricLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TEXT_METRICS m{};
                nextLyricLayout_->GetMetrics(&m);
                nextLyricWidth_ = m.width;
                nextLyricHeight_ = m.height;
            }
        }
        if (isMinimalMode()) {
            // 极简只改变显示策略；歌词行和逐字时间轴仍由上游完整读取，
            // 这里只不创建供逐字绘制使用的几何缓存。
            karaokeSpans_.clear();
            karaokeGeometryLine_ = -1;
            karaokeGeometryLayout_ = nullptr;
        } else {
            buildKaraokeGeometry(displayLine);
        }
        lyricLayoutDoubleLine_ = doubleLineLyrics;
        if (preparedTransition) {
            if (lyricLayout_) {
                // 目标布局就绪后才启动动画计时：准备布局的这一帧不消耗过渡时长。
                lyricTransitionStartMs_ = monotonicNowMs();
                setLyricTransitionActive();
                lyricTransitionRevision_ = frameRevision_;
            } else {
                // 目标行没有可绘制布局：放弃过渡，直接回到稳定状态。
                resetLyricTransition();
            }
        }
        // 动态滚动速度：让歌词在当前行时长内滚动完一圈。两句间隔越小速度越快，
        // 间隔大到算出的速度低于 kLyricScrollSpeed 时保持最慢速度不变
        lyricScrollSpeed_ = kLyricScrollSpeed;
        const float lyricScrollExtent =
            isVerticalTaskbar() && verticalLyricHeight_ > 0.0f ? verticalLyricHeight_
                                                               : lyricWidth_;
        const float lyricScrollLoopGap = isVerticalTaskbar() ? kVerticalLyricGap
                                                              : kTextPadding * 2.0f;
        if (currentLine >= 0 && (size_t)currentLine + 1 < lines.size() &&
            lyricScrollExtent > 0.0f) {
            int64_t durMs =
                lines[(size_t)currentLine + 1].ms - lines[(size_t)currentLine].ms;
            if (durMs > 0) {
                float loopW = lyricScrollExtent + lyricScrollLoopGap;
                lyricScrollSpeed_ =
                    std::max(kLyricScrollSpeed, loopW / (static_cast<float>(durMs) / 1000.0f));
            }
        }
        bool lyricChanged = lyric != lastLyric_;
        bool secondaryChanged = secondary != lastSecondary_;
        if (lyricChanged) {
            lyricScrollOffset_ = 0.0f;
            lastLyric_ = lyric;
        }
        if (secondaryChanged) {
            secondaryScrollOffset_ = 0.0f;
            lastSecondary_ = secondary;
        }
        if (lyricChanged || secondaryChanged) {
            lastTickMs_ = 0;
        }
    }

    float coverSize() const {
        if (!hwnd)
            return 24.0f;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const float h = dip(isVerticalTaskbar() ? pxW : pxH);
        return h - kCoverPadding * 2.0f;
    }

    float coverSlotWidth(float heightDip) const {
        return albumCoverVisible_ ? heightDip - kCoverPadding : 0.0f;
    }

    float infoStartX() const {
        return albumCoverVisible_ ? kCoverPadding + coverSize() + kCoverPadding : kTextPadding;
    }

    float lyricStartPadding() const {
        return songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap
                                                                 : kTextPadding;
    }

    float immersiveControlRadius(float heightDip) const {
        return std::clamp(heightDip * kImmersiveControlRadiusFactor,
                          kImmersiveControlMinRadius, kImmersiveControlMaxRadius);
    }

    float immersiveControlsWidth(float heightDip) const {
        const float radius = immersiveControlRadius(heightDip);
        const float pitch = radius * kImmersiveControlPitchFactor;
        const float groupWidth =
            pitch * static_cast<float>(expandedControlCount() - 1) + radius * 2.0f;
        return groupWidth + kTextPadding * 2.0f;
    }

    struct LayoutMetrics {
        float w = 0.0f;
        float h = 0.0f;
        float leftW = 0.0f;
        float rightW = 0.0f;
        float immersiveControlsW = 0.0f;
    };

    struct VerticalLayout {
        float w = 0.0f;
        float h = 0.0f;
        float coverX = 0.0f;
        float coverY = 0.0f;
        float coverSize = 0.0f;
        float lyricY = 0.0f;
        float lyricBottom = 0.0f;
        bool showControls = false;
        float controlRadius = 0.0f;
        D2D1_POINT_2F controlCenters[4]{};
    };

    VerticalLayout verticalLayout() const {
        VerticalLayout layout;
        int pxW = 0;
        int pxH = 0;
        clientPixelSize(pxW, pxH);
        layout.w = dip(pxW);
        layout.h = dip(pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return layout;

        const float pad = std::min(kVerticalRailPadding, layout.w * 0.18f);
        const float contentW = std::max(1.0f, layout.w - pad * 2.0f);
        const bool playbackScene = scene_ != DisplayScene::Idle &&
                                    scene_ != DisplayScene::NoPlayback;
        if (albumCoverVisible_ && playbackScene) {
            layout.coverSize = contentW;
            layout.coverX = (layout.w - layout.coverSize) * 0.5f;
            layout.coverY = pad;
        }

        layout.lyricY = layout.coverSize > 0.0f
                            ? layout.coverY + layout.coverSize + kVerticalCoverGap
                            : pad;
        layout.lyricBottom = std::max(layout.lyricY + 1.0f, layout.h - pad);
        layout.showControls = mouseOver_ && controlsOnHover_ &&
                              hoverControlStyle_ == HoverControlStyle::Inline && playbackScene;
        if (layout.showControls) {
            layout.controlRadius = std::clamp(layout.w * 0.23f, 6.0f, 9.0f);
            const float diameter = layout.controlRadius * 2.0f;
            // 与横向内嵌控件复用同一套视觉节奏：按钮中心间距约为 2.8r。
            const float transportPitch = layout.controlRadius * 2.8f;
            const float transportGap = std::max(kVerticalControlsGap, transportPitch - diameter);
            // 音量是独立的辅助操作，与上一首/播放/下一首之间留出更明显的层级间距。
            const float volumeGap = transportGap + layout.controlRadius * 0.9f;
            const float blockH = diameter * 4.0f + transportGap * 2.0f + volumeGap;
            const float controlsTop = layout.lyricY;
            const float controlsBottom = layout.h - pad;
            const float freeSpace = controlsBottom - controlsTop - blockH;
            const float groupTop = controlsTop + std::max(0.0f, freeSpace) * 0.5f;
            const float firstY = groupTop + layout.controlRadius;
            if (freeSpace >= 0.0f && groupTop - kVerticalLyricGap > layout.lyricY) {
                for (int i = 0; i < 3; ++i)
                    layout.controlCenters[i] =
                        D2D1::Point2F(layout.w * 0.5f, firstY + i * transportPitch);
                layout.controlCenters[3] = D2D1::Point2F(
                    layout.w * 0.5f, firstY + transportPitch * 2.0f + diameter + volumeGap);
                layout.lyricBottom = groupTop - kVerticalLyricGap;
            } else {
                layout.showControls = false;
            }
        }
        return layout;
    }

    LayoutMetrics layoutMetricsForScene(DisplayScene scene, int pxW, int pxH) const {
        LayoutMetrics m;
        m.w = dip(pxW);
        m.h = dip(pxH);
        const bool immersiveControls = isExpandedView() &&
                                       !isVerticalTaskbar();
        m.immersiveControlsW = immersiveControls ? immersiveControlsWidth(m.h) : 0.0f;
        if (scene == DisplayScene::Idle) {
            m.leftW = 0.0f;
            m.rightW = std::max(
                1.0f, m.w - m.immersiveControlsW - spectrumExtraForScene(scene, m.w));
            return m;
        }
        float effW = m.w - spectrumExtraForScene(scene, m.w);
        const float immersiveStart = songInfoVisible_ ? kSongInfoLyricGap : kTextPadding;
        const float contentW = std::max(
            1.0f, effW - m.immersiveControlsW -
                      (immersiveControls ? immersiveStart : 0.0f));
        if (isExpandedView() && songInfoVisible_) {
            // 沉浸模式的宽度来自整个任务栏，歌曲信息只保留稳定的左侧栏，
            // 把主要空间留给歌词和右侧频谱，避免 1920px 任务栏出现过宽信息区。
            const float available = contentW;
            const float minimum = albumCoverVisible_
                                      ? kCoverPadding + coverSize() + kTextPadding + 80.0f
                                      : 140.0f;
            const float preferred = std::clamp(available * 0.24f, 180.0f, 280.0f);
            m.leftW = std::min(available, std::max(minimum, preferred));
        } else if (isExpandedView()) {
            // 没有歌曲信息时，只有实际显示的封面需要占用左侧安全区。
            // 这样切换封面不会改变歌词的中心锚点，也不会给控件留下无意义的空洞。
            m.leftW = albumCoverVisible_ ? coverSlotWidth(m.h) : 0.0f;
        } else {
            m.leftW = songInfoVisible_ ? contentW * kLeftRatio : coverSlotWidth(m.h);
        }
        m.rightW = std::max(1.0f, m.w - m.leftW - m.immersiveControlsW);
        return m;
    }

    LayoutMetrics layoutMetrics(int pxW, int pxH) const {
        return layoutMetricsForScene(scene_, pxW, pxH);
    }

    struct LyricArea {
        float x = 0.0f;
        float w = 0.0f;
    };

    LyricArea lyricAreaForScene(DisplayScene scene, int pxW, int pxH) const {
        const LayoutMetrics layout = layoutMetricsForScene(scene, pxW, pxH);
        const float start = songInfoVisible_ && scene != DisplayScene::Idle ? kSongInfoLyricGap
                                                                             : kTextPadding;
        // 这里返回的是“可绘制安全区”，不再把它当作歌词的居中基准。
        // 右边界统一落在频谱之前，左边界统一落在封面/歌曲信息/沉浸控件之后。
        float left = layout.leftW + start + layout.immersiveControlsW;
        if (isAppBarView() && scene != DisplayScene::Idle && isSessionVisible() &&
            !isMinimalMode() && !isStoppedMode())
            left += kDockPetSeatW;
        const float right = layout.w - spectrumExtraForScene(scene, layout.w) - kTextPadding;
        return {left, std::max(1.0f, right - left)};
    }

    // 沉浸模式始终以整条任务栏的几何中心作为歌词锚点。封面、歌曲信息、
    // 操作控件、频谱及时钟只参与安全区和裁剪；当空间不足时由绘制路径
    // 把文字约束在安全区内，不能反过来改变正常歌词的中心位置。
    float immersiveLyricCenterX() const {
        if (!isExpandedView() || isVerticalTaskbar())
            return -1.0f;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return -1.0f;
        return layout.w * 0.5f;
    }

    // 当前行有逐字时间轴且歌词布局对应该行时返回该行；极简模式强制使用普通横向滚动。
    const LyricLine* karaokeLine() const {
        if (isMinimalMode())
            return nullptr;
        if (currentLine < 0 || (size_t)currentLine >= lines.size())
            return nullptr;
        const LyricLine* line = &lines[(size_t)currentLine];
        if (line->chars.empty() || !lyricLayout_ || line->text != lastLyric_)
            return nullptr;
        return line;
    }

    void buildKaraokeGeometry(int lineIndex) {
        karaokeSpans_.clear();
        karaokeGeometryLine_ = -1;
        karaokeGeometryLayout_ = nullptr;
        if (lineIndex < 0 || (size_t)lineIndex >= lines.size() || !lyricLayout_)
            return;

        const LyricLine& line = lines[(size_t)lineIndex];
        if (line.chars.empty())
            return;

        UINT32 textOffset = 0;
        karaokeSpans_.reserve(line.chars.size());
        for (const LyricChar& c : line.chars) {
            const UINT32 textLength = static_cast<UINT32>(c.text.size());
            if (textLength == 0)
                continue;

            DWRITE_HIT_TEST_METRICS metrics{};
            float startX = 0.0f;
            float y = 0.0f;
            if (FAILED(lyricLayout_->HitTestTextPosition(textOffset, FALSE, &startX, &y,
                                                          &metrics))) {
                textOffset += textLength;
                continue;
            }

            float endX = startX;
            if (FAILED(lyricLayout_->HitTestTextPosition(textOffset + textLength - 1, TRUE,
                                                          &endX, &y, &metrics)))
                endX = startX;
            karaokeSpans_.push_back({c.startMs, c.endMs, startX, endX});
            textOffset += textLength;
        }
        karaokeGeometryLine_ = lineIndex;
        karaokeGeometryLayout_ = lyricLayout_;
    }

    // 逐字填充目标进度 x（布局像素坐标）：已唱边界 + 当前 token 按时长比例推进；
    // durOut 输出当前 token 时长（ms），供平滑时间常数使用。X 坐标在排版时缓存，
    // 播放过程中只做时间定位，不再每帧调用 DirectWrite 命中测试。
    float karaokeTargetX(const LyricLine& line, int64_t& durOut) const {
        (void)line;
        durOut = 0;
        if (karaokeGeometryLine_ != currentLine || karaokeGeometryLayout_ != lyricLayout_ ||
            karaokeSpans_.empty())
            return 0.0f;

        auto it = std::upper_bound(
            karaokeSpans_.begin(), karaokeSpans_.end(), positionMs_,
            [](int64_t position, const KaraokeSpan& span) { return position < span.startMs; });
        if (it == karaokeSpans_.begin())
            return 0.0f;
        --it;

        durOut = std::max<int64_t>(it->endMs - it->startMs, 1);
        float frac =
            (float)std::clamp((double)(positionMs_ - it->startMs) / (double)durOut, 0.0, 1.0);
        return it->startX + (it->endX - it->startX) * frac;
    }

    // 平滑步进：过渡时间常数取当前字时长的 1/4（40~200ms），指数趋近目标，
    // 每个字的过渡快慢随其时长自然变化，且同步误差有界（约 τ）。
    // SMTC 锚点校正造成的目标抖动经低通后不再闪烁；非自然切换或大幅 seek 直接对齐，
    // 自然转场中的新行则从首字平滑追赶真实位置。
    float karaokeSmoothStep(const LyricLine& line) {
        int64_t charDur = 0;
        float target = karaokeTargetX(line, charDur);
        if (!clientAnimations_) {
            karaokeTick_ = monotonicNowMs();
            karaokeSmoothLine_ = currentLine;
            karaokeSmoothX_ = target;
            karaokeProgX_ = target;
            karaokeSettled_ = true;
            karaokeEnteringLine_ = false;
            return target;
        }
        ULONGLONG now = monotonicNowMs();
        float dt = karaokeTick_ ? (float)(now - karaokeTick_) : 16.7f;
        karaokeTick_ = now;
        float gap = target - karaokeSmoothX_;
        const bool lineChanged = karaokeSmoothLine_ != currentLine;
        if (lineChanged) {
            karaokeSmoothLine_ = currentLine;
            karaokeEnteringLine_ = isLyricTransitionActive();
            karaokeSmoothX_ = karaokeEnteringLine_ ? 0.0f : target;
        } else if (std::fabs(gap) > 100.0f && !karaokeEnteringLine_) {
            karaokeSmoothX_ = target;
        } else {
            float tau = std::clamp((float)charDur * 0.25f, 40.0f, 200.0f);
            float alpha = 1.0f - std::exp(-dt / tau);
            karaokeSmoothX_ += gap * alpha;
        }
        karaokeProgX_ = karaokeSmoothX_;
        karaokeSettled_ = std::fabs(target - karaokeSmoothX_) < 0.5f;
        if (karaokeSettled_ && !isLyricTransitionActive())
            karaokeEnteringLine_ = false;
        return karaokeProgX_;
    }

    float vinylInnerRadius(float s) const {
        float outerRadius = std::max(1.0f, s * 0.5f - 0.5f);
        float recordRadius = std::max(0.5f, outerRadius -
                                                    std::min(kVinylHaloWidth, outerRadius * 0.25f));
        return std::min(s * kVinylInnerRatio, std::max(1.0f, recordRadius - 1.0f));
    }

    // ---------- 图标几何 ----------

    void ensureGeometry() {
        if (!isInvalidated(RenderInvalidation::Geometry))
            return;
        ID2D1Factory* d2d = renderer.d2d();
        if (!d2d)
            return;

        if (coverClip_) {
            coverClip_->Release();
            coverClip_ = nullptr;
        }
        if (vinylCoverClip_) {
            vinylCoverClip_->Release();
            vinylCoverClip_ = nullptr;
        }
        media_control::release(controlGeometry);

        float s = coverSize();
        D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, s, s), 4.0f, 4.0f};
        d2d->CreateRoundedRectangleGeometry(rr, &coverClip_);
        if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
            D2D1_ELLIPSE coverEllipse{
                D2D1::Point2F(s * 0.5f, s * 0.5f), vinylInnerRadius(s), vinylInnerRadius(s)};
            d2d->CreateEllipseGeometry(coverEllipse, &vinylCoverClip_);
        }

        if (!media_control::create(d2d, controlGeometry))
            requestInvalidation(RenderInvalidation::Geometry);
    }

    // ---------- 渲染 ----------

    // ---------- 内嵌悬浮控件布局 ----------

    // 上一首/播放/下一首 + 音量按钮整体居中：centers[0..2] 为播放控制，
    // centers[3] 为音量按钮。返回 false 表示当前不在内嵌控件展示状态。
    bool inlineControlsLayout(float centers[4], float& cy, float& r) const {
        if (isVerticalTaskbar())
            return false;
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return false;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return false;
        cy = layout.h * 0.5f;
        r = layout.h * 0.26f;
        const float spacing = r * 2.8f;
        // 音量按钮与下一首之间多留 0.9r：音量字形比三角形宽，等间距会显得挤
        const float volumeGap = spacing + r * 0.9f;
        // 组跨度 [cx-spacing, cx+spacing+volumeGap]，整体居中即 cx 左移 volumeGap/2
        const float cx = layout.leftW + layout.rightW * 0.5f - volumeGap * 0.5f;
        for (int i = 0; i < 3; ++i)
            centers[i] = cx + (i - 1) * spacing;
        centers[3] = cx + spacing + volumeGap;
        return true;
    }

    // 沉浸模式专属控件：始终显示在歌曲信息分隔线右侧，不受普通任务栏歌词
    // 的悬浮控件开关和样式设置影响。控件组的几何区域也会从歌词区预先扣除。
    bool immersiveControlsLayout(D2D1_POINT_2F centers[kExpandedControlCount], float& cy,
                                 float& r) const {
        if (!isExpandedView() || isVerticalTaskbar())
            return false;

        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f || layout.immersiveControlsW <= 0.0f)
            return false;

        const float start = songInfoVisible_ && scene_ != DisplayScene::Idle
                                ? kSongInfoLyricGap
                                : kTextPadding;
        r = immersiveControlRadius(layout.h);
        const float pitch = r * kImmersiveControlPitchFactor;
        const float groupW =
            pitch * static_cast<float>(expandedControlCount() - 1) + r * 2.0f;
        const float groupLeft = layout.leftW + start +
                                std::max(0.0f, (layout.immersiveControlsW - groupW) * 0.5f);
        cy = layout.h * 0.5f;
        int visibleIndex = 0;
        for (int i = 0; i < kExpandedControlCount; ++i) {
            if (!expandedControlVisible(i))
                continue;
            centers[i] = D2D1::Point2F(groupLeft + r + visibleIndex * pitch, cy);
            ++visibleIndex;
        }
        return true;
    }

    int hitImmersiveControl(float x, float y) const {
        D2D1_POINT_2F centers[kExpandedControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!immersiveControlsLayout(centers, cy, r))
            return -1;

        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        for (int i = 0; i < kExpandedControlCount; ++i) {
            if (!expandedControlVisible(i))
                continue;
            bool enabled = true;
            switch (i) {
            case kImmersiveControlPrevious:
                enabled = media.canPrev;
                break;
            case kImmersiveControlPlayPause:
                enabled = media.canPlayPause;
                break;
            case kImmersiveControlNext:
                enabled = media.canNext;
                break;
            case kDockControlQuickApps:
                enabled = static_cast<bool>(onDockQuickApps);
                break;
            case kImmersiveControlApps:
                enabled = static_cast<bool>(onAppCollection);
                break;
            case kImmersiveControlMenu:
                enabled = static_cast<bool>(onImmersiveMenu);
                break;
            case kImmersiveControlTray:
                enabled = static_cast<bool>(taskbar_);
                break;
            default:
                break;
            }
            if (!enabled)
                continue;
            if (std::hypot(logicalX - centers[i].x, logicalY - centers[i].y) <= r + 4.0f)
                return i;
        }
        return -1;
    }

    void drawExitImmersiveButton(const D2D1_POINT_2F& center, float radius) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushBtn_)
            return;

        // 视图边框配合两支向内收起的箭头，明确表达“退出沉浸模式”，
        // 同时避开托盘“退出程序”所使用的门与外向箭头语义。
        const float frame = radius * 0.74f;
        const float start = radius * 0.50f;
        const float end = radius * 0.08f;
        const float head = radius * 0.22f;
        const float stroke = std::clamp(radius * 0.14f, 1.1f, 1.7f);

        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(center.x - frame, center.y - frame,
                            center.x + frame, center.y + frame),
                radius * 0.15f, radius * 0.15f),
            brushBtn_, stroke);

        const auto topLeftEnd = D2D1::Point2F(center.x - end, center.y - end);
        rt->DrawLine(D2D1::Point2F(center.x - start, center.y - start),
                     topLeftEnd, brushBtn_, stroke);
        rt->DrawLine(topLeftEnd,
                     D2D1::Point2F(topLeftEnd.x - head, topLeftEnd.y), brushBtn_, stroke);
        rt->DrawLine(topLeftEnd,
                     D2D1::Point2F(topLeftEnd.x, topLeftEnd.y - head), brushBtn_, stroke);

        const auto bottomRightEnd = D2D1::Point2F(center.x + end, center.y + end);
        rt->DrawLine(D2D1::Point2F(center.x + start, center.y + start),
                     bottomRightEnd, brushBtn_, stroke);
        rt->DrawLine(bottomRightEnd,
                     D2D1::Point2F(bottomRightEnd.x + head, bottomRightEnd.y), brushBtn_, stroke);
        rt->DrawLine(bottomRightEnd,
                     D2D1::Point2F(bottomRightEnd.x, bottomRightEnd.y + head), brushBtn_, stroke);
    }

    void drawImmersiveControls() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        D2D1_POINT_2F centers[kExpandedControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!immersiveControlsLayout(centers, cy, r))
            return;

        if (brushHover_) {
            for (int i = 0; i < kExpandedControlCount; ++i) {
                if (expandedControlVisible(i) && immersiveControlHover_ == i)
                    rt->FillEllipse(D2D1::Ellipse(centers[i], r + 4.0f, r + 4.0f),
                                    brushHover_);
            }
        }
        drawButton(kImmersiveControlPrevious, centers[kImmersiveControlPrevious], r);
        drawButton(kImmersiveControlPlayPause, centers[kImmersiveControlPlayPause], r);
        drawButton(kImmersiveControlNext, centers[kImmersiveControlNext], r);
        drawVolumeButton(centers[kImmersiveControlVolume], r);
        drawExitImmersiveButton(centers[kImmersiveControlExit], r);
        if (expandedControlVisible(kDockControlQuickApps)) {
            settings_icon::draw(
                rt, settings_icon::Kind::QuickLaunch,
                D2D1::RectF(centers[kDockControlQuickApps].x - r * 0.82f,
                            centers[kDockControlQuickApps].y - r * 0.82f,
                            centers[kDockControlQuickApps].x + r * 0.82f,
                            centers[kDockControlQuickApps].y + r * 0.82f),
                onDockQuickApps ? brushBtn_ : brushBtnDisabled_, 1.45f);
        }
        if (expandedControlVisible(kImmersiveControlApps)) {
            settings_icon::draw(
                rt, settings_icon::Kind::Apps,
                D2D1::RectF(centers[kImmersiveControlApps].x - r * 0.82f,
                            centers[kImmersiveControlApps].y - r * 0.82f,
                            centers[kImmersiveControlApps].x + r * 0.82f,
                            centers[kImmersiveControlApps].y + r * 0.82f),
                brushBtn_, 1.2f);
        }
        if (brushBtn_) {
            const float dotRadius = std::clamp(r * 0.13f, 1.0f, 1.6f);
            const float dotPitch = r * 0.48f;
            for (int i = -1; i <= 1; ++i) {
                rt->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(centers[kImmersiveControlMenu].x + i * dotPitch,
                                     centers[kImmersiveControlMenu].y),
                        dotRadius, dotRadius),
                    brushBtn_);
            }
        }
        if (expandedControlVisible(kImmersiveControlTray)) {
            settings_icon::draw(
                rt, settings_icon::Kind::Tray,
                D2D1::RectF(centers[kImmersiveControlTray].x - r * 0.82f,
                            centers[kImmersiveControlTray].y - r * 0.82f,
                            centers[kImmersiveControlTray].x + r * 0.82f,
                            centers[kImmersiveControlTray].y + r * 0.82f),
                brushBtn_, 1.2f);
        }
    }

    void drawVolumeButton(const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        ID2D1SolidColorBrush* brush =
            appVolume_.available ? brushBtn_ : brushBtnDisabled_;
        const int level = !appVolume_.available || appVolume_.muted
                              ? 0
                              : appVolume_.percent == 0 ? 1 : appVolume_.percent < 50 ? 2 : 3;
        media_control::drawVolume(rt, c, r * 0.8f, brush, level);
    }

    bool hitVolumeButton(float x, float y) const {
        if (isExpandedView())
            return hitImmersiveControl(x, y) == kImmersiveControlVolume;
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (!layout.showControls)
                return false;
            float logicalX = 0.0f;
            float logicalY = 0.0f;
            clientPointToLogicalDip(x, y, logicalX, logicalY);
            return std::hypot(logicalX - layout.controlCenters[3].x,
                              logicalY - layout.controlCenters[3].y) <=
                   layout.controlRadius + 4.0f;
        }
        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r))
            return false;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        x = logicalX;
        y = logicalY;
        return std::hypot(x - centers[3], y - cy) <= r + 4.0f;
    }

    // 音量按钮的屏幕坐标矩形（音量滑块浮窗的锚点）
    RECT volumeButtonScreenRect() const {
        if (isExpandedView()) {
            D2D1_POINT_2F centers[kExpandedControlCount]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (immersiveControlsLayout(centers, cy, r)) {
                const float s = scale();
                POINT pt = logicalDipToClientPoint(centers[kImmersiveControlVolume].x,
                                                   centers[kImmersiveControlVolume].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((r + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (layout.showControls) {
                const float s = scale();
                POINT pt = logicalDipToClientPoint(layout.controlCenters[3].x,
                                                   layout.controlCenters[3].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((layout.controlRadius + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            return rc;
        }
        const float s = scale();
        POINT pt = logicalDipToClientPoint(centers[3], cy);
        ClientToScreen(hwnd, &pt);
        const int half = static_cast<int>(std::lround((r + 6.0f) * s));
        return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
    }

    void prepareForExternalPopup() {
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        requestFrameAndFlush();
    }

    void openTaskbarMenu(POINT screenPoint) {
        if (!onContextMenu)
            return;
        prepareForExternalPopup();
        onContextMenu(screenPoint);
    }

    void openImmersiveMenu(POINT screenPoint) {
        if (!onImmersiveMenu)
            return;
        prepareForExternalPopup();
        onImmersiveMenu(screenPoint);
    }

    void openSystemTrayOverflow(POINT anchor) {
        if (!taskbar_ || !IsWindow(taskbar_))
            return;

        bool expected = false;
        if (!trayOverflowOpening_->compare_exchange_strong(expected, true))
            return;
        prepareForExternalPopup();
        const HWND taskbar = taskbar_;
        const UINT taskbarEdge = taskbarEdge_;
        const auto opening = trayOverflowOpening_;
        try {
            std::thread([taskbar, anchor, taskbarEdge, opening] {
                try {
                    invokeTaskbarTrayOverflow(taskbar, anchor, taskbarEdge);
                } catch (...) {
                }
                opening->store(false);
            }).detach();
        } catch (...) {
            opening->store(false);
        }
    }

    void openAppCollection(POINT screenPoint) {
        if (!onAppCollection)
            return;
        prepareForExternalPopup();
        onAppCollection(screenPoint);
    }

    void openDockQuickApps(POINT screenPoint) {
        if (!isAppBarView() || !onDockQuickApps)
            return;
        prepareForExternalPopup();
        onDockQuickApps(screenPoint);
    }

    void activateImmersiveControl(int index) {
        switch (index) {
        case kImmersiveControlPrevious:
        case kImmersiveControlPlayPause:
        case kImmersiveControlNext:
            if (onControl)
                onControl(static_cast<MediaControl>(index));
            break;
        case kImmersiveControlVolume:
            if (appVolume_.available) {
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect(), false,
                                      taskbarEdge_ == ABE_LEFT);
            }
            break;
        case kImmersiveControlExit:
            if (onImmersiveExit)
                onImmersiveExit();
            break;
        case kDockControlQuickApps: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            openDockQuickApps(pt);
            break;
        }
        case kImmersiveControlApps: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            openAppCollection(pt);
            break;
        }
        case kImmersiveControlMenu: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            // 菜单按钮是完整托盘菜单的固定入口，不受“右键显示
            // 任务栏歌词菜单”开关影响，也不复用任务栏歌词的精简菜单。
            openImmersiveMenu(pt);
            break;
        }
        case kImmersiveControlTray: {
            // 这里打开 Windows 自己的“隐藏图标/托盘溢出”面板，不是应用内 FluentMenu。
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            openSystemTrayOverflow(pt);
            break;
        }
        default:
            break;
        }
    }

    void drawButton(int idx, const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        bool enabled = idx == 0 ? media.canPrev : idx == 1 ? media.canPlayPause : media.canNext;
        ID2D1SolidColorBrush* brush = enabled ? brushBtn_ : brushBtnDisabled_;

        media_control::draw(rt, controlGeometry, idx, media.playing, c, r, brush);
    }

    int hitButton(float x, float y) const {
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return -1;
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (!layout.showControls)
                return -1;
            float logicalX = 0.0f;
            float logicalY = 0.0f;
            clientPointToLogicalDip(x, y, logicalX, logicalY);
            for (int i = 0; i < 3; ++i) {
                const bool enabled = i == 0 ? media.canPrev
                                            : i == 1 ? media.canPlayPause : media.canNext;
                if (enabled &&
                    std::hypot(logicalX - layout.controlCenters[i].x,
                               logicalY - layout.controlCenters[i].y) <=
                        layout.controlRadius + 4.0f)
                    return i;
            }
            return -1;
        }
        // 鼠标消息使用像素坐标，而 render 使用 DIP；先统一到 DIP，
        // 并与 render 使用完全相同的左侧分区计算。
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        float w = layout.w;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        x = logicalX;
        y = logicalY;
        float leftW = layout.leftW;
        if (x < leftW || x > w)
            return -1;

        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r))
            return -1;
        for (int i = 0; i < 3; ++i) {
            bool en = i == 0 ? media.canPrev : i == 1 ? media.canPlayPause : media.canNext;
            if (!en)
                continue;
            if (std::hypot(x - centers[i], y - cy) <= r + 4.0f)
                return i;
        }
        return -1;
    }

    struct LyricTransitionSample {
        float progress = 1.0f;
        float movement = 1.0f;
        float fadeOut = 1.0f;
        float fadeIn = 1.0f;
    };

    static float smoothStep(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void beginImmersiveSongContentTransition() {
        if (!isExpandedView() || isMinimalMode() ||
            !isSongTransitionPending() || songContentTransition_)
            return;
        songContentTransition_ = SongContentTransition{monotonicNowMs()};
    }

    void clearImmersiveSongContentTransition() {
        if (songContentTransition_ && songContentTransition_->compositorLayer)
            renderer.clearLyricTransitionLayers();
        songContentTransition_.reset();
    }

    void finishImmersiveSongContentTransitionIfNeeded() {
        if (!songContentTransition_ || !songContentTransition_->compositorLayer)
            return;
        const ULONGLONG now = monotonicNowMs();
        if (now < songContentTransition_->startMs ||
            now - songContentTransition_->startMs <
                static_cast<ULONGLONG>(std::ceil(kSongTransitionMs)))
            return;
        renderer.clearLyricTransitionLayers();
        songContentTransition_.reset();
        // 下一帧恢复到交换链绘制最新歌曲内容，并与合成层的结束点衔接。
        requestFrame();
    }

    // D2D 回退路径：沉浸模式的遮罩、控件和频谱保持静止，只把封面、歌曲信息
    // 和歌词作为一组内容在 D2D 坐标系内滑入。
    bool applyImmersiveSongContentTransform(ID2D1DeviceContext* rt) {
        if (!rt || !isExpandedView() || !songContentTransition_ ||
            songContentTransition_->compositorLayer)
            return false;

        ULONGLONG now = monotonicNowMs();
        if (now < songContentTransition_->startMs)
            now = songContentTransition_->startMs;
        const float progress = std::clamp(
            static_cast<float>(now - songContentTransition_->startMs) / kSongTransitionMs,
            0.0f, 1.0f);
        if (progress >= 1.0f) {
            songContentTransition_.reset();
            return false;
        }

        const float offset = kSongTransitionTravelDip * (1.0f - smoothStep(progress));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(offset, 0.0f));
        return true;
    }

    static float rangedSmoothStep(float t, float start, float end) {
        if (end <= start)
            return t >= end ? 1.0f : 0.0f;
        return smoothStep((t - start) / (end - start));
    }

    float lyricTransitionDurationMs() const {
        return lyricTransitionKind_ == LyricTransitionKind::Scene ? kSceneTransitionMs
                                                                    : kLyricTransitionMs;
    }

    LyricTransitionSample lyricTransitionSample() const {
        LyricTransitionSample sample;
        if (!isLyricTransitionActive() || lyricTransitionStartMs_ == 0)
            return sample;

        // applyPresentationFrame() 可以在定时器之外直接触发 render()。此时
        // frameNowMs_ 仍可能早于刚设置的起点，不能用 ULONGLONG 直接相减。
        ULONGLONG now = frameNowMs_;
        if (now < lyricTransitionStartMs_)
            now = monotonicNowMs();
        if (now < lyricTransitionStartMs_)
            now = lyricTransitionStartMs_;
        sample.progress = std::clamp(
            static_cast<float>(now - lyricTransitionStartMs_) / lyricTransitionDurationMs(),
            0.0f, 1.0f);
        sample.movement = smoothStep(sample.progress);
        sample.fadeOut = rangedSmoothStep(sample.progress, 0.08f, 0.90f);
        sample.fadeIn = rangedSmoothStep(sample.progress, 0.14f, 1.0f);
        return sample;
    }

    // 绘制可滚动文本：容得下则按 alignment 对齐，容不下则向左无缝滚动。
    // outline/glow 非空时先画 8 方向光晕层和深色描边层，再画主文字。
    // karaokeBrush 非空时启用逐字高亮：整行先用 brush（未播放色）画一遍，
    // 再按像素裁剪出 [文本起点, karaokeX] 区域用 karaokeBrush（已播放色）画第二遍，
    // 实现字内平滑填充（裁剪基于像素，不受滚动偏移影响）
    // 将 glow+outline+本体三层合成到离屏位图（独立的兼容渲染目标，不嵌套主 BeginDraw）。
    // 成功返回可绘制位图（调用方负责 Release），失败返回 nullptr 走直接绘制兜底。
    // 必须在画刷透明度被修改之前调用，缓存内容始终是自然透明度。
    ID2D1Bitmap* textFxBitmap(IDWriteTextLayout* layout, float textW, float textH,
                              ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow) {
        auto* rt = drawTarget();
        if (!rt || !layout || !brush)
            return nullptr;
        constexpr float pad = 3.0f; // 覆盖 2.4 DIP 的光晕外扩和边缘抗锯齿
        const float wDip = textW + pad * 2.0f;
        const float hDip = textH + pad * 2.0f;
        TextFxCacheEntry* entry = nullptr;
        for (auto& candidate : textFxCaches_) {
            if (candidate.target && candidate.layout == layout && candidate.brush == brush &&
                candidate.outline == outline && candidate.glow == glow &&
                candidate.generation == textFxGen_ && candidate.dpi == dpi_ &&
                candidate.w == wDip && candidate.h == hDip) {
                entry = &candidate;
                break;
            }
        }
        if (!entry) {
            entry = &textFxCaches_[0];
            if (textFxCaches_[1].lastUse < entry->lastUse)
                entry = &textFxCaches_[1];
            if (entry->target)
                entry->target->Release();
            *entry = {};
            if (FAILED(rt->CreateCompatibleRenderTarget(D2D1::SizeF(wDip, hDip),
                                                         &entry->target)))
                return nullptr;
            entry->target->SetAntialiasMode(rt->GetAntialiasMode());
            entry->target->SetTextAntialiasMode(rt->GetTextAntialiasMode());
            static constexpr float kDirs[8][2] = {{1.0f, 0.0f},
                                                  {0.7071f, 0.7071f},
                                                  {0.0f, 1.0f},
                                                  {-0.7071f, 0.7071f},
                                                  {-1.0f, 0.0f},
                                                  {-0.7071f, -0.7071f},
                                                  {0.0f, -1.0f},
                                                  {0.7071f, -0.7071f}};
            entry->target->BeginDraw();
            entry->target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            if (glow) {
                for (auto& d : kDirs)
                    entry->target->DrawTextLayout(
                        D2D1::Point2F(pad + d[0] * 2.4f, pad + d[1] * 2.4f), layout, glow);
            }
            if (outline) {
                for (auto& d : kDirs)
                    entry->target->DrawTextLayout(
                        D2D1::Point2F(pad + d[0] * 1.2f, pad + d[1] * 1.2f), layout, outline);
            }
            entry->target->DrawTextLayout(D2D1::Point2F(pad, pad), layout, brush);
            if (FAILED(entry->target->EndDraw())) {
                entry->target->Release();
                *entry = {};
                return nullptr;
            }
            entry->layout = layout;
            entry->brush = brush;
            entry->outline = outline;
            entry->glow = glow;
            entry->generation = textFxGen_;
            entry->dpi = dpi_;
            entry->w = wDip;
            entry->h = hDip;
            entry->pad = pad;
        }
        entry->lastUse = ++textFxUse_;
        ID2D1Bitmap* bmp = nullptr;
        if (FAILED(entry->target->GetBitmap(&bmp)) || !bmp)
            return nullptr;
        return bmp;
    }

    // 滚动文本左缘渐隐画笔：固定两停止点（透明 → 不透明），创建一次终身复用，
    // 几何完全由每帧移动渐变轴实现
    ID2D1LinearGradientBrush* ensureLyricEdgeFadeBrush() {
        if (lyricEdgeFadeBrush_)
            return lyricEdgeFadeBrush_;
        auto* rt = drawTarget();
        if (!rt)
            return nullptr;
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)},
        };
        ID2D1GradientStopCollection* stopCollection = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, 2, &stopCollection)))
            return nullptr;
        const HRESULT hr = rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                D2D1::Point2F(1.0f, 0.0f)),
            D2D1::BrushProperties(), stopCollection, &lyricEdgeFadeBrush_);
        stopCollection->Release();
        return FAILED(hr) ? nullptr : lyricEdgeFadeBrush_;
    }

    ID2D1LinearGradientBrush* ensureSongInfoDividerBrush() {
        if (songInfoDividerBrush_)
            return songInfoDividerBrush_;
        auto* rt = drawTarget();
        if (!rt)
            return nullptr;

        const D2D1_COLOR_F base =
            brushDim_ ? brushDim_->GetColor()
                      : (lightTheme_ ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f)
                                      : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f));
        const float peakAlpha = std::clamp(base.a * 0.72f, 0.0f, 1.0f);
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(base.r, base.g, base.b, 0.0f)},
            {0.18f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha * 0.42f)},
            {0.50f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha)},
            {0.82f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha * 0.42f)},
            {1.0f, D2D1::ColorF(base.r, base.g, base.b, 0.0f)},
        };
        ID2D1GradientStopCollection* stopCollection = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, _countof(stops),
                                                    &stopCollection)) ||
            !stopCollection)
            return nullptr;

        const HRESULT hr = rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                D2D1::Point2F(0.0f, 1.0f)),
            D2D1::BrushProperties(), stopCollection, &songInfoDividerBrush_);
        stopCollection->Release();
        return FAILED(hr) ? nullptr : songInfoDividerBrush_;
    }

    void drawSongInfoDivider(float leftW, float h) {
        auto* rt = drawTarget();
        if (!rt || !songInfoVisible_ || scene_ == DisplayScene::Idle || h <= 2.0f ||
            kSongInfoLyricGap <= kSongInfoDividerWidth)
            return;

        const float top = std::min(kSongInfoDividerInset, h * 0.25f);
        const float bottom = std::max(top + kSongInfoDividerWidth, h - top);
        if (bottom <= top)
            return;
        auto* brush = ensureSongInfoDividerBrush();
        if (!brush)
            return;

        brush->SetStartPoint(D2D1::Point2F(0.0f, top));
        brush->SetEndPoint(D2D1::Point2F(0.0f, bottom));
        const float x = leftW + (kSongInfoLyricGap - kSongInfoDividerWidth) * 0.5f;
        const D2D1_RECT_F rect = D2D1::RectF(x, top, x + kSongInfoDividerWidth, bottom);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(rect, kSongInfoDividerWidth * 0.5f,
                              kSongInfoDividerWidth * 0.5f),
            brush);
    }

    // 区域内左缘渐隐：渐隐带宽度随滚出量从 0 长到 fadeW，且左缘处透明度恒为 0。
    // 若让整个渐隐带按滚出量淡入，则滚出不足 fadeW 时左缘仍有残余不透明度，
    // 半截字符会在边界硬裁剪处留下一条竖线
    ID2D1LinearGradientBrush* lyricEdgeFadeBrush(float x, float fadeW, float offset) {
        ID2D1LinearGradientBrush* brush = ensureLyricEdgeFadeBrush();
        if (!brush || fadeW <= 0.0f)
            return nullptr;
        const float band = std::min(offset, fadeW);
        if (band <= 0.0f)
            return nullptr;
        brush->SetStartPoint(D2D1::Point2F(x, 0.0f));
        brush->SetEndPoint(D2D1::Point2F(x + band, 0.0f));
        return brush;
    }

    // 右缘渐隐带画笔：渐隐带完全位于可视区右缘之外（rightEdge 起 extend 宽），
    // 可读区域内文字始终全不透明，只有越过边界的部分向外渐隐
    ID2D1LinearGradientBrush* ensureLyricRightFadeBrush() {
        if (lyricRightFadeBrush_)
            return lyricRightFadeBrush_;
        auto* rt = drawTarget();
        if (!rt)
            return nullptr;
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        };
        ID2D1GradientStopCollection* stopCollection = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, 2, &stopCollection)))
            return nullptr;
        const HRESULT hr = rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                D2D1::Point2F(1.0f, 0.0f)),
            D2D1::BrushProperties(), stopCollection, &lyricRightFadeBrush_);
        stopCollection->Release();
        return FAILED(hr) ? nullptr : lyricRightFadeBrush_;
    }

    ID2D1LinearGradientBrush* lyricRightFadeBrush(float rightEdge, float extend) {
        ID2D1LinearGradientBrush* brush = ensureLyricRightFadeBrush();
        if (!brush || extend <= 0.0f)
            return nullptr;
        brush->SetStartPoint(D2D1::Point2F(rightEdge, 0.0f));
        brush->SetEndPoint(D2D1::Point2F(rightEdge + extend, 0.0f));
        return brush;
    }

    // 竖排滚动文本的两侧渐隐几何。竖排跑马灯向下滚动（offset > 0）：内容从底部
    // 滚出、顶部滚入；逐字跟随向上回收（offset < 0）：从顶部滚出、底部滚入。
    // 与横向同一套语义：滚出侧在区域内渐隐（带宽随滚出量从 0 建立；每日一言这类
    // 内容确定的无限循环跑马灯恒定保持，避免绕回时渐隐消失再出现），滚入侧借位
    // 到可视区外渐隐，可读区域内文字始终全不透明
    struct VerticalEdgeFades {
        float outBand = 0.0f;   // 滚出侧区域内渐隐带宽，0 表示不渐隐
        bool outAtTop = false;  // 滚出侧是否在可视区顶部
        bool borrow = false;    // 滚入侧是否启用借位渐隐
        bool borrowAtTop = false; // 滚入侧是否在可视区顶部
    };

    VerticalEdgeFades verticalEdgeFades(float offset, float contentH, float areaH,
                                        bool karaoke, bool constantEdgeFade) const {
        VerticalEdgeFades fades;
        if (contentH <= areaH || areaH <= 0.0f)
            return fades;
        const float fadeH = std::min(kLyricEdgeFadeDip, areaH * 0.25f);
        fades.outAtTop = offset < 0.0f;
        fades.outBand =
            constantEdgeFade ? fadeH : std::min(std::fabs(offset), fadeH);
        fades.borrow = true;
        fades.borrowAtTop = !karaoke && offset >= 0.0f;
        return fades;
    }

    // 竖排滚出侧渐隐画笔：复用横向左缘画笔（停止点 0=透明 1=不透明），
    // 每帧改设竖向渐变轴；边界处透明度恒为 0，半截字符不会在边界留下亮线
    ID2D1LinearGradientBrush* verticalOutFadeBrush(float edgeY, float band, bool atTop) {
        ID2D1LinearGradientBrush* brush = ensureLyricEdgeFadeBrush();
        if (!brush || band <= 0.0f)
            return nullptr;
        brush->SetStartPoint(D2D1::Point2F(0.0f, edgeY));
        brush->SetEndPoint(D2D1::Point2F(0.0f, atTop ? edgeY + band : edgeY - band));
        return brush;
    }

    // 竖排滚入侧借位渐隐画笔：复用横向右缘画笔（停止点 0=不透明 1=透明），
    // 渐隐带完全位于可视区之外（edgeY 起向外 extend 宽）
    ID2D1LinearGradientBrush* verticalBorrowFadeBrush(float edgeY, float extend, bool atTop) {
        ID2D1LinearGradientBrush* brush = ensureLyricRightFadeBrush();
        if (!brush || extend <= 0.0f)
            return nullptr;
        brush->SetStartPoint(D2D1::Point2F(0.0f, edgeY));
        brush->SetEndPoint(D2D1::Point2F(0.0f, atTop ? edgeY - extend : edgeY + extend));
        return brush;
    }

    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                           ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                           float opacity = 1.0f,
                           LyricAlignment alignment = LyricAlignment::Center,
                           bool singleCopy = false, float rightExtend = 0.0f,
                           bool constantEdgeFade = false, float leftFadeDip = 0.0f,
                           float centerAnchor = -1.0f) {
        auto* rt = drawTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        if (opacity >= 0.999f)
            opacity = 1.0f;
        // 光晕/描边走离屏缓存：三层合成一次，之后滚动/淡变只 DrawImage；
        // 失败时 fxBmp 为空，回落到逐层直接绘制
        ID2D1Bitmap* fxBmp = nullptr;
        if (glow || outline)
            fxBmp = textFxBitmap(layout, textW, textH, brush, outline, glow);
        ID2D1Brush* brushes[4] = {fxBmp ? nullptr : brush, fxBmp ? nullptr : outline,
                                  fxBmp ? nullptr : glow, karaokeBrush};
        ID2D1Brush* changed[4] = {};
        float previousOpacity[4] = {};
        int changedCount = 0;
        for (ID2D1Brush* candidate : brushes) {
            if (!candidate || opacity >= 0.999f)
                continue;
            bool alreadyChanged = false;
            for (int i = 0; i < changedCount; ++i)
                alreadyChanged = alreadyChanged || changed[i] == candidate;
            if (alreadyChanged)
                continue;
            previousOpacity[changedCount] = candidate->GetOpacity();
            candidate->SetOpacity(previousOpacity[changedCount] * opacity);
            changed[changedCount++] = candidate;
        }
        // 右缘（滚入侧）渐隐带借用到可视区之外的留白，可读区域内文字保持清晰
        ID2D1LinearGradientBrush* rightFade =
            (textW > areaW && rightExtend > 0.0f && lyricRightFadeLayer_)
                ? lyricRightFadeBrush(x + areaW, rightExtend)
                : nullptr;
        // 左缘（滚出侧）在区域内渐隐，文字不会越过区域边界：歌词渐隐带随滚出量
        // 从 0 建立（新行贴边界时不渐隐，与第二行左缘严格对齐）；歌曲信息是
        // 无限循环跑马灯，是否滚动在内容确定时就已知，渐隐恒定保持，
        // 避免每轮循环绕回时渐隐消失再出现。leftFadeDip > 0 时覆盖默认渐隐宽度
        const float fadeDip = leftFadeDip > 0.0f ? leftFadeDip : kLyricEdgeFadeDip;
        const float leftFadeW = std::min(fadeDip, areaW * 0.25f);
        const float leftScrolled = constantEdgeFade ? leftFadeW : offset;
        ID2D1LinearGradientBrush* fadeBrush =
            (textW > areaW && leftScrolled > 0.0f && lyricEdgeFadeLayer_)
                ? lyricEdgeFadeBrush(x, leftFadeW, leftScrolled)
                : nullptr;
        D2D1_RECT_F clip{x, y, x + areaW + (rightFade ? rightExtend : 0.0f), y + textH};
        if (rightFade) {
            rt->PushLayer(D2D1::LayerParameters1(clip, nullptr,
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Identity(), 1.0f,
                                                 rightFade),
                          lyricRightFadeLayer_);
        }
        if (fadeBrush) {
            rt->PushLayer(D2D1::LayerParameters1(clip, nullptr,
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Identity(), 1.0f,
                                                 fadeBrush),
                          lyricEdgeFadeLayer_);
        } else {
            rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        }
        static constexpr float kDirs[8][2] = {{1.0f, 0.0f},
                                              {0.7071f, 0.7071f},
                                              {0.0f, 1.0f},
                                              {-0.7071f, 0.7071f},
                                              {-1.0f, 0.0f},
                                              {-0.7071f, -0.7071f},
                                              {0.0f, -1.0f},
                                              {0.7071f, -0.7071f}};
        // 绘制起点：居中一个，或跑马灯首尾相接两个；逐字/转场入场时只有一份、不循环
        float bases[2];
        int n = 0;
        auto alignedBase = [&]() {
            float freeW = std::max(0.0f, areaW - textW);
            switch (alignment) {
            case LyricAlignment::Left:
                return x;
            case LyricAlignment::Right:
                return x + freeW;
            case LyricAlignment::Center:
            default:
                if (centerAnchor >= 0.0f)
                    return std::clamp(centerAnchor - textW * 0.5f, x, x + freeW);
                return x + freeW * 0.5f;
            }
        };
        if (karaokeBrush || singleCopy) {
            bases[n++] = (textW <= areaW) ? alignedBase() : x - offset;
        } else if (textW <= areaW) {
            bases[n++] = alignedBase();
        } else {
            float loopW = textW + kTextPadding * 2.0f;
            bases[n++] = x - offset;
            bases[n++] = x - offset + loopW;
        }
        if (fxBmp) {
            constexpr float textFxPad = 3.0f;
            const float textFxW = textW + textFxPad * 2.0f;
            const float textFxH = textH + textFxPad * 2.0f;
            for (int i = 0; i < n; ++i)
                rt->DrawBitmap(fxBmp,
                               D2D1::RectF(bases[i] - textFxPad, y - textFxPad,
                                           bases[i] - textFxPad + textFxW,
                                           y - textFxPad + textFxH),
                               opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            if (glow || outline) {
                for (int i = 0; i < n; ++i) {
                    if (glow) {
                        for (auto& d : kDirs)
                            rt->DrawTextLayout(
                                D2D1::Point2F(bases[i] + d[0] * 2.4f, y + d[1] * 2.4f), layout,
                                glow);
                    }
                    if (outline) {
                        for (auto& d : kDirs)
                            rt->DrawTextLayout(
                                D2D1::Point2F(bases[i] + d[0] * 1.2f, y + d[1] * 1.2f), layout,
                                outline);
                    }
                }
            }
            for (int i = 0; i < n; ++i)
                rt->DrawTextLayout(D2D1::Point2F(bases[i], y), layout, brush);
        }
        // 逐字高亮层：裁剪到填充进度
        if (karaokeBrush && karaokeX > 0.0f) {
            rt->PushAxisAlignedClip(D2D1::RectF(bases[0], y, bases[0] + karaokeX, y + textH),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->DrawTextLayout(D2D1::Point2F(bases[0], y), layout, karaokeBrush);
            rt->PopAxisAlignedClip();
        }
        if (fadeBrush)
            rt->PopLayer();
        else
            rt->PopAxisAlignedClip();
        if (rightFade)
            rt->PopLayer();
        for (int i = 0; i < changedCount; ++i)
            changed[i]->SetOpacity(previousOpacity[i]);
        if (fxBmp)
            fxBmp->Release();
    }

    void drawVerticalScrollingText(
        IDWriteTextLayout* layout, float textW, float textH, float areaH, float x, float y,
        float offset, ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
        ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
        float karaokeX = 0.0f, float opacity = 1.0f,
        LyricAlignment alignment = LyricAlignment::Center, bool singleCopy = false,
        bool rotated = false, float topExtend = 0.0f, float bottomExtend = 0.0f,
        bool constantEdgeFade = false, bool skipSlotClip = false, float slotY = -1.0f) {
        auto* rt = drawTarget();
        if (!rt || !layout || textW <= 0.0f || textH <= 0.0f || areaH <= 0.0f)
            return;
        (void)alignment;
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        if (opacity >= 0.999f)
            opacity = 1.0f;

        // rotated 布局保存的是整句横向文字：左右侧共用截图中的固定角度，
        // 再映射到同一个竖向滚动槽；中文逐字布局则保持字形正向。
        ID2D1Bitmap* fxBmp = nullptr;
        // 离屏缓存按横向布局的原始尺寸创建；旋转文字直接绘制，避免把
        // “横向宽高”交换后再交给未旋转的缓存目标而截断英文句子。
        if (!rotated && (glow || outline))
            fxBmp = textFxBitmap(layout, textW, textH, brush, outline, glow);
        ID2D1Brush* brushes[4] = {fxBmp ? nullptr : brush, fxBmp ? nullptr : outline,
                                  fxBmp ? nullptr : glow, karaokeBrush};
        ID2D1Brush* changed[4] = {};
        float previousOpacity[4] = {};
        int changedCount = 0;
        for (ID2D1Brush* candidate : brushes) {
            if (!candidate || opacity >= 0.999f)
                continue;
            bool alreadyChanged = false;
            for (int i = 0; i < changedCount; ++i)
                alreadyChanged = alreadyChanged || changed[i] == candidate;
            if (alreadyChanged)
                continue;
            previousOpacity[changedCount] = candidate->GetOpacity();
            candidate->SetOpacity(previousOpacity[changedCount] * opacity);
            changed[changedCount++] = candidate;
        }

        // 两侧渐隐：滚出侧在区域内按滚出量建立渐隐带，滚入侧借位到可视区外。
        // skipSlotClip 时由调用方（多段竖排歌词）统一裁剪与渐隐。
        // 裁剪与渐隐锚定歌词槽（slotY，转场期间文字随 oldY/newY 移动但槽不动），
        // 避免滑出/滑入的文字飘到封面等区域
        const float slotTop = slotY >= 0.0f ? slotY : y;
        const VerticalEdgeFades fades =
            skipSlotClip ? VerticalEdgeFades{}
                         : verticalEdgeFades(offset, textH, areaH, karaokeBrush != nullptr,
                                             constantEdgeFade);
        const float topExt = fades.borrow && fades.borrowAtTop ? topExtend : 0.0f;
        const float bottomExt = fades.borrow && !fades.borrowAtTop ? bottomExtend : 0.0f;
        ID2D1LinearGradientBrush* borrowFade =
            (topExt > 0.0f || bottomExt > 0.0f) && lyricRightFadeLayer_
                ? verticalBorrowFadeBrush(fades.borrowAtTop ? slotTop : slotTop + areaH,
                                          fades.borrowAtTop ? topExt : bottomExt,
                                          fades.borrowAtTop)
                : nullptr;
        ID2D1LinearGradientBrush* outFade =
            fades.outBand > 0.0f && lyricEdgeFadeLayer_
                ? verticalOutFadeBrush(fades.outAtTop ? slotTop : slotTop + areaH,
                                       fades.outBand, fades.outAtTop)
                : nullptr;
        const D2D1_RECT_F clip{x, slotTop - topExt, x + textW, slotTop + areaH + bottomExt};
        enum class ClipMode { kNone, kClip, kOutLayer, kBorrowLayer, kBothLayers };
        ClipMode clipMode = ClipMode::kNone;
        if (!skipSlotClip) {
            if (borrowFade) {
                rt->PushLayer(D2D1::LayerParameters1(clip, nullptr,
                                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::Matrix3x2F::Identity(), 1.0f,
                                                     borrowFade),
                              lyricRightFadeLayer_);
                clipMode = ClipMode::kBorrowLayer;
            }
            if (outFade) {
                rt->PushLayer(D2D1::LayerParameters1(clip, nullptr,
                                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::Matrix3x2F::Identity(), 1.0f,
                                                     outFade),
                              lyricEdgeFadeLayer_);
                clipMode = clipMode == ClipMode::kBorrowLayer ? ClipMode::kBothLayers
                                                              : ClipMode::kOutLayer;
            }
            if (clipMode == ClipMode::kNone) {
                rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                clipMode = ClipMode::kClip;
            }
        }
        // 两侧统一从上向下滚动。第二份副本放在前一份上方，确保内容离开底部
        // 后从顶部无缝接回，而不会因为右侧任务栏的文字顺序而反向上跑。
        const float base = y + offset;
        float bases[2]{};
        int count = 0;
        if (karaokeBrush || singleCopy || textH <= areaH) {
            bases[count++] = base;
        } else {
            const float loopH = textH + kVerticalLyricGap;
            bases[count++] = base;
            bases[count++] = base - loopH;
        }

        auto rotatedTransform = [&](float drawX, float top) {
            // D2D 坐标系的 Y 轴向下，统一使用顺时针 90°，匹配截图中的固定角度。
            return D2D1::Matrix3x2F(0.0f, 1.0f, -1.0f, 0.0f, drawX + textW, top);
        };

        auto drawRotatedLayout = [&](float drawX, float top, ID2D1Brush* drawBrush) {
            if (!drawBrush)
                return;
            D2D1_MATRIX_3X2_F previous{};
            rt->GetTransform(&previous);
            rt->SetTransform(rotatedTransform(drawX, top) * previous);
            rt->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), layout, drawBrush);
            rt->SetTransform(previous);
        };

        if (fxBmp) {
            constexpr float textFxPad = 3.0f;
            const float textFxW = textW + textFxPad * 2.0f;
            const float textFxH = textH + textFxPad * 2.0f;
            for (int i = 0; i < count; ++i)
                rt->DrawBitmap(fxBmp,
                               D2D1::RectF(x - textFxPad, bases[i] - textFxPad,
                                           x - textFxPad + textFxW,
                                           bases[i] - textFxPad + textFxH),
                               opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            static constexpr float kDirs[8][2] = {{1.0f, 0.0f},
                                                  {0.7071f, 0.7071f},
                                                  {0.0f, 1.0f},
                                                  {-0.7071f, 0.7071f},
                                                  {-1.0f, 0.0f},
                                                  {-0.7071f, -0.7071f},
                                                  {0.0f, -1.0f},
                                                  {0.7071f, -0.7071f}};
            if (glow || outline) {
                for (int i = 0; i < count; ++i) {
                    if (glow) {
                        for (auto& d : kDirs)
                            if (rotated)
                                drawRotatedLayout(x + d[0] * 2.4f,
                                                  bases[i] + d[1] * 2.4f, glow);
                            else
                                rt->DrawTextLayout(
                                    D2D1::Point2F(x + d[0] * 2.4f, bases[i] + d[1] * 2.4f),
                                    layout, glow);
                    }
                    if (outline) {
                        for (auto& d : kDirs)
                            if (rotated)
                                drawRotatedLayout(x + d[0] * 1.2f,
                                                  bases[i] + d[1] * 1.2f, outline);
                            else
                                rt->DrawTextLayout(
                                    D2D1::Point2F(x + d[0] * 1.2f, bases[i] + d[1] * 1.2f),
                                    layout, outline);
                    }
                }
            }
            for (int i = 0; i < count; ++i) {
                if (rotated)
                    drawRotatedLayout(x, bases[i], brush);
                else
                    rt->DrawTextLayout(D2D1::Point2F(x, bases[i]), layout, brush);
            }
        }

        // karaokeX 在竖排路径中统一从上向下填充；旋转英文也按最终可见的
        // 竖向顺序裁剪，避免左右任务栏出现相反的高亮方向。
        if (karaokeBrush && karaokeX > 0.0f) {
            const float progress = std::clamp(karaokeX, 0.0f, 1.0f);
            const float top = bases[0];
            const float bottom = bases[0] + textH * progress;
            if (bottom > top) {
                rt->PushAxisAlignedClip(D2D1::RectF(x, top, x + textW, bottom),
                                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                if (rotated)
                    drawRotatedLayout(x, bases[0], karaokeBrush);
                else
                    rt->DrawTextLayout(D2D1::Point2F(x, bases[0]), layout, karaokeBrush);
                rt->PopAxisAlignedClip();
            }
        }
        if (clipMode == ClipMode::kOutLayer || clipMode == ClipMode::kBothLayers)
            rt->PopLayer();
        if (clipMode == ClipMode::kBorrowLayer || clipMode == ClipMode::kBothLayers)
            rt->PopLayer();
        if (clipMode == ClipMode::kClip)
            rt->PopAxisAlignedClip();
        for (int i = 0; i < changedCount; ++i)
            changed[i]->SetOpacity(previousOpacity[i]);
        if (fxBmp)
            fxBmp->Release();
    }

    void drawVerticalLyricParts(const std::vector<VerticalLyricPart>& parts, float blockH,
                                float areaH, float railW, float y, float offset,
                                ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
                                ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr,
                                float karaokeProgress = 0.0f, float opacity = 1.0f,
                                bool singleCopy = false, float topExtend = 0.0f,
                                float bottomExtend = 0.0f, bool constantEdgeFade = false,
                                float slotY = -1.0f) {
        auto* rt = drawTarget();
        if (!rt || parts.empty() || blockH <= 0.0f || areaH <= 0.0f)
            return;

        // 多段布局的两侧渐隐在整组内容上统一施加，避免每个分段各自渐隐；
        // 分段内部不再单独裁剪，裁剪由此处的图层/裁剪范围承担。
        // 裁剪与渐隐锚定歌词槽（slotY），转场期间文字随动画移动但槽不动
        const float slotTop = slotY >= 0.0f ? slotY : y;
        const VerticalEdgeFades fades =
            verticalEdgeFades(offset, blockH, areaH, karaokeBrush != nullptr, constantEdgeFade);
        const float topExt = fades.borrow && fades.borrowAtTop ? topExtend : 0.0f;
        const float bottomExt = fades.borrow && !fades.borrowAtTop ? bottomExtend : 0.0f;
        ID2D1LinearGradientBrush* borrowFade =
            (topExt > 0.0f || bottomExt > 0.0f) && lyricRightFadeLayer_
                ? verticalBorrowFadeBrush(fades.borrowAtTop ? slotTop : slotTop + areaH,
                                          fades.borrowAtTop ? topExt : bottomExt,
                                          fades.borrowAtTop)
                : nullptr;
        ID2D1LinearGradientBrush* outFade =
            fades.outBand > 0.0f && lyricEdgeFadeLayer_
                ? verticalOutFadeBrush(fades.outAtTop ? slotTop : slotTop + areaH,
                                       fades.outBand, fades.outAtTop)
                : nullptr;
        const D2D1_RECT_F clip{0.0f, slotTop - topExt, railW, slotTop + areaH + bottomExt};
        bool pushedBorrow = false;
        bool pushedOut = false;
        bool pushedClip = false;
        if (borrowFade) {
            rt->PushLayer(D2D1::LayerParameters1(clip, nullptr,
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Identity(), 1.0f,
                                                 borrowFade),
                          lyricRightFadeLayer_);
            pushedBorrow = true;
        }
        if (outFade) {
            rt->PushLayer(D2D1::LayerParameters1(clip, nullptr,
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::Matrix3x2F::Identity(), 1.0f,
                                                 outFade),
                          lyricEdgeFadeLayer_);
            pushedOut = true;
        }
        if (!pushedBorrow && !pushedOut) {
            rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            pushedClip = true;
        }

        const float progress = std::clamp(karaokeProgress, 0.0f, 1.0f);
        const float sungExtent = blockH * progress;
        const float loopH = blockH + kVerticalLyricGap;
        const int copyCount = karaokeBrush || singleCopy || blockH <= areaH ? 1 : 2;
        const float base = y + offset;
        for (int copy = 0; copy < copyCount; ++copy) {
            float partTop = base - copy * loopH;
            float partOffset = 0.0f;
            for (const auto& part : parts) {
                if (!part.layout || part.width <= 0.0f || part.height <= 0.0f)
                    continue;

                float partProgress = 0.0f;
                if (karaokeBrush) {
                    partProgress = std::clamp((sungExtent - partOffset) / part.height,
                                              0.0f, 1.0f);
                }
                ID2D1Brush* partKaraoke =
                    karaokeBrush && partProgress > 0.0f ? karaokeBrush : nullptr;
                const float partX = part.rotated ? (railW - part.width) * 0.5f : 0.0f;
                drawVerticalScrollingText(
                    part.layout, part.width, part.height, areaH, partX, partTop, 0.0f, brush,
                    outline, glow, partKaraoke, partProgress, opacity, LyricAlignment::Center,
                    true, part.rotated, 0.0f, 0.0f, false, true);
                partTop += part.height;
                partOffset += part.height;
            }
        }

        if (pushedOut)
            rt->PopLayer();
        if (pushedBorrow)
            rt->PopLayer();
        if (pushedClip)
            rt->PopAxisAlignedClip();
    }

    void drawVerticalLyrics(const VerticalLayout& layout) {
        auto* rt = drawTarget();
        if (!rt || layout.lyricBottom <= layout.lyricY)
            return;

        const float areaH = layout.lyricBottom - layout.lyricY;
        const bool idleScene = scene_ == DisplayScene::Idle;
        ID2D1Brush* primaryBrush = idleScene
                                       ? static_cast<ID2D1Brush*>(brushText_)
                                       : static_cast<ID2D1Brush*>(brushLyric_ ? brushLyric_
                                                                               : brushText_);
        const bool outgoingIdleScene = outgoingScene_ == DisplayScene::Idle;
        ID2D1Brush* outgoingBrush = outgoingIdleScene
                                        ? static_cast<ID2D1Brush*>(brushText_)
                                        : primaryBrush;
        ID2D1Brush* effectOutline = !idleScene && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_)
                                        : nullptr;
        ID2D1Brush* effectGlow = !idleScene && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_)
                                     : nullptr;
        ID2D1Brush* outgoingOutline = !outgoingIdleScene && lyricOutline_
                                          ? static_cast<ID2D1Brush*>(brushLyricOutline_)
                                          : nullptr;
        ID2D1Brush* outgoingGlow = !outgoingIdleScene && lyricGlow_
                                       ? static_cast<ID2D1Brush*>(brushLyricGlow_)
                                       : nullptr;
        const LyricAlignment alignment = LyricAlignment::Center;

        auto drawLine = [&](IDWriteTextLayout* textLayout,
                            const std::vector<VerticalLyricPart>* parts, float textW,
                            float textH, float y, float offset, ID2D1Brush* brush, float opacity,
                            bool singleCopy, bool rotated, ID2D1Brush* outline,
                            ID2D1Brush* glow, ID2D1Brush* karaokeBrush,
                            float karaokeProgress, bool constantFade) {
            // 滚入侧借位宽度：与竖排各区间距一致（封面与歌词间、歌词与底部/控件间
            // 均为 6 dip 或窗口边缘内边距），渐隐带不外溢到封面/控件上。
            // slotY 始终传歌词槽顶：转场期间 y 随动画移动，但裁剪/渐隐锚定歌词槽
            if (parts && !parts->empty()) {
                drawVerticalLyricParts(*parts, textH, areaH, layout.w, y, offset, brush, outline,
                                       glow, karaokeBrush, karaokeProgress, opacity, singleCopy,
                                       kVerticalLyricGap, kVerticalLyricGap, constantFade,
                                       layout.lyricY);
                return;
            }
            // 旋转英文的实际横向宽度通常小于任务栏槽宽，单独居中，避免整句贴在
            // 侧边栏外沿；中文逐字布局本身已经使用完整槽宽。
            const float textX = rotated ? (layout.w - textW) * 0.5f : 0.0f;
            drawVerticalScrollingText(textLayout, textW, textH, areaH, textX, y, offset, brush,
                                      outline, glow, karaokeBrush, karaokeProgress, opacity,
                                      alignment, singleCopy, rotated, kVerticalLyricGap,
                                      kVerticalLyricGap, constantFade, false, layout.lyricY);
        };

        auto verticalKaraokeProgress = [&](float karaokeX) {
            return lyricWidth_ > 0.0f
                       ? std::clamp(karaokeX / lyricWidth_, 0.0f, 1.0f)
                       : 0.0f;
        };

        // 不再使用整段统一裁剪：各绘制路径自带裁剪/渐隐图层，滚入侧借位内容
        // 需要画出可视槽之外才能形成渐隐带
        if (isLyricTransitionActive() &&
            (outgoingVerticalLyricLayout_ || !outgoingVerticalLyricParts_.empty())) {
            const LyricTransitionSample transition = lyricTransitionSample();
            const float travel = std::clamp(areaH * 0.24f, 18.0f, 48.0f);
            const float direction = lyricTransitionDirection_ >= 0 ? 1.0f : -1.0f;
            const float oldY = layout.lyricY - direction * travel * transition.movement;
            const float newY = layout.lyricY + direction * travel *
                                                         (1.0f - transition.movement);
            drawLine(outgoingVerticalLyricLayout_, &outgoingVerticalLyricParts_,
                     outgoingVerticalLyricWidth_,
                     outgoingVerticalLyricHeight_, oldY, outgoingLyricScrollOffset_,
                     outgoingBrush, 1.0f - transition.fadeOut, true,
                     outgoingVerticalLyricRotated_, outgoingOutline, outgoingGlow, nullptr,
                     0.0f, outgoingIdleScene);

            const LyricLine* incomingLine = !idleScene ? karaokeLine() : nullptr;
            const bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
            const float incomingKaraokeX = incomingKaraoke ? karaokeSmoothStep(*incomingLine)
                                                           : 0.0f;
            drawLine(verticalLyricLayout_, &verticalLyricParts_, verticalLyricWidth_,
                     verticalLyricHeight_, newY,
                     lyricScrollOffset_,
                     incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : primaryBrush,
                     transition.fadeIn, true, verticalLyricRotated_, effectOutline, effectGlow,
                     incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr,
                     verticalKaraokeProgress(incomingKaraokeX), idleScene);
        } else if (verticalLyricLayout_ || !verticalLyricParts_.empty()) {
            const LyricLine* currentLine = !idleScene ? karaokeLine() : nullptr;
            const bool karaoke = currentLine && brushLyric_ && brushLyricDim_;
            const float karaokeX = karaoke ? karaokeSmoothStep(*currentLine) : 0.0f;
            drawLine(verticalLyricLayout_, &verticalLyricParts_, verticalLyricWidth_,
                     verticalLyricHeight_,
                     layout.lyricY, lyricScrollOffset_,
                     karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : primaryBrush,
                     1.0f, false, verticalLyricRotated_, effectOutline, effectGlow,
                     karaoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr,
                     verticalKaraokeProgress(karaokeX), idleScene);
        }
    }

    LyricAlignment activeLyricAlignment() const {
        if (scene_ == DisplayScene::Idle)
            return idleQuoteAlignment_;
        return isExpandedView() ? LyricAlignment::Center
                                                        : lyricAlignment_;
    }

    LyricAlignment lyricAlignmentForScene(DisplayScene scene) const {
        if (scene == DisplayScene::Idle)
            return idleQuoteAlignment_;
        return isExpandedView() ? LyricAlignment::Center
                                                        : lyricAlignment_;
    }

    void drawLyricScrollingTextAligned(
        IDWriteTextLayout* layout, float textW, float textH, float areaW, float x, float y,
        float offset, ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow,
        ID2D1Brush* karaokeBrush, float karaokeX, float opacity, LyricAlignment alignment,
        bool singleCopy = false) {
        // 左缘渐隐宽度分场景：歌曲信息可见时用与分隔间距一致的 8 dip，且渐隐始终
        // 在歌词区域内，文字不会画进信息区；信息区隐藏时用默认宽度
        const float leftFadeDip =
            songInfoVisible_ && scene_ != DisplayScene::Idle ? kTextPadding : 0.0f;
        const float centerAnchor = immersiveLyricCenterX();
        // 每日一言与歌曲信息同属内容确定的无限循环跑马灯，是否滚动在内容确定时
        // 已知，渐隐恒定保持，避免每轮循环绕回时左缘渐隐消失再出现
        drawScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                          karaokeBrush, karaokeX, opacity, alignment, singleCopy,
                          kTextPadding, scene_ == DisplayScene::Idle, leftFadeDip,
                          centerAnchor);
    }

    void drawLyricScrollingText(IDWriteTextLayout* layout, float textW, float textH,
                                float areaW, float x, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                                float opacity = 1.0f, bool singleCopy = false) {
        drawLyricScrollingTextAligned(layout, textW, textH, areaW, x, y, offset, brush, outline,
                                      glow, karaokeBrush, karaokeX, opacity,
                                      activeLyricAlignment(), singleCopy);
    }

    void drawScaledScrollingText(IDWriteTextLayout* layout, float textW, float textH,
                                 float areaW, float x, float y, float offset, ID2D1Brush* brush,
                                 float opacity, float scale, ID2D1Brush* outline = nullptr,
                                 ID2D1Brush* glow = nullptr,
                                 ID2D1Brush* karaokeBrush = nullptr,
                                 float karaokeX = 0.0f, bool singleCopy = false,
                                 float visibleW = 0.0f) {
        auto* rt = drawTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        if (scale >= 0.999f) {
            drawLyricScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                                   karaokeBrush, karaokeX, opacity, singleCopy);
            return;
        }

        float anchorX = x + areaW * 0.5f;
        if (activeLyricAlignment() == LyricAlignment::Left)
            anchorX = x;
        else if (activeLyricAlignment() == LyricAlignment::Right)
            anchorX = x + areaW;
        else if (const float centerAnchor = immersiveLyricCenterX(); centerAnchor >= 0.0f)
            anchorX = centerAnchor;
        const D2D1_POINT_2F anchor = D2D1::Point2F(anchorX, y + textH * 0.5f);
        // 外层裁剪先在最终窗口坐标中固定下来，再缩放文字。否则裁剪矩形会和文字
        // 一起围绕任务栏中心缩放；沉浸 / Dock 的歌词安全区左右不对称时，右边界
        // 会短暂移入频谱、时钟和控件区域，转场结束恢复普通裁剪时形成闪烁。
        const D2D1_RECT_F safeClip =
            D2D1::RectF(x, y, x + areaW + kTextPadding, y + textH);
        rt->PushAxisAlignedClip(safeClip, D2D1_ANTIALIAS_MODE_ALIASED);
        // visibleW 是转场期间可见的文本坐标宽度；缩放轴与歌词的实际居中锚点
        // 保持一致，避免新旧两句在入场时发生横向漂移。
        const float drawW = visibleW > 0.0f ? visibleW : areaW;
        D2D1_MATRIX_3X2_F previous{};
        rt->GetTransform(&previous);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, anchor) * previous);
        drawLyricScrollingText(layout, textW, textH, drawW, x, y, offset, brush, outline, glow,
                                karaokeBrush, karaokeX, opacity, singleCopy);
        rt->SetTransform(previous);
        rt->PopAxisAlignedClip();
    }

    void drawDoubleLineLyrics(float lyricAreaX, float lyricAreaW, float h,
                              ID2D1Brush* primaryBrush) {
        if (!lyricLayout_)
            return;

        const float lyricBlockH = lyricHeight_ +
                                  (nextLyricLayout_
                                       ? kLyricPreviewGap + nextLyricHeight_
                                       : 0.0f);
        const float coreY = h * 0.5f - lyricBlockH * 0.5f;
        ID2D1Brush* coreBrush = primaryBrush
                                    ? primaryBrush
                                    : brushLyric_ ? static_cast<ID2D1Brush*>(brushLyric_)
                                                  : static_cast<ID2D1Brush*>(brushText_);
        ID2D1Brush* previewBrush = brushLyricDim_ ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                                 : static_cast<ID2D1Brush*>(brushDim_);
        ID2D1Brush* effectOutline = scene_ != DisplayScene::Idle && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_)
                                        : nullptr;
        ID2D1Brush* effectGlow = scene_ != DisplayScene::Idle && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_)
                                     : nullptr;
        if (isLyricTransitionActive() && outgoingLyricLayout_) {
            const LyricTransitionSample transition = lyricTransitionSample();
            const float movementT = transition.movement;
            float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                       ? outgoingLyricBlockHeight_
                                       : outgoingLyricHeight_;
            float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
            float travel = std::max(outgoingLyricHeight_, lyricHeight_) + kLyricPreviewGap;
            float direction = lyricTransitionDirection_ >= 0 ? 1.0f : -1.0f;
            float oldShift = -direction * travel * movementT;
            float incomingStartY = direction > 0.0f
                                       ? outgoingY + outgoingLyricHeight_ + kLyricPreviewGap
                                       : coreY - travel;
            float incomingY = incomingStartY + (coreY - incomingStartY) * movementT;
            const LyricLine* incomingLine = karaokeLine();
            bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
            ID2D1Brush* incomingBrush =
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : coreBrush;
            float incomingProgX =
                incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
            float incomingScale = kLyricPreviewScale +
                                  (1.0f - kLyricPreviewScale) * movementT;
            // y 是主字号布局的位置；缩小时校正半个缩放差，使视觉中心跟随位移而不跳动。
            float scaledIncomingY =
                incomingY - lyricHeight_ * (1.0f - incomingScale) * 0.5f;

            // 超出部分的消失与第二行上移同步：转场起点取预览期实际可见的右边界
            // （预览放得下=整行可见；放不下=预览渐隐位置，均换算到核心行文本坐标），
            // 随转场进度收敛到核心行可视宽，避免动画第一帧尾部瞬间消失。
            float incomingVisibleW = 0.0f;
            if (lyricWidth_ > lyricAreaW) {
                // 当前缩放比例下恰好覆盖歌词安全区所需的文本坐标宽度。
                // 使用实时比例而不是对两个宽度线性插值，避免中间帧的可见宽度
                // 反而大于安全区，并由外层固定裁剪保证不会侵入右侧固定内容。
                incomingVisibleW = std::min(lyricWidth_, lyricAreaW / incomingScale);
            }

            // 下一行在转场前已经位于核心行下方；转场从这个位置接入核心，避免跳变。
            drawLyricScrollingText(outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_,
                                   lyricAreaW, lyricAreaX, outgoingY + oldShift,
                                   outgoingLyricScrollOffset_, coreBrush,
                                   effectOutline, effectGlow, nullptr, 0.0f,
                                   1.0f - transition.fadeOut);
            if (!outgoingDoubleLine_ && outgoingSecondaryLayout_)
                drawLyricScrollingText(
                    outgoingSecondaryLayout_, outgoingSecondaryWidth_, outgoingSecondaryHeight_,
                    lyricAreaW, lyricAreaX,
                    outgoingY + outgoingLyricHeight_ + 1.0f + oldShift,
                    outgoingSecondaryScrollOffset_, brushDim_, nullptr, nullptr, nullptr, 0.0f,
                    1.0f - transition.fadeOut);
            drawScaledScrollingText(
                lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX, scaledIncomingY,
                lyricScrollOffset_, incomingBrush,
                kLyricPreviewOpacity + (1.0f - kLyricPreviewOpacity) * transition.fadeIn,
                incomingScale,
                effectOutline, effectGlow,
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr,
                incomingProgX, true, incomingVisibleW);
            return;
        }

        // 核心行仍保留原有逐字高亮；下一行只作为低透明度预览，不参与逐字填充。
        const LyricLine* curLine = karaokeLine();
        bool karaoke = curLine && brushLyric_ && brushLyricDim_;
        float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
        drawLyricScrollingText(lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX,
                               coreY, lyricScrollOffset_,
                               karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : coreBrush,
                               effectOutline, effectGlow,
                               karaoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr, progX);

        if (nextLyricLayout_) {
            float nextY = coreY + lyricHeight_ + kLyricPreviewGap;
            drawLyricScrollingText(nextLyricLayout_, nextLyricWidth_, nextLyricHeight_, lyricAreaW,
                                   lyricAreaX, nextY, 0.0f, previewBrush, nullptr, nullptr, nullptr,
                                   0.0f, kLyricPreviewOpacity);
        }
    }

    // 每日一言与歌词属于任务栏的两个展示场景。场景切换时内容层与外层尺寸
    // 使用同一段转场时序，旧内容仍按切换前的内容区绘制，避免水平布局跳变。
    void drawSceneTransition(float w, float lyricAreaX, float lyricAreaW, float h,
                             float lyricBlockH) {
        auto* rt = drawTarget();
        if (!rt || !lyricLayout_ || !outgoingLyricLayout_ || lastPxW_ <= 0 || lastPxH_ <= 0)
            return;

        const int outgoingPxW = sceneTransitionFromPxW_ > 0 ? sceneTransitionFromPxW_
                                                            : lastLogicalPxW_;
        const int outgoingPxH = sceneTransitionFromPxH_ > 0 ? sceneTransitionFromPxH_
                                                            : lastLogicalPxH_;
        const LyricArea outgoingArea = lyricAreaForScene(outgoingScene_, outgoingPxW,
                                                         outgoingPxH);
        const LyricTransitionSample transition = lyricTransitionSample();
        const float movementT = transition.movement;
        const float direction = lyricTransitionDirection_ >= 0 ? 1.0f : -1.0f;
        const bool incomingDoubleLine = useDoubleLineLyrics() && nextLyricLayout_;
        const float incomingBlockH =
            incomingDoubleLine ? lyricHeight_ + kLyricPreviewGap + nextLyricHeight_ : lyricBlockH;
        const float outgoingPreviewH =
            outgoingDoubleLine_ && outgoingNextLyricLayout_
                ? kLyricPreviewGap + outgoingNextLyricHeight_
                : outgoingSecondaryLayout_ ? 1.0f + outgoingSecondaryHeight_ : 0.0f;
        const float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                         ? outgoingLyricBlockHeight_
                                         : outgoingLyricHeight_ + outgoingPreviewH;
        const float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
        const float incomingY = h * 0.5f - incomingBlockH * 0.5f;
        const float travel = std::max(incomingBlockH, outgoingBlockH);
        const float oldShift = -direction * travel * movementT;
        const float newShift = direction * travel * (1.0f - movementT);

        auto primaryBrushForScene = [this](DisplayScene scene) -> ID2D1Brush* {
            if (scene == DisplayScene::Idle)
                return brushText_;
            return brushLyric_ ? static_cast<ID2D1Brush*>(brushLyric_)
                               : static_cast<ID2D1Brush*>(brushText_);
        };
        const bool outgoingEffects = outgoingScene_ != DisplayScene::Idle;
        const bool incomingEffects = scene_ != DisplayScene::Idle;
        ID2D1Brush* outgoingBrush = primaryBrushForScene(outgoingScene_);
        ID2D1Brush* incomingBrush = primaryBrushForScene(scene_);
        ID2D1Brush* outgoingOutline = outgoingEffects && lyricOutline_ ? brushLyricOutline_ : nullptr;
        ID2D1Brush* outgoingGlow = outgoingEffects && lyricGlow_ ? brushLyricGlow_ : nullptr;
        ID2D1Brush* incomingOutline = incomingEffects && lyricOutline_ ? brushLyricOutline_ : nullptr;
        ID2D1Brush* incomingGlow = incomingEffects && lyricGlow_ ? brushLyricGlow_ : nullptr;

        const LyricLine* incomingLine = scene_ == DisplayScene::Lyrics ? karaokeLine() : nullptr;
        const bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
        if (incomingKaraoke)
            incomingBrush = brushLyricDim_;
        const float incomingProgX = incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, h),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        drawLyricScrollingTextAligned(
            outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_, outgoingArea.w,
            outgoingArea.x, outgoingY + oldShift, outgoingLyricScrollOffset_, outgoingBrush,
            outgoingOutline, outgoingGlow, nullptr, 0.0f, 1.0f - transition.fadeOut,
            lyricAlignmentForScene(outgoingScene_));
        if (outgoingDoubleLine_ && outgoingNextLyricLayout_) {
            ID2D1Brush* previewBrush = brushLyricDim_ ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                                     : static_cast<ID2D1Brush*>(brushDim_);
            drawLyricScrollingTextAligned(
                outgoingNextLyricLayout_, outgoingNextLyricWidth_, outgoingNextLyricHeight_,
                outgoingArea.w, outgoingArea.x,
                outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f,
                previewBrush, nullptr, nullptr, nullptr, 0.0f,
                kLyricPreviewOpacity * (1.0f - transition.fadeOut),
                lyricAlignmentForScene(outgoingScene_), true);
        } else if (outgoingSecondaryLayout_) {
            drawLyricScrollingTextAligned(
                outgoingSecondaryLayout_, outgoingSecondaryWidth_, outgoingSecondaryHeight_,
                outgoingArea.w, outgoingArea.x,
                outgoingY + outgoingLyricHeight_ + 1.0f + oldShift,
                outgoingSecondaryScrollOffset_, brushDim_, nullptr, nullptr, nullptr, 0.0f,
                1.0f - transition.fadeOut, lyricAlignmentForScene(outgoingScene_));
        }

        drawLyricScrollingTextAligned(
            lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX, incomingY + newShift,
            lyricScrollOffset_, incomingBrush, incomingOutline, incomingGlow,
            incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr, incomingProgX,
            transition.fadeIn, lyricAlignmentForScene(scene_), true);
        if (incomingDoubleLine) {
            ID2D1Brush* previewBrush = brushLyricDim_ ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                                     : static_cast<ID2D1Brush*>(brushDim_);
            drawLyricScrollingTextAligned(
                nextLyricLayout_, nextLyricWidth_, nextLyricHeight_, lyricAreaW, lyricAreaX,
                incomingY + lyricHeight_ + kLyricPreviewGap + newShift, 0.0f, previewBrush, nullptr,
                nullptr, nullptr, 0.0f, kLyricPreviewOpacity * transition.fadeIn,
                lyricAlignmentForScene(scene_), true);
        } else if (secondaryLayout_) {
            drawLyricScrollingTextAligned(
                secondaryLayout_, secondaryWidth_, secondaryHeight_, lyricAreaW, lyricAreaX,
                incomingY + lyricHeight_ + 1.0f + newShift, secondaryScrollOffset_, brushDim_,
                nullptr, nullptr, nullptr, 0.0f, transition.fadeIn,
                lyricAlignmentForScene(scene_), true);
        }
        rt->PopAxisAlignedClip();
    }

    void drawTransitionTextTo(ID2D1DeviceContext* rt, IDWriteTextLayout* layout, float textW,
                              float textH, float areaW, float x, float y, float offset,
                              ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
                              ID2D1Brush* glow = nullptr,
                              ID2D1Brush* karaokeBrush = nullptr,
                              float karaokeX = 0.0f, bool singleCopy = false) {
        if (!rt || !layout || !brush || areaW <= 0.0f)
            return;
        D2D1_RECT_F clip{x, y, x + areaW, y + textH};
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        float bases[2];
        int n = 0;
        const float centerAnchor = immersiveLyricCenterX();
        auto alignedBase = [&]() {
            float freeW = std::max(0.0f, areaW - textW);
            switch (activeLyricAlignment()) {
            case LyricAlignment::Left:
                return x;
            case LyricAlignment::Right:
                return x + freeW;
            case LyricAlignment::Center:
            default:
                if (centerAnchor >= 0.0f)
                    return std::clamp(centerAnchor - textW * 0.5f, x, x + freeW);
                return x + freeW * 0.5f;
            }
        };
        if (karaokeBrush || singleCopy) {
            bases[n++] = (textW <= areaW) ? alignedBase() : x - offset;
        } else if (textW <= areaW) {
            bases[n++] = alignedBase();
        } else {
            float loopW = textW + kTextPadding * 2.0f;
            bases[n++] = x - offset;
            bases[n++] = x - offset + loopW;
        }
        static constexpr float kDirs[8][2] = {{1.0f, 0.0f},
                                              {0.7071f, 0.7071f},
                                              {0.0f, 1.0f},
                                              {-0.7071f, 0.7071f},
                                              {-1.0f, 0.0f},
                                              {-0.7071f, -0.7071f},
                                              {0.0f, -1.0f},
                                              {0.7071f, -0.7071f}};
        for (int i = 0; i < n; ++i) {
            if (glow) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(
                        D2D1::Point2F(bases[i] + d[0] * 2.4f, y + d[1] * 2.4f), layout, glow);
            }
            if (outline) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(
                        D2D1::Point2F(bases[i] + d[0] * 1.2f, y + d[1] * 1.2f), layout, outline);
            }
        }
        for (int i = 0; i < n; ++i)
            rt->DrawTextLayout(D2D1::Point2F(bases[i], y), layout, brush);
        if (karaokeBrush && karaokeX > 0.0f) {
            rt->PushAxisAlignedClip(D2D1::RectF(bases[0], y, bases[0] + karaokeX, y + textH),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->DrawTextLayout(D2D1::Point2F(bases[0], y), layout, karaokeBrush);
            rt->PopAxisAlignedClip();
        }
        rt->PopAxisAlignedClip();
    }

    void drawLyricBlockSnapshot(ID2D1DeviceContext* dc, bool outgoing, float lyricAreaX,
                                float lyricAreaW, float h) {
        IDWriteTextLayout* main = outgoing ? outgoingLyricLayout_ : lyricLayout_;
        IDWriteTextLayout* secondary = outgoing ? outgoingSecondaryLayout_ : secondaryLayout_;
        if (!main)
            return;
        const float mainW = outgoing ? outgoingLyricWidth_ : lyricWidth_;
        const float mainH = outgoing ? outgoingLyricHeight_ : lyricHeight_;
        const float secondaryW = outgoing ? outgoingSecondaryWidth_ : secondaryWidth_;
        const float secondaryH = outgoing ? outgoingSecondaryHeight_ : secondaryHeight_;
        const float gap = secondary ? 1.0f : 0.0f;
        const float blockH = mainH + gap + secondaryH;
        const float y = h * 0.5f - blockH * 0.5f;
        ID2D1Brush* mainBrush = scene_ == DisplayScene::Idle
                                    ? static_cast<ID2D1Brush*>(brushText_)
                                    : static_cast<ID2D1Brush*>(brushLyric_ ? brushLyric_
                                                                            : brushText_);
        float mainOffset = 0.0f;
        float secondaryOffset = 0.0f;
        ID2D1Brush* karaokeBrush = nullptr;
        float karaokeX = 0.0f;
        if (!outgoing) {
            const LyricLine* incomingLine = karaokeLine();
            const bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
            mainBrush = incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : mainBrush;
            if (incomingKaraoke) {
                karaokeBrush = brushLyric_;
                karaokeX = karaokeSmoothStep(*incomingLine);
            }
            mainOffset = lyricScrollOffset_;
            secondaryOffset = secondaryScrollOffset_;
        } else {
            mainOffset = outgoingLyricScrollOffset_;
            secondaryOffset = outgoingSecondaryScrollOffset_;
        }
        ID2D1Brush* effectOutline = scene_ != DisplayScene::Idle && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_)
                                        : nullptr;
        ID2D1Brush* effectGlow = scene_ != DisplayScene::Idle && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_)
                                     : nullptr;
        drawTransitionTextTo(dc, main, mainW, mainH, lyricAreaW, lyricAreaX, y, mainOffset,
                             mainBrush, effectOutline, effectGlow, karaokeBrush, karaokeX,
                             !outgoing);
        if (secondary)
            drawTransitionTextTo(dc, secondary, secondaryW, secondaryH, lyricAreaW, lyricAreaX,
                                 y + mainH + gap, secondaryOffset, brushDim_, nullptr, nullptr,
                                 nullptr, 0.0f, !outgoing);
    }

    bool prepareLyricTransitionDComp(float lyricAreaX, float lyricAreaW, float h,
                                     float lyricBlockH) {
        if (isVerticalTaskbar() || useDoubleLineLyrics() || outgoingDoubleLine_ ||
            !isLyricTransitionActive() ||
            !outgoingLyricLayout_ ||
            !lyricLayout_ || lastPxW_ <= 0 || lastPxH_ <= 0)
            return false;
        if (!renderer.ensureLyricTransitionLayers(lastPxW_, lastPxH_, lastPxW_, lastPxH_))
            return false;

        if (ID2D1DeviceContext* dc = renderer.beginLyricLayerDraw(0)) {
            drawLyricBlockSnapshot(dc, true, lyricAreaX, lyricAreaW, h);
            if (!renderer.endLyricLayerDraw(0, dc))
                return false;
        } else {
            return false;
        }
        if (ID2D1DeviceContext* dc = renderer.beginLyricLayerDraw(1)) {
            drawLyricBlockSnapshot(dc, false, lyricAreaX, lyricAreaW, h);
            if (!renderer.endLyricLayerDraw(1, dc))
                return false;
        } else {
            return false;
        }

        const float outgoingGap = outgoingSecondaryLayout_ ? 1.0f : 0.0f;
        const float outgoingBlockH =
            outgoingLyricHeight_ + outgoingGap + outgoingSecondaryHeight_;
        const float travel = std::max(lyricBlockH, outgoingBlockH) * scale();
        const float direction = lyricTransitionDirection_ >= 0 ? 1.0f : -1.0f;
        const float durationSec = kLyricTransitionMs / 1000.0f;
        if (!renderer.animateLyricLayer(0, 0.0f, -direction * travel, 1.0f, 0.0f, durationSec))
            return false;
        if (!renderer.animateLyricLayer(1, direction * travel, 0.0f, 0.0f, 1.0f, durationSec))
            return false;
        // 不在此单独 Commit：图层上线随 render() 末尾 present() 的 Commit 与底层新帧
        // 同批生效，避免「图层已上屏、底层旧歌词未撤」的双画帧。动画以提交时刻为起点，
        // 延迟到 present 提交仅差一次绘制耗时，不影响时长。
        renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::Active);
        return true;
    }

    void syncLyricTransitionDComp(bool showControls, float lyricAreaX, float lyricAreaW, float h,
                                  float lyricBlockH) {
        // 图层增删不单独 Commit：改动挂起到 render() 末尾 present() 的 Commit，与承载
        // 歌词的底层新帧同批上屏，避免「图层已撤、底层新帧未上屏」的空窗闪烁。
        // 沉浸切歌期间整组歌曲内容占用同一套合成层；歌词行转场不能再抢占
        // lyricLayers_，否则会把正在滑入的歌曲快照替换掉。
        if (songContentTransition_ && songContentTransition_->compositorLayer)
            return;
        if ((isVerticalTaskbar() && isLyricDCompActive()) || isLyricDCompEndRequested() ||
            (showControls && isLyricDCompActive()) ||
            (isLyricDCompActive() && karaokeLine())) {
            renderer.clearLyricTransitionLayers();
            clearLyricDCompState();
        }
        if (showControls)
            return;
        // 逐字高亮需要每帧跟随真实播放位置；DComp 快照是静态位图，不能在入场期间
        // 更新填充边界，因此逐字歌词转场保留 D2D 路径，普通歌词仍使用合成器动画。
        if (lyricTransitionKind_ != LyricTransitionKind::Scene &&
            !isLyricDCompActive() && isLyricTransitionActive() && !useDoubleLineLyrics() &&
            !outgoingDoubleLine_ &&
            !karaokeLine() && outgoingLyricLayout_ && lyricLayout_) {
            if (!prepareLyricTransitionDComp(lyricAreaX, lyricAreaW, h, lyricBlockH)) {
                renderer.clearLyricTransitionLayers();
                clearLyricDCompState();
            }
        }
    }

    void drawHorizontalSongContent(ID2D1DeviceContext* contentRt, float w, float h, float leftW,
                                   float lyricAreaX, float lyricAreaW, float lyricBlockH,
                                   ID2D1Brush* primaryBrush, bool idleScene,
                                   bool lyricEffectsEnabled, bool showControls,
                                   bool showSpectrum) {
        if (!contentRt)
            return;

        ID2D1DeviceContext* previousTarget = drawTargetOverride_;
        drawTargetOverride_ = contentRt;

        // 左侧封面
        const float s = coverSize();
        const float coverX = kCoverPadding;
        const float coverY = (h - s) * 0.5f;
        if (albumCoverVisible_ && !idleScene) {
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(coverX, coverY, s);
            } else {
                const D2D1_RECT_F coverRect =
                    D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
                if (coverBmp && coverClip_ && coverLayer_) {
                    contentRt->PushLayer(
                        D2D1::LayerParameters1(
                            D2D1::InfiniteRect(), coverClip_,
                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                            D2D1::Matrix3x2F::Translation(coverX, coverY)),
                        coverLayer_);
                    contentRt->DrawBitmap(coverBmp, coverRect, 1.0f,
                                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    contentRt->PopLayer();
                } else {
                    D2D1_ROUNDED_RECT rr{coverRect, 4.0f, 4.0f};
                    contentRt->FillRoundedRectangle(rr, brushDim_);
                }
            }
            // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
            drawPlatformIcon(coverX, coverY, s);
        }

        if (songInfoVisible_ && !idleScene) {
            // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
            // 左右边缘都保留渐隐，右缘在分隔线前收束，避免标题/歌手撞到歌词区
            const float infoX = infoStartX();
            const float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
            const float infoGap = 2.0f;
            const float totalInfoH = titleHeight_ + infoGap + artistHeight_;
            const float infoY = (h - totalInfoH) * 0.5f;
            drawScrollingText(titleLayout_, titleWidth_, titleHeight_, infoW, infoX, infoY,
                              titleScrollOffset_, brushText_, nullptr, nullptr, nullptr, 0.0f,
                              1.0f, LyricAlignment::Center, false, kTextPadding, true,
                              kTextPadding);
            drawScrollingText(artistLayout_, artistWidth_, artistHeight_, infoW, infoX,
                              infoY + titleHeight_ + infoGap, artistScrollOffset_, brushDim_,
                              nullptr, nullptr, nullptr, 0.0f, 1.0f, LyricAlignment::Center,
                              false, kTextPadding, true, kTextPadding);
            drawSongInfoDivider(leftW, h);
        }

        if (showControls) {
            float centers[4]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (inlineControlsLayout(centers, cy, r)) {
                for (int i = 0; i < 3; ++i)
                    drawButton(i, D2D1::Point2F(centers[i], cy), r);
                drawVolumeButton(D2D1::Point2F(centers[3], cy), r);
            }
        } else {
            if (showSpectrum && !isExpandedView()) {
                const float spectrumW = spectrumContentWForScene(scene_, w);
                drawSpectrum(w - spectrumW - kTextPadding, h, spectrumW);
            }
            if (lyricTransitionKind_ == LyricTransitionKind::Scene && isLyricTransitionActive() &&
                outgoingLyricLayout_) {
                drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
            } else if (useDoubleLineLyrics()) {
                drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
            } else if (!isLyricDCompActive()) {
                if (isLyricTransitionActive() && outgoingLyricLayout_) {
                    // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                    const LyricTransitionSample transition = lyricTransitionSample();
                    const float movementT = transition.movement;
                    const LyricLine* incomingLine = karaokeLine();
                    const bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
                    ID2D1Brush* incomingBrush =
                        incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                        : primaryBrush;
                    const float incomingProgX =
                        incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
                    const float outgoingPreviewH =
                        outgoingDoubleLine_ && outgoingNextLyricLayout_
                            ? kLyricPreviewGap + outgoingNextLyricHeight_
                            : outgoingSecondaryLayout_ ? 1.0f + outgoingSecondaryHeight_ : 0.0f;
                    const float outgoingGap = outgoingDoubleLine_ && outgoingNextLyricLayout_
                                                 ? kLyricPreviewGap
                                                 : outgoingSecondaryLayout_ ? 1.0f : 0.0f;
                    const float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                                     ? outgoingLyricBlockHeight_
                                                     : outgoingLyricHeight_ + outgoingPreviewH;
                    const float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                    const float travel = std::max(lyricBlockH, outgoingBlockH);
                    const float oldShift = -static_cast<float>(lyricTransitionDirection_) * travel *
                                           movementT;
                    const float newShift = static_cast<float>(lyricTransitionDirection_) * travel *
                                           (1.0f - movementT);
                    drawLyricScrollingText(
                        outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_, lyricAreaW,
                        lyricAreaX, outgoingY + oldShift, outgoingLyricScrollOffset_, primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_ : nullptr, nullptr, 0.0f,
                        1.0f - transition.fadeOut);
                    if (outgoingDoubleLine_ && outgoingNextLyricLayout_) {
                        ID2D1Brush* previewBrush =
                            brushLyricDim_ ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                           : static_cast<ID2D1Brush*>(brushDim_);
                        drawLyricScrollingText(
                            outgoingNextLyricLayout_, outgoingNextLyricWidth_,
                            outgoingNextLyricHeight_, lyricAreaW, lyricAreaX,
                            outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f,
                            previewBrush, nullptr, nullptr, nullptr, 0.0f,
                            kLyricPreviewOpacity * (1.0f - transition.fadeOut));
                    } else if (outgoingSecondaryLayout_) {
                        drawLyricScrollingText(
                            outgoingSecondaryLayout_, outgoingSecondaryWidth_, outgoingSecondaryHeight_,
                            lyricAreaW, lyricAreaX,
                            outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                            outgoingSecondaryScrollOffset_, brushDim_, nullptr, nullptr, nullptr, 0.0f,
                            1.0f - transition.fadeOut);
                    }
                    // 入场行同步显示真实播放位置的逐字填充，避免转场结束时突然跳色。
                    drawLyricScrollingText(
                        lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX,
                        h * 0.5f - lyricBlockH * 0.5f + newShift, lyricScrollOffset_, incomingBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_ : nullptr,
                        incomingKaraoke ? brushLyric_ : nullptr, incomingProgX,
                        transition.fadeIn, true);
                    if (secondaryLayout_)
                        drawLyricScrollingText(
                            secondaryLayout_, secondaryWidth_, secondaryHeight_, lyricAreaW, lyricAreaX,
                            h * 0.5f - lyricBlockH * 0.5f + lyricHeight_ +
                                (secondaryLayout_ ? 1.0f : 0.0f) + newShift,
                            secondaryScrollOffset_, brushDim_, nullptr, nullptr, nullptr, 0.0f,
                            transition.fadeIn, true);
                } else {
                    // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                    // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                    const LyricLine* curLine = karaokeLine();
                    const bool karaoke = curLine && brushLyric_ && brushLyricDim_;
                    const float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                    drawLyricScrollingText(
                        lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX,
                        h * 0.5f - lyricBlockH * 0.5f, lyricScrollOffset_,
                        karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_ : nullptr,
                        karaoke ? brushLyric_ : nullptr, progX);
                    // 翻译/罗马音仅作整行附属文本，不参与逐字裁剪、描边或光晕。
                    if (secondaryLayout_)
                        drawLyricScrollingText(
                            secondaryLayout_, secondaryWidth_, secondaryHeight_, lyricAreaW, lyricAreaX,
                            h * 0.5f - lyricBlockH * 0.5f + lyricHeight_ +
                                (secondaryLayout_ ? 1.0f : 0.0f),
                            secondaryScrollOffset_, brushDim_);
                }
            }
        }

        drawTargetOverride_ = previousTarget;
    }

    D2D1_ROUNDED_RECT taskbarBackgroundRect(float w, float h) const {
        const float radius = std::min(kCornerRadius, w * 0.5f);
        return D2D1_ROUNDED_RECT{D2D1::RectF(0.0f, 0.0f, w, h), radius, radius};
    }

    void drawTaskbarBackground(float w, float h, float progressExtent, bool vertical,
                               float dynamicBackgroundW, ID2D1Effect* coverBlurChain) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        const D2D1_ROUNDED_RECT bg = taskbarBackgroundRect(w, h);
        if (isAppBarView() && brushBackground_) {
            // 扩展模式会在下方提前返回；Dock 面向工作区的一侧要在此处绘制分隔线。
            const bool appDark = fluent::isWindowsAppDarkMode();
            brushBackground_->SetColor(appDark
                                           ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f)
                                           : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f));
            const float pixel = 1.0f / std::max(scale(), 1.0f);
            const float top = appBarEdge_ == AppBarEdge::Top
                                  ? std::max(0.0f, h - pixel)
                                  : 0.0f;
            rt->FillRectangle(D2D1::RectF(0.0f, top, w, std::min(h, top + pixel)),
                              brushBackground_);
        }
        if (isExpandedView())
            return; // Immersive 和 Dock 的背景由 Composition 模糊层绘制，窗口保持透明。

        if (coverBlurChain && coverLayer_ && brushBackground_) {
            ID2D1RoundedRectangleGeometry* clip = nullptr;
            if (auto* factory = renderer.d2d())
                factory->CreateRoundedRectangleGeometry(bg, &clip);
            if (clip) {
                rt->PushLayer(D2D1::LayerParameters1(
                                  D2D1::InfiniteRect(), clip,
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::Matrix3x2F::Identity(),
                                  coverBackgroundOpacityPct_ / 100.0f),
                              coverLayer_);
                rt->DrawImage(coverBlurChain, D2D1::Point2F(0.0f, 0.0f));
                rt->PopLayer();
                clip->Release();
                brushBackground_->SetColor(lightTheme_
                                               ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f)
                                               : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
                rt->FillRoundedRectangle(bg, brushBackground_);
            }
        } else if (background_ == TaskbarBackground::Solid && brushBackground_) {
            brushBackground_->SetColor(lightTheme_
                                           ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.85f)
                                           : D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.85f));
            rt->FillRoundedRectangle(bg, brushBackground_);
        }

        if (!isExpandedView()) {
            rt->FillRoundedRectangle(bg, brushBg_);
            if (mouseOver_ && brushHover_)
                rt->FillRoundedRectangle(bg, brushHover_);
        }

        if (progressBackgroundActive() && brushProgressBg_ && media.durationMs > 0) {
            const float fraction = static_cast<float>(std::clamp(
                static_cast<double>(positionMs_) / static_cast<double>(media.durationMs), 0.0,
                1.0));
            const float fillExtent = progressExtent * fraction;
            if (fillExtent > 0.5f) {
                const COLORREF c = media.hasDominantColor ? media.dominantColor
                                                          : fluent::accentColor();
                brushProgressBg_->SetColor(
                    fluent::toD2D(c, progressBackgroundOpacityPct_ / 100.0f));
                const float fillRadius = vertical
                                             ? std::min({bg.radiusX, fillExtent * 0.5f,
                                                         w * 0.5f})
                                             : std::min({bg.radiusX, fillExtent * 0.5f,
                                                         h * 0.5f});
                const D2D1_RECT_F fillRect = vertical
                                                 ? D2D1::RectF(0.0f, 0.0f, w, fillExtent)
                                                 : D2D1::RectF(0.0f, 0.0f, fillExtent, h);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(fillRect, fillRadius, fillRadius), brushProgressBg_);
            }
        }

        if (taskbarDynamicBackgroundVisible())
            drawIdleQuoteBackground(w, h, dynamicBackgroundW);

    }

    bool finishTaskbarFrame(HRESULT hr) {
        if (hr == D2DERR_RECREATE_TARGET) {
            runtime_log::writef(L"[taskbar] EndDraw requests device recreate, discarding device resources");
            discardDeviceResources();
            return false;
        }
        if (SUCCEEDED(hr)) {
            const bool startSongTransition = isSongTransitionPending();
            const bool immersiveLayerActive =
                isExpandedView() && songContentTransition_ &&
                songContentTransition_->compositorLayer;
            if (startSongTransition) {
                renderer.resetRoot();
                if (!isMinimalMode() && !isExpandedView()) {
                    const float travel = kSongTransitionTravelDip * scale();
                    const bool vertical = isVerticalTaskbar();
                    const float fromX = vertical ? 0.0f : travel;
                    const float fromY = vertical
                                            ? (taskbarEdge_ == ABE_LEFT ? -travel : travel)
                                            : 0.0f;
                    if (!renderer.animateRoot(fromX, 0.0f, fromY, 0.0f, 0.0f, 1.0f,
                                              kSongTransitionMs / 1000.0f))
                        renderer.resetRoot();
                }
            }
            // Present 失败（设备丢失/重置）时丢弃设备链，下一帧惰性重建
            if (!renderer.present()) {
                discardDeviceResources();
                return false;
            }
            // 只有真正 Present 成功后才消费该阶段；设备丢失时保留 Pending，
            // 下一次资源重建仍会以同一首歌的完整帧启动转场。
            if (startSongTransition) {
                // 合成层已经在运行时，新的切歌请求不能在本帧被旧动画消费；
                // 等当前层收尾后再启动下一张快照。
                if (immersiveLayerActive) {
                    if (songContentTransition_ && !songContentTransition_->pendingConsumed) {
                        setSongTransitionPending(false);
                        songContentTransition_->pendingConsumed = true;
                    }
                } else {
                    setSongTransitionPending(false);
                }
            }
            return true;
        } else {
            runtime_log::writef(L"[taskbar] EndDraw failed: 0x%08X", hr);
            discardDeviceResources();
            return false;
        }
    }

    bool renderVerticalTaskbar(float w, float h, ID2D1Effect* coverBlurChain) {
        auto* rt = renderer.renderTarget();
        if (!rt || w <= 0.0f || h <= 0.0f)
            return false;

        const VerticalLayout layout = verticalLayout();
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        drawTaskbarBackground(w, h, h, true, w, coverBlurChain);

        const bool playbackScene = scene_ != DisplayScene::Idle &&
                                    scene_ != DisplayScene::NoPlayback;
        if (layout.coverSize > 0.0f && playbackScene) {
            const float s = layout.coverSize;
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(layout.coverX, layout.coverY, s);
            } else {
                const D2D1_RECT_F coverRect =
                    D2D1::RectF(layout.coverX, layout.coverY, layout.coverX + s,
                                layout.coverY + s);
                if (coverBmp && coverClip_ && coverLayer_) {
                    rt->PushLayer(D2D1::LayerParameters1(
                                      D2D1::InfiniteRect(), coverClip_,
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::Matrix3x2F::Translation(layout.coverX, layout.coverY)),
                                  coverLayer_);
                    rt->DrawBitmap(coverBmp, coverRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    rt->PopLayer();
                } else if (brushDim_) {
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(coverRect, std::min(4.0f, s * 0.18f),
                                          std::min(4.0f, s * 0.18f)),
                        brushDim_);
                }
            }
            drawPlatformIcon(layout.coverX, layout.coverY, layout.coverSize);
        }

        if (layout.showControls) {
            for (int i = 0; i < 3; ++i)
                drawButton(i, layout.controlCenters[i], layout.controlRadius);
            drawVolumeButton(layout.controlCenters[3], layout.controlRadius);
        } else {
            drawVerticalLyrics(layout);
        }

        return finishTaskbarFrame(rt->EndDraw());
    }

    bool renderDragPreview(float w, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushBackground_ || !brushDim_ || !brushText_ || !fmtDragPreview_ ||
            w <= 0.0f || h <= 0.0f)
            return false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const float inset = 1.5f;
        const float radius = std::max(
            1.0f, std::min(kCornerRadius, std::min(w, h) * 0.5f - inset));
        const D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(
            D2D1::RectF(inset, inset, std::max(inset, w - inset),
                        std::max(inset, h - inset)),
            radius, radius);
        brushBackground_->SetColor(lightTheme_
                                       ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.88f)
                                       : D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.82f));
        rt->FillRoundedRectangle(preview, brushBackground_);
        rt->DrawRoundedRectangle(preview, brushDim_, 1.25f, dragPreviewStroke_);

        const float textInset = 8.0f;
        const D2D1_RECT_F textRect = D2D1::RectF(
            textInset, textInset, std::max(textInset, w - textInset),
            std::max(textInset, h - textInset));
        if (isVerticalTaskbar()) {
            constexpr wchar_t verticalText[] = L"松\n手\n固\n定\n到\n这\n里";
            rt->DrawTextW(verticalText, _countof(verticalText) - 1, fmtDragPreview_, textRect,
                          brushText_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            rt->DrawTextW(kDragPreviewText, _countof(kDragPreviewText) - 1, fmtDragPreview_,
                          textRect, brushText_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return finishTaskbarFrame(rt->EndDraw());
    }

    void render() {
        if (!isWindowVisible() || !hwnd)
            return;

        if (isExpandedView() && isSceneResizeActive())
            cancelSceneWindowResize();
        if (isSceneResizeActive())
            updateSceneWindowResize(monotonicNowMs());

        // 先调整窗口，再读取客户区并绑定位图，避免用旧尺寸的位图提交后
        // 把刚刚收缩/扩展的窗口尺寸恢复回去。
        if (isInvalidated(RenderInvalidation::Layout) && !isSceneResizeActive() &&
            !lyricDragging_) {
            adjustPosition();
            if (!isWindowVisible())
                return;
        }
        ensureImmersiveZOrder();

        int pxW = 0;
        int pxH = 0;
        clientPixelSize(pxW, pxH);
        if (pxW <= 0 || pxH <= 0)
            return;
        const bool vertical = isVerticalTaskbar();
        int logicalPxW = pxW;
        int logicalPxH = pxH;
        if (pxW != lastPxW_ || pxH != lastPxH_) {
            if (isLyricDCompActive())
                requestLyricDCompEnd();
            if (songContentTransition_ && songContentTransition_->compositorLayer)
                clearImmersiveSongContentTransition();
            lastPxW_ = pxW;
            lastPxH_ = pxH;
            requestInvalidation(RenderInvalidation::Geometry);
            if (vertical)
                requestInvalidation(RenderInvalidation::Text);
            // 横向文本布局以超宽无换行创建，度量与窗口尺寸无关；侧边竖排布局
            // 依赖任务栏窄栏宽度，已在上面标记 Text 失效以便重建。
        }
        lastLogicalPxW_ = logicalPxW;
        lastLogicalPxH_ = logicalPxH;

        if (!renderer.bind(hwnd, pxW, pxH))
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);
        // DComp 设备上下文在首次 bind 后才存在，画刷/图层创建必须排在 bind 之后
        createDeviceResources();
        if (renderState_.deviceResourcePhase() != RenderState::DeviceResourcePhase::Ready)
            return;

        // 预处理和资源创建阶段可能追加失效项；从这里开始才捕获本次提交快照，
        // 避免把本帧刚处理的尺寸/字体失效错误地留到下一帧。
        const InvalidationSnapshot invalidation = renderState_.beginInvalidation();

        if (lyricDragging_) {
            if (isInvalidated(RenderInvalidation::Text) ||
                isInvalidated(RenderInvalidation::SongInfo))
                buildTextLayouts(0.0f, 0.0f);
            if (updateDragPreviewPlacement()) {
                clientPixelSize(pxW, pxH);
                if (pxW <= 0 || pxH <= 0 || !renderer.bind(hwnd, pxW, pxH))
                    return;
                rt = renderer.renderTarget();
                if (!rt)
                    return;
                renderer.setDpi(dpi_);
            }
            if (renderDragPreview(dip(pxW), dip(pxH))) {
                // 预览只消费本帧实际处理过的文字/布局/绘制失效；封面、图标与普通
                // 内容几何继续保留，松手恢复歌词后再由完整渲染链处理。
                InvalidationSnapshot previewInvalidation = invalidation;
                previewInvalidation.mask &=
                    toMask(RenderInvalidation::Paint) |
                    toMask(RenderInvalidation::Text) |
                    toMask(RenderInvalidation::SongInfo) |
                    toMask(RenderInvalidation::Layout);
                renderState_.commitInvalidation(previewInvalidation);
            }
            return;
        }

        ensureGeometry();
        if (isInvalidated(RenderInvalidation::Cover))
            decodeCover();
        if (isInvalidated(RenderInvalidation::PlatformIcon))
            decodePlatformIcon();

        if (vertical) {
            if (isInvalidated(RenderInvalidation::Text) ||
                isInvalidated(RenderInvalidation::SongInfo))
                buildTextLayouts(0.0f, 0.0f);
            if (isLyricDCompActive() || isLyricDCompEndRequested()) {
                renderer.clearLyricTransitionLayers();
                clearLyricDCompState();
            }
            ID2D1Effect* coverBlurChain = background_ == TaskbarBackground::CoverBlur && coverBmp
                                              ? ensureCoverBlurChain(dip(pxW), dip(pxH))
                                              : nullptr;
            if (renderVerticalTaskbar(dip(pxW), dip(pxH), coverBlurChain))
                renderState_.commitInvalidation(invalidation);
            return;
        }

        // 时钟按本地分钟缓存；首帧在这里同步，后续由帧定时器仅在分钟变化时重绘。
        if (!isAppBarView())
            refreshImmersiveClockText();

        LayoutMetrics layout = layoutMetrics(logicalPxW, logicalPxH);
        float w = layout.w;
        float h = layout.h;
        float leftW = layout.leftW;
        float rightW = layout.rightW;

        if (isInvalidated(RenderInvalidation::Text) ||
            isInvalidated(RenderInvalidation::SongInfo))
            buildTextLayouts(leftW, rightW);

        const LyricArea lyricArea = lyricAreaForScene(scene_, logicalPxW, logicalPxH);
        const float lyricAreaX = lyricArea.x;
        const float lyricAreaW = lyricArea.w;
        float secondaryGap = secondaryLayout_ ? 1.0f : 0.0f;
        float lyricBlockH = lyricHeight_ + secondaryGap + secondaryHeight_;
        float lyricY = h * 0.5f - lyricBlockH * 0.5f;
        const bool idleScene = scene_ == DisplayScene::Idle;
        // 每日一言属于任务栏主题正文，不继承上一次播放歌词的用户自定义颜色。
        const bool lyricEffectsEnabled = !idleScene;
        ID2D1Brush* primaryBrush = idleScene
                                       ? static_cast<ID2D1Brush*>(brushText_)
                                       : static_cast<ID2D1Brush*>(brushLyric_ ? brushLyric_
                                                                                : brushText_);
        // 只有开启悬浮控件且鼠标位于窗口内时才替换歌词，否则保持歌词/频谱视图。
        bool showControls = mouseOver_ && controlsOnHover_ &&
                            hoverControlStyle_ == HoverControlStyle::Inline && !idleScene &&
                            !isExpandedView() &&
                            !(lyricTransitionKind_ == LyricTransitionKind::Scene &&
                              isLyricTransitionActive());
        const bool backgroundSpectrum =
            !idleScene && spectrumVisible_ && backgroundWaveEnabled();
        // 独立频谱占用歌词区右端；背景波浪不改变窗口宽度和歌词布局。
        bool showSpectrum = !idleScene && spectrumVisible_ && !showControls && !backgroundSpectrum;
        syncLyricTransitionDComp(showControls, lyricAreaX, lyricAreaW, h, lyricBlockH);

        // 模糊效果链涉及资源创建（CreateEffect），与画刷一样放在 BeginDraw 之前
        ID2D1Effect* coverBlurChain = background_ == TaskbarBackground::CoverBlur && coverBmp
                                           ? ensureCoverBlurChain(w, h)
                                           : nullptr;

        beginImmersiveSongContentTransition();
        bool compositorSongContent =
            songContentTransition_ && songContentTransition_->compositorLayer;
        bool compositorSongContentPending = false;
        if (isExpandedView() && songContentTransition_ &&
            !songContentTransition_->compositorLayer) {
            // 歌词行转场也使用 lyricLayers_。整首歌切换时先交给歌曲内容层，
            // 保证封面、歌曲信息和歌词成为同一张快照，避免两套合成层互相抢占。
            renderer.clearLyricTransitionLayers();
            clearLyricDCompState();
            if (renderer.ensureLyricTransitionLayers(lastPxW_, lastPxH_, 1, 1)) {
                compositorSongContent = true;
                compositorSongContentPending = true;
            }
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const float dynamicBackgroundW = showSpectrum
                                              ? std::max(1.0f, lyricAreaX + lyricAreaW)
                                              : w;
        // 背景、进度和动态底图由横竖两种布局共用，避免后续修复只落在一条路径。
        drawTaskbarBackground(w, h, lyricAreaX + lyricAreaW, false, dynamicBackgroundW,
                              coverBlurChain);

        if (backgroundSpectrum) {
            const float waveX = infoStartX();
            const float waveRight = backgroundWaveRight(w);
            const float waveW = std::max(1.0f, waveRight - waveX);
            drawBackgroundWaveSpectrum(waveX, h, waveW);
        }

        // 沉浸模式的操作组、频谱和时钟都属于固定遮罩层内容，必须在歌曲内容
        // 过渡变换之前绘制；切歌时只让封面、歌曲信息和歌词内容移动。
        if (isExpandedView()) {
            drawImmersiveControls();
            if (isAppBarView() && !idleScene)
                drawDockPet(w, h, leftW);
            drawImmersiveRightSide(w, h, showSpectrum);
        }

        bool contentTransitionActive = false;
        if (!compositorSongContent) {
            contentTransitionActive = applyImmersiveSongContentTransform(rt);
        }

        // 左侧封面
        if (!compositorSongContent) {
        float s = coverSize();
        float coverX = kCoverPadding;
        float coverY = (h - s) * 0.5f;
        if (albumCoverVisible_ && !idleScene) {
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(coverX, coverY, s);
            } else {
                D2D1_RECT_F coverRect = D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
                if (coverBmp && coverClip_ && coverLayer_) {
                    rt->PushLayer(D2D1::LayerParameters1(
                                      D2D1::InfiniteRect(), coverClip_,
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::Matrix3x2F::Translation(coverX, coverY)),
                                  coverLayer_);
                    rt->DrawBitmap(coverBmp, coverRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    rt->PopLayer();
                } else {
                    D2D1_ROUNDED_RECT rr{coverRect, 4.0f, 4.0f};
                    rt->FillRoundedRectangle(rr, brushDim_);
                }
            }
            // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
            drawPlatformIcon(coverX, coverY, s);
        }

        if (songInfoVisible_ && !idleScene) {
            // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
            // 左右边缘都保留渐隐，右缘在分隔线前收束，避免标题/歌手撞到歌词区
            float infoX = infoStartX();
            float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
            float infoGap = 2.0f;
            float totalInfoH = titleHeight_ + infoGap + artistHeight_;
            float infoY = (h - totalInfoH) * 0.5f;
            drawScrollingText(titleLayout_, titleWidth_, titleHeight_, infoW, infoX, infoY,
                              titleScrollOffset_, brushText_, nullptr, nullptr, nullptr, 0.0f,
                              1.0f, LyricAlignment::Center, false, kTextPadding, true,
                              kTextPadding);
            drawScrollingText(artistLayout_, artistWidth_, artistHeight_, infoW, infoX,
                              infoY + titleHeight_ + infoGap, artistScrollOffset_, brushDim_,
                              nullptr, nullptr, nullptr, 0.0f, 1.0f, LyricAlignment::Center,
                              false, kTextPadding, true, kTextPadding);
            drawSongInfoDivider(leftW, h);
        }

        if (showControls) {
            float centers[4]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (inlineControlsLayout(centers, cy, r)) {
                for (int i = 0; i < 3; ++i)
                    drawButton(i, D2D1::Point2F(centers[i], cy), r);
                drawVolumeButton(D2D1::Point2F(centers[3], cy), r);
            }
        } else {
            if (showSpectrum && !isExpandedView()) {
                const float spectrumW = spectrumContentWForScene(scene_, w);
                drawSpectrum(w - spectrumW - kTextPadding, h, spectrumW);
            }
            if (lyricTransitionKind_ == LyricTransitionKind::Scene && isLyricTransitionActive() &&
                outgoingLyricLayout_) {
                drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
            } else if (useDoubleLineLyrics()) {
                drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
            } else if (!isLyricDCompActive()) {
                if (isLyricTransitionActive() && outgoingLyricLayout_) {
                    // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                    const LyricTransitionSample transition = lyricTransitionSample();
                    const float movementT = transition.movement;
                    const LyricLine* incomingLine = karaokeLine();
                    bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
                    ID2D1Brush* incomingBrush =
                        incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                        : primaryBrush;
                    const float incomingProgX =
                        incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
                    const float outgoingPreviewH =
                        outgoingDoubleLine_ && outgoingNextLyricLayout_
                            ? kLyricPreviewGap + outgoingNextLyricHeight_
                            : outgoingSecondaryLayout_ ? 1.0f + outgoingSecondaryHeight_ : 0.0f;
                    float outgoingGap = outgoingDoubleLine_ && outgoingNextLyricLayout_
                                            ? kLyricPreviewGap
                                            : outgoingSecondaryLayout_ ? 1.0f : 0.0f;
                    float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                               ? outgoingLyricBlockHeight_
                                               : outgoingLyricHeight_ + outgoingPreviewH;
                    float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                    float travel = std::max(lyricBlockH, outgoingBlockH);
                    float oldShift = -static_cast<float>(lyricTransitionDirection_) * travel *
                                     movementT;
                    float newShift = static_cast<float>(lyricTransitionDirection_) * travel *
                                     (1.0f - movementT);
                    drawLyricScrollingText(
                        outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_, lyricAreaW,
                        lyricAreaX, outgoingY + oldShift, outgoingLyricScrollOffset_,
                        primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_ : nullptr,
                        nullptr, 0.0f,
                        1.0f - transition.fadeOut);
                    if (outgoingDoubleLine_ && outgoingNextLyricLayout_) {
                        ID2D1Brush* previewBrush =
                            brushLyricDim_ ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                           : static_cast<ID2D1Brush*>(brushDim_);
                        drawLyricScrollingText(
                            outgoingNextLyricLayout_, outgoingNextLyricWidth_,
                            outgoingNextLyricHeight_, lyricAreaW, lyricAreaX,
                            outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f,
                            previewBrush, nullptr, nullptr, nullptr, 0.0f,
                            kLyricPreviewOpacity * (1.0f - transition.fadeOut));
                    } else if (outgoingSecondaryLayout_)
                        drawLyricScrollingText(outgoingSecondaryLayout_, outgoingSecondaryWidth_,
                                               outgoingSecondaryHeight_, lyricAreaW, lyricAreaX,
                                               outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                                               outgoingSecondaryScrollOffset_, brushDim_, nullptr,
                                               nullptr, nullptr, 0.0f,
                                               1.0f - transition.fadeOut);
                    // 入场行同步显示真实播放位置的逐字填充，避免转场结束时突然跳色。
                    drawLyricScrollingText(lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW,
                                           lyricAreaX, lyricY + newShift, lyricScrollOffset_,
                                           incomingBrush,
                                           lyricEffectsEnabled && lyricOutline_
                                               ? brushLyricOutline_
                                               : nullptr,
                                           lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_
                                                                              : nullptr,
                                           incomingKaraoke ? brushLyric_ : nullptr, incomingProgX,
                                           transition.fadeIn, true);
                    if (secondaryLayout_)
                        drawLyricScrollingText(secondaryLayout_, secondaryWidth_, secondaryHeight_,
                                               lyricAreaW, lyricAreaX,
                                               lyricY + lyricHeight_ + secondaryGap + newShift,
                                               secondaryScrollOffset_, brushDim_, nullptr, nullptr,
                                               nullptr, 0.0f, transition.fadeIn, true);
                } else {
                    // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                    // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                    const LyricLine* curLine = karaokeLine();
                    bool karaoke = curLine && brushLyric_ && brushLyricDim_;
                    float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                    drawLyricScrollingText(
                        lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX, lyricY,
                        lyricScrollOffset_,
                        karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                : primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_ : nullptr,
                        karaoke ? brushLyric_ : nullptr, progX);
                    // 翻译/罗马音仅作整行附属文本，不参与逐字裁剪、描边或光晕。
                    if (secondaryLayout_)
                        drawLyricScrollingText(secondaryLayout_, secondaryWidth_, secondaryHeight_,
                                               lyricAreaW, lyricAreaX,
                                               lyricY + lyricHeight_ + secondaryGap,
                                               secondaryScrollOffset_, brushDim_);
                }
            }
        }

        }

        if (contentTransitionActive)
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        // Idle 内容先绘制每日一言，再把宠物放到内容层上方，保证两者同时可见。
        if (isAppBarView() && idleScene)
            drawDockPet(w, h, leftW);

        HRESULT frameHr = rt->EndDraw();
        if (SUCCEEDED(frameHr) && compositorSongContentPending) {
            bool layerReady = false;
            if (ID2D1DeviceContext* layerRt = renderer.beginLyricLayerDraw(0)) {
                drawHorizontalSongContent(layerRt, w, h, leftW, lyricAreaX, lyricAreaW,
                                          lyricBlockH, primaryBrush, idleScene,
                                          lyricEffectsEnabled, showControls, showSpectrum);
                layerReady = renderer.endLyricLayerDraw(0, layerRt);
            }
            if (layerReady) {
                const float travelPx = kSongTransitionTravelDip * scale();
                layerReady = renderer.animateLyricLayerX(
                    0, travelPx, 0.0f, 0.0f, 1.0f, 1.0f, kSongTransitionMs / 1000.0f);
            }
            if (layerReady && songContentTransition_) {
                songContentTransition_->compositorLayer = true;
                // DComp 动画的真实起点在本次 Present/Commit 附近；快照绘制可能耗时，
                // 不能沿用创建转场时的时间，否则动画会被提前收尾。
                songContentTransition_->startMs = monotonicNowMs();
            } else {
                renderer.clearLyricTransitionLayers();
                clearLyricDCompState();
                // 合成层创建失败时回到原有 D2D 路径，当前帧仍然完整可见。
                rt->BeginDraw();
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
                drawTaskbarBackground(w, h, lyricAreaX + lyricAreaW, false, dynamicBackgroundW,
                                      coverBlurChain);
                if (backgroundSpectrum) {
                    const float waveX = infoStartX();
                    const float waveRight = backgroundWaveRight(w);
                    const float waveW = std::max(1.0f, waveRight - waveX);
                    drawBackgroundWaveSpectrum(waveX, h, waveW);
                }
                if (isExpandedView()) {
                    drawImmersiveControls();
                    if (isAppBarView() && !idleScene)
                        drawDockPet(w, h, leftW);
                    drawImmersiveRightSide(w, h, showSpectrum);
                }
                contentTransitionActive = applyImmersiveSongContentTransform(rt);
                // 失败回退时仍需绘制歌曲内容；这里复用与快照相同的绘制入口，
                // 避免回退路径和正常路径再次产生视觉差异。
                drawHorizontalSongContent(rt, w, h, leftW, lyricAreaX, lyricAreaW,
                                          lyricBlockH, primaryBrush, idleScene,
                                          lyricEffectsEnabled, showControls, showSpectrum);
                if (contentTransitionActive)
                    rt->SetTransform(D2D1::Matrix3x2F::Identity());
                if (isAppBarView() && idleScene)
                    drawDockPet(w, h, leftW);
                frameHr = rt->EndDraw();
            }
        }
        if (finishTaskbarFrame(frameHr))
            renderState_.commitInvalidation(invalidation);
    }

    // ---------- 滚动字幕 ----------

    void updateScroll() {
        ULONGLONG now = monotonicNowMs();
        frameNowMs_ = now;
        if (lastTickMs_ == 0)
            lastTickMs_ = now;
        float dt = static_cast<float>(now - lastTickMs_) / 1000.0f;
        lastTickMs_ = now;

        if (isLyricTransitionInProgress() &&
            lyricTransitionRevision_ != 0 && lyricTransitionRevision_ != frameRevision_) {
            // 不存在历史过渡队列：版本过期时直接丢弃旧过渡，下一次排版只处理当前行。
            resetLyricTransition();
            requestInvalidation(RenderInvalidation::Text);
        }

        const bool wasTransitioning = isLyricTransitionInProgress();
        if (isLyricTransitionActive() &&
            now - lyricTransitionStartMs_ >=
                static_cast<ULONGLONG>(lyricTransitionDurationMs())) {
            finalizeLyricTransition(now);
        }

        // 静止跳帧判定：本帧有任何滚动/转场在动就置 animating，供 onTimer 决定是否重绘
        bool animating = wasTransitioning;
        auto marquee = [&](float textW, float areaW, float speed, float& offset,
                           float loopGap = kTextPadding * 2.0f) {
            if (!clientAnimations_) {
                if (offset != 0.0f) {
                    offset = 0.0f;
                    animating = true;
                }
                return;
            }
            if (textW <= areaW || areaW <= 0.0f) {
                if (offset != 0.0f) {
                    offset = 0.0f;
                    animating = true;
                }
                return;
            }
            float loopW = textW + loopGap;
            offset = std::fmod(offset + speed * std::max(dt, 0.0f), loopW);
            if (offset < 0.0f)
                offset += loopW;
            animating = true;
        };
        auto finishOneShotStatus = [&]() {
            statusTextOneShot_ = false;
            statusTextOneShotStartMs_ = 0;
            statusTextOneShotRounds_ = 0;
            statusTextCycleCallbackPending_ = true;
        };
        auto oneShotMarquee = [&](float textW, float areaW, float speed, float& offset,
                                  float loopGap) {
            if (!statusTextOneShot_) {
                marquee(textW, areaW, speed, offset, loopGap);
                return false;
            }
            if (statusTextOneShotStartMs_ == 0)
                statusTextOneShotStartMs_ = now;
            if (!clientAnimations_ || textW <= areaW || areaW <= 0.0f) {
                if (offset != 0.0f) {
                    offset = 0.0f;
                    animating = true;
                }
                if (now - statusTextOneShotStartMs_ >= kOneShotStatusTextHoldMs) {
                    finishOneShotStatus();
                    return true;
                }
                return false;
            }

            const float loopW = textW + loopGap;
            const float advance = speed * std::max(dt, 0.0f);
            const float previous = offset;
            const float total = previous + advance;
            const int completedRounds = static_cast<int>(total / loopW);
            offset = std::fmod(total, loopW);
            if (offset < 0.0f)
                offset += loopW;
            animating = true;
            if (completedRounds > 0) {
                statusTextOneShotRounds_ += completedRounds;
            }
            if (statusTextOneShotRounds_ >= kOneShotStatusTextRounds) {
                finishOneShotStatus();
                return true;
            }
            return false;
        };

        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        if (isVerticalTaskbar()) {
            const VerticalLayout verticalLayoutState = verticalLayout();
            const float lyricAreaH = verticalLayoutState.lyricBottom -
                                     verticalLayoutState.lyricY;
            if (lyricAreaH <= 0.0f) {
                scrollAnimating_ = animating;
                return;
            }

            const bool holdLyricScroll = isLyricTransitionInProgress();
            const bool lyricMarqueePlaying = media.playing;
            const bool idleMarquee = scene_ == DisplayScene::Idle;
            const float verticalLyricExtent = verticalLyricHeight_ > 0.0f
                                                  ? verticalLyricHeight_
                                                  : lyricWidth_;
            if (verticalLayoutState.showControls) {
                scrollAnimating_ = animating;
                return;
            }
            if (karaokeLine() && clientAnimations_) {
                const float before = lyricScrollOffset_;
                if (!holdLyricScroll) {
                    if (verticalLyricExtent > lyricAreaH) {
                        const float sungX =
                            (karaokeSmoothLine_ == currentLine) ? karaokeProgX_ : 0.0f;
                        // 普通竖向跑马灯的 offset 正值代表向下；逐字高亮跟随时，
                        // 只有内容超出底部后才向上回收文本，避免当前字被推出可视区。
                        const float target = std::clamp(
                            lyricAreaH * 0.3f -
                                verticalLyricExtent *
                                    (lyricWidth_ > 0.0f ? sungX / lyricWidth_ : 0.0f),
                            lyricAreaH - verticalLyricExtent, 0.0f);
                        const float diff = target - lyricScrollOffset_;
                        const float followDtMs =
                            std::clamp(std::max(dt, 0.0f) * 1000.0f, 0.0f, 250.0f);
                        const float alpha = 1.0f - std::exp(-followDtMs / kKaraokeScrollFollowMs);
                        lyricScrollOffset_ =
                            std::fabs(diff) < 0.5f ? target : lyricScrollOffset_ + diff * alpha;
                    } else {
                        lyricScrollOffset_ = 0.0f;
                    }
                }
                if (lyricScrollOffset_ != before)
                    animating = true;
            } else if (karaokeLine()) {
                if (lyricScrollOffset_ != 0.0f) {
                    lyricScrollOffset_ = 0.0f;
                    animating = true;
                }
            } else if ((lyricMarqueePlaying || idleMarquee) && !holdLyricScroll) {
                if (statusTextOneShot_ && idleMarquee) {
                    if (oneShotMarquee(verticalLyricExtent, lyricAreaH, kInfoScrollSpeed,
                                       lyricScrollOffset_, kVerticalLyricGap)) {
                        scrollAnimating_ = false;
                        return;
                    }
                } else {
                    marquee(verticalLyricExtent, lyricAreaH,
                            idleMarquee ? kInfoScrollSpeed : lyricScrollSpeed_,
                            lyricScrollOffset_, kVerticalLyricGap);
                }
            }
            scrollAnimating_ = animating;
            return;
        }
        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.h <= 0.0f || layout.w <= 0.0f) {
            scrollAnimating_ = animating;
            return;
        }

        float w = layout.w;
        float h = layout.h;
        float leftW = layout.leftW;
        float infoX = infoStartX();
        float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
        // 与 render 使用同一安全区：周边元素只影响可绘制边界，不影响居中锚点。
        const LyricArea lyricArea = lyricAreaForScene(scene_, pxW, pxH);
        const float lyricAreaW = lyricArea.w;
        const bool holdLyricScroll = isLyricTransitionInProgress();
        // 普通横向歌词只在播放中推进；每日一言无播放状态也允许慢速滚动。
        // 暂停歌词时保留偏移，恢复播放后从原位置继续。
        const bool lyricMarqueePlaying = media.playing;
        const bool idleMarquee = scene_ == DisplayScene::Idle;

        if (songInfoVisible_) {
            marquee(titleWidth_, infoW, kInfoScrollSpeed, titleScrollOffset_);
            marquee(artistWidth_, infoW, kInfoScrollSpeed, artistScrollOffset_);
        }
        // 空闲场景没有内嵌播放控件，悬浮时也应继续滚动每日一言。
        if (mouseOver_ && controlsOnHover_ && hoverControlStyle_ == HoverControlStyle::Inline &&
            !idleMarquee) {
            scrollAnimating_ = animating;
            return;
        }
        // 逐字歌词：滚动位置跟随逐字填充进度，把当前唱到的位置保持在区域 30% 处，
        // 行尾为止不再循环；非逐字歌词保持原有无缝循环滚动
        if (karaokeLine() && clientAnimations_) {
            const float before = lyricScrollOffset_;
            if (!holdLyricScroll) {
                if (lyricWidth_ > lyricAreaW) {
                    // karaokeProgX_ 由 render 每帧更新；行刚切换时还没对应进度，从头开始
                    float sungX = (karaokeSmoothLine_ == currentLine) ? karaokeProgX_ : 0.0f;
                    float target =
                        std::clamp(sungX - lyricAreaW * 0.3f, 0.0f, lyricWidth_ - lyricAreaW);
                    float diff = target - lyricScrollOffset_;
                    // 用时间常数而不是固定帧比例，保证 30/60/120Hz 下观感一致。
                    const float followDtMs =
                        std::clamp(std::max(dt, 0.0f) * 1000.0f, 0.0f, 250.0f);
                    const float alpha = 1.0f - std::exp(-followDtMs / kKaraokeScrollFollowMs);
                    lyricScrollOffset_ =
                        std::fabs(diff) < 0.5f ? target : lyricScrollOffset_ + diff * alpha;
                } else {
                    lyricScrollOffset_ = 0.0f;
                }
            }
            if (lyricScrollOffset_ != before)
                animating = true;
        } else if (karaokeLine()) {
            if (lyricScrollOffset_ != 0.0f) {
                lyricScrollOffset_ = 0.0f;
                animating = true;
            }
        } else if ((lyricMarqueePlaying || idleMarquee) && !holdLyricScroll) {
            if (statusTextOneShot_ && idleMarquee) {
                if (oneShotMarquee(lyricWidth_, lyricAreaW, kInfoScrollSpeed,
                                   lyricScrollOffset_, kTextPadding * 2.0f)) {
                    scrollAnimating_ = false;
                    return;
                }
            } else {
                marquee(lyricWidth_, lyricAreaW,
                        idleMarquee ? kInfoScrollSpeed : lyricScrollSpeed_, lyricScrollOffset_);
            }
        }
        if (lyricMarqueePlaying && !holdLyricScroll)
            marquee(secondaryWidth_, lyricAreaW, kLyricScrollSpeed, secondaryScrollOffset_);
        scrollAnimating_ = animating;
    }

    void updateVinylRotation() {
        ULONGLONG now = monotonicNowMs();
        if (vinylTickMs_ == 0)
            vinylTickMs_ = now;
        if (albumCoverEffect_ != AlbumCoverEffect::Vinyl || !media.playing ||
            !clientAnimations_) {
            // 暂停/切回默认效果时把时间锚点重置，恢复播放不会补算暂停期间的角度。
            vinylTickMs_ = now;
            return;
        }
        ULONGLONG elapsed = now - vinylTickMs_;
        vinylTickMs_ = now;
        vinylAngleDeg_ = std::fmod(
            vinylAngleDeg_ + static_cast<float>(elapsed) * kVinylRotationDegPerSecond / 1000.0f,
            360.0f);
        if (vinylAngleDeg_ < 0.0f)
            vinylAngleDeg_ += 360.0f;
    }

    // ---------- 事件 ----------

    void updateDisplayRefresh() {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency >= 24)
            displayRefreshHz_ = mode.dmDisplayFrequency;
    }

    void refreshFrameTimer() {
        if (!timerRunning_ || !hwnd)
            return;
        const UINT wantMs = desiredFrameMs();
        if (wantMs == timerMs_)
            return;
        SetTimer(hwnd, kTimerId, wantMs, nullptr);
        timerMs_ = wantMs;
    }

    UINT activeFrameMs() const {
        // 只有低渲染模式固定 ~30fps；极简模式仍使用正常刷新率，歌词帧率不变。
        if (isRenderMode(RenderMode::Low))
            return kTimerPausedMs;
        const UINT hz = displayRefreshHz_ ? displayRefreshHz_ : 60;
        return std::clamp(1000 / hz, kTimerMinMs, kTimerMs);
    }

    bool hasHighFrequencyAnimation() const {
        const bool songLayerActive = immersiveSongContentLayerActive();
        if (isAppBarView() && clientAnimations_ && dockPet_.animating())
            return true;
        if (isLyricTransitionInProgress() && !songLayerActive)
            return true;
        if (isSceneResizeActive())
            return true;
        if (songContentTransition_ && !songContentTransition_->compositorLayer)
            return true;
        if (taskbarDynamicBackgroundAnimating())
            return true;
        if (media.playing && scrollAnimating_ && !songLayerActive)
            return true;
        if (media.playing && clientAnimations_ && karaokeLine() && !songLayerActive)
            return true;
        if (media.playing && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        // 播放中的进度背景每帧都在推进
        if (media.playing && progressBackgroundActive())
            return true;
        if (media.playing && spectrumVisible_) {
            for (float v : spectrumBands_)
                if (v > 0.01f)
                    return true;
        }
        return false;
    }

    UINT desiredFrameMs() const {
        return hasHighFrequencyAnimation() ? activeFrameMs() : kTimerPausedMs;
    }

    // 帧定时器只在窗口可见时运行：隐藏后的固定间隔 tick（进度插值、滚动、
    // 快照读取）没有任何可见效果，可见性恢复由 SMTC 事件驱动（不依赖定时器）。
    void startFrameTimer() {
        if (!timerRunning_ && hwnd) {
            updateDisplayRefresh();
            const UINT initialMs = desiredFrameMs();
            SetTimer(hwnd, kTimerId, initialMs, nullptr);
            timerRunning_ = true;
            timerMs_ = initialMs;
        }
    }

    void stopFrameTimer() {
        if (timerRunning_ && hwnd) {
            KillTimer(hwnd, kTimerId);
            timerRunning_ = false;
            timerMs_ = 0;
        }
    }

    void setRenderMode(RenderMode mode) {
        if (mode == renderState_.mode())
            return;
        const bool leavingStopped =
            isStoppedMode() && mode != RenderMode::Stopped;
        const bool wasMinimal = isMinimalMode();
        renderState_.setMode(mode);
        if (leavingStopped) {
            if (isAppBarView()) {
                renderState_.setVisibilitySuppressed(false);
                setPlacementStatus(TaskbarPlacementStatus::Safe);
                startResourceMonitoring();
            } else {
                // 完全停止期间避让缓存可能已经过期。恢复显示前必须重新等待一次
                // 真实探测，不能按旧的“有空间”结果先显示一帧再隐藏。
                renderState_.setVisibilitySuppressed(true);
                probeReady_ = false;
                delete probeOut_.exchange(nullptr);
            }
            stopFrameTimer();
            stopPlacementTimer();
            reconcileWindowVisibility();
        }
        if (wasMinimal != isMinimalMode()) {
            // 极简关闭逐字绘制后，清掉当前帧的逐字状态；退出时由重新排版恢复几何缓存。
            requestInvalidation(RenderInvalidation::Text);
            karaokeSpans_.clear();
            karaokeGeometryLine_ = -1;
            karaokeGeometryLayout_ = nullptr;
            karaokeProgX_ = 0.0f;
            karaokeSmoothX_ = 0.0f;
            karaokeSmoothLine_ = -1;
            karaokeEnteringLine_ = false;
            karaokeSettled_ = true;
            karaokeTick_ = 0;
        }
        const bool popupAvailable = mediaPopupAvailable(isSessionVisible());
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setIdleContent(idle, popupAvailable);
        mediaPopup.setMedia(media, popupAvailable);
        mediaPopup.setPresentationMode(scene_, popupAvailable,
                                       hoverControlStyle_ == HoverControlStyle::Inline);
        mediaPopup.endPresentationUpdate();
        syncMediaPopupEnabled();
        if (isMinimalMode()) {
            // 极简仍保留歌词和方形封面，因此不释放主任务栏渲染器；只清掉附加背景链和媒体卡片。
            volumeHover_ = false;
            volumePopup_.hide();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            releaseCoverBackgroundResources();
            // 切歌转场只改变合成器根视觉；进入极简时立即恢复到静止位置。
            setSongTransitionPending(false);
            clearImmersiveSongContentTransition();
            renderer.resetRoot();
        }
        if (mode == RenderMode::Stopped) {
            // 完全停止：隐藏窗口、停帧定时器、释放 GPU 设备链。此后 SMTC 事件仍更新
            // 内存中的歌词/媒体数据，但 render() 因窗口隐藏直接早退，不占 GPU/CPU
            volumeHover_ = false;
            volumePopup_.hide();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            reconcileWindowVisibility();
            stopPlacementTimer();
            stopResourceMonitoring();
            releaseAll();
            return;
        }
        // 帧定时器运行中则立即按新档位调整间隔（正常/极简=跟随刷新率，低渲染=~30fps）
        if (timerRunning_) {
            const UINT wantMs = desiredFrameMs();
            if (wantMs != timerMs_) {
                SetTimer(hwnd, kTimerId, wantMs, nullptr);
                timerMs_ = wantMs;
            }
        }
        if (!isAppBarView() && renderState_.visibilitySuppressed() && !timerRunning_)
            startPlacementTimer();
        // 从完全停止恢复：按最近会话可见性立即还原窗口；设备链由 render() 惰性重建
        reconcileWindowVisibility();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    // 静止判定：所有动画源都停止且没有待处理的布局/资源变化时，跳过整帧重绘。
    // 跳过时屏幕上保持上一次 DirectComposition 提交的内容，不会闪烁或丢状态。
    bool needsFrameRender() const {
        const bool songLayerActive = immersiveSongContentLayerActive();
        if (renderState_.hasInvalidation())
            return true;
        if (isAppBarView() && clientAnimations_ && dockPet_.animating())
            return true;
        if (isLyricTransitionInProgress() && !songLayerActive)
            return true;
        if (isSceneResizeActive())
            return true;
        if (songContentTransition_ && !songContentTransition_->compositorLayer)
            return true;
        if (taskbarDynamicBackgroundAnimating())
            return true;
        if (scrollAnimating_ && !songLayerActive)
            return true;
        // 逐字平滑未收敛（暂停 seek 后）时渲染到收敛为止
        if (karaokeLine() && !karaokeSettled_ && !songLayerActive)
            return true;
        // 播放中的逐字推进和黑胶旋转每帧都在变
        if (media.playing && !songLayerActive &&
            (karaokeLine() || (clientAnimations_ && albumCoverVisible_ &&
                               albumCoverEffect_ == AlbumCoverEffect::Vinyl)))
            return true;
        // 进度背景随播放进度持续推进
        if (media.playing && progressBackgroundActive())
            return true;
        // 频谱柱等静音衰减到阈值以下才算静止
        if (spectrumVisible_) {
            for (float v : spectrumBands_)
                if (v > 0.01f)
                    return true;
        }
        return false;
    }

    void processPlacementProbe() {
        updateDisplayRefresh();
        const bool probeWasReady = probeReady_;
        bool changed = detectChanges();
        if (pickProbeResult())
            changed = true;
        if (changed)
            adjustPosition();
        if (!probeWasReady && probeReady_) {
            // 首次真实探测结果到达后再决定是否显示；否则创建期间可能先按
            // 空占用区显示一帧，随后被 UIA/窗口探测结果立即隐藏而产生闪烁。
            if (onPlacementStatusChanged_)
                onPlacementStatusChanged_(placementStatus_);
        }
        if (probeReady_)
            releaseVisibilitySuppression();
    }

    void onTimer() {
        if (tick)
            tick();
        finishImmersiveSongContentTransitionIfNeeded();
        // TrafficMonitor 可能在两次渲染之间调整自己的任务栏子窗口 Z 序；
        // 即使当前画面静止，也要及时把沉浸宿主恢复到最上层。
        ensureImmersiveZOrder();
        // 只有可见动画源跟随当前显示器刷新率（封顶 ~125fps）；静态播放降到 ~30fps，
        // 避免歌词没有动画时仍高频轮询和提交。状态事件（悬停、恢复播放、seek）走同步
        // render 路径，不依赖定时器，低档位下也不会延迟显示。
        const UINT wantMs = desiredFrameMs();
        if (timerRunning_ && wantMs != timerMs_) {
            SetTimer(hwnd, kTimerId, wantMs, nullptr);
            timerMs_ = wantMs;
        }
        frameNowMs_ = monotonicNowMs();
        dockPet_.tick(frameNowMs_);
        updateVinylRotation();
        if (refreshResourceSnapshot())
            requestFrame();
        // 任务栏位置/DPI/主题跟踪与避让探测结果拾取，放到慢速分支，不跟 60fps 走
        if (++slowTick_ >= kSlowTickInterval) {
            slowTick_ = 0;
            processPlacementProbe();
        }
        updateScroll();
        if (!isAppBarView() && refreshImmersiveClockText())
            requestFrame();
        const bool statusCycleCallbackHandled = statusTextCycleCallbackPending_;
        if (statusTextCycleCallbackPending_) {
            statusTextCycleCallbackPending_ = false;
            if (onStatusTextCycleCompleted_)
                onStatusTextCycleCompleted_();
        }
        // 静止场景跳过整帧重绘：动画源全部停止且无脏状态时，画面保持上一帧内容
        if (!statusCycleCallbackHandled && needsFrameRender())
            requestFrameAndFlush();
    }

    void onPlacementTimer() {
        if (isStoppedMode() || !hwnd)
            return;
        processPlacementProbe();
    }

    void trackMouseLeave() {
        if (trackingLeave_)
            return;
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        trackingLeave_ = true;
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            // 普通创建保持不可见；若正在等待首次避让探测，定时器会由
            // createWindow() 启动，但仍禁止提交可见窗口。
            return 0;
        case WM_TIMER:
            if (wp == kTimerId)
                onTimer();
            else if (wp == kPlacementTimerId)
                onPlacementTimer();
            else if (wp == kTaskbarAttachTimerId)
                retryTaskbarAttach();
            return 0;
        case WM_DISPLAYCHANGE:
            // 显示拓扑/分辨率变化（远程接入、虚拟屏开关等）是设备丢失的前置信号，
            // 记录下来便于和随后的 device lost / 重建日志对照。
            runtime_log::writef(L"[display] changed: %lux%lu %lubpp",
                                static_cast<unsigned long>(LOWORD(lp)),
                                static_cast<unsigned long>(HIWORD(lp)),
                                static_cast<unsigned long>(wp));
            updateDisplayRefresh();
            if (isAppBarView() && appBarRegistered_)
                updateAppBarPosition();
            else
                adjustPosition();
            // 不等待歌词、频谱等下一次内容变化才发现客户区尺寸已改变。立即按新
            // 任务栏几何重算安全区，并让 renderer.bind() 在下一帧同步交换链尺寸。
            requestInvalidation(toMask(RenderInvalidation::Layout) |
                                toMask(RenderInvalidation::Geometry));
            requestFrameAndFlush();
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                // SetWindowPos/Explorer/AppBar 都可能直接改变客户区；显式唤醒一帧，
                // 避免 DXGI_SCALING_STRETCH 暂时拉伸旧帧直到下一句歌词出现。
                requestInvalidation(RenderInvalidation::Geometry);
                requestFrame();
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_DPICHANGED:
            if (isAppBarView() && appBarRegistered_) {
                dpi_ = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
                renderer.setDpi(dpi_);
                requestInvalidation(toMask(RenderInvalidation::Layout) |
                                    toMask(RenderInvalidation::Text) |
                                    toMask(RenderInvalidation::Cover));
                updateAppBarPosition();
                requestFrameAndFlush();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ACTIVATE:
            if (appBarRegistered_) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                data.lParam = LOWORD(wp) != WA_INACTIVE;
                SHAppBarMessage(ABM_ACTIVATE, &data);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_WINDOWPOSCHANGED:
            if (appBarRegistered_) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            // 整个窗口区域都视为客户区，让透明背景也能接收鼠标消息
            return HTCLIENT;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && lyricDragging_) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_MOUSEMOVE: {
            POINT cursor{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &cursor);
            if (dragPress_) {
                dragCursorScreen_ = cursor;
                if (!lyricDragging_) {
                    const int thresholdX = std::max(GetSystemMetrics(SM_CXDRAG),
                                                    (int)std::lround(kLyricDragThresholdDip * scale()));
                    const int thresholdY = std::max(GetSystemMetrics(SM_CYDRAG),
                                                    (int)std::lround(kLyricDragThresholdDip * scale()));
                    if (std::abs(cursor.x - dragPressScreen_.x) >= thresholdX ||
                        std::abs(cursor.y - dragPressScreen_.y) >= thresholdY)
                        beginLyricDrag();
                }
                if (lyricDragging_ && updateDragPreviewPlacement())
                    requestFrameAndFlush();
                return 0;
            }
            bool wasOver = mouseOver_;
            mouseOver_ = true;
            if (mediaPopupEnabledForScene())
                mediaPopup.onAnchorEnter();
            if (!wasOver)
                requestFrameAndFlush();
            trackMouseLeave();
            const float mouseX = static_cast<float>(GET_X_LPARAM(lp));
            const float mouseY = static_cast<float>(GET_Y_LPARAM(lp));
            const int immersiveHover = isExpandedView()
                                           ? hitImmersiveControl(mouseX, mouseY)
                                           : -1;
            if (immersiveHover != immersiveControlHover_) {
                immersiveControlHover_ = immersiveHover;
                requestFrameAndFlush();
            }
            // 内嵌控件的音量按钮：悬停弹出音量滑块浮窗
            const bool volHover = isExpandedView()
                                      ? immersiveHover == kImmersiveControlVolume
                                      : hitVolumeButton(mouseX, mouseY);
            if (volHover != volumeHover_) {
                volumeHover_ = volHover;
                if (!volHover)
                    volumePopup_.onAnchorLeave();
            }
            if (volHover && appVolume_.available) {
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect(), isVerticalTaskbar(),
                                      taskbarEdge_ == ABE_LEFT);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (dragPress_ || lyricDragging_)
                return 0;
            mouseOver_ = false;
            trackingLeave_ = false;
            volumeHover_ = false;
            immersiveControlHover_ = -1;
            volumePopup_.onAnchorLeave();
            mediaPopup.onAnchorLeave();
            requestFrameAndFlush();
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮消息使用屏幕坐标；音量图标上滚动直接调整应用音量（每格 ±2）
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const bool volumeHit = isExpandedView()
                                       ? hitImmersiveControl(static_cast<float>(pt.x),
                                                             static_cast<float>(pt.y)) ==
                                             kImmersiveControlVolume
                                       : hitVolumeButton(static_cast<float>(pt.x),
                                                         static_cast<float>(pt.y));
            if (appVolume_.available && onAppVolume && volumeHit) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0)
                    onAppVolume(std::clamp(appVolume_.percent + steps * 2, 0, 100));
                return 0;
            }
            if (dockResourceExtrasVisible() &&
                hitDockResourceArea(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0)
                    advanceDockResourcePage(steps);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // 点击与拖动从同一次按下并行识别：未越过系统拖动阈值仍按原按钮/卡片
            // 点击处理，越过阈值后才取消点击并进入位置拖拽。
            if (isExpandedView()) {
                immersiveControlPressed_ =
                    hitImmersiveControl(static_cast<float>(GET_X_LPARAM(lp)),
                                        static_cast<float>(GET_Y_LPARAM(lp)));
                if (immersiveControlPressed_ >= 0)
                    SetCapture(hwnd);
                return 0;
            }
            dragPress_ = true;
            GetCursorPos(&dragPressScreen_);
            dragCursorScreen_ = dragPressScreen_;
            dragCandidateMode_ = positionMode_;
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (isExpandedView()) {
                const int pressed = immersiveControlPressed_;
                const int released =
                    hitImmersiveControl(static_cast<float>(GET_X_LPARAM(lp)),
                                        static_cast<float>(GET_Y_LPARAM(lp)));
                immersiveControlPressed_ = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                // 沉浸层可能在一次任务栏点击的按下与松开之间出现。只有本窗口
                // 确实收到同一控件的按下事件，才把随后到达的松开解释为点击。
                if (pressed >= 0 && pressed == released)
                    activateImmersiveControl(pressed);
                return 0;
            }
            const bool hadPress = dragPress_ || lyricDragging_;
            const bool wasDragging = lyricDragging_;
            if (hadPress)
                finishLyricDrag(true);
            if (wasDragging)
                return 0;
            if (hadPress) {
                RECT client{};
                POINT releasePoint{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                GetClientRect(hwnd, &client);
                if (!PtInRect(&client, releasePoint))
                    return 0;
            }
            float mx = static_cast<float>(GET_X_LPARAM(lp));
            float my = static_cast<float>(GET_Y_LPARAM(lp));
            int btn = hitButton(mx, my);
            if (btn >= 0 && onControl)
                onControl(static_cast<MediaControl>(btn));
            // 卡片的当前页面自己判断是否为点击展开；这样无播放时的每日一言卡片
            // 可以独立于媒体控件样式使用点击展开。
            else if (!isMinimalMode() && !isExpandedView())
                mediaPopup.onAnchorClick();
            return 0;
        }
        case WM_CAPTURECHANGED:
            immersiveControlPressed_ = -1;
            if (dragPress_ || lyricDragging_)
                finishLyricDrag(false);
            return 0;
        case WM_CANCELMODE: {
            const bool hadImmersivePress = immersiveControlPressed_ >= 0;
            immersiveControlPressed_ = -1;
            if (hadImmersivePress && GetCapture() == hwnd)
                ReleaseCapture();
            if (dragPress_ || lyricDragging_)
                finishLyricDrag(false);
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (!contextMenuEnabled_ || !onContextMenu)
                return 0;

            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }

            openTaskbarMenu(pt);
            return 0;
        }
        case kAppBarCallbackMessage:
            if (!appBarRegistered_)
                return 0;
            switch (wp) {
            case ABN_POSCHANGED:
                updateAppBarPosition();
                break;
            case ABN_FULLSCREENAPP:
                appBarFullscreenOpen_ = lp != FALSE;
                runtime_log::writef(L"[dock] fullscreen %s; z-order=%s",
                                    appBarFullscreenOpen_ ? L"opened" : L"closed",
                                    appBarFullscreenOpen_ ? L"bottom" : L"topmost");
                updateAppBarZOrder();
                break;
            case ABN_STATECHANGE:
                updateAppBarZOrder();
                break;
            case ABN_WINDOWARRANGE:
                if (lp)
                    ShowWindow(hwnd, SW_HIDE);
                else if (shouldShowWindow())
                    ShowWindow(hwnd, SW_SHOWNA);
                break;
            default:
                break;
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                quitting = true;
                if (hwnd)
                    DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            quitting = true;
            if (hwnd)
                DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            runtime_log::writef(L"[taskbar] WM_DESTROY (visible=%d)",
                                isWindowVisible() ? 1 : 0);
            dockBackdrop_.reset();
            renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
            unregisterAppBar();
            KillTimer(hwnd, kTaskbarAttachTimerId);
            taskbarEmbedded_ = false;
            stopFrameTimer();
            stopPlacementTimer();
            stopProbe();
            stopResourceMonitoring();
            dragPress_ = false;
            lyricDragging_ = false;
            volumePopup_.destroy();
            mediaPopup.destroy();
            releaseAll();
            hwnd = nullptr;
            if (quitting)
                PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(cs->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
};

TaskbarHost::TaskbarHost() : impl_(std::make_unique<Impl>()) {}

TaskbarHost::~TaskbarHost() {
    if (impl_ && impl_->hwnd)
        DestroyWindow(impl_->hwnd);
}

bool TaskbarHost::create(HINSTANCE inst) {
    return impl_->createWindow(inst);
}

HWND TaskbarHost::hwnd() const {
    return impl_ ? impl_->hwnd : nullptr;
}

void TaskbarHost::setTickCallback(std::function<void()> cb) {
    impl_->tick = std::move(cb);
}

void TaskbarHost::applyPresentationFrame(const PresentationFrame& frame) {
    impl_->applyPresentationFrame(frame);
}

void TaskbarHost::applyPlaybackPatch(const PlaybackPatch& patch) {
    impl_->applyPlaybackPatch(patch);
}

void TaskbarHost::applySpectrumPatch(const SpectrumPatch& patch) {
    impl_->applySpectrumPatch(patch);
}

void TaskbarHost::setMediaInfo(const OverlayMediaInfo& info) {
    // SMTC 的播放、时间线和属性事件可能连续到达；没有可见状态变化时不再
    // 额外提交一次整个分层窗口，下一帧定时器会按当前进度正常绘制。
    const bool popupAvailable = impl_->mediaPopupAvailable(impl_->isSessionVisible());
    impl_->mediaPopup.beginPresentationUpdate();
    impl_->mediaPopup.setIdleContent(impl_->idle, popupAvailable);
    impl_->mediaPopup.setMedia(info, popupAvailable);
    impl_->mediaPopup.setPresentationMode(
        impl_->scene_, popupAvailable,
        impl_->hoverControlStyle_ == HoverControlStyle::Inline);
    impl_->mediaPopup.setProgress(impl_->positionMs_);
    impl_->mediaPopup.endPresentationUpdate();
    if (impl_->updateMediaInfo(info)) {
        impl_->requestFrame();
        impl_->flushRenderRequest();
    }
}

void TaskbarHost::setControlCallback(std::function<void(MediaControl)> cb) {
    impl_->onControl = cb;
    impl_->mediaPopup.setControlCallback(std::move(cb));
}

void TaskbarHost::setImmersiveExitCallback(std::function<void()> cb) {
    impl_->onImmersiveExit = std::move(cb);
}

void TaskbarHost::setAppVolume(const AppVolumeState& state) {
    const bool changed = state.available != impl_->appVolume_.available ||
                         state.percent != impl_->appVolume_.percent ||
                         state.muted != impl_->appVolume_.muted;
    impl_->appVolume_ = state;
    impl_->volumePopup_.setVolume(state.percent, state.muted, state.available);
    impl_->mediaPopup.setAppVolume(state);
    if (!state.available)
        impl_->volumePopup_.hide();
    if (changed) {
        impl_->requestFrame();
        impl_->flushRenderRequest();
    }
}

void TaskbarHost::setAppVolumeCallback(std::function<void(int)> cb) {
    impl_->onAppVolume = cb;
    impl_->volumePopup_.setCallback(cb);
    impl_->mediaPopup.setAppVolumeCallback(std::move(cb));
}

void TaskbarHost::setSourceOpenCallback(std::function<void(const std::wstring&)> cb) {
    impl_->mediaPopup.setSourceOpenCallback(std::move(cb));
}

void TaskbarHost::setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb) {
    impl_->mediaPopup.setIdleAppOpenCallback(std::move(cb));
}

void TaskbarHost::setIdleTaskOpenCallback(std::function<void(const IdleTaskInfo&)> cb) {
    impl_->mediaPopup.setIdleTaskOpenCallback(std::move(cb));
}

void TaskbarHost::setIdleTaskCompleteCallback(
    std::function<void(const IdleTaskInfo&)> cb) {
    impl_->mediaPopup.setIdleTaskCompleteCallback(std::move(cb));
}

void TaskbarHost::setMediaPopupOpenedCallback(std::function<void()> cb) {
    impl_->mediaPopup.setPanelOpenedCallback(std::move(cb));
}

void TaskbarHost::setContextMenuCallback(std::function<void(POINT)> cb) {
    impl_->onContextMenu = std::move(cb);
}

void TaskbarHost::setImmersiveMenuCallback(std::function<void(POINT)> cb) {
    impl_->onImmersiveMenu = std::move(cb);
}

void TaskbarHost::setAppCollectionCallback(std::function<void(POINT)> cb) {
    impl_->onAppCollection = std::move(cb);
}

void TaskbarHost::setDockQuickAppsCallback(std::function<void(POINT)> cb) {
    impl_->onDockQuickApps = std::move(cb);
}

void TaskbarHost::setPositionModeChangedCallback(std::function<void(int)> cb) {
    impl_->onPositionModeChanged = std::move(cb);
}

void TaskbarHost::setStatusTextCycleCompletedCallback(std::function<void()> cb) {
    impl_->onStatusTextCycleCompleted_ = std::move(cb);
}

void TaskbarHost::setPlacementStatusCallback(
    std::function<void(TaskbarPlacementStatus)> cb) {
    impl_->onPlacementStatusChanged_ = std::move(cb);
}

void TaskbarHost::setAllowOverlap(bool on) {
    impl_->allowOverlap_ = on;
    if (impl_->reconcileWindowVisibility() && impl_->isWindowVisible())
        impl_->requestFrameAndFlush();
}

void TaskbarHost::setVisibilitySuppressed(bool on) {
    impl_->setVisibilitySuppressed(on);
}

TaskbarPlacementStatus TaskbarHost::refreshPlacement() {
    return impl_->refreshPlacement();
}

TaskbarPlacementStatus TaskbarHost::placementStatus() const {
    return impl_->placementStatus_;
}

bool TaskbarHost::isDisplayed() const {
    return impl_ && impl_->isWindowVisible() && impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

const std::vector<LyricLine>& TaskbarHost::lyrics() const {
    return impl_->lines;
}

void TaskbarHost::show() {
    impl_->show();
}

void TaskbarHost::hide() {
    impl_->hide();
}

void TaskbarHost::setLyrics(const std::vector<LyricLine>& lines) {
    impl_->lines = lines;
    impl_->scene_ = lines.empty() ? DisplayScene::Message : DisplayScene::Lyrics;
    impl_->currentLine = -1;
    impl_->resetLyricTransition();
    if (impl_->nextLyricLayout_) {
        impl_->nextLyricLayout_->Release();
        impl_->nextLyricLayout_ = nullptr;
    }
    impl_->nextLyricWidth_ = 0.0f;
    impl_->nextLyricHeight_ = 0.0f;
    impl_->requestInvalidation(RenderInvalidation::Text);
    if (!lines.empty())
        impl_->statusText.clear();
    impl_->requestFrame();
    impl_->flushRenderRequest();
}

void TaskbarHost::setCurrentLine(int index) {
    if (index != impl_->currentLine)
        impl_->onLyricLineTargetChanged(index, impl_->positionMs_, impl_->frameRevision_, true);
}

void TaskbarHost::setPosition(int64_t positionMs) {
    impl_->positionMs_ = positionMs;
}

void TaskbarHost::setStatusText(const std::wstring& text) {
    impl_->statusText = text;
    impl_->statusTextOneShot_ = false;
    impl_->statusTextOneShotStartMs_ = 0;
    impl_->statusTextOneShotRounds_ = 0;
    impl_->statusTextCycleCallbackPending_ = false;
    if (!text.empty() && impl_->lines.empty())
        impl_->scene_ = DisplayScene::Message;
    else if (text.empty() && !impl_->lines.empty())
        impl_->scene_ = DisplayScene::Lyrics;
    impl_->requestInvalidation(RenderInvalidation::Text);
    impl_->requestFrame();
    impl_->flushRenderRequest();
}

bool TaskbarHost::isTaskbar() const {
    return true;
}

bool TaskbarHost::isVerticalTaskbar() const {
    return impl_ && impl_->isVerticalTaskbar();
}

int TaskbarHost::currentLine() const {
    return impl_->currentLine;
}

const std::wstring& TaskbarHost::statusText() const {
    return impl_->statusText;
}

void TaskbarHost::changeFont(float delta) {
    impl_->changeFont(delta);
}

void TaskbarHost::setFont(const std::wstring& family, float size, LyricFontStyle style) {
    impl_->setFont(family, size, style);
}

void TaskbarHost::setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) {
    impl_->setFontColors(played, unplayed, unplayedAlphaPct);
}

void TaskbarHost::setFontGlow(bool on) {
    impl_->setFontGlow(on);
}

void TaskbarHost::setFontOutline(bool on) {
    impl_->setFontOutline(on);
}

void TaskbarHost::setFontGlowColors(COLORREF glow, COLORREF outline) {
    impl_->setFontGlowColors(glow, outline);
}

void TaskbarHost::setSecondaryLyricMode(bool translation, bool romanization) {
    impl_->setSecondaryLyricMode(translation, romanization);
}

void TaskbarHost::setDoubleLineLyrics(bool on) {
    impl_->setDoubleLineLyrics(on);
}

void TaskbarHost::setLyricAlignment(LyricAlignment alignment) {
    impl_->setLyricAlignment(alignment);
}

void TaskbarHost::setIdleQuoteAlignment(LyricAlignment alignment) {
    impl_->setIdleQuoteAlignment(alignment);
}

void TaskbarHost::setIdleQuoteBackground(IdleQuoteBackground background) {
    impl_->setIdleQuoteBackground(background);
}

void TaskbarHost::setIdleQuoteBackgroundScope(IdleQuoteBackgroundScope scope) {
    impl_->setIdleQuoteBackgroundScope(scope);
}

void TaskbarHost::setControlsOnHover(bool on) {
    impl_->setControlsOnHover(on);
}

void TaskbarHost::setContextMenuEnabled(bool on) {
    impl_->contextMenuEnabled_ = on;
}

void TaskbarHost::setHoverControlStyle(HoverControlStyle style) {
    impl_->setHoverControlStyle(style);
}

void TaskbarHost::setFloatingCardTrigger(MediaPopupTrigger trigger) {
    impl_->setFloatingCardTrigger(trigger);
}

void TaskbarHost::setFloatingCardBackground(MediaPopupBackground mode) {
    impl_->setFloatingCardBackground(mode);
}

void TaskbarHost::setFloatingCardBackgroundColor(COLORREF color, bool customized) {
    impl_->setFloatingCardBackgroundColor(color, customized);
}

void TaskbarHost::setFloatingCardFollowAlbum(bool on) {
    impl_->setFloatingCardFollowAlbum(on);
}

void TaskbarHost::setFloatingCardAutoTextContrast(bool on) {
    impl_->setFloatingCardAutoTextContrast(on);
}

void TaskbarHost::refreshTheme() {
    impl_->refreshTheme();
}

void TaskbarHost::setSongInfoVisible(bool on) {
    impl_->setSongInfoVisible(on);
}

void TaskbarHost::setAlbumCoverVisible(bool on) {
    impl_->setAlbumCoverVisible(on);
}

void TaskbarHost::setPlatformIconVisible(bool on) {
    impl_->setPlatformIconVisible(on);
}

void TaskbarHost::setAlbumCoverEffect(AlbumCoverEffect effect) {
    impl_->setAlbumCoverEffect(effect);
}

void TaskbarHost::setRenderMode(RenderMode mode) {
    impl_->setRenderMode(mode);
}

void TaskbarHost::setSpectrumVisible(bool on) {
    impl_->setSpectrumVisible(on);
}

void TaskbarHost::setSpectrumStyle(SpectrumStyle style) {
    impl_->setSpectrumStyle(style);
}

void TaskbarHost::setSpectrumColor(COLORREF color, bool customized, bool followAlbum) {
    impl_->setSpectrumColor(color, customized, followAlbum);
}

void TaskbarHost::setSpectrumAlbumColor(COLORREF color, bool available) {
    impl_->setSpectrumAlbumColor(color, available);
}

void TaskbarHost::setSpectrumGradient(bool on) {
    impl_->setSpectrumGradient(on);
}

void TaskbarHost::setSpectrumBackground(bool on) {
    impl_->setSpectrumBackground(on);
}

void TaskbarHost::setSpectrumOpacity(int percent) {
    impl_->setSpectrumOpacity(percent);
}

void TaskbarHost::setProgressBackground(bool on) {
    impl_->setProgressBackground(on);
}

void TaskbarHost::setProgressBackgroundOpacity(int percent) {
    impl_->setProgressBackgroundOpacity(percent);
}

void TaskbarHost::setBackground(TaskbarBackground mode) {
    impl_->setBackground(mode);
}

void TaskbarHost::setCoverBackgroundOpacity(int percent) {
    impl_->setCoverBackgroundOpacity(percent);
}

void TaskbarHost::setViewMode(TaskbarViewMode mode) {
    impl_->setViewMode(mode);
}

void TaskbarHost::setAppBarEdge(AppBarEdge edge) {
    impl_->setAppBarEdge(edge);
}

void TaskbarHost::setImmersiveBackgroundBlur(int percent) {
    impl_->setImmersiveBackgroundBlur(percent);
}

void TaskbarHost::setDockBackgroundBlur(int percent) {
    impl_->setDockBackgroundBlur(percent);
}

void TaskbarHost::setImmersiveBackgroundAdjustment(int mode) {
    impl_->setImmersiveBackgroundAdjustment(mode);
}

void TaskbarHost::setDockBackgroundAdjustment(int mode) {
    impl_->setDockBackgroundAdjustment(mode);
}

bool TaskbarHost::backgroundBlurAvailable() const {
    return impl_->backgroundBlurAvailable();
}

void TaskbarHost::setDockResourceVisibility(const DockResourceVisibility& visibility) {
    impl_->setDockResourceVisibility(visibility);
}

void TaskbarHost::setSpectrumBands(const std::array<float, kSpectrumBands>& bands) {
    impl_->setSpectrumBands(bands);
}

void TaskbarHost::setPositionMode(int mode) {
    impl_->setPositionMode(mode);
}

void TaskbarHost::onTaskbarCreated() {
    impl_->onTaskbarCreated();
}
