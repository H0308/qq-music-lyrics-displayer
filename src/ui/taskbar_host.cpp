#include "taskbar_host.h"
#include "lyric_renderer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId = 2;
constexpr UINT kTimerMs = 100; // 任务栏位置跟踪 10fps
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
    RECT rcTrafficMonitor_{}; // TrafficMonitor 屏幕坐标（缓存，未运行时为 empty）
    UINT dpi_ = 96;
    bool centerAlign_ = true;
    bool lightTheme_ = false;

    // 歌词状态
    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    int currentLine = -1;

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

    // Explorer 重启后重新附着
    UINT taskbarCreatedMsg_ = 0;

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
    ID2D1SolidColorBrush* brushLyric_ = nullptr;        // 歌词主色（用户可配）
    ID2D1SolidColorBrush* brushLyricGlow_ = nullptr;    // 歌词光晕（主色低透明度）
    ID2D1SolidColorBrush* brushLyricOutline_ = nullptr; // 歌词深色描边
    COLORREF lyricColor_ = RGB(49, 194, 124);           // 默认 QQ 绿
    bool lyricGlow_ = false;                            // 描边+光晕开关
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
        return true;
    }

    void updateRects() {
        if (taskbar_)
            GetWindowRect(taskbar_, &rcTaskbar_);
        if (notify_)
            GetWindowRect(notify_, &rcNotify_);
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
        taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
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

        int rightBoundary = notify_ ? rcNotify_.left : rcTaskbar_.right;
        int leftBoundary = rcTaskbar_.left;
        int pxW = 0;
        int x = 0;

        HWND tm = findTrafficMonitor();
        if (tm) {
            RECT rcTm{};
            GetWindowRect(tm, &rcTm);

            // 强制与 TrafficMonitor 互斥：优先放它右边（箭头位置），放不下就放它左边
            int availableRight = rightBoundary - rcTm.right - gap * 2;
            int availableLeft = rcTm.left - leftBoundary - gap * 2;

            if (availableRight >= minW && availableRight >= availableLeft) {
                pxW = std::clamp(availableRight, minW, maxW);
                x = rcTm.right + gap;
            } else if (availableLeft >= minW) {
                pxW = std::clamp(availableLeft, minW, maxW);
                x = rcTm.left - pxW - gap;
            }
        }

        // 没有 TrafficMonitor 或两边都放不下：锚定到通知区左侧
        if (pxW == 0) {
            int available = rightBoundary - leftBoundary - gap;
            if (!notify_) {
                available = (rcTaskbar_.right - rcTaskbar_.left) / 3;
            }
            pxW = std::clamp(available, minW, maxW);
            x = rightBoundary - pxW - gap;
            if (!notify_) {
                x = rcTaskbar_.right - pxW - gap;
            }
        }

        // 保证不超出任务栏范围
        if (x < leftBoundary + gap)
            x = leftBoundary + gap;
        if (x + pxW > rightBoundary - gap && notify_) {
            pxW = rightBoundary - x - gap;
            if (pxW < minW) {
                pxW = minW;
                x = rightBoundary - pxW - gap;
            }
        }

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

        bool changed = dpi != dpi_ || center != centerAlign_ || themeChanged ||
                       !EqualRect(&rcTaskbar, &rcTaskbar_) ||
                       !EqualRect(&rcNotify, &rcNotify_) ||
                       !EqualRect(&rcTm, &rcTrafficMonitor_);
        if (changed) {
            dpi_ = dpi;
            centerAlign_ = center;
            rcTaskbar_ = rcTaskbar;
            rcNotify_ = rcNotify;
            rcTrafficMonitor_ = rcTm;
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
        // 歌词画刷与主题无关，单独创建（用户可换色，换色时只重建这三个）
        createLyricBrushes();
        rt->CreateLayer(&coverLayer_);
        recreateFormats();
    }

    // 歌词主色/光晕/描边画刷：随用户颜色重建，与主题画刷解耦
    void createLyricBrushes() {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        if (brushLyric_) {
            brushLyric_->Release();
            brushLyric_ = nullptr;
        }
        if (brushLyricGlow_) {
            brushLyricGlow_->Release();
            brushLyricGlow_ = nullptr;
        }
        if (brushLyricOutline_) {
            brushLyricOutline_->Release();
            brushLyricOutline_ = nullptr;
        }
        float r = GetRValue(lyricColor_) / 255.0f;
        float g = GetGValue(lyricColor_) / 255.0f;
        float b = GetBValue(lyricColor_) / 255.0f;
        rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 1.00f), &brushLyric_);
        rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 0.28f), &brushLyricGlow_);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f), &brushLyricOutline_);
    }

    void setFontColor(COLORREF rgb) {
        if (lyricColor_ == rgb)
            return;
        lyricColor_ = rgb;
        createLyricBrushes();
        render();
    }

    void setFontGlow(bool on) {
        if (lyricGlow_ == on)
            return;
        lyricGlow_ = on;
        render();
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
    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr) {
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
        auto drawOne = [&](float dx, float dy) {
            if (glow) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(D2D1::Point2F(dx + d[0] * 2.4f, dy + d[1] * 2.4f), layout,
                                       glow);
            }
            if (outline) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(D2D1::Point2F(dx + d[0] * 1.2f, dy + d[1] * 1.2f), layout,
                                       outline);
            }
            rt->DrawTextLayout(D2D1::Point2F(dx, dy), layout, brush);
        };
        if (textW <= areaW) {
            drawOne(x + (areaW - textW) * 0.5f, y);
        } else {
            float loopW = textW + kTextPadding * 2.0f;
            float baseX = x - offset;
            drawOne(baseX, y);
            drawOne(baseX + loopW, y);
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
            drawScrollingText(lyricLayout_, lyricWidth_, lyricHeight_, lyricAreaW, lyricAreaX,
                              lyricY, lyricScrollOffset_,
                              brushLyric_ ? brushLyric_ : brushText_,
                              lyricGlow_ ? brushLyricOutline_ : nullptr,
                              lyricGlow_ ? brushLyricGlow_ : nullptr);
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
        if (!mouseOver_)
            marquee(lyricWidth_, lyricAreaW, lyricScrollSpeed_, lyricScrollOffset_);
    }

    // ---------- 事件 ----------

    void onTimer() {
        if (tick)
            tick();
        if (detectChanges())
            adjustPosition();
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
            if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
                if (findTaskbar()) {
                    SetParent(hwnd, taskbar_);
                    adjustPosition();
                }
                return 0;
            }
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

void TaskbarHost::setFontColor(COLORREF rgb) {
    impl_->setFontColor(rgb);
}

void TaskbarHost::setFontGlow(bool on) {
    impl_->setFontGlow(on);
}

void TaskbarHost::setClickThrough(bool on) {
    // 任务栏歌词不需要鼠标穿透，忽略
    (void)on;
}

bool TaskbarHost::clickThrough() const {
    return false;
}
