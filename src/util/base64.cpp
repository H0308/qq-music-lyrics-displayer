#include "base64.h"

namespace {

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string base64Decode(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 3 / 4);
    int val = 0;
    int bits = -8;
    for (char c : input) {
        if (c == '=') break;
        int d = decodeChar(c);
        if (d < 0) continue; // 跳过换行等非法字符
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
