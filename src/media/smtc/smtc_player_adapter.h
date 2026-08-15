#pragma once

#include "media/smtc/smtc_common.h"

#include <string>

namespace smtc {

struct SmtcSessionIdentity {
    SmtcPlayerType player = SmtcPlayerType::Unknown;
    std::wstring neteaseSongId;
    std::wstring sourceAppUserModelId;
    bool enhancedSmtc = false;
};

class SmtcPlayerAdapter {
public:
    virtual ~SmtcPlayerAdapter() = default;

    virtual SmtcPlayerType playerType() const noexcept = 0;
    virtual SmtcSessionIdentity identifySession(const Session& session) const = 0;

    // 对 refreshAll 读取的原始快照做播放器专属的初始锚点处理。
    virtual void prepareInitialSnapshot(SmtcSnapshot& snapshot) const = 0;

    // 调用方持有 SmtcMonitor::Impl::mtx；适配器不再自行管理会话锁。
    virtual void refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                                 int64_t eventNowMs) = 0;
    virtual void refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                                 int64_t eventNowMs) = 0;

    // 根据适配器自己的规则生成渲染侧快照。QQ 会在这里使用平滑状态，
    // 网易云则直接按自己的锚点插值。
    virtual SmtcSnapshot snapshot(const SmtcSnapshot& source,
                                  int64_t nowUtcMs) const = 0;

    // 切换当前会话时清理该适配器的内部进度状态。
    virtual void reset() = 0;
};

} // namespace smtc
