// The console's function hooks (Console.cpp opens them): hookfunction
// (replaceclosure), hookmetamethod, restorefunction, isfunctionhooked, and
// getgc, which is how a script finds a function to hook.
//
// A hook changes the function itself, not a reference to it: every script
// holding the function -- a local, a metatable's field, an import the VM
// resolved when it loaded the script -- calls the hook from then on, which is
// what an executor's hook does and what replacing a table field can't. The
// function handed back does what the hooked one did before.
//
// How a closure is changed depends on its kind, and neither way changes its
// size or its upvalue count, so the VM's memory stays where it is:
// - A C closure gets one shared body (hookCall) and its continuation; the
//   body finds its hook in a registry table by the closure being called. The
//   continuation is what lets a hook yield (InvokeServer through a __namecall
//   hook): Luau only lets a C function's callee yield when the C function has
//   one.
// - A Luau closure hooked with a Luau function whose upvalues fit in its own
//   becomes that function: a copy of the hook's prototype, the hook's
//   upvalues and its environment. The hook then runs in the frame the
//   closure's caller made, as an executor's does, so debug.getstack(2) from
//   the hook is the script that called it [sUNC: "Index not in stack[2]"].
// - Any other gets a prototype of its own, compiled from
//   `return ("lunarstone.hook")(...)` with that constant then swapped for the
//   hook, over its own upvalues and environment.
//   Either prototype has as many upvalues as the closure (the copy's past the
//   hook's are nil, the trampoline's read and dropped): the VM takes a
//   closure and its prototype to agree on that count (its collector asserts
//   it), and the debug library counts a function's upvalues by its prototype.
//   Restoring one is putting its prototype, upvalues and environment back.
//
// Frames already running the closure go on in a copy of it as it was: the VM
// reads a frame's upvalues through its closure, and a C frame's continuation
// on resume, as does the debug library its line numbers.

#include "Console.hpp"
#include "Runtime.hpp"

#include <lunar/script/ScriptContext.hpp>

#include <lapi.h>
#include <ldo.h>
#include <lfunc.h>
#include <lgc.h>
#include <lmem.h>
#include <lobject.h>
#include <lstate.h>
#include <ltable.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

// The registry's table of hooked closures: closure -> {hook = ..., real = ...},
// real being a copy of the closure from before its first hook.
constexpr const char* hooksKey = "lunarstone.hooks";
// The trampoline's constant that the hook takes the place of.
constexpr std::string_view hookSlot = "lunarstone.hook";

Closure* checkClosure(lua_State* L, int idx)
{
    if (lua_type(L, idx) != LUA_TFUNCTION)
        luaL_typeerror(L, idx, "function");
    return clvalue(luaA_toobject(L, idx));
}

void pushClosure(lua_State* L, Closure* cl)
{
    luaL_checkstack(L, 1, nullptr);
    luaC_threadbarrier(L);
    setclvalue(L, L->top, cl);
    L->top++;
}

void pushHooks(lua_State* L)
{
    if (lua_rawgetfield(L, LUA_REGISTRYINDEX, hooksKey) == LUA_TTABLE)
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetfield(L, LUA_REGISTRYINDEX, hooksKey);
}

// Pushes the closure's record, or nil when it isn't hooked.
void pushRecord(lua_State* L, Closure* cl)
{
    pushHooks(L);
    pushClosure(L, cl);
    lua_rawget(L, -2);
    lua_remove(L, -2);
}

// hooks[closure at idx] = the value on top (nil takes it off), popped.
void setRecord(lua_State* L, int closure)
{
    pushHooks(L);
    lua_pushvalue(L, closure);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 2);
}

// --- Calling the hook ---------------------------------------------------------

// Where a hooked C closure's frame goes once its hook has run: everything the
// hook answered, or its error raised again from here. Raised from here, the
// error comes from the hooked closure's own frame, as the closure's own error
// would, so an xpcall handler's debug.info(2) is still that closure and not
// the hook or what it called on to [sUNC's hookfunction: a game script
// watches __namecall that way, "generic namecall hook detected"]. It is also
// the continuation, for a hook that yields (InvokeServer through a
// __namecall hook) and is resumed with results or an error. Its own and not
// the closure's: a hook that takes itself off (restorefunction from inside
// the hook, the usual way to hook once) has put the original's back by then
// -- for most C functions, none at all -- while the frame has gone on in a
// copy of the hooked closure, which keeps this.
int hookDone(lua_State* L, int status)
{
    if (status != LUA_OK)
        lua_error(L);
    return lua_gettop(L);
}

// The body of every hooked C closure: the hook, with the arguments the
// closure was called with, called protected the way pcall calls (yields
// allowed) so its error can be raised again by hookDone. The hook is looked
// up on each call, so hooking a hooked function again only has to change its
// record.
int hookCall(lua_State* L)
{
    pushRecord(L, clvalue(L->ci->func));
    if (lua_type(L, -1) != LUA_TTABLE)
        raise(L, "this function's hook is gone"); // a copy made by restorefunction, called anew
    lua_rawgetfield(L, -1, "hook");
    lua_remove(L, -2);
    lua_insert(L, 1);

    struct Call {
        StkId func;
        static void run(lua_State* L, void* ud)
        {
            luaD_callint(L, static_cast<Call*>(ud)->func, LUA_MULTRET, lua_isyieldable(L) != 0);
        }
    } call{L->base};
    L->ci->errfunc = 0;
    L->ci->flags |= LUA_CALLINFO_HANDLE;
    const int status = luaD_pcall(L, &Call::run, &call, savestack(L, call.func), 0);
    expandstacklimit(L, L->top);
    if (status == LUA_OK && isyielded(L))
        return C_CALL_YIELD;
    L->ci->flags &= ~LUA_CALLINFO_HANDLE;
    return hookDone(L, status);
}

// The trampoline's source for a closure with `upvalues` of them. With any,
// it is a function inside the chunk that reads each of the chunk's locals
// into one of its own: the compiler only gives a function the upvalues it
// reads, and skips a read whose value goes nowhere.
std::string trampolineSource(int upvalues)
{
    const std::string call = std::format("return (\"{}\")(...)", hookSlot);
    if (upvalues == 0)
        return call;
    std::string names;
    for (int i = 1; i <= upvalues; ++i)
        names += std::format("{}u{}", i > 1 ? ", " : "", i);
    return std::format("local {0} = ...\nreturn function(...)\n    local {0} = {0}\n    {1}\nend", names, call);
}

// The prototype a hooked Luau closure with `upvalues` of them runs, with the
// hook as its constant, and the closure luau_load made of its chunk on top of
// the stack (kept by the caller until the prototype has a closure that holds
// it).
Proto* pushTrampoline(lua_State* L, int hook, int upvalues)
{
    // Compiled once for each count.
    static std::array<std::optional<std::expected<std::string, std::string>>, 256> compiled;
    std::optional<std::expected<std::string, std::string>>& bytecode = compiled.at(static_cast<std::size_t>(upvalues));
    if (!bytecode)
        bytecode = ScriptContext::compile(trampolineSource(upvalues), "hookfunction", 1);
    if (!*bytecode)
        raise(L, bytecode->error());
    if (luau_load(L, "=hookfunction", (*bytecode)->data(), (*bytecode)->size(), 0) != 0)
        raise(L, lua_tostring(L, -1));
    Proto* p = clvalue(L->top - 1)->l.p;
    if (upvalues > 0)
        p = p->sizep == 1 ? p->p[0] : nullptr;
    if (!p || p->nups != upvalues)
        raise(L, "hookfunction's trampoline doesn't have the closure's upvalues");
    const TValue* value = luaA_toobject(L, hook);
    for (int i = 0; i < p->sizek; ++i) {
        TValue* k = &p->k[i];
        if (ttisstring(k) && std::string_view(svalue(k), tsvalue(k)->len) == hookSlot) {
            setobj(L, k, value);
            luaC_barrier(L, p, value);
            return p;
        }
    }
    raise(L, "hookfunction's trampoline has no constant for the hook");
}

// --- Closures as they were ----------------------------------------------------

// Pushes a closure that does what cl does now: the same code or C function,
// the same upvalues and the same environment. The function a hook hands back,
// and what frames already running cl go on in.
Closure* pushCopy(lua_State* L, Closure* cl)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    Closure* copy = nullptr;
    if (cl->isC) {
        copy = luaF_newCclosure(L, cl->nupvalues, cl->env);
        copy->c.f = cl->c.f;
        copy->c.cont = cl->c.cont;
        copy->c.debugname = cl->c.debugname;
        for (int i = 0; i < cl->nupvalues; ++i)
            setobj2n(L, &copy->c.upvals[i], &cl->c.upvals[i]);
    } else {
        copy = luaF_newLclosure(L, cl->nupvalues, cl->env, cl->l.p);
        for (int i = 0; i < cl->nupvalues; ++i)
            setobj2n(L, &copy->l.uprefs[i], &cl->l.uprefs[i]);
    }
    pushClosure(L, copy);
    return copy;
}

struct Retarget {
    Closure* from;
    Closure* to;
};

void retargetFrames(const Retarget& r, lua_State* thread)
{
    bool moved = false;
    for (CallInfo* ci = thread->base_ci; ci <= thread->ci; ++ci)
        if (ttisfunction(ci->func) && clvalue(ci->func) == r.from) {
            setclvalue(thread, ci->func, r.to);
            moved = true;
        }
    if (moved)
        luaC_threadbarrier(thread);
}

// Every frame of every thread running `from` goes on in `to`.
void retarget(lua_State* L, Closure* from, Closure* to)
{
    const Retarget r{from, to};
    retargetFrames(r, L->global->mainthread);
    for (lua_State* thread : Runtime::of(L).threads())
        if (!isdead(L->global, obj2gco(thread)))
            retargetFrames(r, thread);
}

// Makes the Luau closure cl the Luau function `hook` (see the top), when the
// hook's upvalues fit in cl's; false when they don't.
bool hookInPlace(lua_State* L, Closure* cl, const Closure* hook)
{
    if (cl->isC || hook->isC || hook->nupvalues > cl->nupvalues)
        return false;
    Proto* p = copyProto(L, hook->l.p);
    p->nups = cl->nupvalues;
    cl->l.p = p;
    luaC_objbarrier(L, cl, p);
    cl->env = hook->env;
    luaC_objbarrier(L, cl, hook->env);
    cl->stacksize = hook->stacksize;
    for (int i = 0; i < cl->nupvalues; ++i) {
        if (i < hook->nupvalues) {
            setobj(L, &cl->l.uprefs[i], &hook->l.uprefs[i]);
            luaC_barrier(L, cl, &hook->l.uprefs[i]);
        } else {
            setnilvalue(&cl->l.uprefs[i]);
        }
    }
    return true;
}

// Puts what the copy `real` is back into cl.
void putBack(lua_State* L, Closure* cl, const Closure* real)
{
    if (cl->isC) {
        cl->c.f = real->c.f;
        cl->c.cont = real->c.cont;
        return;
    }
    cl->l.p = real->l.p;
    luaC_objbarrier(L, cl, real->l.p);
    cl->env = real->env;
    luaC_objbarrier(L, cl, real->env);
    cl->stacksize = real->stacksize;
    for (int i = 0; i < cl->nupvalues; ++i) {
        setobj(L, &cl->l.uprefs[i], &real->l.uprefs[i]);
        luaC_barrier(L, cl, &real->l.uprefs[i]);
    }
}

// --- What FASTCALL skips ------------------------------------------------------

// The globals and libraries the compiler turns calls of into FASTCALL
// (Luau's Compiler/src/Builtins.cpp): in a safe environment those calls go
// to the VM's own fast path and never reach the closure, so a hook on one
// would be skipped by every script calling it by name.
constexpr std::array fastGlobals{"assert",       "getmetatable", "rawequal", "rawget", "rawlen", "rawset", "select",
                                 "setmetatable", "tonumber",     "tostring", "type",   "typeof", "unpack"};
constexpr std::array fastLibraries{"bit32", "buffer", "integer", "math", "string", "table", "vector"};

bool isFunction(lua_State* L, int idx, const Closure* cl)
{
    return lua_type(L, idx) == LUA_TFUNCTION && clvalue(luaA_toobject(L, idx)) == cl;
}

// Whether a C closure is one of those, as the VM's shared globals hold it.
bool isFastCalled(lua_State* L, const Closure* cl)
{
    if (!cl->isC)
        return false;
    lua_State* main = Runtime::of(L).main();
    lua_pushvalue(main, LUA_GLOBALSINDEX);
    lua_xmove(main, L, 1);
    bool found = false;
    for (const char* name : fastGlobals) {
        lua_rawgetfield(L, -1, name);
        found = found || isFunction(L, -1, cl);
        lua_pop(L, 1);
    }
    for (const char* library : fastLibraries) {
        if (found)
            break;
        if (lua_rawgetfield(L, -1, library) == LUA_TTABLE) {
            lua_pushnil(L);
            while (!found && lua_next(L, -2)) {
                found = isFunction(L, -1, cl);
                lua_pop(L, 1);
            }
            if (found)
                lua_pop(L, 1); // the key lua_next left
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return found;
}

bool clearSafeEnv(void* /*context*/, lua_Page* /*page*/, GCObject* gco)
{
    if (gco->gch.tt == LUA_TTABLE)
        gco2h(gco)->safeenv = false;
    return false;
}

// Takes FASTCALL away from every script, now and from here on: no table is a
// safe environment any more, and Runtime::loadThread leaves new scripts' out
// too. Their calls of a builtin then go through the closure, and so through
// its hook. It costs every script its import cache as well (a global read is
// a lookup again), which is why only hooking a builtin does it.
void stopFastCalls(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    if (rt.builtinsHooked())
        return;
    rt.setBuiltinsHooked();
    luaM_visitgco(L, nullptr, clearSafeEnv);
}

// --- Hooking -------------------------------------------------------------------

// Hooks the closure at `target` with the function at `hook` (both absolute
// indices) and pushes what the closure did until now.
void pushHooked(lua_State* L, int target, int hook)
{
    Closure* cl = checkClosure(L, target);
    checkClosure(L, hook);
    const bool itself = lua_rawequal(L, target, hook) != 0;

    pushRecord(L, cl);
    const int record = lua_gettop(L);
    const bool hooked = lua_type(L, record) == LUA_TTABLE;
    Closure* before = pushCopy(L, cl);
    const int previous = lua_gettop(L);
    retarget(L, cl, before);
    // A function hooked with itself goes on doing what it did, through its
    // copy, and is hooked all the same (restorefunction takes it off), as the
    // executors' is [sUNC's restorefunction hooks coroutine.resume with
    // itself]. Calling itself would never end.
    if (itself)
        hook = previous;

    if (hooked) {
        // What's handed back calls the hook that was there, so hooks stack: two
        // scripts each hooking __namecall and calling what they got both run.
        // It gets a record of its own over the same original.
        lua_createtable(L, 0, 2);
        lua_rawgetfield(L, record, "hook");
        lua_rawsetfield(L, -2, "hook");
        lua_rawgetfield(L, record, "real");
        lua_rawsetfield(L, -2, "real");
        setRecord(L, previous);
        lua_pushvalue(L, hook);
        lua_rawsetfield(L, record, "hook");
    } else {
        lua_createtable(L, 0, 2);
        lua_pushvalue(L, hook);
        lua_rawsetfield(L, -2, "hook");
        lua_pushvalue(L, previous);
        lua_rawsetfield(L, -2, "real");
        setRecord(L, target);
    }

    if (isFastCalled(L, cl))
        stopFastCalls(L);
    if (cl->isC) {
        cl->c.f = hookCall;
        cl->c.cont = hookDone;
    } else if (!hookInPlace(L, cl, clvalue(luaA_toobject(L, hook)))) {
        Proto* p = pushTrampoline(L, hook, cl->nupvalues);
        cl->l.p = p;
        cl->stacksize = p->maxstacksize;
        luaC_objbarrier(L, cl, p);
        lua_pop(L, 1);
    }
    lua_remove(L, record);
}

// hookfunction(f, hook) (replaceclosure): f calls hook from now on, wherever
// it's called from; answers what f did before, to call on to.
int hookFunction(lua_State* L)
{
    lua_settop(L, 2);
    pushHooked(L, 1, 2);
    return 1;
}

// hookmetamethod(object, name, hook): hookfunction on the function the
// object's metatable has under that name, whatever __metatable says. The
// metatable of an Instance is every Instance's, so a __namecall hook sees
// every method call any script makes.
int hookMetamethod(lua_State* L)
{
    luaL_checkany(L, 1);
    const std::string name{checkString(L, 2)};
    checkClosure(L, 3);
    lua_settop(L, 3);
    if (!lua_getmetatable(L, 1))
        raise(L, "hookmetamethod: the object has no metatable");
    if (lua_rawgetfield(L, 4, name.c_str()) != LUA_TFUNCTION)
        raise(L, std::format("hookmetamethod: its metatable has no function {}", name));
    pushHooked(L, 5, 3);
    return 1;
}

// restorefunction(f): takes f's hook off, so it does what it did before it
// was first hooked. A frame now in the hook finishes it.
int restoreFunction(lua_State* L)
{
    Closure* cl = checkClosure(L, 1);
    lua_settop(L, 1);
    pushRecord(L, cl);
    if (lua_type(L, 2) != LUA_TTABLE)
        raise(L, "restorefunction: the function isn't hooked");
    lua_rawgetfield(L, 2, "real");
    const Closure* real = clvalue(luaA_toobject(L, 3));
    Closure* before = pushCopy(L, cl);
    retarget(L, cl, before);
    putBack(L, cl, real);
    lua_pushnil(L);
    setRecord(L, 1);
    return 0;
}

// isfunctionhooked(f): whether f has a hook on it.
int isFunctionHooked(lua_State* L)
{
    pushRecord(L, checkClosure(L, 1));
    lua_pushboolean(L, lua_type(L, -1) == LUA_TTABLE);
    return 1;
}

// --- getgc ---------------------------------------------------------------------

void pushObject(lua_State* L, GCObject* gco)
{
    luaL_checkstack(L, 1, nullptr);
    luaC_threadbarrier(L);
    L->top->value.gc = gco;
    L->top->tt = gco->gch.tt;
    L->top++;
}

struct Collected {
    global_State* g;
    bool tables;
    std::vector<GCObject*> objects;
};

bool collect(void* context, lua_Page* /*page*/, GCObject* gco)
{
    auto& found = *static_cast<Collected*>(context);
    const auto type = gco->gch.tt;
    if (!isdead(found.g, gco)
        && (type == LUA_TFUNCTION || type == LUA_TUSERDATA || (found.tables && type == LUA_TTABLE)))
        found.objects.push_back(gco);
    return false;
}

// getgc(includeTables): every function and userdata the VM holds, and its
// tables too when asked. One the sweep is about to free is left out (handed
// out, it would come back half gone); one the mark hasn't reached yet is
// reachable again from the list, which the barrier tells the collector. So
// no collection first, which on a big heap costs more than the walk.
int getgc(lua_State* L)
{
    Collected found{L->global, lua_toboolean(L, 1) != 0, {}};
    luaM_visitgco(L, &found, collect);
    lua_createtable(L, static_cast<int>(found.objects.size()), 0);
    int i = 0;
    for (GCObject* gco : found.objects) {
        pushObject(L, gco);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// --- filtergc ------------------------------------------------------------------

// The values of the list at options[field], each kept alive by the list.
std::vector<TValue> listOf(lua_State* L, int options, const char* field)
{
    std::vector<TValue> values;
    if (lua_rawgetfield(L, options, field) == LUA_TTABLE) {
        const int list = lua_gettop(L);
        for (int i = 1; lua_rawgeti(L, list, i) != LUA_TNIL; ++i) {
            values.push_back(*luaA_toobject(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return values;
}

struct FunctionFilter {
    std::optional<std::string> name, hash;
    bool ignoreExecutor = true;
    std::vector<TValue> constants, upvalues;
    const LuaTable* genv = nullptr;
    const TString* index = nullptr; // "__index"
};

std::string_view nameOf(const Closure* cl)
{
    if (cl->isC)
        return cl->c.debugname ? std::string_view(getstr(cl->c.debugname), cl->c.debugname->len) : std::string_view{};
    const TString* name = cl->l.p->debugname;
    return name ? std::string_view(getstr(name), name->len) : std::string_view{};
}

// What isexecutorclosure answers: made in the console's globals or a run's.
bool isOurs(const FunctionFilter& filter, const Closure* cl)
{
    const LuaTable* env = cl->env;
    if (!env || !filter.genv)
        return false;
    if (env == filter.genv)
        return true;
    if (!env->metatable)
        return false;
    const TValue* index = luaH_getstr(env->metatable, const_cast<TString*>(filter.index));
    return ttistable(index) && hvalue(index) == filter.genv;
}

// Raw equality, but a NaN is one: a function holding 0/0 is found by it.
bool same(const TValue& a, const TValue& b)
{
    if (ttisnumber(&a) && ttisnumber(&b) && std::isnan(nvalue(&a)) && std::isnan(nvalue(&b)))
        return true;
    return luaO_rawequalObj(&a, &b) != 0;
}

bool holdsAll(std::span<const TValue> held, std::span<const TValue> wanted)
{
    return std::ranges::all_of(wanted, [&](const TValue& want) {
        return std::ranges::any_of(held, [&](const TValue& have) { return same(have, want); });
    });
}

bool matches(const FunctionFilter& filter, const Closure* cl)
{
    if (filter.name && nameOf(cl) != *filter.name)
        return false;
    if (filter.ignoreExecutor && isOurs(filter, cl))
        return false;
    if (!filter.hash && filter.constants.empty() && filter.upvalues.empty())
        return true;
    if (cl->isC)
        return false; // none of those are a C function's
    const Proto* p = cl->l.p;
    if (filter.hash && functionHash(p) != *filter.hash)
        return false;
    if (!holdsAll(std::span(p->k, static_cast<std::size_t>(p->sizek)), filter.constants))
        return false;
    std::vector<TValue> upvalues;
    for (int i = 0; i < cl->nupvalues; ++i) {
        const TValue* v = &cl->l.uprefs[i];
        upvalues.push_back(ttisupval(v) ? *upvalue(v)->v : *v);
    }
    return holdsAll(upvalues, filter.upvalues);
}

// Whether the table on top matches the table filter in `options`.
bool tableMatches(lua_State* L, int options)
{
    const int table = lua_gettop(L);
    bool ok = true;
    if (lua_rawgetfield(L, options, "Keys") == LUA_TTABLE) {
        for (int i = 1; ok && lua_rawgeti(L, -1, i) != LUA_TNIL; ++i) {
            lua_rawget(L, table);
            ok = !lua_isnil(L, -1);
            lua_pop(L, 1);
        }
        if (ok)
            lua_pop(L, 1); // the nil that ended the list
    }
    lua_settop(L, table);
    if (ok && lua_rawgetfield(L, options, "Values") == LUA_TTABLE) {
        const int wanted = lua_gettop(L);
        for (int i = 1; ok && lua_rawgeti(L, wanted, i) != LUA_TNIL; ++i) {
            bool found = false;
            lua_pushnil(L);
            while (!found && lua_next(L, table)) {
                found = lua_rawequal(L, -1, wanted + 1);
                lua_pop(L, 1);
            }
            if (found)
                lua_pop(L, 1); // the key lua_next left
            ok = found;
            lua_pop(L, 1);
        }
    }
    lua_settop(L, table);
    if (ok && lua_rawgetfield(L, options, "KeyValuePairs") == LUA_TTABLE) {
        const int pairs = lua_gettop(L);
        lua_pushnil(L);
        while (ok && lua_next(L, pairs)) {
            lua_pushvalue(L, -2);
            lua_rawget(L, table);
            ok = lua_rawequal(L, -1, -2);
            lua_pop(L, 2);
        }
    }
    lua_settop(L, table);
    if (ok && lua_rawgetfield(L, options, "Metatable") != LUA_TNIL) {
        ok = lua_getmetatable(L, table) && lua_rawequal(L, -1, table + 1);
    }
    lua_settop(L, table);
    return ok;
}

// filtergc(type, options, returnOne): what getgc finds of one type ("function"
// or "table"), narrowed by the options: a function's Name, Hash
// (getfunctionhash's), Constants and Upvalues it holds all of, and whether
// one the console made counts (IgnoreExecutor, true unless false); a table's
// Keys, Values and KeyValuePairs it holds all of, and its raw Metatable. The
// first match (or nil) with returnOne, else a list of them. The options and
// their own lists are never a match.
int filtergc(lua_State* L)
{
    const std::string_view type = checkString(L, 1);
    if (type != "function" && type != "table")
        luaL_argerror(L, 1, "is neither \"function\" nor \"table\"");
    luaL_checktype(L, 2, LUA_TTABLE);
    const bool one = lua_toboolean(L, 3) != 0;
    lua_settop(L, 2);
    const int options = 2;

    Collected found{L->global, type == "table", {}}; // as getgc's
    luaM_visitgco(L, &found, collect);

    // What the options are made of, which never matches.
    std::vector<const GCObject*> own{gcvalue(luaA_toobject(L, options))};
    lua_pushnil(L);
    while (lua_next(L, options)) {
        if (lua_istable(L, -1))
            own.push_back(gcvalue(luaA_toobject(L, -1)));
        lua_pop(L, 1);
    }

    FunctionFilter filter;
    if (type == "function") {
        if (lua_rawgetfield(L, options, "Name") == LUA_TSTRING)
            filter.name = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (lua_rawgetfield(L, options, "Hash") == LUA_TSTRING)
            filter.hash = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_rawgetfield(L, options, "IgnoreExecutor");
        filter.ignoreExecutor = !(lua_isboolean(L, -1) && !lua_toboolean(L, -1));
        lua_pop(L, 1);
        filter.constants = listOf(L, options, "Constants");
        filter.upvalues = listOf(L, options, "Upvalues");
        Runtime& rt = Runtime::of(L);
        if (rt.consoleGlobals() != LUA_NOREF) {
            lua_getref(L, rt.consoleGlobals()); // kept on the stack
            filter.genv = hvalue(luaA_toobject(L, -1));
        }
        lua_pushliteral(L, "__index"); // kept on the stack
        filter.index = tsvalue(luaA_toobject(L, -1));
    }

    lua_newtable(L);
    const int results = lua_gettop(L);
    int count = 0;
    for (GCObject* gco : found.objects) {
        if (std::ranges::find(own, gco) != own.end())
            continue;
        bool match = false;
        if (type == "function") {
            match = gco->gch.tt == LUA_TFUNCTION && matches(filter, gco2cl(gco));
        } else if (gco->gch.tt == LUA_TTABLE && gco != gcvalue(luaA_toobject(L, results))) {
            pushObject(L, gco);
            match = tableMatches(L, options);
            lua_pop(L, 1);
        }
        if (!match)
            continue;
        pushObject(L, gco);
        if (one)
            return 1;
        lua_rawseti(L, results, ++count);
    }
    if (one)
        lua_pushnil(L);
    return 1;
}

// --- The scripts threads run for ------------------------------------------------

// getrunningscripts(): the scripts with a thread that hasn't finished: one
// running, suspended (waiting on task.wait, an event, a coroutine resume) or
// resuming another, and those whose code ran to its end with a connection
// still there to run it again. One with neither is done [sUNC: "Fetched a
// script with a thread that's no longer running"]; one out of the game, or
// destroyed, still runs [sUNC's ANG3LJ0RDAN and the one that sets its
// script global to nil].
// Only this VM's: one that started under an Actor runs in that Actor's VM in
// Roblox, so it isn't listed [sUNC: ReplicatedFirst.bleepbloop.suncinit,
// "Shouldn't return scripts initialized in an actor state"], while one that
// moved into an Actor once it ran still runs here [sUNC's "fake actor"].
int getrunningscripts(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    std::vector<InstancePtr> running;
    for (lua_State* thread : rt.threads()) {
        if (isdead(L->global, obj2gco(thread)))
            continue;
        const int status = lua_costatus(L, thread);
        if (status != LUA_CORUN && status != LUA_COSUS && status != LUA_CONOR)
            continue;
        InstancePtr script = rt.accessOf(thread).script.lock();
        if (script && !rt.startedInActor(*script) && std::ranges::find(running, script) == running.end())
            running.push_back(std::move(script));
    }
    for (const std::shared_ptr<Listener>& listener : rt.listeners()) {
        InstancePtr script = listener->access.script.lock();
        if (script && listener->connection.connected() && !rt.startedInActor(*script) &&
            std::ranges::find(running, script) == running.end())
            running.push_back(std::move(script));
    }
    lua_createtable(L, static_cast<int>(running.size()), 0);
    int i = 0;
    for (const InstancePtr& script : running) {
        pushInstance(L, script.get());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// getscriptfromthread(thread): the script the thread runs for, nil for the
// console's own (Runtime's ThreadAccess::script).
int getscriptfromthread(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTHREAD);
    InstancePtr script = Runtime::of(L).accessOf(lua_tothread(L, 1)).script.lock();
    pushInstance(L, script.get());
    return 1;
}

} // namespace

void openConsoleHooks(lua_State* thread)
{
    const auto set = [thread](const char* name, lua_CFunction function) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    };
    // One function under both names, so the two compare equal.
    lua_pushcfunction(thread, hookFunction, "hookfunction");
    lua_pushvalue(thread, -1);
    lua_setglobal(thread, "replaceclosure");
    lua_setglobal(thread, "hookfunction");
    set("hookmetamethod", hookMetamethod);
    set("restorefunction", restoreFunction);
    set("isfunctionhooked", isFunctionHooked);
    set("getgc", getgc);
    set("filtergc", filtergc);
    set("getrunningscripts", getrunningscripts);
    set("getscriptfromthread", getscriptfromthread);
}

} // namespace lunar::lua
