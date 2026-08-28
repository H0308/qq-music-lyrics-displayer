#include "taskbar_host.h"
#include "logging/runtime_logger.h"
#include "ui/app_icon.h"
#include "fluent_theme.h"
#include "lyric_renderer.h"
#include "media_control_icons.h"
#include "media_popup.h"
#include "platform_icon.h"
#include "volume_popup.h"

#include <d2d1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objbase.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId = 2;
constexpr UINT_PTR kTaskbarAttachTimerId = 3;
constexpr UINT kTimerMs = 16;         // 活动帧最大间隔：60Hz 基准
constexpr UINT kTimerMinMs = 8;       // 活动帧最小间隔：高刷封顶 ~125Hz，避免过度唤醒
constexpr UINT kTimerPausedMs = 33;   // 暂停时 ~30fps：超长文本继续滚动，任务栏合成开销减半
constexpr UINT kTaskbarAttachRetryMs = 250;
constexpr int kSlowTickInterval = 15; // 慢速分支（任务栏位置跟踪等）每 15 帧一次，约 250ms
constexpr wchar_t kWndClassName[] = L"QQMusicLyricTaskbar";

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
constexpr float kSceneTransitionMs = 240.0f; // 每日一言与歌词内容块上下翻页时长
constexpr float kSongTransitionMs = 220.0f; // 切歌时新内容滑入时长
constexpr float kSongTransitionTravelDip = 24.0f; // 切歌时新内容的水平入场距离
constexpr float kLyricPreviewGap = 3.0f; // 普通双行模式的核心行与下一行间距
constexpr float kLyricPreviewOpacity = 0.90f; // 下一行预览透明度
constexpr float kKaraokeScrollFollowMs = 100.0f; // 逐字歌词横向跟随时间常数
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
    UINT timerMs_ = 0; // 当前定时器实际间隔（活动/暂停档位切换用）
    UINT displayRefreshHz_ = 60;

    // 任务栏句柄与子部件
    HWND taskbar_ = nullptr;
    bool taskbarEmbedded_ = false;
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

    // 字体：默认字体族与软件普通窗口一致（fluent::uiFontFamily）
    float fontSize_ = kBaseFontSize;
    std::wstring fontFamily_ = fluent::uiFontFamily();
    LyricFontStyle fontStyle_ = LyricFontStyle::Normal;

    // 媒体信息
    OverlayMediaInfo media;
    IdlePresentation idle;
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
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    MediaPopupTrigger mediaPopupTrigger_ = MediaPopupTrigger::Hover;
    MediaPopupTrigger idleCardTrigger_ = MediaPopupTrigger::Hover;
    bool idleCardTriggerSync_ = true;
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
    ULONGLONG lastTickMs_ = 0;
    int slowTick_ = 0; // 慢速分支计数器

    // 渲染
    DCompRenderer renderer;
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
    IDWriteTextLayout* outgoingNextLyricLayout_ = nullptr;
    float outgoingLyricWidth_ = 0.0f;
    float outgoingLyricHeight_ = 0.0f;
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
    bool lyricTransitionDCompActive_ = false;
    bool lyricTransitionDCompEnd_ = false;
    bool songTransitionPending_ = false;
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
    bool secondaryContentAvailable_ = false;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;
    bool clientAnimations_ = true;
    // 渲染模式：0 正常；1 低渲染（播放中也固定 ~30fps）；2 完全停止（窗口隐藏、
    // 帧定时器停止、GPU 设备释放，数据状态保留在内存）；3 极简（保留歌词刷新率，
    // 仅关闭附加视觉、媒体卡片和切歌弹窗）
    int renderMode_ = 0;
    // 最近一帧的会话可见性：完全停止模式下窗口被强制隐藏且 visible 被清，
    // 恢复时据此判断是否需要立即重新显示
    bool sessionVisible_ = false;
    float vinylAngleDeg_ = 0.0f;
    ULONGLONG vinylTickMs_ = 0;
    // 频谱：画刷随歌词已播放色重建（createLyricBrushes），bands 由 UI 线程每帧写入
    ID2D1SolidColorBrush* brushSpectrum_ = nullptr;
    SpectrumStyle spectrumStyle_ = SpectrumStyle::Default;
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
    ID2D1SolidColorBrush* brushIdleWarm_ = nullptr;
    ID2D1SolidColorBrush* brushIdleCool_ = nullptr;
    ID2D1SolidColorBrush* brushIdleAccent_ = nullptr;
    ID2D1Effect* coverBlurFx_ = nullptr;
    ID2D1Effect* coverScaleFx_ = nullptr;
    ID2D1Bitmap* coverBlurInput_ = nullptr; // 模糊链当前绑定的封面（不持有引用，仅用于比较）
    bool spectrumVisible_ = false;
    std::array<float, TaskbarHost::kSpectrumBands> spectrumBands_{};
    ID2D1RoundedRectangleGeometry* coverClip_ = nullptr;
    ID2D1EllipseGeometry* vinylCoverClip_ = nullptr;
    ID2D1Layer* coverLayer_ = nullptr;
    media_control::Geometry controlGeometry;
    bool textDirty_ = true;
    bool songInfoDirty_ = true; // 标题/歌手布局独立重建，换行不触碰歌曲信息
    bool geomDirty_ = true;
    bool layoutDirty_ = true;
    int lastPxW_ = 0;
    int lastPxH_ = 0;
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
        bool dominantColorChanged =
            info.hasDominantColor != media.hasDominantColor ||
            (info.hasDominantColor && info.dominantColor != media.dominantColor);
        bool durationChanged = info.durationMs != media.durationMs;
        media = info;
        if (thumbChanged)
            coverDirty = true;
        if (platformChanged)
            platformIconDirty = true;
        if (thumbChanged || textChanged)
            vinylAngleDeg_ = 0.0f;
        if (textChanged)
            songInfoDirty_ = true;
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

    void retryTaskbarAttach() {
        if (!hwnd)
            return;
        if (!findTaskbar())
            return;

        if (!attachToTaskbar(hwnd)) {
            // 附着失败时仍保持顶层窗口，但必须使用屏幕坐标，避免落到屏幕顶部。
            adjustPosition();
            return;
        }

        cancelTaskbarAttachRetry();
        adjustPosition();
        if (visible)
            render();
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

        // 内容完全由 DirectComposition visual 提供，不再是分层窗口
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyricTaskbar", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, inst, this);
        if (!h)
            return false;

        hwnd = h;
        app_icon::applyTaskbarIcon(hwnd);
        if (!attachToTaskbar(h))
            scheduleTaskbarAttachRetry();
        if (!mediaPopup.create(inst, hwnd))
            runtime_log::writef(L"[taskbar] media popup creation failed");
        if (!volumePopup_.create(inst))
            runtime_log::writef(L"[taskbar] volume popup creation failed");
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
        if (!songInfoVisible_ && scene_ != DisplayScene::Idle) {
            // 保留原歌词区宽度，只扣除歌曲信息区；左侧压缩为可见的封面区域。
            const float compactLeftDip = albumCoverVisible_ ? coverSlotWidth(dip(pxH)) : 0.0f;
            minWidthDip = kMinWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
            maxWidthDip = kMaxWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
        }
        int minW = (int)std::lround(minWidthDip * scale());
        int maxW = (int)std::lround(maxWidthDip * scale());
        // 仅独立频谱区域需要整体加宽；背景波浪复用内容区，不再占用额外宽度。
        const float spectrumExtra = spectrumExtraW();
        if (spectrumExtra > 0.0f) {
            int extra = (int)std::lround(spectrumExtra * scale());
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

        POINT pt{x, y};
        if (taskbarEmbedded_)
            ScreenToClient(taskbar_, &pt);

        // 把窗口提到任务栏子窗口最前面，避免被其他任务栏子窗口盖住
        SetWindowPos(hwnd, HWND_TOP, pt.x, pt.y, pxW, pxH,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        mediaPopup.setAnchor(hwnd);
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
        ++textFxGen_; // 效果组合变化，不能复用旧的离屏缓存
        render();
    }

    void setFontOutline(bool on) {
        if (lyricOutline_ == on)
            return;
        lyricOutline_ = on;
        ++textFxGen_; // 效果组合变化，不能复用旧的离屏缓存
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

    void setIdleQuoteAlignment(LyricAlignment alignment) {
        if (idleQuoteAlignment_ == alignment)
            return;
        idleQuoteAlignment_ = alignment;
        lyricScrollOffset_ = 0.0f;
        secondaryScrollOffset_ = 0.0f;
        render();
    }

    void setIdleQuoteBackground(IdleQuoteBackground background) {
        if (idleQuoteBackground_ == background)
            return;
        idleQuoteBackground_ = background;
        refreshFrameTimer();
        render();
    }

    void setIdleQuoteBackgroundScope(IdleQuoteBackgroundScope scope) {
        if (idleQuoteBackgroundScope_ == scope)
            return;
        idleQuoteBackgroundScope_ = scope;
        refreshFrameTimer();
        render();
    }

    bool isMinimalMode() const {
        return renderMode_ == static_cast<int>(RenderMode::Minimal);
    }

    bool mediaPopupAvailable(bool sessionVisible) const {
        return sessionVisible && renderMode_ != static_cast<int>(RenderMode::Stopped) &&
               !isMinimalMode();
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
        if (isMinimalMode() || renderMode_ == static_cast<int>(RenderMode::Stopped))
            return false;
        if (scene_ == DisplayScene::Idle)
            return sessionVisible_ && idle.quickStartEnabled;
        if (hoverControlStyle_ == HoverControlStyle::Popup)
            return controlsOnHover_;
        return sessionVisible_ && idle.quickStartEnabled;
    }

    void syncMediaPopupEnabled() {
        const bool enabled = mediaPopupEnabledForScene();
        const bool wasEnabled = mediaPopupEnabled_;
        mediaPopupEnabled_ = enabled;
        mediaPopup.setEnabled(enabled);
        if (enabled && !wasEnabled) {
            // 弹窗样式可能在媒体会话已经存在时才开启；补送当前完整快照，
            // 让弹窗的可用状态和展示类别不依赖下一次 SMTC 事件。
            const bool available = mediaPopupAvailable(sessionVisible_);
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setIdleContent(idle, available);
            mediaPopup.setPresentationMode(scene_, available,
                                            hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.setMedia(media, available);
            mediaPopup.endPresentationUpdate();
        } else if (enabled) {
            const bool available = mediaPopupAvailable(sessionVisible_);
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setPresentationMode(scene_, available,
                                            hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.endPresentationUpdate();
        }
        mediaPopup.setTriggerOnHover(mediaPopupTrigger_ == MediaPopupTrigger::Hover);
        // 内嵌控件没有媒体卡片可跟随，使用每日一言卡片自己的展开方式。
        const bool idleTriggerFollowMedia =
            idleCardTriggerSync_ && hoverControlStyle_ == HoverControlStyle::Popup;
        mediaPopup.setIdleTriggerOnHover(
            (idleTriggerFollowMedia ? mediaPopupTrigger_ : idleCardTrigger_) ==
            MediaPopupTrigger::Hover);
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
        render();
    }

    void setHoverControlStyle(HoverControlStyle style) {
        if (hoverControlStyle_ == style)
            return;
        hoverControlStyle_ = style;
        volumeHover_ = false;
        volumePopup_.hide();
        syncMediaPopupEnabled();
        render();
    }

    void setMediaPopupTrigger(MediaPopupTrigger trigger) {
        if (mediaPopupTrigger_ == trigger)
            return;
        mediaPopupTrigger_ = trigger;
        syncMediaPopupEnabled();
    }

    void setMediaPopupBackground(MediaPopupBackground mode) {
        mediaPopup.setBackgroundMode(mode);
    }

    void setIdleCardBackground(MediaPopupBackground mode) {
        mediaPopup.setIdleBackgroundMode(mode);
    }

    void setIdleCardBackgroundColor(COLORREF color, bool customized) {
        mediaPopup.setIdleBackgroundColor(color, customized);
    }

    void setIdleCardFollowAlbum(bool on) {
        mediaPopup.setIdleFollowAlbumBackground(on);
    }

    void setIdleCardTrigger(bool sync, MediaPopupTrigger trigger) {
        if (idleCardTriggerSync_ == sync && idleCardTrigger_ == trigger)
            return;
        idleCardTriggerSync_ = sync;
        idleCardTrigger_ = trigger;
        syncMediaPopupEnabled();
    }

    void setMediaPopupFollowAlbum(bool on) {
        mediaPopup.setFollowAlbumBackground(on);
    }

    void setMediaPopupAutoTextContrast(bool on) {
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
        render();
    }

    void setSpectrumBackground(bool on) {
        if (spectrumBackground_ == on)
            return;
        const bool wasBackground = backgroundWaveEnabled();
        spectrumBackground_ = on;
        if (spectrumVisible_ && wasBackground != backgroundWaveEnabled())
            adjustPosition();
        render();
    }

    void setSpectrumOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (spectrumOpacityPct_ == next)
            return;
        spectrumOpacityPct_ = next;
        if (backgroundWaveEnabled())
            render();
    }

    // 进度背景实际生效条件：用户开启 && 背景波浪未占用背景 && 当前歌曲有时长
    bool progressBackgroundActive() const {
        return progressBackground_ && !backgroundWaveEnabled() && media.durationMs > 0;
    }

    void setProgressBackground(bool on) {
        if (progressBackground_ == on)
            return;
        progressBackground_ = on;
        render();
    }

    void setProgressBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (progressBackgroundOpacityPct_ == next)
            return;
        progressBackgroundOpacityPct_ = next;
        if (progressBackgroundActive())
            render();
    }

    void setBackground(TaskbarBackground mode) {
        if (background_ == mode)
            return;
        background_ = mode;
        if (mode != TaskbarBackground::CoverBlur)
            releaseCoverBackgroundResources();
        render();
    }

    void setCoverBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (coverBackgroundOpacityPct_ == next)
            return;
        coverBackgroundOpacityPct_ = next;
        if (background_ == TaskbarBackground::CoverBlur)
            render();
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
        songInfoDirty_ = true;
        adjustPosition();
        render();
    }

    void setAlbumCoverVisible(bool on) {
        if (albumCoverVisible_ == on)
            return;
        albumCoverVisible_ = on;
        if (!on) {
            platformIconDirty = true;
            if (platformIconBmp) {
                platformIconBmp->Release();
                platformIconBmp = nullptr;
            }
        } else if (platformIconVisible_) {
            platformIconDirty = true;
        }
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
        vinylTickMs_ = monotonicNowMs();
        geomDirty_ = true;
        if (effect == AlbumCoverEffect::Vinyl)
            createAlbumCoverBrushes();
        else
            releaseAlbumCoverEffectResources();
        render();
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
            mediaPopup.setMedia(media, mediaPopupAvailable(sessionVisible_));
            mediaPopup.setPresentationMode(
                scene_, mediaPopupAvailable(sessionVisible_),
                hoverControlStyle_ == HoverControlStyle::Inline);
        }
        mediaPopup.endPresentationUpdate();
        if (playingChanged) {
            // 悬浮控制按钮的播放/暂停图标随状态变化，直接提交一帧；弹窗自身
            // 的完整展示帧已在上面的批量更新中提交。
            // 这里仅保留宿主歌词的刷新，不再重复触发弹窗绘制。
            render();
        }

        if (patch.currentLine == currentLine)
            return;

        if (lyricTransitionKind_ == LyricTransitionKind::Scene &&
            (lyricTransitionPending_ || lyricTransitionActive_)) {
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
        const bool sceneChanged = frame.scene != scene_;
        const bool idleChanged = frame.idle.sentence != idle.sentence ||
                                 frame.idle.source != idle.source ||
                                 frame.idle.loading != idle.loading ||
                                 frame.idle.showQuote != idle.showQuote ||
                                 frame.idle.copyEnabled != idle.copyEnabled ||
                                 frame.idle.quickStartEnabled != idle.quickStartEnabled ||
                                 frame.idle.apps.size() != idle.apps.size();
        const bool wasVisible = visible;
        const bool shouldAnimateScene =
            visible && frame.visible && clientAnimations_ && !isMinimalMode() &&
            renderMode_ != static_cast<int>(RenderMode::Stopped) &&
            isTaskbarSceneTransition(scene_, frame.scene) && lyricLayout_;
        const bool continueSceneTransition =
            sceneChanged && lyricTransitionKind_ == LyricTransitionKind::Scene &&
            (lyricTransitionPending_ || lyricTransitionActive_) &&
            isTaskbarMediaScene(scene_) && isTaskbarMediaScene(frame.scene);
        const bool mediaIdentityChanged =
            frame.media.title != media.title || frame.media.artist != media.artist ||
            frame.media.sourceAppUserModelId != media.sourceAppUserModelId ||
            frame.media.thumbnail != media.thumbnail;
        const bool confirmedDurationChange = media.durationMs > 0 &&
                                             frame.media.durationMs > 0 &&
                                             media.durationMs != frame.media.durationMs;
        const bool songChanged = trackChanged && !trackKey_.empty() && !frame.trackKey.empty() &&
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
                    outgoingDoubleLine_ = useDoubleLineLyrics();
                    lyricTransitionKind_ = LyricTransitionKind::Scene;
                    lyricTransitionDirection_ = frame.scene == DisplayScene::Lyrics ? 1 : -1;
                    lyricTransitionPending_ = true;
                    lyricTransitionRevision_ = frame.frameRevision;
                }
            }
        }
        scene_ = frame.scene;
        idle = frame.idle;
        if (frame.actualPositionMs != positionMs_ && !media.playing)
            karaokeSettled_ = false; // 暂停中 seek：逐字高亮需要重新收敛
        positionMs_ = frame.actualPositionMs;
        statusText = frame.statusText;

        // 只对已有曲目之间的切换做入场动画；首次显示、同曲刷新和会话关闭保持即时提交。
        songTransitionPending_ = songChanged && frame.visible && !isMinimalMode() &&
                                 renderMode_ != static_cast<int>(RenderMode::Stopped) &&
                                 clientAnimations_;

        const bool sceneTransitionInProgress =
            lyricTransitionKind_ == LyricTransitionKind::Scene &&
            (lyricTransitionPending_ || lyricTransitionActive_);
        if (trackChanged || lyricsChanged) {
            lines = frame.lyrics;
            refreshSecondaryContent();
            currentLine = frame.currentLine;
            if (!sceneTransitionInProgress) {
                resetLyricTransition();
                if (nextLyricLayout_) {
                    nextLyricLayout_->Release();
                    nextLyricLayout_ = nullptr;
                }
                nextLyricWidth_ = 0.0f;
                nextLyricHeight_ = 0.0f;
                textDirty_ = true;
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
        } else if (lyricTransitionPending_ || lyricTransitionActive_) {
            // 同一目标行的低频媒体/场景更新不应打断动画，但动画版本要跟随最新完整帧。
            lyricTransitionRevision_ = frame.frameRevision;
        }
        if (statusChanged || sceneChanged) {
            if (sceneTransitionInProgress && (!sceneChanged || continueSceneTransition)) {
                sceneTransitionNeedsRelayout_ = true;
                lyricTransitionRevision_ = frame.frameRevision;
            } else {
                textDirty_ = true;
            }
        }

        sessionVisible_ = frame.visible;
        if (frame.visible && renderMode_ != static_cast<int>(RenderMode::Stopped)) {
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
        syncMediaPopupEnabled();

        if (visible && (wasVisible != visible || trackChanged || mediaChanged || lyricsChanged ||
                        lineChanged || statusChanged || sceneChanged || idleChanged))
            render();
    }

    // 频谱簇总宽（含柱间间隙）
    float spectrumClusterW() const {
        return TaskbarHost::kSpectrumBands * kSpectrumBarW +
               (TaskbarHost::kSpectrumBands - 1) * kSpectrumGap;
    }

    // 频谱开启时窗口整体加宽的宽度：频谱簇 + 与歌词区间的间距
    float spectrumExtraW() const {
        return spectrumVisible_ && !backgroundWaveEnabled()
                   ? spectrumClusterW() + kTextPadding
                   : 0.0f;
    }

    float spectrumLevel(int index) const {
        return std::clamp(spectrumBands_[index], 0.0f, 1.0f);
    }

    void drawDefaultSpectrum(float x, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushSpectrum_)
            return;
        constexpr int n = TaskbarHost::kSpectrumBands;
        const float cy = h * 0.5f;
        const float maxH = h * 0.74f;
        constexpr float minH = 4.0f; // 静音时也保留小柱，不消失
        for (int i = 0; i < n; ++i) {
            const float bh = minH + spectrumLevel(i) * (maxH - minH);
            D2D1_ROUNDED_RECT rr{
                D2D1::RectF(x, cy - bh * 0.5f, x + kSpectrumBarW, cy + bh * 0.5f),
                kSpectrumBarW * 0.5f, kSpectrumBarW * 0.5f};
            rt->FillRoundedRectangle(rr, brushSpectrum_);
            x += kSpectrumBarW + kSpectrumGap;
        }
    }

    // 新增样式：普通柱状图，柱底贴近歌词窗口下边缘，电平只向上增长。
    void drawBarSpectrum(float x, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushSpectrum_)
            return;
        constexpr int n = TaskbarHost::kSpectrumBands;
        const float baseY = h - kSpectrumBottomPadding;
        const float maxH = h * 0.82f;
        constexpr float minH = 3.0f;
        for (int i = 0; i < n; ++i) {
            const float bh = minH + spectrumLevel(i) * (maxH - minH);
            const D2D1_ROUNDED_RECT bar{
                D2D1::RectF(x, baseY - bh, x + kSpectrumBarW, baseY),
                kSpectrumBarRadius, kSpectrumBarRadius};
            rt->FillRoundedRectangle(bar, brushSpectrum_);
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
        const D2D1_COLOR_F spectrumColor = brushSpectrum_->GetColor();
        const D2D1_GRADIENT_STOP fadeStops[] = {
            {0.00f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b, 0.00f)},
            {0.017f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b,
                                 spectrumColor.a * 0.38f)},
            {0.033f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b,
                                 spectrumColor.a * 0.78f)},
            {0.05f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b,
                                 spectrumColor.a)},
            {0.95f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b,
                                 spectrumColor.a)},
            {0.967f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b,
                                 spectrumColor.a * 0.78f)},
            {0.983f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b,
                                 spectrumColor.a * 0.38f)},
            {1.00f, D2D1::ColorF(spectrumColor.r, spectrumColor.g, spectrumColor.b, 0.00f)},
        };
        ID2D1GradientStopCollection* fadeStopCollection = nullptr;
        ID2D1LinearGradientBrush* fadeBrush = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(fadeStops, _countof(fadeStops),
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

    void drawDreamyWaveSpectrum(float x, float h) {
        drawWaveSpectrum(x, h, spectrumClusterW(), 1.0f);
    }

    void drawBackgroundWaveSpectrum(float x, float h, float width) {
        drawWaveSpectrum(x, h, width,
                         static_cast<float>(spectrumOpacityPct_) / 100.0f);
    }

    // 默认保留原有的中线对称频谱效果；柱状图样式从窗口下边缘向上增长。
    void drawSpectrum(float x, float h) {
        switch (spectrumStyle_) {
        case SpectrumStyle::Bars:
            drawBarSpectrum(x, h);
            break;
        case SpectrumStyle::DreamyWave:
            drawDreamyWaveSpectrum(x, h);
            break;
        case SpectrumStyle::Default:
        default:
            drawDefaultSpectrum(x, h);
            break;
        }
    }

    bool taskbarDynamicBackgroundVisible() const {
        if (scene_ == DisplayScene::NoPlayback ||
            idleQuoteBackground_ == IdleQuoteBackground::None || isMinimalMode())
            return false;

        switch (idleQuoteBackgroundScope_) {
        case IdleQuoteBackgroundScope::DailyQuote:
            return scene_ == DisplayScene::Idle && idle.showQuote;
        case IdleQuoteBackgroundScope::Lyrics:
            return scene_ == DisplayScene::Lyrics;
        case IdleQuoteBackgroundScope::All:
            return (scene_ == DisplayScene::Idle && idle.showQuote) ||
                   scene_ == DisplayScene::Lyrics;
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
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void drawPlatformIcon(float coverX, float coverY, float s) {
        auto* rt = renderer.renderTarget();
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
        runtime_log::writef(L"[taskbar] TaskbarCreated: hwnd=%p alive=%d visible=%d timer=%d",
                            hwnd, (hwnd && IsWindow(hwnd)) ? 1 : 0, visible ? 1 : 0,
                            timerRunning_ ? 1 : 0);
        if (hwnd && IsWindow(hwnd)) {
            if (findTaskbar()) {
                if (!attachToTaskbar(hwnd))
                    scheduleTaskbarAttachRetry();
                else
                    cancelTaskbarAttachRetry();
                adjustPosition();
                if (visible)
                    ShowWindow(hwnd, SW_SHOWNA);
                render();
            } else {
                taskbarEmbedded_ = false;
                scheduleTaskbarAttachRetry();
            }
            return;
        }
        hwnd = nullptr;
        taskbarEmbedded_ = false;
        if (createWindow(inst) && visible) {
            ShowWindow(hwnd, SW_SHOWNA);
            // 旧窗口被系统侧销毁时不一定投递 WM_DESTROY（Explorer 被强杀），
            // timerRunning_ 会残留为 true，但定时器已随旧窗口消失；
            // 必须清掉标志再启动，否则 startFrameTimer 因标志位跳过、新窗口没有定时器
            timerRunning_ = false;
            timerMs_ = 0;
            startFrameTimer();
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
        textDirty_ = true;
        songInfoDirty_ = true;
    }

    void changeFont(float delta) {
        setFont(fontFamily_, fontSize_ + delta, fontStyle_);
    }

    void setFont(const std::wstring& family, float size, LyricFontStyle style) {
        fontFamily_ = family;
        fontSize_ = std::clamp(size, kMinFont, kMaxFont);
        fontStyle_ = style;
        recreateFormats();
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
        r(brushProgressBg_);
        r(brushBackground_);
        r(brushIdleWarm_);
        r(brushIdleCool_);
        r(brushIdleAccent_);
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
        media_control::release(controlGeometry);
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
        }
        if (platformIconBmp) {
            platformIconBmp->Release();
            platformIconBmp = nullptr;
        }
        renderer.discard();
        lyricTransitionDCompActive_ = false;
        lyricTransitionDCompEnd_ = false;
        textDirty_ = true;
        songInfoDirty_ = true;
        geomDirty_ = true;
        layoutDirty_ = true;
        coverDirty = true;
        platformIconDirty = true;
    }

    void refreshTheme() {
        app_icon::applyTaskbarIcon(hwnd);
        const bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        if (light != lightTheme_) {
            lightTheme_ = light;
            discardDeviceResources();
        }
        // 媒体卡片属于普通悬浮窗，使用 Window 主题，而不是任务栏主题。
        mediaPopup.refreshTheme();
        if (visible)
            render();
    }

    void releaseAll() {
        discardDeviceResources();
        renderer.releaseAll();
    }

    // ---------- 避让探测工作线程 ----------

    // 每 3 秒探测一次：TrafficMonitor 窗口矩形 + UIA 任务栏按钮矩形。
    // UIA 属性查询由 explorer 的 UI 线程执行，探测越频繁对任务栏的周期性
    // 打扰越明显；3 秒是避让响应速度与 explorer 负担的折中。
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
            for (int t = 0; t < 30 && !probeStop_.load(); ++t)
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
        platformIconDirty = false;
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
        if (outgoingNextLyricLayout_) {
            outgoingNextLyricLayout_->Release();
            outgoingNextLyricLayout_ = nullptr;
        }
        outgoingLyricWidth_ = 0.0f;
        outgoingLyricHeight_ = 0.0f;
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
        lyricTransitionPending_ = false;
        lyricTransitionActive_ = false;
        lyricTransitionKind_ = LyricTransitionKind::None;
        outgoingScene_ = DisplayScene::NoPlayback;
        outgoingDoubleLine_ = false;
        sceneTransitionNeedsRelayout_ = false;
        lyricTransitionStartMs_ = 0;
        lyricTransitionRevision_ = 0;
        transitionTargetValid_ = false;
        pendingTargetValid_ = false;
        if (lyricTransitionDCompActive_)
            lyricTransitionDCompEnd_ = true;
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
            textDirty_ = true;
            return;
        }
        // 只有相邻自然换行才做空间转场。seek、快速切行或反向跳转不能把多次
        // 280ms 动画串起来，否则画面会长期追不上真实歌词。
        if (std::abs(newLine - previous) > 1) {
            resetLyricTransition();
            textDirty_ = true;
            return;
        }
        LyricTransitionTarget target{newLine, actualPositionMs,
                                     newLine > previous ? 1 : -1, revision};
        if (lyricTransitionActive_) {
            if (target.direction != lyricTransitionDirection_) {
                resetLyricTransition();
                textDirty_ = true;
                return;
            }
            pendingTarget_ = target;
            pendingTargetValid_ = true;
            // 过渡版本跟随最新帧，避免被 updateScroll 的过期检查丢弃。
            lyricTransitionRevision_ = revision;
            return;
        }
        transitionTarget_ = target;
        transitionTargetValid_ = true;
        lyricTransitionKind_ = LyricTransitionKind::Line;
        lyricTransitionDirection_ = target.direction;
        lyricTransitionPending_ = true;
        lyricTransitionRevision_ = revision;
        textDirty_ = true;
    }

    // 行过渡收尾：先冻结动画再交换状态。释放旧布局、消费动画期间记录的最新目标；
    // 自然转场中的新行保留逐字追赶过程，避免收尾时又跳到真实位置。
    void finalizeLyricTransition(ULONGLONG now) {
        const bool sceneTransition = lyricTransitionKind_ == LyricTransitionKind::Scene;
        const bool sceneNeedsRelayout = sceneTransitionNeedsRelayout_;
        if (lyricTransitionDCompActive_)
            lyricTransitionDCompEnd_ = true;
        releaseOutgoingLyricLayouts();
        lyricTransitionActive_ = false;
        lyricTransitionStartMs_ = 0;

        if (sceneTransition) {
            // 类别转场不消费歌词行的 pendingTarget；场景稳定后用最新完整帧
            // 再做一次排版，避免把转场期间收到的歌词行状态写回旧布局。
            lyricTransitionKind_ = LyricTransitionKind::None;
            outgoingScene_ = DisplayScene::NoPlayback;
            outgoingDoubleLine_ = false;
            sceneTransitionNeedsRelayout_ = false;
            transitionTargetValid_ = false;
            pendingTargetValid_ = false;
            lyricTransitionRevision_ = 0;
            if (sceneNeedsRelayout)
                textDirty_ = true;
            return;
        }

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
            karaokeSettled_ = false;
            karaokeTick_ = now;
            karaokeEnteringLine_ = false;
            return;
        }
        transitionTargetValid_ = false;
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
        lyricTransitionKind_ = LyricTransitionKind::None;
        outgoingScene_ = DisplayScene::NoPlayback;
        outgoingDoubleLine_ = false;
    }

    // ---------- 排版 ----------

    void buildTextLayouts(float leftW, float rightW) {
        // 文本布局以超宽无换行创建，度量与区域宽度无关，leftW/rightW 仅保留签名兼容
        (void)leftW;
        (void)rightW;
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle_ || !fmtArtist_ || !fmtLyric_)
            return;

        // 歌曲信息（标题/歌手）独立重建：换行只走下面的歌词分支，
        // 歌曲信息变化也不触碰歌词布局与行过渡状态
        if (songInfoDirty_) {
            songInfoDirty_ = false;
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
        if (!textDirty_)
            return;
        textDirty_ = false;
        ++textFxGen_; // 布局指针重建，离屏缓存全部失效
        karaokeSpans_.clear();
        karaokeGeometryLine_ = -1;
        karaokeGeometryLayout_ = nullptr;

        const bool doubleLineLyrics = useDoubleLineLyrics();
        // 准备阶段：先把当前布局移交为旧行，目标行布局构建完成后才记录动画起点，
        // 避免“新布局已替换但动画初始位置还没准备好”导致的文字瞬移。
        bool preparedTransition = false;
        const bool sceneTransition = lyricTransitionKind_ == LyricTransitionKind::Scene;
        if (lyricTransitionPending_ && lyricLayout_) {
            // 保留旧行离场前的滚动位置。新布局后面会把 lyricScrollOffset_ 重置为 0，
            // 不能让旧的超长歌词因此在转场第一帧跳回开头。
            const float outgoingLyricOffset = lyricScrollOffset_;
            const float outgoingSecondaryOffset = secondaryScrollOffset_;
            if (outgoingLyricLayout_)
                outgoingLyricLayout_->Release();
            outgoingLyricLayout_ = lyricLayout_;
            lyricLayout_ = nullptr;
            outgoingLyricWidth_ = lyricWidth_;
            outgoingLyricHeight_ = lyricHeight_;
            outgoingLyricScrollOffset_ = outgoingLyricOffset;
            if (sceneTransition) {
                // 场景转场需要保留旧场景的完整文本块。这里的 next 布局属于
                // 旧歌词场景，不能等下面清理 nextLyricLayout_ 时一起释放。
                if (outgoingNextLyricLayout_)
                    outgoingNextLyricLayout_->Release();
                outgoingNextLyricLayout_ = nextLyricLayout_;
                nextLyricLayout_ = nullptr;
                outgoingNextLyricWidth_ = nextLyricWidth_;
                outgoingNextLyricHeight_ = nextLyricHeight_;
            } else {
                outgoingLyricBlockHeight_ =
                    doubleLineLyrics
                        ? lyricHeight_ + kLyricPreviewGap +
                              (nextLyricLayout_ ? nextLyricHeight_ : 0.0f)
                        : 0.0f;
            }
            if (outgoingSecondaryLayout_)
                outgoingSecondaryLayout_->Release();
            if (sceneTransition) {
                // 场景转场的旧层必须按旧场景实际拥有的附属文本保存，不能用
                // 目标场景的 doubleLineLyrics 结果覆盖它。
                outgoingSecondaryLayout_ = secondaryLayout_;
                secondaryLayout_ = nullptr;
                outgoingSecondaryWidth_ = secondaryWidth_;
                outgoingSecondaryHeight_ = secondaryHeight_;
                outgoingSecondaryScrollOffset_ = outgoingSecondaryOffset;
                const bool outgoingHasNext = outgoingDoubleLine_ && outgoingNextLyricLayout_;
                const float outgoingPreviewH =
                    outgoingHasNext
                        ? kLyricPreviewGap + outgoingNextLyricHeight_
                        : outgoingSecondaryLayout_ ? 1.0f + outgoingSecondaryHeight_ : 0.0f;
                outgoingLyricBlockHeight_ = outgoingLyricHeight_ + outgoingPreviewH;
            } else if (doubleLineLyrics) {
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
                outgoingSecondaryScrollOffset_ = outgoingSecondaryOffset;
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
        if (isMinimalMode()) {
            // 极简只改变显示策略；歌词行和逐字时间轴仍由上游完整读取，
            // 这里只不创建供逐字绘制使用的几何缓存。
            karaokeSpans_.clear();
            karaokeGeometryLine_ = -1;
            karaokeGeometryLayout_ = nullptr;
        } else {
            buildKaraokeGeometry(displayLine);
        }
        if (preparedTransition) {
            if (lyricLayout_) {
                // 目标布局就绪后才启动动画计时：准备布局的这一帧不消耗过渡时长。
                lyricTransitionStartMs_ = monotonicNowMs();
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
        return songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap
                                                                 : kTextPadding;
    }

    struct LayoutMetrics {
        float w = 0.0f;
        float h = 0.0f;
        float leftW = 0.0f;
        float rightW = 0.0f;
    };

    LayoutMetrics layoutMetricsForScene(DisplayScene scene, int pxW, int pxH) const {
        LayoutMetrics m;
        m.w = dip(pxW);
        m.h = dip(pxH);
        if (scene == DisplayScene::Idle) {
            m.leftW = 0.0f;
            m.rightW = m.w;
            return m;
        }
        float effW = m.w - spectrumExtraW();
        m.leftW = songInfoVisible_ ? effW * kLeftRatio : coverSlotWidth(m.h);
        m.rightW = m.w - m.leftW;
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
        return {layout.leftW + start,
                std::max(1.0f, layout.rightW - start - kTextPadding - spectrumExtraW())};
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
            karaokeEnteringLine_ = lyricTransitionActive_;
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
        if (karaokeSettled_ && !lyricTransitionActive_)
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
            geomDirty_ = true;
    }

    // ---------- 渲染 ----------

    // ---------- 内嵌悬浮控件布局 ----------

    // 上一首/播放/下一首 + 音量按钮整体居中：centers[0..2] 为播放控制，
    // centers[3] 为音量按钮。返回 false 表示当前不在内嵌控件展示状态。
    bool inlineControlsLayout(float centers[4], float& cy, float& r) const {
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        LayoutMetrics layout = layoutMetrics(rc.right - rc.left, rc.bottom - rc.top);
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

    void drawVolumeButton(const D2D1_POINT_2F& c, float r) {
        auto* rt = renderer.renderTarget();
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
        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r))
            return false;
        x = dip(static_cast<int>(x));
        y = dip(static_cast<int>(y));
        return std::hypot(x - centers[3], y - cy) <= r + 4.0f;
    }

    // 音量按钮的屏幕坐标矩形（音量滑块浮窗的锚点）
    RECT volumeButtonScreenRect() const {
        float centers[4]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            return rc;
        }
        const float s = scale();
        POINT pt{static_cast<LONG>(std::lround(centers[3] * s)),
                 static_cast<LONG>(std::lround(cy * s))};
        ClientToScreen(hwnd, &pt);
        const int half = static_cast<int>(std::lround((r + 6.0f) * s));
        return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
    }

    void drawButton(int idx, const D2D1_POINT_2F& c, float r) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        bool enabled = idx == 0 ? media.canPrev : idx == 1 ? media.canPlayPause : media.canNext;
        ID2D1SolidColorBrush* brush = enabled ? brushBtn_ : brushBtnDisabled_;

        media_control::draw(rt, controlGeometry, idx, media.playing, c, r, brush);
    }

    int hitButton(float x, float y) const {
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return -1;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        // 鼠标消息使用像素坐标，而 render 使用 DIP；先统一到 DIP，
        // 并与 render 使用完全相同的左侧分区计算。
        LayoutMetrics layout = layoutMetrics(rc.right - rc.left, rc.bottom - rc.top);
        float w = layout.w;
        x = dip(static_cast<int>(x));
        y = dip(static_cast<int>(y));
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
        if (!lyricTransitionActive_ || lyricTransitionStartMs_ == 0)
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
        auto* rt = renderer.renderTarget();
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

    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                           ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                           float opacity = 1.0f,
                           LyricAlignment alignment = LyricAlignment::Center,
                           bool singleCopy = false) {
        auto* rt = renderer.renderTarget();
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
        rt->PopAxisAlignedClip();
        for (int i = 0; i < changedCount; ++i)
            changed[i]->SetOpacity(previousOpacity[i]);
        if (fxBmp)
            fxBmp->Release();
    }

    LyricAlignment activeLyricAlignment() const {
        return scene_ == DisplayScene::Idle ? idleQuoteAlignment_ : lyricAlignment_;
    }

    LyricAlignment lyricAlignmentForScene(DisplayScene scene) const {
        return scene == DisplayScene::Idle ? idleQuoteAlignment_ : lyricAlignment_;
    }

    void drawLyricScrollingTextAligned(
        IDWriteTextLayout* layout, float textW, float textH, float areaW, float x, float y,
        float offset, ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow,
        ID2D1Brush* karaokeBrush, float karaokeX, float opacity, LyricAlignment alignment,
        bool singleCopy = false) {
        drawScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                          karaokeBrush, karaokeX, opacity, alignment, singleCopy);
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
                                 float karaokeX = 0.0f, bool singleCopy = false) {
        auto* rt = renderer.renderTarget();
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
        const D2D1_POINT_2F anchor = D2D1::Point2F(anchorX, y + textH * 0.5f);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, anchor));
        drawLyricScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                                karaokeBrush, karaokeX, opacity, singleCopy);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
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
        if (lyricTransitionActive_ && outgoingLyricLayout_) {
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

            // 下一行在转场前已经位于核心行下方；转场从这个位置接入核心，避免跳变。
            drawLyricScrollingText(outgoingLyricLayout_, outgoingLyricWidth_, outgoingLyricHeight_,
                                   lyricAreaW, lyricAreaX, outgoingY + oldShift,
                                   outgoingLyricScrollOffset_, coreBrush,
                                   effectOutline, effectGlow, nullptr, 0.0f,
                                   1.0f - transition.fadeOut);
            drawScaledScrollingText(
                lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX, scaledIncomingY,
                lyricScrollOffset_, incomingBrush,
                kLyricPreviewOpacity + (1.0f - kLyricPreviewOpacity) * transition.fadeIn,
                incomingScale,
                effectOutline, effectGlow,
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_) : nullptr,
                incomingProgX, true);
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

    // 每日一言与歌词属于任务栏的两个展示场景。场景切换时只移动内容层，
    // 外层背景和窗口保持不动；这样不会把窗口定位变化误认为内容翻页动画。
    void drawSceneTransition(float w, float lyricAreaX, float lyricAreaW, float h,
                             float lyricBlockH) {
        auto* rt = renderer.renderTarget();
        if (!rt || !lyricLayout_ || !outgoingLyricLayout_ || lastPxW_ <= 0 || lastPxH_ <= 0)
            return;

        const LyricArea outgoingArea = lyricAreaForScene(outgoingScene_, lastPxW_, lastPxH_);
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
        auto alignedBase = [&]() {
            float freeW = std::max(0.0f, areaW - textW);
            switch (activeLyricAlignment()) {
            case LyricAlignment::Left:
                return x;
            case LyricAlignment::Right:
                return x + freeW;
            case LyricAlignment::Center:
            default:
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
        if (useDoubleLineLyrics() || !lyricTransitionActive_ || !outgoingLyricLayout_ ||
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
        lyricTransitionDCompActive_ = true;
        lyricTransitionDCompEnd_ = false;
        return true;
    }

    void syncLyricTransitionDComp(bool showControls, float lyricAreaX, float lyricAreaW, float h,
                                  float lyricBlockH) {
        // 图层增删不单独 Commit：改动挂起到 render() 末尾 present() 的 Commit，与承载
        // 歌词的底层新帧同批上屏，避免「图层已撤、底层新帧未上屏」的空窗闪烁。
        if (lyricTransitionDCompEnd_ || (showControls && lyricTransitionDCompActive_) ||
            (lyricTransitionDCompActive_ && karaokeLine())) {
            renderer.clearLyricTransitionLayers();
            lyricTransitionDCompActive_ = false;
            lyricTransitionDCompEnd_ = false;
        }
        if (showControls)
            return;
        // 逐字高亮需要每帧跟随真实播放位置；DComp 快照是静态位图，不能在入场期间
        // 更新填充边界，因此逐字歌词转场保留 D2D 路径，普通歌词仍使用合成器动画。
        if (lyricTransitionKind_ != LyricTransitionKind::Scene &&
            !lyricTransitionDCompActive_ && lyricTransitionActive_ && !useDoubleLineLyrics() &&
            !karaokeLine() && outgoingLyricLayout_ && lyricLayout_) {
            if (!prepareLyricTransitionDComp(lyricAreaX, lyricAreaW, h, lyricBlockH)) {
                renderer.clearLyricTransitionLayers();
                lyricTransitionDCompActive_ = false;
            }
        }
    }

    void render() {
        if (!visible || !hwnd)
            return;

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
            if (lyricTransitionDCompActive_)
                lyricTransitionDCompEnd_ = true;
            lastPxW_ = pxW;
            lastPxH_ = pxH;
            geomDirty_ = true;
            // 文本布局以超宽无换行创建，度量与窗口尺寸无关，无需重建
        }

        if (!renderer.bind(hwnd, pxW, pxH))
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);
        // DComp 设备上下文在首次 bind 后才存在，画刷/图层创建必须排在 bind 之后
        createDeviceResources();

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

        if (textDirty_ || songInfoDirty_)
            buildTextLayouts(leftW, rightW);

        float lyricStart = lyricStartPadding();
        float lyricAreaX = leftW + lyricStart;
        float lyricAreaW =
            std::max(1.0f, rightW - lyricStart - kTextPadding - spectrumExtraW());
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
                            !(lyricTransitionKind_ == LyricTransitionKind::Scene &&
                              lyricTransitionActive_);
        const bool backgroundSpectrum =
            !idleScene && spectrumVisible_ && backgroundWaveEnabled();
        // 独立频谱占用歌词区右端；背景波浪不改变窗口宽度和歌词布局。
        bool showSpectrum = !idleScene && spectrumVisible_ && !showControls && !backgroundSpectrum;
        syncLyricTransitionDComp(showControls, lyricAreaX, lyricAreaW, h, lyricBlockH);

        // 模糊效果链涉及资源创建（CreateEffect），与画刷一样放在 BeginDraw 之前
        ID2D1Effect* coverBlurChain = background_ == TaskbarBackground::CoverBlur && coverBmp
                                          ? ensureCoverBlurChain(w, h)
                                          : nullptr;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        // 背景：alpha 极低，视觉上透明但保证整个窗口可命中
        D2D1_ROUNDED_RECT bg{D2D1::RectF(0.0f, 0.0f, w, h), kCornerRadius, kCornerRadius};

        // 封面模糊背景：模糊后的封面按不透明度铺满全窗，叠加主题遮罩保证文字可读；
        // 纯色背景：跟随任务栏深浅色。画在最底层，进度背景/背景波浪叠加其上
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

        rt->FillRoundedRectangle(bg, brushBg_);
        // 悬浮反馈：鼠标位于歌词区域时叠加一层柔和的圆角底色
        if (mouseOver_ && brushHover_)
            rt->FillRoundedRectangle(bg, brushHover_);

        // 播放进度背景：从窗口左缘按进度填充到歌词右缘（lyricAreaW 已排除独立频谱
        // 占宽，天然不会延伸进频谱容器）；与背景波浪互斥，画在封面/文字之下
        if (progressBackgroundActive() && brushProgressBg_) {
            const float progressRight = lyricAreaX + lyricAreaW;
            const float fraction = static_cast<float>(std::clamp(
                static_cast<double>(positionMs_) / static_cast<double>(media.durationMs), 0.0,
                1.0));
            const float fillRight = progressRight * fraction;
            if (fillRight > 0.5f) {
                const COLORREF c = media.hasDominantColor ? media.dominantColor
                                                          : fluent::accentColor();
                brushProgressBg_->SetColor(
                    fluent::toD2D(c, progressBackgroundOpacityPct_ / 100.0f));
                const float radius =
                    std::min({kCornerRadius, fillRight * 0.5f, h * 0.5f});
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, fillRight, h), radius, radius),
                    brushProgressBg_);
            }
        }

        // 动态背景先于背景波浪绘制，避免覆盖频谱背景层。
        const float dynamicBackgroundW = showSpectrum
                                              ? std::max(1.0f, lyricAreaX + lyricAreaW)
                                              : w;
        if (taskbarDynamicBackgroundVisible())
            drawIdleQuoteBackground(w, h, dynamicBackgroundW);

        if (backgroundSpectrum) {
            const float waveX = infoStartX();
            const float waveW = std::max(1.0f, w - waveX - kTextPadding);
            drawBackgroundWaveSpectrum(waveX, h, waveW);
        }

        // 左侧封面
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
            if (showSpectrum)
                drawSpectrum(lyricAreaX + lyricAreaW + kTextPadding, h);
            if (lyricTransitionKind_ == LyricTransitionKind::Scene && lyricTransitionActive_ &&
                outgoingLyricLayout_) {
                drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
            } else if (useDoubleLineLyrics()) {
                drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
            } else if (!lyricTransitionDCompActive_) {
                if (lyricTransitionActive_ && outgoingLyricLayout_) {
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
                        lyricAreaX, outgoingY + oldShift, outgoingLyricScrollOffset_,
                        primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_ : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_ : nullptr,
                        nullptr, 0.0f,
                        1.0f - transition.fadeOut);
                    if (outgoingSecondaryLayout_)
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

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardDeviceResources();
            return;
        }
        if (SUCCEEDED(hr)) {
            if (songTransitionPending_) {
                songTransitionPending_ = false;
                renderer.resetRoot();
                if (!isMinimalMode()) {
                    const float travel = kSongTransitionTravelDip * scale();
                    if (!renderer.animateRoot(travel, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                              kSongTransitionMs / 1000.0f))
                        renderer.resetRoot();
                }
            }
            // Present 失败（设备丢失/重置）时丢弃设备链，下一帧惰性重建
            if (!renderer.present())
                discardDeviceResources();
        } else {
            runtime_log::writef(L"[taskbar] EndDraw failed: 0x%08X", hr);
        }
    }

    // ---------- 滚动字幕 ----------

    void updateScroll() {
        ULONGLONG now = monotonicNowMs();
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

        const bool wasTransitioning = lyricTransitionPending_ || lyricTransitionActive_;
        if (lyricTransitionActive_ &&
            now - lyricTransitionStartMs_ >=
                static_cast<ULONGLONG>(lyricTransitionDurationMs())) {
            finalizeLyricTransition(now);
        }

        // 静止跳帧判定：本帧有任何滚动/转场在动就置 animating，供 onTimer 决定是否重绘
        bool animating = wasTransitioning;
        auto marquee = [&](float textW, float areaW, float speed, float& offset) {
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
            float loopW = textW + kTextPadding * 2.0f;
            offset = std::fmod(offset + speed * std::max(dt, 0.0f), loopW);
            if (offset < 0.0f)
                offset += loopW;
            animating = true;
        };

        RECT rc{};
        GetClientRect(hwnd, &rc);
        LayoutMetrics layout = layoutMetrics(rc.right - rc.left, rc.bottom - rc.top);
        if (layout.h <= 0.0f || layout.w <= 0.0f) {
            scrollAnimating_ = animating;
            return;
        }

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
        const bool holdLyricScroll = lyricTransitionPending_ || lyricTransitionActive_;
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
            marquee(lyricWidth_, lyricAreaW,
                    idleMarquee ? kInfoScrollSpeed : lyricScrollSpeed_, lyricScrollOffset_);
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
        if (renderMode_ == static_cast<int>(RenderMode::Low))
            return kTimerPausedMs;
        const UINT hz = displayRefreshHz_ ? displayRefreshHz_ : 60;
        return std::clamp(1000 / hz, kTimerMinMs, kTimerMs);
    }

    bool hasHighFrequencyAnimation() const {
        if (lyricTransitionPending_ || lyricTransitionActive_)
            return true;
        if (taskbarDynamicBackgroundAnimating())
            return true;
        if (media.playing && scrollAnimating_)
            return true;
        if (media.playing && clientAnimations_ && karaokeLine())
            return true;
        if (media.playing && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl)
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

    void setRenderMode(int mode) {
        if (mode == renderMode_)
            return;
        const bool wasMinimal = isMinimalMode();
        renderMode_ = mode;
        if (wasMinimal != isMinimalMode()) {
            // 极简关闭逐字绘制后，清掉当前帧的逐字状态；退出时由重新排版恢复几何缓存。
            textDirty_ = true;
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
        const bool popupAvailable = mediaPopupAvailable(sessionVisible_);
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
            songTransitionPending_ = false;
            renderer.resetRoot();
        }
        if (mode == static_cast<int>(RenderMode::Stopped)) {
            // 完全停止：隐藏窗口、停帧定时器、释放 GPU 设备链。此后 SMTC 事件仍更新
            // 内存中的歌词/媒体数据，但 render() 因 visible=false 直接早退，不占 GPU/CPU
            volumeHover_ = false;
            volumePopup_.hide();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            if (visible) {
                visible = false;
                stopFrameTimer();
                if (hwnd)
                    ShowWindow(hwnd, SW_HIDE);
            }
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
        // 从完全停止恢复：按最近会话可见性立即还原窗口；设备链由 render() 惰性重建
        if (sessionVisible_ && !visible) {
            visible = true;
            if (hwnd)
                ShowWindow(hwnd, SW_SHOWNA);
            startFrameTimer();
        }
        if (visible)
            render();
    }

    // 静止判定：所有动画源都停止且没有待处理的布局/资源变化时，跳过整帧重绘。
    // 跳过时屏幕上保持上一次 DirectComposition 提交的内容，不会闪烁或丢状态。
    bool needsFrameRender() const {
        if (textDirty_ || songInfoDirty_ || geomDirty_ || layoutDirty_ || coverDirty ||
            platformIconDirty)
            return true;
        if (lyricTransitionPending_ || lyricTransitionActive_)
            return true;
        if (taskbarDynamicBackgroundAnimating())
            return true;
        if (scrollAnimating_)
            return true;
        // 逐字平滑未收敛（暂停 seek 后）时渲染到收敛为止
        if (karaokeLine() && !karaokeSettled_)
            return true;
        // 播放中的逐字推进和黑胶旋转每帧都在变
        if (media.playing &&
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

    void onTimer() {
        if (tick)
            tick();
        // 只有可见动画源跟随当前显示器刷新率（封顶 ~125fps）；静态播放降到 ~30fps，
        // 避免歌词没有动画时仍高频轮询和提交。状态事件（悬停、恢复播放、seek）走同步
        // render 路径，不依赖定时器，低档位下也不会延迟显示。
        const UINT wantMs = desiredFrameMs();
        if (timerRunning_ && wantMs != timerMs_) {
            SetTimer(hwnd, kTimerId, wantMs, nullptr);
            timerMs_ = wantMs;
        }
        frameNowMs_ = monotonicNowMs();
        updateVinylRotation();
        // 任务栏位置/DPI/主题跟踪与避让探测结果拾取，放到慢速分支，不跟 60fps 走
        if (++slowTick_ >= kSlowTickInterval) {
            slowTick_ = 0;
            updateDisplayRefresh();
            bool changed = detectChanges();
            if (pickProbeResult())
                changed = true;
            if (changed)
                adjustPosition();
        }
        updateScroll();
        // 静止场景跳过整帧重绘：动画源全部停止且无脏状态时，画面保持上一帧内容
        if (needsFrameRender())
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
            else if (wp == kTaskbarAttachTimerId)
                retryTaskbarAttach();
            return 0;
        case WM_DISPLAYCHANGE:
            updateDisplayRefresh();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            // 整个窗口区域都视为客户区，让透明背景也能接收鼠标消息
            return HTCLIENT;
        case WM_MOUSEMOVE: {
            bool wasOver = mouseOver_;
            mouseOver_ = true;
            if (mediaPopupEnabledForScene())
                mediaPopup.onAnchorEnter();
            if (!wasOver)
                render();
            trackMouseLeave();
            // 内嵌控件的音量按钮：悬停弹出音量滑块浮窗
            const bool volHover =
                hitVolumeButton(static_cast<float>(GET_X_LPARAM(lp)),
                                static_cast<float>(GET_Y_LPARAM(lp)));
            if (volHover != volumeHover_) {
                volumeHover_ = volHover;
                if (!volHover)
                    volumePopup_.onAnchorLeave();
            }
            if (volHover && appVolume_.available) {
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect());
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            mouseOver_ = false;
            trackingLeave_ = false;
            volumeHover_ = false;
            volumePopup_.onAnchorLeave();
            mediaPopup.onAnchorLeave();
            render();
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮消息使用屏幕坐标；音量图标上滚动直接调整应用音量（每格 ±2）
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (appVolume_.available && onAppVolume &&
                hitVolumeButton(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                if (steps != 0)
                    onAppVolume(std::clamp(appVolume_.percent + steps * 2, 0, 100));
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            float mx = static_cast<float>(GET_X_LPARAM(lp));
            float my = static_cast<float>(GET_Y_LPARAM(lp));
            int btn = hitButton(mx, my);
            if (btn >= 0 && onControl)
                onControl(static_cast<MediaControl>(btn));
            // 卡片的当前页面自己判断是否为点击展开；这样无播放时的每日一言卡片
            // 可以独立于媒体控件样式使用点击展开。
            else if (!isMinimalMode())
                mediaPopup.onAnchorClick();
            return 0;
        }
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
            runtime_log::writef(L"[taskbar] WM_DESTROY (visible=%d)", visible ? 1 : 0);
            KillTimer(hwnd, kTaskbarAttachTimerId);
            taskbarEmbedded_ = false;
            stopFrameTimer();
            stopProbe();
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
    const bool popupAvailable = impl_->mediaPopupAvailable(impl_->sessionVisible_);
    impl_->mediaPopup.beginPresentationUpdate();
    impl_->mediaPopup.setIdleContent(impl_->idle, popupAvailable);
    impl_->mediaPopup.setMedia(info, popupAvailable);
    impl_->mediaPopup.setPresentationMode(
        impl_->scene_, popupAvailable,
        impl_->hoverControlStyle_ == HoverControlStyle::Inline);
    impl_->mediaPopup.setProgress(impl_->positionMs_);
    impl_->mediaPopup.endPresentationUpdate();
    if (impl_->updateMediaInfo(info))
        impl_->render();
}

void TaskbarHost::setControlCallback(std::function<void(MediaControl)> cb) {
    impl_->onControl = cb;
    impl_->mediaPopup.setControlCallback(std::move(cb));
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
    if (changed)
        impl_->render();
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
    impl_->volumeHover_ = false;
    impl_->volumePopup_.hide();
    impl_->mediaPopup.hideImmediate();
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

void TaskbarHost::setHoverControlStyle(HoverControlStyle style) {
    impl_->setHoverControlStyle(style);
}

void TaskbarHost::setMediaPopupTrigger(MediaPopupTrigger trigger) {
    impl_->setMediaPopupTrigger(trigger);
}

void TaskbarHost::setMediaPopupBackground(MediaPopupBackground mode) {
    impl_->setMediaPopupBackground(mode);
}

void TaskbarHost::setIdleCardBackground(MediaPopupBackground mode) {
    impl_->setIdleCardBackground(mode);
}

void TaskbarHost::setIdleCardBackgroundColor(COLORREF color, bool customized) {
    impl_->setIdleCardBackgroundColor(color, customized);
}

void TaskbarHost::setIdleCardFollowAlbum(bool on) {
    impl_->setIdleCardFollowAlbum(on);
}

void TaskbarHost::setIdleCardTrigger(bool sync, MediaPopupTrigger trigger) {
    impl_->setIdleCardTrigger(sync, trigger);
}

void TaskbarHost::setMediaPopupFollowAlbum(bool on) {
    impl_->setMediaPopupFollowAlbum(on);
}

void TaskbarHost::setMediaPopupAutoTextContrast(bool on) {
    impl_->setMediaPopupAutoTextContrast(on);
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

void TaskbarHost::setRenderMode(int mode) {
    impl_->setRenderMode(mode);
}

void TaskbarHost::setSpectrumVisible(bool on) {
    impl_->setSpectrumVisible(on);
}

void TaskbarHost::setSpectrumStyle(SpectrumStyle style) {
    impl_->setSpectrumStyle(style);
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

void TaskbarHost::setSpectrumBands(const std::array<float, kSpectrumBands>& bands) {
    impl_->setSpectrumBands(bands);
}

void TaskbarHost::setPositionMode(int mode) {
    impl_->setPositionMode(mode);
}

void TaskbarHost::onTaskbarCreated() {
    impl_->onTaskbarCreated();
}
