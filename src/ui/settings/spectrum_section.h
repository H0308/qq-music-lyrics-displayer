#pragma once
#include "ui/settings/row.h"
#include "ui/settings_dialog.h"

namespace settings_ui {
std::vector<Row> createSpectrumRows(const SettingsState& state);
void updateSpectrumRows(std::vector<Row>& rows, const SettingsState& state);
} // namespace settings_ui
