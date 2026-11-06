#pragma once
#include "ui/settings/row.h"
#include "ui/settings_dialog.h"

namespace settings_ui {
std::vector<Row> createLyricRows(const SettingsState& state);
void updateLyricRows(std::vector<Row>& rows, const SettingsState& state);
void updateCompactLyricRows(std::vector<Row>& rows, const SettingsState& state);
bool handleLyricCommand(int id, Row& row, std::vector<Row>& rows, SettingsState& state,
                        const SettingsActions& actions);
} // namespace settings_ui
