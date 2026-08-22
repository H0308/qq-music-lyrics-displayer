#include "platform_icon.h"

#include <objbase.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstring>
#include <memory>

namespace {

class GdiplusInit {
public:
    GdiplusInit() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartupOutput output;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, &output);
        token_ = token;
    }

    ~GdiplusInit() {
        if (token_)
            Gdiplus::GdiplusShutdown(token_);
    }

private:
    ULONG_PTR token_ = 0;
};

GdiplusInit g_gdiplusInit;

std::wstring findProcessImagePath(const std::wstring& processName) {
    if (processName.empty())
        return {};

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) != 0)
                continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
            if (QueryFullProcessImageNameW(process, 0, path, &length))
                result.assign(path, length);
            CloseHandle(process);
            if (!result.empty())
                break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::wstring resolveSourceIconPath(const std::wstring& sourceAppUserModelId) {
    if (sourceAppUserModelId.empty())
        return {};

    std::vector<std::wstring> candidates;
    if (_wcsicmp(sourceAppUserModelId.c_str(), L"NeteaseBridge.exe") == 0) {
        candidates.emplace_back(L"cloudmusic.exe");
        candidates.emplace_back(L"NeteaseBridge.exe");
    } else {
        candidates.push_back(sourceAppUserModelId);
    }
    for (const auto& candidate : candidates) {
        const std::wstring path = findProcessImagePath(candidate);
        if (!path.empty())
            return path;
    }

    DWORD attributes = GetFileAttributesW(sourceAppUserModelId.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return sourceAppUserModelId;
    return {};
}

void clearIconBlackMatte(std::vector<BYTE>& pixels, UINT width, UINT height) {
    if (pixels.empty() || width == 0 || height == 0)
        return;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<BYTE> cleared(pixelCount, 0);
    std::vector<size_t> pending;
    pending.reserve(pixelCount);

    auto nearBlack = [&](size_t index) {
        const BYTE* p = pixels.data() + index * 4;
        return p[3] != 0 && p[0] <= 48 && p[1] <= 48 && p[2] <= 48;
    };
    auto enqueue = [&](size_t index) {
        if (!cleared[index] && nearBlack(index)) {
            cleared[index] = 1;
            pending.push_back(index);
        }
    };

    for (UINT x = 0; x < width; ++x) {
        enqueue(x);
        enqueue(static_cast<size_t>(height - 1) * width + x);
    }
    for (UINT y = 0; y < height; ++y) {
        enqueue(static_cast<size_t>(y) * width);
        enqueue(static_cast<size_t>(y) * width + width - 1);
    }

    while (!pending.empty()) {
        const size_t index = pending.back();
        pending.pop_back();
        const UINT x = static_cast<UINT>(index % width);
        const UINT y = static_cast<UINT>(index / width);
        if (x > 0)
            enqueue(index - 1);
        if (x + 1 < width)
            enqueue(index + 1);
        if (y > 0)
            enqueue(index - width);
        if (y + 1 < height)
            enqueue(index + width);
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        BYTE* p = pixels.data() + i * 4;
        if (cleared[i] || p[3] == 0)
            p[0] = p[1] = p[2] = p[3] = 0;
    }
}

bool readIconPixels(HICON icon, std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    if (!icon)
        return false;

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromHICON(icon));
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        return false;

    width = bitmap->GetWidth();
    height = bitmap->GetHeight();
    if (width == 0 || height == 0)
        return false;

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok)
        return false;

    pixels.resize(static_cast<size_t>(width) * height * 4);
    const BYTE* base = static_cast<const BYTE*>(data.Scan0);
    const LONG stride = data.Stride;
    const LONG rowStride = stride >= 0 ? stride : -stride;
    for (UINT y = 0; y < height; ++y) {
        const UINT sourceRow = stride >= 0 ? y : height - 1 - y;
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    base + static_cast<size_t>(sourceRow) * rowStride,
                    static_cast<size_t>(width) * 4);
    }
    bitmap->UnlockBits(&data);
    clearIconBlackMatte(pixels, width, height);
    return true;
}

} // namespace

namespace platform_icon {

bool readSourceIconPixels(const std::wstring& sourceAppUserModelId,
                          std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    const std::wstring path = resolveSourceIconPath(sourceAppUserModelId);
    if (path.empty())
        return false;

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    ExtractIconExW(path.c_str(), 0, &largeIcon, &smallIcon, 1);
    HICON icon = largeIcon ? largeIcon : smallIcon;
    HICON shellIcon = nullptr;
    if (!icon) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON))
            shellIcon = info.hIcon;
        icon = shellIcon;
    }

    const bool ok = readIconPixels(icon, pixels, width, height);
    if (largeIcon)
        DestroyIcon(largeIcon);
    if (smallIcon)
        DestroyIcon(smallIcon);
    if (shellIcon)
        DestroyIcon(shellIcon);
    return ok;
}

} // namespace platform_icon
