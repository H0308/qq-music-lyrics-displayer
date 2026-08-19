#include "settings_dialog.h"

#include "ui/dialog_notify.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_theme.h"
#include "resource.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr int kIdNav = 400;
constexpr int kIdSongInfo = 410;
constexpr int kIdAlbumCover = 411;
constexpr int kIdPlatformIcon = 412;
constexpr int kIdCoverEffect = 413;
constexpr int kIdSpectrum = 414;
constexpr int kIdHoverControls = 415;
constexpr int kIdPickFont = 420;
constexpr int kIdFontColor = 421;
constexpr int kIdFollowAlbum = 422;
constexpr int kIdDoubleLine = 430;
constexpr int kIdAlignment = 431;
constexpr int kIdSecondaryOn = 432;
constexpr int kIdSecondaryType = 433;

constexpr float kWindowW = 760.0f;
constexpr float kWindowH = 520.0f;
constexpr float kNavW = 176.0f;
constexpr float kRowH = 56.0f;
constexpr float kRowTallH = 72.0f;
constexpr float kRowGap = 8.0f;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

// 单选组宽度估算：圆 + 间距 + 文本（CJK 按字号宽度估）
float estimateRadioWidth(const std::vector<std::wstring>& options) {
    float w = 0.0f;
    for (const auto& o : options)
        w += 16.0f + 8.0f + static_cast<float>(o.size()) * 14.0f + 20.0f;
    return w > 0.0f ? w - 20.0f : 0.0f;
}

} // namespace

struct SettingsDialog::Impl {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;
    SettingsState state;
    SettingsActions actions;
    int activePage = 0;

    // 一行设置：卡片底 + 主标签 + 可选提示 + 右侧控件
    struct Row {
        std::unique_ptr<fluent::FluentCard> card;
        std::unique_ptr<fluent::FluentLabel> label;
        std::unique_ptr<fluent::FluentLabel> hint; // 可空：次要说明
        bool showHint = false; // hint 有文本时才参与布局
        std::unique_ptr<fluent::LayeredChild> control;
        float controlW = 0.0f; // 控件宽度（DIP），右对齐布局用
        float height = kRowH;
    };

    fluent::FluentList nav;
    std::unique_ptr<fluent::FluentLabel> pageTitles[3];
    std::vector<Row> rows[3];

    // 窗口复用时需要更新内容的控件
    fluent::FluentLabel* hintFont = nullptr;
    fluent::FluentLabel* hintSecondary = nullptr;
    Row* rowSecondary = nullptr;

    // 需要在命令处理里读状态的控件
    fluent::FluentToggle* tglSongInfo = nullptr;
    fluent::FluentToggle* tglAlbumCover = nullptr;
    fluent::FluentToggle* tglPlatformIcon = nullptr;
    fluent::FluentToggle* tglSpectrum = nullptr;
    fluent::FluentToggle* tglHoverControls = nullptr;
    fluent::FluentToggle* tglFollowAlbum = nullptr;
    fluent::FluentToggle* tglDoubleLine = nullptr;
    fluent::FluentToggle* tglSecondaryOn = nullptr;
    fluent::FluentRadioGroup* radioCoverEffect = nullptr;
    fluent::FluentRadioGroup* radioAlign = nullptr;
    fluent::FluentRadioGroup* radioSecondaryType = nullptr;

    template <typename T, typename... Args>
    T& addRowControl(int page, const wchar_t* text, const wchar_t* hint, float controlW,
                     float rowH, Args&&... args) {
        Row row;
        row.card = std::make_unique<fluent::FluentCard>();
        row.card->create(hwnd, 0);
        row.label = std::make_unique<fluent::FluentLabel>();
        row.label->create(hwnd, 0, text, false, 14.0f, 400);
        if (hint) {
            row.hint = std::make_unique<fluent::FluentLabel>();
            row.hint->create(hwnd, 0, hint, true, 12.0f, 400);
            row.showHint = *hint != L'\0';
        }
        auto control = std::make_unique<T>();
        control->create(hwnd, std::forward<Args>(args)...);
        row.controlW = controlW;
        row.height = rowH;
        T* ptr = control.get();
        row.control = std::move(control);
        rows[page].push_back(std::move(row));
        return *ptr;
    }

    void createControls() {
        nav.create(hwnd, kIdNav);
        nav.setItems({{L"显示"}, {L"字体与颜色"}, {L"歌词"}});

        const wchar_t* titles[] = {L"显示", L"字体与颜色", L"歌词"};
        for (int i = 0; i < 3; ++i) {
            pageTitles[i] = std::make_unique<fluent::FluentLabel>();
            pageTitles[i]->create(hwnd, 0, titles[i], false, 20.0f, 600);
        }

        // ---- 显示 ----
        tglSongInfo = &addRowControl<fluent::FluentToggle>(0, L"显示歌曲信息", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdSongInfo, state.songInfoVisible);
        tglAlbumCover = &addRowControl<fluent::FluentToggle>(0, L"显示专辑封面", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdAlbumCover, state.albumCoverVisible);
        tglPlatformIcon = &addRowControl<fluent::FluentToggle>(0, L"显示平台图标", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdPlatformIcon, state.platformIconVisible);
        {
            auto& radio = addRowControl<fluent::FluentRadioGroup>(0, L"专辑封面效果", nullptr,
                estimateRadioWidth({L"默认", L"黑胶唱片"}), kRowH, kIdCoverEffect);
            radio.setOptions({L"默认", L"黑胶唱片"});
            radio.setSelectedIndex(state.coverEffectVinyl ? 1 : 0);
            radio.setEnabled(state.albumCoverVisible);
            radioCoverEffect = &radio;
        }
        tglSpectrum = &addRowControl<fluent::FluentToggle>(0, L"频谱", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdSpectrum, state.spectrumOn);
        tglHoverControls = &addRowControl<fluent::FluentToggle>(0, L"悬浮时显示播放控件", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdHoverControls, state.hoverControls);

        // ---- 字体与颜色 ----
        addRowControl<fluent::FluentButton>(1, L"字体", state.fontDesc.c_str(), 132.0f, kRowH,
                                            kIdPickFont, L"选择字体…");
        hintFont = rows[1].back().hint.get();
        addRowControl<fluent::FluentButton>(1, L"字体颜色与效果", nullptr, 132.0f, kRowH,
                                            kIdFontColor, L"打开…");
        tglFollowAlbum = &addRowControl<fluent::FluentToggle>(1, L"已播放颜色跟随专辑", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdFollowAlbum, state.followAlbum);

        // ---- 歌词 ----
        tglDoubleLine = &addRowControl<fluent::FluentToggle>(2, L"双行歌词", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdDoubleLine, state.doubleLineLyrics);
        {
            auto& radio = addRowControl<fluent::FluentRadioGroup>(2, L"歌词对齐", nullptr,
                estimateRadioWidth({L"左对齐", L"居中", L"右对齐"}), kRowH, kIdAlignment);
            radio.setOptions({L"左对齐", L"居中", L"右对齐"});
            radio.setSelectedIndex(state.lyricAlignment);
            radioAlign = &radio;
        }
        tglSecondaryOn = &addRowControl<fluent::FluentToggle>(2, L"开启翻译/罗马音", nullptr,
            fluent::FluentToggle::kWidth, kRowH, kIdSecondaryOn, state.secondaryEnabled);
        {
            const wchar_t* hint = state.secondaryAvailability == 1 ? L"正在检查翻译和罗马音…"
                                  : state.secondaryAvailability == 2 ? L"当前歌曲无翻译或罗马音"
                                                                     : L"";
            auto& radio = addRowControl<fluent::FluentRadioGroup>(2, L"辅助歌词类型", hint,
                estimateRadioWidth({L"翻译", L"罗马音"}), *hint ? kRowTallH : kRowH,
                kIdSecondaryType);
            radio.setOptions({L"翻译", L"罗马音"});
            radio.setSelectedIndex(state.preferRomanization ? 1 : 0);
            radio.setEnabled(state.secondaryEnabled && state.secondaryAvailability == 0);
            radioSecondaryType = &radio;
            rowSecondary = &rows[2].back();
            hintSecondary = rowSecondary->hint.get();
        }
    }

    // 只布置活动页。非活动页的分层子控件一旦被 move（内部带 SWP_SHOWWINDOW +
    // UpdateLayeredWindow）再立刻隐藏，窗口第一次显示时 DWM 可能把刚更新的
    // 表面合成出一帧过期内容（首开时“字体与颜色”页闪一下的根因）。非活动页
    // 保持从未显示的状态，在 showPage 激活时再通过 layout 布置渲染。
    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        float s = fluent::dipScale(GetDpiForWindow(hwnd));
        auto px = [&](float dip) { return static_cast<int>(std::lround(dip * s)); };
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int navX = px(12.0f);
        nav.move(navX, navX, px(kNavW), h - navX * 2);
        int contentX = px(12.0f + kNavW + 16.0f);
        int contentW = w - contentX - px(24.0f);

        int titleH = px(28.0f);
        int y = px(14.0f) + titleH + px(16.0f);
        const int page = activePage;
        pageTitles[page]->move(contentX, px(14.0f), contentW, titleH);
        for (auto& row : rows[page]) {
            int rowH = px(row.height);
            row.card->move(contentX, y, contentW, rowH);
            int innerX = contentX + px(16.0f);
            int controlW = px(row.controlW);
            int controlX = contentX + contentW - px(16.0f) - controlW;
            int labelW = controlX - innerX - px(12.0f);
            if (row.hint && row.showHint) {
                row.label->move(innerX, y + px(12.0f), labelW, px(22.0f));
                row.hint->move(innerX, y + rowH - px(26.0f), labelW, px(18.0f));
            } else {
                row.label->move(innerX, y + (rowH - px(22.0f)) / 2, labelW, px(22.0f));
            }
            // 控件垂直居中：开关/单选 20-24 DIP，按钮 32 DIP
            float controlH = dynamic_cast<fluent::FluentButton*>(row.control.get())
                                 ? fluent::metrics::controlHeight
                                 : 24.0f;
            int ctrlH = px(controlH);
            row.control->move(controlX, y + (rowH - ctrlH) / 2, controlW, ctrlH);
            y += rowH + px(kRowGap);
        }
        // 分层子窗口按 Z-order 合成：卡片必须压到最底，否则半透明卡片填充
        // 会盖在文字和控件上面，看起来雾蒙蒙（与 about_dialog 的 infoCard 同理）
        for (auto& row : rows[page])
            SetWindowPos(row.card->hwnd(), HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void showPage(int page) {
        if (page != activePage) {
            activePage = page;
            layout(); // 新激活页可能从未布置过，补齐定位与渲染
        }
        for (int i = 0; i < 3; ++i) {
            const int cmd = i == activePage ? SW_SHOW : SW_HIDE;
            ShowWindow(pageTitles[i]->hwnd(), cmd);
            for (auto& row : rows[i]) {
                ShowWindow(row.card->hwnd(), cmd);
                ShowWindow(row.label->hwnd(), cmd);
                if (row.hint)
                    ShowWindow(row.hint->hwnd(), cmd);
                ShowWindow(row.control->hwnd(), cmd);
            }
        }
    }

    void refreshTheme() {
        nav.refreshTheme();
        for (int i = 0; i < 3; ++i) {
            pageTitles[i]->refreshTheme();
            for (auto& row : rows[i]) {
                row.card->refreshTheme();
                row.label->refreshTheme();
                if (row.hint)
                    row.hint->refreshTheme();
                row.control->refreshTheme();
            }
        }
    }

    void onCommand(int id, int code) {
        if (id == kIdNav && code == LBN_SELCHANGE) {
            showPage(nav.selectedIndex());
            return;
        }
        if (code != BN_CLICKED)
            return;
        switch (id) {
        case kIdSongInfo:
            if (actions.onSongInfoVisible)
                actions.onSongInfoVisible(tglSongInfo->checked());
            break;
        case kIdAlbumCover:
            if (actions.onAlbumCoverVisible)
                actions.onAlbumCoverVisible(tglAlbumCover->checked());
            // 封面隐藏时封面效果无意义，联动禁用
            radioCoverEffect->setEnabled(tglAlbumCover->checked());
            break;
        case kIdPlatformIcon:
            if (actions.onPlatformIconVisible)
                actions.onPlatformIconVisible(tglPlatformIcon->checked());
            break;
        case kIdCoverEffect:
            if (actions.onCoverEffectVinyl)
                actions.onCoverEffectVinyl(radioCoverEffect->selectedIndex() == 1);
            break;
        case kIdSpectrum:
            if (actions.onSpectrum)
                actions.onSpectrum(tglSpectrum->checked());
            break;
        case kIdHoverControls:
            if (actions.onHoverControls)
                actions.onHoverControls(tglHoverControls->checked());
            break;
        case kIdPickFont:
            if (actions.onPickFont)
                actions.onPickFont();
            break;
        case kIdFontColor:
            if (actions.onFontColorEffect)
                actions.onFontColorEffect();
            break;
        case kIdFollowAlbum:
            if (actions.onFollowAlbum)
                actions.onFollowAlbum(tglFollowAlbum->checked());
            break;
        case kIdDoubleLine:
            if (actions.onDoubleLineLyrics)
                actions.onDoubleLineLyrics(tglDoubleLine->checked());
            break;
        case kIdAlignment:
            if (actions.onLyricAlignment)
                actions.onLyricAlignment(radioAlign->selectedIndex());
            break;
        case kIdSecondaryOn:
            if (actions.onSecondaryEnabled)
                actions.onSecondaryEnabled(tglSecondaryOn->checked());
            // 辅助歌词类型只在总开关开且当前歌曲有能力时可选
            radioSecondaryType->setEnabled(tglSecondaryOn->checked() &&
                                           state.secondaryAvailability == 0);
            break;
        case kIdSecondaryType:
            if (actions.onPreferRomanization)
                actions.onPreferRomanization(radioSecondaryType->selectedIndex() == 1);
            break;
        }
    }

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
            backdrop = fluent::styleDialogWindow(hwnd, false);
            createControls();
            layout();
            nav.setSelectedIndex(0);
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            refreshTheme();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop, false);
            refreshTheme();
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            fluent::paintDialogBackground(hwnd, hdc, backdrop);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            fluent::paintDialogBackground(hwnd, reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_CLOSE:
            // 只隐藏不销毁：窗口与全部控件复用，再次打开无需重建
            //（同步重建十几个分层子控件会阻塞 UI 线程，导致任务栏歌词动画掉帧）。
            // 窗口随 App 退出由 ~SettingsDialog 销毁。
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::Settings), 0);
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

    // 窗口隐藏期间状态可能经托盘菜单等途径变更，再次打开前同步快照。
    // 各 setter 都有相同值早退，未变化的控件不会触发重绘。
    void updateState(const SettingsState& s) {
        state = s;
        tglSongInfo->setChecked(s.songInfoVisible);
        tglAlbumCover->setChecked(s.albumCoverVisible);
        tglPlatformIcon->setChecked(s.platformIconVisible);
        radioCoverEffect->setSelectedIndex(s.coverEffectVinyl ? 1 : 0);
        radioCoverEffect->setEnabled(s.albumCoverVisible);
        tglSpectrum->setChecked(s.spectrumOn);
        tglHoverControls->setChecked(s.hoverControls);
        tglFollowAlbum->setChecked(s.followAlbum);
        if (hintFont)
            hintFont->setText(s.fontDesc);
        tglDoubleLine->setChecked(s.doubleLineLyrics);
        radioAlign->setSelectedIndex(s.lyricAlignment);
        tglSecondaryOn->setChecked(s.secondaryEnabled);
        const wchar_t* hint = s.secondaryAvailability == 1 ? L"正在检查翻译和罗马音…"
                              : s.secondaryAvailability == 2 ? L"当前歌曲无翻译或罗马音" : L"";
        hintSecondary->setText(hint);
        rowSecondary->showHint = *hint != L'\0';
        rowSecondary->height = rowSecondary->showHint ? kRowTallH : kRowH;
        radioSecondaryType->setSelectedIndex(s.preferRomanization ? 1 : 0);
        radioSecondaryType->setEnabled(s.secondaryEnabled && s.secondaryAvailability == 0);
        layout();
    }

    void updateFontDescription(const std::wstring& description) {
        state.fontDesc = description;
        if (hintFont)
            hintFont->setText(description);
    }
};

SettingsDialog::SettingsDialog() : impl_(std::make_unique<Impl>()) {}

SettingsDialog::~SettingsDialog() {
    if (impl_ && impl_->hwnd)
        impl_->destroy();
}

bool SettingsDialog::create(HINSTANCE inst, HWND parent, const SettingsState& state,
                            SettingsActions actions) {
    impl_->notifyHwnd = parent; // 仅用于关闭通知；托盘窗口不能作为普通窗口的可见所有者
    impl_->inst = inst;
    impl_->state = state;
    impl_->actions = std::move(actions);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricSettingsDialog";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    UINT dpi = GetDpiForSystem();
    float s = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(kWindowW * s)),
            static_cast<LONG>(std::lround(kWindowH * s))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricSettingsDialog", L"设置",
                                  kDialogStyle, x, y, w, h, nullptr, nullptr, inst,
                                  impl_.get());
    return impl_->hwnd != nullptr;
}

void SettingsDialog::updateState(const SettingsState& state) {
    if (impl_->hwnd)
        impl_->updateState(state);
}

void SettingsDialog::updateFontDescription(const std::wstring& description) {
    if (impl_->hwnd)
        impl_->updateFontDescription(description);
}

void SettingsDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
    }
}

void SettingsDialog::destroy() {
    impl_->destroy();
}

bool SettingsDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND SettingsDialog::hwnd() const {
    return impl_->hwnd;
}
