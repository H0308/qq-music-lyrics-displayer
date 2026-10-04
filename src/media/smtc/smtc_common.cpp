#include "media/smtc/smtc_common.h"

#include "logging/runtime_logger.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <chrono>
#include <mutex>

#include <winrt/Windows.Storage.Streams.h>

namespace smtc {

namespace {

bool isDecodableImage(const std::vector<uint8_t>& data, const wchar_t*& stage,
                      HRESULT& result, UINT& width, UINT& height, Gdiplus::Status& status) {
    if (data.empty()) {
        stage = L"image-empty";
        result = E_INVALIDARG;
        return false;
    }

    struct GdiPlusSession {
        ULONG_PTR token = 0;
        ~GdiPlusSession() {
            if (token)
                Gdiplus::GdiplusShutdown(token);
        }
    } session;
    stage = L"gdiplus-startup";
    Gdiplus::GdiplusStartupInput input;
    status = Gdiplus::GdiplusStartup(&session.token, &input, nullptr);
    if (status != Gdiplus::Ok) {
        result = E_FAIL;
        return false;
    }

    winrt::com_ptr<IStream> stream;
    stage = L"image-stream";
    result = CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
    if (FAILED(result))
        return false;
    stage = L"image-stream-write";
    ULONG written = 0;
    result = stream->Write(data.data(), static_cast<ULONG>(data.size()), &written);
    if (FAILED(result))
        return false;
    if (written != data.size()) {
        result = STG_E_WRITEFAULT;
        return false;
    }
    stage = L"image-stream-seek";
    LARGE_INTEGER offset{};
    result = stream->Seek(offset, STREAM_SEEK_SET, nullptr);
    if (FAILED(result))
        return false;

    // 与封面展示使用同一解码器，且输入流必须比 Bitmap 活得更久。
    stage = L"gdiplus-decoder";
    Gdiplus::Bitmap bitmap(stream.get());
    status = bitmap.GetLastStatus();
    if (status != Gdiplus::Ok) {
        result = E_FAIL;
        return false;
    }
    stage = L"gdiplus-size";
    width = bitmap.GetWidth();
    status = bitmap.GetLastStatus();
    if (status != Gdiplus::Ok) {
        result = E_FAIL;
        return false;
    }
    height = bitmap.GetHeight();
    status = bitmap.GetLastStatus();
    result = status == Gdiplus::Ok && width > 0 && height > 0 ? S_OK : E_FAIL;
    return SUCCEEDED(result);
}

} // namespace

int64_t nowUtcMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t timeSpanMs(winrt::Windows::Foundation::TimeSpan value) {
    return value.count() / 10000;
}

int64_t lastUpdatedMs(winrt::Windows::Foundation::DateTime value) {
    // DateTime 使用 1601 年起的 100ns 单位，换算为 Unix 毫秒。
    constexpr int64_t kFileTimeEpochOffsetMs = 11644473600000LL;
    int64_t ms = value.time_since_epoch().count() / 10000 - kFileTimeEpochOffsetMs;
    return ms > 0 ? ms : 0;
}

bool sameSession(const Session& left, const Session& right) noexcept {
    if (winrt::get_abi(left) == winrt::get_abi(right))
        return true;
    if (!left || !right)
        return false;
    try {
        // GetCurrentSession() 和 GetSessions() 可能返回同一逻辑会话的不同
        // WinRT 包装对象，比较 IUnknown 身份可以兼容这种情况；不能只比较
        // SourceAppUserModelId，否则播放器重启后的新旧会话会被误认为同一会话。
        return winrt::get_unknown(left) == winrt::get_unknown(right);
    } catch (...) {
        return false;
    }
}

PlaybackStatus mapStatus(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status) {
    using Status = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    switch (status) {
    case Status::Playing:
        return PlaybackStatus::Playing;
    case Status::Paused:
        return PlaybackStatus::Paused;
    case Status::Stopped:
        return PlaybackStatus::Stopped;
    default:
        return PlaybackStatus::Other;
    }
}

std::shared_ptr<const std::vector<uint8_t>> readThumbnail(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& props,
    uint64_t readSequence) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    };
    const wchar_t* stage = L"thumbnail-reference";
    uint64_t size = 0;
    uint32_t loaded = 0;
    const auto logFailure = [&](const wchar_t* reason, HRESULT result = S_OK) {
        runtime_log::writef(
            L"[cover][smtc] read-failed sequence=%llu stage=%s reason=\"%s\" "
            L"bytes=%llu loaded=%u hresult=0x%08X elapsed=%lldms",
            static_cast<unsigned long long>(readSequence), stage, reason,
            static_cast<unsigned long long>(size), loaded,
            static_cast<unsigned int>(result), elapsedMs());
    };
    try {
        auto ref = props.Thumbnail();
        if (!ref) {
            logFailure(L"Thumbnail reference is null");
            return nullptr;
        }
        stage = L"open-stream";
        auto streamOp = ref.OpenReadAsync();
        if (!streamOp) {
            logFailure(L"OpenReadAsync returned null");
            return nullptr;
        }
        stage = L"open-wait";
        const auto openStatus = streamOp.wait_for(std::chrono::seconds(5));
        if (openStatus != winrt::Windows::Foundation::AsyncStatus::Completed) {
            runtime_log::writef(L"[cover][smtc] async-incomplete sequence=%llu stage=%s status=%d",
                               static_cast<unsigned long long>(readSequence), stage,
                               static_cast<int>(openStatus));
            logFailure(openStatus == winrt::Windows::Foundation::AsyncStatus::Started
                           ? L"OpenReadAsync timed out after 5000ms" : L"OpenReadAsync did not complete",
                       openStatus == winrt::Windows::Foundation::AsyncStatus::Error
                           ? static_cast<HRESULT>(streamOp.ErrorCode().value) : S_OK);
            streamOp.Cancel();
            return nullptr;
        }
        stage = L"open-result";
        auto stream = streamOp.GetResults();
        if (!stream) {
            logFailure(L"Thumbnail stream is null");
            return nullptr;
        }
        stage = L"stream-size";
        size = stream.Size();
        try {
            const auto contentType = stream.ContentType();
            const auto position = stream.Position();
            runtime_log::writef(
                L"[cover][smtc] stream-opened sequence=%llu bytes=%llu position=%llu "
                L"content-type=\"%s\" elapsed=%lldms",
                static_cast<unsigned long long>(readSequence), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(position), contentType.c_str(), elapsedMs());
        } catch (const winrt::hresult_error& error) {
            runtime_log::writef(L"[cover][smtc] stream-diagnostics-failed sequence=%llu hresult=0x%08X",
                               static_cast<unsigned long long>(readSequence),
                               static_cast<unsigned int>(error.code().value));
        } catch (...) {
            runtime_log::writef(L"[cover][smtc] stream-diagnostics-failed sequence=%llu reason=unknown",
                               static_cast<unsigned long long>(readSequence));
        }
        if (size == 0 || size > 4 * 1024 * 1024) {
            logFailure(size == 0 ? L"Thumbnail stream is empty" : L"Thumbnail exceeds 4MiB limit");
            return nullptr;
        }
        stage = L"data-reader";
        auto buffer = std::make_shared<std::vector<uint8_t>>((size_t)size);
        winrt::Windows::Storage::Streams::DataReader reader(stream);
        stage = L"load-bytes";
        auto loadOp = reader.LoadAsync((uint32_t)size);
        if (!loadOp) {
            logFailure(L"LoadAsync returned null");
            return nullptr;
        }
        stage = L"load-wait";
        const auto loadStatus = loadOp.wait_for(std::chrono::seconds(5));
        if (loadStatus != winrt::Windows::Foundation::AsyncStatus::Completed) {
            runtime_log::writef(L"[cover][smtc] async-incomplete sequence=%llu stage=%s status=%d",
                               static_cast<unsigned long long>(readSequence), stage,
                               static_cast<int>(loadStatus));
            logFailure(loadStatus == winrt::Windows::Foundation::AsyncStatus::Started
                           ? L"LoadAsync timed out after 5000ms" : L"LoadAsync did not complete",
                       loadStatus == winrt::Windows::Foundation::AsyncStatus::Error
                           ? static_cast<HRESULT>(loadOp.ErrorCode().value) : S_OK);
            loadOp.Cancel();
            return nullptr;
        }
        stage = L"load-result";
        loaded = loadOp.GetResults();
        if (loaded != size) {
            logFailure(L"Thumbnail stream was only partially loaded");
            return nullptr;
        }
        stage = L"read-bytes";
        reader.ReadBytes(winrt::array_view<uint8_t>(buffer->data(), (uint32_t)buffer->size()));
        std::wstring signature;
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (size_t i = 0; i < buffer->size() && i < 16; ++i) {
            if (i)
                signature += L' ';
            signature += hex[(*buffer)[i] >> 4];
            signature += hex[(*buffer)[i] & 0x0F];
        }
        runtime_log::writef(L"[cover][smtc] bytes-loaded sequence=%llu bytes=%u signature=\"%s\" elapsed=%lldms",
                           static_cast<unsigned long long>(readSequence), loaded,
                           signature.c_str(), elapsedMs());
        HRESULT decodeResult = S_OK;
        UINT width = 0;
        UINT height = 0;
        Gdiplus::Status decodeStatus = Gdiplus::Ok;
        if (!isDecodableImage(*buffer, stage, decodeResult, width, height, decodeStatus)) {
            runtime_log::writef(
                L"[cover][smtc] image-validation-failed sequence=%llu decoder=gdiplus stage=%s status=%d width=%u height=%u",
                static_cast<unsigned long long>(readSequence), stage,
                static_cast<int>(decodeStatus), width, height);
            logFailure(L"Image validation failed", decodeResult);
            return nullptr;
        }
        runtime_log::writef(L"[cover][smtc] read-succeeded sequence=%llu decoder=gdiplus bytes=%u width=%u height=%u elapsed=%lldms",
                           static_cast<unsigned long long>(readSequence), loaded, width, height, elapsedMs());
        return buffer;
    } catch (const winrt::hresult_error& error) {
        logFailure(error.message().c_str(), static_cast<HRESULT>(error.code().value));
        return nullptr;
    } catch (...) {
        logFailure(L"Unknown exception");
        return nullptr;
    }
}

} // namespace smtc
