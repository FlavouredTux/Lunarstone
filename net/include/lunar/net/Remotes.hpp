#pragma once

#include <lunar/RemoteValue.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Streaming.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

// A client's remotes on a live server (--net-join --scripts, --net-replay
// --run). Calls from the client's scripts (ScriptContext's RemoteSink:
// FireServer as OnRemoteServerEvent, InvokeServer as RemoteOnInvokeServer,
// OnClientInvoke's answers as RemoteOnInvokeSuccess/Error, TextChannel:
// SendAsync as TextChatService.ClientToServerMessageReplicateSignalV4) become
// Event Invocation items; the server's remote events (OnClientEvent,
// RemoteOnInvokeClient, the answers to InvokeServer and TextChatService's
// chat messages) go to the receiver (ScriptContext::receiveRemote) instead of
// firing on the replica's instances.
class RemoteBridge {
public:
    using Receiver =
        std::function<void(Instance& remote, std::string_view event, std::span<const RemoteValue> arguments)>;

    // Takes the replica's remote events from now on.
    explicit RemoteBridge(std::shared_ptr<Replica> replica);
    ~RemoteBridge();
    RemoteBridge(const RemoteBridge&) = delete;
    RemoteBridge& operator=(const RemoteBridge&) = delete;

    // Where the server's remote events go. Until there is a receiver they
    // wait in order, the way Roblox keeps them for listeners that come late.
    void setReceiver(Receiver receiver);
    // Remote events a replay already decoded (Replica::events), handed over
    // as if they had just arrived.
    void replay(std::span<const EventInvocationItem> events);
    // A call from the client's scripts.
    void send(Instance& remote, std::string_view event, std::vector<RemoteValue> arguments);

    struct Outgoing {
        std::vector<std::uint8_t> message; // ID_DATA
        bool reliable = true;              // false for an UnreliableRemoteEvent's
    };
    // What was sent since the last call, as ID_DATA messages.
    std::vector<Outgoing> take(const ItemContext& context = {false, kBaseCapabilities});

    // Whether a replicated event is one the client's scripts receive (a
    // remote's, or TextChatService's chat).
    static bool isRemoteEvent(const Instance& target, std::string_view event);
    // Whether it's one the client itself acts on: the teleport's
    // (Player.OnTeleportInternal and OnTeleport, TeleportService's
    // TeleportInProgress and TeleportInitFailed*).
    static bool isClientEvent(const Instance& target, std::string_view event);
    // Where those go (logged like the remotes' "from the server"); unset, they're only logged.
    std::function<void(Instance& target, std::string_view event, std::span<const RemoteValue> arguments)> clientEvent;

    // A readable line per call each way: the first three of each remote's
    // event, then every hundredth.
    std::function<void(const std::string&)> log;
    std::size_t logLimit = 160; // characters of arguments per line
    std::size_t sent = 0, received = 0, dropped = 0;
    // How many calls went each way, by remote and event ("Workspace.Remote.OnClientEvent from the server": 3).
    const std::map<std::string, std::size_t, std::less<>>& counts() const { return counts_; }

private:
    struct Call {
        std::weak_ptr<Instance> remote;
        std::string event;
        std::vector<RemoteValue> arguments;
    };
    void deliver(Call call);
    void note(const Instance& remote, const Call& call, std::string_view what);

    std::shared_ptr<Replica> replica_;
    Receiver receiver_;
    std::vector<Call> incoming_; // waiting for a receiver
    std::vector<Call> outgoing_;
    std::map<std::string, std::size_t, std::less<>> counts_;
};

// A remote call's arguments as one short line: (60, {fps = 60, 2, "a"}).
std::string describeRemoteArguments(std::span<const RemoteValue> arguments, std::size_t limit = 160);

} // namespace lunar::net
