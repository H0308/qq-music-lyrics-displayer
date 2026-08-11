#include "ui/manual_search_dialog.h"

#include "ui/lyric_renderer.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <tuple>

namespace {

constexpr int kIdTitleEdit = 101;
constexpr int kIdArtistEdit = 102;
constexpr int kIdSearchBtn = 103;
constexpr int kIdCandidateList = 104;
constexpr int kIdOkBtn = 105;
constexpr int kIdCancelBtn = 106;
constexpr int kIdPreviewPanel = 107;

constexpr UINT kMsgCandidatesReady = WM_APP + 10;
constexpr UINT kMsgPreviewLyricReady = WM_APP + 11;

// 右侧桌面歌词风格预览面板（7 行，中间高亮，无时间戳）
class LyricPreviewPanel {
public:
    bool create(HWND parent, HINSTANCE inst, int x, int y, int w, int h) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"QQMusicLyricPreviewPanel";
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, L"QQMusicLyricPreviewPanel", L"", WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, (HMENU)(UINT_PTR)kIdPreviewPanel, inst, this);
        return hwnd_ != nullptr;
    }

    void setLyrics(const std::vector<LyricLine>& lines) {
        lines_ = lines;
        if (hwnd_)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void destroy() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        LyricPreviewPanel* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<LyricPreviewPanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<LyricPreviewPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            createDeviceResources();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd_, &ps);
            render(hdc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            releaseResources();
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void createDeviceResources() {
        renderer_.initialize();
        dpi_ = GetDpiForWindow(hwnd_);
        renderer_.setDpi(dpi_);
        IDWriteFactory* dw = renderer_.dwrite();
        if (!dw)
            return;
        float size = 18.0f;
        dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn",
                             &fmtNormal_);
        dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 1.15f,
                             L"zh-cn", &fmtCurrent_);
        if (fmtNormal_) {
            fmtNormal_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtNormal_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fmtNormal_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        if (fmtCurrent_) {
            fmtCurrent_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtCurrent_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fmtCurrent_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        auto* rt = renderer_.renderTarget();
        if (rt) {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), &brushNormal_);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.76f, 0.49f, 1.0f), &brushCurrent_);
        }
    }

    void releaseResources() {
        if (fmtNormal_) {
            fmtNormal_->Release();
            fmtNormal_ = nullptr;
        }
        if (fmtCurrent_) {
            fmtCurrent_->Release();
            fmtCurrent_ = nullptr;
        }
        if (brushNormal_) {
            brushNormal_->Release();
            brushNormal_ = nullptr;
        }
        if (brushCurrent_) {
            brushCurrent_->Release();
            brushCurrent_ = nullptr;
        }
        renderer_.releaseAll();
    }

    void discard() {
        if (fmtNormal_) {
            fmtNormal_->Release();
            fmtNormal_ = nullptr;
        }
        if (fmtCurrent_) {
            fmtCurrent_->Release();
            fmtCurrent_ = nullptr;
        }
        if (brushNormal_) {
            brushNormal_->Release();
            brushNormal_ = nullptr;
        }
        if (brushCurrent_) {
            brushCurrent_->Release();
            brushCurrent_ = nullptr;
        }
        renderer_.discard();
    }

    void render(HDC hdc) {
        if (!hwnd_)
            return;
        createDeviceResources();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0)
            return;
        if (!renderer_.bindDC(w, h))
            return;
        auto* rt = renderer_.renderTarget();
        if (!rt)
            return;
        rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f));
        if (!lines_.empty() && fmtNormal_ && fmtCurrent_) {
            constexpr int kVisible = 7;
            int count = std::min(kVisible, static_cast<int>(lines_.size()));
            constexpr int kMid = kVisible / 2; // 3
            float scale = static_cast<float>(dpi_) / 96.0f;
            float dipW = static_cast<float>(w) / scale;
            float dipH = static_cast<float>(h) / scale;
            float slotH = dipH / kVisible;
            float pad = 12.0f;
            for (int i = 0; i < count; ++i) {
                bool cur = (i == kMid);
                const std::wstring& text = lines_[i].text;
                IDWriteTextFormat* fmt = cur ? fmtCurrent_ : fmtNormal_;
                ID2D1SolidColorBrush* brush = cur ? brushCurrent_ : brushNormal_;
                D2D1_RECT_F rect = D2D1::RectF(pad, i * slotH, dipW - pad, (i + 1) * slotH);
                rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, brush);
            }
        }
        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
            discard();
        else
            renderer_.copyToDC(hdc, w, h);
    }

    HWND hwnd_ = nullptr;
    LyricRenderer renderer_;
    IDWriteTextFormat* fmtNormal_ = nullptr;
    IDWriteTextFormat* fmtCurrent_ = nullptr;
    ID2D1SolidColorBrush* brushNormal_ = nullptr;
    ID2D1SolidColorBrush* brushCurrent_ = nullptr;
    std::vector<LyricLine> lines_;
    UINT dpi_ = 96;
};

std::wstring getWindowTextW(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0)
        return {};
    std::wstring s(len, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize(len);
    return s;
}

} // namespace

struct ManualSearchDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND hTitle = nullptr;
    HWND hArtist = nullptr;
    HWND hSearch = nullptr;
    HWND hList = nullptr;
    HWND hOk = nullptr;
    HWND hCancel = nullptr;
    LyricPreviewPanel preview;

    LyricProvider* provider = nullptr;
    std::wstring targetTitle;
    std::wstring targetArtist;
    std::vector<SearchCandidate> candidates;
    int selectedIdx = -1;
    std::vector<LyricLine> previewLines;
    SongInfo previewInfo;

    ManualSearchDialog::ApplyCallback onApply;

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            createControls();
            layout();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
            return 0;
        case kMsgCandidatesReady: {
            auto* payload = reinterpret_cast<std::vector<SearchCandidate>*>(lp);
            if (payload) {
                onCandidatesReady(*payload);
                delete payload;
            }
            return 0;
        }
        case kMsgPreviewLyricReady: {
            auto* payload = reinterpret_cast<std::tuple<int, bool, std::vector<LyricLine>, SongInfo>*>(lp);
            if (payload) {
                onPreviewLyricReady(std::get<0>(*payload), std::get<1>(*payload),
                                    std::get<2>(*payload), std::get<3>(*payload));
                delete payload;
            }
            return 0;
        }
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            preview.destroy();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void createControls() {
        hTitle = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", targetTitle.c_str(),
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                 (HMENU)(UINT_PTR)kIdTitleEdit, inst, nullptr);
        hArtist = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", targetArtist.c_str(),
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                  (HMENU)(UINT_PTR)kIdArtistEdit, inst, nullptr);
        hSearch = CreateWindowExW(0, L"BUTTON", L"搜索", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)kIdSearchBtn, inst, nullptr);

        hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                                (HMENU)(UINT_PTR)kIdCandidateList, inst, nullptr);

        hOk = CreateWindowExW(0, L"BUTTON", L"使用选中歌词",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED, 0, 0, 0, 0,
                              hwnd, (HMENU)(UINT_PTR)kIdOkBtn, inst, nullptr);
        hCancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
                                  0, 0, 0, hwnd, (HMENU)(UINT_PTR)kIdCancelBtn, inst, nullptr);

        preview.create(hwnd, inst, 0, 0, 0, 0);

        // 默认字体
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hArtist, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hSearch, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hList, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)font, TRUE);
    }

    void layout() {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int pad = 12;
        int topH = 28;
        int btnW = 80;
        int bottomH = 36;

        // 顶部输入区
        int editW = (w - pad * 4 - btnW) / 2;
        SetWindowPos(hTitle, nullptr, pad, pad, editW, topH, SWP_NOZORDER);
        SetWindowPos(hArtist, nullptr, pad * 2 + editW, pad, editW, topH, SWP_NOZORDER);
        SetWindowPos(hSearch, nullptr, w - pad - btnW, pad, btnW, topH, SWP_NOZORDER);

        int yTop = pad * 2 + topH;
        int contentH = h - yTop - bottomH - pad;
        int listW = w * 2 / 5;
        int rightX = pad * 2 + listW;
        int rightW = w - rightX - pad;

        // 左侧候选列表
        SetWindowPos(hList, nullptr, pad, yTop, listW, contentH - pad - 32, SWP_NOZORDER);
        // 右侧预览
        SetWindowPos(preview.hwnd(), nullptr, rightX, yTop, rightW, contentH, SWP_NOZORDER);

        // 底部按钮
        int btnY = h - bottomH - pad / 2;
        SetWindowPos(hOk, nullptr, w - pad - btnW * 2 - 8, btnY, btnW * 2, 28, SWP_NOZORDER);
        SetWindowPos(hCancel, nullptr, w - pad - btnW, btnY, btnW, 28, SWP_NOZORDER);
    }

    void onCommand(int id, int code, HWND h) {
        (void)h;
        if (id == kIdSearchBtn && code == BN_CLICKED) {
            doSearch();
        } else if (id == kIdCancelBtn && code == BN_CLICKED) {
            destroy();
        } else if (id == kIdOkBtn && code == BN_CLICKED) {
            applySelection();
        } else if (id == kIdCandidateList && code == LBN_SELCHANGE) {
            onSelectionChanged();
        }
    }

    void doSearch() {
        if (!provider)
            return;
        EnableWindow(hSearch, FALSE);
        SendMessageW(hList, LB_RESETCONTENT, 0, 0);
        preview.setLyrics({});
        selectedIdx = -1;
        candidates.clear();
        EnableWindow(hOk, FALSE);

        std::wstring title = getWindowTextW(hTitle);
        std::wstring artist = getWindowTextW(hArtist);
        HWND hwndCopy = hwnd;
        provider->searchCandidatesAsync(title, artist,
            [hwndCopy](const std::vector<SearchCandidate>& result) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::vector<SearchCandidate>(result);
                PostMessageW(hwndCopy, kMsgCandidatesReady, 0, reinterpret_cast<LPARAM>(payload));
            });
    }

    void onCandidatesReady(const std::vector<SearchCandidate>& cands) {
        candidates = cands;
        EnableWindow(hSearch, TRUE);
        for (const auto& c : candidates) {
            std::wstring item = c.name + L" - " + c.singer;
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }
        if (!candidates.empty()) {
            SendMessageW(hList, LB_SETCURSEL, 0, 0);
            onSelectionChanged();
        }
    }

    void onSelectionChanged() {
        int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
        if (idx < 0 || idx >= (int)candidates.size())
            return;
        selectedIdx = idx;
        preview.setLyrics({});
        EnableWindow(hOk, FALSE);
        HWND hwndCopy = hwnd;
        int idxCopy = idx;
        provider->fetchLyricAsync(candidates[idx],
            [hwndCopy, idxCopy](bool ok, const std::vector<LyricLine>& lines, const SongInfo& info) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(
                    idxCopy, ok, lines, info);
                PostMessageW(hwndCopy, kMsgPreviewLyricReady, 0,
                             reinterpret_cast<LPARAM>(payload));
            });
    }

    void onPreviewLyricReady(int idx, bool ok, const std::vector<LyricLine>& lines,
                             const SongInfo& info) {
        if (idx != selectedIdx || !ok)
            return;
        previewLines = lines;
        previewInfo = info;
        preview.setLyrics(previewLines);
        EnableWindow(hOk, TRUE);
    }

    void applySelection() {
        if (selectedIdx < 0 || selectedIdx >= (int)candidates.size())
            return;
        if (previewLines.empty())
            return;
        if (provider) {
            provider->setManualOverride(targetTitle, targetArtist,
                                          std::vector<LyricLine>(previewLines), previewInfo);
        }
        if (onApply)
            onApply();
        destroy();
    }

    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

ManualSearchDialog::ManualSearchDialog() : impl_(std::make_unique<Impl>()) {}

ManualSearchDialog::~ManualSearchDialog() {
    destroy();
}

bool ManualSearchDialog::create(HINSTANCE inst, HWND parent, LyricProvider* provider,
                                const std::wstring& targetTitle,
                                const std::wstring& targetArtist) {
    impl_->inst = inst;
    impl_->provider = provider;
    impl_->targetTitle = targetTitle;
    impl_->targetArtist = targetArtist;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricManualSearch";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 900;
    int h = 560;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd =
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, L"QQMusicLyricManualSearch",
                        L"手动搜索歌词", WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE, x, y,
                        w, h, nullptr, nullptr, inst, impl_.get());
    return impl_->hwnd != nullptr;
}

void ManualSearchDialog::show() {
    if (impl_->hwnd)
        ShowWindow(impl_->hwnd, SW_SHOW);
}

void ManualSearchDialog::destroy() {
    impl_->destroy();
}

bool ManualSearchDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND ManualSearchDialog::hwnd() const {
    return impl_->hwnd;
}

void ManualSearchDialog::onCandidatesReady(const std::vector<SearchCandidate>& cands) {
    if (impl_->hwnd)
        PostMessageW(impl_->hwnd, kMsgCandidatesReady, 0,
                     reinterpret_cast<LPARAM>(new std::vector<SearchCandidate>(cands)));
}

void ManualSearchDialog::onPreviewLyricReady(int idx, bool ok,
                                             const std::vector<LyricLine>& lines,
                                             const SongInfo& info) {
    if (impl_->hwnd)
        PostMessageW(impl_->hwnd, kMsgPreviewLyricReady, 0,
                     reinterpret_cast<LPARAM>(
                         new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(idx, ok, lines, info)));
}

void ManualSearchDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}
