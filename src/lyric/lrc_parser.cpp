#include "lyric/lrc_parser.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {

std::wstring toWide(const std::string& text) {
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

bool parseTimeStamp(const std::string& text, int64_t& milliseconds) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            return false;
    }

    char* end = nullptr;
    const double seconds = std::strtod(text.c_str() + colon + 1, &end);
    if (end == text.c_str() + colon + 1)
        return false;
    const long long minutes = std::strtoll(text.c_str(), nullptr, 10);
    milliseconds = static_cast<int64_t>((minutes * 60.0 + seconds) * 1000.0 + 0.5);
    return true;
}

} // namespace

bool parseLrcTimestamp(const std::string& text, int64_t& milliseconds) {
    return parseTimeStamp(text, milliseconds);
}

std::vector<LyricLine> parseLrcText(const std::string& content) {
    std::vector<LyricLine> lines;
    std::istringstream input(content);
    std::string raw;
    while (std::getline(input, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();

        std::vector<int64_t> timestamps;
        size_t position = 0;
        while (position < raw.size() && raw[position] == '[') {
            const size_t close = raw.find(']', position);
            if (close == std::string::npos)
                break;
            int64_t milliseconds = 0;
            if (!parseTimeStamp(raw.substr(position + 1, close - position - 1), milliseconds)) {
                timestamps.clear();
                break; // [ti:] 等元数据行
            }
            timestamps.push_back(milliseconds);
            position = close + 1;
        }
        if (timestamps.empty())
            continue;

        std::wstring text = toWide(raw.substr(position));
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            text.pop_back();
        const size_t first = text.find_first_not_of(L" \t");
        if (first == std::wstring::npos)
            continue;
        text = text.substr(first);
        for (const int64_t milliseconds : timestamps)
            lines.push_back({milliseconds, text});
    }

    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& left, const LyricLine& right) { return left.ms < right.ms; });
    return lines;
}
