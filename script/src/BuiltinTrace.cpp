// research: LUNAR_TRACE_BUILTINS=<chunk name part> logs the builtins a
// script calls (what it hands them, what they give back), for scripts
// that only use the standard library, like an obfuscator's checks before
// they touch the game. The library functions are swapped in place, so
// identities hold (("").sub == string.sub); error and getfenv/setfenv take
// stack levels, and their wrappers count themselves out. The first calls
// go to stderr as they happen, the last ones when the script times out.

#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

struct Trace {
    std::string filter;
    std::deque<std::string> recent; // the last calls, for the timeout
    std::map<std::string, long, std::less<>> counts;
    long logged = 0;
};

Trace& trace()
{
    static Trace t;
    return t;
}

// The calling script matches the filter: the nearest Lua function up the
// stack (a builtin may be called by another, xpcall(getfenv, ...)).
bool traced(lua_State* L)
{
    lua_Debug ar{};
    for (int level = 1; level <= 4 && lua_getinfo(L, level, "s", &ar); ++level)
        if (ar.source && std::string_view(ar.source) != "=[C]")
            return std::string_view(ar.source).contains(trace().filter);
    return false;
}

std::string describe(lua_State* L, int idx)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return "nil";
    case LUA_TBOOLEAN:
        return lua_toboolean(L, idx) ? "true" : "false";
    case LUA_TNUMBER:
        return std::format("{}", lua_tonumber(L, idx));
    case LUA_TSTRING: {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        std::string out;
        for (std::size_t i = 0; i < n && out.size() < 90; ++i) {
            const auto c = static_cast<unsigned char>(s[i]);
            out += c >= 32 && c < 127 && c != '"' ? std::string(1, static_cast<char>(c)) : std::format("\\{}", c);
        }
        return std::format("\"{}\"{}", out, n > 90 ? std::format("..({})", n) : "");
    }
    case LUA_TTABLE: {
        // Which well-known table, if any.
        const int at = lua_absindex(L, idx);
        for (const char* name : {"string", "table", "math", "coroutine", "debug", "task", "buffer", "bit32", "utf8",
                                 "os", "vector", "_G", "shared", "Instance", "Vector3", "Vector2", "CFrame", "Color3",
                                 "UDim", "UDim2", "Ray", "Region3", "TweenInfo", "NumberRange", "NumberSequence",
                                 "ColorSequence", "Random", "Rect", "BrickColor", "Enum", "Faces", "Axes",
                                 "PhysicalProperties", "Font", "RaycastParams", "OverlapParams", "Vector3int16",
                                 "Vector2int16", "DateTime", "SharedTable", "Content"}) {
            lua_getglobal(L, name);
            const bool same = lua_rawequal(L, -1, at);
            lua_pop(L, 1);
            if (same)
                return std::format("<table {}>", name);
        }
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        const bool env = lua_rawequal(L, -1, at);
        lua_pop(L, 1);
        if (env)
            return "<table the script's environment>";
        std::string keys;
        int count = 0;
        lua_pushnil(L);
        while (lua_next(L, at) != 0) {
            if (count < 6 && lua_type(L, -2) == LUA_TSTRING)
                keys += std::string(count ? "," : "") + lua_tostring(L, -2);
            ++count;
            lua_pop(L, 1);
        }
        return std::format("<table {} {} keys: {}>", lua_topointer(L, at), count, keys);
    }
    case LUA_TTHREAD: {
        lua_State* co = lua_tothread(L, idx);
        static constexpr const char* statuses[] = {"running", "suspended", "normal", "finished", "error"};
        const int status = lua_costatus(L, co);
        return std::format("<thread {} {}{}>", static_cast<const void*>(co),
                           status >= 0 && status < 5 ? statuses[status] : "?", co == L ? ", this one" : "");
    }
    case LUA_TFUNCTION: {
        // A datatype library's function: say whose.
        const int at = lua_absindex(L, idx);
        for (const char* name : {"Instance", "Vector3", "Vector2", "CFrame", "Color3", "UDim", "UDim2", "Ray",
                                 "Region3", "TweenInfo", "NumberRange", "NumberSequence", "ColorSequence", "Random",
                                 "Rect", "BrickColor", "Faces", "Axes", "PhysicalProperties", "Font", "RaycastParams",
                                 "OverlapParams", "Vector3int16", "Vector2int16", "DateTime"}) {
            lua_getglobal(L, name);
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    if (lua_rawequal(L, -1, at) && lua_type(L, -2) == LUA_TSTRING) {
                        std::string out = std::format("<function {}.{}>", name, lua_tostring(L, -2));
                        lua_pop(L, 3);
                        return out;
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_Debug ar{};
        lua_pushvalue(L, idx);
        const bool known = lua_getinfo(L, -1, "sn", &ar);
        lua_pop(L, 1);
        return std::format("<function {} {}>", known && ar.name ? ar.name : "?", known && ar.short_src ? ar.short_src : "?");
    }
    default:
        return std::format("<{}>", luaL_typename(L, idx));
    }
}

std::string describeRange(lua_State* L, int first, int last)
{
    std::string out;
    for (int i = first; i <= last; ++i)
        out += (i > first ? ", " : "") + describe(L, i);
    return out;
}

// The hot decoding helpers; anything else is rare enough to always print.
bool hot(std::string_view line)
{
    for (std::string_view name : {"string.char(", "string.sub(", "string.byte(", "select(", "unpack(", "getfenv(",
                                   "table.create(", "table.move(", "buffer.read", "buffer.write"})
        if (line.starts_with(name))
            return true;
    return false;
}

// The last part of the nearest Lua function's chunk name.
std::string whose(lua_State* L)
{
    lua_Debug ar{};
    for (int level = 1; level <= 6 && lua_getinfo(L, level, "s", &ar); ++level)
        if (ar.source && std::string_view(ar.source) != "=[C]") {
            std::string_view s = ar.source;
            return std::string(s.substr(s.find_last_of('.') + 1));
        }
    return "?";
}

lua_State* current = nullptr; // for record's prefix

void record(std::string line)
{
    if (current)
        line = std::format("[{}] {}", whose(current), line);
    Trace& t = trace();
    if (!hot(line) || t.logged++ < 200)
        std::fprintf(stderr, "builtin: %s\n", line.c_str());
    t.recent.push_back(std::move(line));
    if (t.recent.size() > 300)
        t.recent.pop_front();
}

// A plain function: called through, arguments and results recorded.
int wrapped(lua_State* L)
{
    current = L;
    const char* name = lua_tostring(L, lua_upvalueindex(2));
    const bool on = traced(L);
    const int nargs = lua_gettop(L);
    std::string args;
    if (on) {
        ++trace().counts[name];
        args = describeRange(L, 1, nargs);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    if (on) {
        if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0) {
            record(std::format("{}({}) raised {}", name, args, describe(L, -1)));
            lua_error(L);
        }
        record(std::format("{}({}) -> {}", name, args, describeRange(L, 1, lua_gettop(L))));
    } else {
        lua_call(L, nargs, LUA_MULTRET);
    }
    return lua_gettop(L);
}

// The console functions that count stack levels from their caller: the
// wrapper is one more level, which their first argument (a level, when it's
// a number) gets for it.
constexpr std::string_view levelCounting[] = {"getstack", "setstack", "getinfo", "info"};

// A console function: as wrapped, but always on, and capped by name. Its lines
// are notices, which LogService doesn't see (a script listening there would
// hear its own trace, and sUNC sends what it hears to its server).
int consoleWrapped(lua_State* L)
{
    const char* name = lua_tostring(L, lua_upvalueindex(2));
    // LUNAR_TRACE_CONSOLE=<n> for more than the first 40.
    static const long cap = [] {
        const long n = std::strtol(std::getenv("LUNAR_TRACE_CONSOLE"), nullptr, 10);
        return n > 1 ? n : 40;
    }();
    const long n = ++trace().counts[name];
    const bool on = n <= cap || n % 500 == 0;
    const int nargs = lua_gettop(L);
    const std::string args = on ? describeRange(L, 1, nargs) : std::string();
    const std::string_view bare = std::string_view(name).substr(std::string_view(name).rfind('.') + 1);
    if (std::ranges::find(levelCounting, bare) != std::end(levelCounting) && lua_type(L, 1) == LUA_TNUMBER) {
        lua_pushnumber(L, lua_tonumber(L, 1) + 1);
        lua_replace(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0) {
        if (on)
            Runtime::of(L).notice(MessageType::Info, std::format("trace: {}({}) raised {}", name, args, describe(L, -1)));
        lua_error(L);
    }
    if (on)
        Runtime::of(L).notice(MessageType::Info,
                              std::format("trace: {}({}) -> {}", name, args, describeRange(L, 1, lua_gettop(L))));
    return lua_gettop(L);
}

// error(message, level): as the library's, the level counted from our caller.
int tracedError(lua_State* L)
{
    current = L;
    const int level = luaL_optinteger(L, 2, 1);
    if (traced(L)) {
        ++trace().counts["error"];
        record(std::format("error({}, level {})", describe(L, 1), level));
    }
    lua_settop(L, 1);
    if (lua_isstring(L, 1) && level > 0) {
        luaL_where(L, level);
        lua_pushvalue(L, 1);
        lua_concat(L, 2);
    }
    lua_error(L);
}

// getfenv(level or function) / setfenv(level or function, table): a level
// passes one deeper, past this wrapper, to the original.
int levelled(lua_State* L)
{
    current = L;
    const char* name = lua_tostring(L, lua_upvalueindex(2));
    const bool on = traced(L);
    const int nargs = lua_gettop(L);
    std::string args = on ? describeRange(L, 1, nargs) : std::string();
    if (on && std::string_view(name) == "debug.info" && lua_type(L, 1) == LUA_TNUMBER) {
        // The whole stack, as the caller sees it (level 1 is the caller).
        std::string frames;
        lua_Debug ar{};
        for (int level = 1; lua_getinfo(L, level, "sln", &ar); ++level)
            frames += std::format(" [{}] {} {}:{}", level - 1, ar.name ? ar.name : "?",
                                  ar.short_src ? ar.short_src : "?", ar.currentline);
        record(std::format("stack at debug.info({}):{}", lua_tonumber(L, 1), frames));
    }
    // Anything but the plain getfenv() before the call: it may raise.
    if (on && nargs > 0 && std::string_view(name) != "debug.info"
        && !(lua_type(L, 1) == LUA_TNUMBER && lua_tonumber(L, 1) <= 2))
        record(std::format("{}({}) ...", name, args));
    if (lua_type(L, 1) == LUA_TNUMBER && lua_tonumber(L, 1) > 0) {
        lua_pushnumber(L, lua_tonumber(L, 1) + 1);
        lua_replace(L, 1);
    } else if (nargs == 0 && std::string_view(name) == "getfenv") {
        lua_pushnumber(L, 2); // getfenv(): our caller's
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    if (on) {
        // What it raises, if anything (then raised on as it was).
        if (lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0) != 0) {
            record(std::format("{}({}) raised {}", name, args, describe(L, -1)));
            lua_error(L);
        }
    } else {
        lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    }
    if (on) {
        ++trace().counts[name];
        record(std::format("{}({}) -> {}", name, args, describeRange(L, 1, lua_gettop(L))));
    }
    return lua_gettop(L);
}

// Replaces table[field] with `with` around it, under the original's debug
// name (debug.info(string.gmatch, "n") is "gmatch": obfuscators check it).
void wrap(lua_State* L, int table, const char* field, std::string_view label, lua_CFunction with)
{
    lua_getfield(L, table, field);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_Debug ar{};
    lua_pushvalue(L, -1);
    const char* original = lua_getinfo(L, -1, "n", &ar) && ar.name ? ar.name : field;
    lua_pop(L, 1);
    lua_pushlstring(L, label.data(), label.size());
    lua_pushcclosure(L, with, original, 2);
    lua_setfield(L, table, field);
}

} // namespace

void installBuiltinTrace(lua_State* L)
{
    const char* filter = std::getenv("LUNAR_TRACE_BUILTINS");
    if (!filter || !*filter)
        return;
    trace().filter = filter;
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    const int globals = lua_gettop(L);
    for (const char* name : {"tostring", "tonumber", "type", "typeof", "select", "unpack", "rawequal", "rawget",
                             "rawset", "rawlen", "setmetatable", "getmetatable", "next", "assert", "tick", "time",
                             "elapsedTime", "gcinfo", "collectgarbage", "newproxy", "version", "print", "warn",
                             "settings", "UserSettings", "ipairs", "pairs"})
        wrap(L, globals, name, name, wrapped);
    wrap(L, globals, "error", "error", tracedError);
    wrap(L, globals, "getfenv", "getfenv", levelled);
    wrap(L, globals, "setfenv", "setfenv", levelled);
    lua_getfield(L, globals, "debug");
    wrap(L, lua_gettop(L), "info", "debug.info", levelled);
    lua_pop(L, 1);
    struct Library {
        const char* name;
        std::array<const char*, 12> functions;
    };
    for (const Library& library : {
             Library{"string", {"sub", "match", "gmatch", "gsub", "find", "format", "rep", "char", "byte", "len",
                                "reverse", "split"}},
             Library{"table", {"move", "create", "concat", "unpack", "pack", "insert", "remove", "find", "clone",
                               "freeze", "isfrozen", "clear"}},
             // Not isyieldable, nor task.wait: inside a wrapper they'd see a C call boundary.
             Library{"coroutine", {"wrap", "create", "status", "running", "close"}},
             Library{"debug", {}},
             Library{"os", {"clock", "time", "date"}},
             Library{"math", {"random", "floor", "noise", "clamp", "huge"}},
             Library{"utf8", {"char", "codepoint", "len", "offset", "graphemes", "nfcnormalize"}},
             Library{"task", {"spawn", "defer", "delay", "cancel"}},
             Library{"buffer", {"create", "fromstring", "tostring", "len", "readu8", "readu32", "readi32", "readf64",
                                "readstring", "copy", "writeu8", "writestring"}},
         }) {
        lua_getfield(L, globals, library.name);
        const int table = lua_gettop(L);
        for (const char* function : library.functions)
            if (function)
                wrap(L, table, function, std::format("{}.{}", library.name, function), wrapped);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// research: LUNAR_TRACE_CONSOLE=1 logs the console's own functions the same
// way, from any chunk, into the client's output (with what the script
// prints): the first 40 calls of each (or as many as it is set to), then
// every 500th. They are wrapped in
// the console's globals and its tables (debug, crypt, ...), so identities hold
// there. Left alone: what yields (a wrapper is a C call boundary) and what
// looks further up the stack than its own caller.
void installConsoleTrace(lua_State* thread)
{
    const char* on = std::getenv("LUNAR_TRACE_CONSOLE");
    if (!on || !*on)
        return;
    static constexpr std::string_view untraced[]
        = {"request", "http_request", "getcallingscript", "traceback", "getlocal", "setlocal", "connect"};
    const auto skip = [](std::string_view name) { return std::ranges::find(untraced, name) != std::end(untraced); };
    const auto wrapAll = [&](int table, std::string_view prefix) {
        std::vector<std::string> names;
        lua_pushnil(thread);
        while (lua_next(thread, table) != 0) {
            if (lua_type(thread, -2) == LUA_TSTRING && lua_iscfunction(thread, -1) && !skip(lua_tostring(thread, -2)))
                names.emplace_back(lua_tostring(thread, -2));
            lua_pop(thread, 1);
        }
        for (const std::string& name : names)
            wrap(thread, table, name.c_str(), std::format("{}{}", prefix, name), consoleWrapped);
    };
    lua_pushvalue(thread, LUA_GLOBALSINDEX);
    const int globals = lua_gettop(thread);
    std::vector<std::string> tables;
    lua_pushnil(thread);
    while (lua_next(thread, globals) != 0) {
        if (lua_type(thread, -2) == LUA_TSTRING && lua_istable(thread, -1))
            tables.emplace_back(lua_tostring(thread, -2));
        lua_pop(thread, 1);
    }
    wrapAll(globals, "");
    for (const std::string& name : tables) {
        lua_rawgetfield(thread, globals, name.c_str());
        if (!lua_getreadonly(thread, -1))
            wrapAll(lua_gettop(thread), name + ".");
        lua_pop(thread, 1);
    }
    lua_pop(thread, 1);
}

void dumpBuiltinTrace()
{
    Trace& t = trace();
    if (t.filter.empty())
        return;
    std::fprintf(stderr, "builtin: --- the last %zu calls before the timeout ---\n", t.recent.size());
    for (const std::string& line : t.recent)
        std::fprintf(stderr, "builtin: %s\n", line.c_str());
    std::fprintf(stderr, "builtin: --- calls by function ---\n");
    for (const auto& [name, count] : t.counts)
        std::fprintf(stderr, "builtin: %8ld %s\n", count, name.c_str());
}

} // namespace lunar::lua
