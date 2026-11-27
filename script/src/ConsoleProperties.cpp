// The console's reach into the instances themselves (Console.cpp opens them):
// the members Roblox keeps from scripts, the scriptability of a member this
// console has changed, the callbacks an instance holds, the folder the client's
// own GUI draws, and the method a hooked __namecall is in the middle of.

#include <lunar/ClassRegistry.hpp>

#include "Console.hpp"
#include "InstanceApi.hpp"
#include "Runtime.hpp"

#include <format>
#include <string>
#include <string_view>

namespace lunar::lua {

namespace {

// gethiddenproperty(instance, name): the member, and whether it is one of the
// hidden ones -- a member the game's scripts could not have reached however they
// asked for it.
int getHiddenProperty(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    const std::string_view name = checkString(L, 2);
    bool hidden = false;
    readHiddenMember(L, instance, name, hidden);
    // The hidden flag on top of the value, as the executors answer.
    lua_pushboolean(L, hidden);
    return 2;
}

// sethiddenproperty(instance, name, value): assigns it the way a script that
// could reach it would, and answers whether it was one of the hidden ones.
int setHiddenProperty(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    const std::string_view name = checkString(L, 2);
    const bool hidden = hiddenMember(instance, name);
    writeHiddenMember(L, instance, name, 3);
    lua_pushboolean(L, hidden);
    return 1;
}

// Whether a script of the game's own could read and write this member: what
// the API data says it needs (nothing, as game scripts run with no
// capabilities), and not one of the members Roblox keeps from scripts.
bool scriptableByReflection(lua_State* L, const Instance& instance, std::string_view name)
{
    const api::MemberData* member = api::findMember(instance.className(), name);
    if (!member || member->kind != api::MemberKind::Property)
        raise(L, std::format("{} is not a valid member of {} \"{}\"", name, instance.className(), instance.name()));
    return member->scriptable && member->readCapability == 0 && member->writeCapability == 0;
}

// isscriptable(instance, name): whether a script could reach the member, unless
// this console has made it scriptable.
int isScriptable(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    const std::string_view name = checkString(L, 2);
    if (Runtime::of(L).scriptableOverride(instance, name)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, scriptableByReflection(L, instance, name));
    return 1;
}

// setscriptable(instance, name, true|false): whether this console may read and
// write the member, and answers what it was before. The change is the console's
// own: another instance of the same class, and the game's scripts, are as they
// were.
int setScriptable(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    const std::string_view name = checkString(L, 2);
    luaL_checkany(L, 3);
    const bool wanted = lua_toboolean(L, 3) != 0;
    Runtime& rt = Runtime::of(L);
    // What it is now, before this run changes it: this console's own change if
    // there is one, and Roblox's reflection otherwise.
    const bool was = rt.scriptableOverride(instance, name) ? true : scriptableByReflection(L, instance, name);
    rt.setScriptableOverride(instance, name, wanted);
    lua_pushboolean(L, was);
    return 1;
}

// getcallbackvalue(instance, name): the function a callback member holds --
// OnInvoke, OnServerInvoke, OnClientInvoke. Reading one is what the game does
// not let a script do (InstanceApi.cpp's index refuses it); this is the console's
// own way in, over the same reference the VM keeps it in.
int getCallbackValue(lua_State* L)
{
    Instance& instance = checkInstance(L, 1);
    const std::string_view name = checkString(L, 2);
    const std::optional<Shared::Callback> callback = Runtime::of(L).shared().callback(instance, name);
    if (!callback || callback->ref == LUA_NOREF) {
        lua_pushnil(L);
        return 1;
    }
    lua_getref(L, callback->ref);
    return 1;
}

// gethui(): where the console's GUIs go. CoreGui is the service the client's
// own GUI lives in and the one the GUI engine draws over everything else, so
// this is a Folder parented into it -- which is where the executors' gethui()
// lands too. A Folder and not a ScreenGui: scripts parent ScreenGuis of their
// own to it (Infinite Yield does, as UNC's example does), and a ScreenGui in
// a ScreenGui doesn't show. Made on first use and kept, so the same one comes
// back.
int getHui(lua_State* L)
{
    return returnInstance(L, &consoleGui(L));
}

// getnamecallmethod(): the method of the last method call this thread made,
// as the VM's NAMECALL left it, which is what a __namecall hook asks for: the
// hook runs before the real __namecall does anything, so the name is still
// the one the call was made with. Executors read the same field, with the
// same catch: a hook that makes a method call of its own before asking gets
// that one's name. Nil on a thread that has made none.
int getNamecallMethod(lua_State* L)
{
    if (const char* name = lua_namecallatom(L, nullptr))
        lua_pushstring(L, name);
    else
        lua_pushnil(L);
    return 1;
}

} // namespace

Instance& consoleGui(lua_State* L)
{
    DataModel& game = Runtime::of(L).game();
    Instance* coreGui = findOrCreateService(L, game, "CoreGui", true);
    if (Instance* existing = coreGui->findFirstChild("ConsoleGui"))
        return *existing;
    auto folder = ClassRegistry::create("Folder");
    if (!folder)
        raise(L, "This client has no Folder to make a gethui() of");
    folder->setName("ConsoleGui");
    guard(L, [&] { folder->setParent(coreGui); });
    return *folder;
}

void openConsoleProperties(lua_State* thread)
{
    const auto set = [thread](const char* name, lua_CFunction function) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    };
    set("gethiddenproperty", getHiddenProperty);
    set("sethiddenproperty", setHiddenProperty);
    set("isscriptable", isScriptable);
    set("setscriptable", setScriptable);
    set("getcallbackvalue", getCallbackValue);
    set("gethui", getHui);
    set("getnamecallmethod", getNamecallMethod);
}

} // namespace lunar::lua