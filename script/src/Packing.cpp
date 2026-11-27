// Copying Luau values out of a VM and back (Packed), and the EventHub that
// carries packed arguments to listeners in any VM of the game.

#include "Runtime.hpp"
#include "Services.hpp"

#include <cmath>
#include <format>
#include <unordered_map>
#include <utility>

namespace lunar::lua {

namespace {

// Roblox refuses deeper tables ("Tables are too deeply nested"); nil past it here.
constexpr int maxDepth = 64;

Packed packAt(lua_State* L, int idx, int depth)
{
    if (const RandomState* random = to<RandomState>(L, idx))
        return Packed{.random = random->state};
    if (lua_type(L, idx) != LUA_TTABLE) {
        if (auto value = toValue(L, idx))
            return Packed{std::move(*value), nullptr};
        return {};
    }
    if (depth >= maxDepth)
        return {};
    auto table = std::make_shared<PackedTable>();
    idx = lua_absindex(L, idx);
    lua_checkstack(L, 4);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        Packed key = packAt(L, -2, depth + 1);
        Packed value = packAt(L, -1, depth + 1);
        lua_pop(L, 1);
        // Keys that don't survive (functions, nil) drop the entry.
        if (!key.table && std::holds_alternative<std::monostate>(key.value))
            continue;
        table->emplace_back(std::move(key), std::move(value));
    }
    return Packed{Value(), std::move(table)};
}

struct SendCount {
    std::uint64_t strings = 0, variants = 0;
    bool over() const { return strings > kClientSendStringBytes || variants > kClientSendVariants; }
};

// Counts as packAt walks, stopping once over; the stack is the caller's to
// put back.
bool countAt(lua_State* L, int idx, int depth, SendCount& count)
{
    ++count.variants;
    const int type = lua_type(L, idx);
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        lua_tolstring(L, idx, &length);
        count.strings += length;
        return count.over();
    }
    if (type != LUA_TTABLE || depth >= maxDepth)
        return count.over();
    idx = lua_absindex(L, idx);
    lua_checkstack(L, 4);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING) { // a dictionary's key, sent as a string
            std::size_t length = 0;
            lua_tolstring(L, -2, &length);
            count.strings += length;
        }
        if (countAt(L, -1, depth + 1, count))
            return true;
        lua_pop(L, 1);
    }
    return count.over();
}

} // namespace

Packed pack(lua_State* L, int idx)
{
    return packAt(L, idx, 0);
}

std::optional<std::string> overSendLimits(lua_State* L, int first, int last)
{
    const int top = lua_gettop(L);
    SendCount count;
    bool over = false;
    for (int i = first; i <= last && !over; ++i)
        over = countAt(L, i, 0, count);
    lua_settop(L, top);
    if (count.strings > kClientSendStringBytes)
        return std::format("Replicator send variant: Total string data size exceed limit! ({})", count.strings);
    if (count.variants > kClientSendVariants)
        return std::format("Replicator send variant: Variant count exceeded limit! ({})", count.variants);
    return std::nullopt;
}

PackedArgs packArgs(lua_State* L, int first, int last)
{
    PackedArgs args;
    for (int i = first; i <= last; ++i)
        args.push_back(pack(L, i));
    return args;
}

void unpack(lua_State* L, const Packed& value)
{
    lua_checkstack(L, 3);
    if (value.random) {
        push<RandomState>(L, RandomState{.state = *value.random});
        return;
    }
    if (!value.table) {
        pushValue(L, value.value);
        return;
    }
    lua_createtable(L, 0, static_cast<int>(value.table->size()));
    for (const auto& [k, v] : *value.table) {
        unpack(L, k);
        unpack(L, v);
        lua_rawset(L, -3);
    }
}

int unpackArgs(lua_State* L, std::span<const Packed> args)
{
    lua_checkstack(L, static_cast<int>(args.size()) + 2);
    for (const Packed& arg : args)
        unpack(L, arg);
    return static_cast<int>(args.size());
}

RemoteValue toRemote(const Packed& value)
{
    if (value.random)
        return RemoteValue{.random = value.random};
    if (!value.table)
        return remoteValue(value.value);
    // The array part: the keys 1..n, all present.
    std::unordered_map<std::size_t, const Packed*> numbered;
    for (const auto& [k, v] : *value.table)
        if (const double* d = std::get_if<double>(&k.value); d && !k.table && *d >= 1 && *d <= 1e9 && *d == std::floor(*d))
            numbered.emplace(static_cast<std::size_t>(*d), &v);
    std::size_t n = 0;
    while (numbered.contains(n + 1))
        ++n;
    auto table = std::make_shared<RemoteTable>();
    if (n > 0) {
        for (std::size_t i = 1; i <= n; ++i)
            table->members.emplace_back(std::string(), toRemote(*numbered[i]));
    } else {
        table->dictionary = true;
        for (const auto& [k, v] : *value.table) {
            if (k.table)
                continue; // a table key has no string form worth sending
            const std::string* s = std::get_if<std::string>(&k.value);
            table->members.emplace_back(s ? *s : toString(k.value), toRemote(v));
        }
    }
    return RemoteValue{Value{}, std::move(table)};
}

Packed fromRemote(const RemoteValue& value)
{
    if (value.random)
        return Packed{.random = value.random};
    if (!value.table)
        return packed(value.value);
    auto table = std::make_shared<PackedTable>();
    table->reserve(value.table->members.size());
    double index = 0;
    for (const auto& [key, member] : value.table->members)
        table->emplace_back(value.table->dictionary ? packed(Value(key)) : packed(Value(++index)), fromRemote(member));
    return Packed{Value(), std::move(table)};
}

// --- EventHub ----------------------------------------------------------------

EventHub::~EventHub()
{
    for (auto& [owner, entry] : entries_) {
        entry.engineEvents.disconnect();
        entry.destroying.disconnect();
        for (Connection& connection : entry.bridged)
            connection.disconnect();
    }
}

EventHub::Entry* EventHub::entry(const Instance& owner, bool create)
{
    auto it = entries_.find(&owner);
    if (it != entries_.end()) {
        // A dead instance's address can come back for a new one.
        if (it->second.owner.lock().get() == &owner)
            return &it->second;
        it->second.engineEvents.disconnect();
        it->second.destroying.disconnect();
        for (Connection& connection : it->second.bridged)
            connection.disconnect();
        entries_.erase(it);
    }
    if (!create)
        return nullptr;
    if (entries_.size() > 256 && entries_.size() % 256 == 0)
        std::erase_if(entries_, [](auto& item) { return item.second.owner.expired(); });

    auto self = std::const_pointer_cast<Instance>(owner.shared_from_this());
    Entry& made = entries_[&owner];
    made.owner = self;
    // Events the engine fires by name come through as packed arguments.
    made.engineEvents
        = self->eventFired.connect([this, key = &owner](std::string_view name, std::span<const Value> args) {
              PackedArgs packedArgs;
              packedArgs.reserve(args.size());
              for (const Value& arg : args)
                  packedArgs.push_back(packed(arg));
              auto found = entries_.find(key);
              if (found == entries_.end())
                  return;
              auto event = found->second.events.find(name);
              if (event != found->second.events.end())
                  event->second->fire(packedArgs);
          });
    made.bridged = bridgeEngineSignals(*this, *self);
    // Destroy disconnects everything, like the engine's own signals.
    made.destroying = self->destroying.connect([this, key = &owner] {
        auto found = entries_.find(key);
        if (found == entries_.end())
            return;
        for (auto& [name, event] : found->second.events)
            event->disconnectAll();
    });
    return &made;
}

EventHub::Event& EventHub::event(const Instance& owner, std::string_view name)
{
    Entry* e = entry(owner, true);
    auto it = e->events.find(name);
    if (it == e->events.end())
        it = e->events.emplace(std::string(name), std::make_unique<Event>()).first;
    return *it->second;
}

EventHub::Event* EventHub::find(const Instance& owner, std::string_view name)
{
    Entry* e = entry(owner, false);
    if (!e)
        return nullptr;
    auto it = e->events.find(name);
    return it == e->events.end() ? nullptr : it->second.get();
}

void EventHub::fire(const Instance& owner, std::string_view name, const PackedArgs& args)
{
    if (Event* e = find(owner, name))
        e->fire(args);
}

bool EventHub::hasListeners(const Instance& owner, std::string_view name)
{
    Event* e = find(owner, name);
    return e && e->connectionCount() > 0;
}

std::shared_ptr<EventHub::NativeConnection> EventHub::connectNative(const Instance& owner, std::string_view name,
                                                                    std::function<void(const PackedArgs&)> handler)
{
    auto native = std::make_shared<NativeConnection>();
    native->handler = std::move(handler);
    native->connection = event(owner, name).connect([weak = std::weak_ptr(native)](const PackedArgs& args) {
        if (const auto held = weak.lock(); held && held->enabled)
            held->handler(args);
    });
    noteNative(owner, name, native);
    return native;
}

void EventHub::noteNative(const Instance& owner, std::string_view name, const std::shared_ptr<NativeConnection>& native)
{
    Entry* e = entry(owner, true);
    std::erase_if(e->natives, [](const auto& item) { return item.second.expired(); });
    e->natives.emplace_back(std::string(name), native);
}

std::vector<std::shared_ptr<EventHub::NativeConnection>> EventHub::nativeConnections(const Instance& owner,
                                                                                    std::string_view name)
{
    std::vector<std::shared_ptr<NativeConnection>> found;
    if (Entry* e = entry(owner, false))
        for (const auto& [event, weak] : e->natives)
            if (const auto held = weak.lock(); held && event == name && held->connection.connected())
                found.push_back(held);
    return found;
}

// --- Shared ------------------------------------------------------------------

Shared::Shared() : state(std::make_unique<SharedState>()) {}

Shared::~Shared()
{
    tagWatch.disconnect();
    tagUnwatch.disconnect();
}

void Shared::setCallback(const Instance& owner, std::string_view name, Runtime* runtime, int ref,
                         ThreadAccess access, int globals)
{
    std::erase_if(callbacks_, [](const CallbackKey& key) { return key.owner.expired(); });
    for (CallbackKey& key : callbacks_) {
        if (key.owner.lock().get() == &owner && key.name == name) {
            if (key.callback.runtime && key.callback.runtime == runtime) {
                if (key.callback.ref != LUA_NOREF)
                    lua_unref(runtime->main(), key.callback.ref);
                if (key.callback.globals != LUA_NOREF)
                    lua_unref(runtime->main(), key.callback.globals);
            }
            key.callback = Callback{runtime, ref, access, globals};
            return;
        }
    }
    callbacks_.push_back(CallbackKey{std::const_pointer_cast<Instance>(owner.shared_from_this()), std::string(name),
                                     {runtime, ref, access, globals}});
}

std::optional<Shared::Callback> Shared::callback(const Instance& owner, std::string_view name) const
{
    for (const CallbackKey& key : callbacks_)
        if (key.owner.lock().get() == &owner && key.name == name && key.callback.ref != LUA_NOREF)
            return key.callback;
    return std::nullopt;
}

void Shared::dropCallbacks(Runtime* runtime)
{
    std::erase_if(callbacks_, [runtime](const CallbackKey& key) { return key.callback.runtime == runtime; });
}

Runtime* Shared::runtimeFor(ScriptRole role) const
{
    for (Runtime* runtime : runtimes)
        if (runtime->role() == role || runtime->role() == ScriptRole::Standalone)
            return runtime;
    return nullptr;
}

void Shared::deliverMessages()
{
    // Messages whose receiver isn't listening yet wait for a later frame.
    std::deque<Message> waiting;
    while (!inbox.empty()) {
        Message message = std::move(inbox.front());
        inbox.pop_front();
        auto remote = message.remote.lock();
        if (!remote || remote->destroyed())
            continue;
        if (!events.hasListeners(*remote, message.event)) {
            waiting.push_back(std::move(message));
            continue;
        }
        events.fire(*remote, message.event, message.args);
    }
    inbox = std::move(waiting);

    auto calls = std::move(invocations);
    invocations.clear();
    for (auto& call : calls)
        call();
}

} // namespace lunar::lua
