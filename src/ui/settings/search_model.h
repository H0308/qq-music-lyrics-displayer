#pragma once
#include <string>
#include <vector>

struct SettingsSearchResult {
    int page;
    int id;
    std::wstring path;
};
struct SettingsSearchEntry {
    int page;
    int id;
    std::wstring path;
    std::wstring text;
};

class SettingsSearchModel {
public:
    void refresh(std::wstring query, const std::vector<SettingsSearchEntry>& entries);
    void clear();
    bool active() const { return !query_.empty(); }
    const std::vector<SettingsSearchResult>& results() const { return results_; }

private:
    std::wstring query_;
    std::vector<SettingsSearchResult> results_;
};
