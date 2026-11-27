#pragma once

// What InstanceApi.cpp and the service implementations share: the method
// table entries and helpers for writing instance methods.

#include "Runtime.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lunar::lua {

// A method instances of `requiredClass` (and subclasses) have. Its closure's
// upvalues are the name and the class, for the "Expected ':' not '.'" check;
// called through __namecall it has none, and the name is the namecall atom.
struct Method {
    std::string_view name;
    std::string_view requiredClass;
    lua_CFunction function;
};

// The services' methods (Services.cpp), merged into the Instance method table.
std::span<const Method> serviceMethods();
// Tool, Humanoid:EquipTool and friends and the core GUI switches (Tools.cpp).
std::span<const Method> toolMethods();
// TextChannel:SendAsync, DisplaySystemMessage and TextChatService:DisplayBubble (TextChat.cpp).
std::span<const Method> textChatMethods();
std::span<const Method> inputActionMethods();
std::span<const Method> emoteMethods();
std::span<const Method> socialMethods();
// ProximityPrompt:InputHoldBegin and InputHoldEnd (Prompts.cpp).
std::span<const Method> promptMethods();
// HttpService:RequestInternal, HttpRequest and HttpRbxApiService (AppHttp.cpp).
std::span<const Method> appHttpMethods();
void openAppHttp(lua_State* L);
void updateImageLoading(Shared& shared);
// StyleSheet, StyleRule and Instance:GetStyled (StylesApi.cpp).
std::span<const Method> styleMethods();
// LocalizationTable, Translator and LocalizationService's translators (Localization.cpp).
std::span<const Method> localizationMethods();
// What the client's own Lua app asks of the engine: fast flags and the like (AppApi.cpp).
std::span<const Method> appMethods();
// AppStorageService's store for ScriptContext::setAppStorageItem (AppApi.cpp).
void setAppStorageItem(std::string_view key, std::string_view value);
void setMemStorageItem(Runtime& runtime, std::string_view key, std::string_view value);
// The client settings for ScriptContext::loadClientSettings (AppApi.cpp).
std::optional<std::size_t> loadClientSettings(std::string_view json);
// game:HttpGet and the rest the executors add to DataModel (ConsoleHttp.cpp):
// members to the console's threads alone, none at all to the game's.
std::span<const Method> consoleMethods();

// `self` of a method: argument 1 if it's an instance of the method's class,
// otherwise the client's "Expected ':' not '.'" error.
Instance& self(lua_State* L);
// That error, for the method running now.
[[noreturn]] void expectedColon(lua_State* L);
// The name of the method running now: the closure's, or the namecall's.
const char* methodName(lua_State* L);
// IsA as the client answers it, for classes the engine only has a placeholder
// for too (a ClientReplicator is a NetworkReplicator).
bool isA(const Instance& instance, std::string_view className);
int pushInstances(lua_State* L, std::span<const InstancePtr> instances);
int returnInstance(lua_State* L, const Instance* instance);
// GetService/FindService, including services only the client knows (placeholders).
Instance* findOrCreateService(lua_State* L, ServiceProvider& provider, std::string_view name, bool create);

// Tag changes that CollectionService's signals announce.
void addTagAndNotify(lua_State* L, Instance& instance, std::string_view tag);
void removeTagAndNotify(lua_State* L, Instance& instance, std::string_view tag);

// Assigning a callback member (OnServerInvoke, OnInvoke...): the function at idx, or nil.
void setCallback(lua_State* L, Instance& instance, std::string_view name, int idx);

// The console's hidden member access (ConsoleProperties.cpp). readHiddenMember
// pushes the member called `name` on `instance` without the capability check
// the game's scripts get and says through `hidden` whether it is one of the
// client's, which scripts could not have reached anyway. writeHiddenMember
// assigns it the same way, with what it holds at `idx`.
void readHiddenMember(lua_State* L, Instance& instance, std::string_view name, bool& hidden);
void writeHiddenMember(lua_State* L, Instance& instance, std::string_view name, int idx);
// Whether a member is one Roblox keeps from scripts: a property they can't
// index (NotScriptable, api::MemberData::scriptable) or one that needs a
// capability to read.
bool hiddenMember(const Instance& instance, std::string_view name);
// What a member the engine has no value of its own for reads as, where the
// classes Roblox keeps from scripts are ones Lunarstone only has placeholders
// for. Values are Roblox's own defaults; empty when there is no value to give.
std::optional<Value> hiddenDefault(std::string_view className, std::string_view name);

// Gives a property only the API data knows (one the engine doesn't model) the
// value scripts read, as if a script had set it.
void setUnmodelledProperty(lua_State* L, const Instance& instance, std::string_view name, Value value);

} // namespace lunar::lua
