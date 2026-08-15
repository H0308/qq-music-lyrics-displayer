#pragma once

#include "media/smtc/smtc_player_adapter.h"

namespace smtc {

class NeteaseSmtcAdapter final : public SmtcPlayerAdapter {
public:
    SmtcPlayerType playerType() const noexcept override;
    SmtcSessionIdentity identifySession(const Session& session) const override;
    void prepareInitialSnapshot(SmtcSnapshot& snapshot) const override;
    void refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                         int64_t eventNowMs) override;
    void refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                         int64_t eventNowMs) override;
    SmtcSnapshot snapshot(const SmtcSnapshot& source,
                          int64_t nowUtcMs) const override;
    void reset() override;

private:
    static bool parseSongId(const std::wstring& genre, std::wstring& songId);
};

} // namespace smtc
