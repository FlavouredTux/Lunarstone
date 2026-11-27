#include <lunar/net/Remotes.hpp>

#include <lunar/Instance.hpp>

#include <array>
#include <cstdlib>
#include <format>
#include <utility>

namespace lunar::net {

namespace {

void describeValue(std::string& out, const RemoteValue& value, int depth)
{
    if (value.random) {
        out += std::format("Random({:#x})", *value.random);
        return;
    }
    if (!value.table) {
        if (const auto* s = std::get_if<std::string>(&value.value))
            out += std::format("\"{}\"", *s);
        else if (const auto* inst = std::get_if<InstancePtr>(&value.value); inst && *inst)
            out += (*inst)->fullName();
        else
            out += toString(value.value);
        return;
    }
    if (depth > 3) {
        out += "{...}";
        return;
    }
    out += '{';
    bool first = true;
    for (const auto& [key, member] : value.table->members) {
        out += first ? "" : ", ";
        first = false;
        if (value.table->dictionary)
            out += key + " = ";
        describeValue(out, member, depth + 1);
    }
    out += '}';
}

} // namespace

std::string describeRemoteArguments(std::span<const RemoteValue> arguments, std::size_t limit)
{
    std::string out = "(";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i)
            out += ", ";
        describeValue(out, arguments[i], 0);
    }
    out += ')';
    if (out.size() > limit)
        out = out.substr(0, limit) + "...";
    return out;
}

bool RemoteBridge::isRemoteEvent(const Instance& target, std::string_view event)
{
    if (event == "OnClientEvent")
        return target.isA("BaseRemoteEvent");
    // TextChatService's chat (the client's listeners, 0x103cf436e and on):
    // the server's messages, and the hidden ones of an account that may not read chat.
    if (target.isA("TextChatService"))
        return event == "ServerToClientMessageReplicateSignal" || event == "ServerToClientMessageReplicateSignalV2"
               || event == "UserMessageIntentSentRemote" || event == "ServerToClientPresetChatConfigChangedSignal"
               || event == "ServerToClientPresetChatUserAccessChangedSignal";
    // RemoteFunction's network events (0x1057a01d4): the server invoking us and answering us.
    return target.isA("RemoteFunction")
           && (event == "RemoteOnInvokeClient" || event == "RemoteOnInvokeSuccess" || event == "RemoteOnInvokeError");
}

bool RemoteBridge::isClientEvent(const Instance& target, std::string_view event)
{
    if (target.isA("Player"))
        return event == "OnTeleportInternal" || event == "OnTeleport";
    if (target.isA("TeleportService"))
        return event == "TeleportInProgress" || event == "TeleportInitFailed" || event == "TeleportInitFailedInternal"
               || event == "ReconnectTeleportInitFailed";
    return false;
}

RemoteBridge::RemoteBridge(std::shared_ptr<Replica> replica) : replica_(std::move(replica))
{
    replica_->takeEvent = [this](const InstancePtr& target, std::string_view event, const EventInvocationItem& item) {
        if (isClientEvent(*target, event)) {
            const Call call{target, std::string(event), remoteArguments(item)};
            ++received;
            note(*target, call, "from the server");
            if (clientEvent)
                clientEvent(*target, event, call.arguments);
            return true;
        }
        if (!isRemoteEvent(*target, event))
            return false;
        deliver(Call{target, std::string(event), remoteArguments(item)});
        return true;
    };
}

RemoteBridge::~RemoteBridge()
{
    replica_->takeEvent = nullptr;
}

void RemoteBridge::setReceiver(Receiver receiver)
{
    receiver_ = std::move(receiver);
    auto waiting = std::move(incoming_);
    incoming_.clear();
    for (Call& call : waiting)
        deliver(std::move(call));
}

void RemoteBridge::replay(std::span<const EventInvocationItem> events)
{
    const Schema* schema = replica_->schema();
    if (!schema)
        return;
    for (const EventInvocationItem& e : events) {
        InstancePtr target = replica_->find(e.id);
        if (!target || e.eventId >= schema->events.size())
            continue;
        if (isClientEvent(*target, schema->events[e.eventId].name)) {
            const Call call{target, schema->events[e.eventId].name, remoteArguments(e)};
            note(*target, call, "from the server");
            if (clientEvent)
                clientEvent(*target, call.event, call.arguments);
            continue;
        }
        if (!isRemoteEvent(*target, schema->events[e.eventId].name))
            continue;
        deliver(Call{target, schema->events[e.eventId].name, remoteArguments(e)});
    }
}

void RemoteBridge::deliver(Call call)
{
    if (!receiver_) {
        incoming_.push_back(std::move(call));
        return;
    }
    InstancePtr remote = call.remote.lock();
    if (!remote)
        return;
    ++received;
    note(*remote, call, "from the server");
    receiver_(*remote, call.event, call.arguments);
}

void RemoteBridge::note(const Instance& remote, const Call& call, std::string_view what)
{
    const std::string name = std::format("{}.{}", remote.fullName(), call.event);
    const std::size_t n = ++counts_[std::format("{} {}", name, what)];
    // research: LUNAR_NET_REMOTE_LOG=<text> logs every call of the remotes whose name has it.
    static const char* every = std::getenv("LUNAR_NET_REMOTE_LOG");
    const bool all = every && *every && name.find(every) != std::string::npos;
    if (log && (all || n <= 3 || n % 100 == 0))
        log(std::format("remote: {}{} {}{}", name, describeRemoteArguments(call.arguments, logLimit), what,
                        n > 3 ? std::format(" (#{})", n) : ""));
}

void RemoteBridge::send(Instance& remote, std::string_view event, std::vector<RemoteValue> arguments)
{
    outgoing_.push_back(Call{remote.weak_from_this(), std::string(event), std::move(arguments)});
}

std::vector<RemoteBridge::Outgoing> RemoteBridge::take(const ItemContext& context)
{
    std::vector<Outgoing> out;
    const Schema* schema = replica_->schema();
    if (outgoing_.empty() || !schema)
        return out;
    // One ID_DATA for the reliable calls and one for the unreliable ones.
    // GUESS: an UnreliableRemoteEvent's invocations travel in an unreliable
    // ID_DATA, the way the server sends Animator.OnCombinedUpdate [capture];
    // the client's code that picks the reliability wasn't found.
    std::array<std::vector<Item>, 2> items;
    for (Call& call : outgoing_) {
        InstancePtr remote = call.remote.lock();
        if (!remote)
            continue;
        auto item = replica_->eventItem(*remote, call.event, call.arguments);
        note(*remote, call, item ? "to the server" : "dropped (not replicated)");
        if (!item) {
            ++dropped;
            continue;
        }
        ++sent;
        items[remote->isA("UnreliableRemoteEvent") ? 1 : 0].emplace_back(std::move(*item));
    }
    outgoing_.clear();
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!items[i].empty())
            out.push_back({encodeData(items[i], *schema, replica_->context(), replica_->framing, context), i == 0});
    return out;
}

} // namespace lunar::net
