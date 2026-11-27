#include "Base64.hpp"

#include <cstdint>

namespace lunar::detail {

namespace {

int sextet(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

} // namespace

std::optional<std::string> decodeBase64(std::string_view text)
{
    std::string out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    bool padding = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=') {
            padding = true;
            continue;
        }
        int v = sextet(c);
        if (v < 0 || padding)
            return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

} // namespace lunar::detail
