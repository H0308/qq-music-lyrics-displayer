#include "ui/settings/sections.h"
#include "ui/settings/section_labels.h"
#include "ui/settings/section_metrics.h"
#include "ui/settings/lyric_section.h"
#include "ui/settings/spectrum_section.h"
#include <algorithm>

namespace settings_ui {
Row& SettingsSections::addRow(int page, int id, ControlKind kind, const wchar_t* text,
                              const wchar_t* hint, float controlW, float height) {
    return RowBuilder(rows[page]).addRow(id, kind, text, hint, controlW, height);
}

Row& SettingsSections::addToggle(int page, int id, const wchar_t* text, bool checked) {
    return RowBuilder(rows[page]).addToggle(id, text, checked);
}

Row& SettingsSections::addRadio(int page, int id, const wchar_t* text, const wchar_t* hint,
                                std::vector<std::wstring> options, int selected, bool enabled,
                                float height) {
    return RowBuilder(rows[page])
        .addRadio(id, text, hint, std::move(options), selected, enabled, height);
}

Row& SettingsSections::addModeGrid(int page, int id, const wchar_t* text, const wchar_t* hint,
                                   std::vector<std::wstring> options,
                                   std::vector<std::wstring> optionHints, int selected,
                                   bool enabled, float height) {
    Row& row = addRow(page, id, ControlKind::ModeGrid, text, hint, 0.0f, height);
    row.options = std::move(options);
    row.optionHints = std::move(optionHints);
    row.selected = selected;
    row.enabled = enabled;
    return row;
}

Row& SettingsSections::addSlider(int page, int id, const wchar_t* text, int value, bool enabled,
                                 const wchar_t* hint, float height) {
    return RowBuilder(rows[page]).addSlider(id, text, value, enabled, hint, height);
}

Row& SettingsSections::addButton(int page, int id, const wchar_t* text, const wchar_t* hint,
                                 const wchar_t* controlText, float height) {
    return RowBuilder(rows[page]).addButton(id, text, hint, controlText, height);
}

Row& SettingsSections::addHeader(int page, const wchar_t* text) {
    return RowBuilder(rows[page]).addHeader(text);
}

void SettingsSections::createDisplayRows() {
    const bool minimal = minimalModeActive();
    const bool vertical = state.verticalTaskbar;
    addHeader(0, L"主题");
    addRadio(0, kIdTaskbarTheme, L"任务栏内容主题",
             L"“系统”和“应用”分别跟随 Windows 的系统模式与应用模式。",
             {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.taskbarThemeMode), true,
             kRowTallH);
    addRadio(0, kIdWindowTheme, L"对话框与悬浮窗主题",
             L"独立设置普通对话框与悬浮窗的深浅色，不影响任务栏内容。",
             {L"系统", L"应用", L"浅色", L"深色"}, themeModeIndex(state.windowThemeMode), true,
             kRowTallH);
    addHeader(0, L"字体");
    Row& font = addButton(0, kIdPickFont, L"通用字体", kFontSettingNotice, L"选择字体…", kRowTallH);
    font.valueText = state.fontDesc;
    addHeader(0, L"歌曲信息与封面");
    Row& songInfo =
        addToggle(0, kIdSongInfo, L"显示歌曲信息", vertical ? false : state.songInfoVisible);
    songInfo.enabled = !vertical;
    addToggle(0, kIdAlbumCover, L"显示专辑封面", state.albumCoverVisible);
    Row& platformIcon = addToggle(0, kIdPlatformIcon, L"显示平台图标", state.platformIconVisible);
    platformIcon.enabled = state.albumCoverVisible;
    addRadio(0, kIdCoverEffect, L"专辑封面效果", nullptr, {L"默认", L"黑胶唱片"},
             minimal ? 0 : (state.coverEffectVinyl ? 1 : 0), state.albumCoverVisible && !minimal,
             kCoverEffectRowH);
    addHeader(0, L"菜单交互");
    Row& taskbarContextMenu = addRow(
        0, kIdTaskbarContextMenu, ControlKind::Toggle, L"右键显示任务栏歌词菜单",
        L"关闭后，右键任务栏歌词不再弹出菜单；托盘图标的右键菜单不受影响。", 40.0f, kRowTallH);
    taskbarContextMenu.checked = state.taskbarContextMenu;
}

void SettingsSections::createTaskbarModeRows() {
    const bool vertical = state.verticalTaskbar;
    addHeader(kTaskbarModePage, L"展示模式");
    addRadio(kTaskbarModePage, kIdTaskbarViewMode, L"任务栏歌词展示模式",
             taskbarViewModeHint(state.mediaSessionAlive, vertical),
             {L"内嵌", L"沉浸", L"Dock 模式"}, std::clamp(state.taskbarViewMode, 0, 2), true,
             kRowTallH);
    Row& taskbarMonitor = addButton(
        kTaskbarModePage, kIdTaskbarMonitor, L"目标显示器",
        L"内嵌模式固定显示在主显示器；沉浸模式覆盖所选任务栏，全部显示器时只覆盖横向任务栏。Dock 模式占用对应显示器的工作区；断开后切回主显示器。",
        state.taskbarMonitorLabel.c_str(), kRowTallH);
    taskbarMonitor.enabled = state.taskbarViewMode != 0;
    addRadio(kTaskbarModePage, kIdAppBarEdge, L"Dock 位置", L"当前仅支持水平顶部和底部。",
             {L"顶部", L"底部"}, std::clamp(state.appBarEdge, 0, 1), state.taskbarViewMode == 2,
             kRowH);
    addHeader(kTaskbarModePage, L"沉浸模式背景");
    addRadio(kTaskbarModePage, kIdImmersiveBackgroundAdjustment, L"调整方式", nullptr,
             {L"模糊程度", L"不透明度"}, std::clamp(state.immersiveBackgroundAdjustment, 0, 1),
             !vertical && state.taskbarViewMode == 1 && state.backgroundBlurSupported, kRowH);
    addSlider(kTaskbarModePage, kIdImmersiveBackgroundBlur,
              backgroundAdjustmentLabel(true, state.immersiveBackgroundAdjustment),
              state.immersiveBackgroundBlur,
              !vertical && state.taskbarViewMode == 1 && state.backgroundBlurSupported,
              backgroundAdjustmentHint(state.backgroundBlurSupported,
                                       state.immersiveBackgroundAdjustment),
              kRowTallH);
    addHeader(kTaskbarModePage, L"Dock 背景");
    addRadio(kTaskbarModePage, kIdDockBackgroundAdjustment, L"调整方式", nullptr,
             {L"模糊程度", L"不透明度"}, std::clamp(state.dockBackgroundAdjustment, 0, 1),
             state.taskbarViewMode == 2 && state.backgroundBlurSupported, kRowH);
    addSlider(
        kTaskbarModePage, kIdDockBackgroundBlur,
        backgroundAdjustmentLabel(false, state.dockBackgroundAdjustment), state.dockBackgroundBlur,
        state.taskbarViewMode == 2 && state.backgroundBlurSupported,
        backgroundAdjustmentHint(state.backgroundBlurSupported, state.dockBackgroundAdjustment),
        kRowTallH);
    addHeader(kTaskbarModePage, L"资源监视");
    Row& embeddedResources = addRow(
        kTaskbarModePage, kIdEmbeddedResourceMonitor, ControlKind::Toggle, L"内嵌模式显示资源监视",
        L"在任务栏右侧显示 CPU 和内存；扩展指标依次补位，超出一页可用滚轮切换。", 40.0f, kRowTallH);
    embeddedResources.checked = state.embeddedResourceMonitor;
    embeddedResources.enabled = state.taskbarViewMode == 0 && !vertical;
    Row& download =
        addRow(kTaskbarModePage, kIdDockResourceDownload, ControlKind::Toggle, L"显示下行速度",
               L"扩展指标；关闭后由后续指标补位，没有其他指标时释放对应宽度。", 40.0f, kRowTallH);
    download.checked = state.dockResourceDownload;
    Row& upload =
        addRow(kTaskbarModePage, kIdDockResourceUpload, ControlKind::Toggle, L"显示上行速度",
               L"扩展指标；关闭后由后续指标补位，没有其他指标时释放对应宽度。", 40.0f, kRowTallH);
    upload.checked = state.dockResourceUpload;
    Row& dockResourceGpu =
        addRow(kTaskbarModePage, kIdDockResourceGpuUsage, ControlKind::Toggle, L"显示 GPU 使用率",
               L"示例：GPU: 7%；没有可用 GPU 性能计数器时显示“—”。", 40.0f, kRowTallH);
    dockResourceGpu.checked = state.dockResourceGpuUsage;
    Row& dockResourceCpuFrequency =
        addRow(kTaskbarModePage, kIdDockResourceCpuFrequency, ControlKind::Toggle, L"显示 CPU 频率",
               L"示例：频率: 3.6GHz；系统未提供当前频率时显示“—”。", 40.0f, kRowTallH);
    dockResourceCpuFrequency.checked = state.dockResourceCpuFrequency;
    const bool resourceExtrasEnabled =
        state.taskbarViewMode == 2 ||
        (state.taskbarViewMode == 0 && !vertical && state.embeddedResourceMonitor);
    if (auto* row = findRow(kIdDockResourceDownload))
        row->enabled = resourceExtrasEnabled;
    if (auto* row = findRow(kIdDockResourceUpload))
        row->enabled = resourceExtrasEnabled;
    if (auto* row = findRow(kIdDockResourceGpuUsage))
        row->enabled = resourceExtrasEnabled;
    if (auto* row = findRow(kIdDockResourceCpuFrequency))
        row->enabled = resourceExtrasEnabled;
}

void SettingsSections::createIdleRows() {
    const bool vertical = state.verticalTaskbar;
    addHeader(kIdlePage, L"空闲内容");
    Row& idleEntry = addRow(kIdlePage, kIdIdleEntry, ControlKind::Toggle, L"无播放时保留任务栏入口",
                            L"播放器未运行时，任务栏显示空闲内容；悬浮后可打开已配置的应用。"
                            L"关闭后，播放器未运行时隐藏任务栏入口。",
                            40.0f, kRowTallH);
    idleEntry.checked = state.idleEntryEnabled;
    addRadio(kIdlePage, kIdIdleQuote, L"空闲文字",
             L"“不显示”需先开启内嵌模式资源监视；滴答清单仍会在启动时播报一次今日任务。",
             {L"每日一言", L"欢迎语", L"不显示"}, idleTextMode(), true, kRowTallH);
    addButton(
        kIdlePage, kIdIdleCustomWelcome, L"自定义欢迎语",
        L"选择欢迎语时显示自定义内容，最多 20 个字符且不能为纯空格；留空则恢复按时间和日期类型生成的默认欢迎语。",
        L"编辑…", kRowTallH);
    addRadio(kIdlePage, kIdIdleQuoteSource, L"每日一言来源",
             idleQuoteSourceHint(state.idleQuoteSource), {L"一言", L"今日诗词", L"网易云热评"},
             state.idleQuoteSource, state.idleQuoteEnabled, kRowTallH);
    addRadio(kIdlePage, kIdIdleQuoteRefreshInterval, L"每日一言更新频率", nullptr,
             {L"每天", L"每 12 小时", L"每小时"}, state.idleQuoteRefreshInterval,
             state.idleQuoteEnabled, kRowTallH);
    addRadio(kIdlePage, kIdIdleQuoteAlignment, L"每日一言字体对齐方式",
             L"仅影响任务栏和悬浮卡片中的每日一言，不影响正常歌词对齐。",
             {L"左对齐", L"居中", L"右对齐"}, vertical ? 0 : state.idleQuoteAlignment,
             state.idleQuoteEnabled && !vertical, kRowH);
    addHeader(kIdlePage, L"快捷启动");
    Row& idleApps = addRow(
        kIdlePage, kIdIdleApps, ControlKind::AppList, L"可打开的应用",
        L"可拖动应用条目调整快速启动顺序；通过文件选择器添加本地 EXE，最多添加 10 个应用；开关统一控制名称显示。",
        0.0f,
        kIdleAppsBaseH +
            static_cast<float>(std::max<size_t>(1, state.idleApps.size())) * kIdleAppItemH);
    idleApps.checked = state.idleAppNamesVisible;
    idleApps.height = idleApps.minHeight;
    idleApps.enabled = state.idleEntryEnabled;
    updateIdleRowsEnabled();
    addHeader(kIdlePage, L"滴答清单");
    Row& tickTickEnabled =
        addRow(kIdlePage, kIdTickTickEnabled, ControlKind::Toggle, L"启用滴答清单",
               L"开启后自动同步今日任务，并在快捷启动卡片中显示今日任务入口。", 40.0f, kRowTallH);
    tickTickEnabled.checked = state.tickTickEnabled;
    addButton(kIdlePage, kIdTickTickApiToken, L"API 口令",
              L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建后填写。",
              state.tickTickApiTokenConfigured ? L"已配置" : L"编辑…", kRowTallH);
    addButton(kIdlePage, kIdTickTickConnect, L"连接滴答清单",
              L"验证普通用户 API 口令，并读取今天的未完成任务。",
              state.tickTickConnected ? L"重新连接" : L"连接…", kRowTallH);
    addButton(kIdlePage, kIdTickTickRefresh, L"同步今日任务",
              L"从滴答清单读取今天的未完成任务，并刷新卡片内容。",
              state.tickTickSyncing ? L"同步中…" : L"立即同步", kRowH);
    addButton(kIdlePage, kIdTickTickDisconnect, L"断开连接", L"删除本机保存的滴答清单 API 口令。",
              L"断开", kRowH);
    updateTickTickRows();
}

void SettingsSections::createBackgroundRows() {
    const bool minimal = minimalModeActive();
    addHeader(0, L"背景效果");
    Row& progressBackground = addRow(
        0, kIdProgressBackground, ControlKind::Toggle, L"播放进度背景",
        L"从窗口左缘到歌词右缘，按播放进度填充专辑主题色；与频谱的背景波浪互斥", 40.0f, kRowTallH);
    progressBackground.checked = minimal ? false : state.progressBackground;
    addSlider(0, kIdProgressBackgroundOpacity, L"进度背景不透明度", state.progressBackgroundOpacity,
              !minimal && state.progressBackground);
    addRadio(
        0, kIdTaskbarBackground, L"任务栏内容背景",
        L"封面模糊将专辑封面高斯模糊后铺满背景；纯色跟随任务栏深浅色；画在最底层，可与播放进度背景、背景波浪叠加",
        {L"无", L"封面模糊", L"纯色"}, minimal ? 0 : state.taskbarBackground, !minimal, kRowTallH);
    addSlider(0, kIdCoverBackgroundOpacity, L"封面背景不透明度", state.coverBackgroundOpacity,
              !minimal && state.taskbarBackground == 1);
    addRadio(
        0, kIdIdleQuoteBackground, L"任务栏内容动态背景",
        L"选择任务栏动态背景效果；作用范围在下方单独设置，关闭的内容区域不绘制背景，悬浮卡片不使用此效果，极简模式下暂时关闭。",
        {L"无", L"落叶", L"闪烁星星", L"二进制", L"流光粒子"}, state.idleQuoteBackground, !minimal,
        kIdleQuoteBackgroundRowH);
    addModeGrid(
        0, kIdIdleQuoteBackgroundScope, L"动态背景作用范围",
        L"仅歌词覆盖已显示的封面、歌曲信息、歌词和频谱区域；仅待机内容覆盖每日一言和资源监视区域，各区域关闭后不绘制；都启用时覆盖整个任务栏内容窗口。",
        {L"都不启用", L"仅待机内容启用", L"仅歌词启用", L"都启用"},
        {L"不显示动态背景", L"每日一言和资源监视区域", L"封面、歌曲信息、歌词和频谱区域", L"整个任务栏内容区域"},
        state.idleQuoteBackgroundScope, !minimal && state.idleQuoteBackground != 0, kModeGridMinH);
}

void SettingsSections::createMediaControlRows() {
    const bool minimal = minimalModeActive();
    addHeader(kMediaPopupPage, L"播放控件");
    addToggle(kMediaPopupPage, kIdHoverControls, L"显示播放控件", state.hoverControls);
    addRadio(
        kMediaPopupPage, kIdHoverControlStyle, L"控件样式",
        L"内嵌控件在鼠标悬浮时显示，仅提供播放操作；媒体卡片还显示歌词进度，其打开方式由“悬浮卡片”中的“悬浮卡片展开方式”决定，点击应用图标或名称可打开播放器。",
        {L"内嵌控件", L"媒体卡片"}, minimal ? 0 : state.hoverControlStyle,
        state.hoverControls && !minimal, kHoverControlStyleRowH);
}

void SettingsSections::createFloatingCardRows() {
    const bool minimal = minimalModeActive();
    addHeader(kFloatingCardPage, L"行为");
    addRadio(
        kFloatingCardPage, kIdFloatingCardTrigger, L"悬浮卡片展开方式",
        L"悬浮展开：鼠标在歌词区域停留片刻后展开；点击展开：点击歌词区域任意位置立即展开。媒体卡片、每日一言和快捷启动卡片共用此设置。",
        {L"悬浮展开", L"点击展开"}, state.floatingCardTrigger, !minimal, kRowTallH);
    addHeader(kFloatingCardPage, L"背景");
    addRadio(kFloatingCardPage, kIdFloatingCardBackground, L"悬浮卡片背景",
             L"音乐控件、每日一言和快捷启动卡片共用纯色或 Windows 磨砂玻璃背景。",
             {L"纯色", L"磨砂玻璃"}, state.floatingCardBackground, !minimal, kRowTallH);
    Row& floatingCardBackgroundColor =
        addButton(kFloatingCardPage, kIdFloatingCardBackgroundColor, L"悬浮卡片背景颜色",
                  L"磨砂背景未使用有效专辑色时，媒体卡片、每日一言和快捷启动卡片共用此颜色。",
                  colorText(state.floatingCardBackgroundColor).c_str(), kRowTallH);
    floatingCardBackgroundColor.enabled = !minimal && state.floatingCardBackground == 1;
    Row& floatingCardFollowAlbum = addRow(
        kFloatingCardPage, kIdFloatingCardFollowAlbum, ControlKind::Toggle, L"磨砂玻璃颜色跟随专辑",
        L"播放音乐且提取到有效专辑色时，磨砂卡片使用专辑色；无播放时使用上面的自定义颜色。", 40.0f,
        kRowTallH);
    floatingCardFollowAlbum.checked = state.floatingCardFollowAlbum;
    addHeader(kFloatingCardPage, L"文字");
    Row& floatingCardAutoTextContrast = addRow(
        kFloatingCardPage, kIdFloatingCardAutoTextContrast, ControlKind::Toggle,
        L"磨砂卡片字体颜色动态变化",
        L"根据卡片背后内容的明暗，自动切换黑色或白色文字；媒体卡片、每日一言和快捷启动卡片共用此设置。",
        40.0f, kRowTallH);
    floatingCardAutoTextContrast.checked = state.floatingCardAutoTextContrast;
    updateFloatingCardRowsEnabled();
}

void SettingsSections::createSongToastRows() {
    const bool minimal = minimalModeActive();
    addHeader(kSongToastPage, L"显示与时长");
    Row& songToast =
        addRow(kSongToastPage, kIdSongToast, ControlKind::Toggle, L"切歌时弹出歌曲信息",
               L"在所选显示器短暂弹出封面、标题和艺术家；弹窗磨砂半透明，"
               L"始终不响应鼠标操作",
               40.0f, kRowTallH);
    songToast.checked = minimal ? false : state.songToastEnabled;
    songToast.enabled = !minimal;
    Row& songToastDuration =
        addSlider(kSongToastPage, kIdSongToastDuration, L"切歌弹窗显示时长",
                  state.songToastDurationSec, !minimal && state.songToastEnabled);
    songToastDuration.minValue = 1;
    songToastDuration.maxValue = 10;
    songToastDuration.valueSuffix = L" 秒";
    Row& songToastSkipFullscreen = addToggle(kSongToastPage, kIdSongToastSkipFullscreen,
                                             L"全屏应用时关闭弹窗", state.songToastSkipFullscreen);
    songToastSkipFullscreen.enabled = !minimal && state.songToastEnabled;
    addHeader(kSongToastPage, L"弹出位置");
    addRadio(kSongToastPage, kIdSongToastMonitor, L"显示器选择",
             L"在每次弹窗出现时确定显示器，不随鼠标移动。", {L"主显示器", L"当前鼠标所在显示器"},
             state.songToastFollowMouseMonitor ? 1 : 0, !minimal && state.songToastEnabled,
             kRowTallH);
    addRadio(kSongToastPage, kIdSongToastPosition, L"切歌弹窗位置", nullptr, {L"中上", L"中下"},
             state.songToastPosition, !minimal && state.songToastEnabled, kSongToastPositionRowH);
    addHeader(kSongToastPage, L"效果预览");
    Row& songToastPreview =
        addButton(kSongToastPage, kIdSongToastPreview, L"预览切歌弹窗",
                  L"使用当前歌曲，按所选显示器、位置和停留时间展示；需要开启弹窗且当前有歌曲。",
                  L"预览弹窗", kRowTallH);
    songToastPreview.enabled =
        !minimal && state.songToastEnabled && state.songToastPreviewAvailable;
}

void SettingsSections::createPerformanceRows() {
    addHeader(kPerformancePage, L"渲染与资源占用");
    addModeGrid(kPerformancePage, kIdRenderMode, L"性能模式",
                L"模式仅本次运行有效，重启软件后恢复正常模式",
                {L"正常", L"低渲染", L"完全停止", L"极简"},
                {L"完整视觉效果", L"降低帧率，节省 GPU", L"停止渲染，保留监听",
                 L"横向歌词，关闭逐字与转场"},
                std::clamp(state.renderMode, 0, kRenderModeMinimal), true, kModeGridMinH);
}

void SettingsSections::create() {
    createDisplayRows();
    createTaskbarModeRows();
    createIdleRows();
    createBackgroundRows();
    createMediaControlRows();
    createFloatingCardRows();
    createSongToastRows();
    createPerformanceRows();
    rows[kSpectrumPage] = settings_ui::createSpectrumRows(state);
    rows[kLyricsPage] = settings_ui::createLyricRows(state);
    updateProgressBackgroundRowsEnabled();
    updateCompactLyricRows();
    updateImmersiveRowsEnabled();
}

} // namespace settings_ui
