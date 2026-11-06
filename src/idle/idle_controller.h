#pragma once
#include "settings/app_settings.h"
#include "app/messages.h"
#include "idle/idle_quote_provider.h"
#include "idle/ticktick_provider.h"
#include <functional>
#include <utility>

struct IdleQuotePayload {
    uint64_t generation = 0;
    IdleQuoteResult result;
};

struct HolidayPayload {
    uint64_t generation = 0;
    HolidayCalendarResult result;
};

struct TickTickTasksPayload {
    uint64_t generation = 0;
    TickTickTasksResult result;
};

struct TickTickTaskCompletePayload {
    uint64_t generation = 0;
    std::wstring taskId;
    TickTickTaskMutationResult result;
};

// 仅供读取的运行快照；请求状态只能由 IdleController 修改。
struct IdleRuntimeState {
    std::wstring idleQuoteText_;
    std::wstring idleQuoteOrigin_;
    std::wstring idleQuoteUuid_;
    bool idleQuoteLoading_ = false;
    uint64_t idleQuoteRequestGeneration_ = 0;
    std::string idleQuoteAttemptKey_;
    std::string idleQuoteRequestKey_;
    TickTickService tickTickService_ = TickTickService::Dida365;
    std::wstring tickTickApiToken_;
    bool tickTickConnected_ = false;
    bool tickTickConnecting_ = false;
    bool tickTickTasksLoading_ = false;
    std::wstring tickTickStatus_ = L"请在设置中连接滴答清单";
    std::vector<IdleTaskInfo> todayTasks_;
    uint64_t tickTickRequestGeneration_ = 0;
    std::wstring tickTickCompletingTaskId_;
    int tickTickStartupRetryAttempt_ = 0;
    bool startupTaskSummaryPending_ = false;
    bool startupTaskSummaryActive_ = false;
    uint64_t holidayRequestGeneration_ = 0;
    bool holidayRequestInFlight_ = false;
    int holidayRequestYear_ = 0;
    int64_t holidayRequestAttemptAt_ = 0;
};

class IdleController {
public:
    IdleController(AppSettings& settings, std::function<void()> save, std::function<void()> publish,
                   std::function<void()> refreshSettings, std::function<bool()> mediaAlive,
                   std::function<bool()> vertical);
    const IdleRuntimeState& state() const { return state_; }
    bool hasTickTickToken() const { return !state_.tickTickApiToken_.empty(); }
    void setTimerWindow(HWND window) { timerWindow_ = window; }
    void initializeCredentials();
    void finishSettingsLoad();
    void saveTickTickApiToken(const std::wstring& text, bool enableAfterSave);
    void cancelStartupSummary();
    void prepareIdleApps();
    bool holidayCalendarFresh(int year) const;
    void refreshHolidayCalendar(bool force);
    IdleDayType todayDayType() const;
    std::wstring defaultIdleWelcome() const;
    bool tickTickEffectiveEnabled() const;
    bool idleTextVisible() const;
    IdlePresentation currentIdlePresentation() const;
    std::wstring startupTaskSummaryText() const;
    void onHolidayReady(std::unique_ptr<HolidayPayload> payload);
    void refreshIdleQuote(bool force);
    void onIdleQuoteReady(std::unique_ptr<IdleQuotePayload> payload);
    void applyIdleTextMode(int mode);
    void applyIdleEntryEnabled(bool enabled);
    void applyIdleQuoteSource(int source);
    void applyIdleQuoteRefreshInterval(int interval);
    void applyTickTickEnabled(bool enabled);
    void connectTickTick();
    void cancelTickTickStartupRetry();
    void resetTickTickStartupRetry();
    bool scheduleTickTickStartupRetry();
    void refreshTickTickTasks();
    void completeTickTickTask(const IdleTaskInfo& task);
    void disconnectTickTick();
    void onTickTickTasksReady(std::unique_ptr<TickTickTasksPayload> payload);
    void onTickTickTaskCompleteReady(std::unique_ptr<TickTickTaskCompletePayload> payload);
    void onStartupTaskSummaryCompleted();
    void openTickTickTask(const IdleTaskInfo& task);

private:
    AppSettings& settings_;
    IdleRuntimeState state_;
    std::function<void()> save_;
    std::function<void()> publish_;
    std::function<void()> refreshSettings_;
    std::function<bool()> mediaAlive_;
    std::function<bool()> vertical_;
    DWORD mainThread_ = GetCurrentThreadId();
    HWND timerWindow_ = nullptr;
    // Providers 最后声明，析构时先等待工作线程；回调引用的状态和通知仍然有效。
    IdleQuoteProvider idleQuoteProvider_;
    TickTickProvider tickTickProvider_;
};
