#include "ui/settings/spectrum_section.h"
#include "ui/settings/ids.h"
#include <algorithm>

namespace settings_ui {
namespace {
constexpr float kSpectrumStyleRowH = 252.0f;
}
std::vector<Row> createSpectrumRows(const SettingsState& state) {
    std::vector<Row> rows;
    RowBuilder builder(rows);
    const bool vertical = state.verticalTaskbar;
    const bool minimal = state.renderMode == kRenderModeMinimal;
    const bool spectrumEnabled = !minimal && !vertical;
    builder.addHeader(L"启用与配色");
    Row& spectrum =
        builder.addToggle(kIdSpectrum, L"频谱", spectrumEnabled ? state.spectrumOn : false);
    spectrum.enabled = spectrumEnabled;
    builder.addRadio(
        kIdSpectrumColorMode, L"频谱颜色",
        L"跟随已播放色恢复原有配色；跟随专辑主题色与歌词设置行为一致；自定义颜色作为频谱基色。",
        {L"跟随已播放色", L"跟随专辑主题色", L"自定义颜色"},
        state.spectrumCustomColor   ? 2
        : state.spectrumFollowAlbum ? 1
                                    : 0,
        state.spectrumOn && spectrumEnabled, kRowTallH);
    Row& spectrumColor =
        builder.addButton(kIdSpectrumColor, L"自定义频谱颜色", L"只在上面选择“自定义颜色”时生效。",
                          colorText(state.spectrumColor).c_str(), kRowTallH);
    spectrumColor.enabled = state.spectrumOn && state.spectrumCustomColor && spectrumEnabled;
    builder.addHeader(L"频谱样式");
    builder.addRadio(kIdSpectrumStyle, L"选择样式", nullptr,
                     {L"默认", L"柱状图", L"梦幻波浪", L"分段电平", L"底缘频谱带"},
                     vertical ? 0 : state.spectrumStyle, state.spectrumOn && spectrumEnabled,
                     kSpectrumStyleRowH);
    builder.addHeader(L"频谱特效");
    const wchar_t* effectNames[kSpectrumEffectCount] = {L"柔光辉晕", L"衰减残影", L"峰值散粒",
                                                        L"流动光泽"};
    for (int i = 0; i < kSpectrumEffectCount; ++i) {
        Row& effect = builder.addToggle(
            kIdSpectrumEffectFirst + i, effectNames[i],
            (state.spectrumEffects & spectrumEffectBit(kSpectrumEffects[i])) != 0);
        effect.enabled = state.spectrumOn && spectrumEnabled;
    }
    Row& spectrumGradient = builder.addToggle(kIdSpectrumGradient, L"频谱渐变色",
                                              spectrumEnabled ? state.spectrumGradient : false);
    spectrumGradient.enabled = state.spectrumOn && spectrumEnabled;
    Row& spectrumBackground =
        builder.addRow(kIdSpectrumBackground, ControlKind::Toggle, L"背景波浪",
                       L"仅在选择“梦幻波浪”样式时可开启。", 40.0f, kRowTallH);
    spectrumBackground.checked = spectrumEnabled ? state.spectrumBackground : false;
    spectrumBackground.enabled = state.spectrumOn && state.spectrumStyle == 2 && spectrumEnabled;
    builder.addSlider(kIdSpectrumOpacity, L"背景波浪不透明度", state.spectrumOpacity,
                      state.spectrumOn && state.spectrumStyle == 2 && state.spectrumBackground &&
                          spectrumEnabled);

    updateSpectrumRows(rows, state);
    return rows;
}
void updateSpectrumRows(std::vector<Row>& rows, const SettingsState& s) {
    auto findRow = [&](int id) { return settings_ui::findRow(rows, id); };
    const bool vertical = s.verticalTaskbar;
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdSpectrum)) {
        row->checked = minimal || vertical ? false : s.spectrumOn;
        row->enabled = !minimal && !vertical;
    }
    if (auto* row = findRow(kIdSpectrumColorMode)) {
        row->selected = vertical ? 0 : (s.spectrumCustomColor ? 2 : s.spectrumFollowAlbum ? 1 : 0);
        row->enabled = s.spectrumOn && !minimal && !vertical;
    }
    if (auto* row = findRow(kIdSpectrumColor)) {
        row->controlText = colorText(s.spectrumColor);
        row->enabled = s.spectrumOn && s.spectrumCustomColor && !minimal && !vertical;
    }
    if (auto* row = findRow(kIdSpectrumStyle)) {
        row->selected = vertical ? 0 : std::clamp(s.spectrumStyle, 0, 4);
        row->enabled = s.spectrumOn && !minimal && !vertical;
    }
    if (auto* row = findRow(kIdSpectrumGradient)) {
        row->checked = minimal || vertical ? false : s.spectrumGradient;
        row->enabled = s.spectrumOn && !minimal && !vertical;
    }
    for (int i = 0; i < kSpectrumEffectCount; ++i) {
        if (auto* row = findRow(kIdSpectrumEffectFirst + i)) {
            row->checked = (s.spectrumEffects & spectrumEffectBit(kSpectrumEffects[i])) != 0;
            row->enabled = s.spectrumOn && !minimal && !vertical;
        }
    }
    if (auto* row = findRow(kIdSpectrumBackground)) {
        row->checked = minimal || vertical ? false : s.spectrumBackground;
        row->enabled = s.spectrumOn && s.spectrumStyle == 2 && !minimal && !vertical;
    }
    if (auto* row = findRow(kIdSpectrumOpacity)) {
        row->value = std::clamp(s.spectrumOpacity, 0, 100);
        row->enabled =
            s.spectrumOn && s.spectrumStyle == 2 && s.spectrumBackground && !minimal && !vertical;
    }
}
} // namespace settings_ui
