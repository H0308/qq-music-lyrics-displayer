<div align="center">
  <img src="asset/logo.png" width="96" alt="logo">
  <h1>QQ 音乐/网易云任务栏歌词</h1>
  <p>把 QQ 音乐和网易云音乐的歌词嵌入 Windows 11 任务栏——滚动歌词、逐字高亮、播放控制、音频频谱，程序本体为 C++ 原生实现，无 Electron。</p>
</div>

## 项目核心功能

- **任务栏内嵌歌词**：窗口作为任务栏（Shell_TrayWnd）的子窗口，外观贴近 Windows 11 原生媒体卡片——左侧圆角封面 + 歌名/歌手，右侧当前行歌词（超长自动滚动），支持 QQ 音乐和网易云音乐
- **多格式歌词与逐字高亮**：支持逐字时间轴歌词；逐字歌词按像素进度平滑填充并滚动，LRC 保持整行高亮；当前歌曲没有可用翻译或罗马音时，可选双行歌词，同时显示当前行与下一行预览
- **翻译与罗马音**：可显示部分外文歌曲的翻译或罗马音，并可从设置窗口启用、关闭及切换显示类型
- **双平台自动识别**：同时运行 QQ 音乐和网易云音乐时，根据 Windows 当前媒体会话及播放状态自动选择正在播放的来源；切换平台时同步切换歌曲、歌词和控制目标
- **播放控制**：开启悬浮播放控件后，鼠标悬浮歌词区时叠加显示「上一首 / 播放暂停 / 下一首」，控制当前已识别的播放器
- **音频频谱**：跟随当前播放器（QQ音乐/网易云音乐，其他软件出声不受影响）播放跳动的圆角柱频谱，可在设置窗口开启/关闭；频谱颜色跟随已播放歌词颜色，开启时窗口自动加宽、关闭即恢复
- **双锚定位 + 自动避让**：歌词可锚定在「通知区域左侧」或「任务栏最左侧」，自动避开小组件、搜索、任务视图、应用图标；空间不足时原位收缩，仍放不下则自动换到容得下的空闲区
- **外观自定义**：字体、字号、粗体/斜体、歌词颜色、逐字未播放色与不透明度、光晕/描边（颜色独立可调），支持歌词左/中/右对齐，设置持久化；已播放色可一键跟随专辑封面主色调
- **手动搜索歌词**：自动匹配错误时可手动搜索并选择正确版本，支持 QQ 音乐、网易云音乐和酷狗歌词候选；对话框可预览逐字滚动或逐行歌词，选择结果按平台和歌曲标识分别持久化，下次自动命中
- **开机自启动**：可通过托盘菜单设置随 Windows 登录自动运行，无需管理员权限
- **歌曲信息和封面隐藏**：通过设置窗口设置隐藏歌名、歌手、专辑信息或专辑封面
- **平台识别图标**：可在专辑封面右下角显示当前播放平台图标，便于区分 QQ 音乐和网易云音乐
- **专辑封面效果**：可在默认圆角方形封面与黑胶唱片效果之间切换；黑胶唱片在播放时旋转，暂停时保持当前位置
- **手动调整歌词进度**：对于手动搜索的歌词支持手动提前或者延迟 0.5 秒并持久化
- **无音频播放时自动隐藏**：软件挂在后台时，没有QQ音乐或者网易云音乐正在播放时，软件自动从任务栏隐藏

## 工作原理

- **播放状态**：通过 Windows SMTC（System Media Transport Controls）监听已适配的播放器会话。QQ 音乐按 `SourceAppUserModelId=QQMusic.exe` 识别；网易云音乐目前按 InfLink-rs 写入 SMTC `Genres` 的 `NCM-{歌曲ID}` 标识识别。两个平台分别处理时间线和暂停/恢复逻辑，避免互相复用进度状态
- **歌词来源**：QQ 音乐自动匹配依次尝试酷狗 KRC 逐字歌词、QQ 音乐 QRC 逐字歌词和 QQ 音乐 LRC 逐行歌词；网易云音乐按歌曲 ID 优先获取 YRC 逐字歌词，失败后回退网易云 LRC 逐行歌词。命中后会查询可用的翻译与罗马音，内置 LRU 缓存避免重复请求
- **音频频谱**：WASAPI 进程级回环捕获（`ActivateAudioInterfaceAsync`，根据当前来源捕获 `QQMusic.exe` 或 `cloudmusic.exe` 进程树，Win10 2004+ 原生支持，无驱动无注入）→ 1024 点 FFT（50% 重叠）→ 60Hz~14kHz 对数分 6 频段 →「上升立即、下降平滑」后 60fps 渲染
- **SMTC 适配结构**：QQ 音乐和网易云音乐使用独立的播放器适配器，公共层只负责会话监听、来源选择和控制转发，便于后续增加其他播放器

## 项目技术栈

| 层 | 技术 |
| --- | --- |
| 语言 / 标准 | C++20（MSVC） |
| 窗口 / 渲染 | Win32 分层窗口（UpdateLayeredWindow）、DirectComposition、Direct2D、DirectWrite |
| 系统接口 | C++/WinRT（SMTC）、WASAPI 进程级回环捕获、UI Automation（任务栏按钮避让） |
| 网络 / 数据 | libcurl、nlohmann/json、zlib |
| 构建 | CMake + Ninja + vcpkg |

程序本体以单可执行文件交付，无 Electron / 无 Node / 无托管代码。

## 构建

1. 准备环境：Visual Studio 2022（MSVC + Windows SDK）、[vcpkg](https://github.com/microsoft/vcpkg)、CMake ≥ 3.25、Ninja
2. 安装依赖（vcpkg，triplet `x64-windows`）：
   ```bash
   vcpkg install curl nlohmann-json zlib
   ```
3. 命令行构建（Ninja）：
   ```bash
   cmake -S . -B build -G Ninja ^
     -DCMAKE_TOOLCHAIN_FILE=<vcpkg 根目录>/scripts/buildsystems/vcpkg.cmake ^
     -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```
   也可以用 Visual Studio 2022 直接「打开本地文件夹」，在 CMake 设置中指定 `CMAKE_TOOLCHAIN_FILE` 和 `VCPKG_TARGET_TRIPLET=x64-windows` 后构建。

## 使用

1. 运行 `QQMusicLyric.exe`，再打开 QQ 音乐或网易云音乐播放歌曲，歌词会自动出现在任务栏上
2. 右键托盘图标：
   - **开启/关闭任务栏歌词**：显示或隐藏任务栏歌词
   - **任务栏位置**：通知区域左侧 / 任务栏最左侧（适配开始菜单居中的布局）
   - **手动搜索歌词**：自动匹配不准时手动选词，支持手动提前或者延迟0.5秒并持久化
   - **设置…**：打开设置窗口，集中调整显示、字体与颜色、歌词相关选项
   - **开机自启动**：登录 Windows 时自动运行（写入 HKCU Run 注册表项）
   - **关于**：查看版本信息、检查 GitHub Release 更新及设置启动时自动检查
   - **退出**：退出程序
3. 在 **设置…** 窗口中：
   - **显示**：显示/隐藏歌曲信息、专辑封面和平台图标，选择默认或黑胶封面效果，开启/关闭频谱，以及设置悬浮时是否显示播放控件
   - **字体与颜色**：选择字体、字号和常规/加粗/斜体/粗斜体样式，调整歌词颜色、未播放歌词不透明度、光晕/描边，并设置已播放颜色是否跟随专辑
   - **歌词**：开启双行歌词，选择左对齐/居中/右对齐，开启翻译/罗马音并切换辅助歌词类型
4. 鼠标悬浮歌词区域可显示播放控制按钮（可在设置中选择关闭，默认开启）

> QQ 音乐桌面版（`QQMusic.exe`）需要在 QQ 音乐设置中打开“显示系统媒体传输控件（SMTC）”；网易云音乐桌面版需要先按下方说明安装 BetterNCM 和 InfLink-rs。仅支持 Windows（推荐 Windows 11，频谱功能需 Windows 10 2004 及以上）。

## 网易云音乐适配（Beta）

网易云音乐桌面版需要额外的 SMTC 增强插件才能提供歌曲 ID 和可靠的播放进度。使用网易云音乐前必须安装 BetterNCM 和 InfLink-rs：

1. 从 [BetterNCM Installer](https://github.com/std-microblock/BetterNCM-Installer/releases/latest) 下载并安装 BetterNCM。
2. 打开网易云音乐的 BetterNCM 插件商店，搜索并安装 **InfLink-rs**。
   > 如果插件商店不可用，也可以从 [InfLink-rs Releases](https://github.com/apoint123/inflink-rs/releases/latest) 下载插件后手动安装。
3. 按提示重启网易云音乐，再运行或重启 `QQMusicLyric.exe`。

> 只安装 **InfLink-rs**，不要同时安装旧版 InfLink。网易云适配的可用性受 BetterNCM、InfLink-rs 以及网易云音乐客户端版本影响，不保证所有版本均可用。

## 适配与功能提示

- QQ 音乐桌面版：支持 SMTC 播放信息、歌词、手动搜索、频谱和播放控制；需要在 QQ 音乐设置中打开“显示系统媒体传输控件（SMTC）”
- 网易云音乐桌面版：支持 SMTC 播放信息、YRC/LRC 歌词、手动搜索、频谱和播放控制；属于 Beta 公测，依赖 BetterNCM + InfLink-rs
- 目前仅支持 64 位 Windows（推荐 Windows 11，频谱功能需 Windows 10 2004 及以上）
- 酷狗音乐目前还未适配

## 软件测试版本

- QQ音乐：22.5.2
- 网易云音乐：3.1.38 (64 位)(Build: 205386) Patch:5ca9ba0，额外需要安装 BetterNCM + InfLink-rs；适配版本以 InfLink-rs 项目的测试范围为准
- Windows 11版本：Windows 11 25H2 OS build：26200.9168

## 许可证

[MIT](LICENSE)

## 致谢

- QQ 音乐歌词与封面数据来自 QQ 音乐公开接口
- 网易云音乐 SMTC 增强依赖 [BetterNCM](https://github.com/std-microblock/BetterNCM-Installer) 和 [InfLink-rs](https://github.com/apoint123/inflink-rs)
- 频谱捕获方案参考微软 [Application Loopback Audio 官方示例](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- 部分歌词操作参考[Lyricify-Lyrics-Helper](https://github.com/WXRIW/Lyricify-Lyrics-Helper)
- 灵感来自各类任务栏歌词/系统监控工具（如 TrafficMonitor）的任务栏嵌入思路
