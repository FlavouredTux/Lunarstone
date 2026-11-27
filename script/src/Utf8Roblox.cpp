// Roblox's additions to Luau's utf8 library: graphemes, nfcnormalize and
// nfdnormalize (the client registers the three together, strings at
// 0x1068d11dd). Games use them, and obfuscators check the library has them:
// Luraph enumerates utf8 and hangs its script when it finds Luau's six
// alone (JJS' camera). The Unicode rules are ICU's.

#include "Runtime.hpp"

#include <unicode/ubrk.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

// UTF-8 to UTF-16, and for each UTF-16 unit the byte it starts at (one more
// at the end, the string's length).
bool toUtf16(std::string_view in, std::u16string& out, std::vector<std::size_t>* bytes)
{
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, in.data(), static_cast<int32_t>(in.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
        return false;
    out.resize(static_cast<std::size_t>(length));
    status = U_ZERO_ERROR;
    u_strFromUTF8(out.data(), length, nullptr, in.data(), static_cast<int32_t>(in.size()), &status);
    if (U_FAILURE(status))
        return false;
    if (bytes) {
        // Walk the UTF-8 (valid, ICU said so): one entry per UTF-16 unit.
        bytes->clear();
        for (std::size_t at = 0; at < in.size();) {
            const auto lead = static_cast<unsigned char>(in[at]);
            const std::size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            bytes->push_back(at);
            if (len == 4)
                bytes->push_back(at); // the pair's second unit
            at += len;
        }
        bytes->push_back(in.size());
    }
    return true;
}

std::string toUtf8(const std::u16string& in)
{
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    u_strToUTF8(nullptr, 0, &length, in.data(), static_cast<int32_t>(in.size()), &status);
    std::string out(static_cast<std::size_t>(length), '\0');
    status = U_ZERO_ERROR;
    u_strToUTF8(out.data(), length, nullptr, in.data(), static_cast<int32_t>(in.size()), &status);
    return out;
}

int normalize(lua_State* L, bool composed)
{
    std::size_t size = 0;
    const char* text = luaL_checklstring(L, 1, &size);
    std::u16string wide;
    if (!toUtf16({text, size}, wide, nullptr))
        luaL_error(L, "invalid UTF-8 code"); // GUESS: the client's words
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* form = composed ? unorm2_getNFCInstance(&status) : unorm2_getNFDInstance(&status);
    std::u16string out(wide.size() * 3 + 8, u'\0');
    const int32_t length = unorm2_normalize(form, wide.data(), static_cast<int32_t>(wide.size()), out.data(),
                                            static_cast<int32_t>(out.size()), &status);
    if (U_FAILURE(status))
        luaL_error(L, "invalid UTF-8 code");
    out.resize(static_cast<std::size_t>(length));
    const std::string result = toUtf8(out);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

int nfcNormalize(lua_State* L)
{
    return normalize(L, true);
}

int nfdNormalize(lua_State* L)
{
    return normalize(L, false);
}

// The iterator: upvalue 1 the clusters' first and last bytes (1-based, in
// pairs), upvalue 2 how many it has given.
int nextGrapheme(lua_State* L)
{
    const int given = lua_tointeger(L, lua_upvalueindex(2));
    lua_rawgeti(L, lua_upvalueindex(1), given * 2 + 1);
    if (lua_isnil(L, -1))
        return 0;
    lua_rawgeti(L, lua_upvalueindex(1), given * 2 + 2);
    lua_pushinteger(L, given + 1);
    lua_replace(L, lua_upvalueindex(2));
    return 2;
}

// utf8.graphemes(s, i, j): for first, last in it, each extended grapheme
// cluster of s between bytes i and j.
int graphemes(lua_State* L)
{
    std::size_t size = 0;
    const char* text = luaL_checklstring(L, 1, &size);
    const auto length = static_cast<lua_Integer>(size);
    lua_Integer i = luaL_optinteger(L, 2, 1);
    lua_Integer j = luaL_optinteger(L, 3, length);
    if (i < 0)
        i = std::max<lua_Integer>(length + i + 1, 1);
    if (j < 0)
        j = length + j + 1;
    i = std::max<lua_Integer>(i, 1);
    j = std::min(j, length);
    const std::string_view part = i <= j ? std::string_view(text + i - 1, static_cast<std::size_t>(j - i + 1))
                                         : std::string_view();
    std::u16string wide;
    std::vector<std::size_t> bytes;
    if (!toUtf16(part, wide, &bytes))
        luaL_error(L, "invalid UTF-8 code");
    lua_createtable(L, 0, 0);
    if (!wide.empty()) {
        UErrorCode status = U_ZERO_ERROR;
        UBreakIterator* clusters = ubrk_open(UBRK_CHARACTER, "", wide.data(), static_cast<int32_t>(wide.size()), &status);
        if (U_FAILURE(status))
            luaL_error(L, "graphemes: %s", u_errorName(status));
        int n = 0;
        for (int32_t start = ubrk_first(clusters), end = ubrk_next(clusters); end != UBRK_DONE;
             start = end, end = ubrk_next(clusters)) {
            lua_pushinteger(L, static_cast<lua_Integer>(bytes[static_cast<std::size_t>(start)]) + i);
            lua_rawseti(L, -2, ++n);
            lua_pushinteger(L, static_cast<lua_Integer>(bytes[static_cast<std::size_t>(end)]) + i - 1);
            lua_rawseti(L, -2, ++n);
        }
        ubrk_close(clusters);
    }
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, nextGrapheme, "graphemes", 2);
    return 1;
}

} // namespace

void openUtf8Extensions(lua_State* L)
{
    lua_getglobal(L, "utf8");
    lua_pushcfunction(L, graphemes, "graphemes");
    lua_setfield(L, -2, "graphemes");
    lua_pushcfunction(L, nfcNormalize, "nfcnormalize");
    lua_setfield(L, -2, "nfcnormalize");
    lua_pushcfunction(L, nfdNormalize, "nfdnormalize");
    lua_setfield(L, -2, "nfdnormalize");
    lua_pop(L, 1);
}

} // namespace lunar::lua
