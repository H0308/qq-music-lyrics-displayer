#include "runtime_log_dialog.h"

#include "ui/app_icon.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"

#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

namespace {

constexpr UINT kMsgDirectoryPicked = WM_APP + 240;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT64 kInitialPickerGeneration = 1;

constexpr float kWindowW = 680.0f;
constexpr float kWindowH = 560.0f;
constexpr float kMinClientWidthDip = 620.0f;
constexpr float kMinClientHeightDip = 520.0f;
constexpr float kMinClientAspectRatio = kMinClientWidthDip / kMinClientHeightDip;
constexpr float kPagePadding = 24.0f;
constexpr float kCardGap = 12.0f;
constexpr float kCardRadius = 8.0f;
constexpr float kCoverImageSizeDip = 40.0f;
constexpr float kCoverToggleWidthDip = 36.0f;
constexpr float kCoverToggleHeightDip = 20.0f;
constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

constexpr int kHitChooseDirectory = 1;
constexpr int kHitOpenDirectory = 2;
constexpr int kHitRetention = 3;
constexpr int kHitCoverToggle = 4;

const std::array<int, 4> kRetentionDays{0, 7, 30, 90};

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

ID2D1Bitmap* decodeCoverBitmap(
    ID2D1RenderTarget* target, const std::shared_ptr<const std::vector<uint8_t>>& image,
    UINT dpi, UINT targetPx) {
    if (!target || !image || image->empty() || targetPx == 0)
        return nullptr;

    HGLOBAL hglobal = GlobalAlloc(GHND, image->size());
    if (!hglobal)
        return nullptr;
    void* buffer = GlobalLock(hglobal);
    if (!buffer) {
        GlobalFree(hglobal);
        return nullptr;
    }
    std::memcpy(buffer, image->data(), image->size());
    GlobalUnlock(hglobal);

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hglobal, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        GlobalFree(hglobal);
        return nullptr;
    }

    Gdiplus::Bitmap bitmap(stream);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        stream->Release();
        return nullptr;
    }

    UINT width = bitmap.GetWidth();
    UINT height = bitmap.GetHeight();
    if (width == 0 || height == 0) {
        stream->Release();
        return nullptr;
    }

    Gdiplus::Bitmap scaled(static_cast<INT>(targetPx), static_cast<INT>(targetPx),
                           PixelFormat32bppPARGB);
    Gdiplus::Bitmap* pixels = &bitmap;
    if ((width > targetPx || height > targetPx) && scaled.GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics graphics(&scaled);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        if (graphics.DrawImage(&bitmap,
                               Gdiplus::Rect(0, 0, static_cast<INT>(targetPx),
                                             static_cast<INT>(targetPx)),
                               0, 0, static_cast<INT>(width), static_cast<INT>(height),
                               Gdiplus::UnitPixel) == Gdiplus::Ok) {
            pixels = &scaled;
            width = targetPx;
            height = targetPx;
        }
    }

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    if (pixels->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok) {
        stream->Release();
        return nullptr;
    }

    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi), static_cast<float>(dpi));
    ID2D1Bitmap* decoded = nullptr;
    hr = target->CreateBitmap(D2D1::SizeU(width, height), data.Scan0, data.Stride, &props,
                               &decoded);
    pixels->UnlockBits(&data);
    stream->Release();
    if (FAILED(hr))
        return nullptr;
    return decoded;
}

struct DirectoryPayload {
    uint64_t generation = 0;
    std::wstring path;
};

std::wstring retentionLabel(int days) {
    if (days <= 0)
        return L"定期清理：不清理";
    return L"定期清理：保留 " + std::to_wstring(days) + L" 天";
}

std::wstring percentText(double value) {
    if (value < 0.0)
        return L"不可用";
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f%%", value);
    return text;
}

std::wstring memoryText(uint64_t bytes) {
    if (bytes == 0)
        return L"不可用";
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

std::wstring durationText(int64_t durationMs) {
    if (durationMs <= 0)
        return L"未知";
    const int64_t totalSeconds = durationMs / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;
    wchar_t text[32]{};
    if (hours > 0)
        swprintf_s(text, L"%lld:%02lld:%02lld", hours, minutes, seconds);
    else
        swprintf_s(text, L"%lld:%02lld", minutes, seconds);
    return text;
}

} // namespace

struct RuntimeLogDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND notifyHwnd = nullptr;
    HWND hwnd = nullptr;
    runtime_log::RuntimeLogger* logger = nullptr;
    DirectoryCallback onDirectoryChanged;
    RetentionCallback onRetentionChanged;
    fluent::FluentDialogSurface surface;
    bool backdrop = false;
    int hoverId = 0;
    int pressedId = 0;
    int focusedId = kHitChooseDirectory;
    bool focusVisible = false;
    bool showCover = false;
    uint64_t pickerGeneration = kInitialPickerGeneration;
    bool pickerOpen = false;
    D2D1_RECT_F chooseRect{};
    D2D1_RECT_F openRect{};
    D2D1_RECT_F retentionRect{};
    D2D1_RECT_F coverToggleRect{};

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* self = static_cast<Impl*>(cs->lpCreateParams);
            self->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, msg, wp, lp);
        return self->handle(msg, wp, lp);
    }

    static bool contains(const D2D1_RECT_F& rect, float x, float y) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    int hitTest(float x, float y) const {
        if (contains(coverToggleRect, x, y))
            return kHitCoverToggle;
        if (contains(chooseRect, x, y))
            return kHitChooseDirectory;
        if (contains(openRect, x, y))
            return kHitOpenDirectory;
        if (contains(retentionRect, x, y))
            return kHitRetention;
        return 0;
    }

    void layout() {
        if (!hwnd)
            return;
        RECT client{};
        GetClientRect(hwnd, &client);
        const float s = surface.dipScale();
        const float width = static_cast<float>(client.right - client.left) / s;
        const float height = static_cast<float>(client.bottom - client.top) / s;

        const float statusTop = 82.0f;
        const float songH = 68.0f;
        const float gridTop = statusTop + songH + kCardGap;
        const float contentWidth = std::max(1.0f, width - kPagePadding * 2.0f);
        const float gridW = (contentWidth - kCardGap * 2.0f) / 3.0f;
        const float coverCardRight =
            kPagePadding + 2.0f * (gridW + kCardGap) + gridW;
        coverToggleRect = D2D1::RectF(
            coverCardRight - 14.0f - kCoverToggleWidthDip, gridTop + 9.0f,
            coverCardRight - 14.0f,
            gridTop + 9.0f + kCoverToggleHeightDip);

        const float settingsTop = std::max(0.0f, height - 188.0f);
        const float buttonTop = settingsTop + 118.0f;
        const float buttonH = 32.0f;
        const float buttonInset = 14.0f;
        const float chooseW = 124.0f;
        const float openW = 124.0f;
        const float retentionW = 180.0f;
        const float buttonLeft = kPagePadding + buttonInset;
        chooseRect = D2D1::RectF(buttonLeft, buttonTop, buttonLeft + chooseW,
                                 buttonTop + buttonH);
        openRect = D2D1::RectF(chooseRect.right + 8.0f, buttonTop,
                               chooseRect.right + 8.0f + openW, buttonTop + buttonH);
        const float buttonRight = width - kPagePadding - buttonInset;
        retentionRect = D2D1::RectF(buttonRight - retentionW, buttonTop, buttonRight,
                                    buttonTop + buttonH);
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter,
                    const D2D1_RECT_F& rect, const std::wstring& text, int id,
                    bool accent = false) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        D2D1_COLOR_F fill = accent ? p.accent : p.controlFill;
        if (pressed)
            fill = accent ? p.accentPressed : p.controlPressed;
        else if (hovered)
            fill = accent ? p.accentHover : p.controlHover;
        painter.fillRoundRect(fill, rect, fluent::metrics::controlRadius);
        if (!accent)
            painter.strokeRoundRect(p.cardStroke, rect, 1.0f, fluent::metrics::controlRadius);
        painter.drawText(text, painter.textFormat(13.0f, 600, true, true), rect,
                         accent ? p.textOnAccent : p.text);
        if (focusVisible && focusedId == id)
            painter.strokeRoundRect(p.accent,
                                    D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f,
                                                rect.right - 1.5f, rect.bottom - 1.5f),
                                    1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
    }

    void drawCard(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                  const std::wstring& label, const std::wstring& value,
                  bool primary = false) {
        const auto& p = fluent::palette();
        painter.fillRoundRect(p.cardFill, rect, kCardRadius);
        painter.strokeRoundRect(p.cardStroke, rect, 1.0f, kCardRadius);
        painter.drawText(label, painter.textFormat(12.0f, 500, false, true),
                         D2D1::RectF(rect.left + 14.0f, rect.top + 10.0f, rect.right - 14.0f,
                                     rect.top + 28.0f),
                         p.textSecondary);
        const float valueTop = primary ? rect.top + 30.0f : rect.top + 32.0f;
        painter.drawTrimmedText(value, painter.textFormat(primary ? 16.0f : 15.0f,
                                                          primary ? 600 : 500, false, true),
                                D2D1::RectF(rect.left + 14.0f, valueTop, rect.right - 14.0f,
                                            rect.bottom - 10.0f),
                                p.text);
    }

    void drawCoverToggle(fluent::FluentDialogSurface::Painter& painter, bool checked) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == kHitCoverToggle;
        const bool pressed = pressedId == kHitCoverToggle;
        const bool focused = focusedId == kHitCoverToggle && focusVisible;
        const float trackH = std::min(kCoverToggleHeightDip,
                                      coverToggleRect.bottom - coverToggleRect.top);
        const float centerY = (coverToggleRect.top + coverToggleRect.bottom) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(
            coverToggleRect.left + 0.5f, centerY - trackH * 0.5f,
            coverToggleRect.right - 0.5f, centerY + trackH * 0.5f);
        const float radius = trackH * 0.5f;
        const float knobR = std::max(1.0f, trackH * 0.5f - 3.5f);
        const float knobX = checked ? track.right - trackH * 0.5f
                                    : track.left + trackH * 0.5f;
        if (checked) {
            painter.fillRoundRect(pressed ? p.accentPressed
                                          : hovered ? p.accentHover : p.accent,
                                  track, radius);
            if (auto* br = painter.brush(p.textOnAccent))
                painter.target()->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR), br);
        } else {
            painter.fillRoundRect(pressed ? p.controlPressed
                                          : hovered ? p.controlHover : p.controlFill,
                                  track, radius);
            painter.strokeRoundRect(p.cardStroke, track, 1.0f, radius);
            if (auto* br = painter.brush(p.textSecondary))
                painter.target()->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR), br);
        }
        if (focused)
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(track.left + 1.5f, track.top + 1.5f,
                            track.right - 1.5f, track.bottom - 1.5f),
                1.5f, std::max(1.0f, radius - 1.5f));
    }

    void drawCoverCard(fluent::FluentDialogSurface::Painter& painter,
                       const D2D1_RECT_F& rect,
                       bool playbackActive,
                       bool coverLoaded,
                       const std::shared_ptr<const std::vector<uint8_t>>& image) {
        const auto& p = fluent::palette();
        painter.fillRoundRect(p.cardFill, rect, kCardRadius);
        painter.strokeRoundRect(p.cardStroke, rect, 1.0f, kCardRadius);
        painter.drawText(L"封面加载", painter.textFormat(12.0f, 500, false, true),
                         D2D1::RectF(rect.left + 14.0f, rect.top + 10.0f,
                                     coverToggleRect.left - 8.0f,
                                     rect.top + 28.0f),
                         p.textSecondary);
        drawCoverToggle(painter, showCover);

        if (!showCover) {
            painter.drawText(!playbackActive ? L"未加载"
                                             : coverLoaded ? L"加载成功" : L"加载失败",
                             painter.textFormat(15.0f, 500, true, true),
                             D2D1::RectF(rect.left + 14.0f, rect.top + 32.0f,
                                         rect.right - 14.0f, rect.bottom - 10.0f),
                             p.text);
            return;
        }

        const float imageSize = std::min(
            kCoverImageSizeDip,
            std::min(rect.right - rect.left - 28.0f, rect.bottom - rect.top - 36.0f));
        const float imageLeft = rect.left + (rect.right - rect.left - imageSize) * 0.5f;
        const float imageTop = rect.bottom - imageSize - 6.0f;
        const UINT targetPx = std::max(
            1u, static_cast<UINT>(std::ceil(imageSize * surface.dipScale())));
        ID2D1Bitmap* bitmap = playbackActive
                                  ? decodeCoverBitmap(painter.target(), image, surface.dpi(),
                                                      targetPx)
                                  : nullptr;
        if (bitmap) {
            painter.target()->DrawBitmap(
                bitmap, D2D1::RectF(imageLeft, imageTop, imageLeft + imageSize,
                                    imageTop + imageSize),
                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            bitmap->Release();
            return;
        }

        painter.drawText(playbackActive ? L"加载失败" : L"未加载",
                         painter.textFormat(15.0f, 500, true, true),
                         D2D1::RectF(rect.left + 14.0f, rect.top + 32.0f, rect.right - 14.0f,
                                     rect.bottom - 10.0f),
                         p.text);
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float width, float height) {
        if (!logger)
            return;
        const auto snapshot = logger->snapshot();
        const auto& p = fluent::palette();
        const float contentWidth = std::max(1.0f, width - kPagePadding * 2.0f);
        const float titleY = 20.0f;
        painter.drawText(L"运行日志", painter.textFormat(22.0f, 600, false, true),
                         D2D1::RectF(kPagePadding, titleY, width - kPagePadding, titleY + 30.0f),
                         p.text);
        painter.drawText(L"当前会话状态与资源占用；详细事件写入下方日志文件",
                         painter.textFormat(12.0f, 400, false, true),
                         D2D1::RectF(kPagePadding, titleY + 32.0f, width - kPagePadding,
                                     titleY + 54.0f),
                         p.textSecondary);

        const float statusTop = 82.0f;
        const float songH = 68.0f;
        std::wstring songValue = L"暂无播放";
        if (snapshot.playbackActive && !snapshot.currentTitle.empty()) {
            songValue = snapshot.currentTitle;
            if (!snapshot.currentArtist.empty())
                songValue += L" · " + snapshot.currentArtist;
        }
        drawCard(painter, D2D1::RectF(kPagePadding, statusTop, width - kPagePadding,
                                     statusTop + songH),
                 L"当前播放", songValue, true);

        const float gridTop = statusTop + songH + kCardGap;
        const float gridH = 76.0f;
        const float gridW = (contentWidth - kCardGap * 2.0f) / 3.0f;
        const auto card = [gridTop, gridH, gridW](int column, int row) {
            const float left = kPagePadding + column * (gridW + kCardGap);
            const float top = gridTop + row * (gridH + kCardGap);
            return D2D1::RectF(left, top, left + gridW, top + gridH);
        };
        drawCard(painter, card(0, 0), L"歌曲总时长", durationText(snapshot.durationMs));
        drawCard(painter, card(1, 0), L"歌词来源", snapshot.playbackActive ? snapshot.lyricSource
                                                                         : L"未加载");
        drawCoverCard(painter, card(2, 0), snapshot.playbackActive, snapshot.coverLoaded,
                      snapshot.coverImage);
        drawCard(painter, card(0, 1), L"CPU", percentText(snapshot.cpuPercent));
        drawCard(painter, card(1, 1), L"GPU", percentText(snapshot.gpuPercent));
        drawCard(painter, card(2, 1), L"专用内存", memoryText(snapshot.memoryBytes));

        const float settingsTop = std::max(0.0f, height - 188.0f);
        const D2D1_RECT_F settingsRect = D2D1::RectF(kPagePadding, settingsTop,
                                                     width - kPagePadding, height - kPagePadding);
        painter.fillRoundRect(p.cardFill, settingsRect, kCardRadius);
        painter.strokeRoundRect(p.cardStroke, settingsRect, 1.0f, kCardRadius);
        painter.drawText(L"日志文件", painter.textFormat(14.0f, 600, false, true),
                         D2D1::RectF(settingsRect.left + 14.0f, settingsRect.top + 12.0f,
                                     settingsRect.right - 14.0f, settingsRect.top + 34.0f),
                         p.text);
        painter.drawText(L"保存位置", painter.textFormat(12.0f, 500, false, true),
                         D2D1::RectF(settingsRect.left + 14.0f, settingsRect.top + 42.0f,
                                     settingsRect.left + 82.0f, settingsRect.top + 64.0f),
                         p.textSecondary);
        painter.drawTrimmedText(snapshot.logDirectory.empty() ? L"未配置" : snapshot.logDirectory,
                                painter.textFormat(12.0f, 400, false, true),
                                D2D1::RectF(settingsRect.left + 86.0f, settingsRect.top + 42.0f,
                                            settingsRect.right - 14.0f, settingsRect.top + 64.0f),
                                p.text);
        std::wstring sessionFileText = L"当前文件：";
        if (snapshot.sessionFileName.empty())
            sessionFileText += L"未创建";
        else
            sessionFileText += snapshot.sessionFileName;
        painter.drawTrimmedText(sessionFileText, painter.textFormat(12.0f, 400, false, true),
                                D2D1::RectF(settingsRect.left + 14.0f, settingsRect.top + 68.0f,
                                            settingsRect.right - 14.0f, settingsRect.top + 90.0f),
                                p.textSecondary);
        drawButton(painter, chooseRect, L"选择位置", kHitChooseDirectory, true);
        drawButton(painter, openRect, L"打开位置", kHitOpenDirectory);
        drawButton(painter, retentionRect, retentionLabel(snapshot.retentionDays), kHitRetention);
    }

    void pickDirectory() {
        if (!hwnd || pickerOpen)
            return;
        pickerOpen = true;
        const HWND target = hwnd;
        const uint64_t generation = ++pickerGeneration;
        const std::wstring initialPath = logger ? logger->directory() : L"";
        std::thread([target, generation, initialPath] {
            std::wstring selectedPath;
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                         COINIT_DISABLE_OLE1DDE);
            if (SUCCEEDED(init)) {
                IFileDialog* dialog = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&dialog)))) {
                    DWORD options = 0;
                    if (SUCCEEDED(dialog->GetOptions(&options)))
                        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                    dialog->SetTitle(L"选择运行日志保存位置");
                    if (!initialPath.empty()) {
                        IShellItem* folder = nullptr;
                        if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.c_str(), nullptr,
                                                                  IID_PPV_ARGS(&folder)))) {
                            dialog->SetFolder(folder);
                            folder->Release();
                        }
                    }
                    if (SUCCEEDED(dialog->Show(nullptr))) {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(dialog->GetResult(&item))) {
                            PWSTR path = nullptr;
                            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                                selectedPath = path;
                                CoTaskMemFree(path);
                            }
                            item->Release();
                        }
                    }
                    dialog->Release();
                }
                CoUninitialize();
            }
            auto* payload = new DirectoryPayload{generation, std::move(selectedPath)};
            if (!PostMessageW(target, kMsgDirectoryPicked, 0,
                              reinterpret_cast<LPARAM>(payload)))
                delete payload;
        }).detach();
    }

    void onCommand(int id) {
        if (id == kHitCoverToggle) {
            showCover = !showCover;
            runtime_log::writef(L"[action][runtime-log] cover-display=%s",
                                showCover ? L"on" : L"off");
        } else if (id == kHitChooseDirectory) {
            pickDirectory();
        } else if (id == kHitOpenDirectory) {
            if (logger)
                logger->openDirectory();
        } else if (id == kHitRetention) {
            const int current = logger ? logger->retentionDays() : 30;
            auto it = std::find(kRetentionDays.begin(), kRetentionDays.end(), current);
            const size_t next = it == kRetentionDays.end()
                                    ? 0
                                    : (static_cast<size_t>(it - kRetentionDays.begin()) + 1) %
                                          kRetentionDays.size();
            if (onRetentionChanged)
                onRetentionChanged(kRetentionDays[next]);
        }
        surface.invalidate();
    }

    void focusStep(int direction) {
        constexpr std::array<int, 4> ids{kHitChooseDirectory, kHitOpenDirectory,
                                         kHitRetention, kHitCoverToggle};
        auto it = std::find(ids.begin(), ids.end(), focusedId);
        int index = it == ids.end() ? 0 : static_cast<int>(it - ids.begin());
        index = (index + direction + static_cast<int>(ids.size())) % static_cast<int>(ids.size());
        focusedId = ids[static_cast<size_t>(index)];
        focusVisible = true;
        surface.invalidate();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd, false);
            surface.initialize(hwnd, backdrop);
            SetTimer(hwnd, kRefreshTimer, 1000, nullptr);
            layout();
            return 0;
        case WM_TIMER:
            if (wp == kRefreshTimer) {
                surface.invalidate();
                return 0;
            }
            break;
        case WM_SIZE:
            layout();
            surface.invalidate();
            return 0;
        case WM_GETMINMAXINFO:
            fluent::setDialogMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp),
                                               kDialogStyle, kDialogExStyle,
                                               kMinClientWidthDip, kMinClientHeightDip);
            return 0;
        case WM_SIZING:
            fluent::enforceDialogMinimumAspectRatio(hwnd, wp, reinterpret_cast<RECT*>(lp),
                                                     kMinClientAspectRatio);
            return TRUE;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            surface.invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop, false);
            surface.setBackdrop(backdrop);
            surface.invalidate();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            surface.paint(hdc, backdrop,
                          [this](fluent::FluentDialogSurface::Painter& painter, float w, float h) {
                              paint(painter, w, h);
                          });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            surface.eraseBackground(reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            const float s = surface.dipScale();
            const int hit = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            if (hit != hoverId) {
                hoverId = hit;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverId = 0;
            surface.invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            focusVisible = false;
            const float s = surface.dipScale();
            pressedId = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            if (pressedId != 0) {
                focusedId = pressedId;
                SetCapture(hwnd);
            }
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const float s = surface.dipScale();
            const int hit = hitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            const int pressed = pressedId;
            pressedId = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (pressed != 0 && pressed == hit)
                onCommand(pressed);
            surface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressedId = 0;
            surface.invalidate();
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB;
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                focusStep((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                onCommand(focusedId);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            focusVisible = false;
            surface.invalidate();
            return 0;
        case kMsgDirectoryPicked: {
            auto* payload = reinterpret_cast<DirectoryPayload*>(lp);
            if (!payload)
                return 0;
            const bool current = payload->generation == pickerGeneration;
            pickerOpen = false;
            if (current && !payload->path.empty() && onDirectoryChanged)
                onDirectoryChanged(payload->path);
            delete payload;
            surface.invalidate();
            return 0;
        }
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kRefreshTimer);
            ++pickerGeneration;
            pickerOpen = false;
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::RuntimeLog), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

RuntimeLogDialog::RuntimeLogDialog() : impl_(std::make_unique<Impl>()) {}

RuntimeLogDialog::~RuntimeLogDialog() {
    if (impl_ && impl_->hwnd)
        impl_->destroy();
}

bool RuntimeLogDialog::create(HINSTANCE inst, HWND parent, runtime_log::RuntimeLogger* logger,
                              DirectoryCallback onDirectoryChanged,
                              RetentionCallback onRetentionChanged) {
    if (!impl_ || impl_->hwnd)
        return impl_ && impl_->hwnd != nullptr;
    impl_->showCover = false;
    impl_->inst = inst;
    impl_->notifyHwnd = parent;
    impl_->logger = logger;
    impl_->onDirectoryChanged = std::move(onDirectoryChanged);
    impl_->onRetentionChanged = std::move(onRetentionChanged);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricRuntimeLogDialog";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    const float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(kWindowW * s)),
            static_cast<LONG>(std::lround(kWindowH * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;
    impl_->hwnd = CreateWindowExW(kDialogExStyle, wc.lpszClassName, L"运行日志", kDialogStyle,
                                  x, y, width, height, nullptr, nullptr, inst, impl_.get());
    if (impl_->hwnd)
        app_icon::applyWindowIcon(impl_->hwnd);
    return impl_->hwnd != nullptr;
}

void RuntimeLogDialog::show() {
    if (!impl_ || !impl_->hwnd)
        return;
    ShowWindow(impl_->hwnd, SW_SHOW);
    SetForegroundWindow(impl_->hwnd);
    SetFocus(impl_->hwnd);
}

void RuntimeLogDialog::destroy() {
    if (impl_)
        impl_->destroy();
}

bool RuntimeLogDialog::isOpen() const {
    return impl_ && impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND RuntimeLogDialog::hwnd() const {
    return impl_ ? impl_->hwnd : nullptr;
}
