#include "ui/settings/search_palette.h"
#include "ui/settings/section_labels.h"
#include "ui/app_icon.h"
#include "ui/settings_icons.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace settings_ui {
bool SettingsSearchPalette::searching() const {
    return searchModel_.active();
}

void SettingsSearchPalette::refreshSearch() {
    if (searchPaletteAnimating && searchPaletteOpen)
        finishSearchPaletteAnimation();
    std::vector<SettingsSearchEntry> entries;
    for (int page = 0; page < kSettingsPageCount; ++page) {
        std::wstring group;
        for (const auto& row : owner_.sections.rows[page]) {
            if (row.kind == ControlKind::Header) {
                group = row.text;
                continue;
            }
            std::wstring text =
                owner_.pageTitles[page] + L" " + group + L" " + row.text + L" " + row.hint;
            for (const auto& option : row.options)
                text += L" " + option;
            entries.push_back({page, row.id,
                               owner_.pageTitles[page] + (group.empty() ? L"" : L" › " + group),
                               std::move(text)});
        }
    }
    searchModel_.refresh(settingsSearch.text(), entries);
    selectedSearchResult = searchModel_.results().empty() ? -1 : 0;
    searchPaletteScroll = 0.0f;
    searchPaletteHoverKind = kSearchPaletteHitNone;
    searchPaletteHoverRow = -1;
    if (searchPaletteOpen) {
        layoutSearchPalette();
        searchPaletteSurface.invalidate();
    }
}

void SettingsSearchPalette::selectSearchResult(int direction) {
    if (searchModel_.results().empty())
        return;
    const int count = static_cast<int>(searchModel_.results().size());
    if (direction == 1 || direction == -1) {
        const int base = selectedSearchResult < 0 ? 0 : selectedSearchResult;
        selectedSearchResult = (base + direction + count) % count;
    } else {
        selectedSearchResult = std::clamp(selectedSearchResult + direction, 0, count - 1);
    }
    const float rowTop = kSearchPaletteListPadY + selectedSearchResult * kSearchPaletteRowH;
    const float rowBottom = rowTop + kSearchPaletteRowH + kSearchPaletteListPadY;
    const float viewportHeight =
        std::max(0.0f, searchPaletteListRect.bottom - searchPaletteListRect.top);
    if (rowTop < searchPaletteScroll + kSearchPaletteListPadY) {
        searchPaletteScroll =
            std::clamp(rowTop - kSearchPaletteListPadY, 0.0f, searchPaletteMaxScroll);
    } else if (rowBottom > searchPaletteScroll + viewportHeight) {
        searchPaletteScroll = std::clamp(rowBottom - viewportHeight, 0.0f, searchPaletteMaxScroll);
    }
    searchPaletteSurface.invalidate();
}

void SettingsSearchPalette::openSearchResult(int index) {
    if (index < 0 || index >= static_cast<int>(searchModel_.results().size()))
        return;
    const auto result = searchModel_.results()[index];
    closeSearchPalette(true);
    onSelect_(result);
}

LRESULT CALLBACK SettingsSearchPalette::searchEditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                                                       UINT_PTR subclassId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<SettingsSearchPalette*>(refData);
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(edit, &SettingsSearchPalette::searchEditProc, subclassId);
        return DefSubclassProc(edit, msg, wp, lp);
    }
    if (self && msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            self->closeSearchPalette(true);
            return 0;
        }
        if (wp == VK_RETURN && self->searching() && self->selectedSearchResult >= 0) {
            self->openSearchResult(self->selectedSearchResult);
            return 0;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && self->searching()) {
            self->selectSearchResult(wp == VK_DOWN ? 1 : -1);
            return 0;
        }
        if ((wp == VK_PRIOR || wp == VK_NEXT) && self->searching()) {
            self->selectSearchResult(wp == VK_NEXT ? 5 : -5);
            return 0;
        }
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wp == VK_TAB) {
            return 0;
        }
    }
    if (self && msg == WM_MOUSEWHEEL && self->searchPaletteHwnd) {
        SendMessageW(self->searchPaletteHwnd, msg, wp, lp);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 6))
        return 0;
    return DefSubclassProc(edit, msg, wp, lp);
}

void SettingsSearchPalette::styleSearchPaletteWindow() {
    if (!searchPaletteHwnd)
        return;
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(searchPaletteHwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions,
                          sizeof(disableTransitions));
    fluent::applyRoundCorners(searchPaletteHwnd, false);
    const bool dark = fluent::isDarkMode();
    fluent::applyDarkCaption(searchPaletteHwnd, dark);
    fluent::applyBorderColor(searchPaletteHwnd, dark ? RGB(68, 68, 68) : RGB(214, 214, 214));
}

void SettingsSearchPalette::setWindowClickThrough(HWND window, bool clickThrough) {
    if (!window || !IsWindow(window))
        return;
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const LONG_PTR nextStyle = clickThrough ? (exStyle | WS_EX_TRANSPARENT)
                                            : (exStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (nextStyle != exStyle)
        SetWindowLongPtrW(window, GWL_EXSTYLE, nextStyle);
}

void SettingsSearchPalette::updateSearchPaletteProgress() {
    if (!searchPaletteAnimating)
        return;
    const float elapsed = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - searchPaletteAnimStarted).count();
    const float t = std::clamp(elapsed / searchPaletteAnimDurationMs, 0.0f, 1.0f);
    // 与合成器的缓出曲线一致，仅用于中断和完成判定。
    const float inverse = 1.0f - t;
    const float eased = searchPaletteAnimTarget > 0.5f
                            ? 1.0f - inverse * inverse * inverse
                            : 1.0f - inverse * inverse;
    searchPaletteProgress = searchPaletteAnimFrom +
                            (searchPaletteAnimTarget - searchPaletteAnimFrom) * eased;
}

bool SettingsSearchPalette::startSearchPaletteAnimation() {
    if (!searchAnimationHwnd) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &SettingsSearchPalette::animationWndProc;
        windowClass.hInstance = owner_.inst;
        windowClass.lpszClassName = L"QQMusicLyricSettingsSearchAnimation";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&windowClass);
        searchAnimationHwnd = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
            owner_.hwnd, nullptr, owner_.inst, this);
        if (searchAnimationHwnd) {
            const BOOL disableTransitions = TRUE;
            DwmSetWindowAttribute(searchAnimationHwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                                  &disableTransitions, sizeof(disableTransitions));
        }
    }
    RECT client{};
    if (!searchAnimationHwnd || !GetClientRect(searchPaletteHwnd, &client))
        return false;
    const int margin = static_cast<int>(std::ceil(
        std::max(std::abs(kSearchPaletteOpenSlideDip), std::abs(kSearchPaletteCloseSlideDip)) *
        searchPaletteScale));
    SetWindowPos(searchAnimationHwnd, HWND_TOP, searchPaletteBaseX, searchPaletteBaseY - margin,
                 client.right, client.bottom + margin, SWP_NOACTIVATE);
    const UINT dpi = GetDpiForWindow(searchPaletteHwnd);
    // 原生子控件也必须完成首帧，不能只更新排除了 EDIT 的父窗口表面。
    RedrawWindow(searchPaletteHwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    searchAnimationRenderer.setDpi(dpi);
    if (!searchAnimationRenderer.bind(searchAnimationHwnd, client.right, client.bottom + margin) ||
        !searchAnimationRenderer.setWindowContent(searchPaletteHwnd) ||
        !searchAnimationRenderer.setRootRoundedClip(
            0.0f, static_cast<float>(client.bottom), fluent::metrics::cardRadius * searchPaletteScale))
        return false;
    const float slide = (searchPaletteAnimTarget > 0.5f ? kSearchPaletteOpenSlideDip
                                                       : kSearchPaletteCloseSlideDip) *
                        searchPaletteScale;
    const float fromY = margin + (1.0f - searchPaletteAnimFrom) * slide;
    const float toY = margin + (1.0f - searchPaletteAnimTarget) * slide;
    const float duration = searchPaletteAnimDurationMs / 1000.0f;
    const auto curve = searchPaletteAnimTarget > 0.5f
                           ? DCompRenderer::RootAnimationCurve::EaseOutCubic
                           : DCompRenderer::RootAnimationCurve::EaseOutQuadratic;
    if (!searchAnimationRenderer.animateRoot(0.0f, 0.0f, fromY, toY,
                                             searchPaletteAnimFrom, searchPaletteAnimTarget, duration, curve) ||
        !searchScrimRenderer.animateRoot(0.0f, 0.0f, 0.0f, 0.0f,
                                         searchPaletteAnimFrom, searchPaletteAnimTarget, duration, curve))
        return false;
    // Cloak 只隐藏原窗口，保留其原生 EDIT 的实时表面和键盘输入。
    const BOOL cloak = TRUE;
    if (FAILED(DwmSetWindowAttribute(searchPaletteHwnd, DWMWA_CLOAK, &cloak, sizeof(cloak))))
        return false;
    searchAnimationRenderer.commit();
    searchScrimRenderer.commit();
    ShowWindow(searchAnimationHwnd, SW_SHOWNOACTIVATE);
    searchPaletteAnimStarted = std::chrono::steady_clock::now();
    searchPaletteAnimating = true;
    // 定时器只做最终交接，逐帧位移和透明度由合成器按刷新率推进。
    return SetTimer(owner_.hwnd, kSearchPaletteAnimTimer,
                    static_cast<UINT>(std::ceil(searchPaletteAnimDurationMs)), nullptr) != 0;
}

void SettingsSearchPalette::finishSearchPaletteAnimation() {
    if (owner_.hwnd)
        KillTimer(owner_.hwnd, kSearchPaletteAnimTimer);
    searchPaletteAnimating = false;
    searchPaletteProgress = 1.0f;
    const BOOL cloak = FALSE;
    if (searchPaletteHwnd)
        DwmSetWindowAttribute(searchPaletteHwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (searchAnimationHwnd)
        ShowWindow(searchAnimationHwnd, SW_HIDE);
    searchScrimRenderer.resetRoot();
    searchScrimRenderer.commit();
}

LRESULT CALLBACK SettingsSearchPalette::animationWndProc(HWND window, UINT message,
                                                         WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SettingsSearchPalette*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SettingsSearchPalette*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        if (!self->searchPaletteOpen)
            return HTTRANSPARENT;
        self->updateSearchPaletteProgress();
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(self->searchPaletteHwnd, &point);
        const float slide = kSearchPaletteOpenSlideDip * self->searchPaletteScale;
        const float y = point.y - (1.0f - self->searchPaletteProgress) * slide;
        RECT client{};
        GetClientRect(self->searchPaletteHwnd, &client);
        return point.x >= 0 && point.x < client.right && y >= 0.0f && y < client.bottom
                   ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        if (!self->searchPaletteOpen)
            return 0;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(window, &point);
        ScreenToClient(self->searchPaletteHwnd, &point);
        self->updateSearchPaletteProgress();
        point.y -= static_cast<LONG>(std::lround(
            (1.0f - self->searchPaletteProgress) * kSearchPaletteOpenSlideDip * self->searchPaletteScale));
        self->finishSearchPaletteAnimation();
        SetActiveWindow(self->searchPaletteHwnd);
        self->settingsSearch.focus();
        HWND target = self->searchPaletteHwnd;
        for (;;) {
            HWND child = ChildWindowFromPointEx(target, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (!child || child == target)
                break;
            MapWindowPoints(target, child, &point, 1);
            target = child;
        }
        SendMessageW(target, message, wp, MAKELPARAM(point.x, point.y));
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (self->searchPaletteOpen) {
            self->finishSearchPaletteAnimation();
            SendMessageW(self->searchPaletteHwnd, message, wp, lp);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wp, lp);
    }
    return DefWindowProcW(window, message, wp, lp);
}

void SettingsSearchPalette::invalidateSearchTrigger() {
    const float scale = owner_.surface.dipScale();
    const RECT dirty{static_cast<LONG>(std::floor(searchTriggerRect.left * scale)) - 2,
                     static_cast<LONG>(std::floor(searchTriggerRect.top * scale)) - 2,
                     static_cast<LONG>(std::ceil(searchTriggerRect.right * scale)) + 2,
                     static_cast<LONG>(std::ceil(searchTriggerRect.bottom * scale)) + 2};
    owner_.surface.invalidate(&dirty);
}

void SettingsSearchPalette::finalizeSearchPaletteClosed() {
    if (owner_.hwnd)
        KillTimer(owner_.hwnd, kSearchPaletteAnimTimer);
    searchPaletteAnimating = false;
    searchPaletteProgress = 0.0f;
    searchPaletteAnimFrom = 0.0f;
    searchPaletteAnimTarget = 0.0f;
    closingSearchPalette = true;
    if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
        ShowWindow(searchPaletteHwnd, SW_HIDE);
        const BOOL cloak = FALSE;
        DwmSetWindowAttribute(searchPaletteHwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        setWindowClickThrough(searchPaletteHwnd, false);
        settingsSearch.setText(L"");
    }
    if (searchAnimationHwnd)
        ShowWindow(searchAnimationHwnd, SW_HIDE);
    if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
        ShowWindow(searchScrimHwnd, SW_HIDE);
        setWindowClickThrough(searchScrimHwnd, false);
    }
    searchModel_.clear();
    selectedSearchResult = -1;
    searchPaletteScroll = 0.0f;
    searchPaletteMaxScroll = 0.0f;
    closingSearchPalette = false;
}

void SettingsSearchPalette::advanceSearchPaletteAnimation() {
    if (!searchPaletteAnimating)
        return;
    updateSearchPaletteProgress();
    const float elapsed = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - searchPaletteAnimStarted).count();
    if (elapsed < searchPaletteAnimDurationMs) {
        SetTimer(owner_.hwnd, kSearchPaletteAnimTimer,
                 static_cast<UINT>(std::ceil(searchPaletteAnimDurationMs - elapsed)), nullptr);
        return;
    }
    if (searchPaletteAnimTarget <= 0.5f)
        finalizeSearchPaletteClosed();
    else
        finishSearchPaletteAnimation();
}

float SettingsSearchPalette::searchPaletteDesiredHeightDip() const {
    if (!searching())
        return kSearchPaletteHeaderH + kSearchPaletteEmptyPromptH;
    if (searchModel_.results().empty())
        return kSearchPaletteHeaderH + kSearchPaletteNoResultH;
    const int visibleCount =
        std::min(static_cast<int>(searchModel_.results().size()), kSearchPaletteMaxVisibleRows);
    const float bodyHeight = kSearchPaletteListPadY * 2.0f + visibleCount * kSearchPaletteRowH;
    return kSearchPaletteHeaderH + bodyHeight + kSearchPaletteFooterH;
}

void SettingsSearchPalette::layoutSearchPalette() {
    if (searchPaletteAnimating) {
        if (searchPaletteOpen)
            finishSearchPaletteAnimation();
        else {
            finalizeSearchPaletteClosed();
            return;
        }
    }
    if (!searchPaletteHwnd || !owner_.hwnd || !IsWindow(owner_.hwnd))
        return;
    RECT ownerClient{};
    GetClientRect(owner_.hwnd, &ownerClient);
    POINT topLeft{ownerClient.left, ownerClient.top};
    POINT bottomRight{ownerClient.right, ownerClient.bottom};
    ClientToScreen(owner_.hwnd, &topLeft);
    ClientToScreen(owner_.hwnd, &bottomRight);

    UINT dpi = GetDpiForWindow(searchPaletteHwnd);
    if (dpi == 0)
        dpi = GetDpiForWindow(owner_.hwnd);
    if (dpi == 0)
        dpi = 96;
    const float scale = fluent::dipScale(dpi);
    auto toPx = [scale](float dipValue) { return static_cast<int>(std::lround(dipValue * scale)); };

    const int ownerWidthPx = std::max(1, static_cast<int>(bottomRight.x - topLeft.x));
    const int ownerHeightPx = std::max(1, static_cast<int>(bottomRight.y - topLeft.y));
    const float ownerWidthDip = static_cast<float>(ownerWidthPx) / scale;
    const float ownerHeightDip = static_cast<float>(ownerHeightPx) / scale;

    const float maxAllowedWidthDip = std::max(320.0f, ownerWidthDip - 48.0f);
    const float paletteWidthDip =
        std::min(maxAllowedWidthDip, std::clamp(ownerWidthDip * 0.58f, 480.0f, 560.0f));
    const float maxAllowedHeightDip = std::max(132.0f, ownerHeightDip - 88.0f);
    const float paletteHeightDip = std::min(searchPaletteDesiredHeightDip(), maxAllowedHeightDip);

    const bool hasInputText =
        settingsSearch.editHwnd() && GetWindowTextLengthW(settingsSearch.editHwnd()) > 0;
    searchPaletteEscRect =
        D2D1::RectF(paletteWidthDip - 48.0f, 16.0f, paletteWidthDip - 16.0f, 36.0f);
    searchPaletteClearRect =
        hasInputText ? D2D1::RectF(paletteWidthDip - 80.0f, 14.0f, paletteWidthDip - 56.0f, 38.0f)
                     : D2D1_RECT_F{};
    const float editLeftDip = 46.0f;
    const float editRightDip =
        hasInputText ? searchPaletteClearRect.left - 8.0f : searchPaletteEscRect.left - 10.0f;

    if (searching() && !searchModel_.results().empty()) {
        const float listTop = kSearchPaletteHeaderH;
        const float listBottom = std::max(listTop, paletteHeightDip - kSearchPaletteFooterH);
        searchPaletteListRect = D2D1::RectF(0.0f, listTop, paletteWidthDip, listBottom);
        const float totalListHeight =
            kSearchPaletteListPadY * 2.0f +
            static_cast<float>(searchModel_.results().size()) * kSearchPaletteRowH;
        const float viewportHeight = std::max(0.0f, listBottom - listTop);
        searchPaletteMaxScroll = std::max(0.0f, totalListHeight - viewportHeight);
        searchPaletteScroll = std::clamp(searchPaletteScroll, 0.0f, searchPaletteMaxScroll);
    } else {
        searchPaletteListRect = D2D1_RECT_F{};
        searchPaletteMaxScroll = 0.0f;
        searchPaletteScroll = 0.0f;
    }

    const int paletteWidthPx = std::max(1, toPx(paletteWidthDip));
    const int paletteHeightPx = std::max(1, toPx(paletteHeightDip));
    int screenX = topLeft.x + (ownerWidthPx - paletteWidthPx) / 2;
    int screenY = topLeft.y + toPx(64.0f);

    HMONITOR monitor = MonitorFromWindow(owner_.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const bool hasWorkArea = monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
    if (hasWorkArea) {
        const RECT& workArea = monitorInfo.rcWork;
        if (screenX + paletteWidthPx > workArea.right - 8)
            screenX = workArea.right - 8 - paletteWidthPx;
        if (screenX < workArea.left + 8)
            screenX = workArea.left + 8;
        if (screenY + paletteHeightPx > workArea.bottom - 8)
            screenY = workArea.bottom - 8 - paletteHeightPx;
        if (screenY < workArea.top + 8)
            screenY = workArea.top + 8;
    }

    searchPaletteBaseX = screenX;
    searchPaletteBaseY = screenY;
    searchPaletteScale = scale;
    SetWindowPos(searchPaletteHwnd, HWND_TOP, screenX, screenY, paletteWidthPx,
                 paletteHeightPx, SWP_NOACTIVATE);
    if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
        RECT scrimBounds{};
        if (FAILED(DwmGetWindowAttribute(owner_.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &scrimBounds,
                                         sizeof(scrimBounds))) ||
            scrimBounds.right <= scrimBounds.left || scrimBounds.bottom <= scrimBounds.top) {
            scrimBounds = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        } else if (IsZoomed(owner_.hwnd) != FALSE && hasWorkArea) {
            scrimBounds.left = std::max(scrimBounds.left, monitorInfo.rcWork.left);
            scrimBounds.top = std::max(scrimBounds.top, monitorInfo.rcWork.top);
            scrimBounds.right = std::min(scrimBounds.right, monitorInfo.rcWork.right);
            scrimBounds.bottom = std::min(scrimBounds.bottom, monitorInfo.rcWork.bottom);
        }
        const int scrimWidthPx =
            std::max(1, static_cast<int>(scrimBounds.right - scrimBounds.left));
        const int scrimHeightPx =
            std::max(1, static_cast<int>(scrimBounds.bottom - scrimBounds.top));
        SetWindowPos(searchScrimHwnd, searchPaletteHwnd, scrimBounds.left, scrimBounds.top,
                     scrimWidthPx, scrimHeightPx, SWP_NOACTIVATE);
        renderSearchScrim(scrimWidthPx, scrimHeightPx, dpi);
    }
    if (settingsSearch.hwnd()) {
        settingsSearch.move(toPx(editLeftDip), toPx(10.0f),
                            std::max(1, toPx(editRightDip - editLeftDip)), toPx(32.0f));
    }
}

void SettingsSearchPalette::renderSearchScrim(int widthPx, int heightPx, UINT dpi) {
    if (!searchScrimHwnd || widthPx <= 0 || heightPx <= 0)
        return;
    const bool dark = fluent::isDarkMode();
    const bool zoomed = owner_.hwnd && IsZoomed(owner_.hwnd) != FALSE;
    if (!searchScrimRenderer.initialize())
        return;
    searchScrimRenderer.setDpi(dpi);
    if (!searchScrimRenderer.bind(searchScrimHwnd, widthPx, heightPx)) {
        searchScrimLastWidth = 0;
        return;
    }
    searchScrimRenderer.setRootOpacity(searchPaletteProgress);
    if (widthPx == searchScrimLastWidth && heightPx == searchScrimLastHeight &&
        dpi == searchScrimLastDpi && dark == searchScrimLastDark &&
        zoomed == searchScrimLastZoomed &&
        searchScrimLastDeviceGeneration == searchScrimRenderer.deviceGeneration()) {
        searchScrimRenderer.commit();
        return;
    }
    auto* target = searchScrimRenderer.renderTarget();
    if (!target)
        return;

    const float scale = fluent::dipScale(dpi);
    const float widthDip = static_cast<float>(widthPx) / scale;
    const float heightDip = static_cast<float>(heightPx) / scale;
    DWORD cornerPreference = 0;
    constexpr DWORD kWindowCornerPreferenceAttr = 33;
    const bool roundedWindow =
        !zoomed && owner_.hwnd &&
        SUCCEEDED(DwmGetWindowAttribute(owner_.hwnd, kWindowCornerPreferenceAttr, &cornerPreference,
                                        sizeof(cornerPreference))) &&
        cornerPreference != 1;
    const float cornerRadius = roundedWindow ? std::min(fluent::metrics::cardRadius,
                                                        std::min(widthDip * 0.5f, heightDip * 0.5f))
                                             : 0.0f;
    const D2D1_COLOR_F scrimColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, dark ? 0.46f : 0.28f);

    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    ID2D1SolidColorBrush* scrimBrush = nullptr;
    if (SUCCEEDED(target->CreateSolidColorBrush(scrimColor, &scrimBrush)) && scrimBrush) {
        if (cornerRadius > 0.0f) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip), cornerRadius,
                                  cornerRadius),
                scrimBrush);
        } else {
            target->FillRectangle(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip), scrimBrush);
        }
        scrimBrush->Release();
    }
    if (FAILED(target->EndDraw())) {
        searchScrimRenderer.discard();
        searchScrimLastWidth = 0;
        return;
    }
    if (searchScrimRenderer.present()) {
        searchScrimLastDeviceGeneration = searchScrimRenderer.deviceGeneration();
        searchScrimLastWidth = widthPx;
        searchScrimLastHeight = heightPx;
        searchScrimLastDpi = dpi;
        searchScrimLastDark = dark;
        searchScrimLastZoomed = zoomed;
    } else {
        searchScrimRenderer.discard();
        searchScrimLastWidth = 0;
    }
}

LRESULT CALLBACK SettingsSearchPalette::searchScrimWndProc(HWND window, UINT message, WPARAM wp,
                                                           LPARAM lp) {
    SettingsSearchPalette* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<SettingsSearchPalette*>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->searchScrimHwnd = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsSearchPalette*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (!self)
        return DefWindowProcW(window, message, wp, lp);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST:
        // 普通合成窗口的 WS_EX_TRANSPARENT 不负责鼠标穿透。
        return self->searchPaletteOpen ? HTCLIENT : HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        self->closeSearchPalette(true);
        return 0;
    case WM_MOUSEWHEEL:
        if (self->searchPaletteOpen && self->searchPaletteHwnd)
            SendMessageW(self->searchPaletteHwnd, message, wp, lp);
        return 0;
    case WM_DESTROY:
        self->searchScrimRenderer.releaseAll();
        self->searchScrimLastWidth = 0;
        self->searchScrimLastHeight = 0;
        self->searchScrimHwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

bool SettingsSearchPalette::ensureSearchScrimWindow() {
    if (searchScrimHwnd && IsWindow(searchScrimHwnd))
        return true;
    WNDCLASSEXW scrimClass{};
    scrimClass.cbSize = sizeof(scrimClass);
    scrimClass.lpfnWndProc = &SettingsSearchPalette::searchScrimWndProc;
    scrimClass.hInstance = owner_.inst;
    scrimClass.lpszClassName = L"QQMusicLyricSettingsSearchScrim";
    scrimClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&scrimClass);

    searchScrimHwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                      L"QQMusicLyricSettingsSearchScrim", L"", WS_POPUP, 0, 0, 1, 1,
                                      owner_.hwnd, nullptr, owner_.inst, this);
    searchScrimLastWidth = 0;
    searchScrimLastHeight = 0;
    return searchScrimHwnd != nullptr;
}

int SettingsSearchPalette::searchPaletteHitTest(float xDip, float yDip, int* outRow) const {
    if (outRow)
        *outRow = -1;
    if (searchPaletteClearRect.right > searchPaletteClearRect.left &&
        contains(searchPaletteClearRect, xDip, yDip))
        return kSearchPaletteHitClear;
    if (contains(searchPaletteEscRect, xDip, yDip))
        return kSearchPaletteHitClose;
    if (searching() && !searchModel_.results().empty() &&
        contains(searchPaletteListRect, xDip, yDip)) {
        const float listOffset =
            yDip - searchPaletteListRect.top + searchPaletteScroll - kSearchPaletteListPadY;
        if (listOffset >= 0.0f) {
            const int rowIndex = static_cast<int>(listOffset / kSearchPaletteRowH);
            const float withinRow = listOffset - rowIndex * kSearchPaletteRowH;
            if (rowIndex >= 0 && rowIndex < static_cast<int>(searchModel_.results().size()) &&
                withinRow >= 2.0f && withinRow <= kSearchPaletteRowH - 2.0f && xDip >= 8.0f &&
                xDip <= searchPaletteListRect.right - 8.0f) {
                if (outRow)
                    *outRow = rowIndex;
                return kSearchPaletteHitRow;
            }
        }
    }
    return kSearchPaletteHitNone;
}

void SettingsSearchPalette::paintSearchPalette(fluent::FluentDialogSurface::Painter& painter,
                                               float widthDip, float heightDip) {
    const auto& paletteColors = fluent::palette();
    if (auto* backgroundBrush = painter.brush(paletteColors.menuFill)) {
        painter.target()->FillRectangle(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip),
                                        backgroundBrush);
    }
    painter.strokeRoundRect(paletteColors.menuStroke,
                            D2D1::RectF(0.5f, 0.5f, widthDip - 0.5f, heightDip - 0.5f), 1.0f,
                            fluent::metrics::cardRadius);

    if (auto* searchBrush = painter.brush(paletteColors.accent)) {
        settings_icon::draw(painter.target(), settings_icon::Kind::Search,
                            D2D1::RectF(18.0f, 17.0f, 36.0f, 35.0f), searchBrush, 1.4f);
    }

    if (searchPaletteClearRect.right > searchPaletteClearRect.left) {
        const bool clearHovered = searchPaletteHoverKind == kSearchPaletteHitClear;
        const bool clearPressed = searchPalettePressedKind == kSearchPaletteHitClear;
        if (clearPressed || clearHovered) {
            painter.fillRoundRect(clearPressed ? paletteColors.listSelected
                                               : paletteColors.listHover,
                                  searchPaletteClearRect, 5.0f);
        }
        const float centerX = (searchPaletteClearRect.left + searchPaletteClearRect.right) * 0.5f;
        const float centerY = (searchPaletteClearRect.top + searchPaletteClearRect.bottom) * 0.5f;
        if (auto* crossBrush =
                painter.brush(clearHovered ? paletteColors.text : paletteColors.textSecondary)) {
            painter.target()->DrawLine(D2D1::Point2F(centerX - 4.0f, centerY - 4.0f),
                                       D2D1::Point2F(centerX + 4.0f, centerY + 4.0f), crossBrush,
                                       1.35f);
            painter.target()->DrawLine(D2D1::Point2F(centerX + 4.0f, centerY - 4.0f),
                                       D2D1::Point2F(centerX - 4.0f, centerY + 4.0f), crossBrush,
                                       1.35f);
        }
    }

    const bool escHovered = searchPaletteHoverKind == kSearchPaletteHitClose;
    const bool escPressed = searchPalettePressedKind == kSearchPaletteHitClose;
    painter.fillRoundRect(escPressed
                              ? paletteColors.listSelected
                              : (escHovered ? paletteColors.controlHover : paletteColors.listHover),
                          searchPaletteEscRect, 4.0f);
    painter.strokeRoundRect(paletteColors.cardStroke, searchPaletteEscRect, 1.0f, 4.0f);
    painter.drawText(L"Esc", painter.textFormat(11.0f, 500, true, true), searchPaletteEscRect,
                     escHovered ? paletteColors.text : paletteColors.textSecondary);

    if (auto* separatorBrush = painter.brush(paletteColors.separator)) {
        painter.target()->DrawLine(D2D1::Point2F(0.0f, kSearchPaletteHeaderH - 0.5f),
                                   D2D1::Point2F(widthDip, kSearchPaletteHeaderH - 0.5f),
                                   separatorBrush, 1.0f);
    }

    if (!searching()) {
        painter.drawText(L"输入关键词搜索设置名称、选项或说明，支持空格分词筛选",
                         painter.textFormat(13.0f, 400, false, true),
                         D2D1::RectF(20.0f, kSearchPaletteHeaderH, widthDip - 20.0f, heightDip),
                         paletteColors.textSecondary);
        return;
    }

    if (searchModel_.results().empty()) {
        painter.drawText(L"未找到匹配的设置，请尝试其他关键词",
                         painter.textFormat(13.5f, 400, false, true),
                         D2D1::RectF(20.0f, kSearchPaletteHeaderH, widthDip - 20.0f, heightDip),
                         paletteColors.textSecondary);
        return;
    }

    painter.target()->PushAxisAlignedClip(searchPaletteListRect, D2D1_ANTIALIAS_MODE_ALIASED);
    const bool hasScrollbar = searchPaletteMaxScroll > 0.5f;
    const float itemRight = widthDip - (hasScrollbar ? 14.0f : 8.0f);
    for (int index = 0; index < static_cast<int>(searchModel_.results().size()); ++index) {
        const float rowTop = searchPaletteListRect.top + kSearchPaletteListPadY +
                             index * kSearchPaletteRowH - searchPaletteScroll;
        if (rowTop + kSearchPaletteRowH < searchPaletteListRect.top ||
            rowTop > searchPaletteListRect.bottom)
            continue;
        const auto& result = searchModel_.results()[index];
        const auto* row = owner_.sections.findRow(result.id);
        if (!row)
            continue;

        const D2D1_RECT_F itemRect =
            D2D1::RectF(8.0f, rowTop + 2.0f, itemRight, rowTop + kSearchPaletteRowH - 2.0f);
        const bool selected = index == selectedSearchResult;
        const bool hovered =
            searchPaletteHoverKind == kSearchPaletteHitRow && searchPaletteHoverRow == index;
        if (selected) {
            painter.fillRoundRect(paletteColors.listSelected, itemRect, 6.0f);
            const float midY = (itemRect.top + itemRect.bottom) * 0.5f;
            painter.fillRoundRect(
                paletteColors.accent,
                D2D1::RectF(itemRect.left + 4.0f, midY - 8.0f, itemRect.left + 7.0f, midY + 8.0f),
                1.5f);
        } else if (hovered) {
            painter.fillRoundRect(paletteColors.listHover, itemRect, 6.0f);
        }

        const float iconCenterY = (itemRect.top + itemRect.bottom) * 0.5f;
        const D2D1_RECT_F iconBadge = D2D1::RectF(itemRect.left + 14.0f, iconCenterY - 15.0f,
                                                  itemRect.left + 44.0f, iconCenterY + 15.0f);
        const D2D1_COLOR_F badgeFill =
            selected ? D2D1::ColorF(paletteColors.accent.r, paletteColors.accent.g,
                                    paletteColors.accent.b, 0.12f)
                     : paletteColors.listHover;
        painter.fillRoundRect(badgeFill, iconBadge, 6.0f);
        if (auto* iconBrush =
                painter.brush(selected ? paletteColors.accent : paletteColors.textSecondary)) {
            settings_icon::draw(painter.target(), iconForRow(result.id),
                                D2D1::RectF(iconBadge.left + 7.0f, iconBadge.top + 7.0f,
                                            iconBadge.right - 7.0f, iconBadge.bottom - 7.0f),
                                iconBrush, 1.25f);
        }

        const float textRight = selected ? itemRect.right - 56.0f : itemRect.right - 14.0f;
        painter.drawTrimmedText(row->text,
                                painter.textFormat(13.5f, selected ? 600 : 500, false, true),
                                D2D1::RectF(itemRect.left + 54.0f, itemRect.top + 6.0f, textRight,
                                            itemRect.top + 28.0f),
                                paletteColors.text);
        painter.drawTrimmedText(result.path, painter.textFormat(12.0f, 400, false, true),
                                D2D1::RectF(itemRect.left + 54.0f, itemRect.top + 27.0f, textRight,
                                            itemRect.bottom - 5.0f),
                                paletteColors.textSecondary);
        if (selected) {
            painter.drawText(L"跳转 ↵", painter.textFormat(11.5f, 500, true, true),
                             D2D1::RectF(itemRect.right - 54.0f, itemRect.top,
                                         itemRect.right - 10.0f, itemRect.bottom),
                             paletteColors.accent);
        }
    }

    if (hasScrollbar) {
        const float trackTop = searchPaletteListRect.top + 6.0f;
        const float trackBottom = searchPaletteListRect.bottom - 6.0f;
        const float trackHeight = std::max(12.0f, trackBottom - trackTop);
        const float viewportHeight =
            std::max(1.0f, searchPaletteListRect.bottom - searchPaletteListRect.top);
        const float totalHeight = viewportHeight + searchPaletteMaxScroll;
        const float thumbHeight = std::max(20.0f, trackHeight * (viewportHeight / totalHeight));
        const float scrollRatio =
            std::clamp(searchPaletteScroll / searchPaletteMaxScroll, 0.0f, 1.0f);
        const float thumbTop = trackTop + (trackHeight - thumbHeight) * scrollRatio;
        painter.fillRoundRect(
            paletteColors.controlStroke,
            D2D1::RectF(widthDip - 8.0f, thumbTop, widthDip - 4.0f, thumbTop + thumbHeight), 2.0f);
    }
    painter.target()->PopAxisAlignedClip();

    const float footerTop = heightDip - kSearchPaletteFooterH;
    if (auto* separatorBrush = painter.brush(paletteColors.separator)) {
        painter.target()->DrawLine(D2D1::Point2F(0.0f, footerTop + 0.5f),
                                   D2D1::Point2F(widthDip, footerTop + 0.5f), separatorBrush, 1.0f);
    }
    auto* footerFormat = painter.textFormat(11.5f, 400, false, true);
    painter.drawText(L"↑↓ 选择    Enter 跳转    Esc 关闭", footerFormat,
                     D2D1::RectF(16.0f, footerTop, widthDip - 100.0f, heightDip),
                     paletteColors.textSecondary);
    const std::wstring countLabel =
        L"共 " + std::to_wstring(searchModel_.results().size()) + L" 项";
    const float countWidth = painter.measureTextWidth(countLabel, footerFormat);
    painter.drawText(countLabel, footerFormat,
                     D2D1::RectF(std::max(16.0f, widthDip - 16.0f - countWidth - 2.0f), footerTop,
                                 widthDip - 14.0f, heightDip),
                     paletteColors.textSecondary);
}

LRESULT CALLBACK SettingsSearchPalette::searchPaletteWndProc(HWND window, UINT message, WPARAM wp,
                                                             LPARAM lp) {
    SettingsSearchPalette* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<SettingsSearchPalette*>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->searchPaletteHwnd = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsSearchPalette*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (!self)
        return DefWindowProcW(window, message, wp, lp);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paintStruct{};
        HDC dc = BeginPaint(window, &paintStruct);
        self->searchPaletteSurface.paint(
            dc, false,
            [self](fluent::FluentDialogSurface::Painter& painter, float widthDip, float heightDip) {
                self->paintSearchPalette(painter, widthDip, heightDip);
            });
        EndPaint(window, &paintStruct);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT trackEvent{sizeof(trackEvent), TME_LEAVE, window, 0};
        TrackMouseEvent(&trackEvent);
        const POINT currentPt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool pointerMoved = currentPt.x != self->searchPaletteLastMovePt.x ||
                                  currentPt.y != self->searchPaletteLastMovePt.y;
        self->searchPaletteLastMovePt = currentPt;
        const float scale = self->searchPaletteSurface.dipScale();
        int hitRow = -1;
        const int hitKind =
            self->searchPaletteHitTest(static_cast<float>(currentPt.x) / scale,
                                       static_cast<float>(currentPt.y) / scale, &hitRow);
        bool changed =
            hitKind != self->searchPaletteHoverKind || hitRow != self->searchPaletteHoverRow;
        self->searchPaletteHoverKind = hitKind;
        self->searchPaletteHoverRow = hitRow;
        if (pointerMoved && hitKind == kSearchPaletteHitRow && hitRow >= 0 &&
            hitRow != self->selectedSearchResult) {
            self->selectedSearchResult = hitRow;
            changed = true;
        }
        if (changed)
            self->searchPaletteSurface.invalidate();
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self->searchPaletteHoverKind != kSearchPaletteHitNone ||
            self->searchPaletteHoverRow != -1) {
            self->searchPaletteHoverKind = kSearchPaletteHitNone;
            self->searchPaletteHoverRow = -1;
            self->searchPaletteSurface.invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const float scale = self->searchPaletteSurface.dipScale();
        int hitRow = -1;
        const int hitKind =
            self->searchPaletteHitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, &hitRow);
        self->searchPalettePressedKind = hitKind;
        self->searchPalettePressedRow = hitRow;
        if (hitKind != kSearchPaletteHitNone)
            SetCapture(window);
        self->settingsSearch.focus();
        self->searchPaletteSurface.invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float scale = self->searchPaletteSurface.dipScale();
        int hitRow = -1;
        const int hitKind =
            self->searchPaletteHitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, &hitRow);
        const int pressedKind = self->searchPalettePressedKind;
        const int pressedRow = self->searchPalettePressedRow;
        self->searchPalettePressedKind = kSearchPaletteHitNone;
        self->searchPalettePressedRow = -1;
        if (GetCapture() == window)
            ReleaseCapture();
        if (pressedKind == kSearchPaletteHitClear && hitKind == kSearchPaletteHitClear) {
            self->settingsSearch.setText(L"");
            self->refreshSearch();
            self->settingsSearch.focus();
            return 0;
        }
        if (pressedKind == kSearchPaletteHitClose && hitKind == kSearchPaletteHitClose) {
            self->closeSearchPalette(true);
            return 0;
        }
        if (pressedKind == kSearchPaletteHitRow && hitKind == kSearchPaletteHitRow &&
            pressedRow == hitRow && hitRow >= 0) {
            self->openSearchResult(hitRow);
            return 0;
        }
        self->searchPaletteSurface.invalidate();
        return 0;
    }
    case WM_CAPTURECHANGED:
        self->searchPalettePressedKind = kSearchPaletteHitNone;
        self->searchPalettePressedRow = -1;
        self->searchPaletteSurface.invalidate();
        return 0;
    case WM_MOUSEWHEEL: {
        if (self->searchPaletteMaxScroll > 0.0f) {
            const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wp);
            self->searchPaletteScroll =
                std::clamp(self->searchPaletteScroll - static_cast<float>(wheelDelta) /
                                                           static_cast<float>(WHEEL_DELTA) * 48.0f,
                           0.0f, self->searchPaletteMaxScroll);
            self->searchPaletteSurface.invalidate();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            self->closeSearchPalette(true);
            return 0;
        }
        if (wp == VK_RETURN && self->searching() && self->selectedSearchResult >= 0) {
            self->openSearchResult(self->selectedSearchResult);
            return 0;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && self->searching()) {
            self->selectSearchResult(wp == VK_DOWN ? 1 : -1);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdSettingsSearch && HIWORD(wp) == EN_CHANGE) {
            if (!self->closingSearchPalette)
                self->refreshSearch();
            return 0;
        }
        break;
    case WM_DPICHANGED:
        self->layoutSearchPalette();
        self->searchPaletteSurface.invalidate();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && self->searchPaletteOpen && !self->closingSearchPalette) {
            if (self->owner_.hwnd && IsWindow(self->owner_.hwnd)) {
                POINT cursor{};
                if (GetCursorPos(&cursor) && ScreenToClient(self->owner_.hwnd, &cursor)) {
                    const float ownerScale = self->owner_.surface.dipScale();
                    const float cursorX = static_cast<float>(cursor.x) / ownerScale;
                    const float cursorY = static_cast<float>(cursor.y) / ownerScale;
                    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 &&
                        contains(self->searchTriggerRect, cursorX, cursorY)) {
                        self->suppressNextSearchTriggerPress = true;
                    }
                }
            }
            self->closeSearchPalette(false);
            return 0;
        }
        break;
    case WM_CLOSE:
        self->closeSearchPalette(true);
        return 0;
    case WM_DESTROY:
        self->searchPaletteOpen = false;
        self->searchPaletteSurface.discard();
        self->searchPaletteHwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

bool SettingsSearchPalette::ensureSearchPaletteWindow() {
    if (searchPaletteHwnd && IsWindow(searchPaletteHwnd))
        return true;
    WNDCLASSEXW paletteClass{};
    paletteClass.cbSize = sizeof(paletteClass);
    paletteClass.style = CS_HREDRAW | CS_VREDRAW;
    paletteClass.lpfnWndProc = &SettingsSearchPalette::searchPaletteWndProc;
    paletteClass.hInstance = owner_.inst;
    paletteClass.lpszClassName = L"QQMusicLyricSettingsSearchPalette";
    paletteClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&paletteClass);

    searchPaletteHwnd =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"QQMusicLyricSettingsSearchPalette", L"",
                        WS_POPUP, 0, 0, 1, 1, owner_.hwnd, nullptr, owner_.inst, this);
    if (!searchPaletteHwnd)
        return false;
    SetLayeredWindowAttributes(searchPaletteHwnd, 0, 255, LWA_ALPHA);
    styleSearchPaletteWindow();
    searchPaletteSurface.initialize(searchPaletteHwnd, false, true);
    if (!settingsSearch.create(searchPaletteHwnd, kIdSettingsSearch, L"搜索设置项、选项或说明…",
                               false, 14.5f)) {
        DestroyWindow(searchPaletteHwnd);
        searchPaletteHwnd = nullptr;
        return false;
    }
    settingsSearch.setChromelessStyle(true);
    SendMessageW(settingsSearch.editHwnd(), EM_SETLIMITTEXT, 120, 0);
    SetWindowSubclass(settingsSearch.editHwnd(), &SettingsSearchPalette::searchEditProc, 2,
                      reinterpret_cast<DWORD_PTR>(this));
    return true;
}

void SettingsSearchPalette::openSearchPalette() {
    if (!owner_.hwnd || !IsWindow(owner_.hwnd))
        return;
    beforeOpen_();
    if (searchPaletteOpen && searchPaletteHwnd && IsWindowVisible(searchPaletteHwnd)) {
        SetActiveWindow(searchPaletteHwnd);
        settingsSearch.focus();
        SendMessageW(settingsSearch.editHwnd(), EM_SETSEL, 0, -1);
        return;
    }
    if (!ensureSearchPaletteWindow())
        return;
    ensureSearchScrimWindow();

    updateSearchPaletteProgress();
    const bool wasClosing =
        !searchPaletteOpen && searchPaletteAnimating && searchPaletteProgress > 0.01f;
    if (owner_.hwnd)
        KillTimer(owner_.hwnd, kSearchPaletteAnimTimer);
    searchPaletteAnimating = false;
    setWindowClickThrough(searchPaletteHwnd, false);
    setWindowClickThrough(searchScrimHwnd, false);

    closingSearchPalette = true;
    settingsSearch.setText(L"");
    closingSearchPalette = false;
    searchModel_.clear();
    selectedSearchResult = -1;
    searchPaletteScroll = 0.0f;
    searchPaletteMaxScroll = 0.0f;
    searchPaletteHoverKind = kSearchPaletteHitNone;
    searchPaletteHoverRow = -1;
    searchPalettePressedKind = kSearchPaletteHitNone;
    searchPalettePressedRow = -1;
    searchPaletteLastMovePt = {-1, -1};

    BOOL clientAnimations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimations, 0);
    const float startProgress =
        clientAnimations ? (wasClosing ? searchPaletteProgress : 0.0f) : 1.0f;
    searchPaletteProgress = startProgress;
    searchPaletteAnimFrom = startProgress;
    searchPaletteAnimTarget = 1.0f;
    searchPaletteOpen = true;

    layoutSearchPalette();
    // 先隐藏原表面再显示窗口，避免准备合成资源期间闪出完整面板。
    const BOOL cloak = clientAnimations && startProgress < 0.999f;
    DwmSetWindowAttribute(searchPaletteHwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    SetLayeredWindowAttributes(searchPaletteHwnd, 0, 255, LWA_ALPHA);
    if (searchScrimHwnd && IsWindow(searchScrimHwnd))
        ShowWindow(searchScrimHwnd, SW_SHOWNOACTIVATE);
    ShowWindow(searchPaletteHwnd, SW_SHOW);
    SetWindowPos(searchPaletteHwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
        SetWindowPos(searchScrimHwnd, searchPaletteHwnd, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    searchPaletteSurface.invalidate();
    SetActiveWindow(searchPaletteHwnd);
    settingsSearch.focus();
    if (clientAnimations && startProgress < 0.999f) {
        searchPaletteAnimDurationMs =
            std::max(40.0f, kSearchPaletteOpenDurationMs * (1.0f - startProgress));
        if (!startSearchPaletteAnimation())
            finishSearchPaletteAnimation();
    } else
        finishSearchPaletteAnimation();
    invalidateSearchTrigger();
}

void SettingsSearchPalette::closeSearchPalette(bool restoreOwnerFocus, bool immediate) {
    if (immediate) {
        if (!searchPaletteOpen && !searchPaletteAnimating)
            return;
        searchPaletteOpen = false;
        searchPaletteHoverKind = kSearchPaletteHitNone;
        searchPaletteHoverRow = -1;
        searchPalettePressedKind = kSearchPaletteHitNone;
        searchPalettePressedRow = -1;
        if (searchPaletteHwnd && GetCapture() == searchPaletteHwnd)
            ReleaseCapture();
        finalizeSearchPaletteClosed();
        if (restoreOwnerFocus && owner_.hwnd && IsWindow(owner_.hwnd)) {
            const bool keepFocusVisible = owner_.focusVisible;
            SetActiveWindow(owner_.hwnd);
            SetFocus(owner_.hwnd);
            owner_.focusVisible = keepFocusVisible;
        }
        if (owner_.hwnd && IsWindow(owner_.hwnd))
            invalidateSearchTrigger();
        return;
    }
    if (!searchPaletteOpen || closingSearchPalette)
        return;
    updateSearchPaletteProgress();
    closingSearchPalette = true;
    searchPaletteOpen = false;
    searchPaletteHoverKind = kSearchPaletteHitNone;
    searchPaletteHoverRow = -1;
    searchPalettePressedKind = kSearchPaletteHitNone;
    searchPalettePressedRow = -1;
    if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
        if (GetCapture() == searchPaletteHwnd)
            ReleaseCapture();
    }
    setWindowClickThrough(searchPaletteHwnd, true);
    setWindowClickThrough(searchScrimHwnd, true);
    BOOL clientAnimations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimations, 0);
    closingSearchPalette = false;
    if (restoreOwnerFocus && owner_.hwnd && IsWindow(owner_.hwnd)) {
        const bool keepFocusVisible = owner_.focusVisible;
        SetActiveWindow(owner_.hwnd);
        SetFocus(owner_.hwnd);
        owner_.focusVisible = keepFocusVisible;
    }
    if (!clientAnimations || searchPaletteProgress <= 0.02f || !owner_.hwnd) {
        finalizeSearchPaletteClosed();
    } else {
        searchPaletteAnimFrom = searchPaletteProgress;
        searchPaletteAnimTarget = 0.0f;
        searchPaletteAnimDurationMs =
            std::max(36.0f, kSearchPaletteCloseDurationMs * searchPaletteProgress);
        if (!startSearchPaletteAnimation())
            finalizeSearchPaletteClosed();
    }
    if (owner_.hwnd && IsWindow(owner_.hwnd))
        invalidateSearchTrigger();
}

void SettingsSearchPalette::destroy() {
    closeSearchPalette(false, true);
    searchPaletteAnimating = false;
    searchAnimationRenderer.releaseAll();
    if (searchAnimationHwnd) {
        DestroyWindow(searchAnimationHwnd);
        searchAnimationHwnd = nullptr;
    }
    if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
        DestroyWindow(searchPaletteHwnd);
        searchPaletteHwnd = nullptr;
    }
    if (searchScrimHwnd && IsWindow(searchScrimHwnd)) {
        DestroyWindow(searchScrimHwnd);
        searchScrimHwnd = nullptr;
    }
    searchScrimRenderer.releaseAll();
    searchPaletteOpen = false;
    searchPaletteSurface.discard();
}
void SettingsSearchPalette::refreshTheme() {
    searchScrimLastWidth = 0;
    searchScrimLastHeight = 0;
    if (searchPaletteHwnd && IsWindow(searchPaletteHwnd)) {
        settingsSearch.refreshTheme();
        styleSearchPaletteWindow();
        if (searchPaletteOpen)
            layoutSearchPalette();
        searchPaletteSurface.invalidate();
    }
}

} // namespace settings_ui
