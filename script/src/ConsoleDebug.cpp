// The console's own debug table (Console.cpp opens it): what the executors
// read and write of a function's compiled form -- its constants, its inner
// functions, the registers of a running frame -- over the VM's debug library,
// whose own members are copied across so debug.traceback and debug.info keep
// working.
//
// This is the one file of the console's that reads the VM's own structures.
// Luau keeps Closure and Proto in lobject.h and the frames of a running thread
// in lstate.h; both are the VM's headers, not its API, so everything that
// touches them is here and nowhere else.
//
// What the constants and the inner functions are is what the compiler wrote:
// the constants of a function are its Proto's table in index order, and the
// functions written inside it are its Proto's children in the same order.

#include "Console.hpp"
#include "Runtime.hpp"

#include <Luau/Bytecode.h>
#include <lapi.h>
#include <lfunc.h>
#include <lgc.h>
#include <lmem.h>
#include <lobject.h>
#include <lstate.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

Proto* copyProto(lua_State* L, const Proto* from, int sizecode)
{
    Proto* f = luaF_newproto(L);
    f->nups = from->nups;
    f->numparams = from->numparams;
    f->is_vararg = from->is_vararg;
    f->maxstacksize = from->maxstacksize;
    f->flags = from->flags;
    f->source = from->source;
    f->debugname = from->debugname;
    f->linedefined = from->linedefined;
    f->bytecodeid = from->bytecodeid;

    f->k = luaM_newarray(L, static_cast<std::size_t>(from->sizek), TValue, f->memcat);
    for (int i = 0; i < from->sizek; ++i)
        setobj(L, &f->k[i], &from->k[i]);
    f->sizek = from->sizek;
    f->p = luaM_newarray(L, static_cast<std::size_t>(from->sizep), Proto*, f->memcat);
    std::copy_n(from->p, from->sizep, f->p);
    f->sizep = from->sizep;
    f->locvars = luaM_newarray(L, static_cast<std::size_t>(from->sizelocvars), LocVar, f->memcat);
    std::copy_n(from->locvars, from->sizelocvars, f->locvars);
    f->sizelocvars = from->sizelocvars;
    f->upvalues = luaM_newarray(L, static_cast<std::size_t>(from->sizeupvalues), TString*, f->memcat);
    std::copy_n(from->upvalues, from->sizeupvalues, f->upvalues);
    f->sizeupvalues = from->sizeupvalues;

    // The lines are one per instruction, so they stay the original's only
    // when the length does.
    sizecode = std::max(from->sizecode, sizecode);
    f->code = luaM_newarray(L, static_cast<std::size_t>(sizecode), Instruction, f->memcat);
    std::copy_n(from->code, from->sizecode, f->code);
    std::fill(f->code + from->sizecode, f->code + sizecode, Instruction{LOP_NOP});
    f->sizecode = sizecode;
    f->codeentry = f->code + (from->codeentry - from->code);
    if (from->lineinfo && sizecode == from->sizecode) {
        f->lineinfo = luaM_newarray(L, static_cast<std::size_t>(from->sizelineinfo), std::uint8_t, f->memcat);
        std::copy_n(from->lineinfo, from->sizelineinfo, f->lineinfo);
        f->sizelineinfo = from->sizelineinfo;
        // The absolute lines follow the gaps, aligned as the VM laid them out.
        const std::ptrdiff_t absolute = reinterpret_cast<const std::uint8_t*>(from->abslineinfo) - from->lineinfo;
        f->abslineinfo = static_cast<int*>(static_cast<void*>(f->lineinfo + absolute));
        f->linegaplog2 = from->linegaplog2;
    }
    return f;
}

namespace {

// The closure at idx, or an argument error.
Closure* checkClosure(lua_State* L, int idx)
{
    if (lua_type(L, idx) != LUA_TFUNCTION)
        luaL_typeerror(L, idx, "function");
    return clvalue(luaA_toobject(L, idx));
}

// The proto of a Luau function. Null for a C closure, which has no code.
Proto* luaProto(lua_State* L, int idx)
{
    Closure* closure = checkClosure(L, idx);
    return closure->isC ? nullptr : closure->l.p;
}

// checkproto(L, idx): the proto, with an error when there is none.
Proto* checkProto(lua_State* L, int idx)
{
    if (Proto* proto = luaProto(L, idx))
        return proto;
    luaL_typeerror(L, idx, "Luau function");
}

// checkindex(L, idx): a 1-based index, as the executors' number theirs from
// 1. An index past the end of whatever it points into is a reading of nothing
// there, which the callers answer with nil rather than an error; one below 1 is
// no index at all.
int checkIndex(lua_State* L, int idx)
{
    const lua_Integer n = luaL_checkinteger(L, idx);
    if (n < 1)
        luaL_argerror(L, idx, "is not an index (they start at 1)");
    return static_cast<int>(n) - 1;
}

// --- Constants -------------------------------------------------------------

// A constant as the executors hand it out: what the compiler wrote as a value
// (nil, a boolean, a number, an integer, a string or a vector). The rest of the table is
// the loader's: an import (a global such as print, which it resolved to the
// function) or a closure or table template, and those read as nil.
void pushConstant(lua_State* L, const TValue& constant)
{
    switch (ttype(&constant)) {
    case LUA_TBOOLEAN:
    case LUA_TNUMBER:
    case LUA_TINTEGER:
    case LUA_TSTRING:
    case LUA_TVECTOR:
        luaA_pushvalue(L, &constant);
        break;
    default:
        lua_pushnil(L);
    }
}

// debug.getconstant(function, n): the nth constant of the function's code.
// An n past them is an error, as the executors' is [Myriad counts them that
// way]; a constant that is no value of its own (an import) is nil.
int getConstant(lua_State* L)
{
    Proto* proto = checkProto(L, 1);
    const int index = checkIndex(L, 2);
    if (index >= proto->sizek)
        luaL_argerror(L, 2, "constant index out of range");
    pushConstant(L, proto->k[index]);
    return 1;
}

// debug.getconstants(function): all of them, in the order they are indexed,
// so a nil in the middle of them leaves a hole rather than shortening the run.
int getConstants(lua_State* L)
{
    Proto* proto = checkProto(L, 1);
    lua_createtable(L, proto->sizek, 0);
    for (int i = 0; i < proto->sizek; ++i) {
        pushConstant(L, proto->k[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// debug.setconstant(function, n, value): the nth constant becomes `value`.
// Only what the compiler itself puts in a constant table can go there: nil, a
// boolean, a number, a string or a vector. The interpreter reads the table as
// it runs, so the function answers with the new value from its next call.
int setConstant(lua_State* L)
{
    Proto* proto = checkProto(L, 1);
    const int index = checkIndex(L, 2);
    if (index >= proto->sizek)
        return 0; // nothing there to change
    switch (lua_type(L, 3)) {
    case LUA_TNIL:
    case LUA_TBOOLEAN:
    case LUA_TNUMBER:
    case LUA_TSTRING:
    case LUA_TVECTOR:
        break;
    default:
        luaL_argerror(L, 3, "is not something a constant can be");
    }
    lua_pushvalue(L, 3);
    StkId value = L->base + (lua_gettop(L) - 1);
    setobj(L, &proto->k[index], value);
    luaC_barrier(L, proto, value);
    return 0;
}

// --- Inner functions -------------------------------------------------------

// A copy of `from` that reads as it does -- its name, lines, constants, inner
// functions, locals and upvalue names are the same -- but whose code raises
// as it starts ("attempt to call a nil value"): what an inactive proto is to
// the executors, a function to look at that isn't for calling [sUNC:
// "Inactive protos should not be callable"].
Proto* uncallableCopy(lua_State* L, const Proto* from)
{
    // LOADNIL R0, CALL R0 with no arguments and no results; the rest as it
    // was, never reached.
    Proto* f = copyProto(L, from, 2);
    f->maxstacksize = std::max<std::uint8_t>(f->maxstacksize, 1);
    f->code[0] = LOP_LOADNIL;
    f->code[1] = LOP_CALL | (1u << 16) | (1u << 24);
    return f;
}

// A closure over a copy of one of a function's own inner functions
// (uncallableCopy), in the environment the outer one has. It has the
// upvalues the inner function uses, each a box of its own holding nil, so
// debug.getupvalue and debug.setupvalue read and write it as they would the
// running code's [sUNC's tests crashed the VM writing one that wasn't a box].
// A box is made as the VM makes one for a local, then closed as the local's
// scope ending closes it.
void pushInner(lua_State* L, Closure* outer, Proto* proto)
{
    lua_checkstack(L, 2);
    luaC_threadbarrier(L);
    Proto* copy = uncallableCopy(L, proto);
    Closure* closure = luaF_newLclosure(L, copy->nups, outer->env, copy);
    setclvalue(L, L->top, closure);
    L->top++;
    for (int i = 0; i < copy->nups; ++i) {
        lua_pushnil(L);
        StkId local = L->top - 1;
        UpVal* box = luaF_findupval(L, local);
        setupvalue(L, &closure->l.uprefs[i], box);
        luaC_objbarrier(L, closure, box);
        luaF_close(L, local);
        lua_pop(L, 1);
    }
}

struct ClosuresOf {
    global_State* g;
    const Proto* proto;
    std::vector<Closure*> found;
};

bool collectClosuresOf(void* context, lua_Page* /*page*/, GCObject* gco)
{
    auto& closures = *static_cast<ClosuresOf*>(context);
    if (gco->gch.tt == LUA_TFUNCTION && !isdead(closures.g, gco) && !gco2cl(gco)->isC
        && gco2cl(gco)->l.p == closures.proto)
        closures.found.push_back(gco2cl(gco));
    return false;
}

// The closures the VM has made of `proto` that are still alive, as a table in
// the order the heap holds them (getproto's own are of copies of it). A dead one is the sweep's, never handed out;
// one the mark hasn't reached yet is reachable again from this stack, which
// the barrier tells the collector.
void pushClosuresOf(lua_State* L, const Proto* proto)
{
    ClosuresOf closures{L->global, proto, {}};
    luaM_visitgco(L, &closures, collectClosuresOf);
    lua_createtable(L, static_cast<int>(closures.found.size()), 0);
    int i = 0;
    for (Closure* closure : closures.found) {
        luaL_checkstack(L, 1, nullptr);
        luaC_threadbarrier(L);
        setclvalue(L, L->top, closure);
        L->top++;
        lua_rawseti(L, -2, ++i);
    }
}

// debug.getproto(function, n, true): the nth function written inside it, as a
// table of the closures the running code has made of it (the ones it hands
// out: the same values). Without the third argument it is a closure of its
// own over a copy of that function, made here, which is for reading (its
// name, its constants) and not for calling (pushInner).
int getProto(lua_State* L)
{
    Closure* outer = checkClosure(L, 1);
    Proto* proto = luaProto(L, 1);
    if (!proto)
        raise(L, "a C closure has no code");
    const int index = checkIndex(L, 2);
    if (index >= proto->sizep)
        raise(L, std::format("the function has {} inner functions", proto->sizep));
    if (lua_toboolean(L, 3))
        pushClosuresOf(L, proto->p[index]);
    else
        pushInner(L, outer, proto->p[index]);
    return 1;
}

// debug.getprotos(function): the functions written inside it, in the order
// they are indexed, each as getproto(fn, n) gives it.
int getProtos(lua_State* L)
{
    Closure* outer = checkClosure(L, 1);
    Proto* proto = luaProto(L, 1);
    if (!proto)
        raise(L, "a C closure has no code");
    lua_createtable(L, proto->sizep, 0);
    for (int i = 0; i < proto->sizep; ++i) {
        pushInner(L, outer, proto->p[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// --- Upvalues --------------------------------------------------------------

// A Luau function, for the upvalue readers: a C closure's upvalues are the
// client's own business, so they have none to hand out.
void checkLuauClosure(lua_State* L, int idx)
{
    if (checkClosure(L, idx)->isC)
        luaL_argerror(L, idx, "Luau function expected, got a C closure");
}

// debug.getupvalue(function, n): the nth upvalue the function closes over. An
// n it has none at is an error, as the executors' is [Myriad counts them that
// way].
int getUpvalue(lua_State* L)
{
    checkLuauClosure(L, 1);
    if (lua_getupvalue(L, 1, checkIndex(L, 2) + 1))
        return 1;
    luaL_argerror(L, 2, "upvalue index out of range");
}

// debug.getupvalues(function): all of them, in index order.
int getUpvalues(lua_State* L)
{
    checkLuauClosure(L, 1);
    lua_createtable(L, 0, 0);
    for (int n = 1;; ++n) {
        if (!lua_getupvalue(L, 1, n))
            break;
        lua_rawseti(L, -2, n);
    }
    return 1;
}

// debug.setupvalue(function, n, value): what the nth upvalue closes over
// becomes `value`, so the function answers with it from its next call. An n
// it has no upvalue at is an error: nothing would change.
int setUpvalue(lua_State* L)
{
    const Closure* closure = checkClosure(L, 1);
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_checkany(L, 3);
    if (n < 1 || n > closure->nupvalues)
        luaL_argerror(L, 2, "upvalue index out of range");
    // lua_setupvalue reads the value off the top of the stack.
    lua_checkstack(L, 2);
    lua_pushvalue(L, 3);
    if (!lua_setupvalue(L, 1, static_cast<int>(n)))
        lua_pop(L, 1);
    return 0;
}

// --- The stack of a running frame -----------------------------------------

// The frame `level` calls up from here: 0 is the console function itself, 1
// whatever called it, as the executors' getstack numbers them.
CallInfo* checkFrame(lua_State* L, int level)
{
    if (level < 0 || static_cast<unsigned>(level) >= static_cast<unsigned>(L->ci - L->base_ci))
        luaL_argerror(L, 1, "is not a level of a running call");
    // [sUNC: getstack(0) and setstack(0, 1, 0), getstack's own frame, raise]
    CallInfo* frame = L->ci - level;
    if (!isLua(frame))
        raise(L, "a C function has no stack");
    return frame;
}

// How many registers the frame has: what its code reserves.
int frameSize(const CallInfo& frame)
{
    return static_cast<int>(frame.top - frame.base);
}

// The frame `level` calls up from here, again: a frame's registers move when
// the thread's stack grows, which pushing can make it do.
CallInfo* frameAt(lua_State* L, int level)
{
    return L->ci - level;
}

// debug.getstack(level, index): the register at `index` of that frame, counted
// from 1 as the executors count them. Without the index, the whole frame as a
// table of registers.
int getStack(lua_State* L)
{
    const int level = static_cast<int>(luaL_checkinteger(L, 1));
    CallInfo* frame = checkFrame(L, level);
    if (lua_isnoneornil(L, 2)) {
        const int size = frameSize(*frame);
        lua_checkstack(L, size + 4);
        lua_createtable(L, size, 0);
        for (int i = 0; i < size; ++i) {
            const TValue value = frameAt(L, level)->base[i];
            luaA_pushvalue(L, &value);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    const int index = checkIndex(L, 2);
    if (index >= frameSize(*frame)) {
        lua_pushnil(L);
        return 1;
    }
    const TValue value = frame->base[index];
    luaA_pushvalue(L, &value);
    return 1;
}

// debug.setstack(level, index, value): what register `index` holds becomes
// `value`. The frame is one the interpreter is running, so this is what a
// hooked function is for: change a register and the running code reads the new
// value from it. Answers with what it wrote.
int setStack(lua_State* L)
{
    const int level = static_cast<int>(luaL_checkinteger(L, 1));
    CallInfo* frame = checkFrame(L, level);
    const int index = checkIndex(L, 2);
    if (index >= frameSize(*frame))
        raise(L, "the frame has no such register");
    luaL_checkany(L, 3);
    lua_checkstack(L, 2);
    lua_pushvalue(L, 3);
    luaC_threadbarrier(L);
    setobj2s(L, &frameAt(L, level)->base[index], L->base + (lua_gettop(L) - 1));
    lua_pushvalue(L, 3);
    return 1;
}

// --- Info ------------------------------------------------------------------

// debug.getinfo(target): what the VM knows about a function or a level, as the
// table the executors hand back. The names are Roblox's, which is where the
// executors take them from; is_vararg is a number because Roblox's is.
int getInfo(lua_State* L)
{
    lua_Debug info{};
    int level = 0;
    if (lua_isnumber(L, 1)) {
        level = static_cast<int>(luaL_checkinteger(L, 1));
    } else if (lua_type(L, 1) == LUA_TFUNCTION) {
        level = -lua_gettop(L);
    } else {
        luaL_typeerror(L, 1, "function or level");
    }
    // 's' the source, 'l' the line, 'a' the parameters and vararg flag, 'u' the
    // upvalue count, 'n' the name and 'f' the closure itself.
    if (!lua_getinfo(L, level, "slaunf", &info))
        return 0;
    const int closure = lua_gettop(L); // what 'f' left, which goes in the table
    lua_createtable(L, 0, 10);
    const int table = lua_gettop(L);
    const auto set = [&](const char* name) {
        lua_pushstring(L, name);
        lua_insert(L, table + 1);
        lua_rawset(L, table);
    };
    lua_pushstring(L, info.source ? info.source : "");
    set("source");
    lua_pushstring(L, info.short_src ? info.short_src : "");
    set("short_src");
    lua_pushstring(L, info.what ? info.what : "");
    set("what");
    lua_pushinteger(L, info.currentline);
    set("currentline");
    lua_pushstring(L, info.name ? info.name : "");
    set("name");
    lua_pushinteger(L, info.nupvals);
    set("nups");
    lua_pushinteger(L, info.nparams);
    set("numparams");
    lua_pushinteger(L, info.isvararg);
    set("is_vararg");
    lua_pushvalue(L, closure);
    set("func");
    return 1;
}

} // namespace

// Whether a value is a Luau closure rather than one of the client's C
// functions, which is what a connection's LuaConnection says of its callback.
bool consoleIsLuauClosure(lua_State* L, int idx)
{
    return lua_type(L, idx) == LUA_TFUNCTION && checkClosure(L, idx)->isC == 0;
}

void openConsoleDebug(lua_State* thread)
{
    // The console's debug table starts as a copy of the VM's own: the game
    // scripts keep the library exactly as it was (writing into that one is
    // not an option: game scripts see it too), and the console sees the copy
    // plus what goes on it.
    lua_newtable(thread);
    const int table = lua_gettop(thread);
    lua_getglobal(thread, "debug");
    if (lua_istable(thread, -1)) {
        lua_pushnil(thread);
        while (lua_next(thread, -2)) {
            lua_pushvalue(thread, -2); // the name
            lua_pushvalue(thread, -2); // what's under it
            lua_rawset(thread, table);
            lua_pop(thread, 1);
        }
    }
    lua_pop(thread, 1);

    for (const auto& [name, function] :
         {std::pair{"getconstant", getConstant},
          std::pair{"getconstants", getConstants},
          std::pair{"setconstant", setConstant},
          std::pair{"getproto", getProto},
          std::pair{"getprotos", getProtos},
          std::pair{"getupvalue", getUpvalue},
          std::pair{"getupvalues", getUpvalues},
          std::pair{"setupvalue", setUpvalue},
          std::pair{"getstack", getStack},
          std::pair{"setstack", setStack},
          std::pair{"getinfo", getInfo}}) {
        lua_pushcfunction(thread, function, name);
        lua_setfield(thread, table, name);
    }
    lua_setglobal(thread, "debug");
}

} // namespace lunar::lua