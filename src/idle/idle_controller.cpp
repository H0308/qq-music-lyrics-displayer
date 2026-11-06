#include "idle/idle_controller.h"
#include "logging/runtime_logger.h"
#include "ui/platform_icon.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cwctype>

namespace {
constexpr int64_t kHolidayCacheMaxAgeSec = 7 * 24 * 60 * 60;
constexpr int kTickTickStartupMaxRetries = 3;
constexpr UINT kTickTickStartupRetryDelaysMs[kTickTickStartupMaxRetries] = {5000, 15000, 30000};
std::string idleQuotePeriodKey(IdleQuoteRefreshInterval interval) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char key[64]{};
    if (interval == IdleQuoteRefreshInterval::Hourly) {
        sprintf_s(key, "%04d-%02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour);
    } else if (interval == IdleQuoteRefreshInterval::HalfDay) {
        sprintf_s(key, "%04d-%02d-%02d-%s", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour < 12 ? "0-12" : "12-24");
    } else {
        sprintf_s(key, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    }
    return key;
}

std::tm localNow() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return local;
}

std::string localDateKey(const std::tm& local) {
    char key[32]{};
    sprintf_s(key, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return key;
}

bool validExePath(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".exe") == 0;
}

std::wstring fallbackExeName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
    std::wstring name = path.substr(start);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
        name.resize(dot);
    return name;
}

const wchar_t* idleQuoteSourceLabel(IdleQuoteSource source) {
    return source == IdleQuoteSource::Jinrishici ? L"今日诗词"
           : source == IdleQuoteSource::Netease  ? L"网易云热评"
                                                 : L"一言";
}

const char* idleQuoteSourceConfigName(IdleQuoteSource source) {
    return source == IdleQuoteSource::Jinrishici ? "jinrishici"
           : source == IdleQuoteSource::Netease  ? "netease"
                                                 : "hitokoto";
}

const char* idleQuoteIntervalConfigName(IdleQuoteRefreshInterval interval) {
    switch (interval) {
    case IdleQuoteRefreshInterval::HalfDay:
        return "half-day";
    case IdleQuoteRefreshInterval::Hourly:
        return "hourly";
    case IdleQuoteRefreshInterval::Daily:
    default:
        return "daily";
    }
}
} // namespace

IdleController::IdleController(AppSettings& settings, std::function<void()> save,
                               std::function<void()> publish, std::function<void()> refreshSettings,
                               std::function<bool()> mediaAlive, std::function<bool()> vertical)
    : settings_(settings), save_(std::move(save)), publish_(std::move(publish)),
      refreshSettings_(std::move(refreshSettings)), mediaAlive_(std::move(mediaAlive)),
      vertical_(std::move(vertical)) {}

void IdleController::initializeCredentials() {
    tickTickProvider_.loadApiToken(state_.tickTickService_, state_.tickTickApiToken_);
    settings_.tickTickEnabled_ = hasTickTickToken();
    resetTickTickStartupRetry();
    state_.startupTaskSummaryPending_ = tickTickEffectiveEnabled();
    state_.tickTickStatus_ = tickTickEffectiveEnabled() ? L"API 口令已配置，正在同步今日任务…"
                                                        : L"请在设置中填写 API 口令";
}

void IdleController::finishSettingsLoad() {
    state_.tickTickConnected_ = false;
    state_.tickTickConnecting_ = false;
    state_.tickTickTasksLoading_ = false;
    if (!hasTickTickToken()) {
        settings_.tickTickEnabled_ = false;
        state_.tickTickStatus_ = L"请在设置中填写 API 口令";
    } else if (!settings_.tickTickEnabled_) {
        state_.tickTickStatus_ = L"滴答清单已关闭";
    } else {
        state_.tickTickStatus_ = L"API 口令已配置，正在同步今日任务…";
    }
    state_.startupTaskSummaryPending_ = tickTickEffectiveEnabled();
    state_.startupTaskSummaryActive_ = false;
}

void IdleController::cancelStartupSummary() {
    state_.startupTaskSummaryPending_ = false;
    state_.startupTaskSummaryActive_ = false;
    resetTickTickStartupRetry();
}

void IdleController::saveTickTickApiToken(const std::wstring& text, bool enableAfterSave) {
    std::wstring normalized = text;
    while (!normalized.empty() && iswspace(normalized.front()) != 0)
        normalized.erase(normalized.begin());
    while (!normalized.empty() && iswspace(normalized.back()) != 0)
        normalized.pop_back();

    ++state_.tickTickRequestGeneration_;
    resetTickTickStartupRetry();
    state_.startupTaskSummaryPending_ = false;
    state_.startupTaskSummaryActive_ = false;
    state_.tickTickConnected_ = false;
    state_.tickTickConnecting_ = false;
    state_.tickTickTasksLoading_ = false;
    state_.tickTickCompletingTaskId_.clear();
    state_.todayTasks_.clear();
    bool shouldRefresh = false;
    if (normalized.empty()) {
        tickTickProvider_.clearApiToken(state_.tickTickService_);
        state_.tickTickApiToken_.clear();
        settings_.tickTickEnabled_ = false;
        state_.tickTickStatus_ = L"请在设置中填写 API 口令";
    } else if (!tickTickProvider_.saveApiToken(state_.tickTickService_, normalized)) {
        state_.tickTickStatus_ = L"无法保存滴答清单 API 口令";
    } else {
        state_.tickTickApiToken_ = std::move(normalized);
        if (enableAfterSave)
            settings_.tickTickEnabled_ = true;
        shouldRefresh = tickTickEffectiveEnabled();
        state_.tickTickStatus_ = shouldRefresh ? L"API 口令已保存，正在同步今日任务…"
                                               : L"API 口令已保存，滴答清单已关闭";
    }
    save_();
    refreshSettings_();
    publish_();
    if (shouldRefresh)
        refreshTickTickTasks();
}

void IdleController::prepareIdleApps() {
    for (auto& app : settings_.idleApps_) {
        app.pathValid = validExePath(app.path);
        app.iconPixels.reset();
        app.iconWidth = 0;
        app.iconHeight = 0;
        app.name.clear();
        if (!app.customName.empty()) {
            app.name = app.customName;
        } else if (!app.pathValid) {
            app.name = fallbackExeName(app.path);
        } else if (!platform_icon::readExeDisplayName(app.path, app.name) || app.name.empty()) {
            app.name = fallbackExeName(app.path);
        }

        if (!app.pathValid)
            continue;

        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        if (platform_icon::readExeIconPixels(app.path, pixels, width, height) && !pixels.empty() &&
            width > 0 && height > 0) {
            app.iconPixels = std::make_shared<const std::vector<BYTE>>(std::move(pixels));
            app.iconWidth = width;
            app.iconHeight = height;
        }
    }
}

bool IdleController::holidayCalendarFresh(int year) const {
    if (settings_.holidayCalendarYear_ != year || settings_.holidayCalendarUpdatedAt_ <= 0)
        return false;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    return now < settings_.holidayCalendarUpdatedAt_ ||
           now - settings_.holidayCalendarUpdatedAt_ <= kHolidayCacheMaxAgeSec;
}

void IdleController::refreshHolidayCalendar(bool force) {
    const std::tm local = localNow();
    const int year = local.tm_year + 1900;
    if (!force && holidayCalendarFresh(year))
        return;
    if (!force && state_.holidayRequestInFlight_)
        return;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (!force && state_.holidayRequestYear_ == year && state_.holidayRequestAttemptAt_ > 0 &&
        (now < state_.holidayRequestAttemptAt_ ||
         now - state_.holidayRequestAttemptAt_ < kHolidayCacheMaxAgeSec))
        return;

    ++state_.holidayRequestGeneration_;
    state_.holidayRequestInFlight_ = true;
    state_.holidayRequestYear_ = year;
    state_.holidayRequestAttemptAt_ = now;
    const uint64_t generation = state_.holidayRequestGeneration_;
    idleQuoteProvider_.requestHolidayYearAsync(
        year, [this, generation](HolidayCalendarResult result) {
            auto* payload = new HolidayPayload{generation, std::move(result)};
            if (!PostThreadMessageW(mainThread_, kMsgHolidayReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

IdleDayType IdleController::todayDayType() const {
    const std::tm local = localNow();
    const std::string today = localDateKey(local);
    if (settings_.holidayCalendarYear_ == local.tm_year + 1900) {
        for (const auto& day : settings_.holidayCalendar_) {
            if (day.date == today && day.type >= 0 && day.type <= 3)
                return static_cast<IdleDayType>(day.type);
        }
    }
    // 年度数据尚未返回或接口没有覆盖该日时，先使用自然星期生成首屏文案；
    // 缓存到达后只刷新内容，不影响网络请求和入口状态。
    return local.tm_wday == 0 || local.tm_wday == 6 ? IdleDayType::Weekend : IdleDayType::Workday;
}

std::wstring IdleController::defaultIdleWelcome() const {
    const std::tm local = localNow();
    const wchar_t* greeting = local.tm_hour >= 5 && local.tm_hour < 12    ? L"早上好"
                              : local.tm_hour >= 12 && local.tm_hour < 18 ? L"中午好"
                                                                          : L"晚上好";
    switch (todayDayType()) {
    case IdleDayType::MakeupWorkday:
        return L"放假最开心，调休伤身体，用一首歌的时间平衡一下吧~";
    case IdleDayType::Weekend:
    case IdleDayType::Holiday:
        return std::wstring(greeting) + L"，牛马快乐日用一首歌开启周末美好时光～";
    case IdleDayType::Workday:
    default:
        return std::wstring(greeting) + L"，致敬奋斗者，闲暇时光用一首歌的时间放松一下心情吧～";
    }
}

bool IdleController::tickTickEffectiveEnabled() const {
    return settings_.tickTickEnabled_ && !state_.tickTickApiToken_.empty();
}

bool IdleController::idleTextVisible() const {
    return settings_.idleQuoteEnabled_ || settings_.idleWelcomeEnabled_ ||
           settings_.taskbarViewMode_ != TaskbarViewMode::Embedded ||
           !settings_.embeddedResourceMonitor_ || vertical_();
}

IdlePresentation IdleController::currentIdlePresentation() const {
    IdlePresentation presentation;
    presentation.showText = idleTextVisible();
    presentation.showQuote = settings_.idleQuoteEnabled_;
    presentation.quickStartEnabled = settings_.idleEntryEnabled_;
    presentation.showAppNames = settings_.idleAppNamesVisible_;
    if (settings_.idleQuoteEnabled_) {
        presentation.loading = state_.idleQuoteLoading_;
        presentation.sentence = state_.idleQuoteText_;
        if (presentation.sentence.empty())
            presentation.sentence =
                state_.idleQuoteLoading_ ? L"正在获取每日一言…" : L"暂时无法获取每日一言";
        presentation.source = idleQuoteSourceLabel(settings_.idleQuoteSource_);
        presentation.copyEnabled = !state_.idleQuoteLoading_ && !state_.idleQuoteText_.empty();
        if (!state_.idleQuoteOrigin_.empty()) {
            presentation.source += L" · ";
            presentation.source += state_.idleQuoteOrigin_;
        }
    } else if (presentation.showText) {
        presentation.sentence = settings_.idleCustomWelcome_.empty() ? defaultIdleWelcome()
                                                                     : settings_.idleCustomWelcome_;
    }
    presentation.apps = settings_.idleApps_;
    presentation.todayTasksEnabled = tickTickEffectiveEnabled();
    presentation.todayTasks = state_.todayTasks_;
    presentation.todayTasksLoading = state_.tickTickTasksLoading_;
    presentation.todayTasksConnected = state_.tickTickConnected_;
    presentation.todayTasksStatus = state_.tickTickStatus_;
    return presentation;
}

std::wstring IdleController::startupTaskSummaryText() const {
    std::size_t remaining = 0;
    std::size_t high = 0;
    std::size_t medium = 0;
    std::size_t low = 0;
    std::size_t none = 0;
    for (const auto& task : state_.todayTasks_) {
        if (task.completed)
            continue;
        ++remaining;
        switch (task.priority) {
        case IdleTaskPriority::High:
            ++high;
            break;
        case IdleTaskPriority::Medium:
            ++medium;
            break;
        case IdleTaskPriority::Low:
            ++low;
            break;
        case IdleTaskPriority::None:
        default:
            ++none;
            break;
        }
    }
    if (remaining == 0)
        return L"今日没有任务";

    std::wstring summary = L"今日剩余 ";
    summary += std::to_wstring(remaining);
    summary += L" 个任务";

    std::size_t emptyPriorityCount = 0;
    const auto appendPriority = [&](const wchar_t* label, std::size_t count) {
        if (count == 0) {
            ++emptyPriorityCount;
            return;
        }
        summary += L"，";
        summary += label;
        summary += L"有 ";
        summary += std::to_wstring(count);
        summary += L" 个";
    };
    appendPriority(L"高优先级", high);
    appendPriority(L"中优先级", medium);
    appendPriority(L"低优先级", low);
    appendPriority(L"无优先级", none);
    if (emptyPriorityCount > 0)
        summary += L"，其余优先级没有任务";
    return summary;
}

void IdleController::onHolidayReady(std::unique_ptr<HolidayPayload> payload) {
    if (!payload || payload->generation != state_.holidayRequestGeneration_)
        return;
    state_.holidayRequestInFlight_ = false;
    const std::tm local = localNow();
    const int currentYear = local.tm_year + 1900;
    if (!payload->result.ok || payload->result.year != currentYear) {
        if (payload->result.year != currentYear)
            refreshHolidayCalendar(false);
        return;
    }

    settings_.holidayCalendarYear_ = payload->result.year;
    settings_.holidayCalendarUpdatedAt_ = static_cast<int64_t>(std::time(nullptr));
    settings_.holidayCalendar_ = std::move(payload->result.days);
    save_();
    if (settings_.idleEntryEnabled_ && !settings_.idleQuoteEnabled_ && idleTextVisible() &&
        !mediaAlive_())
        publish_();
}

void IdleController::refreshIdleQuote(bool force) {
    // 每日一言属于空闲面板的独立内容状态；即使启动时已经存在播放器，
    // 也要先把内容准备好，用户切到“每日一言 + 快速打开”面板时才能直接显示。
    if (!settings_.idleEntryEnabled_ || !settings_.idleQuoteEnabled_)
        return;

    const std::string period = idleQuotePeriodKey(settings_.idleQuoteRefreshInterval_);
    const std::string key = std::string(idleQuoteSourceConfigName(settings_.idleQuoteSource_)) +
                            "|" + idleQuoteIntervalConfigName(settings_.idleQuoteRefreshInterval_) +
                            "|" + period;
    const bool cacheMatches =
        settings_.idleQuoteCacheSource_ == idleQuoteSourceConfigName(settings_.idleQuoteSource_) &&
        settings_.idleQuoteCachePeriod_ == period && !settings_.idleQuoteCacheText_.empty();
    if (cacheMatches && !force) {
        state_.idleQuoteText_ = settings_.idleQuoteCacheText_;
        state_.idleQuoteOrigin_ = settings_.idleQuoteCacheOrigin_;
        state_.idleQuoteUuid_ = settings_.idleQuoteCacheUuid_;
        state_.idleQuoteLoading_ = false;
        state_.idleQuoteAttemptKey_ = key;
        publish_();
        return;
    }
    if (!force && (state_.idleQuoteRequestKey_ == key || state_.idleQuoteAttemptKey_ == key))
        return;

    ++state_.idleQuoteRequestGeneration_;
    state_.idleQuoteRequestKey_ = key;
    state_.idleQuoteAttemptKey_ = key;
    state_.idleQuoteText_.clear();
    state_.idleQuoteOrigin_.clear();
    state_.idleQuoteUuid_.clear();
    state_.idleQuoteLoading_ = true;
    const uint64_t generation = state_.idleQuoteRequestGeneration_;
    const IdleQuoteSource source = settings_.idleQuoteSource_;
    idleQuoteProvider_.requestAsync(
        source, settings_.jinrishiciToken_, [this, generation](IdleQuoteResult result) {
            auto* payload = new IdleQuotePayload{generation, std::move(result)};
            if (!PostThreadMessageW(mainThread_, kMsgIdleQuoteReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
    publish_();
}

void IdleController::onIdleQuoteReady(std::unique_ptr<IdleQuotePayload> payload) {
    if (!settings_.idleEntryEnabled_ || !settings_.idleQuoteEnabled_ || !payload ||
        payload->generation != state_.idleQuoteRequestGeneration_ ||
        state_.idleQuoteRequestKey_.empty())
        return;

    if (!payload->result.token.empty()) {
        settings_.jinrishiciToken_ = payload->result.token;
        save_();
    }
    state_.idleQuoteLoading_ = false;
    if (payload->result.ok) {
        state_.idleQuoteText_ = payload->result.content;
        state_.idleQuoteOrigin_ = payload->result.origin;
        state_.idleQuoteUuid_ = payload->result.uuid;
        settings_.idleQuoteCacheSource_ = idleQuoteSourceConfigName(settings_.idleQuoteSource_);
        settings_.idleQuoteCachePeriod_ = idleQuotePeriodKey(settings_.idleQuoteRefreshInterval_);
        settings_.idleQuoteCacheText_ = state_.idleQuoteText_;
        settings_.idleQuoteCacheOrigin_ = state_.idleQuoteOrigin_;
        settings_.idleQuoteCacheUuid_ = state_.idleQuoteUuid_;
        save_();
    }
    publish_();
}

void IdleController::applyIdleTextMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    const bool enabled = mode == 0;
    const bool welcome = mode == 1;
    if (settings_.idleQuoteEnabled_ == enabled && settings_.idleWelcomeEnabled_ == welcome)
        return;
    settings_.idleQuoteEnabled_ = enabled;
    settings_.idleWelcomeEnabled_ = welcome;
    runtime_log::writef(L"[action][idle-text] mode=%d", mode);
    if (!enabled) {
        ++state_.idleQuoteRequestGeneration_;
        state_.idleQuoteRequestKey_.clear();
        state_.idleQuoteLoading_ = false;
    } else {
        refreshIdleQuote(false);
    }
    save_();
    publish_();
}

void IdleController::applyIdleEntryEnabled(bool enabled) {
    if (settings_.idleEntryEnabled_ == enabled)
        return;
    settings_.idleEntryEnabled_ = enabled;
    runtime_log::writef(L"[action][idle-entry] enabled=%s", enabled ? L"on" : L"off");
    if (!enabled) {
        ++state_.idleQuoteRequestGeneration_;
        state_.idleQuoteRequestKey_.clear();
        state_.idleQuoteLoading_ = false;
    } else if (settings_.idleQuoteEnabled_) {
        refreshIdleQuote(false);
    }
    save_();
    publish_();
}

void IdleController::applyIdleQuoteSource(int source) {
    const IdleQuoteSource next = source == 1   ? IdleQuoteSource::Jinrishici
                                 : source == 2 ? IdleQuoteSource::Netease
                                               : IdleQuoteSource::Hitokoto;
    if (settings_.idleQuoteSource_ == next)
        return;
    settings_.idleQuoteSource_ = next;
    ++state_.idleQuoteRequestGeneration_;
    state_.idleQuoteRequestKey_.clear();
    state_.idleQuoteAttemptKey_.clear();
    state_.idleQuoteText_.clear();
    state_.idleQuoteOrigin_.clear();
    state_.idleQuoteUuid_.clear();
    state_.idleQuoteLoading_ = false;
    save_();
    refreshIdleQuote(false);
    publish_();
}

void IdleController::applyIdleQuoteRefreshInterval(int interval) {
    const IdleQuoteRefreshInterval next = interval == 2   ? IdleQuoteRefreshInterval::Hourly
                                          : interval == 1 ? IdleQuoteRefreshInterval::HalfDay
                                                          : IdleQuoteRefreshInterval::Daily;
    if (settings_.idleQuoteRefreshInterval_ == next)
        return;
    settings_.idleQuoteRefreshInterval_ = next;
    ++state_.idleQuoteRequestGeneration_;
    state_.idleQuoteRequestKey_.clear();
    state_.idleQuoteAttemptKey_.clear();
    state_.idleQuoteText_.clear();
    state_.idleQuoteOrigin_.clear();
    state_.idleQuoteUuid_.clear();
    state_.idleQuoteLoading_ = false;
    save_();
    refreshIdleQuote(false);
    publish_();
}

void IdleController::applyTickTickEnabled(bool enabled) {
    if (enabled) {
        if (state_.tickTickApiToken_.empty()) {
            return;
        }
        if (settings_.tickTickEnabled_)
            return;
        resetTickTickStartupRetry();
        settings_.tickTickEnabled_ = true;
        state_.tickTickStatus_ = L"滴答清单已开启，正在同步今日任务…";
        save_();
        refreshSettings_();
        publish_();
        refreshTickTickTasks();
        return;
    }

    ++state_.tickTickRequestGeneration_;
    resetTickTickStartupRetry();
    settings_.tickTickEnabled_ = false;
    state_.tickTickConnected_ = false;
    state_.tickTickConnecting_ = false;
    state_.tickTickTasksLoading_ = false;
    state_.tickTickCompletingTaskId_.clear();
    state_.todayTasks_.clear();
    state_.startupTaskSummaryPending_ = false;
    state_.startupTaskSummaryActive_ = false;
    state_.tickTickStatus_ = L"滴答清单已关闭";
    save_();
    refreshSettings_();
    publish_();
}

void IdleController::connectTickTick() {
    if (state_.tickTickConnecting_ || state_.tickTickTasksLoading_ ||
        !state_.tickTickCompletingTaskId_.empty())
        return;
    if (!tickTickEffectiveEnabled())
        return;

    state_.tickTickConnecting_ = true;
    refreshTickTickTasks();
}

void IdleController::cancelTickTickStartupRetry() {
    if (timerWindow_)
        KillTimer(timerWindow_, kTimerTickTickStartupRetry);
}

void IdleController::resetTickTickStartupRetry() {
    cancelTickTickStartupRetry();
    state_.tickTickStartupRetryAttempt_ = 0;
}

bool IdleController::scheduleTickTickStartupRetry() {
    if (!timerWindow_ || !state_.startupTaskSummaryPending_ || !tickTickEffectiveEnabled() ||
        state_.tickTickStartupRetryAttempt_ >= kTickTickStartupMaxRetries)
        return false;

    const int retryIndex = state_.tickTickStartupRetryAttempt_++;
    const UINT delayMs = kTickTickStartupRetryDelaysMs[retryIndex];
    if (!SetTimer(timerWindow_, kTimerTickTickStartupRetry, delayMs, nullptr)) {
        runtime_log::writef(L"[ticktick] startup retry timer failed attempt=%d delay-ms=%u",
                            retryIndex + 1, delayMs);
        return false;
    }
    runtime_log::writef(L"[ticktick] startup retry scheduled attempt=%d delay-ms=%u",
                        retryIndex + 1, delayMs);
    return true;
}

void IdleController::refreshTickTickTasks() {
    // 用户打开任务面板或手动点击刷新时，立即接管启动补试，不让旧的延迟消息再触发一次请求。
    cancelTickTickStartupRetry();
    if (!tickTickEffectiveEnabled() || state_.tickTickTasksLoading_ ||
        !state_.tickTickCompletingTaskId_.empty())
        return;

    ++state_.tickTickRequestGeneration_;
    const uint64_t generation = state_.tickTickRequestGeneration_;
    state_.tickTickTasksLoading_ = true;
    if (state_.tickTickConnecting_)
        state_.tickTickConnected_ = false;
    state_.tickTickStatus_ =
        state_.tickTickConnecting_ ? L"正在连接并同步今日任务…" : L"正在同步今日任务…";
    refreshSettings_();
    publish_();
    tickTickProvider_.requestTodayTasksAsync(
        state_.tickTickService_, state_.tickTickApiToken_,
        [this, generation](TickTickTasksResult result) {
            auto* payload = new TickTickTasksPayload{generation, std::move(result)};
            if (!PostThreadMessageW(mainThread_, kMsgTickTickTasksReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

void IdleController::completeTickTickTask(const IdleTaskInfo& task) {
    if (!tickTickEffectiveEnabled() || task.id.empty() || task.projectId.empty() ||
        !state_.tickTickCompletingTaskId_.empty())
        return;

    const auto it = std::find_if(state_.todayTasks_.begin(), state_.todayTasks_.end(),
                                 [&task](const IdleTaskInfo& item) { return item.id == task.id; });
    if (it == state_.todayTasks_.end())
        return;

    ++state_.tickTickRequestGeneration_;
    const uint64_t generation = state_.tickTickRequestGeneration_;
    state_.tickTickCompletingTaskId_ = task.id;
    state_.tickTickStatus_ = L"正在完成任务…";
    refreshSettings_();
    publish_();
    tickTickProvider_.completeTaskAsync(
        state_.tickTickService_, state_.tickTickApiToken_, task.projectId, task.id,
        [this, generation, taskId = task.id](TickTickTaskMutationResult result) {
            auto* payload = new TickTickTaskCompletePayload{generation, taskId, std::move(result)};
            if (!PostThreadMessageW(mainThread_, kMsgTickTickTaskCompleteReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

void IdleController::disconnectTickTick() {
    ++state_.tickTickRequestGeneration_;
    resetTickTickStartupRetry();
    tickTickProvider_.clearApiToken(state_.tickTickService_);
    state_.tickTickApiToken_.clear();
    settings_.tickTickEnabled_ = false;
    state_.tickTickConnected_ = false;
    state_.tickTickConnecting_ = false;
    state_.tickTickTasksLoading_ = false;
    state_.tickTickCompletingTaskId_.clear();
    state_.todayTasks_.clear();
    state_.tickTickStatus_ = L"已断开滴答清单连接";
    state_.startupTaskSummaryPending_ = false;
    state_.startupTaskSummaryActive_ = false;
    save_();
    refreshSettings_();
    publish_();
}

void IdleController::onTickTickTasksReady(std::unique_ptr<TickTickTasksPayload> payload) {
    if (!payload || payload->generation != state_.tickTickRequestGeneration_ ||
        !tickTickEffectiveEnabled())
        return;
    state_.tickTickTasksLoading_ = false;
    state_.tickTickConnecting_ = false;
    const bool sessionAlive = mediaAlive_();
    if (payload->result.ok) {
        resetTickTickStartupRetry();
        state_.tickTickConnected_ = true;
        state_.todayTasks_ = std::move(payload->result.tasks);
        state_.tickTickStatus_ = state_.todayTasks_.empty()
                                     ? L"已连接，今天没有待办任务"
                                     : std::wstring(L"已连接 · ") +
                                           std::to_wstring(state_.todayTasks_.size()) +
                                           L" 项今日任务";
    } else {
        state_.tickTickConnected_ =
            !payload->result.authRequired && !state_.tickTickApiToken_.empty();
        state_.todayTasks_.clear();
        state_.tickTickStatus_ =
            payload->result.error.empty() ? L"滴答清单任务同步失败" : payload->result.error;
    }
    if (state_.startupTaskSummaryPending_) {
        if (payload->result.ok) {
            state_.startupTaskSummaryPending_ = false;
            state_.startupTaskSummaryActive_ = !sessionAlive && settings_.idleEntryEnabled_;
        } else if (!payload->result.authRequired && !sessionAlive &&
                   scheduleTickTickStartupRetry()) {
            // 保持 loading 状态，直到补试成功或达到上限；任务面板会同时显示可读的失败状态。
            state_.startupTaskSummaryActive_ = false;
            state_.tickTickStatus_ = L"同步失败，稍后自动重试今日任务…";
        } else {
            state_.startupTaskSummaryPending_ = false;
            state_.startupTaskSummaryActive_ = false;
            resetTickTickStartupRetry();
        }
    } else if (state_.startupTaskSummaryActive_ && !payload->result.ok) {
        // 播报期间刷新失败时不继续播报一份已经清空的任务统计，直接回到普通空闲文案。
        state_.startupTaskSummaryActive_ = false;
    }
    refreshSettings_();
    publish_();
}

void IdleController::onTickTickTaskCompleteReady(
    std::unique_ptr<TickTickTaskCompletePayload> payload) {
    if (!payload || payload->generation != state_.tickTickRequestGeneration_ ||
        !tickTickEffectiveEnabled() || payload->taskId != state_.tickTickCompletingTaskId_)
        return;

    state_.tickTickCompletingTaskId_.clear();
    if (payload->result.ok) {
        const std::wstring completedId = payload->taskId;
        std::erase_if(state_.todayTasks_,
                      [&completedId](const IdleTaskInfo& item) { return item.id == completedId; });
        state_.tickTickConnected_ = true;
        state_.tickTickStatus_ = state_.todayTasks_.empty()
                                     ? L"已连接，今天没有待办任务"
                                     : std::wstring(L"已连接 · ") +
                                           std::to_wstring(state_.todayTasks_.size()) +
                                           L" 项今日任务";
    } else {
        state_.tickTickStatus_ =
            payload->result.error.empty() ? L"滴答清单任务完成失败" : payload->result.error;
        if (payload->result.authRequired)
            state_.tickTickConnected_ = false;
    }
    refreshSettings_();
    publish_();
}

void IdleController::onStartupTaskSummaryCompleted() {
    if (!state_.startupTaskSummaryActive_)
        return;
    state_.startupTaskSummaryActive_ = false;
    const bool sessionAlive = mediaAlive_();
    publish_();
}

void IdleController::openTickTickTask(const IdleTaskInfo& task) {
    if (!tickTickEffectiveEnabled())
        return;
    if (task.id.empty() || task.projectId.empty()) {
        const bool opened = platform_icon::launchUri(L"https://dida365.com/webapp/");
        runtime_log::writef(L"[action][ticktick] open-task missing-id target=web result=%s",
                            opened ? L"ok" : L"failed");
        return;
    }

    // 滴答清单网页版的任务链接由清单 ID 和任务 ID 唯一定位。
    const std::wstring taskPath = task.projectId + L"/tasks/" + task.id;
    const std::wstring webUrl = L"https://dida365.com/webapp/#p/" + taskPath;
    const bool opened = platform_icon::launchUri(webUrl);
    const wchar_t* target = opened ? L"web" : L"failed";

    runtime_log::writef(L"[action][ticktick] open-task id=%s project=%s target=%s result=%s",
                        task.id.c_str(), task.projectId.c_str(), target,
                        opened ? L"ok" : L"failed");
}
