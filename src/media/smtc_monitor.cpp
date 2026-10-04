#include "media/smtc_monitor.h"

#include "media/smtc/netease_smtc_adapter.h"
#include "media/smtc/qq_music_smtc_adapter.h"
#include "media/smtc/smtc_common.h"
#include "logging/runtime_logger.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

using Session = smtc::Session;
using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace {

bool processAlive(const wchar_t* executableName) {
    if (!executableName || !*executableName)
        return false;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executableName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool playerProcessAlive(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return processAlive(L"QQMusic.exe");
    case SmtcPlayerType::NetEase:
        return processAlive(L"cloudmusic.exe");
    default:
        return false;
    }
}

const wchar_t* playerName(SmtcPlayerType player) noexcept {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return L"QQMusic";
    case SmtcPlayerType::NetEase:
        return L"NetEase";
    default:
        return L"Unknown";
    }
}

const wchar_t* playbackStatusName(PlaybackStatus status) noexcept {
    switch (status) {
    case PlaybackStatus::Stopped:
        return L"Stopped";
    case PlaybackStatus::Playing:
        return L"Playing";
    case PlaybackStatus::Paused:
        return L"Paused";
    default:
        return L"Other";
    }
}

const wchar_t* asyncStatusName(winrt::Windows::Foundation::AsyncStatus status) noexcept {
    using AsyncStatus = winrt::Windows::Foundation::AsyncStatus;
    switch (status) {
    case AsyncStatus::Started:
        return L"Started";
    case AsyncStatus::Completed:
        return L"Completed";
    case AsyncStatus::Canceled:
        return L"Canceled";
    case AsyncStatus::Error:
        return L"Error";
    default:
        return L"Unknown";
    }
}

void writeSmtcControlLog(const std::wstring& message) noexcept {
    try {
        runtime_log::writef(L"[smtc-control] %s", message.c_str());
    } catch (...) {
    }
}

std::wstring sessionSourceAppUserModelId(const Session& session) {
    if (!session)
        return L"<none>";
    try {
        return session.SourceAppUserModelId().c_str();
    } catch (...) {
        return L"<unavailable>";
    }
}

} // namespace

struct SmtcMonitor::Impl {
    struct RefreshSignal {
        std::mutex mtx;
        std::condition_variable cv;
        bool stopping = false;
        bool requested = false;
        bool rebuildWatchers = false;
    };

    struct ThumbnailRequest {
        GlobalSystemMediaTransportControlsSessionMediaProperties props{ nullptr };
        uint64_t generation = 0;
        uint64_t sequence = 0;
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        std::wstring source;
    };
    struct SessionWatcher {
        Session session{ nullptr };
        Session::PlaybackInfoChanged_revoker playbackRevoker;
        Session::MediaPropertiesChanged_revoker propsRevoker;
    };

    GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
    Session session{ nullptr };

    mutable std::mutex mtx;
    // 会话选择、属性和进度适配只由刷新线程执行；封面线程仅提交图片字节。
    SmtcSnapshot snap;
    SmtcMonitor::ChangeCallback onChange;

    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessionsRevoker;
    GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker
        currentSessionRevoker;
    Session::TimelinePropertiesChanged_revoker timelineRevoker;

    // 所有媒体会话都保留轻量监听，避免只监听当前选中会话导致切换丢失。
    std::vector<SessionWatcher> sessionWatchers;

    // 新播放器只需实现一个适配器并在这里注册，监控器本身不再增加播放器分支。
    std::vector<std::unique_ptr<smtc::SmtcPlayerAdapter>> adapters;

    std::shared_ptr<RefreshSignal> refreshSignal = std::make_shared<RefreshSignal>();
    std::thread pollThread;
    std::mutex thumbnailMtx;
    std::condition_variable thumbnailCv;
    bool thumbnailStopping = false;
    ThumbnailRequest thumbnailRequest;
    std::thread thumbnailThread;
    uint64_t thumbnailGeneration = 0;
    bool thumbnailPending = false;
    int64_t thumbnailRetryAtMs = 0;
    uint64_t thumbnailReadSequence = 0;
    uint64_t thumbnailDiagnosticsGeneration = 0;
    int thumbnailReferenceState = -1;
    int64_t sampledPositionMs = 0;
    int64_t sampledDurationMs = 0;
    int64_t sampledAnchorMs = 0;

    // Windows 在其他 SMTC 应用成为当前会话时，可能从枚举结果中漏掉
    // 已暂停的音乐会话。只要该会话所属的音乐播放器仍在运行，就保留暂停快照；
    // 播放器退出后再等待一个稳定窗口，避免生命周期边界的瞬时空列表。
    std::chrono::steady_clock::time_point sessionLossCandidateSince;
    static constexpr auto kSessionLossStabilityWindow = std::chrono::milliseconds(1500);

    Impl() {
        adapters.emplace_back(std::make_unique<smtc::QqMusicSmtcAdapter>());
        adapters.emplace_back(std::make_unique<smtc::NeteaseSmtcAdapter>());
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(refreshSignal->mtx);
            refreshSignal->stopping = true;
        }
        refreshSignal->cv.notify_all();
        if (pollThread.joinable())
            pollThread.join();
        timelineRevoker.revoke();
        currentSessionRevoker.revoke();
        sessionsRevoker.revoke();
        sessionWatchers.clear();
        {
            std::lock_guard<std::mutex> lk(thumbnailMtx);
            thumbnailStopping = true;
            thumbnailRequest = {};
        }
        thumbnailCv.notify_all();
        if (thumbnailThread.joinable())
            thumbnailThread.join();
    }

    static void requestRefresh(const std::shared_ptr<RefreshSignal>& signal,
                               bool rebuildWatchers = false) {
        {
            std::lock_guard<std::mutex> lk(signal->mtx);
            if (signal->stopping)
                return;
            signal->requested = true;
            signal->rebuildWatchers = signal->rebuildWatchers || rebuildWatchers;
        }
        signal->cv.notify_one();
    }

    smtc::SmtcPlayerAdapter* adapterFor(SmtcPlayerType player) const {
        for (const auto& adapter : adapters) {
            if (adapter && adapter->playerType() == player)
                return adapter.get();
        }
        return nullptr;
    }

    std::wstring thumbCacheKey_;
    std::shared_ptr<const std::vector<uint8_t>> thumbCache_;

    static std::wstring thumbnailKey(const SmtcSnapshot& snapshot) {
        return snapshot.sourceAppUserModelId + L'\x1f' + snapshot.neteaseSongId +
               L'\x1f' + snapshot.title + L'\x1f' + snapshot.artist +
               L'\x1f' + snapshot.album;
    }

    void startThumbnailReader() {
        thumbnailThread = std::thread([this] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (const winrt::hresult_error& error) {
                runtime_log::writef(L"[cover][smtc] reader-init-failed hresult=0x%08X reason=\"%s\"",
                                   static_cast<unsigned int>(error.code().value), error.message().c_str());
                return;
            } catch (...) {
                runtime_log::writef(L"[cover][smtc] reader-init-failed reason=unknown");
                return;
            }
            for (;;) {
                ThumbnailRequest request;
                {
                    std::unique_lock<std::mutex> lk(thumbnailMtx);
                    thumbnailCv.wait(lk, [this] {
                        return thumbnailStopping || thumbnailRequest.props;
                    });
                    if (thumbnailStopping)
                        break;
                    request = std::move(thumbnailRequest);
                    thumbnailRequest = {};
                }
                runtime_log::writef(
                    L"[cover][smtc] read-started sequence=%llu generation=%llu source=\"%s\" "
                    L"title=\"%s\" artist=\"%s\" album=\"%s\"",
                    static_cast<unsigned long long>(request.sequence),
                    static_cast<unsigned long long>(request.generation), request.source.c_str(),
                    request.title.c_str(), request.artist.c_str(), request.album.c_str());
                auto thumbnail = smtc::readThumbnail(request.props, request.sequence);
                const size_t bytes = thumbnail ? thumbnail->size() : 0;
                bool changed = false;
                uint64_t currentGeneration = 0;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    currentGeneration = thumbnailGeneration;
                    if (request.generation == thumbnailGeneration) {
                        thumbnailPending = false;
                        thumbnailRetryAtMs = smtc::nowUtcMs() + 1000;
                        if (thumbnail) {
                            thumbCache_ = std::move(thumbnail);
                            snap.thumbnail = thumbCache_;
                            changed = true;
                        }
                    }
                }
                if (request.generation != currentGeneration) {
                    runtime_log::writef(
                        L"[cover][smtc] read-discarded sequence=%llu generation=%llu current-generation=%llu "
                        L"title=\"%s\" bytes=%zu reason=track-or-session-changed",
                        static_cast<unsigned long long>(request.sequence),
                        static_cast<unsigned long long>(request.generation),
                        static_cast<unsigned long long>(currentGeneration), request.title.c_str(), bytes);
                } else {
                    runtime_log::writef(
                        L"[cover][smtc] %s sequence=%llu generation=%llu title=\"%s\" bytes=%zu retry-after=%dms",
                        changed ? L"read-committed" : L"retry-scheduled",
                        static_cast<unsigned long long>(request.sequence),
                        static_cast<unsigned long long>(request.generation), request.title.c_str(),
                        bytes, changed ? 0 : 1000);
                }
                if (changed)
                    notify();
            }
            winrt::uninit_apartment();
        });
    }

    // 调用方持有快照锁；图片流读取独立于歌曲信息和播放状态的提交。
    void requestThumbnail(
        const GlobalSystemMediaTransportControlsSessionMediaProperties& props) {
        if (!props || thumbCache_ || thumbnailPending ||
            smtc::nowUtcMs() < thumbnailRetryAtMs)
            return;
        const bool hasThumbnail = static_cast<bool>(props.Thumbnail());
        if (thumbnailDiagnosticsGeneration != thumbnailGeneration ||
            thumbnailReferenceState != static_cast<int>(hasThumbnail)) {
            thumbnailDiagnosticsGeneration = thumbnailGeneration;
            thumbnailReferenceState = static_cast<int>(hasThumbnail);
            runtime_log::writef(
                L"[cover][smtc] thumbnail-%s generation=%llu source=\"%s\" title=\"%s\" artist=\"%s\" album=\"%s\"",
                hasThumbnail ? L"provided" : L"missing",
                static_cast<unsigned long long>(thumbnailGeneration), snap.sourceAppUserModelId.c_str(),
                snap.title.c_str(), snap.artist.c_str(), snap.album.c_str());
        }
        if (!hasThumbnail)
            return;
        const uint64_t sequence = ++thumbnailReadSequence;
        {
            std::lock_guard<std::mutex> lk(thumbnailMtx);
            if (thumbnailRequest.props) {
                runtime_log::writef(
                    L"[cover][smtc] read-discarded sequence=%llu generation=%llu title=\"%s\" reason=request-replaced-before-read",
                    static_cast<unsigned long long>(thumbnailRequest.sequence),
                    static_cast<unsigned long long>(thumbnailRequest.generation), thumbnailRequest.title.c_str());
            }
            thumbnailRequest = { props, thumbnailGeneration, sequence,
                                 snap.title, snap.artist, snap.album, snap.sourceAppUserModelId };
            thumbnailPending = true;
            runtime_log::writef(L"[cover][smtc] read-queued sequence=%llu generation=%llu title=\"%s\"",
                               static_cast<unsigned long long>(sequence),
                               static_cast<unsigned long long>(thumbnailGeneration), snap.title.c_str());
        }
        thumbnailCv.notify_one();
    }

    smtc::SmtcSessionIdentity identifySession(const Session& candidate) const {
        for (const auto& adapter : adapters) {
            if (!adapter)
                continue;
            auto identity = adapter->identifySession(candidate);
            if (identity.player != SmtcPlayerType::Unknown)
                return identity;
        }
        return {};
    }

    void notify() {
        if (onChange)
            onChange();
    }

    // 控制按钮运行在任务栏窗口线程上，不能同步等待远程 WinRT 操作。
    // 完成回调只记录异步结果，不阻塞任务栏消息循环。
    void logControlSession(const wchar_t* action, const wchar_t* api,
                           const Session& target, const SmtcSnapshot& snapshot) {
        Session systemCurrent{ nullptr };
        int targetIndex = -1;
        int currentIndex = -1;
        int sessionCount = -1;
        const wchar_t* managerQuery = manager ? L"ok" : L"unavailable";

        if (manager) {
            try {
                systemCurrent = manager.GetCurrentSession();
                auto sessions = manager.GetSessions();
                if (sessions) {
                    sessionCount = static_cast<int>(sessions.Size());
                    for (uint32_t i = 0; i < sessions.Size(); ++i) {
                        const Session candidate = sessions.GetAt(i);
                        if (target && targetIndex < 0 && smtc::sameSession(target, candidate))
                            targetIndex = static_cast<int>(i);
                        if (systemCurrent && currentIndex < 0 &&
                            smtc::sameSession(systemCurrent, candidate))
                            currentIndex = static_cast<int>(i);
                    }
                }
            } catch (...) {
                managerQuery = L"error";
            }
        }

        std::wostringstream line;
        line << L"phase=dispatch action=" << action << L" api=" << api
             << L" player=" << playerName(snapshot.player)
             << L" selectedSource=" << sessionSourceAppUserModelId(target)
             << L" selectedListed=" << (targetIndex >= 0 ? 1 : 0)
             << L" selectedIndex=" << targetIndex
             << L" cachedStatus=" << playbackStatusName(snapshot.status)
             << L" controls(prev=" << (snapshot.canPrev ? 1 : 0)
             << L",playPause=" << (snapshot.canPlayPause ? 1 : 0)
             << L",next=" << (snapshot.canNext ? 1 : 0) << L")"
             << L" systemCurrentSource=" << sessionSourceAppUserModelId(systemCurrent)
             << L" currentListed=" << (currentIndex >= 0 ? 1 : 0)
             << L" currentIndex=" << currentIndex
             << L" sameSession=" << (target && systemCurrent &&
                                           smtc::sameSession(target, systemCurrent) ? 1 : 0)
             << L" sessionCount=" << sessionCount << L" managerQuery=" << managerQuery;
        writeSmtcControlLog(line.str());
    }

    template <typename StartOperation>
    void startControlOperation(const wchar_t* action, const wchar_t* api,
                               const Session& target, const SmtcSnapshot& snapshot,
                               StartOperation&& start) {
        try {
            logControlSession(action, api, target, snapshot);
        } catch (...) {
            // 诊断日志异常不能阻止实际控制请求。
        }
        if (!target) {
            std::wostringstream line;
            line << L"phase=operation-skipped action=" << action << L" api=" << api
                 << L" reason=no-selected-session";
            writeSmtcControlLog(line.str());
            return;
        }

        try {
            auto operation = start();
            if (!operation) {
                std::wostringstream line;
                line << L"phase=operation-start action=" << action << L" api=" << api
                     << L" result=no-operation";
                writeSmtcControlLog(line.str());
                return;
            }

            {
                std::wostringstream line;
                line << L"phase=operation-start action=" << action << L" api=" << api
                     << L" result=started";
                writeSmtcControlLog(line.str());
            }

            const std::wstring actionCopy(action);
            const std::wstring apiCopy(api);
            operation.Completed([actionCopy, apiCopy, signal = refreshSignal](
                                    auto&& completedOperation, auto status) {
                using AsyncStatus = winrt::Windows::Foundation::AsyncStatus;
                if (status != AsyncStatus::Completed) {
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus="
                         << asyncStatusName(status) << L"(" << static_cast<int>(status) << L")";
                    writeSmtcControlLog(line.str());
                    return;
                }

                try {
                    const bool succeeded = completedOperation.GetResults();
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus=Completed result="
                         << (succeeded ? L"true" : L"false");
                    writeSmtcControlLog(line.str());
                    if (succeeded)
                        requestRefresh(signal);
                } catch (const winrt::hresult_error& error) {
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus=Completed result=exception hresult=0x"
                         << std::hex << static_cast<uint32_t>(error.code());
                    writeSmtcControlLog(line.str());
                } catch (...) {
                    std::wostringstream line;
                    line << L"phase=operation-complete action=" << actionCopy
                         << L" api=" << apiCopy << L" asyncStatus=Completed result=unknown-exception";
                    writeSmtcControlLog(line.str());
                }
            });
        } catch (const winrt::hresult_error& error) {
            std::wostringstream line;
            line << L"phase=operation-start action=" << action << L" api=" << api
                 << L" result=exception hresult=0x" << std::hex
                 << static_cast<uint32_t>(error.code());
            writeSmtcControlLog(line.str());
        } catch (...) {
            std::wostringstream line;
            line << L"phase=operation-start action=" << action << L" api=" << api
                 << L" result=unknown-exception";
            writeSmtcControlLog(line.str());
        }
    }

    void refreshAll() {
        Session currentSession{ nullptr };
        SmtcSnapshot previous;
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
            previous = snap;
        }
        if (!currentSession)
            return;

        bool playbackChanged = false;
        try {
            auto info = currentSession.GetPlaybackInfo();
            std::lock_guard<std::mutex> lk(mtx);
            if (info && snap.sessionAlive) {
                if (smtc::mapStatus(info.PlaybackStatus()) != snap.status) {
                    try {
                        if (auto* adapter = adapterFor(snap.player))
                            adapter->refreshPlayback(currentSession, snap, smtc::nowUtcMs());
                    } catch (...) {
                    }
                    snap.status = smtc::mapStatus(info.PlaybackStatus());
                }
                smtc::applyPlaybackControls(info, snap);
                playbackChanged = snap.status != previous.status ||
                                  snap.canPrev != previous.canPrev ||
                                  snap.canPlayPause != previous.canPlayPause ||
                                  snap.canNext != previous.canNext;
            }
            previous = snap;
        } catch (...) {
        }
        if (playbackChanged)
            notify();

        SmtcSnapshot next = previous;
        if (!next.sessionAlive) {
            const auto identity = identifySession(currentSession);
            next.player = identity.player;
            next.neteaseSongId = identity.neteaseSongId;
            next.sourceAppUserModelId = identity.sourceAppUserModelId;
            next.enhancedSmtc = identity.enhancedSmtc;
            next.sessionAlive = identity.player != SmtcPlayerType::Unknown;
        }
        auto* adapter = adapterFor(next.player);
        if (!adapter)
            return;

        GlobalSystemMediaTransportControlsSessionMediaProperties props{ nullptr };
        try {
            auto operation = currentSession.TryGetMediaPropertiesAsync();
            if (operation.wait_for(std::chrono::seconds(1)) ==
                winrt::Windows::Foundation::AsyncStatus::Completed)
                props = operation.GetResults();
            else
                operation.Cancel();
            if (props) {
                next.title = props.Title().c_str();
                next.artist = props.Artist().c_str();
                next.album = props.AlbumTitle().c_str();
                if (next.player == SmtcPlayerType::NetEase) {
                    for (const auto& genre : props.Genres()) {
                        const std::wstring value = genre.c_str();
                        if (value.starts_with(L"NCM-") && value.size() > 4 &&
                            value.find_first_not_of(L"0123456789", 4) == std::wstring::npos) {
                            next.neteaseSongId = value.substr(4);
                            break;
                        }
                    }
                }
            }
        } catch (...) {
        }

        const bool trackChanged = !previous.sessionAlive ||
                                  next.title != previous.title ||
                                  next.artist != previous.artist ||
                                  next.neteaseSongId != previous.neteaseSongId;
        try {
            auto timeline = currentSession.GetTimelineProperties();
            auto info = currentSession.GetPlaybackInfo();
            const int64_t now = smtc::nowUtcMs();
            const int64_t position = smtc::timeSpanMs(timeline.Position());
            const int64_t duration = smtc::timeSpanMs(timeline.EndTime());
            const int64_t anchor = smtc::lastUpdatedMs(timeline.LastUpdatedTime());
            std::lock_guard<std::mutex> lk(mtx);
            if (trackChanged) {
                next.positionMs = position;
                next.durationMs = duration;
                next.anchorUtcMs = anchor > 0 ? anchor : now;
                next.timelineStale = false;
                adapter->prepareInitialSnapshot(next);
                next.status = smtc::mapStatus(info.PlaybackStatus());
            } else {
                if (smtc::mapStatus(info.PlaybackStatus()) != next.status)
                    adapter->refreshPlayback(currentSession, next, now);
                if (next.timelineStale || position != sampledPositionMs ||
                    duration != sampledDurationMs || anchor != sampledAnchorMs)
                    adapter->refreshTimeline(currentSession, next, now);
            }
            smtc::applyPlaybackControls(info, next);
            sampledPositionMs = position;
            sampledDurationMs = duration;
            sampledAnchorMs = anchor;
        } catch (...) {
        }

        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            const std::wstring key = thumbnailKey(next);
            if (key != thumbCacheKey_) {
                thumbCacheKey_ = key;
                thumbCache_.reset();
                ++thumbnailGeneration;
                thumbnailPending = false;
                thumbnailRetryAtMs = 0;
            }
            next.thumbnail = thumbCache_;
            changed = next.sessionAlive != snap.sessionAlive ||
                      next.player != snap.player || next.title != snap.title ||
                      next.artist != snap.artist || next.album != snap.album ||
                      next.neteaseSongId != snap.neteaseSongId ||
                      next.status != snap.status || next.durationMs != snap.durationMs ||
                      next.positionMs != snap.positionMs || next.anchorUtcMs != snap.anchorUtcMs ||
                      next.timelineStale != snap.timelineStale ||
                      next.thumbnail != snap.thumbnail || next.canPrev != snap.canPrev ||
                      next.canPlayPause != snap.canPlayPause || next.canNext != snap.canNext;
            snap = std::move(next);
            try {
                requestThumbnail(props);
            } catch (const winrt::hresult_error& error) {
                if (thumbnailDiagnosticsGeneration != thumbnailGeneration || thumbnailReferenceState != -2) {
                    thumbnailDiagnosticsGeneration = thumbnailGeneration;
                    thumbnailReferenceState = -2;
                    runtime_log::writef(
                        L"[cover][smtc] request-failed generation=%llu title=\"%s\" hresult=0x%08X reason=\"%s\"",
                        static_cast<unsigned long long>(thumbnailGeneration), snap.title.c_str(),
                        static_cast<unsigned int>(error.code().value), error.message().c_str());
                }
            } catch (...) {
                if (thumbnailDiagnosticsGeneration != thumbnailGeneration || thumbnailReferenceState != -2) {
                    thumbnailDiagnosticsGeneration = thumbnailGeneration;
                    thumbnailReferenceState = -2;
                    runtime_log::writef(L"[cover][smtc] request-failed generation=%llu title=\"%s\" reason=unknown",
                                       static_cast<unsigned long long>(thumbnailGeneration), snap.title.c_str());
                }
            }
        }
        if (changed)
            notify();
    }

    static bool isResponsivePlaybackStatus(
        GlobalSystemMediaTransportControlsSessionPlaybackStatus status) noexcept {
        using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
        // QQ 音乐切歌时会先短暂上报 Stopped，再提交下一首的媒体属性。
        // 只要 PlaybackInfo 仍可读取，Stopped 表示会话仍在，而不是会话已撤销。
        return status == Status::Opened || status == Status::Changing ||
               status == Status::Playing || status == Status::Paused ||
               status == Status::Stopped;
    }

    bool isResponsiveSession(const Session& candidate) const {
        if (!candidate)
            return false;
        try {
            auto info = candidate.GetPlaybackInfo();
            return info && isResponsivePlaybackStatus(info.PlaybackStatus());
        } catch (...) {
            return false;
        }
    }

    void attach(const Session& selected) {
        try {
            timelineRevoker.revoke();
        } catch (...) {
            // 旧会话刚结束时撤销事件也可能返回 HRESULT；旧会话已经失效，
            // 不能让异常穿过 SMTC 事件回调。
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            session = selected;
            snap = {};
            thumbCacheKey_.clear();
            thumbCache_.reset();
            ++thumbnailGeneration;
            thumbnailPending = false;
            thumbnailRetryAtMs = 0;
            sampledPositionMs = 0;
            sampledDurationMs = 0;
            sampledAnchorMs = 0;
            for (auto& adapter : adapters) {
                if (adapter)
                    adapter->reset();
            }
        }
        if (!selected) {
            notify();
            return;
        }

        try {
            timelineRevoker = selected.TimelinePropertiesChanged(winrt::auto_revoke,
                [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal); });
        } catch (...) {
            // 会话可能在绑定监听前已经被播放器撤销；下一次系统事件或后台健康检查
            // 会重新尝试绑定。
        }
    }

    void watchSessions(const auto& sessions) {
        sessionWatchers.clear();
        for (auto const& watched : sessions) {
            if (!watched)
                continue;
            try {
                SessionWatcher watcher;
                watcher.session = watched;
                watcher.playbackRevoker = watched.PlaybackInfoChanged(winrt::auto_revoke,
                    [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal); });
                watcher.propsRevoker = watched.MediaPropertiesChanged(winrt::auto_revoke,
                    [signal = refreshSignal](auto&&, auto&&) { requestRefresh(signal); });
                sessionWatchers.push_back(std::move(watcher));
            } catch (...) {
                // 单个已失效会话绑定失败不应阻断其他会话的监听。
            }
        }
    }

    void updateSession(bool rebuildWatchers = false) {
        Session found{ nullptr };
        Session selectedSession{ nullptr };
        bool selectedSessionAlive = false;
        SmtcPlayerType selectedPlayer = SmtcPlayerType::Unknown;
        {
            std::lock_guard<std::mutex> lk(mtx);
            selectedSession = session;
            selectedSessionAlive = snap.sessionAlive;
            selectedPlayer = snap.player;
        }
        if (manager) {
            try {
                // Windows 已经维护了“用户当前最可能想控制”的媒体会话。
                auto current = manager.GetCurrentSession();
                auto sessions = manager.GetSessions();
                // GetCurrentSession() 在播放器退出的边界上可能短暂返回已经断开的
                // 旧会话。先用会话列表确认它仍然存在，再读取媒体属性，避免对
                // 已失效的网易云会话继续调用 TryGetMediaPropertiesAsync()。
                bool currentListed = false;
                if (current && sessions) {
                    for (auto const& candidate : sessions) {
                        if (smtc::sameSession(current, candidate)) {
                            currentListed = true;
                            break;
                        }
                    }
                }

                smtc::SmtcSessionIdentity currentIdentity;
                bool currentSessionResponsive = false;
                if (current && currentListed) {
                    currentIdentity = identifySession(current);
                    if (currentIdentity.player != SmtcPlayerType::Unknown) {
                        try {
                            auto info = current.GetPlaybackInfo();
                            if (info) {
                                const auto status = info.PlaybackStatus();
                                currentSessionResponsive = isResponsivePlaybackStatus(status);
                                if (smtc::mapStatus(status) == PlaybackStatus::Playing)
                                    found = current;
                            }
                        } catch (...) {
                        }
                    }
                }

                if (sessions) {
                    if (rebuildWatchers)
                        watchSessions(sessions);
                    if (!found) {
                        // 当前会话可能暂停，而另一个受支持会话正在播放。
                        for (auto const& candidate : sessions) {
                            const auto identity = identifySession(candidate);
                            if (identity.player == SmtcPlayerType::Unknown)
                                continue;
                            try {
                                auto info = candidate.GetPlaybackInfo();
                                if (info &&
                                    smtc::mapStatus(info.PlaybackStatus()) == PlaybackStatus::Playing) {
                                    found = candidate;
                                    break;
                                }
                            } catch (...) {
                            }
                        }
                    }
                }

                // 暂停或停止不是切换理由：已选中的会话仍存在时保持选中。否则网易云暂停后，
                // Windows 当前会话若指向另一个暂停的播放器（如 QQ 音乐），显示会跳走，
                // 后续控制按钮也会作用到错误的会话上。
                if (!found && selectedSessionAlive && sessions) {
                    for (auto const& candidate : sessions) {
                        if (smtc::sameSession(selectedSession, candidate) &&
                            isResponsiveSession(candidate)) {
                            found = candidate;
                            break;
                        }
                    }
                }

                if (!found && currentListed && currentSessionResponsive &&
                    currentIdentity.player != SmtcPlayerType::Unknown)
                    found = current;

                // 网易云暂停/恢复时可能短暂上报 Changing/Opened 状态。适配器会把这类
                // 非稳定状态映射为 Other，识别结果暂时变成 Unknown；但只要仍是此前
                // 已选中的同一会话，就不能在这个过渡窗口把会话解绑，否则主程序会先
                // 收到空快照并隐藏任务栏窗口，下一条 Playing 事件到达时再整体显示。
                // 保留条件必须仅限 Opened/Changing 这种真正的过渡态：Playing/Paused
                // 是稳定态，此时识别失败只可能是属性 RPC 已随播放器退出而失败
                // （RPC_E_SERVERUNAVAILABLE）。若把这类已失效会话保留下来，
                // sameSession 判等会让下面跳过 attach/refreshAll，且死会话不会再
                // 产生任何事件，歌词就永久停在网易云退出的前一刻。
                if (!found && selectedSessionAlive && currentListed &&
                    smtc::sameSession(selectedSession, current) && current) {
                    bool transient = false;
                    try {
                        auto info = current.GetPlaybackInfo();
                        if (info) {
                            const auto status = info.PlaybackStatus();
                            transient =
                                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened ||
                                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing;
                        }
                    } catch (...) {
                        // 会话已经无法读取时不能继续保留旧会话；否则网易云退出后
                        // 会一直显示退出前的最后一帧歌词。
                    }
                    if (transient)
                        found = current;
                }

                // 已经有选中会话时，如果它在退出/切换边界上失效，不能再从
                // 会话列表中随便挑一个同播放器的暂停会话把旧状态续回来；否则
                // 网易云退出后会重新选中旧会话，歌词就会停在最后一刻。只有在
                // 尚未选中过任何会话时，才用已识别的会话作为初始候选。
                if (!found && !selectedSessionAlive && sessions) {
                    for (auto const& candidate : sessions) {
                        if (identifySession(candidate).player != SmtcPlayerType::Unknown) {
                            found = candidate;
                            break;
                        }
                    }
                }
            } catch (...) {
            }
        }

        if (!found && selectedSessionAlive && selectedSession) {
            if (playerProcessAlive(selectedPlayer)) {
                sessionLossCandidateSince = {};
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (sessionLossCandidateSince == std::chrono::steady_clock::time_point{})
                sessionLossCandidateSince = now;
            if (now - sessionLossCandidateSince < kSessionLossStabilityWindow)
                return;
            sessionLossCandidateSince = {};
        } else {
            sessionLossCandidateSince = {};
        }

        Session currentSession{ nullptr };
        {
            std::lock_guard<std::mutex> lk(mtx);
            currentSession = session;
        }
        if (!smtc::sameSession(currentSession, found)) {
            try {
                attach(found);
            } catch (...) {
                // 会话切换与属性读取发生在播放器生命周期边界，失败时保持
                // 当前快照，等待下一次系统媒体会话事件或后台健康检查重试。
            }
        }
    }

    void startPolling() {
        pollThread = std::thread([this] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
                return;
            }
            for (;;) {
                bool rebuildWatchers = false;
                {
                    std::unique_lock<std::mutex> lk(refreshSignal->mtx);
                    refreshSignal->cv.wait_for(lk, std::chrono::milliseconds(100), [this] {
                        return refreshSignal->stopping || refreshSignal->requested;
                    });
                    if (refreshSignal->stopping)
                        break;
                    rebuildWatchers = refreshSignal->rebuildWatchers;
                    refreshSignal->requested = false;
                    refreshSignal->rebuildWatchers = false;
                }
                try {
                    updateSession(rebuildWatchers);
                    refreshAll();
                } catch (...) {
                }
            }
            winrt::uninit_apartment();
        });
    }
};

SmtcMonitor::SmtcMonitor() : impl_(std::make_unique<Impl>()) {}
SmtcMonitor::~SmtcMonitor() = default;

void SmtcMonitor::start(ChangeCallback onChange) {
    writeSmtcControlLog(L"phase=monitor-start");
    impl_->onChange = std::move(onChange);
    try {
        auto managerOp = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        if (!managerOp)
            return;
        impl_->manager = managerOp.get();
        if (!impl_->manager)
            return;
        impl_->sessionsRevoker = impl_->manager.SessionsChanged(winrt::auto_revoke,
            [signal = impl_->refreshSignal](auto&&, auto&&) {
                Impl::requestRefresh(signal, true);
            });
        impl_->currentSessionRevoker = impl_->manager.CurrentSessionChanged(winrt::auto_revoke,
            [signal = impl_->refreshSignal](auto&&, auto&&) {
                Impl::requestRefresh(signal);
            });
        impl_->startThumbnailReader();
        Impl::requestRefresh(impl_->refreshSignal, true);
        impl_->startPolling();
    } catch (...) {
        // 没有可用媒体会话或系统媒体控制能力时，监控器保持空快照，
        // 不让启动阶段的 HRESULT 终止主程序。
        impl_->manager = nullptr;
    }
}

SmtcSnapshot SmtcMonitor::snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SmtcSnapshot source = impl_->snap;
    if (!source.sessionAlive)
        return source;
    if (auto* adapter = impl_->adapterFor(source.player))
        return adapter->snapshot(source, smtc::nowUtcMs());
    return source;
}

void SmtcMonitor::playPause() {
    Session current{ nullptr };
    SmtcSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
        snapshot = impl_->snap;
    }
    impl_->startControlOperation(L"play-pause", L"TryTogglePlayPauseAsync", current, snapshot,
                                 [current] { return current.TryTogglePlayPauseAsync(); });
}

void SmtcMonitor::skipNext() {
    Session current{ nullptr };
    SmtcSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
        snapshot = impl_->snap;
    }
    impl_->startControlOperation(L"next", L"TrySkipNextAsync", current, snapshot,
                                 [current] { return current.TrySkipNextAsync(); });
}

void SmtcMonitor::skipPrevious() {
    Session current{ nullptr };
    SmtcSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        current = impl_->session;
        snapshot = impl_->snap;
    }
    impl_->startControlOperation(L"previous", L"TrySkipPreviousAsync", current, snapshot,
                                 [current] { return current.TrySkipPreviousAsync(); });
}
