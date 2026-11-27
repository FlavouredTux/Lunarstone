// The console's instance and function caches (Console.cpp opens them):
// cache.invalidate/iscached/replace, cloneref, compareinstances, the closure
// helpers an executor offers and the lz4 pair.
//
// What these do is the same in every executor: they work on the VM's own
// bookkeeping rather than on the game. The instance cache is this VM's table
// of one userdata per Instance, so invalidating an entry makes the next
// lookup hand out a fresh one; cloneref goes behind that cache to make two
// values stand for one Instance, and cache.replace makes one Instance's entry
// another's value.

#include "Console.hpp"
#include "Decompiler.hpp"
#include "InstanceApi.hpp"
#include "Runtime.hpp"

#include <lunar/Lz4.hpp>

#include <lapi.h>
#include <ldo.h>
#include <lfunc.h>
#include <lgc.h>
#include <lobject.h>
#include <lstate.h>
#include <ltm.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

// The VM's instance cache as a table on top of the stack: Instance* -> userdata.
void pushInstanceCache(lua_State* L)
{
    lua_getref(L, Runtime::of(L).instanceCache);
}

// --- The instance cache -----------------------------------------------------

// cache.invalidate(instance): forgets the userdata the VM has for this
// instance, so the next read of it hands out a new one. A script holding the
// old one still has it: this only empties the cache.
int cacheInvalidate(lua_State* L)
{
    // checkInstance first: pushing the cache would move the arguments up.
    Instance& instance = checkInstance(L, 1);
    pushInstanceCache(L);
    lua_pushlightuserdata(L, &instance);
    lua_pushnil(L);
    lua_rawset(L, -3);
    return 0;
}

// cache.iscached(instance): whether the VM still has a userdata for it.
int cacheIsCached(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    pushInstanceCache(L);
    lua_pushlightuserdata(L, &instance);
    lua_rawget(L, -2);
    const bool cached = !lua_isnil(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, cached);
    return 1;
}

// cache.replace(old, new): the cache's entry for `old` becomes the userdata of
// `new`, so wherever the game hands out `old` from here on (workspace.Old, a
// child list, an event's argument) a script gets `new`. The references to
// `old` scripts already hold are left as they are [sUNC: "Should only replace
// the instance cache, not the reference"].
int cacheReplace(lua_State* L)
{
    Instance& from = checkInstance(L, 1);
    checkInstance(L, 2);
    pushInstanceCache(L);
    lua_pushlightuserdata(L, &from);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    return 0;
}

// --- References to one instance --------------------------------------------

// cloneref(instance): another value that reads and writes the same instance.
// It is a userdata of its own that the cache doesn't hold, so rawequal calls
// the two different values while every member of it resolves to the one
// instance -- which is what the references a script passes around are for.
int cloneRef(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(InstancePtr), InstanceTag);
    new (memory) InstancePtr(instance.shared_from_this());
    return 1;
}

// compareinstances(a, b): whether two values stand for the same instance,
// which rawequal cannot answer when one of them is a cloneref.
int compareInstances(lua_State* L)
{
    const Instance* first = &checkInstance(L, 1);
    lua_pushboolean(L, first == &checkInstance(L, 2));
    return 1;
}

// --- Closures ---------------------------------------------------------------

// The environment a closure was made in: the globals table of the code that
// made it. A Luau function gets the environment of the function that wrote it
// and a C one the environment of the C function that pushed it (getcurrenv),
// which is how isexecutorclosure tells one from the other without a list.
LuaTable* closureEnvironment(lua_State* L, int idx)
{
    return lua_type(L, idx) == LUA_TFUNCTION ? clvalue(luaA_toobject(L, idx))->env : nullptr;
}

// checkfunction(L, idx): the closure at idx, or an argument error.
Closure* checkClosure(lua_State* L, int idx)
{
    if (lua_type(L, idx) != LUA_TFUNCTION)
        luaL_typeerror(L, idx, "function");
    return clvalue(luaA_toobject(L, idx));
}

// iscclosure(f): whether the value is a C closure -- the library's own
// functions and what newcclosure makes, against what Luau compiled.
int isCClosure(lua_State* L)
{
    lua_pushboolean(L, checkClosure(L, 1)->isC != 0);
    return 1;
}

// islclosure(f): the other way about.
int isLClosure(lua_State* L)
{
    lua_pushboolean(L, checkClosure(L, 1)->isC == 0);
    return 1;
}

// isexecutorclosure(f) (checkclosure, isourclosure): whether this closure came
// from the console. Its functions were made in getgenv()'s table, and each
// console run's globals are a table over that one (openConsole); so every
// closure the console makes has one of those as its environment -- the
// globals themselves, what newcclosure and clonefunction make, and anything
// written by code the Luau tab ran -- and nothing the client's libraries made
// has it.
int isExecutorClosure(lua_State* L)
{
    luaL_checkany(L, 1);
    LuaTable* env = closureEnvironment(L, 1);
    Runtime& rt = Runtime::of(L);
    bool ours = false;
    if (env && rt.consoleGlobals() != LUA_NOREF) {
        lua_checkstack(L, 3);
        lua_getref(L, rt.consoleGlobals());
        const LuaTable* genv = hvalue(luaA_toobject(L, -1));
        ours = env == genv;
        luaC_threadbarrier(L);
        sethvalue(L, L->top, env);
        L->top++;
        if (!ours && lua_getmetatable(L, -1)) {
            lua_rawgetfield(L, -1, "__index");
            ours = lua_rawequal(L, -1, -4);
            lua_pop(L, 2);
        }
        lua_pop(L, 2);
    }
    lua_pushboolean(L, ours);
    return 1;
}

// The body of a newcclosure: call what the upvalue holds and pass on every
// result, which is what makes a C closure of the function. The continuation
// lets the function yield: Luau only lets a C function's callee yield when
// the C function has one, and a newcclosure'd __namecall hook passes on
// InvokeServer and WaitForChild, which do. Not lua_callyieldable, which calls
// the continuation the closure had when the call began: hookfunction may have
// swapped it since (see ConsoleHooks.cpp).
int callWrapped(lua_State* L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    if (isyielded(L))
        return C_CALL_YIELD;
    return lua_gettop(L);
}

int wrappedResults(lua_State* L, int /*status*/)
{
    return lua_gettop(L);
}

// newcclosure(function[, name]): the same function as a C closure, which
// debug.info calls `name` when one is given. Its environment is the
// console's globals, so it is one of ours as well.
int newCClosure(lua_State* L)
{
    checkClosure(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 1);
        lua_pushcclosurek(L, callWrapped, "newcclosure", 1, wrappedResults);
        return 1;
    }
    // The closure keeps its name as a second upvalue, which keeps the
    // characters its debug name points at alive (strings don't move).
    const char* name = luaL_checkstring(L, 2);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosurek(L, callWrapped, name, 2, wrappedResults);
    return 1;
}

// clonefunction(f): a second closure over the same code. Luau's own
// lua_clonefunction copies the closure and shares its prototype, which is what
// a clone of a function is; it takes only Luau closures (handed a C one, it
// reads the C function's pointer as a prototype), so a C closure is copied
// field by field. A Luau clone keeps the original's environment (setfenv'd
// or not, Myriad checks); a C one's is the console's.
int cloneFunction(lua_State* L)
{
    Closure* cl = checkClosure(L, 1);
    if (!cl->isC) {
        lua_clonefunction(L, 1); // which gives it this thread's globals
        lua_getfenv(L, 1);
        lua_setfenv(L, -2);
        return 1;
    }
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    luaL_checkstack(L, 1, nullptr);
    Closure* copy = luaF_newCclosure(L, cl->nupvalues, L->gt);
    copy->c.f = cl->c.f;
    copy->c.cont = cl->c.cont;
    copy->c.debugname = cl->c.debugname;
    for (int i = 0; i < cl->nupvalues; ++i)
        setobj2n(L, &copy->c.upvals[i], &cl->c.upvals[i]);
    setclvalue(L, L->top, copy);
    L->top++;
    return 1;
}

// --- Scripts ---------------------------------------------------------------

// The Source of a script, or an argument error: not every Instance has one.
std::string_view sourceOf(lua_State* L, int idx)
{
    if (const auto* source = dynamic_cast<const LuaSourceContainer*>(&checkInstance(L, idx)))
        return source->source();
    luaL_typeerror(L, idx, "LuaSourceContainer");
}

// getscriptclosure(script) (getscriptfunction): the function a script's own
// code would make, without the script having run it -- its bytecode compiled
// and loaded here, so calling it runs the module body afresh and gives a
// table that isn't the cached one require() handed out. Nothing for a script
// with no source, or one whose source doesn't compile.
int scriptClosure(lua_State* L)
{
    const Instance& script = checkInstance(L, 1);
    auto bytecode = Runtime::of(L).codeOf(sourceOf(L, 1), script.fullName());
    if (!bytecode)
        return 0;
    const std::string name = std::format("={}", script.fullName());
    if (luau_load(L, name.c_str(), bytecode->data(), bytecode->size(), 0) != 0) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

// getscriptbytecode(script) (dumpstring): the bytecode the script is run from
// as the client holds it: the bytecode the server sent, unwrapped, or its
// Source compiled as Luau, with the opcodes encoded as the server's compiler
// encodes them and in a format from before version 11 (Runtime::sentCodeOf),
// which is what decompilers read. Not a chunk loadstring can load, which
// compiles Luau source and says so.
int getScriptBytecode(lua_State* L)
{
    const Instance& script = checkInstance(L, 1);
    if (sourceOf(L, 1).empty())
        return 0; // a script with no code has no bytecode
    auto bytecode = Runtime::of(L).sentCodeOf(sourceOf(L, 1), script.fullName());
    if (!bytecode)
        raise(L, bytecode.error());
    lua_pushlstring(L, bytecode->data(), bytecode->size());
    return 1;
}

// --- decompile ----------------------------------------------------------------

std::string_view text(const TString* s)
{
    return s ? std::string_view(getstr(s), s->len) : std::string_view{};
}

std::string describeConstant(const TValue* k)
{
    switch (ttype(k)) {
    case LUA_TNIL:
        return "nil";
    case LUA_TBOOLEAN:
        return bvalue(k) ? "true" : "false";
    case LUA_TNUMBER:
        return std::format("{}", nvalue(k));
    case LUA_TSTRING: {
        std::string quoted = "\"";
        for (const char c : text(tsvalue(k))) {
            if (c == '"' || c == '\\')
                quoted += '\\';
            if (static_cast<unsigned char>(c) < 0x20)
                quoted += std::format("\\{}", static_cast<int>(static_cast<unsigned char>(c)));
            else
                quoted += c;
        }
        return quoted + "\"";
    }
    case LUA_TVECTOR:
        return std::format("vector({}, {}, {})", k->value.v[0], k->value.v[1], k->value.v[2]);
    default:
        return std::format("<{}>", luaT_typenames[ttype(k)]);
    }
}

// One function of the listing and the ones inside it, a level deeper.
void list(std::string& out, const Proto* p, int depth)
{
    const std::string indent(static_cast<std::size_t>(depth) * 4, ' ');
    std::string params;
    for (int i = 0; i < p->numparams; ++i) {
        const std::string_view name
            = i < p->sizelocvars ? text(p->locvars[i].varname) : std::string_view{};
        params += std::format("{}{}", i ? ", " : "", name.empty() ? std::format("p{}", i + 1) : std::string(name));
    }
    if (p->is_vararg)
        params += params.empty() ? "..." : ", ...";
    const std::string_view name = text(p->debugname);
    out += std::format("{}{} function {}({}) -- line {}\n", indent, depth ? "local" : "--", name.empty() ? "anonymous" : name,
                       params, p->linedefined);
    const std::string body = indent + "    -- ";
    if (p->sizeupvalues > 0) {
        out += body + "upvalues:";
        for (int i = 0; i < p->sizeupvalues; ++i)
            out += std::format(" {}", text(p->upvalues[i]));
        out += '\n';
    }
    if (p->sizelocvars > p->numparams) {
        out += body + "locals:";
        for (int i = p->numparams; i < p->sizelocvars; ++i)
            out += std::format(" {}", text(p->locvars[i].varname));
        out += '\n';
    }
    if (p->sizek > 0) {
        out += body + "constants:";
        for (int i = 0; i < p->sizek; ++i)
            out += std::format(" {}", describeConstant(&p->k[i]));
        out += '\n';
    }
    out += std::format("{}{} instructions\n", body, p->sizecode);
    for (int i = 0; i < p->sizep; ++i)
        list(out, p->p[i], depth + 1);
    if (depth)
        out += indent + "end\n";
}

// decompile(script or function): the script's own Source when it has one,
// Luau rebuilt from the bytecode (Decompiler.hpp) for a script the server
// sent as bytecode, and otherwise a listing of what the bytecode holds --
// each function with its parameters, upvalues, locals and constants, the
// inner ones nested in it -- for any Luau function, or a script whose
// bytecode didn't decompile (after a comment saying why).
int decompile(lua_State* L)
{
    std::string out;
    if (lua_type(L, 1) != LUA_TFUNCTION) {
        const Instance& script = checkInstance(L, 1);
        const std::string_view source = sourceOf(L, 1);
        Runtime& rt = Runtime::of(L);
        if (!rt.shared().networkBytecode || !rt.shared().networkBytecode(source)) {
            lua_pushlstring(L, source.data(), source.size());
            return 1;
        }
        auto code = rt.codeOf(source, script.fullName());
        auto text = code ? decompileBytecode(*code) : std::unexpected(code.error());
        if (text) {
            lua_pushlstring(L, text->data(), text->size());
            return 1;
        }
        out = std::format("--[[decompiler: failed: {}]]\n", text.error());
        lua_settop(L, 1);
        if (scriptClosure(L) == 0)
            raise(L, std::format("{}'s bytecode didn't load", script.fullName()));
        lua_replace(L, 1);
    }
    const Closure* closure = clvalue(luaA_toobject(L, 1));
    if (closure->isC)
        luaL_argerror(L, 1, "lua function expected");
    out += "-- Listed by Lunarstone from the bytecode: what each function holds, not its source.\n";
    list(out, closure->l.p, 0);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// --- lz4 --------------------------------------------------------------------

// lz4compress(raw) / lz4decompress(block, size): the LZ4 block format the place
// files use (see Lz4.hpp), as the executors' pair of them does -- the block
// carries no frame header, so the caller says how much it decodes to.
int lz4Compress(lua_State* L)
{
    const std::string_view raw = checkString(L, 1);
    const std::string block = lz4::compressBlock(raw);
    lua_pushlstring(L, block.data(), block.size());
    return 1;
}

int lz4Decompress(lua_State* L)
{
    const std::string_view block = checkString(L, 1);
    const lua_Integer size = luaL_checkinteger(L, 2);
    if (size < 0)
        luaL_argerror(L, 2, "size out of range");
    auto decoded = lz4::decompressBlock(block, static_cast<std::size_t>(size));
    if (!decoded)
        raise(L, decoded.error());
    lua_pushlstring(L, decoded->data(), decoded->size());
    return 1;
}

} // namespace

void openConsoleCaches(lua_State* thread)
{
    const auto set = [thread](const char* name, lua_CFunction function) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    };
    set("clonefunction", cloneFunction);
    set("cloneref", cloneRef);
    set("compareinstances", compareInstances);
    set("iscclosure", isCClosure);
    set("islclosure", isLClosure);
    set("isexecutorclosure", isExecutorClosure);
    set("checkclosure", isExecutorClosure);
    set("isourclosure", isExecutorClosure);
    set("newcclosure", newCClosure);
    set("getscriptclosure", scriptClosure);
    set("getscriptfunction", scriptClosure);
    set("getscriptbytecode", getScriptBytecode);
    set("decompile", decompile);
    set("dumpstring", getScriptBytecode);
    set("lz4compress", lz4Compress);
    set("lz4decompress", lz4Decompress);

    // The cache table, as the executors spell it: cache.invalidate(instance),
    // cache.iscached(instance) and cache.replace(old, new).
    lua_newtable(thread);
    const int table = lua_gettop(thread);
    for (const auto& [name, function] :
         {std::pair{"invalidate", cacheInvalidate}, std::pair{"iscached", cacheIsCached},
          std::pair{"replace", cacheReplace}}) {
        lua_pushcfunction(thread, function, name);
        lua_setfield(thread, table, name);
    }
    lua_setglobal(thread, "cache");
}

} // namespace lunar::lua