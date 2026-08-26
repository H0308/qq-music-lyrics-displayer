#include "runtime_logger.h"

#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace runtime_log {

namespace {

constexpr wchar_t kLogPrefix[] = L"QQMusicLyric-";
constexpr wchar_t kLogExtension[] = L".log";
constexpr size_t kBufferedBytesLimit = 64 * 1024;
constexpr auto kFlushInterval = std::chrono::milliseconds(1000);
constexpr auto kSampleInterval = std::chrono::milliseconds(1000);
constexpr auto kResourceLogInterval = std::chrono::seconds(5);
constexpr auto kCleanupInterval = std::chrono::hours(1);
constexpr uint64_t kResourceMemoryDeltaBytes = 1024ull * 1024ull;

std::atomic<RuntimeLogger*> g_activeLogger{nullptr};

std::string utf8Of(const std::wstring& value) {
    if (value.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

std::wstring nowStamp(bool includeMilliseconds) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    if (includeMilliseconds) {
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth,
                   st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    } else {
        swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u-%03u", st.wYear, st.wMonth,
                   st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    }
    return buffer;
}

std::wstring formatMessage(const wchar_t* format, va_list args) {
    if (!format)
        return {};

    va_list lengthArgs;
    va_copy(lengthArgs, args);
    const int length = _vscwprintf(format, lengthArgs);
    va_end(lengthArgs);
    if (length <= 0)
        return length == 0 ? std::wstring() : std::wstring(L"[logger] format failed");

    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    va_list valueArgs;
    va_copy(valueArgs, args);
    _vsnwprintf_s(result.data(), result.size(), _TRUNCATE, format, valueArgs);
    va_end(valueArgs);
    result.resize(static_cast<size_t>(length));
    return result;
}

uint64_t fileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::wstring processIdText() {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%lu", GetCurrentProcessId());
    return buffer;
}

struct CpuSample {
    uint64_t process = 0;
    uint64_t system = 0;
    bool valid = false;
};

CpuSample readCpuSample() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    FILETIME idle{}, systemKernel{}, systemUser{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) ||
        !GetSystemTimes(&idle, &systemKernel, &systemUser))
        return {};
    return {fileTimeValue(kernel) + fileTimeValue(user),
            fileTimeValue(systemKernel) + fileTimeValue(systemUser), true};
}

struct GpuSampler {
    PDH_HQUERY query = nullptr;
    std::vector<PDH_HCOUNTER> counters;
    bool firstSample = true;

    ~GpuSampler() {
        reset();
    }

    void reset() {
        if (query)
            PdhCloseQuery(query);
        query = nullptr;
        counters.clear();
        firstSample = true;
    }

    bool initialize() {
        reset();

        DWORD counterChars = 0;
        DWORD instanceChars = 0;
        const PDH_STATUS sizeStatus = PdhEnumObjectItemsW(
            nullptr, nullptr, L"GPU Engine", nullptr, &counterChars, nullptr, &instanceChars,
            PERF_DETAIL_WIZARD, 0);
        if (sizeStatus != PDH_MORE_DATA && sizeStatus != ERROR_SUCCESS)
            return false;
        if (instanceChars == 0)
            return false;

        std::vector<wchar_t> counterNames(static_cast<size_t>(counterChars) + 1);
        std::vector<wchar_t> instanceNames(static_cast<size_t>(instanceChars) + 1);
        if (PdhEnumObjectItemsW(nullptr, nullptr, L"GPU Engine", counterNames.data(),
                                &counterChars, instanceNames.data(), &instanceChars,
                                PERF_DETAIL_WIZARD, 0) != ERROR_SUCCESS)
            return false;

        const std::wstring pidToken = L"pid_" + processIdText();
        for (const wchar_t* instance = instanceNames.data(); instance && *instance;
             instance += wcslen(instance) + 1) {
            const std::wstring instanceName(instance);
            if (instanceName.rfind(pidToken + L"_", 0) != 0)
                continue;

            std::wstring path = L"\\GPU Engine(";
            path += instance;
            path += L")\\Utilization Percentage";
            if (!query && PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
                return false;
            PDH_HCOUNTER counter = nullptr;
            if (PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS &&
                counter)
                counters.push_back(counter);
        }
        return query != nullptr && !counters.empty();
    }

    double sample() {
        if (!query || counters.empty()) {
            if (!initialize())
                return -1.0;
        }

        const PDH_STATUS collectStatus = PdhCollectQueryData(query);
        if (collectStatus != ERROR_SUCCESS)
            return -1.0;
        if (firstSample) {
            firstSample = false;
            return -1.0;
        }

        double maximum = 0.0;
        bool valid = false;
        for (PDH_HCOUNTER counter : counters) {
            PDH_FMT_COUNTERVALUE value{};
            DWORD type = 0;
            if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &type, &value) !=
                    ERROR_SUCCESS ||
                (value.CStatus != PDH_CSTATUS_NEW_DATA &&
                 value.CStatus != PDH_CSTATUS_VALID_DATA))
                continue;
            maximum = std::max(maximum, value.doubleValue);
            valid = true;
        }
        if (!valid)
            return -1.0;
        return std::clamp(maximum, 0.0, 100.0);
    }
};

struct ResourceSample {
    double cpu = -1.0;
    double gpu = -1.0;
    uint64_t memory = 0;
    uint64_t commit = 0;
    DWORD handles = 0;
    DWORD threads = 0;
    bool processCountsValid = false;
};

class ResourceSampler {
public:
    ResourceSample sample() {
        ResourceSample result;
        const CpuSample cpu = readCpuSample();
        if (cpu.valid && previousCpu_.valid && cpu.system > previousCpu_.system) {
            const uint64_t processDelta = cpu.process - previousCpu_.process;
            const uint64_t systemDelta = cpu.system - previousCpu_.system;
            if (systemDelta > 0)
                result.cpu = std::clamp(static_cast<double>(processDelta) * 100.0 /
                                            static_cast<double>(systemDelta),
                                        0.0, 100.0);
        }
        previousCpu_ = cpu;
        result.gpu = gpu_.sample();

        PROCESS_MEMORY_COUNTERS_EX2 memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory)))
        {
            result.memory = static_cast<uint64_t>(memory.PrivateWorkingSetSize);
            result.commit = static_cast<uint64_t>(memory.PrivateUsage);
        }
        return result;
    }

    void addProcessCounts(ResourceSample& result) const {
        DWORD handleCount = 0;
        const bool handlesValid = GetProcessHandleCount(GetCurrentProcess(), &handleCount) != 0;

        DWORD threadCount = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        bool threadsValid = snapshot != INVALID_HANDLE_VALUE;
        if (threadsValid) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID == GetCurrentProcessId())
                        ++threadCount;
                } while (Thread32Next(snapshot, &entry));
            } else {
                threadsValid = false;
            }
            CloseHandle(snapshot);
        }

        result.handles = handleCount;
        result.threads = threadCount;
        result.processCountsValid = handlesValid && threadsValid;
    }

private:
    CpuSample previousCpu_;
    GpuSampler gpu_;
};

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pointers) {
    RuntimeLogger* logger = g_activeLogger.load(std::memory_order_acquire);
    if (logger) {
        const DWORD code = pointers && pointers->ExceptionRecord
                               ? pointers->ExceptionRecord->ExceptionCode
                               : 0;
        const void* address = pointers && pointers->ExceptionRecord
                                  ? pointers->ExceptionRecord->ExceptionAddress
                                  : nullptr;
        logger->writef(L"[crash] unhandled exception: code=0x%08lX address=%p", code,
                       address);
        logger->flushSync();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void terminateHandler() {
    RuntimeLogger* logger = g_activeLogger.load(std::memory_order_acquire);
    if (logger) {
        logger->write(L"[crash] std::terminate invoked");
        logger->flushSync();
    }
    std::abort();
}

} // namespace

struct RuntimeLogger::Impl {
    mutable std::recursive_mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::string> pending;
    size_t pendingBytes = 0;
    std::ofstream file;
    std::wstring filePath;
    std::wstring directory;
    std::wstring sessionFileName;
    int retentionDays = 30;
    bool started = false;
    bool stopping = false;
    std::thread worker;
    RuntimeLogSnapshot snapshot;

    void ensureDirectoryLocked() {
        if (directory.empty())
            directory = RuntimeLogger::defaultDirectory();
        if (directory.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(directory), ec);
    }

    void openFileLocked() {
        if (file.is_open() && file)
            return;
        if (file.is_open())
            file.close();
        file.clear();
        ensureDirectoryLocked();
        if (directory.empty())
            return;
        if (sessionFileName.empty())
            sessionFileName = std::wstring(kLogPrefix) + nowStamp(false) + kLogExtension;
        const auto path = std::filesystem::path(directory) /
                          std::filesystem::path(sessionFileName);
        filePath = path.wstring();
        file.open(path, std::ios::binary | std::ios::app);
        if (!file)
            OutputDebugStringW(L"[logger] failed to open runtime log file\n");
    }

    void enqueueUtf8Locked(const std::string& line) {
        if (line.empty())
            return;
        pendingBytes += line.size();
        pending.push_back(line);
        wake.notify_one();
    }

    void writePendingLocked() {
        if (pending.empty())
            return;
        openFileLocked();
        if (!file)
            return;
        while (!pending.empty()) {
            const std::string& line = pending.front();
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            if (!file)
                break;
            pendingBytes -= std::min(pendingBytes, line.size());
            pending.pop_front();
        }
    }

    void flushLocked() {
        writePendingLocked();
        if (file)
            file.flush();
    }

    void cleanupLogs() {
        std::wstring directoryCopy;
        std::wstring currentFileCopy;
        int days = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            directoryCopy = directory;
            currentFileCopy = sessionFileName;
            days = retentionDays;
        }
        if (directoryCopy.empty() || days <= 0)
            return;

        const auto now = std::filesystem::file_time_type::clock::now();
        const auto cutoff = now - std::chrono::hours(24 * days);
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::path(directoryCopy), ec), end;
             it != end && !ec; it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_regular_file(ec) || ec)
                continue;
            const std::wstring name = entry.path().filename().wstring();
            if (name == currentFileCopy || name.rfind(kLogPrefix, 0) != 0 ||
                entry.path().extension().wstring() != kLogExtension)
                continue;
            const auto modified = entry.last_write_time(ec);
            if (ec || modified >= cutoff)
                continue;
            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
        }
    }

    void run() {
        ResourceSampler resources;
        auto nextSample = std::chrono::steady_clock::now();
        auto nextResourceLog = nextSample;
        auto nextCleanup = std::chrono::steady_clock::now();
        auto nextFlush = std::chrono::steady_clock::now() + kFlushInterval;
        bool hasResourceLog = false;
        uint64_t lastLoggedMemory = 0;

        for (;;) {
            std::unique_lock<std::recursive_mutex> lock(mutex);
            if (!stopping && pending.empty()) {
                wake.wait_for(lock, std::chrono::milliseconds(250), [this] {
                    return stopping || !pending.empty();
                });
            } else if (!stopping && pendingBytes < kBufferedBytesLimit) {
                wake.wait_until(lock, nextFlush, [this] {
                    return stopping || pendingBytes >= kBufferedBytesLimit;
                });
            }
            const auto now = std::chrono::steady_clock::now();
            const bool shouldStop = stopping;
            const bool shouldFlush = !pending.empty() &&
                                      (pendingBytes >= kBufferedBytesLimit || now >= nextFlush);
            if (shouldFlush || shouldStop) {
                flushLocked();
                nextFlush = now + kFlushInterval;
            }
            lock.unlock();

            if (now >= nextSample) {
                ResourceSample sample = resources.sample();
                {
                    std::lock_guard<std::recursive_mutex> stateLock(mutex);
                    snapshot.cpuPercent = sample.cpu;
                    snapshot.gpuPercent = sample.gpu;
                    snapshot.memoryBytes = sample.memory;
                }

                const uint64_t memoryDelta =
                    sample.memory >= lastLoggedMemory ? sample.memory - lastLoggedMemory
                                                       : lastLoggedMemory - sample.memory;
                const bool intervalDue = now >= nextResourceLog;
                const bool memoryChanged = hasResourceLog && sample.memory > 0 &&
                                            lastLoggedMemory > 0 &&
                                            memoryDelta >= kResourceMemoryDeltaBytes;
                if (!hasResourceLog || intervalDue || memoryChanged) {
                    resources.addProcessCounts(sample);
                    const double privateMb =
                        static_cast<double>(sample.memory) / (1024.0 * 1024.0);
                    const double commitMb =
                        static_cast<double>(sample.commit) / (1024.0 * 1024.0);
                    const double deltaMb =
                        hasResourceLog
                            ? static_cast<double>(static_cast<int64_t>(sample.memory) -
                                                  static_cast<int64_t>(lastLoggedMemory)) /
                                  (1024.0 * 1024.0)
                            : 0.0;
                    const wchar_t* reason =
                        !hasResourceLog ? L"initial" : intervalDue ? L"interval" : L"memory-change";
                    runtime_log::writef(
                        L"[resource][snapshot] reason=%s private=%.1fMB delta=%+.1fMB "
                        L"commit=%.1fMB handles=%lu threads=%lu detail=%s",
                        reason, privateMb, deltaMb, commitMb,
                        static_cast<unsigned long>(sample.handles),
                        static_cast<unsigned long>(sample.threads),
                        sample.processCountsValid ? L"ok" : L"partial");
                    hasResourceLog = true;
                    if (sample.memory > 0)
                        lastLoggedMemory = sample.memory;
                    nextResourceLog = now + kResourceLogInterval;
                }
                nextSample = now + kSampleInterval;
            }
            if (now >= nextCleanup) {
                cleanupLogs();
                nextCleanup = now + kCleanupInterval;
            }
            if (shouldStop)
                break;
        }
    }
};

RuntimeLogger::RuntimeLogger() : impl_(std::make_unique<Impl>()) {}

RuntimeLogger::~RuntimeLogger() {
    stop();
}

void RuntimeLogger::start(const std::wstring& directory, int retentionDays) {
    if (!impl_)
        return;
    stop();

    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        impl_->directory = directory.empty() ? defaultDirectory() : directory;
        impl_->retentionDays = std::clamp(retentionDays, 0, 3650);
        impl_->sessionFileName.clear();
        impl_->stopping = false;
        impl_->started = true;
        impl_->snapshot.logDirectory = impl_->directory;
        impl_->snapshot.sessionFileName.clear();
        impl_->snapshot.retentionDays = impl_->retentionDays;
        impl_->openFileLocked();
        if (!impl_->sessionFileName.empty())
            impl_->snapshot.sessionFileName = impl_->sessionFileName;
        g_activeLogger.store(this, std::memory_order_release);
    }

    std::set_terminate(terminateHandler);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    impl_->worker = std::thread([this] { impl_->run(); });
    writef(L"[lifecycle] runtime logger started: file=%s", impl_->sessionFileName.c_str());
}

void RuntimeLogger::stop() {
    if (!impl_)
        return;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        if (!impl_->started && !impl_->worker.joinable())
            return;
        impl_->stopping = true;
        impl_->wake.notify_all();
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        impl_->flushLocked();
        if (impl_->file.is_open())
            impl_->file.close();
        impl_->filePath.clear();
        impl_->started = false;
        impl_->stopping = false;
    }
    RuntimeLogger* expected = this;
    g_activeLogger.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void RuntimeLogger::write(const std::wstring& message) {
    try {
        if (!impl_ || message.empty())
            return;

        std::wstring normalized = message;
        while (!normalized.empty() &&
               (normalized.back() == L'\r' || normalized.back() == L'\n'))
            normalized.pop_back();
        if (normalized.empty())
            return;

        const std::wstring line = L"[" + nowStamp(true) + L"] " + normalized + L"\r\n";
        const std::string utf8 = utf8Of(line);
        if (utf8.empty())
            return;

        // main 在启动阶段把 stdout 设为 _O_U8TEXT；使用宽字符输出可保留 Debug
        // 控制台的中文显示，磁盘仍使用下面的 UTF-8 字节队列。
        std::fputws(line.c_str(), stdout);
        OutputDebugStringW(line.c_str());
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        if (!impl_->started) {
            return;
        }
        impl_->enqueueUtf8Locked(utf8);
    } catch (...) {
        OutputDebugStringW(L"[logger] write failed\n");
    }
}

void RuntimeLogger::writef(const wchar_t* format, ...) {
    try {
        va_list args;
        va_start(args, format);
        const std::wstring message = formatMessage(format, args);
        va_end(args);
        write(message);
    } catch (...) {
        OutputDebugStringW(L"[logger] format failed\n");
    }
}

void RuntimeLogger::flushSync() {
    if (!impl_)
        return;
    std::wstring filePath;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        impl_->flushLocked();
        filePath = impl_->filePath;
    }
    if (!filePath.empty()) {
        HANDLE handle = CreateFileW(filePath.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(handle);
            CloseHandle(handle);
        }
    }
}

void RuntimeLogger::setDirectory(const std::wstring& directory) {
    if (!impl_ || directory.empty())
        return;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        if (impl_->directory == directory)
            return;
        impl_->flushLocked();
        if (impl_->file.is_open())
            impl_->file.close();
        impl_->directory = directory;
        impl_->openFileLocked();
        impl_->snapshot.logDirectory = impl_->directory;
        impl_->snapshot.sessionFileName = impl_->sessionFileName;
    }
    writef(L"[logger] log directory changed: %s", directory.c_str());
}

void RuntimeLogger::setRetentionDays(int days) {
    if (!impl_)
        return;
    days = std::clamp(days, 0, 3650);
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
        impl_->retentionDays = days;
        impl_->snapshot.retentionDays = days;
    }
    impl_->cleanupLogs();
    writef(L"[logger] retention changed: %d days", days);
}

std::wstring RuntimeLogger::directory() const {
    if (!impl_)
        return {};
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->directory;
}

int RuntimeLogger::retentionDays() const {
    if (!impl_)
        return 0;
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->retentionDays;
}

void RuntimeLogger::openDirectory() const {
    const std::wstring path = directory();
    if (!path.empty())
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RuntimeLogger::setPlayback(const std::wstring& title, const std::wstring& artist,
                                int64_t durationMs, bool active) {
    if (!impl_)
        return;
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->snapshot.playbackActive = active;
    impl_->snapshot.currentTitle = active ? title : L"";
    impl_->snapshot.currentArtist = active ? artist : L"";
    impl_->snapshot.durationMs = active ? std::max<int64_t>(0, durationMs) : 0;
    if (!active) {
        impl_->snapshot.lyricSource = L"未加载";
        impl_->snapshot.coverLoaded = false;
        impl_->snapshot.coverImage.reset();
    }
}

void RuntimeLogger::setLyricSource(const std::wstring& source) {
    if (!impl_)
        return;
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->snapshot.lyricSource = source;
}

void RuntimeLogger::setCoverImage(
    const std::shared_ptr<const std::vector<uint8_t>>& cover) {
    if (!impl_)
        return;
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->snapshot.coverImage = cover;
    impl_->snapshot.coverLoaded = cover && !cover->empty();
}

void RuntimeLogger::setCoverLoaded(bool loaded) {
    if (!impl_)
        return;
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->snapshot.coverLoaded = loaded;
    if (!loaded)
        impl_->snapshot.coverImage.reset();
}

RuntimeLogSnapshot RuntimeLogger::snapshot() const {
    if (!impl_)
        return {};
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    RuntimeLogSnapshot result = impl_->snapshot;
    result.logDirectory = impl_->directory;
    result.sessionFileName = impl_->sessionFileName;
    result.retentionDays = impl_->retentionDays;
    return result;
}

std::wstring RuntimeLogger::defaultDirectory() {
    DWORD capacity = MAX_PATH;
    for (;;) {
        std::vector<wchar_t> buffer(capacity);
        const DWORD length = GetTempPathW(capacity, buffer.data());
        if (length == 0)
            return {};
        if (length < capacity) {
            std::wstring result(buffer.data(), length);
            while (!result.empty() && (result.back() == L'\\' || result.back() == L'/'))
                result.pop_back();
            if (result.empty())
                return {};
            return result + L"\\QQMusicLyric\\logs";
        }
        if (length >= 32768)
            return {};
        capacity = length + 1;
    }
}

std::wstring RuntimeLogger::legacyDefaultDirectory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) || !raw)
        return {};
    std::wstring result = raw;
    CoTaskMemFree(raw);
    result += L"\\QQMusicLyric\\logs";
    return result;
}

void writef(const wchar_t* format, ...) {
    try {
        va_list args;
        va_start(args, format);
        const std::wstring message = formatMessage(format, args);
        va_end(args);
        if (RuntimeLogger* logger = g_activeLogger.load(std::memory_order_acquire)) {
            logger->write(message);
        } else {
            OutputDebugStringW(message.c_str());
            OutputDebugStringW(L"\n");
        }
    } catch (...) {
        OutputDebugStringW(L"[logger] format failed\n");
    }
}

} // namespace runtime_log
