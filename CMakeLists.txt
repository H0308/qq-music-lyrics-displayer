cmake_minimum_required(VERSION 3.25)
project(QQMusicLyric LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# 生成资源脚本（嵌入应用图标）
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/resource.rc @ONLY)

add_executable(QQMusicLyric
    src/main.cpp
    src/media/smtc_monitor.cpp
    src/lyric/lyric_provider.cpp
    src/lyric/cover_provider.cpp
    src/ui/lyric_window.cpp
    src/ui/lyric_renderer.cpp
    src/ui/taskbar_host.cpp
    src/ui/manual_search_dialog.cpp
    src/util/base64.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/resource.rc
)

target_include_directories(QQMusicLyric PRIVATE src)

target_compile_definitions(QQMusicLyric PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(QQMusicLyric PRIVATE /utf-8 /EHsc)

target_link_libraries(QQMusicLyric PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB
    windowsapp
    d2d1
    dwrite
    gdiplus
    user32
    gdi32
    shell32
    ole32
    comdlg32
    windowscodecs
    advapi32
    winmm
)
