#include "ui/settings/lyric_section.h"
#include "ui/settings/ids.h"
#include <algorithm>

namespace settings_ui {
std::vector<Row> createLyricRows(const SettingsState& state) {
    std::vector<Row> rows;
    RowBuilder builder(rows);
    const bool vertical = state.verticalTaskbar;
    const bool minimal = state.renderMode == kRenderModeMinimal;
    builder.addHeader(L"排版");
    builder.addToggle(kIdDoubleLine, L"双行歌词", vertical ? false : state.doubleLineLyrics)
        .enabled = !vertical;
    builder.addRadio(kIdAlignment, L"歌词对齐", nullptr, {L"左对齐", L"居中", L"右对齐"},
                     vertical ? 0 : state.lyricAlignment, !vertical && state.taskbarViewMode == 0,
                     kRowH);
    builder.addHeader(L"颜色与效果");
    builder.addButton(kIdFontColor, L"歌词字体颜色与效果", nullptr, L"打开…");
    builder.addToggle(kIdFollowAlbum, L"歌词已播放颜色跟随专辑", state.followAlbum);
    builder.addHeader(L"歌词特效");
    const wchar_t* lyricEffectNames[kLyricEffectCount] = {L"星尘跟唱", L"跟唱流光", L"笔触下划线",
                                                          L"字间涟漪", L"逐字浮波"};
    for (int i = 0; i < kLyricEffectCount; ++i) {
        Row& effect = builder.addRow(
            kIdLyricEffectFirst + i, ControlKind::Toggle, lyricEffectNames[i],
            i == 0
                ? L"仅单行逐字歌词生效，歌词下沉、星星跳动旋转并带流星粒子拖尾；与逐字浮波及翻译/罗马音互斥。"
                : i == static_cast<int>(LyricEffect::WordWave)
                    ? L"支持单行和双行逐字歌词，整行长波轻柔起伏，逐字下沉后缓慢浮起；与星尘跟唱互斥。"
                    : L"需要逐字歌词，仅作用于当前演唱行。",
            40.0f, kRowTallH);
        effect.checked = (state.lyricEffects & (1u << i)) != 0;
        effect.enabled = !minimal && (i != 0 || vertical || !state.doubleLineLyrics);
        if (i == static_cast<int>(LyricEffect::WordWave))
            effect.enabled = !minimal && !vertical && !state.compactTaskbar;
    }
    builder.addHeader(L"翻译与罗马音");
    builder.addToggle(kIdSecondaryOn, L"开启翻译/罗马音", vertical ? false : state.secondaryEnabled)
        .enabled = !vertical;
    const wchar_t* secondaryHint =
        state.secondaryAvailability == SecondaryLyricAvailability::Loading
            ? L"正在检查翻译和罗马音…"
        : state.secondaryAvailability == SecondaryLyricAvailability::Unavailable
            ? L"当前歌曲无翻译或罗马音"
            : L"";
    builder.addRadio(kIdSecondaryType, L"辅助歌词类型", secondaryHint, {L"翻译", L"罗马音"},
                     vertical ? 0 : (state.preferRomanization ? 1 : 0),
                     !vertical && state.secondaryEnabled &&
                         state.secondaryAvailability == SecondaryLyricAvailability::Available,
                     *secondaryHint ? kRowTallH : kRowH);
    builder.addHeader(L"本地歌词");
    builder.addToggle(kIdQqLocalLyricsEnabled, L"使用 QQ 音乐本地歌词", state.qqLocalLyricsEnabled);
    Row& persistOrder = builder.addRow(
        kIdQqLocalLyricsPersistOrder, ControlKind::Toggle, L"记住歌词版本选择",
        L"记住每首歌切换后的本地/在线版本；关闭后不保存新记录，但仍读取已有记录", 40.0f, kRowTallH);
    persistOrder.checked = state.qqLocalLyricsPersistOrder;
    persistOrder.enabled = state.qqLocalLyricsEnabled;
    const std::wstring localPathHint =
        state.qqLocalLyricsPath.empty() ? std::wstring(L"未配置") : state.qqLocalLyricsPath;
    Row& localPath = builder.addButton(kIdQqLocalLyricsPath, L"QQ 音乐本地歌词目录",
                                       localPathHint.c_str(), L"选择文件夹…", kRowTallH);
    localPath.enabled = state.qqLocalLyricsEnabled;

    updateLyricRows(rows, state);
    return rows;
}

void updateLyricRows(std::vector<Row>& rows, const SettingsState& s) {
    auto findRow = [&](int id) { return settings_ui::findRow(rows, id); };
    const bool vertical = s.verticalTaskbar;
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdFollowAlbum))
        row->checked = s.followAlbum;
    if (auto* row = findRow(kIdDoubleLine)) {
        row->checked = vertical ? false : s.doubleLineLyrics;
        row->enabled = !vertical;
    }
    for (int i = 0; i < kLyricEffectCount; ++i) {
        if (auto* row = findRow(kIdLyricEffectFirst + i)) {
            row->checked = (s.lyricEffects & (1u << i)) != 0;
            row->enabled = !minimal && (i != 0 || vertical || !s.doubleLineLyrics);
            if (i == static_cast<int>(LyricEffect::WordWave))
                row->enabled = !minimal && !vertical && !s.compactTaskbar;
        }
    }
    if (auto* row = findRow(kIdAlignment)) {
        row->selected = vertical ? 0 : s.lyricAlignment;
        row->enabled = !vertical && s.taskbarViewMode == 0;
    }
    if (auto* row = findRow(kIdSecondaryOn)) {
        row->checked = vertical ? false : s.secondaryEnabled;
        row->enabled = !vertical;
    }
    if (auto* row = findRow(kIdSecondaryType)) {
        row->hint = s.secondaryAvailability == SecondaryLyricAvailability::Loading
                        ? L"正在检查翻译和罗马音…"
                    : s.secondaryAvailability == SecondaryLyricAvailability::Unavailable
                        ? L"当前歌曲无翻译或罗马音"
                        : L"";
        row->showHint = !row->hint.empty();
        row->minHeight = row->showHint ? kRowTallH : kRowH;
        row->height = row->minHeight;
        row->selected = vertical ? 0 : (s.preferRomanization ? 1 : 0);
        row->enabled = !vertical && s.secondaryEnabled &&
                       s.secondaryAvailability == SecondaryLyricAvailability::Available;
    }
    if (auto* row = findRow(kIdQqLocalLyricsEnabled))
        row->checked = s.qqLocalLyricsEnabled;
    if (auto* row = findRow(kIdQqLocalLyricsPersistOrder)) {
        row->checked = s.qqLocalLyricsPersistOrder;
        row->enabled = s.qqLocalLyricsEnabled;
    }
    if (auto* row = findRow(kIdQqLocalLyricsPath)) {
        row->hint = s.qqLocalLyricsPath.empty() ? std::wstring(L"未配置") : s.qqLocalLyricsPath;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = s.qqLocalLyricsEnabled;
    }

    updateCompactLyricRows(rows, s);
}

void updateCompactLyricRows(std::vector<Row>& rows, const SettingsState& state) {
    auto findRow = [&](int id) { return settings_ui::findRow(rows, id); };
    const wchar_t* compactHint = L"小任务栏下暂时停用，恢复正常大小后自动恢复；已保留开关偏好。";
    for (int id : {kIdDoubleLine, kIdSecondaryOn}) {
        if (auto* row = findRow(id)) {
            row->hint = state.compactTaskbar ? compactHint : L"";
            row->showHint = state.compactTaskbar;
            row->minHeight = row->showHint ? kRowTallH : kRowH;
            row->height = row->minHeight;
            if (state.compactTaskbar)
                row->enabled = false;
        }
    }
    if (auto* row = findRow(kIdLyricEffectFirst)) {
        row->hint =
            state.compactTaskbar
                ? compactHint
                : L"仅单行逐字歌词生效，歌词下沉、星星跳动旋转并带流星粒子拖尾；与逐字浮波及翻译/罗马音互斥。";
        if (state.compactTaskbar)
            row->enabled = false;
    }
    if (state.compactTaskbar) {
        if (auto* row = findRow(kIdSecondaryType))
            row->enabled = false;
    }
}

bool handleLyricCommand(int id, Row& value, std::vector<Row>& rows, SettingsState& state,
                        const SettingsActions& actions) {
    Row* row = &value;
    auto findRow = [&](int rowId) { return settings_ui::findRow(rows, rowId); };
    switch (id) {
    case kIdFontColor:
        if (actions.onFontColorEffect)
            actions.onFontColorEffect();
        break;
    case kIdFollowAlbum:
        row->checked = !row->checked;
        if (actions.onFollowAlbum)
            actions.onFollowAlbum(row->checked);
        break;
    case kIdLyricEffectFirst:
    case kIdLyricEffectFirst + 1:
    case kIdLyricEffectFirst + 2:
    case kIdLyricEffectFirst + 3:
    case kIdLyricEffectFirst + 4:
        row->checked = !row->checked;
        state.lyricEffects = withLyricEffect(state.lyricEffects,
            static_cast<LyricEffect>(row->id - kIdLyricEffectFirst), row->checked);
        updateLyricRows(rows, state);
        if (actions.onLyricEffect)
            actions.onLyricEffect(row->id - kIdLyricEffectFirst, row->checked);
        break;
    case kIdDoubleLine:
        row->checked = !row->checked;
        state.doubleLineLyrics = row->checked;
        updateLyricRows(rows, state);
        if (actions.onDoubleLineLyrics)
            actions.onDoubleLineLyrics(row->checked);
        break;
    case kIdAlignment:
        if (actions.onLyricAlignment)
            actions.onLyricAlignment(row->selected);
        break;
    case kIdSecondaryOn:
        row->checked = !row->checked;
        if (actions.onSecondaryEnabled)
            actions.onSecondaryEnabled(row->checked);
        if (auto* type = findRow(kIdSecondaryType))
            type->enabled = row->checked &&
                            state.secondaryAvailability == SecondaryLyricAvailability::Available;
        break;
    case kIdSecondaryType:
        if (actions.onPreferRomanization)
            actions.onPreferRomanization(row->selected == 1);
        break;
    case kIdQqLocalLyricsEnabled:
        row->checked = !row->checked;
        if (auto* persist = findRow(kIdQqLocalLyricsPersistOrder))
            persist->enabled = row->checked;
        if (auto* path = findRow(kIdQqLocalLyricsPath))
            path->enabled = row->checked;
        if (actions.onQqLocalLyricsEnabled)
            actions.onQqLocalLyricsEnabled(row->checked);
        break;
    case kIdQqLocalLyricsPersistOrder:
        row->checked = !row->checked;
        if (actions.onQqLocalLyricsPersistOrder)
            actions.onQqLocalLyricsPersistOrder(row->checked);
        break;
    case kIdQqLocalLyricsPath:
        if (actions.onPickQqLocalLyricsPath)
            actions.onPickQqLocalLyricsPath();
        break;
    default:
        return false;
    }
    return true;
}
} // namespace settings_ui
