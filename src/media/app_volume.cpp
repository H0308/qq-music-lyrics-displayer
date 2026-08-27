#include "app_volume.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// 判断进程 ID 对应的进程镜像文件名是否与任一目标可执行文件名一致（不区分大小写）。
bool isTargetProcess(DWORD pid, const std::vector<std::wstring>& processNames) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;

    wchar_t path[MAX_PATH]{};
    DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const bool match = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!match)
        return false;

    const wchar_t* name = wcsrchr(path, L'\\');
    const wchar_t* baseName = name ? name + 1 : path;
    for (const auto& target : processNames) {
        if (_wcsicmp(baseName, target.c_str()) == 0)
            return true;
    }
    return false;
}

// 音频会话事件：音量/静音变化和会话过期都通知上层刷新。
class SessionEventsSink : public IAudioSessionEvents {
public:
    explicit SessionEventsSink(std::function<void()> onChanged)
        : onChanged_(std::move(onChanged)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
            *out = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ref_.fetch_add(1) + 1; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = ref_.fetch_sub(1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override {
        return S_OK;
    }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState state) override {
        if (state == AudioSessionStateExpired && onChanged_)
            onChanged_();
        return S_OK;
    }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
        if (onChanged_)
            onChanged_();
        return S_OK;
    }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override {
        if (onChanged_)
            onChanged_();
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::function<void()> onChanged_;
};

// 新会话创建通知：播放器启动/重启后第一次发声时出现，用于从“不可用”恢复。
class SessionNotificationSink : public IAudioSessionNotification {
public:
    using CreatedCallback = std::function<void(IAudioSessionControl*)>;

    explicit SessionNotificationSink(CreatedCallback onCreated)
        : onCreated_(std::move(onCreated)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
            *out = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ref_.fetch_add(1) + 1; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = ref_.fetch_sub(1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP OnSessionCreated(IAudioSessionControl* session) override {
        if (onCreated_) {
            try {
                onCreated_(session);
            } catch (...) {
            }
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    CreatedCallback onCreated_;
};

// 默认输出设备变化通知。回调线程只标记需要重绑并通知 UI，实际 COM 对象重建仍在
// UI 线程完成，避免与 query/applyToAll 并发访问会话列表。
class EndpointNotificationSink : public IMMNotificationClient {
public:
    explicit EndpointNotificationSink(std::function<void()> onChanged)
        : onChanged_(std::move(onChanged)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ref_.fetch_add(1) + 1; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = ref_.fetch_sub(1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override {
        notify();
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override {
        notify();
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override {
        notify();
        return S_OK;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole)
            notify();
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        notify();
        return S_OK;
    }

private:
    void notify() {
        if (onChanged_)
            onChanged_();
    }

    std::atomic<ULONG> ref_{1};
    std::function<void()> onChanged_;
};

} // namespace

struct AppVolumeController::Impl {
    struct SessionEntry {
        IAudioSessionControl* control = nullptr; // 持有引用，注销事件时要用
        ISimpleAudioVolume* volume = nullptr;
        SessionEventsSink* sink = nullptr;       // 我们持有 1 个引用
        std::wstring instanceId;
    };

    struct PendingSession {
        IAudioSessionControl* control = nullptr; // 我们持有 1 个引用
        uint64_t generation = 0;
    };

    std::vector<std::wstring> processNames;
    ChangedCallback onChanged;

    IMMDeviceEnumerator* devices = nullptr;
    IAudioSessionManager2* manager = nullptr;
    SessionNotificationSink* sessionSink = nullptr; // 我们持有 1 个引用
    bool sessionNotifyRegistered = false;
    EndpointNotificationSink* endpointSink = nullptr; // 我们持有 1 个引用
    bool endpointNotifyRegistered = false;
    std::wstring endpointId;
    std::atomic_bool endpointChanged{false};
    std::atomic<uint64_t> endpointGeneration{1};
    std::mutex pendingMutex;
    std::vector<PendingSession> pendingSessions;
    std::vector<SessionEntry> sessions;

    ~Impl() {
        endpointGeneration.fetch_add(1, std::memory_order_acq_rel);
        clearPendingSessions();
        clearSessions();
        unregisterSessionNotification();
        unregisterEndpointNotification();
        if (manager)
            manager->Release();
        if (devices)
            devices->Release();
    }

    void notifyEndpointChanged() {
        endpointChanged.store(true, std::memory_order_release);
        if (onChanged)
            onChanged();
    }

    void queueCreatedSession(IAudioSessionControl* control, uint64_t generation) {
        if (!control || generation != endpointGeneration.load(std::memory_order_acquire))
            return;
        control->AddRef();
        try {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (generation != endpointGeneration.load(std::memory_order_acquire)) {
                control->Release();
                return;
            }
            pendingSessions.push_back({control, generation});
        } catch (...) {
            control->Release();
            return;
        }
        if (onChanged)
            onChanged();
    }

    void clearPendingSessions() {
        std::vector<PendingSession> pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pending.swap(pendingSessions);
        }
        for (auto& entry : pending) {
            if (entry.control)
                entry.control->Release();
        }
    }

    bool hasPendingSessions() {
        std::lock_guard<std::mutex> lock(pendingMutex);
        return !pendingSessions.empty();
    }

    std::vector<PendingSession> takePendingSessions() {
        std::vector<PendingSession> pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pending.swap(pendingSessions);
        }
        return pending;
    }

    void resetManager() {
        endpointGeneration.fetch_add(1, std::memory_order_acq_rel);
        clearPendingSessions();
        clearSessions();
        unregisterSessionNotification();
        if (manager) {
            manager->Release();
            manager = nullptr;
        }
        endpointId.clear();
    }

    void registerEndpointNotification() {
        if (endpointNotifyRegistered || !devices || !onChanged)
            return;
        if (!endpointSink)
            endpointSink = new EndpointNotificationSink([this] { notifyEndpointChanged(); });
        endpointNotifyRegistered =
            SUCCEEDED(devices->RegisterEndpointNotificationCallback(endpointSink));
    }

    void unregisterEndpointNotification() {
        if (endpointNotifyRegistered && devices && endpointSink)
            devices->UnregisterEndpointNotificationCallback(endpointSink);
        endpointNotifyRegistered = false;
        if (endpointSink) {
            endpointSink->Release();
            endpointSink = nullptr;
        }
    }

    bool ensureManager() {
        if (!devices && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                                reinterpret_cast<void**>(&devices))))
            return false;
        registerEndpointNotification();

        IMMDevice* device = nullptr;
        HRESULT hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) {
            if (manager)
                resetManager();
            return false;
        }

        LPWSTR rawId = nullptr;
        std::wstring currentId;
        if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
            currentId = rawId;
            CoTaskMemFree(rawId);
        }
        const bool changed = endpointChanged.exchange(false, std::memory_order_acq_rel);
        const bool sameEndpoint = currentId.empty() ? endpointId.empty()
                                                     : currentId == endpointId;
        if (manager && !changed && sameEndpoint) {
            device->Release();
            return true;
        }
        if (manager)
            resetManager();

        hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&manager));
        device->Release();
        if (FAILED(hr) || !manager)
            return false;
        endpointId = std::move(currentId);
        registerSessionNotification();
        return true;
    }

    void registerSessionNotification() {
        if (sessionNotifyRegistered || !manager || !onChanged)
            return;
        if (!sessionSink)
            sessionSink = new SessionNotificationSink(
                [this, generation = endpointGeneration.load(std::memory_order_acquire)](
                    IAudioSessionControl* control) { queueCreatedSession(control, generation); });
        sessionNotifyRegistered =
            SUCCEEDED(manager->RegisterSessionNotification(sessionSink));
    }

    void unregisterSessionNotification() {
        if (sessionNotifyRegistered && manager)
            manager->UnregisterSessionNotification(sessionSink);
        sessionNotifyRegistered = false;
        if (sessionSink) {
            sessionSink->Release();
            sessionSink = nullptr;
        }
    }

    void clearSessions() {
        for (auto& entry : sessions) {
            if (entry.control && entry.sink)
                entry.control->UnregisterAudioSessionNotification(entry.sink);
            if (entry.sink)
                entry.sink->Release();
            if (entry.volume)
                entry.volume->Release();
            if (entry.control)
                entry.control->Release();
        }
        sessions.clear();
    }

    bool hasSessionInstance(const std::wstring& instanceId) const {
        if (instanceId.empty())
            return false;
        return std::any_of(sessions.begin(), sessions.end(), [&](const SessionEntry& entry) {
            return entry.instanceId == instanceId;
        });
    }

    // 接管一个由枚举器或 OnSessionCreated 转交的会话引用；不匹配或重复时释放它。
    void addSession(IAudioSessionControl* control) {
        if (!control)
            return;

        AudioSessionState state = AudioSessionStateExpired;
        IAudioSessionControl2* control2 = nullptr;
        DWORD pid = 0;
        ISimpleAudioVolume* volume = nullptr;
        std::wstring instanceId;
        LPWSTR rawInstanceId = nullptr;
        const bool matched =
            SUCCEEDED(control->GetState(&state)) && state != AudioSessionStateExpired &&
            SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                              reinterpret_cast<void**>(&control2))) &&
            control2 && SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0 &&
            isTargetProcess(pid, processNames) &&
            SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                              reinterpret_cast<void**>(&volume))) &&
            volume;

        if (matched && control2 &&
            SUCCEEDED(control2->GetSessionInstanceIdentifier(&rawInstanceId)) &&
            rawInstanceId) {
            instanceId = rawInstanceId;
            CoTaskMemFree(rawInstanceId);
            rawInstanceId = nullptr;
        }

        if (matched && !hasSessionInstance(instanceId)) {
            SessionEntry entry;
            entry.control = control;
            entry.volume = volume;
            entry.instanceId = std::move(instanceId);
            if (onChanged) {
                entry.sink = new SessionEventsSink(onChanged);
                if (FAILED(control->RegisterAudioSessionNotification(entry.sink))) {
                    entry.sink->Release();
                    entry.sink = nullptr;
                }
            }
            sessions.push_back(entry);
        } else {
            if (volume)
                volume->Release();
            control->Release();
        }
        if (control2)
            control2->Release();
    }

    // 重新枚举默认输出设备上与目标进程名匹配的未过期会话（可跨多个进程）。
    void refreshSessions() {
        clearSessions();
        if (processNames.empty()) {
            clearPendingSessions();
            return;
        }
        if (!ensureManager())
            return;

        IAudioSessionEnumerator* enumerator = nullptr;
        if (FAILED(manager->GetSessionEnumerator(&enumerator)) || !enumerator) {
            // 设备可能在端点通知到达前已经失效；主动丢弃旧管理器，下一次查询时重建。
            resetManager();
            if (!ensureManager() || FAILED(manager->GetSessionEnumerator(&enumerator)) ||
                !enumerator)
                return;
        }
        int count = 0;
        if (FAILED(enumerator->GetCount(&count))) {
            enumerator->Release();
            return;
        }
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            if (SUCCEEDED(enumerator->GetSession(i, &control)))
                addSession(control);
        }
        enumerator->Release();

        auto pending = takePendingSessions();
        const uint64_t generation = endpointGeneration.load(std::memory_order_acquire);
        for (auto& entry : pending) {
            if (entry.control && entry.generation == generation)
                addSession(entry.control);
            else if (entry.control)
                entry.control->Release();
            entry.control = nullptr;
        }
    }

    // 默认设备、待接管会话变化，或缓存中有会话已过期时重建。
    void ensureSessionsFresh() {
        bool stale = !ensureManager() || sessions.empty() || hasPendingSessions();
        for (auto& entry : sessions) {
            AudioSessionState state = AudioSessionStateExpired;
            if (!entry.control || FAILED(entry.control->GetState(&state)) ||
                state == AudioSessionStateExpired) {
                stale = true;
                break;
            }
        }
        if (stale)
            refreshSessions();
    }

    bool query(int& percent, bool& muted) {
        percent = 0;
        muted = false;
        if (processNames.empty()) {
            clearPendingSessions();
            return false;
        }
        ensureSessionsFresh();
        if (sessions.empty())
            return false;

        auto readFirstValidSession = [&](float& level, BOOL& muteFlag) {
            for (auto& entry : sessions) {
                if (entry.volume && SUCCEEDED(entry.volume->GetMasterVolume(&level)) &&
                    SUCCEEDED(entry.volume->GetMute(&muteFlag)))
                    return true;
            }
            return false;
        };

        float level = 0.0f;
        BOOL muteFlag = FALSE;
        if (!readFirstValidSession(level, muteFlag)) {
            refreshSessions();
            if (sessions.empty() || !readFirstValidSession(level, muteFlag))
                return false;
        }
        percent = std::clamp(static_cast<int>(std::lround(level * 100.0f)), 0, 100);
        muted = muteFlag != FALSE;
        return true;
    }

    bool applyToAll(float level, const bool* mute) {
        ensureSessionsFresh();
        if (sessions.empty())
            return false;
        bool any = false;
        for (auto& entry : sessions) {
            if (SUCCEEDED(entry.volume->SetMasterVolume(level, nullptr)))
                any = true;
            if (mute)
                entry.volume->SetMute(*mute, nullptr);
        }
        if (!any) {
            refreshSessions();
            for (auto& entry : sessions) {
                if (SUCCEEDED(entry.volume->SetMasterVolume(level, nullptr)))
                    any = true;
                if (mute)
                    entry.volume->SetMute(*mute, nullptr);
            }
        }
        return any;
    }
};

AppVolumeController::AppVolumeController() : impl_(std::make_unique<Impl>()) {}
AppVolumeController::~AppVolumeController() = default;

void AppVolumeController::setTargetProcessNames(std::vector<std::wstring> processNames) {
    if (impl_->processNames == processNames)
        return;
    impl_->processNames = std::move(processNames);
    impl_->refreshSessions();
}

void AppVolumeController::setChangedCallback(ChangedCallback cb) {
    impl_->onChanged = std::move(cb);
    impl_->registerEndpointNotification();
    impl_->registerSessionNotification();
}

bool AppVolumeController::query(int& percent, bool& muted) {
    return impl_->query(percent, muted);
}

bool AppVolumeController::setVolumePercent(int percent) {
    if (impl_->processNames.empty())
        return false;
    const float level = std::clamp(percent, 0, 100) / 100.0f;
    const bool unmute = false; // 调整音量同时解除静音
    return impl_->applyToAll(level, &unmute);
}

bool AppVolumeController::setMuted(bool muted) {
    if (impl_->processNames.empty())
        return false;
    int percent = 0;
    bool currentMuted = false;
    if (!impl_->query(percent, currentMuted))
        return false;
    return impl_->applyToAll(percent / 100.0f, &muted);
}
