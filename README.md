<div align="center">
  <img src="asset/logo.png" width="96" alt="logo">
  <h1>QQ 音乐任务栏歌词</h1>
  <p>把 QQ 音乐的歌词嵌入 Windows 11 任务栏——滚动歌词、逐字高亮、播放控制、音频频谱，单文件 C++ 原生实现，无 Electron。</p>
</div>

## 项目核心功能

- **任务栏内嵌歌词**：窗口作为任务栏（Shell_TrayWnd）的子窗口，外观贴近 Windows 11 原生媒体卡片——左侧圆角封面 + 歌名/歌手，右侧当前行歌词（超长自动滚动）
- **逐字（卡拉 OK）高亮**：支持逐字时间轴歌词，按像素进度平滑填充已唱部分；逐行 LRC 则整行高亮
- **播放控制**：鼠标悬浮歌词区时叠加显示「上一首 / 播放暂停 / 下一首」，直接控制 QQ 音乐
- **音频频谱**：跟随 QQ 音乐播放跳动的圆角柱频谱（仅捕获 QQ 音乐进程的声音，其他软件出声不受影响），可在右键菜单手动开启/关闭，开启时窗口自动加宽、关闭即恢复
- **双锚定位 + 自动避让**：歌词可锚定在「通知区域左侧」或「任务栏最左侧」，自动避开小组件、搜索、任务视图、应用图标；空间不足时原位收缩，仍放不下则自动换到容得下的空闲区
- **外观自定义**：字体、字号、歌词颜色、逐字未播放色与透明度、光晕/描边（颜色独立可调），设置持久化
- **手动搜索歌词**：自动匹配错误时可手动搜索并选择正确版本，选择结果持久化，下次自动命中

## 工作原理

- **播放状态**：通过 Windows SMTC（System Media Transport Controls）监听 `QQMusic.exe` 会话（按 SourceAppUserModelId 精确过滤，不受其他播放器干扰）；SMTC 进度只有约 1 秒粒度且带抖动，本地按「锚点 + 时钟插值」推算平滑进度，驱动逐字高亮
- **歌词来源**：QQ 音乐公开接口（无需登录）——搜索 →「标题完全匹配 + 歌手包含 + 时长差 ≤ 2s」三重匹配 → 下载 → base64 解码逐行 LRC；内置 LRU 缓存避免重复请求
- **音频频谱**：WASAPI 进程级回环捕获（`ActivateAudioInterfaceAsync`，仅 QQMusic.exe 进程树，Win10 2004+ 原生支持，无驱动无注入）→ 1024 点 FFT（50% 重叠）→ 60Hz~14kHz 对数分 6 频段 → 「上升立即、下降平滑」后 60fps 渲染

## 项目技术栈

| 层 | 技术 |
| --- | --- |
| 语言 / 标准 | C++20（MSVC） |
| 窗口 / 渲染 | Win32 分层窗口（UpdateLayeredWindow）、Direct2D、DirectWrite |
| 系统接口 | C++/WinRT（SMTC）、WASAPI 进程级回环捕获、UI Automation（任务栏按钮避让） |
| 网络 / 数据 | libcurl、nlohmann/json、zlib |
| 构建 | CMake + Ninja + vcpkg |

单可执行文件交付，无 Electron / 无 Node / 无托管代码。

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

1. 运行 `QQMusicLyric.exe`，再打开 QQ 音乐播放歌曲，歌词会自动出现在任务栏上
2. 右键托盘图标：
   - **开启/关闭任务栏歌词**：显示或隐藏任务栏歌词
   - **任务栏位置**：通知区域左侧 / 任务栏最左侧（适配开始菜单居中的布局）
   - **频谱**：开启/关闭音频频谱
   - **字体…** / **字体颜色与效果…**：外观自定义
   - **手动搜索歌词**：自动匹配不准时手动选词
3. 鼠标悬浮歌词区域可显示播放控制按钮

> 目前仅支持 QQ 音乐桌面版（`QQMusic.exe`），并且需要在QQ音乐设置中打开“显示系统媒体传输控件(SMTC）”；仅支持 Windows（推荐 Windows 11，频谱功能需 Windows 10 2004 及以上）。

## 适配与功能提示

- 目前软件只支持同步QQ音乐播放信息，后续考虑接入酷狗音乐、网易云音乐。
- 目前软件只支持64位Windows 11系统

## 软件测试版本

- QQ音乐：22.5.2
- Windows 11版本：Windows 11 25H2 OS build：26200.9168

## 许可证

[MIT](LICENSE)

## 致谢

- 歌词与封面数据来自 QQ 音乐公开接口
- 频谱捕获方案参考微软 [Application Loopback Audio 官方示例](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- 灵感来自各类任务栏歌词/系统监控工具（如 TrafficMonitor）的任务栏嵌入思路
