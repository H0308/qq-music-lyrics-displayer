cmake_minimum_required(VERSION 3.25)
project(QQMusicLyric LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# 生成资源脚本（嵌入深浅主题 ICO 图标）
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/resource.rc @ONLY)

# RC 里通过 1 24 "..." 引用的外部文件不会被 Ninja 自动跟踪，
# 显式声明依赖，否则改清单/图标后资源不会重编（曾导致 Debug 静默使用过期清单）
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/resource.rc
    PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/asset/logo-dark.ico;${CMAKE_CURRENT_SOURCE_DIR}/asset/logo-light.ico")

add_executable(QQMusicLyric
    src/main.cpp
    src/logging/runtime_logger.cpp
    src/media/smtc_monitor.cpp
    src/media/smtc/smtc_common.cpp
    src/media/smtc/qq_music_smtc_adapter.cpp
    src/media/smtc/netease_smtc_adapter.cpp
    src/media/audio_spectrum.cpp
    src/media/app_volume.cpp
    src/idle/idle_quote_provider.cpp
    src/idle/ticktick_provider.cpp
    src/lyric/lyric_provider.cpp
    src/lyric/qrc_decoder.cpp
    src/lyric/qrc_des.cpp
    src/lyric/cover_provider.cpp
    src/ui/lyric_renderer.cpp
    src/ui/media_control_icons.cpp
    src/ui/media_popup.cpp
    src/ui/volume_popup.cpp
    src/ui/song_toast.cpp
    src/ui/app_icon.cpp
    src/ui/platform_icon.cpp
    src/ui/fluent_dialog_surface.cpp
    src/ui/taskbar_host.cpp
    src/ui/about_dialog.cpp
    src/ui/manual_search_dialog.cpp
    src/ui/fluent_theme.cpp
    src/ui/fluent_controls.cpp
    src/ui/fluent_menu.cpp
    src/ui/font_picker_dialog.cpp
    src/ui/color_picker_dialog.cpp
    src/ui/idle_app_name_dialog.cpp
    src/ui/font_color_dialog.cpp
    src/ui/settings_dialog.cpp
    src/ui/runtime_log_dialog.cpp
    src/util/base64.cpp
    src/util/dominant_color.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/resource.rc
)

target_include_directories(QQMusicLyric PRIVATE src)

# 清单文件已通过 resource.rc 嵌入（app.manifest），关闭链接器的默认清单生成，避免重复。
# Release 使用 Windows 子系统（不弹控制台黑窗），代码入口是 main()，
# 需显式指定 CRT 入口 mainCRTStartup；Debug 保留控制台便于查看日志
target_link_options(QQMusicLyric PRIVATE /MANIFEST:NO
    $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)

target_compile_definitions(QQMusicLyric PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    $<$<CONFIG:Release>:QQMUSICLYRIC_RELEASE_BUILD=1>)
target_compile_options(QQMusicLyric PRIVATE /utf-8 /EHsc)

target_link_libraries(QQMusicLyric PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB
    windowsapp
    winhttp
    d2d1
    d3d11
    dxgi
    dcomp
    dwrite
    gdiplus
    user32
    gdi32
    shell32
    ole32
    shcore
    uiautomationcore
    dwmapi
    uxtheme
    comctl32
    windowscodecs
    advapi32
    pdh
    psapi
    winmm
    version
)
