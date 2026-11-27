// SharedTable, the table Actors share in parallel Luau, and
// SharedTableRegistry's GetSharedTable and SetSharedTable.
//
// Every Actor here runs on the one VM (task.desynchronize doesn't leave the
// thread), so a SharedTable is a userdata standing for a plain Luau table
// that holds its entries, kept in a weak-keyed table in the registry. Keys
// are strings and numbers; a table assigned in becomes a SharedTable of its
// own (as SharedTable.new does), functions and threads are refused.

#include "Runtime.hpp"
#include "Userdata.hpp"

#include <lualib.h>

namespace lunar::lua {

namespace {

void setFunction(lua_State* L, const char* name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

struct SharedTableData {
    bool frozen = false;
};

constexpr const char* entriesKey = "lunar.sharedtable.entries";
constexpr const char* registryKey = "lunar.sharedtableregistry";

SharedTableData* toShared(lua_State* L, int idx)
{
    return static_cast<SharedTableData*>(lua_touserdatatagged(L, idx, SharedTableTag));
}

SharedTableData& checkShared(lua_State* L, int idx)
{
    SharedTableData* shared = toShared(L, idx);
    if (!shared)
        luaL_typeerrorL(L, idx, "SharedTable");
    return *shared;
}

// A table in the registry, made (weak-keyed or not) on first use.
void pushRegistryTable(lua_State* L, const char* key, bool weakKeys)
{
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    if (weakKeys) {
        lua_newtable(L);
        lua_pushliteral(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, key);
}

// The plain table holding the SharedTable at idx's entries.
void pushEntries(lua_State* L, int idx)
{
    idx = lua_absindex(L, idx);
    pushRegistryTable(L, entriesKey, true);
    lua_pushvalue(L, idx);
    lua_rawget(L, -2);
    lua_remove(L, -2);
}

void pushNewShared(lua_State* L)
{
    new (lua_newuserdatataggedwithmetatable(L, sizeof(SharedTableData), SharedTableTag)) SharedTableData{};
    pushRegistryTable(L, entriesKey, true);
    lua_pushvalue(L, -2);
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

void checkKey(lua_State* L, int idx)
{
    const int type = lua_type(L, idx);
    if (type != LUA_TSTRING && type != LUA_TNUMBER)
        luaL_error(L, "SharedTable keys must be strings or numbers, got %s", luaL_typename(L, idx));
}

void pushConverted(lua_State* L, int idx);

// The table at idx as a new SharedTable, its tables turned too.
void pushFromTable(lua_State* L, int idx)
{
    idx = lua_absindex(L, idx);
    pushNewShared(L);
    pushEntries(L, -1);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        checkKey(L, -2);
        pushConverted(L, -1);
        lua_pushvalue(L, -3);
        lua_insert(L, -2);
        lua_rawset(L, -5);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// What a SharedTable keeps for the value at idx: a table becomes a
// SharedTable, functions and threads can't cross between Actors.
void pushConverted(lua_State* L, int idx)
{
    switch (lua_type(L, idx)) {
    case LUA_TTABLE:
        pushFromTable(L, idx);
        return;
    case LUA_TFUNCTION:
    case LUA_TTHREAD:
        luaL_error(L, "%s is not a valid SharedTable value", luaL_typename(L, idx));
    default:
        lua_pushvalue(L, idx);
    }
}

// A copy of the SharedTable at idx, its SharedTables copied too when deep.
void pushClone(lua_State* L, int idx, bool deep, bool freeze)
{
    idx = lua_absindex(L, idx);
    pushNewShared(L);
    toShared(L, -1)->frozen = freeze;
    pushEntries(L, -1);
    pushEntries(L, idx);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pushvalue(L, -2);
        if (deep && toShared(L, -2))
            pushClone(L, -2, true, freeze);
        else
            lua_pushvalue(L, -2);
        lua_rawset(L, -6);
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

void checkWritable(lua_State* L, const SharedTableData& shared)
{
    if (shared.frozen)
        luaL_error(L, "attempt to modify a frozen SharedTable");
}

int sharedIndex(lua_State* L)
{
    checkShared(L, 1);
    pushEntries(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

int sharedNewIndex(lua_State* L)
{
    checkWritable(L, checkShared(L, 1));
    checkKey(L, 2);
    pushEntries(L, 1);
    lua_pushvalue(L, 2);
    pushConverted(L, 3);
    lua_rawset(L, -3);
    return 0;
}

int sharedLen(lua_State* L)
{
    checkShared(L, 1);
    pushEntries(L, 1);
    lua_pushinteger(L, lua_objlen(L, -1));
    return 1;
}

// for k, v in shared: next over the entries.
int sharedIter(lua_State* L)
{
    checkShared(L, 1);
    lua_getglobal(L, "next");
    pushEntries(L, 1);
    return 2;
}

int sharedToString(lua_State* L)
{
    lua_pushliteral(L, "SharedTable");
    return 1;
}

// SharedTable.new([t])
int sharedNew(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
        pushNewShared(L);
    else {
        luaL_checktype(L, 1, LUA_TTABLE);
        pushFromTable(L, 1);
    }
    return 1;
}

int sharedClear(lua_State* L)
{
    checkWritable(L, checkShared(L, 1));
    pushRegistryTable(L, entriesKey, true);
    lua_pushvalue(L, 1);
    lua_newtable(L);
    lua_rawset(L, -3);
    return 0;
}

int sharedCloneFn(lua_State* L)
{
    checkShared(L, 1);
    pushClone(L, 1, luaL_optboolean(L, 2, false), false);
    return 1;
}

int sharedCloneAndFreeze(lua_State* L)
{
    checkShared(L, 1);
    pushClone(L, 1, luaL_optboolean(L, 2, false), true);
    return 1;
}

int sharedIsFrozen(lua_State* L)
{
    lua_pushboolean(L, checkShared(L, 1).frozen);
    return 1;
}

int sharedSize(lua_State* L)
{
    checkShared(L, 1);
    pushEntries(L, 1);
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        ++count;
        lua_pop(L, 1);
    }
    lua_pushinteger(L, count);
    return 1;
}

// SharedTable.increment(st, key, delta): adds delta and returns the value
// from before (a missing entry counts as 0).
int sharedIncrement(lua_State* L)
{
    checkWritable(L, checkShared(L, 1));
    checkKey(L, 2);
    const double delta = luaL_checknumber(L, 3);
    pushEntries(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1) && !lua_isnumber(L, -1))
        luaL_error(L, "SharedTable.increment: the value at that key isn't a number");
    const double before = lua_isnil(L, -1) ? 0.0 : lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_pushnumber(L, before + delta);
    lua_rawset(L, -3);
    lua_pushnumber(L, before);
    return 1;
}

// SharedTable.update(st, key, fn): the entry becomes fn(old); nil removes it.
int sharedUpdate(lua_State* L)
{
    checkWritable(L, checkShared(L, 1));
    checkKey(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    pushEntries(L, 1); // 4
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 2);
    lua_rawget(L, 4);
    lua_call(L, 1, 1); // 5
    lua_pushvalue(L, 2);
    pushConverted(L, 5);
    lua_rawset(L, 4);
    return 0;
}

// SharedTableRegistry:GetSharedTable(name): the one set under that name,
// or a new empty one, kept under it from then on.
int registryGet(lua_State* L)
{
    luaL_checkstring(L, 2);
    pushRegistryTable(L, registryKey, false);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (toShared(L, -1))
        return 1;
    lua_pop(L, 1);
    pushNewShared(L);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    return 1;
}

// SharedTableRegistry:SetSharedTable(name, st): nil forgets the name.
int registrySet(lua_State* L)
{
    luaL_checkstring(L, 2);
    if (!lua_isnoneornil(L, 3))
        checkShared(L, 3);
    pushRegistryTable(L, registryKey, false);
    lua_pushvalue(L, 2);
    if (lua_isnoneornil(L, 3))
        lua_pushnil(L);
    else
        lua_pushvalue(L, 3);
    lua_rawset(L, -3);
    return 0;
}

} // namespace

bool isSharedTable(lua_State* L, int idx)
{
    return toShared(L, idx) != nullptr;
}

int sharedTableRegistryGet(lua_State* L)
{
    return registryGet(L);
}

int sharedTableRegistrySet(lua_State* L)
{
    return registrySet(L);
}

void openSharedTable(lua_State* L)
{
    lua_newtable(L);
    pushMetamethod(L, sharedIndex);
    lua_setfield(L, -2, "__index");
    pushMetamethod(L, sharedNewIndex);
    lua_setfield(L, -2, "__newindex");
    pushMetamethod(L, sharedLen);
    lua_setfield(L, -2, "__len");
    pushMetamethod(L, sharedIter);
    lua_setfield(L, -2, "__iter");
    pushMetamethod(L, sharedToString);
    lua_setfield(L, -2, "__tostring");
    registerType(L, SharedTableTag, "SharedTable");

    lua_newtable(L);
    setFunction(L, "new", sharedNew);
    setFunction(L, "clear", sharedClear);
    setFunction(L, "clone", sharedCloneFn);
    setFunction(L, "cloneAndFreeze", sharedCloneAndFreeze);
    setFunction(L, "isFrozen", sharedIsFrozen);
    setFunction(L, "size", sharedSize);
    setFunction(L, "increment", sharedIncrement);
    setFunction(L, "update", sharedUpdate);
    lua_setglobal(L, "SharedTable");
}

} // namespace lunar::lua
