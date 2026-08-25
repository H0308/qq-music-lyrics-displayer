#include "app_volume.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
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
    explicit SessionNotificationSink(std::function<void()> onCreated)
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

    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override {
        if (onCreated_)
            onCreated_();
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::function<void()> onCreated_;
};

} // namespace

struct AppVolumeController::Impl {
    struct SessionEntry {
        IAudioSessionControl* control = nullptr; // 持有引用，注销事件时要用
        ISimpleAudioVolume* volume = nullptr;
        SessionEventsSink* sink = nullptr;       // 我们持有 1 个引用
    };

    std::vector<std::wstring> processNames;
    ChangedCallback onChanged;

    IMMDeviceEnumerator* devices = nullptr;
    IAudioSessionManager2* manager = nullptr;
    SessionNotificationSink* sessionSink = nullptr; // 我们持有 1 个引用
    bool sessionNotifyRegistered = false;
    std::vector<SessionEntry> sessions;

    ~Impl() {
        clearSessions();
        unregisterSessionNotification();
        if (manager)
            manager->Release();
        if (devices)
            devices->Release();
    }

    bool ensureManager() {
        if (manager)
            return true;
        if (!devices && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                                reinterpret_cast<void**>(&devices))))
            return false;
        IMMDevice* device = nullptr;
        HRESULT hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&manager));
        if (device)
            device->Release();
        if (FAILED(hr) || !manager)
            return false;
        registerSessionNotification();
        return true;
    }

    void registerSessionNotification() {
        if (sessionNotifyRegistered || !manager || !onChanged)
            return;
        if (!sessionSink)
            sessionSink = new SessionNotificationSink(onChanged);
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

    // 重新枚举默认输出设备上与目标进程名匹配的未过期会话（可跨多个进程）。
    void refreshSessions() {
        clearSessions();
        if (processNames.empty() || !ensureManager())
            return;

        IAudioSessionEnumerator* enumerator = nullptr;
        if (FAILED(manager->GetSessionEnumerator(&enumerator)) || !enumerator)
            return;
        int count = 0;
        enumerator->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(enumerator->GetSession(i, &control)) || !control)
                continue;

            AudioSessionState state = AudioSessionStateExpired;
            control->GetState(&state);
            IAudioSessionControl2* control2 = nullptr;
            DWORD pid = 0;
            ISimpleAudioVolume* volume = nullptr;
            if (state != AudioSessionStateExpired &&
                SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                  reinterpret_cast<void**>(&control2))) &&
                control2 && SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0 &&
                isTargetProcess(pid, processNames) &&
                SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                  reinterpret_cast<void**>(&volume))) &&
                volume) {
                SessionEntry entry;
                entry.control = control;
                entry.volume = volume;
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
        enumerator->Release();
    }

    // 缓存为空或其中有会话已过期时重建。
    void ensureSessionsFresh() {
        bool stale = sessions.empty();
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
        if (processNames.empty())
            return false;
        ensureSessionsFresh();
        if (sessions.empty())
            return false;

        float level = 0.0f;
        BOOL muteFlag = FALSE;
        if (FAILED(sessions.front().volume->GetMasterVolume(&level)) ||
            FAILED(sessions.front().volume->GetMute(&muteFlag))) {
            refreshSessions();
            if (sessions.empty() ||
                FAILED(sessions.front().volume->GetMasterVolume(&level)) ||
                FAILED(sessions.front().volume->GetMute(&muteFlag)))
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
