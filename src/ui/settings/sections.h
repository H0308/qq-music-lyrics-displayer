#pragma once
#include "ui/settings/row.h"
#include "ui/settings/ids.h"
#include "ui/settings_dialog.h"
#include <array>

namespace settings_ui {
// 设置页的行数据和绑定逻辑。窗口仅处理布局、焦点、编辑器及系统弹窗。
class SettingsSections {
public:
    static bool isIdleAppDragOption(int option) { return option >= kIdleAppDragOptionBase; }
    SettingsState state;
    SettingsActions actions;
    std::array<std::vector<Row>, kSettingsPageCount> rows;
    Row* findRow(int id);
    const Row* findRow(int id) const;
    bool minimalModeActive() const;
    bool backgroundWaveActive() const;
    bool spectrumBackgroundAvailable() const;
    int idleTextMode() const;
    bool embeddedResourceOnlyAvailable() const;
    void create();
    void update(const SettingsState& s);
    void onCommand(int id, int option = -1);
    void updateFloatingCardRowsEnabled();
    void updateProgressBackgroundRowsEnabled();
    void updateBackgroundAdjustmentPresentation();
    void updateImmersiveRowsEnabled();
    void updateIdleAppsRowHeight();
    void updateTickTickRows();
    void updateIdleQuoteSourceRow();
    void updateIdleRowsEnabled();
    void updateCompactLyricRows();

private:
    Row& addRow(int page, int id, ControlKind kind, const wchar_t* text, const wchar_t* hint,
                float controlW, float height);
    Row& addToggle(int page, int id, const wchar_t* text, bool checked);
    Row& addRadio(int page, int id, const wchar_t* text, const wchar_t* hint,
                  std::vector<std::wstring> options, int selected, bool enabled, float height);
    Row& addModeGrid(int page, int id, const wchar_t* text, const wchar_t* hint,
                     std::vector<std::wstring> options, std::vector<std::wstring> optionHints,
                     int selected, bool enabled, float height);
    Row& addSlider(int page, int id, const wchar_t* text, int value, bool enabled,
                   const wchar_t* hint = nullptr, float height = kRowH);
    Row& addButton(int page, int id, const wchar_t* text, const wchar_t* hint,
                   const wchar_t* controlText, float height = kRowH);
    Row& addHeader(int page, const wchar_t* text);
    void createDisplayRows();
    void createTaskbarModeRows();
    void createIdleRows();
    void createBackgroundRows();
    void createMediaControlRows();
    void createFloatingCardRows();
    void createSongToastRows();
    void createPerformanceRows();
    bool onIdleCommand(int id, Row* row, int option);
    bool onSpectrumCommand(int id, Row* row);
    bool onFloatingCardCommand(int id, Row* row);
    bool onSongToastCommand(int id, Row* row);
    void syncIdleRows(const SettingsState& s);
    void syncDisplayRows(const SettingsState& s);
    void syncTaskbarModeRows(const SettingsState& s);
    void syncBackgroundRows(const SettingsState& s);
    void syncMediaControlRows(const SettingsState& s);
    void syncFloatingCardRows(const SettingsState& s);
    void syncSongToastRows(const SettingsState& s);
    void syncThemeFontRows(const SettingsState& s);
};
} // namespace settings_ui
