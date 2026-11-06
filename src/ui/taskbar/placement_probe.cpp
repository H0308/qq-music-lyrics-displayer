#include "ui/taskbar/placement_probe.h"
#include "ui/taskbar/platform.h"
#include <wrl/client.h>
#include <thread>
using namespace taskbar_detail;
namespace {
constexpr UINT kProbeIntervalMs = 3000;
constexpr UINT kNoSpaceProbeIntervalMs = 1000;
} // namespace
void PlacementProbe::start() {
    if (running_)
        return;
    if (state_->stop.load()) {
        auto state = std::make_shared<ProbeState>();
        state->taskbar = state_->taskbar.load();
        state->fast = state_->fast.load();
        state_ = std::move(state);
    }
    std::thread([state = state_] { run(state); }).detach();
    running_ = true;
}

void PlacementProbe::stop() {
    state_->stop = true;
    state_->fast = false;
    running_ = false;
    delete state_->out.exchange(nullptr);
}

void PlacementProbe::run(const std::shared_ptr<ProbeState>& state) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return;
    Microsoft::WRL::ComPtr<IUIAutomation> uia;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(uia.GetAddressOf()));
    while (!state->stop.load()) {
        if (HWND tb = state->taskbar.load()) {
            auto p = std::make_unique<PlacementProbeResult>();
            if (HWND tm = findTrafficMonitorWnd(tb))
                GetWindowRect(tm, &p->rcTm);
            if (state->stop.load())
                break;
            queryTaskbarButtonsUia(uia.Get(), tb, p->buttons);
            if (state->stop.load())
                break;
            queryKugouTaskbarWindows(tb, p->kugouWindows);
            if (state->stop.load())
                break;
            delete state->out.exchange(p.release()); // 上一份未被拾取则丢弃
        }
        int elapsed = 0;
        while (!state->stop.load()) {
            const UINT interval = state->fast.load() ? kNoSpaceProbeIntervalMs : kProbeIntervalMs;
            if (elapsed >= static_cast<int>(interval))
                break;
            Sleep(100);
            elapsed += 100;
        }
    }
    uia.Reset();
    CoUninitialize();
}
