#include "qrc_decoder.h"
#include "qrc_des.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

#include <windows.h>
#include <zlib.h>

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& text, std::vector<unsigned char>& out) {
    std::string hex;
    hex.reserve(text.size());
    for (char ch : text)
        if (!std::isspace(static_cast<unsigned char>(ch))) hex.push_back(ch);
    if (hex.empty() || hex.size() % 16 != 0) {
        return false; // 3DES ECB 密文必须是 8 字节块
    }
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = hexValue(hex[i]);
        int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

bool tripleDesDecrypt(const std::vector<unsigned char>& encrypted,
                      std::vector<unsigned char>& decrypted) {
    return decryptQrcDes(encrypted, decrypted);
}

bool inflateQrc(const std::vector<unsigned char>& compressed, std::string& out) {
    z_stream stream{};
    int initResult = inflateInit(&stream);
    if (initResult != Z_OK) {
        return false;
    }
    stream.next_in = const_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    char buffer[16384];
    out.clear();
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            return false;
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    }
    inflateEnd(&stream);
    return result == Z_STREAM_END && !out.empty();
}

// QQ 客户端本地 QRC 外层使用固定 8x7 掩码表，并在每个 0x8000 字节边界
// 跳过一个掩码位置；解掩码后首行通常为 [offset:0]。
std::vector<unsigned char> unmaskLocalQmc(const std::vector<unsigned char>& input) {
    constexpr unsigned char kMaskSeed[8][7] = {
        {0x4a, 0xd6, 0xca, 0x90, 0x67, 0xf7, 0x52},
        {0x5e, 0x95, 0x23, 0x9f, 0x13, 0x11, 0x7e},
        {0x47, 0x74, 0x3d, 0x90, 0xaa, 0x3f, 0x51},
        {0xc6, 0x09, 0xd5, 0x9f, 0xfa, 0x66, 0xf9},
        {0xf3, 0xd6, 0xa1, 0x90, 0xa0, 0xf7, 0xf0},
        {0x1d, 0x95, 0xde, 0x9f, 0x84, 0x11, 0xf4},
        {0x0e, 0x74, 0xbb, 0x90, 0xbc, 0x3f, 0x92},
        {0x00, 0x09, 0x5b, 0x9f, 0x62, 0x66, 0xa1},
    };

    int x = -1;
    int y = 8;
    int dx = 1;
    int64_t index = -1;
    auto nextMask = [&]() -> unsigned char {
        unsigned char mask = 0;
        if (x < 0) {
            dx = 1;
            y = (8 - y) % 8;
            mask = 0xc3;
        } else if (x > 6) {
            dx = -1;
            y = 7 - y;
            mask = 0xd8;
        } else {
            mask = kMaskSeed[y][x];
        }
        x += dx;
        return mask;
    };

    std::vector<unsigned char> output;
    output.reserve(input.size());
    for (unsigned char value : input) {
        ++index;
        unsigned char mask = nextMask();
        if (index == 0x8000 || (index > 0x8000 && (index + 1) % 0x8000 == 0)) {
            ++index;
            mask = nextMask();
        }
        output.push_back(static_cast<unsigned char>(value ^ mask));
    }
    return output;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string decodeXmlEntities(std::string text) {
    replaceAll(text, "&quot;", "\"");
    replaceAll(text, "&apos;", "'");
    replaceAll(text, "&lt;", "<");
    replaceAll(text, "&gt;", ">");
    replaceAll(text, "&amp;", "&");
    return text;
}

std::string unwrapLyricContent(std::string text) {
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
    if (text.find("<Lyric_1") == std::string::npos)
        return text;
    const std::string marker = "LyricContent=\"";
    size_t begin = text.find(marker);
    if (begin == std::string::npos)
        return {};
    begin += marker.size();
    size_t end = text.find('"', begin);
    if (end == std::string::npos)
        return {};
    return decodeXmlEntities(text.substr(begin, end - begin));
}

bool parseNumber(const std::string& text, size_t begin, size_t end, int64_t& value) {
    if (begin >= end)
        return false;
    value = 0;
    for (size_t i = begin; i < end; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            return false;
        value = value * 10 + (text[i] - '0');
    }
    return true;
}

bool parseQrc(const std::string& content, std::vector<LyricLine>& out) {
    std::vector<LyricLine> lines;
    std::istringstream input(content);
    std::string raw;
    while (std::getline(input, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.empty() || raw[0] != '[')
            continue;
        size_t close = raw.find(']');
        size_t comma = raw.find(',', 1);
        if (close == std::string::npos || comma == std::string::npos || comma > close)
            continue;
        int64_t lineStart = 0;
        int64_t lineDuration = 0;
        if (!parseNumber(raw, 1, comma, lineStart) ||
            !parseNumber(raw, comma + 1, close, lineDuration))
            continue;

        std::vector<LyricChar> chars;
        std::wstring lineText;
        size_t pos = close + 1;
        while (pos < raw.size()) {
            size_t open = raw.find('(', pos);
            if (open == std::string::npos)
                break;
            size_t tokenComma = raw.find(',', open + 1);
            size_t end = raw.find(')', tokenComma == std::string::npos ? open + 1 : tokenComma + 1);
            if (tokenComma == std::string::npos || end == std::string::npos)
                break;
            int64_t start = 0;
            int64_t duration = 0;
            if (!parseNumber(raw, open + 1, tokenComma, start) ||
                !parseNumber(raw, tokenComma + 1, end, duration))
                break;
            std::wstring token = toWide(raw.substr(pos, open - pos));
            if (!token.empty()) {
                lineText += token;
                chars.push_back({start, start + std::max<int64_t>(duration, 1), token});
            }
            pos = end + 1;
        }
        if (chars.empty() || lineText.empty())
            continue;
        lines.push_back({lineStart, std::move(lineText), std::move(chars)});
    }
    if (lines.empty()) {
        return false;
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    out = std::move(lines);
    return true;
}

} // namespace

bool decodeQrcLyrics(const std::string& encryptedHex, std::vector<LyricLine>& out) {
    std::vector<unsigned char> encrypted;
    std::vector<unsigned char> decrypted;
    std::string content;
    if (!hexDecode(encryptedHex, encrypted) || !tripleDesDecrypt(encrypted, decrypted) ||
        !inflateQrc(decrypted, content)) {
        return false;
    }
    std::string unwrapped = unwrapLyricContent(std::move(content));
    return parseQrc(unwrapped, out);
}

bool decodeLocalQrcText(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    if (bytes.empty())
        return false;
    std::vector<unsigned char> raw(bytes.begin(), bytes.end());

    const std::vector<unsigned char> unmasked = unmaskLocalQmc(raw);
    const std::string unmaskedText(unmasked.begin(), unmasked.end());
    size_t bodyOffset = 0;
    if (unmaskedText.rfind("[offset:", 0) == 0) {
        const size_t newline = unmaskedText.find('\n');
        if (newline == std::string::npos)
            return false;
        bodyOffset = newline + 1;
    }
    if (bodyOffset >= unmasked.size())
        return false;

    std::vector<unsigned char> encrypted(unmasked.begin() + bodyOffset, unmasked.end());
    std::vector<unsigned char> decrypted;
    std::string content;
    if (!decryptQrcDes(encrypted, decrypted) || !inflateQrc(decrypted, content))
        return false;

    out = unwrapLyricContent(std::move(content));
    return !out.empty();
}

bool parseQrcLyricsText(const std::string& content, std::vector<LyricLine>& out) {
    return parseQrc(content, out);
}
