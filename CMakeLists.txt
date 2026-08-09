cmake_minimum_required(VERSION 3.25)
project(QQMusicLyric LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(QQMusicLyric
    src/main.cpp
    src/media/smtc_monitor.cpp
    src/lyric/lyric_provider.cpp
    src/ui/lyric_window.cpp
    src/util/base64.cpp
)

target_include_directories(QQMusicLyric PRIVATE src)

target_compile_definitions(QQMusicLyric PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(QQMusicLyric PRIVATE /utf-8 /EHsc)

target_link_libraries(QQMusicLyric PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    windowsapp
    d2d1
    dwrite
    user32
    gdi32
    shell32
    ole32
)
