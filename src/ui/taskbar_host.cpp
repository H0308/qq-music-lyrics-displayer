#include "taskbar_host.h"
#include "lyric_renderer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
constexpr float kSongInfoLyricGap = 0.0f; // 歌曲信息与歌词之间的左侧间距
constexpr float kCornerRadius = 8.0f;
constexpr float kInfoScrollSpeed = 10.0f;  // 歌名/歌手滚动速度（DIP/s）
constexpr float kLyricScrollSpeed = 15.0f; // 歌词滚动速度（DIP/s）
constexpr float kLyricTransitionMs = 280.0f; // 相邻歌词上下切换时长
constexpr float kLyricPreviewGap = 3.0f; // 普通双行模式的核心行与下一行间距
constexpr float kLyricPreviewOpacity = 0.42f; // 下一行预览透明度
constexpr float kLyricMainFontScale = 1.18f;
constexpr float kLyricPreviewFontScale = 0.86f;
constexpr float kLyricPreviewScale = kLyricPreviewFontScale / kLyricMainFontScale;
constexpr float kMinFont = 9.0f;
constexpr float kMaxFont = 18.0f;
constexpr float kBaseFontSize = 12.0f;
constexpr float kSpectrumBarW = 7.0f;  // 频谱柱宽
constexpr float kSpectrumGap = 6.0f;   // 频谱柱间隙
constexpr float kVinylRotationDegPerSecond = 30.0f; // 黑胶唱片转速：12 秒一圈，保持视觉克制
constexpr float kVinylHaloWidth = 2.5f;
constexpr float kVinylInnerRatio = 0.30f; // 圆形专辑封面半径 / 封面槽边长

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

std::wstring findProcessImagePath(const std::wstring& processName) {
    if (processName.empty())
        return {};

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) != 0)
                continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
            if (QueryFullProcessImageNameW(process, 0, path, &length))
                result.assign(path, length);
            CloseHandle(process);
            if (!result.empty())
                break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::wstring resolveSourceIconPath(const std::wstring& sourceAppUserModelId) {
    if (sourceAppUserModelId.empty())
        return {};

    // 网易云的增强会话可能由桥接器发布，但平台图标应优先取网易云客户端本体。
    std::vector<std::wstring> candidates;
    if (_wcsicmp(sourceAppUserModelId.c_str(), L"NeteaseBridge.exe") == 0) {
        candidates.emplace_back(L"cloudmusic.exe");
        candidates.emplace_back(L"NeteaseBridge.exe");
    } else {
        candidates.push_back(sourceAppUserModelId);
    }
    for (const auto& candidate : candidates) {
        std::wstring path = findProcessImagePath(candidate);
        if (!path.empty())
            return path;
    }

    // 兼容 SMTC 直接返回本地可访问路径的来源标识。
    DWORD attributes = GetFileAttributesW(sourceAppUserModelId.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return sourceAppUserModelId;
    return {};
}

void clearIconBlackMatte(std::vector<BYTE>& pixels, UINT width, UINT height) {
    if (pixels.empty() || width == 0 || height == 0)
        return;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<BYTE> cleared(pixelCount, 0);
    std::vector<size_t> pending;
    pending.reserve(pixelCount);

    auto nearBlack = [&](size_t index) {
        const BYTE* p = pixels.data() + index * 4;
        return p[3] != 0 && p[0] <= 48 && p[1] <= 48 && p[2] <= 48;
    };
    auto enqueue = [&](size_t index) {
        if (!cleared[index] && nearBlack(index)) {
            cleared[index] = 1;
            pending.push_back(index);
        }
    };

    for (UINT x = 0; x < width; ++x) {
        enqueue(x);
        enqueue(static_cast<size_t>(height - 1) * width + x);
    }
    for (UINT y = 0; y < height; ++y) {
        enqueue(static_cast<size_t>(y) * width);
        enqueue(static_cast<size_t>(y) * width + width - 1);
    }

    while (!pending.empty()) {
        const size_t index = pending.back();
        pending.pop_back();
        const UINT x = static_cast<UINT>(index % width);
        const UINT y = static_cast<UINT>(index / width);
        if (x > 0)
            enqueue(index - 1);
        if (x + 1 < width)
            enqueue(index + 1);
        if (y > 0)
            enqueue(index - width);
        if (y + 1 < height)
            enqueue(index + width);
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        BYTE* p = pixels.data() + i * 4;
        if (cleared[i] || p[3] == 0)
            p[0] = p[1] = p[2] = p[3] = 0;
    }
}

bool readIconPixels(HICON icon, std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    if (!icon)
        return false;

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromHICON(icon));
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        return false;

    width = bitmap->GetWidth();
    height = bitmap->GetHeight();
    if (width == 0 || height == 0)
        return false;

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok)
        return false;

    pixels.resize(static_cast<size_t>(width) * height * 4);
    const BYTE* base = static_cast<const BYTE*>(data.Scan0);
    const LONG stride = data.Stride;
    const LONG rowStride = stride >= 0 ? stride : -stride;
    for (UINT y = 0; y < height; ++y) {
        const UINT sourceRow = stride >= 0 ? y : height - 1 - y;
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    base + static_cast<size_t>(sourceRow) * rowStride,
                    static_cast<size_t>(width) * 4);
    }
    bitmap->UnlockBits(&data);

    // Direct2D 使用预乘 Alpha；同时清理透明像素的残留 RGB 和黑色不透明底边。
    clearIconBlackMatte(pixels, width, height);
    return true;
}

bool readSourceIconPixels(const std::wstring& path, std::vector<BYTE>& pixels, UINT& width,
                          UINT& height) {
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    ExtractIconExW(path.c_str(), 0, &largeIcon, &smallIcon, 1);
    HICON icon = largeIcon ? largeIcon : smallIcon;
    HICON shellIcon = nullptr;
    if (!icon) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON))
            shellIcon = info.hIcon;
        icon = shellIcon;
    }

    const bool ok = readIconPixels(icon, pixels, width, height);
    if (largeIcon)
        DestroyIcon(largeIcon);
    if (smallIcon)
        DestroyIcon(smallIcon);
    if (shellIcon)
        DestroyIcon(shellIcon);
    return ok;
}

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
        if (!offscreen && rc.right > rc.left)
            out.push_back(rc);
    }
    arr->Release();
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

} // namespace

struct TaskbarHost::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool visible = false;
    bool timerRunning_ = false;

    // 任务栏句柄与子部件
    HWND taskbar_ = nullptr;
    HWND notify_ = nullptr;
    HWND start_ = nullptr;
    RECT rcTaskbar_{};   // 任务栏屏幕坐标（缓存）
    RECT rcNotify_{};    // 通知区屏幕坐标（缓存）
    RECT rcStart_{};     // 开始按钮屏幕坐标（缓存，找不到时为空）
    RECT rcTrafficMonitor_{}; // TrafficMonitor 屏幕坐标（缓存，未运行时为 empty）
    // 任务栏 XAML 按钮（开始/搜索/任务视图/小组件/应用图标）包围矩形缓存（屏幕坐标）。
    // 以上两项由探测工作线程产出、UI 线程拾取（见 probeOut_）
    std::vector<RECT> uiaButtons_;
    UINT dpi_ = 96;
    bool centerAlign_ = true;
    bool lightTheme_ = false;
    int positionMode_ = 0; // 0 = 通知区域左侧；1 = 任务栏最左侧

    // 避让探测工作线程：UIA / GetWindowText 等阻塞型跨进程调用全部在这里执行，
    // UI 线程永不阻塞，从根上避免与 explorer 互相死等。结果经原子指针交付
    struct ProbeResult {
        RECT rcTm{};
        std::vector<RECT> buttons;
    };
    std::thread probeThread_;
    std::atomic<bool> probeStop_{false};
    std::atomic<ProbeResult*> probeOut_{nullptr};
    std::atomic<HWND> taskbarAtomic_{nullptr}; // taskbar_ 的线程安全副本（探测线程读）

    // 歌词状态
    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    int currentLine = -1;
    int64_t positionMs_ = 0; // 播放进度（每帧更新），驱动逐字高亮
    DisplayScene scene_ = DisplayScene::NoPlayback;
    std::wstring trackKey_;
    uint64_t requestGeneration_ = 0;
    uint64_t frameRevision_ = 0;

    // 字体
    float fontSize_ = kBaseFontSize;
    std::wstring fontFamily_ = kFontFamily;

    // 媒体信息
    OverlayMediaInfo media;
    ID2D1Bitmap* coverBmp = nullptr;
    bool coverDirty = true;
    ID2D1Bitmap* platformIconBmp = nullptr;
    bool platformIconDirty = true;

    // 交互
    std::function<void()> tick;
    std::function<void(MediaControl)> onControl;
    bool mouseOver_ = false;
    bool trackingLeave_ = false;
    bool controlsOnHover_ = true;
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
    float secondaryWidth_ = 0.0f;
    float secondaryHeight_ = 0.0f;
    float secondaryScrollOffset_ = 0.0f;
    std::wstring lastTitle_;
    std::wstring lastArtist_;
    std::wstring lastLyric_;
    std::wstring lastSecondary_;
    ULONGLONG lastTickMs_ = 0;
    int slowTick_ = 0; // 慢速分支计数器

    // 渲染
    LyricRenderer renderer;
    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtArtist_ = nullptr;
    IDWriteTextFormat* fmtLyric_ = nullptr;
    IDWriteTextFormat* fmtNextLyric_ = nullptr;
    IDWriteTextFormat* fmtSecondary_ = nullptr;
    IDWriteTextLayout* titleLayout_ = nullptr;
    IDWriteTextLayout* artistLayout_ = nullptr;
    IDWriteTextLayout* lyricLayout_ = nullptr;
    IDWriteTextLayout* nextLyricLayout_ = nullptr;
    IDWriteTextLayout* secondaryLayout_ = nullptr;
    // 行切换动画保留上一帧的布局，避免新行直接替换导致文字瞬移。
    IDWriteTextLayout* outgoingLyricLayout_ = nullptr;
    IDWriteTextLayout* outgoingSecondaryLayout_ = nullptr;
    float outgoingLyricWidth_ = 0.0f;
    float outgoingLyricHeight_ = 0.0f;
    float outgoingLyricBlockHeight_ = 0.0f;
    float outgoingSecondaryWidth_ = 0.0f;
    float outgoingSecondaryHeight_ = 0.0f;
    float nextLyricWidth_ = 0.0f;
    float nextLyricHeight_ = 0.0f;
    ULONGLONG lyricTransitionStartMs_ = 0;
    int lyricTransitionDirection_ = 1; // 1: 新行从下方进入，-1: 从上方进入
    uint64_t lyricTransitionRevision_ = 0;
    bool lyricTransitionPending_ = false;
    bool lyricTransitionActive_ = false;
    // 行过渡目标：currentLine 是逻辑当前行；transitionTarget_ 是本次动画要进入的行，
    // pendingTarget_ 是动画期间收到的最新目标（latest-frame-wins，只覆盖不累积），
    // 动画结束收尾时统一消费，不让旧的结束逻辑覆盖新行。
    struct LyricTransitionTarget {
        int lineIndex = -1;
        int64_t actualPositionMs = 0;
        int direction = 1;
        uint64_t frameRevision = 0;
    };
    LyricTransitionTarget transitionTarget_{};
    LyricTransitionTarget pendingTarget_{};
    bool transitionTargetValid_ = false;
    bool pendingTargetValid_ = false;
    ULONGLONG frameNowMs_ = 0;
    ID2D1SolidColorBrush* brushBg_ = nullptr;
    ID2D1SolidColorBrush* brushText_ = nullptr;
    ID2D1SolidColorBrush* brushDim_ = nullptr;
    ID2D1SolidColorBrush* brushBtn_ = nullptr;
    ID2D1SolidColorBrush* brushBtnDisabled_ = nullptr;
    ID2D1SolidColorBrush* brushLyric_ = nullptr;        // 已播放歌词颜色（用户可配）
    ID2D1SolidColorBrush* brushLyricDim_ = nullptr;     // 逐字歌词未唱部分（独立颜色+透明度）
    ID2D1SolidColorBrush* brushLyricGlow_ = nullptr;    // 歌词光晕（主色低透明度）
    ID2D1SolidColorBrush* brushLyricOutline_ = nullptr; // 歌词深色描边
    ID2D1SolidColorBrush* brushCoverHalo_ = nullptr;    // 黑胶外圈光环（复用已播放色）
    ID2D1SolidColorBrush* brushVinylBase_ = nullptr;    // 黑胶唱片底色
    ID2D1SolidColorBrush* brushVinylGroove_ = nullptr;  // 黑胶纹理线
    COLORREF lyricColor_ = RGB(49, 194, 124);           // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124);   // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                    // 逐字未播放透明度（%）
    COLORREF lyricGlowColor_ = RGB(49, 194, 124);       // 光晕颜色
    COLORREF lyricOutlineColor_ = RGB(0, 0, 0);         // 描边颜色
    bool lyricGlow_ = false;                            // 光晕开关
    bool lyricOutline_ = false;                         // 描边开关
    bool translationEnabled_ = true;
    bool romanizationEnabled_ = false;
    bool doubleLineLyricsEnabled_ = false;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    bool secondaryContentAvailable_ = false;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;
    bool clientAnimations_ = true;
    float vinylAngleDeg_ = 0.0f;
    ULONGLONG vinylTickMs_ = 0;
    // 频谱：画刷随歌词已播放色重建（createLyricBrushes），bands 由 UI 线程每帧写入
    ID2D1SolidColorBrush* brushSpectrum_ = nullptr;
    bool spectrumVisible_ = false;
    std::array<float, TaskbarHost::kSpectrumBands> spectrumBands_{};
    ID2D1RoundedRectangleGeometry* coverClip_ = nullptr;
    ID2D1EllipseGeometry* vinylCoverClip_ = nullptr;
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

    bool updateMediaInfo(const OverlayMediaInfo& info) {
        bool thumbChanged = info.thumbnail != media.thumbnail;
        bool textChanged = info.title != media.title || info.artist != media.artist;
        bool controlsChanged = info.canPrev != media.canPrev ||
                               info.canPlayPause != media.canPlayPause ||
                               info.canNext != media.canNext;
        bool playingChanged = info.playing != media.playing;
        bool platformChanged = info.sourceAppUserModelId != media.sourceAppUserModelId;
        media = info;
        if (thumbChanged)
            coverDirty = true;
        if (platformChanged)
            platformIconDirty = true;
        if (thumbChanged || textChanged)
            vinylAngleDeg_ = 0.0f;
        if (textChanged)
            textDirty_ = true;
        if (thumbChanged || textChanged || playingChanged)
            vinylTickMs_ = GetTickCount64();
        return thumbChanged || textChanged || controlsChanged || playingChanged || platformChanged;
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
        lightTheme_ = isSystemLightTheme();
        updateRects();
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
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        clientAnimations_ = animations != FALSE;

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
        startProbe(); // 避让探测（阻塞型跨进程调用）全程在工作线程执行
        return true;
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
        float minWidthDip = kMinWidthDip;
        float maxWidthDip = kMaxWidthDip;
        if (!songInfoVisible_) {
            // 保留原歌词区宽度，只扣除歌曲信息区；左侧压缩为可见的封面区域。
            const float compactLeftDip = albumCoverVisible_ ? coverSlotWidth(dip(pxH)) : 0.0f;
            minWidthDip = kMinWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
            maxWidthDip = kMaxWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
        }
        int minW = (int)std::lround(minWidthDip * scale());
        int maxW = (int)std::lround(maxWidthDip * scale());
        // 频谱开启时整体加宽（频谱簇 + 与歌词的间距），不压缩歌词原有宽度；关闭即恢复
        if (spectrumVisible_) {
            int extra = (int)std::lround((spectrumClusterW() + kTextPadding) * scale());
            minW += extra;
            maxW += extra;
        }

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

        RECT rcStart{};
        if (start_)
            GetWindowRect(start_, &rcStart);

        // TrafficMonitor / UIA 按钮矩形由探测工作线程提供（pickProbeResult），
        // 这里只做非阻塞检查，UI 线程不允许出现阻塞型跨进程调用
        bool changed = dpi != dpi_ || center != centerAlign_ || themeChanged ||
                       !EqualRect(&rcTaskbar, &rcTaskbar_) ||
                       !EqualRect(&rcNotify, &rcNotify_) ||
                       !EqualRect(&rcStart, &rcStart_);
        if (changed) {
            // 封面位图按显示尺寸解码（decodeCover），DPI/任务栏高度变化时按新尺寸重解码
            if (dpi != dpi_ ||
                (rcTaskbar.bottom - rcTaskbar.top) != (rcTaskbar_.bottom - rcTaskbar_.top))
                coverDirty = true;
            dpi_ = dpi;
            centerAlign_ = center;
            rcTaskbar_ = rcTaskbar;
            rcNotify_ = rcNotify;
            rcStart_ = rcStart;
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

    // 歌词与黑胶画刷：随用户颜色重建，与主题画刷解耦
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
        if (brushSpectrum_) {
            brushSpectrum_->Release();
            brushSpectrum_ = nullptr;
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
        // 频谱柱：跟随已播放色，略降透明度与歌词文字区分层次
        rt->CreateSolidColorBrush(rgb(lyricColor_, 0.60f), &brushSpectrum_);
        createAlbumCoverBrushes();
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

    void setSecondaryLyricMode(bool translation, bool romanization) {
        // 调用方保证互斥，这里再收紧一次，避免异常状态绘制三行。
        if (translation && romanization)
            romanization = false;
        if (translationEnabled_ == translation && romanizationEnabled_ == romanization)
            return;
        translationEnabled_ = translation;
        romanizationEnabled_ = romanization;
        refreshSecondaryContent();
        resetLyricTransition();
        textDirty_ = true;
        render();
    }

    void setDoubleLineLyrics(bool on) {
        if (doubleLineLyricsEnabled_ == on)
            return;
        doubleLineLyricsEnabled_ = on;
        if (!secondaryContentAvailable_)
            resetLyricTransition();
        textDirty_ = true;
        render();
    }

    void setLyricAlignment(LyricAlignment alignment) {
        if (lyricAlignment_ == alignment)
            return;
        lyricAlignment_ = alignment;
        lyricScrollOffset_ = 0.0f;
        secondaryScrollOffset_ = 0.0f;
        render();
    }

    void setControlsOnHover(bool on) {
        if (controlsOnHover_ == on)
            return;
        controlsOnHover_ = on;
        render();
    }

    void setSpectrumVisible(bool on) {
        if (spectrumVisible_ == on)
            return;
        spectrumVisible_ = on;
        if (!on)
            spectrumBands_.fill(0.0f);
        // 先改窗口宽度再渲染：若在 render 内经 layoutDirty_ 改大小，
        // render 结尾的 present 会用旧尺寸位图把窗口尺寸拽回去（与 setPositionMode 同序）
        adjustPosition();
        render();
    }

    void setSongInfoVisible(bool on) {
        if (songInfoVisible_ == on)
            return;
        songInfoVisible_ = on;
        titleScrollOffset_ = 0.0f;
        artistScrollOffset_ = 0.0f;
        textDirty_ = true;
        adjustPosition();
        render();
    }

    void setAlbumCoverVisible(bool on) {
        if (albumCoverVisible_ == on)
            return;
        albumCoverVisible_ = on;
        textDirty_ = true;
        adjustPosition();
        render();
    }

    void setPlatformIconVisible(bool on) {
        if (platformIconVisible_ == on)
            return;
        platformIconVisible_ = on;
        platformIconDirty = true;
        render();
    }

    void setAlbumCoverEffect(AlbumCoverEffect effect) {
        if (albumCoverEffect_ == effect)
            return;
        albumCoverEffect_ = effect;
        vinylAngleDeg_ = 0.0f;
        vinylTickMs_ = GetTickCount64();
        render();
    }

    void setSpectrumBands(const std::array<float, TaskbarHost::kSpectrumBands>& bands) {
        spectrumBands_ = bands; // 无需立即 render：60fps 定时器每帧都会渲染
    }

    void applyPlaybackPatch(const PlaybackPatch& patch) {
        // 高频补丁必须属于当前已经应用的完整帧；旧曲目或旧帧的位置不能回写。
        if (patch.frameRevision != frameRevision_ ||
            patch.requestGeneration != requestGeneration_)
            return;

        positionMs_ = patch.actualPositionMs;
        if (media.playing != patch.playing) {
            media.playing = patch.playing;
            vinylTickMs_ = GetTickCount64();
        }

        if (patch.currentLine == currentLine)
            return;

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
        const bool sceneChanged = frame.scene != scene_;
        const bool wasVisible = visible;
        const bool mediaChanged = updateMediaInfo(frame.media);

        frameRevision_ = frame.frameRevision;
        requestGeneration_ = frame.requestGeneration;
        trackKey_ = frame.trackKey;
        scene_ = frame.scene;
        positionMs_ = frame.actualPositionMs;
        statusText = frame.statusText;

        if (trackChanged || lyricsChanged) {
            lines = frame.lyrics;
            refreshSecondaryContent();
            currentLine = frame.currentLine;
            resetLyricTransition();
            if (nextLyricLayout_) {
                nextLyricLayout_->Release();
                nextLyricLayout_ = nullptr;
            }
            nextLyricWidth_ = 0.0f;
            nextLyricHeight_ = 0.0f;
            textDirty_ = true;
        } else if (lineChanged) {
            onLyricLineTargetChanged(frame.currentLine, frame.actualPositionMs,
                                     frame.frameRevision, frame.animateTransition);
        } else if (lyricTransitionPending_ || lyricTransitionActive_) {
            // 同一目标行的低频媒体/场景更新不应打断动画，但动画版本要跟随最新完整帧。
            lyricTransitionRevision_ = frame.frameRevision;
        }
        if (statusChanged || sceneChanged)
            textDirty_ = true;

        if (frame.visible) {
            if (!visible) {
                visible = true;
                if (hwnd)
                    ShowWindow(hwnd, SW_SHOWNA);
                startFrameTimer();
            }
        } else if (visible) {
            visible = false;
            stopFrameTimer();
            if (hwnd)
                ShowWindow(hwnd, SW_HIDE);
        }

        if (visible && (wasVisible != visible || mediaChanged || lyricsChanged || lineChanged ||
                        statusChanged || sceneChanged))
            render();
    }

    // 频谱簇总宽（含柱间间隙）
    float spectrumClusterW() const {
        return TaskbarHost::kSpectrumBands * kSpectrumBarW +
               (TaskbarHost::kSpectrumBands - 1) * kSpectrumGap;
    }

    // 频谱开启时窗口整体加宽的宽度：频谱簇 + 与歌词区间的间距
    float spectrumExtraW() const {
        return spectrumVisible_ ? spectrumClusterW() + kTextPadding : 0.0f;
    }

    // 频谱柱：x 起的一簇圆角柱，垂直方向以中线对称伸缩（位于歌词右侧，与左侧封面/歌曲信息对称）
    void drawSpectrum(float x, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushSpectrum_)
            return;
        constexpr int n = TaskbarHost::kSpectrumBands;
        float cy = h * 0.5f;
        float maxH = h * 0.74f;
        float minH = 4.0f; // 静音时也保留小圆角柱，不消失
        for (int i = 0; i < n; ++i) {
            float bh = minH + spectrumBands_[i] * (maxH - minH);
            D2D1_ROUNDED_RECT rr{
                D2D1::RectF(x, cy - bh * 0.5f, x + kSpectrumBarW, cy + bh * 0.5f),
                kSpectrumBarW * 0.5f, kSpectrumBarW * 0.5f};
            rt->FillRoundedRectangle(rr, brushSpectrum_);
            x += kSpectrumBarW + kSpectrumGap;
        }
    }

    void drawVinylCover(float coverX, float coverY, float s) {
        auto* rt = renderer.renderTarget();
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
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(vinylAngleDeg_, center));
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
            rt->PushLayer(D2D1::LayerParameters(
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
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void drawPlatformIcon(float coverX, float coverY, float s) {
        auto* rt = renderer.renderTarget();
        if (!rt || !platformIconVisible_ || !platformIconBmp || s <= 0.0f)
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
        make(fontSize_ * kLyricMainFontScale, DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtLyric_);
        // 双行模式的下一行：比翻译字号（0.78）略大，但明显小于核心歌词（1.18）。
        make(fontSize_ * kLyricPreviewFontScale, DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtNextLyric_);
        make(fontSize_ * 0.78f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
             &fmtSecondary_);
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
        r(fmtNextLyric_);
        r(fmtSecondary_);
        r(titleLayout_);
        r(artistLayout_);
        r(lyricLayout_);
        r(nextLyricLayout_);
        r(secondaryLayout_);
        r(outgoingLyricLayout_);
        r(outgoingSecondaryLayout_);
        r(brushBg_);
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
        r(coverClip_);
        r(vinylCoverClip_);
        r(coverLayer_);
        r(icoPlay_);
        r(icoPrev_);
        r(icoNext_);
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
        }
        if (platformIconBmp) {
            platformIconBmp->Release();
            platformIconBmp = nullptr;
        }
        renderer.discard();
        textDirty_ = true;
        geomDirty_ = true;
        layoutDirty_ = true;
        coverDirty = true;
        platformIconDirty = true;
    }

    void releaseAll() {
        discardDeviceResources();
        renderer.releaseAll();
    }

    // ---------- 避让探测工作线程 ----------

    // 每秒探测一次：TrafficMonitor 窗口矩形 + UIA 任务栏按钮矩形。
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
                delete probeOut_.exchange(p); // 上一份未被拾取则丢弃
            }
            for (int t = 0; t < 10 && !probeStop_.load(); ++t)
                Sleep(100);
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
        if (probeThread_.joinable())
            probeThread_.join();
        delete probeOut_.exchange(nullptr);
    }

    // UI 线程慢速分支：拾取探测结果，与缓存比较有变化才更新。返回是否有变化
    bool pickProbeResult() {
        std::unique_ptr<ProbeResult> p(probeOut_.exchange(nullptr));
        if (!p)
            return false;
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
        if (changed) {
            rcTrafficMonitor_ = p->rcTm;
            uiaButtons_ = std::move(p->buttons);
        }
        return changed;
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
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = static_cast<float>(dpi_);
        props.dpiY = static_cast<float>(dpi_);
        hr = rt->CreateBitmap(D2D1::SizeU(w, h), bitmapData.Scan0, bitmapData.Stride, &props,
                                &coverBmp);
        pixels->UnlockBits(&bitmapData);
        stream->Release();
    }

    void decodePlatformIcon() {
        platformIconDirty = false;
        if (platformIconBmp) {
            platformIconBmp->Release();
            platformIconBmp = nullptr;
        }
        if (!platformIconVisible_ || media.sourceAppUserModelId.empty())
            return;

        const std::wstring path = resolveSourceIconPath(media.sourceAppUserModelId);
        if (path.empty())
            return;

        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!readSourceIconPixels(path, pixels, width, height))
            return;

        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = static_cast<float>(dpi_);
        props.dpiY = static_cast<float>(dpi_);
        rt->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4, &props,
                         &platformIconBmp);
    }

    void refreshSecondaryContent() {
        secondaryContentAvailable_ = false;
        for (const auto& line : lines) {
            if ((translationEnabled_ && !line.translation.empty()) ||
                (romanizationEnabled_ && !line.romanization.empty())) {
                secondaryContentAvailable_ = true;
                return;
            }
        }
    }

    bool useDoubleLineLyrics() const {
        return doubleLineLyricsEnabled_ && !secondaryContentAvailable_;
    }

    int displayLyricLine() const {
        if (scene_ == DisplayScene::NoPlayback || scene_ == DisplayScene::Searching ||
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

    // ---------- 行过渡状态 ----------

    void releaseOutgoingLyricLayouts() {
        if (outgoingLyricLayout_) {
            outgoingLyricLayout_->Release();
            outgoingLyricLayout_ = nullptr;
        }
        if (outgoingSecondaryLayout_) {
            outgoingSecondaryLayout_->Release();
            outgoingSecondaryLayout_ = nullptr;
        }
        outgoingLyricBlockHeight_ = 0.0f;
    }

    // 丢弃当前行过渡：清空待启动/进行中状态并释放旧布局，下一次排版直接提交最终行。
    void resetLyricTransition() {
        lyricTransitionPending_ = false;
        lyricTransitionActive_ = false;
        lyricTransitionRevision_ = 0;
        transitionTargetValid_ = false;
        pendingTargetValid_ = false;
        releaseOutgoingLyricLayouts();
    }

    // 行目标变化统一入口：动画进行中只记录最新目标（不重启当前动画），
    // 待启动的过渡直接改打最新目标，空闲时发起新过渡；不允许动画时立即提交最终状态。
    void onLyricLineTargetChanged(int newLine, int64_t actualPositionMs, uint64_t revision,
                                  bool allowAnimate) {
        const int previous = currentLine;
        currentLine = newLine;
        if (!allowAnimate || !clientAnimations_ || previous < 0 || newLine < 0) {
            resetLyricTransition();
            textDirty_ = true;
            return;
        }
        LyricTransitionTarget target{newLine, actualPositionMs,
                                     newLine > previous ? 1 : -1, revision};
        if (lyricTransitionActive_) {
            pendingTarget_ = target;
            pendingTargetValid_ = true;
            // 过渡版本跟随最新帧，避免被 updateScroll 的过期检查丢弃。
            lyricTransitionRevision_ = revision;
            return;
        }
        transitionTarget_ = target;
        transitionTargetValid_ = true;
        lyricTransitionDirection_ = target.direction;
        lyricTransitionPending_ = true;
        lyricTransitionRevision_ = revision;
        textDirty_ = true;
    }

    // 行过渡收尾：先冻结动画再交换状态。释放旧布局、消费动画期间记录的最新目标；
    // 新行逐字进度直接对齐真实播放位置，不再无条件从零追赶。
    void finalizeLyricTransition(ULONGLONG now) {
        releaseOutgoingLyricLayouts();
        lyricTransitionActive_ = false;
        lyricTransitionStartMs_ = 0;

        if (pendingTargetValid_) {
            // 动画期间收到了更新的目标行：当前布局仍是旧目标，以它为旧行立即
            // 发起向最新目标的过渡，不经过稳定的中间帧。
            transitionTarget_ = pendingTarget_;
            transitionTargetValid_ = true;
            pendingTargetValid_ = false;
            lyricTransitionDirection_ = transitionTarget_.direction;
            lyricTransitionPending_ = true;
            lyricTransitionRevision_ = frameRevision_;
            textDirty_ = true;
            // 布局尚未对应新行：解绑逐字平滑状态，排版完成后直接对齐真实目标。
            karaokeSmoothLine_ = -1;
            karaokeTick_ = now;
            return;
        }
        transitionTargetValid_ = false;
        lyricTransitionRevision_ = 0;

        karaokeTick_ = now;
        karaokeSmoothLine_ = currentLine;
        if (const LyricLine* line = karaokeLine()) {
            int64_t charDur = 0;
            const float target = karaokeTargetX(*line, charDur);
            karaokeSmoothX_ = target;
            karaokeProgX_ = target;
        } else {
            karaokeSmoothX_ = 0.0f;
            karaokeProgX_ = 0.0f;
        }
    }

    // ---------- 排版 ----------

    void buildTextLayouts(float leftW, float rightW) {
        textDirty_ = false;
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle_ || !fmtArtist_ || !fmtLyric_)
            return;

        const bool doubleLineLyrics = useDoubleLineLyrics();
        if (titleLayout_) {
            titleLayout_->Release();
            titleLayout_ = nullptr;
        }
        if (artistLayout_) {
            artistLayout_->Release();
            artistLayout_ = nullptr;
        }
        // 准备阶段：先把当前布局移交为旧行，目标行布局构建完成后才记录动画起点，
        // 避免“新布局已替换但动画初始位置还没准备好”导致的文字瞬移。
        bool preparedTransition = false;
        if (lyricTransitionPending_ && lyricLayout_) {
            if (outgoingLyricLayout_)
                outgoingLyricLayout_->Release();
            outgoingLyricLayout_ = lyricLayout_;
            lyricLayout_ = nullptr;
            outgoingLyricWidth_ = lyricWidth_;
            outgoingLyricHeight_ = lyricHeight_;
            outgoingLyricBlockHeight_ =
                doubleLineLyrics
                    ? lyricHeight_ + kLyricPreviewGap +
                          (nextLyricLayout_ ? nextLyricHeight_ : 0.0f)
                    : 0.0f;
            if (outgoingSecondaryLayout_)
                outgoingSecondaryLayout_->Release();
            if (doubleLineLyrics) {
                outgoingSecondaryLayout_ = nullptr;
                if (secondaryLayout_)
                    secondaryLayout_->Release();
                secondaryLayout_ = nullptr;
                outgoingSecondaryWidth_ = 0.0f;
                outgoingSecondaryHeight_ = 0.0f;
            } else {
                outgoingSecondaryLayout_ = secondaryLayout_;
                secondaryLayout_ = nullptr;
                outgoingSecondaryWidth_ = secondaryWidth_;
                outgoingSecondaryHeight_ = secondaryHeight_;
            }
            preparedTransition = true;
        } else {
            resetLyricTransition();
            if (lyricLayout_)
                lyricLayout_->Release();
            if (secondaryLayout_)
                secondaryLayout_->Release();
            lyricLayout_ = nullptr;
            secondaryLayout_ = nullptr;
        }
        lyricTransitionPending_ = false;

        if (nextLyricLayout_) {
            nextLyricLayout_->Release();
            nextLyricLayout_ = nullptr;
        }
        nextLyricWidth_ = 0.0f;
        nextLyricHeight_ = 0.0f;

        float infoX = infoStartX();
        float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
        titleWidth_ = 0.0f;
        titleHeight_ = 0.0f;
        artistWidth_ = 0.0f;
        artistHeight_ = 0.0f;
        if (songInfoVisible_ && !media.title.empty()) {
            dwrite->CreateTextLayout(media.title.c_str(), (UINT32)media.title.size(), fmtTitle_,
                                     100000.0f, 40.0f, &titleLayout_);
            if (titleLayout_) {
                DWRITE_TEXT_METRICS m{};
                titleLayout_->GetMetrics(&m);
                titleWidth_ = m.width;
                titleHeight_ = m.height;
            }
        }
        if (songInfoVisible_ && !media.artist.empty()) {
            dwrite->CreateTextLayout(media.artist.c_str(), (UINT32)media.artist.size(), fmtArtist_,
                                     100000.0f, 40.0f, &artistLayout_);
            if (artistLayout_) {
                DWRITE_TEXT_METRICS m{};
                artistLayout_->GetMetrics(&m);
                artistWidth_ = m.width;
                artistHeight_ = m.height;
            }
        }

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
        if (preparedTransition) {
            if (lyricLayout_) {
                // 目标布局就绪后才启动动画计时：准备布局的这一帧不消耗过渡时长。
                lyricTransitionStartMs_ = GetTickCount64();
                lyricTransitionActive_ = true;
                lyricTransitionRevision_ = frameRevision_;
            } else {
                // 目标行没有可绘制布局：放弃过渡，直接回到稳定状态。
                resetLyricTransition();
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
        bool secondaryChanged = secondary != lastSecondary_;
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
        if (secondaryChanged) {
            secondaryScrollOffset_ = 0.0f;
            lastSecondary_ = secondary;
        }
        if (titleChanged || artistChanged || lyricChanged || secondaryChanged) {
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

    float coverSlotWidth(float heightDip) const {
        return albumCoverVisible_ ? heightDip - kCoverPadding : 0.0f;
    }

    float infoStartX() const {
        return albumCoverVisible_ ? kCoverPadding + coverSize() + kCoverPadding : kTextPadding;
    }

    float lyricStartPadding() const {
        return songInfoVisible_ ? kSongInfoLyricGap : kTextPadding;
    }

    struct LayoutMetrics {
        float w = 0.0f;
        float h = 0.0f;
        float leftW = 0.0f;
        float rightW = 0.0f;
    };

    LayoutMetrics layoutMetrics(int pxW, int pxH) const {
        LayoutMetrics m;
        m.w = dip(pxW);
        m.h = dip(pxH);
        float effW = m.w - spectrumExtraW();
        m.leftW = songInfoVisible_ ? effW * kLeftRatio : coverSlotWidth(m.h);
        m.rightW = m.w - m.leftW;
        return m;
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

    float vinylInnerRadius(float s) const {
        float outerRadius = std::max(1.0f, s * 0.5f - 0.5f);
        float recordRadius = std::max(0.5f, outerRadius -
                                                    std::min(kVinylHaloWidth, outerRadius * 0.25f));
        return std::min(s * kVinylInnerRatio, std::max(1.0f, recordRadius - 1.0f));
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
        if (vinylCoverClip_) {
            vinylCoverClip_->Release();
            vinylCoverClip_ = nullptr;
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
        D2D1_ELLIPSE coverEllipse{
            D2D1::Point2F(s * 0.5f, s * 0.5f), vinylInnerRadius(s), vinylInnerRadius(s)};
        d2d->CreateEllipseGeometry(coverEllipse, &vinylCoverClip_);

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
        if (!controlsOnHover_ || !mouseOver_)
            return -1;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        // 鼠标消息使用像素坐标，而 render 使用 DIP；先统一到 DIP，
        // 并与 render 使用完全相同的左侧分区计算。
        LayoutMetrics layout = layoutMetrics(rc.right - rc.left, rc.bottom - rc.top);
        float w = layout.w;
        float h = layout.h;
        x = dip(static_cast<int>(x));
        y = dip(static_cast<int>(y));
        float leftW = layout.leftW;
        if (x < leftW || x > w)
            return -1;

        float cx = leftW + layout.rightW * 0.5f;
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

    // 绘制可滚动文本：容得下则按 alignment 对齐，容不下则向左无缝滚动。
    // outline/glow 非空时先画 8 方向光晕层和深色描边层，再画主文字。
    // karaokeBrush 非空时启用逐字高亮：整行先用 brush（未播放色）画一遍，
    // 再按像素裁剪出 [文本起点, karaokeX] 区域用 karaokeBrush（已播放色）画第二遍，
    // 实现字内平滑填充（裁剪基于像素，不受滚动偏移影响）
    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                           ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                           float opacity = 1.0f,
                           LyricAlignment alignment = LyricAlignment::Center) {
        auto* rt = renderer.renderTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        if (opacity >= 0.999f)
            opacity = 1.0f;
        ID2D1Brush* brushes[4] = {brush, outline, glow, karaokeBrush};
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
        auto alignedBase = [&]() {
            float freeW = std::max(0.0f, areaW - textW);
            switch (alignment) {
            case LyricAlignment::Left:
                return x;
            case LyricAlignment::Right:
                return x + freeW;
            case LyricAlignment::Center:
            default:
                return x + freeW * 0.5f;
            }
        };
        if (karaokeBrush) {
            bases[n++] = (textW <= areaW) ? alignedBase() : x - offset;
        } else if (textW <= areaW) {
            bases[n++] = alignedBase();
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
        for (int i = 0; i < changedCount; ++i)
            changed[i]->SetOpacity(previousOpacity[i]);
    }

    void drawLyricScrollingText(IDWriteTextLayout* layout, float textW, float textH,
                                float areaW, float x, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                                float opacity = 1.0f) {
        drawScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                          karaokeBrush, karaokeX, opacity, lyricAlignment_);
    }

    void drawScaledScrollingText(IDWriteTextLayout* layout, float textW, float textH,
                                 float areaW, float x, float y, float offset, ID2D1Brush* brush,
                                 float opacity, float scale) {
        auto* rt = renderer.renderTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        if (scale >= 0.999f) {
            drawLyricScrollingText(layout, textW, textH, areaW, x, y, offset, brush, nullptr,
                                   nullptr, nullptr, 0.0f, opacity);
            return;
        }

        float anchorX = x + areaW * 0.5f;
        if (lyricAlignment_ == LyricAlignment::Left)
            anchorX = x;
        else if (lyricAlignment_ == LyricAlignment::Right)
            anchorX = x + areaW;
        const D2D1_POINT_2F anchor = D2D1::Point2F(anchorX, y + textH * 0.5f);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, anchor));
        drawLyricScrollingText(layout, textW, textH, areaW, x, y, offset, brush, nullptr, nullptr,
                               nullptr, 0.0f, opacity);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void drawDoubleLineLyrics(float lyricAreaX, float lyricAreaW, float h) {
        if (!lyricLayout_)
            return;

        const float lyricBlockH = lyricHeight_ +
                                  (nextLyricLayout_
                                       ? kLyricPreviewGap + nextLyricHeight_
                                       : 0.0f);
        const float coreY = h * 0.5f - lyricBlockH * 0.5f;
        ID2D1Brush* coreBrush = brushLyric_ ? static_cast<ID2D1Brush*>(brushLyric_)
                                           : static_cast<ID2D1Brush*>(brushText_);
        ID2D1Brush* previewBrush = brushLyricDim_ ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                                 : static_cast<ID2D1Brush*>(brushDim_);
        if (lyricTransitionActive_ && outgoingLyricLayout_) {
            float transitionT = std::clamp(
                static_cast<float>((frameNowMs_ ? frameNowMs_ : GetTickCount64()) -
                                   lyricTransitionStartMs_) /
                    kLyricTransitionMs,
                0.0f, 1.0f);
            float movementT = transitionT * transitionT * (3.0f - 2.0f * transitionT);
            auto smoothProgress = [](float t, float start, float end) {
                float p = std::clamp((t - start) / (end - start), 0.0f, 1.0f);
                return p * p * (3.0f - 2.0f * p);
            };
            float fadeOutT = smoothProgress(transitionT, 0.08f, 0.90f);
            float fadeInT = smoothProgress(transitionT, 0.14f, 1.0f);
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
            float incomingScale = kLyricPreviewScale +
                                  (1.0f - kLyricPreviewScale) * movementT;
            // y 是主字号布局的位置；缩小时校正半个缩放差，使视觉中心跟随位移而不跳动。
            float scaledIncomingY =
                incomingY - lyricHeight_ * (1.0f - incomingScale) * 0.5f;

            // 下一行在转场前已经位于核心行下方；转场从这个位置接入核心，避免跳变。
            drawLyricScrollingText(outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_,
                                   lyricAreaW, lyricAreaX, outgoingY + oldShift, 0.0f, coreBrush,
                                   nullptr, nullptr, nullptr, 0.0f, 1.0f - fadeOutT);
            drawScaledScrollingText(
                lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX, scaledIncomingY,
                lyricScrollOffset_, incomingBrush,
                kLyricPreviewOpacity + (1.0f - kLyricPreviewOpacity) * fadeInT, incomingScale);
            return;
        }

        // 核心行仍保留原有逐字高亮；下一行只作为低透明度预览，不参与逐字填充。
        const LyricLine* curLine = karaokeLine();
        bool karaoke = curLine && brushLyric_ && brushLyricDim_;
        float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
        drawLyricScrollingText(lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX,
                               coreY, lyricScrollOffset_,
                               karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_) : coreBrush,
                               lyricOutline_ ? brushLyricOutline_ : nullptr,
                               lyricGlow_ ? brushLyricGlow_ : nullptr,
                               karaoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr, progX);

        if (nextLyricLayout_) {
            float nextY = coreY + lyricHeight_ + kLyricPreviewGap;
            drawLyricScrollingText(nextLyricLayout_, nextLyricWidth_, nextLyricHeight_, lyricAreaW,
                                   lyricAreaX, nextY, 0.0f, previewBrush, nullptr, nullptr, nullptr,
                                   0.0f, kLyricPreviewOpacity);
        }
    }

    void render() {
        if (!visible || !hwnd)
            return;
        createDeviceResources();

        // 先调整窗口，再读取客户区并绑定位图，避免用旧尺寸的位图提交后
        // 把刚刚收缩/扩展的窗口尺寸恢复回去。
        if (layoutDirty_) {
            adjustPosition();
            layoutDirty_ = false;
        }

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

        ensureGeometry();
        if (coverDirty)
            decodeCover();
        if (platformIconDirty)
            decodePlatformIcon();

        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        float w = layout.w;
        float h = layout.h;
        float leftW = layout.leftW;
        float rightW = layout.rightW;

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
        if (albumCoverVisible_) {
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(coverX, coverY, s);
            } else {
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
            }
            // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
            drawPlatformIcon(coverX, coverY, s);
        }

        if (songInfoVisible_) {
            // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
            float infoX = infoStartX();
            float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
            float infoGap = 2.0f;
            float totalInfoH = titleHeight_ + infoGap + artistHeight_;
            float infoY = (h - totalInfoH) * 0.5f;
            drawScrollingText(titleLayout_, titleWidth_, titleHeight_, infoW, infoX, infoY,
                              titleScrollOffset_, brushText_);
            drawScrollingText(artistLayout_, artistWidth_, artistHeight_, infoW, infoX,
                              infoY + titleHeight_ + infoGap, artistScrollOffset_, brushDim_);
        }

        // 右侧歌词区：未悬浮时滚动显示歌词，悬浮时显示控制按钮
        float lyricStart = lyricStartPadding();
        float lyricAreaX = leftW + lyricStart;
        float lyricAreaW =
            std::max(1.0f, rightW - lyricStart - kTextPadding - spectrumExtraW());
        float secondaryGap = secondaryLayout_ ? 1.0f : 0.0f;
        float lyricBlockH = lyricHeight_ + secondaryGap + secondaryHeight_;
        float lyricY = h * 0.5f - lyricBlockH * 0.5f;

        // 只有开启悬浮控件且鼠标位于窗口内时才替换歌词，否则保持歌词/频谱视图。
        bool showControls = mouseOver_ && controlsOnHover_;
        // 频谱独占歌词区右端（窗口整体加宽，歌词宽度不变，见 adjustPosition）
        bool showSpectrum = spectrumVisible_ && !showControls;

        if (showControls) {
            float cx = leftW + rightW * 0.5f;
            float cy = h * 0.5f;
            float r = h * 0.26f;
            float spacing = r * 2.8f;
            for (int i = 0; i < 3; ++i)
                drawButton(i, D2D1::Point2F(cx + (i - 1) * spacing, cy), r);
        } else {
            if (showSpectrum)
                drawSpectrum(lyricAreaX + lyricAreaW + kTextPadding, h);
            if (useDoubleLineLyrics()) {
                drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h);
            } else {
                float transitionT = 1.0f;
                if (lyricTransitionActive_) {
                    transitionT = std::clamp(
                        (static_cast<float>((frameNowMs_ ? frameNowMs_ : GetTickCount64()) -
                                            lyricTransitionStartMs_) /
                         kLyricTransitionMs),
                        0.0f, 1.0f);
                }
                if (lyricTransitionActive_ && outgoingLyricLayout_) {
                    // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                    float movementT = transitionT * transitionT * (3.0f - 2.0f * transitionT);
                    // 淡出稍晚开始并留出余韵；新行略早进入，避免画面突然变空。
                    auto smoothProgress = [](float t, float start, float end) {
                        float p = std::clamp((t - start) / (end - start), 0.0f, 1.0f);
                        return p * p * (3.0f - 2.0f * p);
                    };
                    float fadeOutT = smoothProgress(transitionT, 0.08f, 0.90f);
                    float fadeInT = smoothProgress(transitionT, 0.14f, 1.0f);
                    const LyricLine* incomingLine = karaokeLine();
                    bool incomingKaraoke = incomingLine && brushLyric_ && brushLyricDim_;
                    ID2D1Brush* incomingBrush =
                        incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_)
                                        : static_cast<ID2D1Brush*>(brushLyric_ ? brushLyric_
                                                                                : brushText_);
                    float outgoingGap = outgoingSecondaryLayout_ ? 1.0f : 0.0f;
                    float outgoingBlockH = outgoingLyricHeight_ + outgoingGap +
                                           outgoingSecondaryHeight_;
                    float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                    float travel = std::max(lyricBlockH, outgoingBlockH);
                    float oldShift = -static_cast<float>(lyricTransitionDirection_) * travel *
                                     movementT;
                    float newShift = static_cast<float>(lyricTransitionDirection_) * travel *
                                     (1.0f - movementT);
                    drawLyricScrollingText(
                        outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_, lyricAreaW,
                        lyricAreaX, outgoingY + oldShift, 0.0f,
                        brushLyric_ ? brushLyric_ : brushText_, nullptr, nullptr, nullptr, 0.0f,
                        1.0f - fadeOutT);
                    if (outgoingSecondaryLayout_)
                        drawLyricScrollingText(outgoingSecondaryLayout_, outgoingSecondaryWidth_,
                                               outgoingSecondaryHeight_, lyricAreaW, lyricAreaX,
                                               outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                                               0.0f, brushDim_, nullptr, nullptr, nullptr, 0.0f,
                                               1.0f - fadeOutT);
                    // 过渡期间停用逐字填充，避免高亮进度和移动中的旧行叠加造成视觉噪声。
                    drawLyricScrollingText(lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW,
                                           lyricAreaX, lyricY + newShift, lyricScrollOffset_,
                                           incomingBrush, nullptr, nullptr,
                                           nullptr, 0.0f, fadeInT);
                    if (secondaryLayout_)
                        drawLyricScrollingText(secondaryLayout_, secondaryWidth_, secondaryHeight_,
                                               lyricAreaW, lyricAreaX,
                                               lyricY + lyricHeight_ + secondaryGap + newShift,
                                               secondaryScrollOffset_, brushDim_, nullptr, nullptr,
                                               nullptr, 0.0f, fadeInT);
                } else {
                    // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                    // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                    const LyricLine* curLine = karaokeLine();
                    bool karaoke = curLine && brushLyric_ && brushLyricDim_;
                    float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                    drawLyricScrollingText(
                        lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX, lyricY,
                        lyricScrollOffset_,
                        karaoke ? brushLyricDim_
                                : (brushLyric_ ? brushLyric_ : brushText_),
                        lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricGlow_ ? brushLyricGlow_ : nullptr,
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
        frameNowMs_ = now;
        if (lastTickMs_ == 0)
            lastTickMs_ = now;
        float dt = static_cast<float>(now - lastTickMs_) / 1000.0f;
        lastTickMs_ = now;

        if ((lyricTransitionPending_ || lyricTransitionActive_) &&
            lyricTransitionRevision_ != 0 && lyricTransitionRevision_ != frameRevision_) {
            // 不存在历史过渡队列：版本过期时直接丢弃旧过渡，下一次排版只处理当前行。
            resetLyricTransition();
            textDirty_ = true;
        }

        if (lyricTransitionActive_ &&
            now - lyricTransitionStartMs_ >= static_cast<ULONGLONG>(kLyricTransitionMs)) {
            finalizeLyricTransition(now);
        }

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
        LayoutMetrics layout = layoutMetrics(rc.right - rc.left, rc.bottom - rc.top);
        if (layout.h <= 0.0f || layout.w <= 0.0f)
            return;

        float w = layout.w;
        float h = layout.h;
        float leftW = layout.leftW;
        float rightW = layout.rightW;
        float infoX = infoStartX();
        float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
        // 与 render 一致：滚动/跟随范围不含频谱独占区
        float lyricStart = lyricStartPadding();
        float lyricAreaW =
            std::max(1.0f, rightW - lyricStart - kTextPadding - spectrumExtraW());

        if (songInfoVisible_) {
            marquee(titleWidth_, infoW, kInfoScrollSpeed, titleScrollOffset_);
            marquee(artistWidth_, infoW, kInfoScrollSpeed, artistScrollOffset_);
        }
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
        marquee(secondaryWidth_, lyricAreaW, kLyricScrollSpeed, secondaryScrollOffset_);
    }

    void updateVinylRotation() {
        ULONGLONG now = GetTickCount64();
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

    // 帧定时器只在窗口可见时运行：隐藏后每 16ms 的 tick（进度插值、滚动、
    // 快照读取）没有任何可见效果，可见性恢复由 SMTC 事件驱动（不依赖定时器）。
    void startFrameTimer() {
        if (!timerRunning_ && hwnd) {
            SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
            timerRunning_ = true;
        }
    }

    void stopFrameTimer() {
        if (timerRunning_ && hwnd) {
            KillTimer(hwnd, kTimerId);
            timerRunning_ = false;
        }
    }

    void onTimer() {
        if (tick)
            tick();
        frameNowMs_ = GetTickCount64();
        updateVinylRotation();
        // 任务栏位置/DPI/主题跟踪与避让探测结果拾取，放到慢速分支，不跟 60fps 走
        if (++slowTick_ >= kSlowTickInterval) {
            slowTick_ = 0;
            bool changed = detectChanges();
            if (pickProbeResult())
                changed = true;
            if (changed)
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
            // 初始不可见：帧定时器在首次可见时（applyPresentationFrame/show）启动
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
            stopFrameTimer();
            stopProbe();
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
    if (impl_->updateMediaInfo(info))
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
        impl_->startFrameTimer();
    }
    impl_->render();
}

void TaskbarHost::hide() {
    if (impl_->visible) {
        impl_->visible = false;
        impl_->stopFrameTimer();
        ShowWindow(impl_->hwnd, SW_HIDE);
    }
}

void TaskbarHost::setLyrics(const std::vector<LyricLine>& lines) {
    impl_->lines = lines;
    impl_->scene_ = lines.empty() ? DisplayScene::Message : DisplayScene::Lyrics;
    impl_->refreshSecondaryContent();
    impl_->currentLine = -1;
    impl_->resetLyricTransition();
    if (impl_->nextLyricLayout_) {
        impl_->nextLyricLayout_->Release();
        impl_->nextLyricLayout_ = nullptr;
    }
    impl_->nextLyricWidth_ = 0.0f;
    impl_->nextLyricHeight_ = 0.0f;
    impl_->textDirty_ = true;
    if (!lines.empty())
        impl_->statusText.clear();
    impl_->render();
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
    if (!text.empty() && impl_->lines.empty())
        impl_->scene_ = DisplayScene::Message;
    else if (text.empty() && !impl_->lines.empty())
        impl_->scene_ = DisplayScene::Lyrics;
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

void TaskbarHost::setSecondaryLyricMode(bool translation, bool romanization) {
    impl_->setSecondaryLyricMode(translation, romanization);
}

void TaskbarHost::setDoubleLineLyrics(bool on) {
    impl_->setDoubleLineLyrics(on);
}

void TaskbarHost::setLyricAlignment(LyricAlignment alignment) {
    impl_->setLyricAlignment(alignment);
}

void TaskbarHost::setControlsOnHover(bool on) {
    impl_->setControlsOnHover(on);
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

void TaskbarHost::setSpectrumVisible(bool on) {
    impl_->setSpectrumVisible(on);
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
