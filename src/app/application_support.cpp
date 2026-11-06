#include "app/application_internal.h"
namespace lyrics_app {
const wchar_t* trayCommandName(int command) {
    switch (command) {
    case kCmdToggleTaskbar:
        return L"toggle-taskbar";
    case kCmdTaskbarPosNotify:
        return L"taskbar-position-notify";
    case kCmdTaskbarPosLeft:
        return L"taskbar-position-left";
    case kCmdSpectrum:
        return L"spectrum";
    case kCmdAutoStart:
        return L"autostart";
    case kCmdFollowAlbum:
        return L"follow-album";
    case kCmdSecondaryTranslation:
        return L"secondary-translation";
    case kCmdSecondaryRomanization:
        return L"secondary-romanization";
    case kCmdSongInfo:
        return L"song-info";
    case kCmdAlbumCover:
        return L"album-cover";
    case kCmdAlbumCoverEffectDefault:
        return L"album-cover-effect-default";
    case kCmdAlbumCoverEffectVinyl:
        return L"album-cover-effect-vinyl";
    case kCmdDoubleLineLyrics:
        return L"double-line-lyrics";
    case kCmdPlatformIcon:
        return L"platform-icon";
    case kCmdAbout:
        return L"about";
    case kCmdLyricAlignLeft:
        return L"lyric-align-left";
    case kCmdLyricAlignCenter:
        return L"lyric-align-center";
    case kCmdLyricAlignRight:
        return L"lyric-align-right";
    case kCmdHoverPlaybackControls:
        return L"hover-playback-controls";
    case kCmdSettings:
        return L"settings";
    case kCmdSwitchLyricSource:
        return L"switch-lyric-source";
    case kCmdRuntimeLog:
        return L"runtime-log";
    case kCmdRenderModeNormal:
        return L"render-mode-normal";
    case kCmdRenderModeLow:
        return L"render-mode-low";
    case kCmdRenderModeStopped:
        return L"render-mode-stopped";
    case kCmdRenderModeMinimal:
        return L"render-mode-minimal";
    case kCmdSecondaryOff:
        return L"secondary-off";
    case kCmdTaskbarModeEmbedded:
        return L"taskbar-mode-embedded";
    case kCmdTaskbarModeImmersive:
        return L"taskbar-mode-immersive";
    case kCmdTaskbarModeAppBar:
        return L"taskbar-mode-appbar";
    case kCmdAppBarTop:
        return L"appbar-top";
    case kCmdAppBarBottom:
        return L"appbar-bottom";
    case kCmdPickFont:
        return L"pick-font";
    case kCmdFontColorEffect:
        return L"font-color-effect";
    case kCmdManualSearch:
        return L"manual-search";
    case kCmdRestart:
        return L"restart";
    case kCmdExit:
        return L"exit";
    default:
        return L"unknown";
    }
}

const wchar_t* coverSourceDisplayName(CoverSource source) {
    switch (source) {
    case CoverSource::Smtc:
        return L"系统 SMTC";
    case CoverSource::QqApi:
        return L"QQ 音乐接口";
    case CoverSource::NeteaseApi:
        return L"网易云音乐接口";
    default:
        return L"";
    }
}

COLORREF defaultFloatingCardBackgroundColor() {
    return fluent::isDarkMode(fluent::ThemeTarget::Window) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

const wchar_t* fontStyleLabel(LyricFontStyle style) {
    switch (style) {
    case LyricFontStyle::Bold:
        return L"加粗";
    case LyricFontStyle::Italic:
        return L"斜体";
    case LyricFontStyle::BoldItalic:
        return L"粗斜体";
    default:
        return L"常规";
    }
}

int spectrumStyleIndex(SpectrumStyle style) {
    switch (style) {
    case SpectrumStyle::Bars:
        return 1;
    case SpectrumStyle::DreamyWave:
        return 2;
    case SpectrumStyle::Segmented:
        return 3;
    case SpectrumStyle::BottomBand:
        return 4;
    case SpectrumStyle::Default:
    default:
        return 0;
    }
}

int idleQuoteBackgroundIndex(IdleQuoteBackground background) {
    switch (background) {
    case IdleQuoteBackground::FallingLeaves:
        return 1;
    case IdleQuoteBackground::TwinklingStars:
        return 2;
    case IdleQuoteBackground::BinaryRain:
        return 3;
    case IdleQuoteBackground::FloatingParticles:
        return 4;
    case IdleQuoteBackground::None:
    default:
        return 0;
    }
}

int idleQuoteBackgroundScopeIndex(IdleQuoteBackgroundScope scope) {
    switch (scope) {
    case IdleQuoteBackgroundScope::Lyrics:
        return 2;
    case IdleQuoteBackgroundScope::All:
        return 3;
    case IdleQuoteBackgroundScope::None:
        return 0;
    case IdleQuoteBackgroundScope::DailyQuote:
    default:
        return 1;
    }
}

std::wstring normalizeIdleCustomWelcome(std::wstring text) {
    if (text.size() > kIdleCustomWelcomeMaxLen)
        text.resize(kIdleCustomWelcomeMaxLen);
    const bool allSpace =
        std::all_of(text.begin(), text.end(), [](wchar_t c) { return iswspace(c) != 0; });
    if (allSpace)
        text.clear();
    return text;
}

bool validExePath(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".exe") == 0;
}

std::wstring fallbackExeName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
    std::wstring name = path.substr(start);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
        name.resize(dot);
    return name;
}

std::wstring configDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        dir = p;
        CoTaskMemFree(p);
        dir += L"\\QQMusicLyric";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

bool autoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG ret = RegQueryValueExW(key, kAutoStartValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return ret == ERROR_SUCCESS && type == REG_SZ;
}

bool setAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG ret;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\""; // 引号包裹，防止含空格路径解析失败
        cmd += path;
        cmd += L"\"";
        ret = RegSetValueExW(key, kAutoStartValueName, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(cmd.c_str()),
                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        ret = RegDeleteValueW(key, kAutoStartValueName);
    }
    RegCloseKey(key);
    return ret == ERROR_SUCCESS;
}

const wchar_t* spectrumProcessName(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return L"QQMusic.exe";
    case SmtcPlayerType::NetEase:
        return L"cloudmusic.exe";
    default:
        return L"";
    }
}

std::vector<std::wstring> volumeProcessNames(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return {L"QQMusic.exe"};
    case SmtcPlayerType::NetEase:
        return {L"cloudmusic.exe", L"NeteaseBridge.exe"};
    default:
        return {};
    }
}

fluent::NativeMenuIcon ownMenuIcon(HICON icon) {
    return fluent::NativeMenuIcon(icon, [](std::remove_pointer_t<HICON>* value) {
        if (value)
            DestroyIcon(value);
    });
}

fluent::NativeMenuIcon shellIconForPath(const std::wstring& path) {
    if (path.empty())
        return {};
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) == 0 ||
        !info.hIcon)
        return {};
    return ownMenuIcon(info.hIcon);
}
} // namespace lyrics_app
