#include "audio_spectrum.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFftSize = 1024;                 // 频率分辨率 ~46.9Hz
constexpr int kFftHop = 512;                   // 50% 重叠，~94 次 FFT/s
constexpr float kBandLowHz = 60.0f;            // 频段下界（以下为 DC/轰头区，丢弃）
constexpr float kBandHighHz = 14000.0f;        // 频段上界
constexpr float kMinDb = -50.0f;               // 电平映射：minDb..maxDb -> 0..1
constexpr float kMaxDb = -6.0f;
constexpr float kRelease = 0.90f;              // 每个 FFT 帧的回落系数（~150ms 时间常数）
constexpr ULONGLONG kNoDataRebuildMs = 2000;  // 连续无音频包约 2s 后重建捕获会话

// 找 QQ 音乐进程树根：父进程不是 QQMusic.exe 的那个 QQMusic.exe。
// 进程级回环捕获 INCLUDE 模式覆盖目标进程及其子进程，从树根捕获最完整。
DWORD findQQMusicRootPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    struct Proc { DWORD pid; DWORD parent; };
    std::vector<Proc> qq;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"QQMusic.exe") == 0)
            qq.push_back({pe.th32ProcessID, pe.th32ParentProcessID});
    }
    CloseHandle(snap);

    for (const auto& p : qq) {
        bool parentIsQQ = std::any_of(qq.begin(), qq.end(),
                                      [&](const Proc& q) { return q.pid == p.parent; });
        if (!parentIsQQ)
            return p.pid;
    }
    return qq.empty() ? 0 : qq.front().pid;
}

bool isQQMusicProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;

    wchar_t path[MAX_PATH]{};
    DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    bool match = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!match)
        return false;

    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"QQMusic.exe") == 0;
}

// 从当前默认输出设备的活动音频会话中找真正产生声音的 QQMusic.exe。
// QQ 音乐重启后可能同时存在多个同名进程，不能再依赖进程快照中的第一个根进程。
DWORD findQQMusicAudioPid() {
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;
    DWORD result = 0;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&devices));
    if (SUCCEEDED(hr))
        hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&manager));
    if (SUCCEEDED(hr))
        hr = manager->GetSessionEnumerator(&sessions);

    int count = 0;
    if (SUCCEEDED(hr))
        hr = sessions->GetCount(&count);
    for (int i = 0; SUCCEEDED(hr) && i < count && !result; ++i) {
        IAudioSessionControl* control = nullptr;
        IAudioSessionControl2* control2 = nullptr;
        AudioSessionState state = AudioSessionStateExpired;
        DWORD pid = 0;

        hr = sessions->GetSession(i, &control);
        if (SUCCEEDED(hr))
            hr = control->GetState(&state);
        if (SUCCEEDED(hr) && state == AudioSessionStateActive)
            hr = control->QueryInterface(__uuidof(IAudioSessionControl2),
                                          reinterpret_cast<void**>(&control2));
        if (SUCCEEDED(hr) && control2 && SUCCEEDED(control2->GetProcessId(&pid)) &&
            pid != 0 && isQQMusicProcess(pid))
            result = pid;

        if (control2)
            control2->Release();
        if (control)
            control->Release();
    }

    if (sessions)
        sessions->Release();
    if (manager)
        manager->Release();
    if (device)
        device->Release();
    if (devices)
        devices->Release();
    return result;
}

// ActivateAudioInterfaceAsync 完成回调：把结果转交给等待中的捕获线程
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivateHandler(HANDLE done) : done_(done) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0)
            delete this;
        return r;
    }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrAct = E_FAIL;
        IUnknown* unk = nullptr;
        HRESULT hr = op->GetActivateResult(&hrAct, &unk);
        if (SUCCEEDED(hr))
            hr = hrAct;
        if (SUCCEEDED(hr) && unk)
            hr = unk->QueryInterface(__uuidof(IAudioClient),
                                     reinterpret_cast<void**>(&client_));
        if (unk)
            unk->Release();
        hr_ = hr;
        SetEvent(done_);
        return S_OK;
    }

    HRESULT hr_ = E_FAIL;
    IAudioClient* client_ = nullptr;

private:
    HANDLE done_;
    ULONG ref_ = 1;
};

// 迭代 radix-2 FFT（输入长度必须是 2 的幂）
void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { // 位反转重排
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * 3.14159265358979f / static_cast<float>(len);
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                auto u = a[i + j];
                auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// 捕获线程内部分析器：攒采样 -> 加窗 FFT -> 对数分频段 -> 平滑 -> 发布
struct Analyzer {
    std::array<std::atomic<float>, AudioSpectrum::kBandCount>* out;
    float window[kFftSize];
    float ring[kFftSize]{};      // 单声道环形缓冲
    int writePos = 0;
    int sinceFft = 0;
    float smooth[AudioSpectrum::kBandCount]{};
    // 各频段对应的 FFT bin 区间 [lo, hi]
    int binLo[AudioSpectrum::kBandCount];
    int binHi[AudioSpectrum::kBandCount];

    explicit Analyzer(decltype(out) o) : out(o) {
        for (int i = 0; i < kFftSize; ++i) // Hann 窗
            window[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / (kFftSize - 1));
        constexpr int bands = AudioSpectrum::kBandCount;
        const float ratio = kBandHighHz / kBandLowHz;
        for (int b = 0; b < bands; ++b) {
            float f0 = kBandLowHz * std::pow(ratio, static_cast<float>(b) / bands);
            float f1 = kBandLowHz * std::pow(ratio, static_cast<float>(b + 1) / bands);
            binLo[b] = std::max(1, static_cast<int>(f0 * kFftSize / kSampleRate));
            binHi[b] = std::max(binLo[b], static_cast<int>(f1 * kFftSize / kSampleRate) - 1);
        }
    }

    // interleaved float32 立体声 -> 单声道入环
    void push(const float* interleaved, UINT32 frames) {
        for (UINT32 i = 0; i < frames; ++i) {
            ring[writePos] = (interleaved[i * 2] + interleaved[i * 2 + 1]) * 0.5f;
            writePos = (writePos + 1) % kFftSize;
            if (++sinceFft >= kFftHop) {
                sinceFft = 0;
                analyze();
            }
        }
    }

    void pushSilence(UINT32 frames) {
        for (UINT32 i = 0; i < frames; ++i) {
            ring[writePos] = 0.0f;
            writePos = (writePos + 1) % kFftSize;
            if (++sinceFft >= kFftHop) {
                sinceFft = 0;
                analyze();
            }
        }
    }

    void analyze() {
        std::vector<std::complex<float>> buf(kFftSize);
        for (int i = 0; i < kFftSize; ++i) { // 最旧采样在前
            int idx = (writePos + i) % kFftSize;
            buf[i] = ring[idx] * window[i];
        }
        fft(buf);
        constexpr int bands = AudioSpectrum::kBandCount;
        for (int b = 0; b < bands; ++b) {
            float sum = 0.0f;
            for (int k = binLo[b]; k <= binHi[b]; ++k)
                sum += std::abs(buf[k]);
            // Hann 窗相干增益 0.5、单侧谱乘 2：mag 近似正弦分量幅值（满幅 = 1）
            float mag = sum / (binHi[b] - binLo[b] + 1) * 4.0f / kFftSize;
            float db = 20.0f * std::log10(mag + 1e-9f) + b * 2.0f; // 高频补偿粉噪倾斜
            float level = std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
            // 上升立即、下降平滑
            smooth[b] = level > smooth[b] ? level
                                          : smooth[b] * kRelease + level * (1.0f - kRelease);
            (*out)[b].store(smooth[b], std::memory_order_relaxed);
        }
    }

    void publishZeros() {
        for (auto& v : *out)
            v.store(0.0f, std::memory_order_relaxed);
        for (auto& s : smooth)
            s = 0.0f;
    }
};

template <typename T>
void releaseCom(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

AudioSpectrum::~AudioSpectrum() {
    stop();
}

void AudioSpectrum::start() {
    if (thread_.joinable())
        return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void AudioSpectrum::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    for (auto& v : bands_)
        v.store(0.0f, std::memory_order_relaxed);
}

void AudioSpectrum::requestReconnect() {
    reconnectRequested_.store(true, std::memory_order_release);
}

std::array<float, AudioSpectrum::kBandCount> AudioSpectrum::bands() const {
    std::array<float, kBandCount> r;
    for (int i = 0; i < kBandCount; ++i)
        r[i] = bands_[i].load(std::memory_order_relaxed);
    return r;
}

void AudioSpectrum::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto sleepOrStop = [this](DWORD ms) {
        // 分段睡眠以便 stop() 快速响应
        for (DWORD t = 0; t < ms && !stop_.load() && !reconnectRequested_.load(); t += 100)
            Sleep(std::min<DWORD>(100, ms - t));
    };

    while (!stop_.load()) {
        reconnectRequested_.store(false, std::memory_order_release);
        // 优先使用当前真正出声的 QQ 音乐音频会话，避免重启后命中同名辅助进程。
        // 暂停或尚未开始播放时没有活动会话，再回退到进程树扫描。
        DWORD pid = findQQMusicAudioPid();
        if (!pid)
            pid = findQQMusicRootPid();
        if (!pid) {
            for (auto& v : bands_)
                v.store(0.0f, std::memory_order_relaxed);
            sleepOrStop(2000);
            continue;
        }

        // ---- 激活进程级回环虚拟设备（仅捕获 pid 进程树）----
        AUDIOCLIENT_ACTIVATION_PARAMS actParams{};
        actParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        actParams.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        actParams.ProcessLoopbackParams.TargetProcessId = pid;

        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(actParams);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&actParams);

        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto* handler = new ActivateHandler(done);
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                 __uuidof(IAudioClient), &pv, handler, &op);
        IAudioClient* client = nullptr;
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(done, 5000);
            hr = handler->hr_;
            client = handler->client_;
            handler->client_ = nullptr;
        }
        handler->Release();
        releaseCom(op);
        CloseHandle(done);
        if (FAILED(hr) || !client) {
            releaseCom(client);
            sleepOrStop(2000);
            continue;
        }

        // ---- 初始化捕获（进程回环不支持 GetMixFormat，格式手工指定 + 系统自动转 PCM）----
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = kSampleRate;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK |
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                2000000, 0, &fmt, nullptr); // 200ms 缓冲
        HANDLE dataReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        IAudioCaptureClient* capture = nullptr;
        if (SUCCEEDED(hr))
            hr = client->SetEventHandle(dataReady);
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&capture));
        if (SUCCEEDED(hr))
            hr = client->Start();

        Analyzer analyzer(&bands_);
        bool sessionOk = SUCCEEDED(hr);

        // ---- 捕获循环：事件驱动取包；静音/暂停时不投递数据包，用等量静音推进分析让频段自然回落 ----
        ULONGLONG lastPacketTick = GetTickCount64();
        while (sessionOk && !stop_.load() && !reconnectRequested_.load(std::memory_order_acquire)) {
            DWORD wr = WaitForSingleObject(dataReady, 500);
            if (wr == WAIT_FAILED) {
                sessionOk = false;
                break;
            }
            if (wr == WAIT_TIMEOUT) {
                analyzer.pushSilence(kSampleRate / 2); // 0.5s 静音 -> 按 release 节奏回落
                // QQ 退出/重启或音频会话重建时，进程回环可能不再来数据。
                // 不能只依赖 WAIT_TIMEOUT：旧会话也可能持续触发事件但没有数据包。
                if (GetTickCount64() - lastPacketTick >= kNoDataRebuildMs) {
                    break;
                }
                continue;
            }

            UINT32 packet = 0;
            hr = capture->GetNextPacketSize(&packet);
            while (SUCCEEDED(hr) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr))
                    break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                    analyzer.pushSilence(frames);
                else
                    analyzer.push(reinterpret_cast<const float*>(data), frames);
                capture->ReleaseBuffer(frames);
                if (frames > 0)
                    lastPacketTick = GetTickCount64();
                hr = capture->GetNextPacketSize(&packet);
            }
            if (FAILED(hr))
                sessionOk = false;
            else if (GetTickCount64() - lastPacketTick >= kNoDataRebuildMs)
                break;
        }

        client->Stop();
        releaseCom(capture);
        releaseCom(client);
        CloseHandle(dataReady);
        analyzer.publishZeros();
        if (!reconnectRequested_.load(std::memory_order_acquire))
            sleepOrStop(1000); // 会话断开（QQ 重启等）后重建
    }

    CoUninitialize();
}
