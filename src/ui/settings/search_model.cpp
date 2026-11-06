#include "ui/settings/search_model.h"
#include <algorithm>
#include <cwctype>
#include <utility>

namespace {
void lowercase(std::wstring& text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
}
} // namespace

void SettingsSearchModel::clear() {
    query_.clear();
    results_.clear();
}

void SettingsSearchModel::refresh(std::wstring query,
                                  const std::vector<SettingsSearchEntry>& entries) {
    clear();
    lowercase(query);
    std::vector<std::wstring> terms;
    for (size_t begin = 0; begin < query.size();) {
        while (begin < query.size() && std::iswspace(query[begin]))
            ++begin;
        size_t end = begin;
        while (end < query.size() && !std::iswspace(query[end]))
            ++end;
        if (end > begin)
            terms.push_back(query.substr(begin, end - begin));
        begin = end;
    }
    if (terms.empty())
        return;
    query_ = std::move(query);
    for (const auto& entry : entries) {
        std::wstring text = entry.text;
        lowercase(text);
        if (std::all_of(terms.begin(), terms.end(), [&](const std::wstring& term) {
                return text.find(term) != std::wstring::npos;
            }))
            results_.push_back({entry.page, entry.id, entry.path});
    }
}
