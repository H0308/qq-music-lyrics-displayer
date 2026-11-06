#include "ui/ui_dispatcher.h"
#include "ui/fluent_theme.h"
#include "ui/dock_backdrop.h"
#include <deque>
#include <mutex>
#include <winrt/base.h>

struct UiDispatcher::State {
    std::mutex mutex;
    std::deque<std::function<void()>> actions;
    HWND window = nullptr;
};

UiDispatcher::UiDispatcher() : state_(std::make_shared<State>()) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = windowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"QQMusicLyricUiDispatcher";
    RegisterClassExW(&type);
    window_ = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, type.hInstance, this);
    if (!window_)
        throw std::runtime_error("Cannot create UI dispatcher window");
    state_->window = window_;
}

UiDispatcher::~UiDispatcher() {
    close();
    if (window_)
        DestroyWindow(window_);
}

void UiDispatcher::close() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->window = nullptr;
    state_->actions.clear();
}

bool UiDispatcher::postTo(const std::shared_ptr<State>& state, std::function<void()> action,
                           bool notify) {
    if (!state)
        return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->window)
        return false;
    state->actions.push_back(std::move(action));
    if (notify && !PostMessageW(state->window, kRun, 0, 0)) {
        state->actions.pop_back();
        return false;
    }
    return true;
}

bool UiDispatcher::post(std::function<void()> action) {
    return postTo(state_, std::move(action));
}

std::function<void()> UiDispatcher::onceCallback(std::function<void()> function) {
    if (!function)
        return {};
    auto pending = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<State> weak = state_;
    return [weak, pending, function = std::move(function)] {
        if (pending->exchange(true))
            return;
        if (!postTo(weak.lock(), [pending, function] {
            pending->store(false);
            function();
        }))
            pending->store(false);
    };
}

void UiDispatcher::drain() {
    // 回调可以销毁所属宿主及本派发窗口；后续只使用独立状态，不再解引用 this。
    const auto state = state_;
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        batch.swap(state->actions);
    }
    // 只消费进入本次消息前的任务，避免持续的播放回调饿死绘制和输入消息。
    for (auto& action : batch) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->window)
                return;
        }
        action();
    }
}

LRESULT CALLBACK UiDispatcher::windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<UiDispatcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<UiDispatcher*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && message == kRun) {
        self->drain();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

UiThread::UiThread() {
    std::promise<UiDispatcher*> ready;
    auto result = ready.get_future();
    thread_ = std::thread([ready = std::move(ready)]() mutable {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        std::unique_ptr<UiDispatcher> dispatcher;
        try {
            dispatcher = std::make_unique<UiDispatcher>();
        } catch (...) {
            ready.set_exception(std::current_exception());
            winrt::uninit_apartment();
            return;
        }
        ready.set_value(dispatcher.get());
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        dispatcher.reset();
        DockBackdrop::shutdownThreadResources();
        fluent::releaseThreadThemeResources();
        winrt::uninit_apartment();
    });
    try {
        dispatcher_ = result.get();
    } catch (...) {
        thread_.join();
        throw;
    }
}

UiThread::~UiThread() {
    if (thread_.joinable()) {
        invoke([this] { PostQuitMessage(0); });
        thread_.join();
    }
}
