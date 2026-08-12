#include "taskbar_host.h"
#include "lyric_renderer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId = 2;
constexpr UINT kTimerMs = 16;         // ~60fps 滚动渲染（配合 timeBeginPeriod(1) 保证触发精度）
constexpr int kSlowTickInterval = 15; // 慢速分支（任务栏位置跟踪等）每 15 帧一次，约 250ms
constexpr wchar_t kWndClassName[] = L"QQMusicLyricTaskbar";
constexpr wchar_t kFontFamily[] = L"Microsoft YaHei UI";

constexpr float kMinWidthDip = 160.0f;
constexpr float kMaxWidthDip = 280.0f;
constexpr float kLeftRatio = 0.38f;
constexpr float kCoverPadding = 4.0f;
constexpr float kTextPadding = 8.0f;
constexpr float kCornerRadius = 8.0f;
constexpr float kInfoScrollSpeed = 10.0f;  // 歌名/歌手滚动速度（DIP/s）
constexpr float kLyricScrollSpeed = 15.0f; // 歌词滚动速度（DIP/s）
constexpr float kMinFont = 9.0f;
constexpr float kMaxFont = 18.0f;
constexpr float kBaseFontSize = 12.0f;

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

bool isSystemLightTheme() {
    // 1 = 浅色，0 = 深色（默认）
    return regDword(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    L"SystemUsesLightTheme", 0) != 0;
}

} // namespace

struct TaskbarHost::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool visible = false;

    // 任务栏句柄与子部件
    HWND taskbar_ = nullptr;
    HWND notify_ = nullptr;
    HWND start_ = nullptr;
    RECT rcTaskbar_{};   // 任务栏屏幕坐标（缓存）
    RECT rcNotify_{};    // 通知区屏幕坐标（缓存）
    RECT rcStart_{};     // 开始按钮屏幕坐标（缓存，找不到时为空）
    RECT rcTrafficMonitor_{}; // TrafficMonitor 屏幕坐标（缓存，未运行时为 empty）
    IUIAutomation* uia_ = nullptr; // UI Automation 客户端（惰性创建）
    // 任务栏 XAML 按钮（开始/搜索/任务视图/小组件/应用图标）包围矩形缓存（屏幕坐标）
    std::vector<RECT> uiaButtons_;
    UINT dpi_ = 96;
    bool centerAlign_ = true;
    bool lightTheme_ = false;
    int positionMode_ = 0; // 0 = 通知区域左侧；1 = 任务栏最左侧

    // 歌词状态
    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    int currentLine = -1;
    int64_t positionMs_ = 0; // 播放进度（每帧更新），驱动逐字高亮

    // 字体
    float fontSize_ = kBaseFontSize;
    std::wstring fontFamily_ = kFontFamily;

    // 媒体信息
    OverlayMediaInfo media;
    ID2D1Bitmap* coverBmp = nullptr;
    bool coverDirty = true;

    // 交互
    std::function<void()> tick;
    std::function<void(MediaControl)> onControl;
    bool mouseOver_ = false;
    bool trackingLeave_ = false;
    bool quitting = false;

    // 逐字填充进度（布局像素坐标）：目标值 + 平滑值。
    // SMTC 进度是锚点插值的，每次锚点校正都会阶跃一次；平滑值按当前字时长
    // 决定的时间常数指数趋近目标，消除阶跃闪烁且同步误差有界
    float karaokeProgX_ = 0.0f;      // 本帧实际使用的填充进度（平滑后）
    float karaokeSmoothX_ = 0.0f;    // 平滑状态
    int karaokeSmoothLine_ = -1;     // 平滑状态所属行号
    ULONGLONG karaokeTick_ = 0;      // 上次平滑步进的时刻

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
    std::wstring lastTitle_;
    std::wstring lastArtist_;
    std::wstring lastLyric_;
    ULONGLONG lastTickMs_ = 0;
    int slowTick_ = 0; // 慢速分支计数器

    // 渲染
    LyricRenderer renderer;
    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtArtist_ = nullptr;
    IDWriteTextFormat* fmtLyric_ = nullptr;
    IDWriteTextLayout* titleLayout_ = nullptr;
    IDWriteTextLayout* artistLayout_ = nullptr;
    IDWriteTextLayout* lyricLayout_ = nullptr;
    ID2D1SolidColorBrush* brushBg_ = nullptr;
    ID2D1SolidColorBrush* brushText_ = nullptr;
    ID2D1SolidColorBrush* brushDim_ = nullptr;
    ID2D1SolidColorBrush* brushBtn_ = nullptr;
    ID2D1SolidColorBrush* brushBtnDisabled_ = nullptr;
    ID2D1SolidColorBrush* brushLyric_ = nullptr;        // 已播放歌词颜色（用户可配）
    ID2D1SolidColorBrush* brushLyricDim_ = nullptr;     // 逐字歌词未唱部分（独立颜色+透明度）
    ID2D1SolidColorBrush* brushLyricGlow_ = nullptr;    // 歌词光晕（主色低透明度）
    ID2D1SolidColorBrush* brushLyricOutline_ = nullptr; // 歌词深色描边
    COLORREF lyricColor_ = RGB(49, 194, 124);           // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124);   // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                    // 逐字未播放透明度（%）
    COLORREF lyricGlowColor_ = RGB(49, 194, 124);       // 光晕颜色
    COLORREF lyricOutlineColor_ = RGB(0, 0, 0);         // 描边颜色
    bool lyricGlow_ = false;                            // 光晕开关
    bool lyricOutline_ = false;                         // 描边开关
    ID2D1RoundedRectangleGeometry* coverClip_ = nullptr;
    ID2D1Layer* coverLayer_ = nullptr;
    ID2D1PathGeometry* icoPlay_ = nullptr;   // 播放（右向三角）
    ID2D1PathGeometry* icoPrev_ = nullptr;   // 上一首（左向三角 + 左侧竖条）
    ID2D1PathGeometry* icoNext_ = nullptr;   // 下一首（右向三角 + 右侧竖条）
    bool textDirty_ = true;
    bool geomDirty_ = true;
    bool layoutDirty_ = true;
    int lastPxW_ = 0;
    int lastPxH_ = 0;

    float scale() const { return static_cast<float>(dpi_) / 96.0f; }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    // ---------- 窗口创建与定位 ----------

    bool findTaskbar() {
        taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!taskbar_)
            return false;
        notify_ = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr);
        start_ = FindWindowExW(taskbar_, nullptr, L"Start", nullptr);
        dpi_ = GetDpiForWindow(taskbar_);
        centerAlign_ = isTaskbarCenterAlign();
        lightTheme_ = isSystemLightTheme();
        updateRects();
        // TrafficMonitor 矩形原本只在 detectChanges 里刷新，首次定位前要先初始化
        if (HWND tm = findTrafficMonitor())
            GetWindowRect(tm, &rcTrafficMonitor_);
        queryTaskbarButtons(uiaButtons_);
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

    bool createWindow(HINSTANCE inst) {
        this->inst = inst;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWndClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        if (!findTaskbar())
            return false;

        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyricTaskbar", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, inst, this);
        if (!h)
            return false;

        // 嵌入任务栏；失败则仍可作为独立分层窗口存在
        if (!SetParent(h, taskbar_)) {
            std::wprintf(L"[taskbar] SetParent failed, fallback to popup\n");
        }

        hwnd = h;
        adjustPosition();
        return true;
    }

    HWND findTrafficMonitor() const {
        if (!taskbar_)
            return nullptr;
        HWND child = nullptr;
        while ((child = FindWindowExW(taskbar_, child, nullptr, nullptr)) != nullptr) {
            wchar_t text[256] = {};
            GetWindowTextW(child, text, 256);
            if (wcscmp(text, L"TrafficMonitorTaskbarWindow") == 0)
                return child;
        }
        return nullptr;
    }

    bool ensureUia() {
        if (uia_)
            return true;
        // 本线程可能未初始化 COM（SMTC 用的是 WinRT 单元）；已初始化时返回
        // RPC_E_CHANGED_MODE，不影响后续使用，故不检查返回值，也不在退出时 Uninitialize
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        return SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&uia_)));
    }

    // 通过 UI Automation 取任务栏 XAML 部件（开始/搜索/任务视图/小组件/固定与运行中的
    // 应用图标）的屏幕包围矩形。这些按钮是 XAML 元素而非窗口，HWND 枚举看不到，
    // 但 UIA 的 TaskbarFrame 子树完整暴露；小组件等后续新增的按钮同样作为其子元素出现
    void queryTaskbarButtons(std::vector<RECT>& out) {
        out.clear();
        if (!taskbar_ || !ensureUia())
            return;
        IUIAutomationElement* tb = nullptr;
        if (FAILED(uia_->ElementFromHandle(taskbar_, &tb)) || !tb)
            return;
        VARIANT aid{};
        aid.vt = VT_BSTR;
        aid.bstrVal = SysAllocString(L"TaskbarFrame");
        IUIAutomationCondition* cond = nullptr;
        uia_->CreatePropertyCondition(UIA_AutomationIdPropertyId, aid, &cond);
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
        uia_->CreateTrueCondition(&trueCond);
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
            if (!offscreen && rc.right > rc.left)
                out.push_back(rc);
        }
        arr->Release();
    }

    // 任务栏上的占用区间 [left, right)（屏幕坐标，已合并）：UIA 按钮 + 通知区 +
    // TrafficMonitor。开始按钮的 HWND 矩形与 UIA StartButton 重复，合并后无害，
    // 留着可在 UIA 不可用时兜底
    std::vector<std::pair<int, int>> occupiedIntervals() const {
        std::vector<std::pair<int, int>> v;
        auto add = [&v](const RECT& r) {
            if (r.right > r.left)
                v.emplace_back(r.left, r.right);
        };
        for (const RECT& r : uiaButtons_)
            add(r);
        if (notify_)
            add(rcNotify_);
        if (start_)
            add(rcStart_);
        add(rcTrafficMonitor_);
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

    void adjustPosition() {
        if (!hwnd || !taskbar_)
            return;

        updateRects();

        int taskbarH = rcTaskbar_.bottom - rcTaskbar_.top;
        int marginY = std::max(2, (int)std::lround(2.0f * scale()));
        int pxH = taskbarH - marginY * 2;
        if (pxH < 16)
            pxH = taskbarH;

        int gap = std::max(4, (int)std::lround(4.0f * scale()));
        int minW = (int)std::lround(kMinWidthDip * scale());
        int maxW = (int)std::lround(kMaxWidthDip * scale());

        // 空闲区间 = 任务栏减去占用区间
        struct Span {
            int l, r;
        };
        std::vector<Span> spans;
        int cursor = rcTaskbar_.left;
        for (const auto& o : occupiedIntervals()) {
            if (o.first > cursor)
                spans.push_back({cursor, o.first});
            cursor = std::max(cursor, o.second);
        }
        if (cursor < rcTaskbar_.right)
            spans.push_back({cursor, rcTaskbar_.right});
        if (spans.empty())
            spans.push_back({rcTaskbar_.left, rcTaskbar_.right});

        auto usableW = [gap](const Span& s) { return s.r - s.l - gap * 2; };
        // 原位优先：模式 0 锚定最右空闲区（通知区/TrafficMonitor 左侧），
        // 模式 1 锚定最左空闲区（居中任务栏时在开始按钮左侧，左对齐时在应用图标右侧）
        const Span& pref = positionMode_ == 1 ? spans.front() : spans.back();

        int pxW = 0;
        int x = 0;
        auto place = [&](const Span& s, int w) {
            pxW = w;
            x = positionMode_ == 1 ? s.l + gap : s.r - gap - w;
        };
        if (usableW(pref) >= minW) {
            place(pref, std::min(usableW(pref), maxW)); // 原位优先：被挤压先收缩宽度
        } else {
            // 压到最小宽度仍放不下：换到容得下的最大空闲区
            const Span* best = nullptr;
            for (const auto& s : spans) {
                if (usableW(s) >= minW && (!best || s.r - s.l > best->r - best->l))
                    best = &s;
            }
            if (best) {
                place(*best, std::min(usableW(*best), maxW));
            } else {
                // 全任务栏都放不下：维持最小宽度锚在原位（与旧行为一致，允许重叠）
                place(pref, minW);
            }
        }
        if (x < rcTaskbar_.left)
            x = rcTaskbar_.left;

        int y = rcTaskbar_.top + marginY;

        // 转成 Shell_TrayWnd 客户区坐标
        POINT pt{x, y};
        ScreenToClient(taskbar_, &pt);

        // 把窗口提到任务栏子窗口最前面，避免被其他任务栏子窗口盖住
        SetWindowPos(hwnd, HWND_TOP, pt.x, pt.y, pxW, pxH,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
        bool light = isSystemLightTheme();
        bool themeChanged = light != lightTheme_;

        RECT rcTm{};
        HWND tm = findTrafficMonitor();
        if (tm)
            GetWindowRect(tm, &rcTm);

        RECT rcStart{};
        if (start_)
            GetWindowRect(start_, &rcStart);

        // 任务栏 XAML 按钮（应用图标随窗口开关增减、居中任务栏随图标数移动）
        std::vector<RECT> btns;
        queryTaskbarButtons(btns);
        bool buttonsChanged = btns.size() != uiaButtons_.size();
        if (!buttonsChanged) {
            for (size_t i = 0; i < btns.size(); ++i) {
                if (!EqualRect(&btns[i], &uiaButtons_[i])) {
                    buttonsChanged = true;
                    break;
                }
            }
        }

        bool changed = dpi != dpi_ || center != centerAlign_ || themeChanged ||
                       buttonsChanged ||
                       !EqualRect(&rcTaskbar, &rcTaskbar_) ||
                       !EqualRect(&rcNotify, &rcNotify_) ||
                       !EqualRect(&rcStart, &rcStart_) ||
                       !EqualRect(&rcTm, &rcTrafficMonitor_);
        if (changed) {
            dpi_ = dpi;
            centerAlign_ = center;
            rcTaskbar_ = rcTaskbar;
            rcNotify_ = rcNotify;
            rcStart_ = rcStart;
            rcTrafficMonitor_ = rcTm;
            uiaButtons_ = std::move(btns);
            renderer.setDpi(dpi_);
            layoutDirty_ = true;
            if (themeChanged) {
                lightTheme_ = light;
                discardDeviceResources();
            }
        }
        return changed;
    }

    // ---------- 资源 ----------

    void createDeviceResources() {
        if (brushBg_)
            return;
        renderer.initialize();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);

        // 根据任务栏深浅主题选择配色，贴近 Windows 11 原生媒体控件
        // 背景使用极低的 alpha，视觉上透明但保证分层窗口命中测试覆盖整个区域
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.01f), &brushBg_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.95f), &brushText_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.30f, 0.30f, 0.75f), &brushDim_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.90f), &brushBtn_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.30f), &brushBtnDisabled_);
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.01f), &brushBg_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.95f), &brushText_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.65f), &brushDim_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.90f), &brushBtn_);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.35f), &brushBtnDisabled_);
        }
        // 歌词画刷与主题无关，单独创建（用户可换色，换色时只重建这四个）
        createLyricBrushes();
        rt->CreateLayer(&coverLayer_);
        recreateFormats();
    }

    // 歌词已播放/未播放/光晕/描边画刷：随用户颜色重建，与主题画刷解耦
    void createLyricBrushes() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
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
        auto rgb = [](COLORREF c, float a) {
            return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                                GetBValue(c) / 255.0f, a);
        };
        rt->CreateSolidColorBrush(rgb(lyricColor_, 1.00f), &brushLyric_);
        rt->CreateSolidColorBrush(rgb(lyricUnplayedColor_, lyricUnplayedAlphaPct_ / 100.0f),
                                  &brushLyricDim_);
        rt->CreateSolidColorBrush(rgb(lyricGlowColor_, 0.28f), &brushLyricGlow_);
        rt->CreateSolidColorBrush(rgb(lyricOutlineColor_, 0.50f), &brushLyricOutline_);
    }

    void setFontGlowColors(COLORREF glow, COLORREF outline) {
        if (lyricGlowColor_ == glow && lyricOutlineColor_ == outline)
            return;
        lyricGlowColor_ = glow;
        lyricOutlineColor_ = outline;
        createLyricBrushes();
        render();
    }

    void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) {
        if (lyricColor_ == played && lyricUnplayedColor_ == unplayed &&
            lyricUnplayedAlphaPct_ == unplayedAlphaPct)
            return;
        lyricColor_ = played;
        lyricUnplayedColor_ = unplayed;
        lyricUnplayedAlphaPct_ = unplayedAlphaPct;
        createLyricBrushes();
        render();
    }

    void setFontGlow(bool on) {
        if (lyricGlow_ == on)
            return;
        lyricGlow_ = on;
        render();
    }

    void setFontOutline(bool on) {
        if (lyricOutline_ == on)
            return;
        lyricOutline_ = on;
        render();
    }

    void setPositionMode(int mode) {
        if (positionMode_ == mode)
            return;
        positionMode_ = mode;
        adjustPosition();
        render();
    }

    // Explorer 重启后由托盘主窗口调用（TaskbarCreated 广播只发给顶层窗口）。
    // 作为 Shell_TrayWnd 的子窗口，歌词窗在 explorer 退出时会随任务栏一起被销毁，
    // 此时只剩野句柄，必须整体重建；若幸存（崩溃非正常退出）则重新附着即可。
    // 歌词/媒体/字体等状态都保存在 Impl 成员里，重建窗口后原样恢复
    void onTaskbarCreated() {
        if (hwnd && IsWindow(hwnd)) {
            if (findTaskbar()) {
                SetParent(hwnd, taskbar_);
                adjustPosition();
                if (visible)
                    ShowWindow(hwnd, SW_SHOWNA);
                render();
            }
            return;
        }
        hwnd = nullptr;
        if (createWindow(inst) && visible) {
            ShowWindow(hwnd, SW_SHOWNA);
            render();
        }
    }

    void recreateFormats() {
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite)
            return;
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_PARAGRAPH_ALIGNMENT pa,
                        IDWriteTextFormat** out) {
            if (*out) {
                (*out)->Release();
                *out = nullptr;
            }
            dwrite->CreateTextFormat(fontFamily_.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", out);
            if (*out) {
                (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                (*out)->SetParagraphAlignment(pa);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                // 不裁剪，超长时由 drawScrollingText 滚动显示
            }
        };
        // 歌名/歌手固定字号，不随歌词字号调整；只有歌词跟随 fontSize_
        make(kBaseFontSize * 1.05f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtTitle_);
        make(kBaseFontSize * 0.92f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtArtist_);
        make(fontSize_ * 1.18f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtLyric_);
        textDirty_ = true;
    }

    void changeFont(float delta) {
        setFont(fontFamily_, fontSize_ + delta);
    }

    void setFont(const std::wstring& family, float size) {
        fontFamily_ = family;
        fontSize_ = std::clamp(size, kMinFont, kMaxFont);
        recreateFormats();
        textDirty_ = true;
        layoutDirty_ = true;
        render();
    }

    void discardDeviceResources() {
        auto r = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        r(fmtTitle_);
        r(fmtArtist_);
        r(fmtLyric_);
        r(titleLayout_);
        r(artistLayout_);
        r(lyricLayout_);
        r(brushBg_);
        r(brushText_);
        r(brushDim_);
        r(brushBtn_);
        r(brushBtnDisabled_);
        r(brushLyric_);
        r(brushLyricDim_);
        r(brushLyricGlow_);
        r(brushLyricOutline_);
        r(coverClip_);
        r(coverLayer_);
        r(icoPlay_);
        r(icoPrev_);
        r(icoNext_);
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
        }
        renderer.discard();
        textDirty_ = true;
        geomDirty_ = true;
        layoutDirty_ = true;
        coverDirty = true;
    }

    void releaseAll() {
        discardDeviceResources();
        renderer.releaseAll();
        if (uia_) {
            uia_->Release();
            uia_ = nullptr;
        }
    }

    // ---------- 封面解码 ----------

    void decodeCover() {
        coverDirty = false;
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
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
        Gdiplus::BitmapData bitmapData{};
        Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
        if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB,
                            &bitmapData) != Gdiplus::Ok) {
            stream->Release();
            return;
        }
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = static_cast<float>(dpi_);
        props.dpiY = static_cast<float>(dpi_);
        hr = rt->CreateBitmap(D2D1::SizeU(w, h), bitmapData.Scan0, bitmapData.Stride, &props,
                                &coverBmp);
        bitmap.UnlockBits(&bitmapData);
        stream->Release();
    }

    // ---------- 排版 ----------

    void buildTextLayouts(float leftW, float rightW) {
        textDirty_ = false;
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle_ || !fmtArtist_ || !fmtLyric_)
            return;

        if (titleLayout_) {
            titleLayout_->Release();
            titleLayout_ = nullptr;
        }
        if (artistLayout_) {
            artistLayout_->Release();
            artistLayout_ = nullptr;
        }
        if (lyricLayout_) {
            lyricLayout_->Release();
            lyricLayout_ = nullptr;
        }

        float infoX = kCoverPadding + coverSize() + kCoverPadding;
        float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
        titleWidth_ = 0.0f;
        titleHeight_ = 0.0f;
        artistWidth_ = 0.0f;
        artistHeight_ = 0.0f;
        if (!media.title.empty()) {
            dwrite->CreateTextLayout(media.title.c_str(), (UINT32)media.title.size(), fmtTitle_,
                                     100000.0f, 40.0f, &titleLayout_);
            if (titleLayout_) {
                DWRITE_TEXT_METRICS m{};
                titleLayout_->GetMetrics(&m);
                titleWidth_ = m.width;
                titleHeight_ = m.height;
            }
        }
        if (!media.artist.empty()) {
            dwrite->CreateTextLayout(media.artist.c_str(), (UINT32)media.artist.size(), fmtArtist_,
                                     100000.0f, 40.0f, &artistLayout_);
            if (artistLayout_) {
                DWRITE_TEXT_METRICS m{};
                artistLayout_->GetMetrics(&m);
                artistWidth_ = m.width;
                artistHeight_ = m.height;
            }
        }

        std::wstring lyric;
        if (currentLine >= 0 && (size_t)currentLine < lines.size()) {
            lyric = lines[(size_t)currentLine].text;
        } else if (!lines.empty()) {
            // 进度还没到达首句时，先显示第一句，避免空白
            lyric = lines.front().text;
        } else {
            lyric = statusText;
        }
        lyricLayout_ = nullptr;
        lyricWidth_ = 0.0f;
        lyricHeight_ = 0.0f;
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
        }
        // 动态滚动速度：让歌词在当前行时长内滚动完一圈。两句间隔越小速度越快，
        // 间隔大到算出的速度低于 kLyricScrollSpeed 时保持最慢速度不变
        lyricScrollSpeed_ = kLyricScrollSpeed;
        if (currentLine >= 0 && (size_t)currentLine + 1 < lines.size() && lyricWidth_ > 0.0f) {
            int64_t durMs =
                lines[(size_t)currentLine + 1].ms - lines[(size_t)currentLine].ms;
            if (durMs > 0) {
                float loopW = lyricWidth_ + kTextPadding * 2.0f;
                lyricScrollSpeed_ =
                    std::max(kLyricScrollSpeed, loopW / (static_cast<float>(durMs) / 1000.0f));
            }
        }
        bool titleChanged = media.title != lastTitle_;
        bool artistChanged = media.artist != lastArtist_;
        bool lyricChanged = lyric != lastLyric_;
        if (titleChanged) {
            titleScrollOffset_ = 0.0f;
            lastTitle_ = media.title;
        }
        if (artistChanged) {
            artistScrollOffset_ = 0.0f;
            lastArtist_ = media.artist;
        }
        if (lyricChanged) {
            lyricScrollOffset_ = 0.0f;
            lastLyric_ = lyric;
        }
        if (titleChanged || artistChanged || lyricChanged) {
            lastTickMs_ = 0;
        }
    }

    float coverSize() const {
        if (!hwnd)
            return 24.0f;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        float h = dip(rc.bottom - rc.top);
        return h - kCoverPadding * 2.0f;
    }

    // 当前行有逐字时间轴且歌词布局对应该行时返回该行，否则 nullptr
    const LyricLine* karaokeLine() const {
        if (currentLine < 0 || (size_t)currentLine >= lines.size())
            return nullptr;
        const LyricLine* line = &lines[(size_t)currentLine];
        if (line->chars.empty() || !lyricLayout_ || line->text != lastLyric_)
            return nullptr;
        return line;
    }

    // 逐字填充目标进度 x（布局像素坐标）：已唱边界 + 当前字按字时长比例推进；
    // durOut 输出当前字时长（ms），供平滑时间常数使用
    float karaokeTargetX(const LyricLine& line, int64_t& durOut) const {
        durOut = 0;
        UINT32 sungLen = 0;
        const LyricChar* curChar = nullptr;
        for (const auto& c : line.chars) {
            if (c.startMs > positionMs_)
                break;
            curChar = &c;
            sungLen += (UINT32)c.text.size();
        }
        if (!curChar)
            return 0.0f;
        UINT32 charOff = sungLen - (UINT32)curChar->text.size();
        DWRITE_HIT_TEST_METRICS hm{};
        float startX = 0.0f, hy = 0.0f;
        if (FAILED(lyricLayout_->HitTestTextPosition(charOff, FALSE, &startX, &hy, &hm)))
            return 0.0f;
        float endX = startX;
        // 当前字结束位置 = 当前字最后一个字形的右边缘（sungLen-1 的 trailing edge）。
        // 注意不能传 sungLen：那取到的是下一个字的右边缘，会多算一个字宽，
        // 导致进入下一字时目标回跳、填充倒退
        if (FAILED(lyricLayout_->HitTestTextPosition(sungLen - 1, TRUE, &endX, &hy, &hm)))
            endX = startX;
        durOut = std::max<int64_t>(curChar->endMs - curChar->startMs, 1);
        float frac =
            (float)std::clamp((double)(positionMs_ - curChar->startMs) / (double)durOut, 0.0, 1.0);
        return startX + (endX - startX) * frac;
    }

    // 平滑步进：过渡时间常数取当前字时长的 1/4（40~200ms），指数趋近目标，
    // 每个字的过渡快慢随其时长自然变化，且同步误差有界（约 τ）。
    // SMTC 锚点校正造成的目标抖动经低通后不再闪烁；行切换或大幅 seek 时直接对齐
    float karaokeSmoothStep(const LyricLine& line) {
        int64_t charDur = 0;
        float target = karaokeTargetX(line, charDur);
        ULONGLONG now = GetTickCount64();
        float dt = karaokeTick_ ? (float)(now - karaokeTick_) : 16.7f;
        karaokeTick_ = now;
        float gap = target - karaokeSmoothX_;
        if (karaokeSmoothLine_ != currentLine || std::fabs(gap) > 100.0f) {
            karaokeSmoothLine_ = currentLine;
            karaokeSmoothX_ = target;
        } else {
            float tau = std::clamp((float)charDur * 0.25f, 40.0f, 200.0f);
            float alpha = 1.0f - std::exp(-dt / tau);
            karaokeSmoothX_ += gap * alpha;
        }
        karaokeProgX_ = karaokeSmoothX_;
        return karaokeProgX_;
    }

    // ---------- 图标几何 ----------

    void ensureGeometry() {
        if (!geomDirty_)
            return;
        geomDirty_ = false;
        ID2D1Factory* d2d = renderer.d2d();
        if (!d2d)
            return;

        if (coverClip_) {
            coverClip_->Release();
            coverClip_ = nullptr;
        }
        if (icoPlay_) {
            icoPlay_->Release();
            icoPlay_ = nullptr;
        }
        if (icoPrev_) {
            icoPrev_->Release();
            icoPrev_ = nullptr;
        }
        if (icoNext_) {
            icoNext_->Release();
            icoNext_ = nullptr;
        }

        float s = coverSize();
        D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, s, s), 4.0f, 4.0f};
        d2d->CreateRoundedRectangleGeometry(rr, &coverClip_);

        // 绘制带圆角的三角形：dir=1 向右，dir=-1 向左
        auto makeRoundedTriangle = [&](int dir, ID2D1PathGeometry** out) {
            float tipX = dir * 0.55f;
            float baseX = -dir * 0.35f;
            constexpr float rad = 0.10f;
            float ux = dir * 0.8739f;
            float uy = 0.4856f;
            D2D1_POINT_2F tip = {tipX, 0.0f};
            D2D1_POINT_2F baseTop = {baseX, -0.5f};
            D2D1_POINT_2F baseBottom = {baseX, 0.5f};
            if (FAILED(d2d->CreatePathGeometry(out)))
                return;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED((*out)->Open(&sink)))
                return;
            sink->BeginFigure({baseTop.x + rad * ux, baseTop.y + rad * uy},
                              D2D1_FIGURE_BEGIN_FILLED);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                baseTop, {baseTop.x, baseTop.y + rad}));
            sink->AddLine({baseBottom.x, baseBottom.y - rad});
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                baseBottom, {baseBottom.x + rad * ux, baseBottom.y - rad * uy}));
            sink->AddLine({tip.x - rad * ux, tip.y + rad * uy});
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                tip, {tip.x - rad * ux, tip.y - rad * uy}));
            sink->AddLine({baseTop.x + rad * ux, baseTop.y + rad * uy});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
        };

        // 播放 / 下一首：右向圆角三角
        makeRoundedTriangle(1, &icoPlay_);
        // 上一首：左向圆角三角
        makeRoundedTriangle(-1, &icoPrev_);
        // 下一首与播放同形，绘制时直接使用
        makeRoundedTriangle(1, &icoNext_);
    }

    // ---------- 渲染 ----------

    void drawButton(int idx, const D2D1_POINT_2F& c, float r) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        bool enabled = idx == 0 ? media.canPrev : idx == 1 ? media.canPlayPause : media.canNext;
        ID2D1SolidColorBrush* brush = enabled ? brushBtn_ : brushBtnDisabled_;

        if (idx == 1) {
            // 播放/暂停：使用系统风格纯色图标
            if (media.playing) {
                // 暂停：两根圆角竖条
                float w = r * 0.22f;
                float gap = r * 0.20f;
                float h = r * 0.55f;
                float barR = w * 0.45f;
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(c.x - gap - w, c.y - h, c.x - gap, c.y + h), barR,
                                      barR),
                    brush);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(c.x + gap, c.y - h, c.x + gap + w, c.y + h), barR,
                                      barR),
                    brush);
            } else if (icoPlay_) {
                rt->SetTransform(D2D1::Matrix3x2F::Scale(r * 1.4f, r * 1.4f) *
                                 D2D1::Matrix3x2F::Translation(c.x + r * 0.05f, c.y));
                rt->FillGeometry(icoPlay_, brush);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        } else {
            // 上一首/下一首：圆角三角 + 圆角竖条
            ID2D1PathGeometry* g = idx == 0 ? icoPrev_ : icoNext_;
            float sc = r * 1.4f;
            float barW = 0.16f * sc;
            float barH = 1.0f * sc;
            float barR = barW * 0.45f;
            float barX = idx == 0 ? c.x - 0.72f * sc : c.x + 0.56f * sc;
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(barX, c.y - barH * 0.5f, barX + barW,
                                              c.y + barH * 0.5f),
                                  barR, barR),
                brush);
            if (g) {
                rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) *
                                 D2D1::Matrix3x2F::Translation(c.x, c.y));
                rt->FillGeometry(g, brush);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }
    }

    int hitButton(float x, float y) const {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        float w = static_cast<float>(rc.right - rc.left);
        float h = static_cast<float>(rc.bottom - rc.top);
        float leftW = w * kLeftRatio;
        if (x < leftW || x > w)
            return -1;

        float cx = leftW + (w - leftW) * 0.5f;
        float cy = h * 0.5f;
        float r = h * 0.26f;
        float spacing = r * 2.8f;
        for (int i = 0; i < 3; ++i) {
            float bx = cx + (i - 1) * spacing;
            bool en = i == 0 ? media.canPrev : i == 1 ? media.canPlayPause : media.canNext;
            if (!en)
                continue;
            if (std::hypot(x - bx, y - cy) <= r + 4.0f)
                return i;
        }
        return -1;
    }

    // 绘制可滚动文本：容得下则居中，容不下则向左无缝滚动。
    // outline/glow 非空时先画 8 方向光晕层和深色描边层，再画主文字。
    // karaokeBrush 非空时启用逐字高亮：整行先用 brush（未播放色）画一遍，
    // 再按像素裁剪出 [文本起点, karaokeX] 区域用 karaokeBrush（已播放色）画第二遍，
    // 实现字内平滑填充（裁剪基于像素，不受滚动偏移影响）
    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                           ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f) {
        auto* rt = renderer.renderTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        D2D1_RECT_F clip{x, y, x + areaW, y + textH};
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        static constexpr float kDirs[8][2] = {{1.0f, 0.0f},
                                              {0.7071f, 0.7071f},
                                              {0.0f, 1.0f},
                                              {-0.7071f, 0.7071f},
                                              {-1.0f, 0.0f},
                                              {-0.7071f, -0.7071f},
                                              {0.0f, -1.0f},
                                              {0.7071f, -0.7071f}};
        // 绘制起点：居中一个，或跑马灯首尾相接两个；逐字跟随滚动时只有一份、不循环
        float bases[2];
        int n = 0;
        if (karaokeBrush) {
            bases[n++] = (textW <= areaW) ? x + (areaW - textW) * 0.5f : x - offset;
        } else if (textW <= areaW) {
            bases[n++] = x + (areaW - textW) * 0.5f;
        } else {
            float loopW = textW + kTextPadding * 2.0f;
            bases[n++] = x - offset;
            bases[n++] = x - offset + loopW;
        }
        if (glow || outline) {
            for (int i = 0; i < n; ++i) {
                if (glow) {
                    for (auto& d : kDirs)
                        rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 2.4f, y + d[1] * 2.4f),
                                           layout, glow);
                }
                if (outline) {
                    for (auto& d : kDirs)
                        rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 1.2f, y + d[1] * 1.2f),
                                           layout, outline);
                }
            }
        }
        for (int i = 0; i < n; ++i)
            rt->DrawTextLayout(D2D1::Point2F(bases[i], y), layout, brush);
        // 逐字高亮层：裁剪到填充进度
        if (karaokeBrush && karaokeX > 0.0f) {
            rt->PushAxisAlignedClip(D2D1::RectF(bases[0], y, bases[0] + karaokeX, y + textH),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->DrawTextLayout(D2D1::Point2F(bases[0], y), layout, karaokeBrush);
            rt->PopAxisAlignedClip();
        }
        rt->PopAxisAlignedClip();
    }

    void render() {
        if (!visible || !hwnd)
            return;
        createDeviceResources();

        RECT rc{};
        GetClientRect(hwnd, &rc);
        int pxW = rc.right - rc.left;
        int pxH = rc.bottom - rc.top;
        if (pxW <= 0 || pxH <= 0)
            return;
        if (pxW != lastPxW_ || pxH != lastPxH_) {
            lastPxW_ = pxW;
            lastPxH_ = pxH;
            geomDirty_ = true;
            textDirty_ = true;
        }

        if (!renderer.bindDC(pxW, pxH))
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);

        if (layoutDirty_) {
            adjustPosition();
            layoutDirty_ = false;
        }

        ensureGeometry();
        if (coverDirty)
            decodeCover();

        float w = dip(pxW);
        float h = dip(pxH);
        float leftW = w * kLeftRatio;
        float rightW = w - leftW;

        if (textDirty_)
            buildTextLayouts(leftW, rightW);

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        // 背景：alpha 极低，视觉上透明但保证整个窗口可命中
        D2D1_ROUNDED_RECT bg{D2D1::RectF(0.0f, 0.0f, w, h), kCornerRadius, kCornerRadius};
        rt->FillRoundedRectangle(bg, brushBg_);

        // 左侧封面
        float s = coverSize();
        float coverX = kCoverPadding;
        float coverY = (h - s) * 0.5f;
        D2D1_RECT_F coverRect = D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
        if (coverBmp && coverClip_ && coverLayer_) {
            rt->PushLayer(D2D1::LayerParameters(
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

        // 左侧歌曲信息（封面右侧垂直排列，整体垂直居中，超长自动滚动）
        float infoX = coverX + s + kCoverPadding;
        float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
        float infoGap = 2.0f;
        float totalInfoH = titleHeight_ + infoGap + artistHeight_;
        float infoY = (h - totalInfoH) * 0.5f;
        drawScrollingText(titleLayout_, titleWidth_, titleHeight_, infoW, infoX, infoY,
                          titleScrollOffset_, brushText_);
        drawScrollingText(artistLayout_, artistWidth_, artistHeight_, infoW, infoX,
                          infoY + titleHeight_ + infoGap, artistScrollOffset_, brushDim_);

        // 右侧歌词区：未悬浮时滚动显示歌词，悬浮时显示控制按钮
        float lyricAreaX = leftW + kTextPadding;
        float lyricAreaW = std::max(1.0f, rightW - kTextPadding * 2.0f);
        float lyricY = h * 0.5f - lyricHeight_ * 0.5f;

        if (mouseOver_) {
            float cx = leftW + rightW * 0.5f;
            float cy = h * 0.5f;
            float r = h * 0.26f;
            float spacing = r * 2.8f;
            for (int i = 0; i < 3; ++i)
                drawButton(i, D2D1::Point2F(cx + (i - 1) * spacing, cy), r);
        } else {
            // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
            // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
            const LyricLine* curLine = karaokeLine();
            bool karaoke = curLine && brushLyric_ && brushLyricDim_;
            float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
            drawScrollingText(lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX,
                              lyricY, lyricScrollOffset_,
                              karaoke ? brushLyricDim_
                                      : (brushLyric_ ? brushLyric_ : brushText_),
                              lyricOutline_ ? brushLyricOutline_ : nullptr,
                              lyricGlow_ ? brushLyricGlow_ : nullptr,
                              karaoke ? brushLyric_ : nullptr, progX);
        }

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardDeviceResources();
            return;
        }
        if (SUCCEEDED(hr))
            renderer.present(hwnd);
        else
            std::wprintf(L"[taskbar] EndDraw failed: 0x%08X\n", hr);
    }

    // ---------- 滚动字幕 ----------

    void updateScroll() {
        ULONGLONG now = GetTickCount64();
        if (lastTickMs_ == 0)
            lastTickMs_ = now;
        float dt = static_cast<float>(now - lastTickMs_) / 1000.0f;
        lastTickMs_ = now;

        auto marquee = [&](float textW, float areaW, float speed, float& offset) {
            if (textW <= areaW || areaW <= 0.0f) {
                offset = 0.0f;
                return;
            }
            float loopW = textW + kTextPadding * 2.0f;
            offset += speed * dt;
            if (offset >= loopW)
                offset -= loopW;
        };

        RECT rc{};
        GetClientRect(hwnd, &rc);
        float h = dip(rc.bottom - rc.top);
        float w = dip(rc.right - rc.left);
        if (h <= 0.0f || w <= 0.0f)
            return;

        float leftW = w * kLeftRatio;
        float rightW = w - leftW;
        float infoX = kCoverPadding + coverSize() + kCoverPadding;
        float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
        float lyricAreaW = std::max(1.0f, rightW - kTextPadding * 2.0f);

        marquee(titleWidth_, infoW, kInfoScrollSpeed, titleScrollOffset_);
        marquee(artistWidth_, infoW, kInfoScrollSpeed, artistScrollOffset_);
        if (mouseOver_)
            return;
        // 逐字歌词：滚动位置跟随逐字填充进度，把当前唱到的位置保持在区域 30% 处，
        // 行尾为止不再循环；非逐字歌词保持原有无缝循环滚动
        if (karaokeLine()) {
            if (lyricWidth_ > lyricAreaW) {
                // karaokeProgX_ 由 render 每帧更新；行刚切换时还没对应进度，从头开始
                float sungX = (karaokeSmoothLine_ == currentLine) ? karaokeProgX_ : 0.0f;
                float target =
                    std::clamp(sungX - lyricAreaW * 0.3f, 0.0f, lyricWidth_ - lyricAreaW);
                float diff = target - lyricScrollOffset_;
                // ease-out 平滑跟随，避免跳动生硬
                lyricScrollOffset_ =
                    std::fabs(diff) < 0.5f ? target : lyricScrollOffset_ + diff * 0.15f;
            } else {
                lyricScrollOffset_ = 0.0f;
            }
        } else {
            marquee(lyricWidth_, lyricAreaW, lyricScrollSpeed_, lyricScrollOffset_);
        }
    }

    // ---------- 事件 ----------

    void onTimer() {
        if (tick)
            tick();
        // 任务栏位置/DPI/主题跟踪是耗时操作，放到慢速分支，不跟 60fps 走
        if (++slowTick_ >= kSlowTickInterval) {
            slowTick_ = 0;
            if (detectChanges())
                adjustPosition();
        }
        updateScroll();
        render();
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
            SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kTimerId)
                onTimer();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            // 整个窗口区域都视为客户区，让透明背景也能接收鼠标消息
            return HTCLIENT;
        case WM_MOUSEMOVE: {
            bool wasOver = mouseOver_;
            mouseOver_ = true;
            if (!wasOver)
                render();
            trackMouseLeave();
            return 0;
        }
        case WM_MOUSELEAVE:
            mouseOver_ = false;
            trackingLeave_ = false;
            render();
            return 0;
        case WM_LBUTTONUP: {
            float mx = static_cast<float>(GET_X_LPARAM(lp));
            float my = static_cast<float>(GET_Y_LPARAM(lp));
            int btn = hitButton(mx, my);
            if (btn >= 0 && onControl)
                onControl(static_cast<MediaControl>(btn));
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            releaseAll();
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

void TaskbarHost::setMediaInfo(const OverlayMediaInfo& info) {
    bool thumbChanged = info.thumbnail != impl_->media.thumbnail;
    bool textChanged = info.title != impl_->media.title || info.artist != impl_->media.artist;
    impl_->media = info;
    if (thumbChanged)
        impl_->coverDirty = true;
    if (textChanged)
        impl_->textDirty_ = true;
    impl_->render();
}

void TaskbarHost::setControlCallback(std::function<void(MediaControl)> cb) {
    impl_->onControl = std::move(cb);
}

const std::vector<LyricLine>& TaskbarHost::lyrics() const {
    return impl_->lines;
}

void TaskbarHost::show() {
    if (!impl_->visible) {
        impl_->visible = true;
        ShowWindow(impl_->hwnd, SW_SHOWNA);
    }
    impl_->render();
}

void TaskbarHost::hide() {
    if (impl_->visible) {
        impl_->visible = false;
        ShowWindow(impl_->hwnd, SW_HIDE);
    }
}

void TaskbarHost::setLyrics(const std::vector<LyricLine>& lines) {
    impl_->lines = lines;
    impl_->currentLine = -1;
    impl_->textDirty_ = true;
    if (!lines.empty())
        impl_->statusText.clear();
    impl_->render();
}

void TaskbarHost::setCurrentLine(int index) {
    if (index != impl_->currentLine) {
        impl_->currentLine = index;
        impl_->textDirty_ = true;
    }
}

void TaskbarHost::setPosition(int64_t positionMs) {
    impl_->positionMs_ = positionMs;
}

void TaskbarHost::setStatusText(const std::wstring& text) {
    impl_->statusText = text;
    impl_->textDirty_ = true;
    impl_->render();
}

bool TaskbarHost::isTaskbar() const {
    return true;
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

void TaskbarHost::setFont(const std::wstring& family, float size) {
    impl_->setFont(family, size);
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

void TaskbarHost::setPositionMode(int mode) {
    impl_->setPositionMode(mode);
}

void TaskbarHost::onTaskbarCreated() {
    impl_->onTaskbarCreated();
}

void TaskbarHost::setClickThrough(bool on) {
    // 任务栏歌词不需要鼠标穿透，忽略
    (void)on;
}

bool TaskbarHost::clickThrough() const {
    return false;
}
