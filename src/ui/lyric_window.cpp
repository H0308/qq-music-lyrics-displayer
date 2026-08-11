#include "lyric_window.h"
#include "lyric_renderer.h"

#include <cstdio>
#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 16; // ~60fps（配合 timeBeginPeriod(1) 保证触发精度）

constexpr wchar_t kWndClassName[] = L"QQMusicLyricOverlay";
constexpr wchar_t kFontFamily[] = L"Microsoft YaHei UI";

constexpr float kAnchorRatio = 0.42f; // 当前行垂直锚点
constexpr float kScrollEase = 0.134f; // 滚动 ease-out 系数（60fps 下与原来 30fps/0.25 的时长一致）
constexpr float kMinFont = 14.0f;
constexpr float kMaxFont = 48.0f;
constexpr float kBarH = 60.0f;        // 底部控制条高度（DIP）

// GDI+ 一次性初始化（用于 WIC 解不了的 JPEG 兜底）
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
        if (token_) Gdiplus::GdiplusShutdown(token_);
    }
private:
    ULONG_PTR token_ = 0;
};

// 进程级初始化一次 GDI+（封面解码、颜色抽取共用）
GdiplusInit g_gdiplusInit;

// 从封面字节抽取两个差异较大的主色，用于渐变背景
std::pair<D2D1_COLOR_F, D2D1_COLOR_F> extractGradientColors(const std::vector<uint8_t>& bytes) {
    D2D1_COLOR_F fallback1 = D2D1::ColorF(0.07f, 0.07f, 0.07f, 1.0f);
    D2D1_COLOR_F fallback2 = D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f);
    if (bytes.empty()) return {fallback1, fallback2};

    HGLOBAL hglobal = GlobalAlloc(GHND, bytes.size());
    if (!hglobal) return {fallback1, fallback2};
    void* ptr = GlobalLock(hglobal);
    if (ptr) {
        memcpy(ptr, bytes.data(), bytes.size());
        GlobalUnlock(hglobal);
    }
    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hglobal, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        GlobalFree(hglobal);
        return {fallback1, fallback2};
    }
    Gdiplus::Bitmap bitmap(stream);
    stream->Release();
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return {fallback1, fallback2};

    UINT w = bitmap.GetWidth();
    UINT h = bitmap.GetHeight();
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) !=
        Gdiplus::Ok)
        return {fallback1, fallback2};

    struct Bucket {
        uint32_t r = 0, g = 0, b = 0, n = 0;
    };
    Bucket buckets[16][16][16]{};
    const uint8_t* pixels = static_cast<const uint8_t*>(data.Scan0);
    int stride = data.Stride;
    for (UINT y = 0; y < h; y += 4) {
        for (UINT x = 0; x < w; x += 4) {
            const uint8_t* p = pixels + y * stride + x * 4;
            BYTE bb = p[0], pg = p[1], pr = p[2];
            int luma = (pr * 299 + pg * 587 + bb * 114) / 1000;
            if (luma < 24 || luma > 235) continue;
            auto& bk = buckets[pr >> 4][pg >> 4][bb >> 4];
            bk.r += pr;
            bk.g += pg;
            bk.b += bb;
            bk.n++;
        }
    }
    bitmap.UnlockBits(&data);

    struct Cand {
        float w;
        uint8_t r, g, b;
    };
    std::vector<Cand> cands;
    cands.reserve(64);
    for (int R = 0; R < 16; ++R)
        for (int G = 0; G < 16; ++G)
            for (int B = 0; B < 16; ++B) {
                auto& bk = buckets[R][G][B];
                if (bk.n < 8) continue;
                float fr = bk.r / (float)bk.n / 255.0f;
                float fg = bk.g / (float)bk.n / 255.0f;
                float fb = bk.b / (float)bk.n / 255.0f;
                float mx = std::max(fr, std::max(fg, fb));
                float mn = std::min(fr, std::min(fg, fb));
                float sat = mx > 0 ? (mx - mn) / mx : 0;
                cands.push_back({bk.n * (0.3f + sat), (uint8_t)(fr * 255), (uint8_t)(fg * 255),
                                 (uint8_t)(fb * 255)});
            }
    if (cands.empty()) return {fallback1, fallback2};

    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.w > b.w; });

    auto makeColor = [](uint8_t r, uint8_t g, uint8_t b) {
        return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    };
    D2D1_COLOR_F primary = makeColor(cands[0].r, cands[0].g, cands[0].b);
    D2D1_COLOR_F secondary = primary;
    for (auto& c : cands) {
        int dr = (int)c.r - (int)cands[0].r;
        int dg = (int)c.g - (int)cands[0].g;
        int db = (int)c.b - (int)cands[0].b;
        if (dr * dr + dg * dg + db * db > 3264) {
            secondary = makeColor(c.r, c.g, c.b);
            break;
        }
    }
    return {primary, secondary};
}

// 读取注册表 DWORD，失败返回默认值
DWORD regDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD defaultValue) {
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
    return value;
}

bool isSystemLightTheme() {
    // 1 = 浅色，0 = 深色（默认）
    return regDword(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    L"SystemUsesLightTheme", 0) != 0;
}

} // namespace

struct OverlayHost::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool visible = false;
    bool clickThrough = false;

    // 布局（DIP，96dpi 基准）
    float wndW = 860.0f;
    float fontSize = 24.0f;
    std::wstring fontFamily = kFontFamily;
    UINT dpi = 96;

    float lineHeight() const { return fontSize * 2.2f; }
    float lyricH() const { return lineHeight() * 5.0f; } // 歌词区高度
    float wndH() const { return lyricH() + kBarH; }      // 歌词区 + 底部控制条

    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    int currentLine = -1;
    float scroll = 0.0f;
    float scrollTarget = 0.0f;

    // 歌词背板渐变
    D2D1_COLOR_F bgPrimary = D2D1::ColorF(0, 0, 0, 0);
    D2D1_COLOR_F bgSecondary = D2D1::ColorF(0, 0, 0, 0);
    bool gradientDirty = true;

    // 每行预计算的排版：正常 / 高亮两种格式各自的单行最大宽度与折行结果
    struct LineLayout {
        IDWriteTextLayout* layoutNormal = nullptr;
        IDWriteTextLayout* layoutCurrent = nullptr;
        float naturalWNormal = 0.0f;  // 正常格式单行最大宽度
        float naturalWCurrent = 0.0f; // 高亮格式单行最大宽度
        float heightNormal = 0.0f;    // 按内容宽度折行后的高度
        float heightCurrent = 0.0f;
    };
    std::vector<LineLayout> layouts;
    bool layoutsDirty = true;

    std::function<void()> tick;

    // 底部控制条
    OverlayMediaInfo media;
    std::function<void(MediaControl)> onControl;
    ID2D1Bitmap* coverBmp = nullptr;
    bool coverDirty = true;
    bool barTextDirty = true;
    bool barGeomDirty = true;
    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtArtist = nullptr;
    IDWriteTextLayout* titleLayout = nullptr;
    IDWriteTextLayout* artistLayout = nullptr;
    ID2D1SolidColorBrush* brushDivider = nullptr; // 分隔线/占位/置灰
    ID2D1SolidColorBrush* brushBtn = nullptr;     // 按钮/标题文字
    ID2D1SolidColorBrush* brushBarLight = nullptr;   // 封面渐变背景上的浅色文字/按钮
    ID2D1SolidColorBrush* brushBarDividerLight = nullptr; // 封面渐变背景上的浅色分隔线
    ID2D1RoundedRectangleGeometry* coverClip = nullptr; // 设备无关，跨重建存活
    ID2D1PathGeometry* barBgPath = nullptr;             // 底部控制条背景（底部圆角）
    ID2D1Layer* coverLayer = nullptr;                   // 设备相关
    ID2D1PathGeometry* icoPlay = nullptr;   // 播放（右向三角）
    ID2D1PathGeometry* icoPrev = nullptr;   // 上一首（左向三角 + 左侧竖条）
    ID2D1PathGeometry* icoNext = nullptr;   // 下一首（右向三角 + 右侧竖条）

    // D2D / GDI 资源
    LyricRenderer renderer;
    ID2D1SolidColorBrush* brushBg = nullptr;
    ID2D1SolidColorBrush* brushNormal = nullptr;
    ID2D1SolidColorBrush* brushCurrent = nullptr;
    ID2D1SolidColorBrush* brushShadow = nullptr;
    ID2D1SolidColorBrush* brushGradientOverlay = nullptr; // 渐变背景上的暗色遮罩
    ID2D1LinearGradientBrush* brushGradient = nullptr;     // 封面主辅色渐变
    ID2D1SolidColorBrush* brushBarBg = nullptr;            // 底部控制条独立背景
    IDWriteTextFormat* fmtLine = nullptr;
    IDWriteTextFormat* fmtCurrent = nullptr;

    bool lightTheme_ = false;
    int themeTick_ = 0; // 主题轮询计数器（注册表读取不跟 60fps 走）

    bool dragging = false;
    bool quitting = false;
    POINT dragCursor{};
    RECT dragWnd{};

    float scale() const { return (float)dpi / 96.0f; }

    // ---------- 资源 ----------

    void createDeviceResources() {
        if (brushBg)
            return;
        lightTheme_ = isSystemLightTheme();
        renderer.initialize();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        rt->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f), &brushBg);
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.60f), &brushNormal);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.76f, 0.49f, 1.0f),&brushCurrent); // QQ 绿
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &brushShadow);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f), &brushGradientOverlay);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f), &brushBarBg);
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.15f), &brushDivider);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.90f), &brushBtn);
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &brushDivider);
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f), &brushBtn);
        }
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), &brushBarLight);
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f), &brushBarDividerLight);
        rt->CreateLayer(&coverLayer);
        recreateFormats();
    }

    void ensureGradientBrush() {
        auto* rt = renderer.renderTarget();
        if (!gradientDirty || !rt) return;
        gradientDirty = false;
        if (brushGradient) {
            brushGradient->Release();
            brushGradient = nullptr;
        }
        // alpha 为 0 表示没有有效颜色（无封面），此时不创建渐变画刷
        if (bgPrimary.a == 0.0f && bgSecondary.a == 0.0f) return;

        ID2D1GradientStopCollection* stops = nullptr;
        D2D1_GRADIENT_STOP gs[2];
        gs[0].position = 0.0f;
        gs[0].color = bgPrimary;
        gs[1].position = 1.0f;
        gs[1].color = bgSecondary;
        if (FAILED(rt->CreateGradientStopCollection(gs, 2, &stops))) return;

        float h = wndH();
        HRESULT hr = rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                D2D1::Point2F(0.0f, h)),
            stops, &brushGradient);
        stops->Release();
    }

    void invalidateGradient() {
        gradientDirty = true;
        if (brushGradient) {
            brushGradient->Release();
            brushGradient = nullptr;
        }
    }

    void recreateFormats() {
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite) return;
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT ta,
                        DWRITE_PARAGRAPH_ALIGNMENT pa, IDWriteTextFormat** out) {
            if (*out) {
                (*out)->Release();
                *out = nullptr;
            }
            dwrite->CreateTextFormat(fontFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", out);
            if (*out) {
                (*out)->SetTextAlignment(ta);
                (*out)->SetParagraphAlignment(pa);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };
        make(fontSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, &fmtLine);
        make(fontSize * 1.15f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, &fmtCurrent);
        make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR, &fmtTitle);
        make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR, &fmtArtist);
        barTextDirty = true;
        barGeomDirty = true;
    }

    // ---------- 歌词排版（显示前预计算） ----------

    float contentPad() const { return 24.0f; }
    float contentWidth() const { return wndW - contentPad() * 2.0f; }
    float lineGap() const { return fontSize * 0.8f; }

    // 先用无限宽度测单行最大宽度，再限制到内容宽度得到折行高度
    IDWriteTextLayout* buildLayout(IDWriteTextFormat* fmt, const std::wstring& text,
                                   float& naturalW, float& height) {
        IDWriteFactory* dwrite = renderer.dwrite();
        IDWriteTextLayout* lay = nullptr;
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 100000.0f,
                                            100000.0f, &lay)))
            return nullptr;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS m{};
        lay->GetMetrics(&m);
        naturalW = m.width;
        lay->SetMaxWidth(contentWidth());
        lay->GetMetrics(&m);
        height = m.height;
        // 布局框默认 100000 高，段落居中会把文字推到框中央；收紧到内容高度
        lay->SetMaxHeight(height);
        return lay;
    }

    void releaseLayouts() {
        for (auto& ll : layouts) {
            if (ll.layoutNormal) ll.layoutNormal->Release();
            if (ll.layoutCurrent) ll.layoutCurrent->Release();
        }
        layouts.clear();
    }

    void rebuildLayouts() {
        layoutsDirty = false;
        releaseLayouts();
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtLine || !fmtCurrent) {
            layoutsDirty = true; // 资源未就绪，渲染时重试
            return;
        }
        layouts.resize(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            LineLayout& ll = layouts[i];
            if (lines[i].text.empty()) {
                ll.heightNormal = fontSize * 1.4f;
                ll.heightCurrent = fontSize * 1.15f * 1.4f;
                continue;
            }
            ll.layoutNormal =
                buildLayout(fmtLine, lines[i].text, ll.naturalWNormal, ll.heightNormal);
            ll.layoutCurrent =
                buildLayout(fmtCurrent, lines[i].text, ll.naturalWCurrent, ll.heightCurrent);
            if (ll.heightNormal <= 0.0f) ll.heightNormal = fontSize * 1.4f;
            if (ll.heightCurrent <= 0.0f) ll.heightCurrent = fontSize * 1.15f * 1.4f;
        }
        updateScrollTarget();
    }

    float blockHeight(size_t i, bool cur) const {
        const LineLayout& ll = layouts[i];
        return (cur ? ll.heightCurrent : ll.heightNormal) + lineGap();
    }

    // 使当前行块垂直居中于锚点所需的滚动偏移（DIP）
    void updateScrollTarget() {
        if (currentLine < 0 || layouts.size() != lines.size() ||
            (size_t)currentLine >= layouts.size()) {
            scrollTarget = 0.0f;
            return;
        }
        float prefix = 0.0f;
        for (int i = 0; i < currentLine; ++i) prefix += blockHeight((size_t)i, false);
        scrollTarget = prefix + blockHeight((size_t)currentLine, true) / 2.0f;
    }

    // ---------- 底部控制条 ----------

    D2D1_RECT_F coverRect() const {
        float top = lyricH() + 9.0f;
        return D2D1::RectF(14.0f, top, 56.0f, top + 42.0f);
    }

    D2D1_POINT_2F btnCenter(int idx) const { // 0=上一首 1=播放/暂停 2=下一首
        float cy = lyricH() + kBarH / 2.0f;
        float nextX = wndW - 14.0f - 15.0f;
        return D2D1::Point2F(nextX - (2.0f - (float)idx) * 38.0f, cy);
    }

    float barTextWidth() const { return wndW - 66.0f - (14.0f + 30.0f * 3.0f + 16.0f) - 10.0f; }

    // GDI+ 解码封面字节 -> D2D 位图（WIC 对某些 QQ 音乐 CDN 的 JPEG 会报 0x88982F72）
    void decodeCover() {
        coverDirty = false;
        if (coverBmp) {
            coverBmp->Release();
            coverBmp = nullptr;
        }
        auto* rt = renderer.renderTarget();
        if (!rt || !media.thumbnail || media.thumbnail->empty()) return;

        HGLOBAL hglobal = GlobalAlloc(GHND, media.thumbnail->size());
        if (!hglobal) return;
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
            std::wprintf(L"[cover] decode failed: GDI+ status=%d\n",
                         (int)bitmap.GetLastStatus());
            stream->Release();
            return;
        }
        UINT w = bitmap.GetWidth();
        UINT h = bitmap.GetHeight();
        Gdiplus::BitmapData bitmapData{};
        Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
        if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB,
                            &bitmapData) != Gdiplus::Ok) {
            std::wprintf(L"[cover] decode failed: GDI+ LockBits\n");
            stream->Release();
            return;
        }
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = (float)dpi;
        props.dpiY = (float)dpi;
        hr = rt->CreateBitmap(D2D1::SizeU(w, h), bitmapData.Scan0, bitmapData.Stride, &props,
                              &coverBmp);
        bitmap.UnlockBits(&bitmapData);
        stream->Release();
        if (FAILED(hr)) {
            std::wprintf(L"[cover] decode failed: D2D CreateBitmap hr=0x%08X\n", hr);
            return;
        }
        std::wprintf(L"[cover] decode ok: %ux%u, size=%zu\n", w, h, media.thumbnail->size());
    }

    void buildBarText() {
        barTextDirty = false;
        if (titleLayout) {
            titleLayout->Release();
            titleLayout = nullptr;
        }
        if (artistLayout) {
            artistLayout->Release();
            artistLayout = nullptr;
        }
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !fmtTitle || !fmtArtist) {
            barTextDirty = true;
            return;
        }
        auto mk = [&](IDWriteTextFormat* fmt, const std::wstring& s, IDWriteTextLayout** out) {
            if (s.empty()) return;
            if (FAILED(dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, barTextWidth(),
                                                20.0f, out)))
                return;
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            (*out)->SetTrimming(&trim, nullptr); // 超长省略号
        };
        mk(fmtTitle, media.title, &titleLayout);
        mk(fmtArtist, media.artist, &artistLayout);
    }

    void ensureBarGeometry() {
        ID2D1Factory* d2d = renderer.d2d();
        if (!barGeomDirty || !d2d) return;
        barGeomDirty = false;
        if (coverClip) {
            coverClip->Release();
            coverClip = nullptr;
        }
        if (barBgPath) {
            barBgPath->Release();
            barBgPath = nullptr;
        }

        D2D1_ROUNDED_RECT rr{coverRect(), 7.0f, 7.0f};
        d2d->CreateRoundedRectangleGeometry(rr, &coverClip);

        // 控制条背景：顶部平直，底部与窗口同圆角
        if (FAILED(d2d->CreatePathGeometry(&barBgPath))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(barBgPath->Open(&sink))) {
            barBgPath->Release();
            barBgPath = nullptr;
            return;
        }
        constexpr float r = 14.0f;
        float top = lyricH();
        float bottom = wndH();
        float left = 0.0f;
        float right = wndW;
        sink->BeginFigure(D2D1::Point2F(left, top), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(right, top));
        sink->AddLine(D2D1::Point2F(right, bottom - r));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(right, bottom), D2D1::Point2F(right - r, bottom)));
        sink->AddLine(D2D1::Point2F(left + r, bottom));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(left, bottom), D2D1::Point2F(left, bottom - r)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
    }

    // 系统风格媒体控制图标（设备无关）
    void ensureIcons() {
        ID2D1Factory* d2d = renderer.d2d();
        if (!d2d || icoPlay)
            return;

        auto makePath = [&](auto&& build, ID2D1PathGeometry** out) {
            if (FAILED(d2d->CreatePathGeometry(out)))
                return;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED((*out)->Open(&sink)))
                return;
            build(sink);
            sink->Close();
            sink->Release();
        };

        // 播放：右向实心三角
        makePath([](ID2D1GeometrySink* sink) {
            sink->BeginFigure(D2D1::Point2F(-0.35f, -0.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(0.55f, 0.0f));
            sink->AddLine(D2D1::Point2F(-0.35f, 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }, &icoPlay);

        // 上一首：左向三角 + 左侧竖条
        makePath([](ID2D1GeometrySink* sink) {
            sink->BeginFigure(D2D1::Point2F(0.35f, -0.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(-0.55f, 0.0f));
            sink->AddLine(D2D1::Point2F(0.35f, 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(D2D1::Point2F(-0.72f, -0.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(-0.56f, -0.5f));
            sink->AddLine(D2D1::Point2F(-0.56f, 0.5f));
            sink->AddLine(D2D1::Point2F(-0.72f, 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }, &icoPrev);

        // 下一首：右向三角 + 右侧竖条
        makePath([](ID2D1GeometrySink* sink) {
            sink->BeginFigure(D2D1::Point2F(-0.35f, -0.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(0.55f, 0.0f));
            sink->AddLine(D2D1::Point2F(-0.35f, 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(D2D1::Point2F(0.56f, -0.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(0.72f, -0.5f));
            sink->AddLine(D2D1::Point2F(0.72f, 0.5f));
            sink->AddLine(D2D1::Point2F(0.56f, 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }, &icoNext);
    }

    void drawButton(int idx) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        D2D1_POINT_2F c = btnCenter(idx);
        bool enabled = idx == 0 ? media.canPrev : idx == 1 ? media.canPlayPause : media.canNext;
        // 有封面渐变时背景必为深色，按钮强制用浅色
        ID2D1SolidColorBrush* fg = brushGradient ? brushBarLight : brushBtn;
        ID2D1SolidColorBrush* brush = enabled ? fg : brushDivider;
        if (idx == 1) {
            // 播放/暂停：系统风格纯色图标，无圆环
            if (media.playing) { // 暂停图标：双竖条
                rt->FillRectangle(D2D1::RectF(c.x - 4.5f, c.y - 5.5f, c.x - 1.8f, c.y + 5.5f), brush);
                rt->FillRectangle(D2D1::RectF(c.x + 1.8f, c.y - 5.5f, c.x + 4.5f, c.y + 5.5f), brush);
            } else if (icoPlay) {
                rt->SetTransform(D2D1::Matrix3x2F::Scale(13.0f, 13.0f) *
                                 D2D1::Matrix3x2F::Translation(c.x + 0.8f, c.y));
                rt->FillGeometry(icoPlay, brush);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        } else {
            // 上一首/下一首
            ID2D1PathGeometry* g = idx == 0 ? icoPrev : icoNext;
            if (g) {
                rt->SetTransform(D2D1::Matrix3x2F::Scale(11.0f, 11.0f) *
                                 D2D1::Matrix3x2F::Translation(c.x, c.y));
                rt->FillGeometry(g, brush);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }
    }

    void drawBar() {
        auto* rt = renderer.renderTarget();
        if (!rt) return;
        float top = lyricH();
        // 有封面渐变时背景必为深色，控制条前景强制浅色；无封面时按系统主题
        bool onGradient = brushGradient != nullptr;
        ID2D1SolidColorBrush* fg = onGradient ? brushBarLight : brushBtn;
        ID2D1SolidColorBrush* dim = onGradient ? brushNormal : brushBtn;
        ID2D1SolidColorBrush* div = onGradient ? brushBarDividerLight : brushDivider;
        // 控制条独立背景，和歌词区明确分区；底部与窗口同圆角
        if (brushBarBg && barBgPath)
            rt->FillGeometry(barBgPath, brushBarBg);
        rt->DrawLine(D2D1::Point2F(14.0f, top + 0.5f), D2D1::Point2F(wndW - 14.0f, top + 0.5f),
                     div, 1.0f);
        if (coverDirty) decodeCover();
        D2D1_RECT_F cr = coverRect();
        ensureBarGeometry();
        if (coverBmp && coverClip && coverLayer) {
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), coverClip), coverLayer);
            rt->DrawBitmap(coverBmp, cr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
        } else {
            D2D1_ROUNDED_RECT rr{cr, 7.0f, 7.0f};
            rt->FillRoundedRectangle(rr, div);
        }
        if (barTextDirty) buildBarText();
        float tx = cr.right + 10.0f;
        if (titleLayout)
            rt->DrawTextLayout(D2D1::Point2F(tx, top + 10.0f), titleLayout, fg);
        if (artistLayout)
            rt->DrawTextLayout(D2D1::Point2F(tx, top + 33.0f), artistLayout, dim);
        ensureIcons();
        for (int i = 0; i < 3; ++i) drawButton(i);
    }

    // 命中检测：返回按钮序号（0/1/2），未命中或已置灰返回 -1
    int hitButton(float x, float y) const {
        if (y < lyricH()) return -1;
        bool en[3] = {media.canPrev, media.canPlayPause, media.canNext};
        for (int i = 0; i < 3; ++i) {
            if (!en[i]) continue;
            D2D1_POINT_2F c = btnCenter(i);
            if (std::fabs(x - c.x) <= 15.0f && std::fabs(y - c.y) <= 15.0f) return i;
        }
        return -1;
    }

    void discardDeviceResources() {
        auto r = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        r(fmtLine);
        r(fmtCurrent);
        r(fmtTitle);
        r(fmtArtist);
        r(titleLayout);
        r(artistLayout);
        r(coverBmp);
        r(coverLayer);
        r(brushBg);
        r(brushNormal);
        r(brushCurrent);
        r(brushShadow);
        r(brushGradientOverlay);
        r(brushGradient);
        r(brushBarBg);
        r(brushDivider);
        r(brushBtn);
        r(brushBarLight);
        r(brushBarDividerLight);
        renderer.discard();
        coverDirty = true;
        barTextDirty = true;
        gradientDirty = true;
    }

    void releaseAll() {
        releaseLayouts();
        discardDeviceResources();
        if (coverClip) {
            coverClip->Release();
            coverClip = nullptr;
        }
        if (barBgPath) {
            barBgPath->Release();
            barBgPath = nullptr;
        }
        if (icoPlay) {
            icoPlay->Release();
            icoPlay = nullptr;
        }
        if (icoPrev) {
            icoPrev->Release();
            icoPrev = nullptr;
        }
        if (icoNext) {
            icoNext->Release();
            icoNext = nullptr;
        }
        renderer.releaseAll();
    }

    // ---------- 渲染 ----------

    void drawText(IDWriteTextFormat* fmt, const std::wstring& text, const D2D1_RECT_F& rect,
                  ID2D1SolidColorBrush* brush) {
        auto* rt = renderer.renderTarget();
        if (!rt) return;
        rt->DrawTextW(text.c_str(), (UINT32)text.size(), fmt, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }

    void render() {
        if (!visible || !hwnd) return;
        createDeviceResources();
        int pxW = (int)std::lround(wndW * scale());
        int pxH = (int)std::lround(wndH() * scale());
        if (!renderer.bindDC(pxW, pxH)) return;
        auto* rt = renderer.renderTarget();
        if (!rt) return;
        renderer.setDpi(dpi);

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        ensureGradientBrush();
        D2D1_ROUNDED_RECT bg{D2D1::RectF(0.0f, 0.0f, wndW, wndH()), 14.0f, 14.0f};
        if (brushGradient) {
            rt->FillRoundedRectangle(bg, brushGradient);
            rt->FillRoundedRectangle(bg, brushGradientOverlay);
        } else {
            rt->FillRoundedRectangle(bg, brushBg);
        }

        // 歌词只画在歌词区，不进入底部控制条
        D2D1_RECT_F lyricClip = D2D1::RectF(0.0f, 0.0f, wndW, lyricH());
        rt->PushAxisAlignedClip(lyricClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float anchorY = lyricH() * kAnchorRatio;
        if (!lines.empty()) {
            if (layoutsDirty || layouts.size() != lines.size()) rebuildLayouts();
            const float x = contentPad();
            float cum = 0.0f; // 第 i 行块顶相对窗口顶部的累计偏移（未减 scroll）
            for (size_t i = 0; i < lines.size(); ++i) {
                bool cur = ((int)i == currentLine);
                float bh = blockHeight(i, cur);
                float yTop = anchorY + cum - scroll;
                cum += bh;
                if (yTop > lyricH() || yTop + bh < 0.0f) continue;
                const LineLayout& ll = layouts[i];
                IDWriteTextLayout* lay = cur ? ll.layoutCurrent : ll.layoutNormal;
                if (!lay) continue;
                float layH = cur ? ll.heightCurrent : ll.heightNormal;
                float y = yTop + (bh - layH) * 0.5f;
                ID2D1SolidColorBrush* brush = cur ? brushCurrent : brushNormal;
                rt->DrawTextLayout(D2D1::Point2F(x + 1.0f, y + 1.5f), lay, brushShadow);
                rt->DrawTextLayout(D2D1::Point2F(x, y), lay, brush);
            }
        } else if (!statusText.empty()) {
            D2D1_RECT_F rect = D2D1::RectF(0.0f, 0.0f, wndW, lyricH());
            drawText(fmtLine, statusText, rect, brushNormal);
        }

        rt->PopAxisAlignedClip();

        if (brushDivider && brushBtn) drawBar();

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardDeviceResources();
            return;
        }
        if (SUCCEEDED(hr))
            renderer.present(hwnd);
        else
            std::wprintf(L"[overlay] EndDraw failed: 0x%08X\n", hr);
    }

    // ---------- 事件 ----------

    void detectThemeChange() {
        bool light = isSystemLightTheme();
        if (light != lightTheme_) {
            lightTheme_ = light;
            discardDeviceResources();
        }
    }

    void onTimer() {
        if (tick)
            tick();
        // 主题轮询要读注册表，降到约 1s 一次；WM_SETTINGCHANGE 会立即触发，不影响响应
        if (++themeTick_ >= 60) {
            themeTick_ = 0;
            detectThemeChange();
        }
        float diff = scrollTarget - scroll;
        if (std::fabs(diff) > 0.002f)
            scroll += diff * kScrollEase;
        else
            scroll = scrollTarget;
        render();
    }

    void resizeWindow() {
        if (!hwnd) return;
        int w = (int)std::lround(wndW * scale());
        int h = (int)std::lround(wndH() * scale());
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void changeFont(float delta) {
        setFont(fontFamily, fontSize + delta);
    }

    void setFont(const std::wstring& family, float size) {
        fontFamily = family;
        fontSize = std::clamp(size, kMinFont, kMaxFont);
        recreateFormats();
        layoutsDirty = true; // 字体变化需重新计算折行
        invalidateGradient();
        resizeWindow();
        render();
    }

    void setClickThrough(bool on) {
        clickThrough = on;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (on)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kTimerId) onTimer();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            if (!clickThrough) {
                float mx = (float)GET_X_LPARAM(lp) / scale();
                float my = (float)GET_Y_LPARAM(lp) / scale();
                int btn = hitButton(mx, my);
                if (btn >= 0) {
                    if (onControl) onControl((MediaControl)btn);
                    return 0;
                }
                dragging = true;
                SetCapture(hwnd);
                GetCursorPos(&dragCursor);
                GetWindowRect(hwnd, &dragWnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                POINT pt;
                GetCursorPos(&pt);
                SetWindowPos(hwnd, nullptr, dragWnd.left + (pt.x - dragCursor.x),
                             dragWnd.top + (pt.y - dragCursor.y), 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_MOUSEWHEEL:
            changeFont(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 2.0f : -2.0f);
            return 0;
        case WM_DPICHANGED: {
            dpi = HIWORD(wp);
            RECT* sug = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                         sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
            recreateFormats();
            invalidateGradient();
            render();
            return 0;
        }
        case WM_SETTINGCHANGE:
            detectThemeChange();
            render();
            return 0;
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
        if (self) return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
};

OverlayHost::OverlayHost() : impl_(std::make_unique<Impl>()) {}

OverlayHost::~OverlayHost() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
}

bool OverlayHost::create(HINSTANCE inst) {
    impl_->inst = inst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int pxW = (int)std::lround(impl_->wndW);
    int pxH = (int)std::lround(impl_->wndH());
    int x = work.left + ((work.right - work.left) - pxW) / 2;
    int y = work.bottom - pxH - 80;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyric", WS_POPUP, x, y, pxW, pxH, nullptr,
                             nullptr, inst, impl_.get());
    if (!h) return false;
    impl_->hwnd = h;
    impl_->dpi = GetDpiForWindow(h);
    impl_->resizeWindow();
    return true;
}

HWND OverlayHost::hwnd() const {
    return impl_ ? impl_->hwnd : nullptr;
}

void OverlayHost::setTickCallback(std::function<void()> cb) {
    impl_->tick = std::move(cb);
}

void OverlayHost::setMediaInfo(const OverlayMediaInfo& info) {
    bool thumbChanged = info.thumbnail != impl_->media.thumbnail;
    bool textChanged = info.title != impl_->media.title || info.artist != impl_->media.artist;
    impl_->media = info;
    if (thumbChanged) {
        impl_->coverDirty = true;
        if (info.thumbnail && !info.thumbnail->empty()) {
            auto colors = extractGradientColors(*info.thumbnail);
            impl_->bgPrimary = colors.first;
            impl_->bgSecondary = colors.second;
        } else {
            impl_->bgPrimary = D2D1::ColorF(0, 0, 0, 0);
            impl_->bgSecondary = D2D1::ColorF(0, 0, 0, 0);
        }
        impl_->invalidateGradient();
    }
    if (textChanged) impl_->barTextDirty = true;
    impl_->render();
}

void OverlayHost::setControlCallback(std::function<void(MediaControl)> cb) {
    impl_->onControl = std::move(cb);
}

const std::vector<LyricLine>& OverlayHost::lyrics() const {
    return impl_->lines;
}

void OverlayHost::show() {
    if (!impl_->visible) {
        impl_->visible = true;
        ShowWindow(impl_->hwnd, SW_SHOWNA);
    }
    impl_->render();
}

void OverlayHost::hide() {
    if (impl_->visible) {
        impl_->visible = false;
        ShowWindow(impl_->hwnd, SW_HIDE);
    }
}

void OverlayHost::setLyrics(const std::vector<LyricLine>& lines) {
    impl_->lines = lines;
    impl_->currentLine = -1;
    impl_->scroll = 0.0f;
    impl_->scrollTarget = 0.0f;
    impl_->layoutsDirty = true;
    if (!lines.empty()) impl_->statusText.clear();
    impl_->render();
}

void OverlayHost::setCurrentLine(int index) {
    if (index != impl_->currentLine) {
        impl_->currentLine = index;
        impl_->updateScrollTarget();
    }
}

void OverlayHost::setStatusText(const std::wstring& text) {
    impl_->statusText = text;
    impl_->render();
}

bool OverlayHost::isTaskbar() const {
    return false;
}

int OverlayHost::currentLine() const {
    return impl_->currentLine;
}

const std::wstring& OverlayHost::statusText() const {
    return impl_->statusText;
}

void OverlayHost::changeFont(float delta) {
    impl_->changeFont(delta);
}

void OverlayHost::setFont(const std::wstring& family, float size) {
    impl_->setFont(family, size);
}

void OverlayHost::setClickThrough(bool on) {
    impl_->setClickThrough(on);
}

bool OverlayHost::clickThrough() const {
    return impl_->clickThrough;
}
