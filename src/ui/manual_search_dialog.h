#pragma once

#include "lyric/lyric_provider.h"

#include <windows.h>
#include <functional>
#include <memory>
#include <string>

// 手动搜索/选择歌词对话框
class ManualSearchDialog {
public:
    using ApplyCallback = std::function<void()>;

    ManualSearchDialog();
    ~ManualSearchDialog();

    ManualSearchDialog(const ManualSearchDialog&) = delete;
    ManualSearchDialog& operator=(const ManualSearchDialog&) = delete;

    // provider 必须比对话框生命周期长
    bool create(HINSTANCE inst, HWND parent, LyricProvider* provider,
                const std::wstring& targetTitle, const std::wstring& targetArtist);
    void show();
    void destroy();
    bool isOpen() const;
    HWND hwnd() const;

    // 由主线程在收到工作线程消息后调用
    void onCandidatesReady(const std::vector<SearchCandidate>& cands);
    void onPreviewLyricReady(int idx, bool ok, const std::vector<LyricLine>& lines,
                             const SongInfo& info);

    void setApplyCallback(ApplyCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
