#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

// 拥有线程上的消息队列。跨线程调用按队列顺序执行，不在调用方直接访问窗口状态。
class UiDispatcher {
public:
    UiDispatcher();
    ~UiDispatcher();
    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;
    bool post(std::function<void()> action);
    void close();

    template<class F> auto invoke(F action) -> std::invoke_result_t<F> {
        using Result = std::invoke_result_t<F>;
        if (GetCurrentThreadId() == ownerThread_)
            return action();
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(action));
        auto result = task->get_future();
        if (!postTo(state_, [task] { (*task)(); }, false))
            throw std::runtime_error("UI dispatcher is closed");
        // SendMessage 的等待会服务同步窗口消息，避免原生窗口跨线程调用相互等待。
        SendMessageW(window_, kRun, 0, 0);
        return result.get();
    }

    template<class... Args>
    std::function<void(Args...)> callback(std::function<void(Args...)> function) {
        if (!function)
            return {};
        std::weak_ptr<State> weak = state_;
        return [weak, function = std::move(function)](Args... args) {
            auto values = std::make_tuple(args...);
            postTo(weak.lock(), [function, values = std::move(values)]() mutable {
                std::apply(function, values);
            });
        };
    }
    std::function<void()> onceCallback(std::function<void()> function);

private:
    struct State;
    static constexpr UINT kRun = WM_APP + 1;
    static bool postTo(const std::shared_ptr<State>& state, std::function<void()> action,
                       bool notify = true);
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp);
    void drain();
    HWND window_ = nullptr;
    DWORD ownerThread_ = GetCurrentThreadId();
    std::shared_ptr<State> state_;
};

// 专用 STA 消息线程；窗口、COM 和绘制资源都必须在该线程创建及释放。
class UiThread {
public:
    UiThread();
    ~UiThread();
    UiThread(const UiThread&) = delete;
    UiThread& operator=(const UiThread&) = delete;
    template<class F> auto invoke(F action) -> std::invoke_result_t<F> {
        return dispatcher_->invoke(std::move(action));
    }
    bool post(std::function<void()> action) { return dispatcher_->post(std::move(action)); }
private:
    std::thread thread_;
    UiDispatcher* dispatcher_ = nullptr;
};
