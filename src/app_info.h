#pragma once

namespace app_info {

// 发布新版本时同步修改这里，并使用相同的版本号创建 GitHub Release 标签。
inline constexpr wchar_t kVersion[] = L"1.5.0";
inline constexpr wchar_t kProjectUrl[] =
    L"https://github.com/H0308/qq-music-lyrics-displayer";
inline constexpr wchar_t kLatestReleasePage[] =
    L"https://github.com/H0308/qq-music-lyrics-displayer/releases/latest";
inline constexpr char kLatestReleaseApi[] =
    "https://api.github.com/repos/H0308/qq-music-lyrics-displayer/releases/latest";

} // namespace app_info
