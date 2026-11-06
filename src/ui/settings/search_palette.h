#pragma once
#include "ui/settings/sections.h"
#include "ui/settings/search_model.h"
#include "ui/settings/search_metrics.h"
#include "ui/fluent_controls.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/lyric_renderer.h"
#include <functional>
#include <utility>

namespace settings_ui {
struct SearchPaletteOwner {
    const HWND& hwnd;
    const HINSTANCE& inst;
    fluent::FluentDialogSurface& surface;
    const SettingsSections& sections;
    const std::array<std::wstring, kSettingsPageCount>& pageTitles;
    bool& focusVisible;
};
class SettingsSearchPalette {
public:
    SettingsSearchPalette(SearchPaletteOwner owner, std::function<void()> beforeOpen,
                          std::function<void(const SettingsSearchResult&)> onSelect)
        : owner_(owner), beforeOpen_(std::move(beforeOpen)), onSelect_(std::move(onSelect)) {}
    ~SettingsSearchPalette() { destroy(); }
    SettingsSearchPalette(const SettingsSearchPalette&) = delete;
    SettingsSearchPalette& operator=(const SettingsSearchPalette&) = delete;
    bool isOpen() const { return searchPaletteOpen; }
    bool animating() const { return searchPaletteAnimating; }
    void setTriggerRect(const D2D1_RECT_F& rect) { searchTriggerRect = rect; }
    const D2D1_RECT_F& triggerRect() const { return searchTriggerRect; }
    bool consumeTriggerSuppression() {
        return std::exchange(suppressNextSearchTriggerPress, false);
    }
    void destroy();
    void refreshTheme();
    void resetResults() {
        searchModel_.clear();
        selectedSearchResult = -1;
    }
    bool searching() const;
    void refreshSearch();
    void advanceSearchPaletteAnimation();
    void layoutSearchPalette();
    void openSearchPalette();
    void closeSearchPalette(bool restoreOwnerFocus, bool immediate = false);

private:
    void selectSearchResult(int direction);
    void openSearchResult(int index);
    static LRESULT CALLBACK searchEditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR subclassId, DWORD_PTR refData);
    void styleSearchPaletteWindow();
    static void setWindowClickThrough(HWND window, bool clickThrough);
    BYTE searchPaletteAlphaByte() const;
    int searchPaletteOffsetYPx(float scale) const;
    void applySearchPaletteAnimationVisuals();
    void finalizeSearchPaletteClosed();
    float searchPaletteDesiredHeightDip() const;
    void renderSearchScrim(int widthPx, int heightPx, UINT dpi);
    static LRESULT CALLBACK searchScrimWndProc(HWND window, UINT message, WPARAM wp, LPARAM lp);
    bool ensureSearchScrimWindow();
    int searchPaletteHitTest(float xDip, float yDip, int* outRow = nullptr) const;
    void paintSearchPalette(fluent::FluentDialogSurface::Painter& painter, float widthDip,
                            float heightDip);
    static LRESULT CALLBACK searchPaletteWndProc(HWND window, UINT message, WPARAM wp, LPARAM lp);
    bool ensureSearchPaletteWindow();
    SearchPaletteOwner owner_;
    std::function<void()> beforeOpen_;
    std::function<void(const SettingsSearchResult&)> onSelect_;
    fluent::FluentEdit settingsSearch;
    SettingsSearchModel searchModel_;
    int selectedSearchResult = -1;
    D2D1_RECT_F searchTriggerRect{};
    HWND searchScrimHwnd = nullptr;
    LyricRenderer searchScrimRenderer;
    int searchScrimLastWidth = 0;
    int searchScrimLastHeight = 0;
    UINT searchScrimLastDpi = 0;
    bool searchScrimLastDark = false;
    bool searchScrimLastZoomed = false;
    HWND searchPaletteHwnd = nullptr;
    fluent::FluentDialogSurface searchPaletteSurface;
    bool searchPaletteOpen = false;
    bool closingSearchPalette = false;
    bool suppressNextSearchTriggerPress = false;
    bool searchPaletteAnimating = false;
    ULONGLONG searchPaletteAnimStartMs = 0;
    float searchPaletteAnimDurationMs = kSearchPaletteOpenDurationMs;
    float searchPaletteAnimFrom = 0.0f;
    float searchPaletteAnimTarget = 0.0f;
    float searchPaletteProgress = 0.0f;
    int searchPaletteBaseX = 0;
    int searchPaletteBaseY = 0;
    float searchPaletteScale = 1.0f;
    float searchPaletteScroll = 0.0f;
    float searchPaletteMaxScroll = 0.0f;
    int searchPaletteHoverKind = kSearchPaletteHitNone;
    int searchPaletteHoverRow = -1;
    int searchPalettePressedKind = kSearchPaletteHitNone;
    int searchPalettePressedRow = -1;
    POINT searchPaletteLastMovePt{-1, -1};
    D2D1_RECT_F searchPaletteClearRect{};
    D2D1_RECT_F searchPaletteEscRect{};
    D2D1_RECT_F searchPaletteListRect{};
    static bool contains(const D2D1_RECT_F& r, float x, float y) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }
};
} // namespace settings_ui
