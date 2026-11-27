// Where scripts run, and require() with ModuleScript caching.
//
// require's messages are the 0.742 client's: "Attempted to call require with
// invalid argument(s).", "Requested module was required recursively", "Module
// code did not return exactly one value", "Requested module experienced an
// error while loading".

#include "Runtime.hpp"

#include <array>
#include <format>
#include <string_view>

namespace lunar {

namespace {

bool readsBool(const Instance& instance, std::string_view property, bool expected)
{
    const Property* p = instance.classInfo().findProperty(property);
    if (!p)
        return false;
    Value value = p->get(instance);
    const bool* b = std::get_if<bool>(&value);
    return b && *b == expected;
}

// Older places say Disabled, newer ones Enabled. Placeholder Scripts have
// neither, so they count as enabled.
bool disabled(const Instance& script)
{
    return readsBool(script, "Disabled", true) || readsBool(script, "Enabled", false);
}

Enum::RunContext runContext(const Instance& script)
{
    if (const auto* base = dynamic_cast<const BaseScript*>(&script))
        return base->runContext();
    return Enum::RunContext::Legacy;
}

// The service (child of the DataModel) the instance is under, or null.
const Instance* serviceOf(const Instance& instance)
{
    const Instance* node = &instance;
    while (node->parent() && node->parent()->parent())
        node = node->parent();
    return node->parent() && node->parent()->isA("DataModel") ? node : nullptr;
}

bool underClass(const Instance& instance, std::string_view className)
{
    return instance.findFirstAncestorOfClass(className) != nullptr;
}

const Players* playersOf(const Instance& instance)
{
    const Instance* service = serviceOf(instance);
    const Instance* game = service ? service->parent() : nullptr;
    return game ? dynamic_cast<const Players*>(game->findFirstChildOfClass("Players")) : nullptr;
}

// The player whose character model holds the instance, or null.
const Player* characterOwner(const Instance& instance)
{
    const Players* players = playersOf(instance);
    if (!players)
        return nullptr;
    for (const Instance* node = instance.parent(); node; node = node->parent()) {
        if (!node->isA("Model"))
            continue;
        for (const InstancePtr& player : players->children())
            if (auto* p = dynamic_cast<const Player*>(player.get()); p && p->character() == node)
                return p;
    }
    return nullptr;
}

// ReplicatedFirst, or the local player's Backpack, PlayerGui, PlayerScripts
// or character: a client runs no other player's LocalScripts. Without a
// LocalPlayer any player's count.
bool inClientContainer(const Instance& script)
{
    if (underClass(script, "ReplicatedFirst"))
        return true;
    const Players* players = playersOf(script);
    const Player* local = players ? players->localPlayer() : nullptr;
    auto mine = [local](const Instance* player) { return player && (!local || player == local); };
    if (underClass(script, "Backpack") || underClass(script, "PlayerGui") || underClass(script, "PlayerScripts"))
        return mine(script.findFirstAncestorOfClass("Player"));
    return mine(characterOwner(script));
}

bool inStarterContainer(const Instance& script)
{
    const Instance* service = serviceOf(script);
    if (!service)
        return true; // not in the game at all
    std::string_view name = service->className();
    return name == "StarterGui" || name == "StarterPack" || name == "StarterPlayer";
}

} // namespace

bool runsIn(const Instance& script, ScriptRole role)
{
    if (!script.isA("BaseScript") && script.className() != "Script" && script.className() != "LocalScript")
        return false;
    if (disabled(script) || !serviceOf(script))
        return false;
    const Instance* service = serviceOf(script);
    const std::string_view serviceName = service->className();
    const bool server = role != ScriptRole::Client;
    const bool client = role != ScriptRole::Server;

    if (script.className() == "LocalScript")
        return client && role != ScriptRole::Standalone && inClientContainer(script);

    switch (runContext(script)) {
    case Enum::RunContext::Legacy:
        return server
               && (serviceName == "Workspace" || serviceName == "ServerScriptService"
                   || underClass(script, "Backpack"));
    case Enum::RunContext::Server:
        return server && serviceName != "ServerStorage" && !inStarterContainer(script);
    case Enum::RunContext::Client:
        return client && role != ScriptRole::Standalone && serviceName != "ServerStorage"
               && serviceName != "ServerScriptService" && !inStarterContainer(script);
    case Enum::RunContext::Plugin:
        return false;
    }
    return false;
}

std::vector<InstancePtr> serverScripts(const DataModel& game)
{
    std::vector<InstancePtr> scripts;
    for (const InstancePtr& service : game.children()) {
        if (service->className() != "Workspace" && service->className() != "ServerScriptService")
            continue;
        for (const InstancePtr& instance : service->descendants())
            // LocalScript derives from Script in Roblox, so compare the exact class.
            if (instance->className() == "Script" && !disabled(*instance))
                scripts.push_back(instance);
    }
    return scripts;
}

namespace lua {

namespace {

constexpr std::string_view loadError = "Requested module experienced an error while loading";

// Where require continues after waiting for a module another thread loads:
// the waker pushed (true, result) or (false, message).
int requireContinue(lua_State* L, int)
{
    if (!lua_toboolean(L, 1))
        raise(L, lua_tostring(L, 2));
    lua_settop(L, 2);
    lua_remove(L, 1);
    return 1;
}

} // namespace

int Runtime::require(lua_State* L, Instance& module)
{
    auto it = modules_.find(&module);
    if (it != modules_.end() && it->second.module.lock().get() != &module) {
        modules_.erase(it);
        it = modules_.end();
    }

    if (it == modules_.end()) {
        if (Shared::NativeModule push = shared_->nativeModule(module)) {
            // Made by Lunarstone (PlayerModule.cpp): loaded at once, cached like any other.
            Module record;
            record.module = module.shared_from_this();
            record.state = Module::State::Loaded;
            push(L, module);
            record.result = lua_ref(L, -1);
            lua_pop(L, 1);
            modules_.insert_or_assign(&module, std::move(record));
            lua_getref(L, modules_.at(&module).result);
            return 1;
        }
        const std::string chunkName = module.fullName();
        Value source = module.get("Source");
        const auto* text = std::get_if<std::string>(&source);
        std::optional<Profiler::Scope> compiling;
        if (shared_->profiler)
            compiling.emplace(shared_->profiler.get(), &module, role_, "compile");
        auto bytecode = codeOf(text ? *text : std::string(), chunkName);
        if (compiling)
            compiling->finish();
        Module record;
        record.module = module.shared_from_this();
        record.requiredBy = currentScript(L);
        if (!bytecode) {
            output(MessageType::Error, bytecode.error());
            record.state = Module::State::Failed;
            record.error = std::string(loadError);
            modules_.insert_or_assign(&module, std::move(record));
            raise(L, loadError);
        }
        auto thread = loadThread(*bytecode, chunkName, &module);
        if (!thread)
            raise(L, thread.error().message);
        record.threadRef = lua_ref(main_, -1);
        lua_pop(main_, 1);
        record.thread = *thread;
        modules_.insert_or_assign(&module, std::move(record));
        onFinish(*thread, [this, key = &module](lua_State* t, int status, int results) {
            finishModule(key, t, status, results);
        });
        if (auto error = resume(*thread, 0, "require"))
            report(*error);
        it = modules_.find(&module);
    } else if (it->second.state == Module::State::Loading) {
        // Required again while it loads: a cycle if we're inside its own loading.
        for (const Instance* node = currentScript(L); node;) {
            if (node == &module)
                raise(L, "Requested module was required recursively");
            auto parent = modules_.find(node);
            node = parent == modules_.end() ? nullptr : parent->second.requiredBy;
        }
    }

    Module& record = it->second;
    switch (record.state) {
    case Module::State::Loaded:
        lua_getref(L, record.result);
        return 1;
    case Module::State::Failed:
        raise(L, record.error);
    case Module::State::Loading:
        break;
    }
    // The module yielded: wait for it to finish.
    checkYieldable(L);
    record.waiting.emplace_back(L, suspend(L));
    lua_settop(L, 0);
    return lua_yield(L, 0);
}

void Runtime::finishModule(const Instance* key, lua_State* thread, int status, int results)
{
    auto it = modules_.find(key);
    if (it == modules_.end())
        return;
    Module& record = it->second;
    if (status == LUA_OK && results == 1) {
        lua_pushvalue(thread, -1);
        record.result = lua_ref(thread, -1);
        lua_pop(thread, 1);
        record.state = Module::State::Loaded;
    } else {
        record.state = Module::State::Failed;
        record.error = std::string(status == LUA_OK ? "Module code did not return exactly one value" : loadError);
        if (status == LUA_OK)
            output(MessageType::Error, record.error);
    }
    record.thread = nullptr;
    if (record.threadRef != LUA_NOREF)
        lua_unref(main_, record.threadRef);
    record.threadRef = LUA_NOREF;

    auto waiting = std::move(record.waiting);
    const bool loaded = record.state == Module::State::Loaded;
    const int result = record.result;
    const std::string error = record.error;
    for (auto [waiter, token] : waiting) {
        defer([this, waiter, token, loaded, result, error] {
            wake(
                waiter, token,
                [&](lua_State* t) {
                    lua_pushboolean(t, loaded);
                    if (loaded)
                        lua_getref(t, result);
                    else
                        lua_pushlstring(t, error.data(), error.size());
                    return 2;
                },
                "require");
        });
    }
}

lua_Continuation requireContinuation()
{
    return requireContinue;
}

} // namespace lua

} // namespace lunar
