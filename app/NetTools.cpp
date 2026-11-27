#include "BenchGraphics.hpp"
#include "BenchReplay.hpp"
#include "LiveAnimation.hpp"
#include "LiveCharacter.hpp"
#include "LiveChat.hpp"
#include "LivePlay.hpp"
#include "NetJoin.hpp"
#include "NetTools.hpp"
#include "ToolActions.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/audio/SoundSystem.hpp>
#include <lunar/net/Capture.hpp>
#include <lunar/net/CaptureDecoder.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Motion.hpp>

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/classes/World.hpp>
#include <lunar/net/Peer.hpp>
#include <lunar/net/Physics.hpp>
#include <lunar/net/Remotes.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Session.hpp>
#include <lunar/net/Socket.hpp>
#include <lunar/net/ToolReplication.hpp>
#include <lunar/platform/Display.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <numbers>
#include <print>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// The services the local server replicates (the parts a client needs).
const std::vector<std::string>& replicatedServices()
{
    static const std::vector<std::string> services{"Workspace",       "Lighting",   "ReplicatedStorage", "Players",
                                                   "ReplicatedFirst", "StarterGui", "StarterPack",       "Teams"};
    return services;
}
constexpr std::string_view kEchoEvent = "LunarEcho";
// The server moves this part and streams it with 0x85 physics; the client
// owns this one and streams its motion back (goal 5, ROBLOX-LAYER.md 6).
constexpr std::string_view kServerBall = "ServerBall";
constexpr std::string_view kClientBall = "ClientBall";
constexpr std::string_view kOwnershipEvent = "AssignOwnership";
constexpr std::string_view kAddFunction = "LunarAdd"; // a RemoteFunction (goal 6)
// The legacy chat path is RemoteEvents in ReplicatedStorage (goal 6).
constexpr std::string_view kChatFolder = "DefaultChatSystemChatEvents";
constexpr std::string_view kSayMessage = "SayMessageRequest";
constexpr std::string_view kChatMessage = "OnMessageDoneFiltering";
constexpr std::uint64_t kPhysicsPeriodMs = 66; // ~15 Hz, S2PhysicsSenderRate
// The legacy chat packet the client sends from its Player (0x87, Session.hpp).
constexpr std::string_view kChatLine = "hello via 0x87";

// Engine-made classes (Player ...) aren't script-creatable: make them through the class's factory.
lunar::InstancePtr makeInstance(std::string_view className)
{
    const lunar::ClassInfo* info = lunar::ClassRegistry::find(className);
    return info && info->factory ? info->factory() : nullptr;
}

using lunar::net::Peer;
using lunar::net::PhysicsPV;
using lunar::net::Reliability;

std::uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Applies a decoded physics PV to a part (goal 5: the receiver's side).
void applyPv(lunar::Instance* part, const PhysicsPV& pv)
{
    if (!part)
        return;
    part->set("CFrame", lunar::Value(pv.cframe));
    part->set("AssemblyLinearVelocity", lunar::Value(pv.linearVelocity));
    part->set("AssemblyAngularVelocity", lunar::Value(pv.angularVelocity));
}

// A part on a circle, with the matching linear velocity (a visible, checkable motion).
PhysicsPV orbit(double t, double radius, double height, double speed)
{
    PhysicsPV pv;
    const double a = t * speed;
    pv.cframe.position = {static_cast<float>(radius * std::cos(a)), static_cast<float>(height),
                          static_cast<float>(radius * std::sin(a))};
    pv.linearVelocity
        = {static_cast<float>(-radius * speed * std::sin(a)), 0.0f, static_cast<float>(radius * speed * std::cos(a))};
    return pv;
}

// --net-server's state: the place, its replication and the connected clients.
struct Server {
    std::shared_ptr<lunar::DataModel> game;
    std::unique_ptr<lunar::net::ReplicationServer> replication;
    std::shared_ptr<lunar::net::Replica> logReplica = std::make_shared<lunar::net::Replica>();
    std::unique_ptr<Peer> peer;
    std::vector<int> clients;
    Clock::time_point lastChange = Clock::now();
    int changes = 0;
    int echoes = 0;
    int invokes = 0; // RemoteFunction OnServerInvoke calls answered (goal 6)
    int chats = 0;   // legacy chat SayMessageRequest messages broadcast (goal 6)
    // The session layer (part 3): pings each way, the character request, the
    // real chat packets, a kick at the end.
    std::map<int, lunar::net::ServerSession> sessions;
    std::map<int, lunar::Instance*> players; // the Player we made for each client
    int charactersSpawned = 0;
    int chatPackets = 0; // 0x87 packets relayed
    int kicks = 0;
    // Physics (goal 5): the server orbits ServerBall and streams it; it applies
    // the physics the owner client sends for ClientBall.
    lunar::Instance* serverBall = nullptr;
    lunar::Instance* clientBall = nullptr;
    std::uint64_t physicsSeq = 0;
    std::uint64_t lastPhysicsMs = 0;
    std::uint64_t startMs = nowMs();
    int physicsSent = 0;
    int physicsFromClient = 0;
    bool freezePhysics = false; // the drain phase: keep sending the same pose so the client converges
    PhysicsPV lastServerBallPv; // what we last sent for ServerBall
    PhysicsPV lastClientBallPv; // what the client last sent for ClientBall

    std::expected<void, std::string> start(const std::filesystem::path& place, std::string_view host,
                                           std::uint16_t port)
    {
        auto bytes = lunar::readFile(place);
        if (!bytes)
            return std::unexpected(bytes.error());
        auto loaded = lunar::loadPlace(*bytes);
        if (!loaded)
            return std::unexpected(loaded.error());
        game = loaded->game;
        // A RemoteEvent for the round trip, unless the place brings one.
        auto* storage = game->getService("ReplicatedStorage");
        if (!storage->findFirstChild(kEchoEvent)) {
            auto echo = lunar::ClassRegistry::create("RemoteEvent");
            echo->setName(std::string(kEchoEvent));
            echo->setParent(storage);
        }
        if (!storage->findFirstChild(kOwnershipEvent)) {
            auto ev = lunar::ClassRegistry::create("RemoteEvent");
            ev->setName(std::string(kOwnershipEvent));
            ev->setParent(storage);
        }
        if (!storage->findFirstChild(kAddFunction)) {
            auto fn = lunar::ClassRegistry::create("RemoteFunction");
            fn->setName(std::string(kAddFunction));
            fn->setParent(storage);
        }
        // The legacy chat events, by their real names.
        if (!storage->findFirstChild(kChatFolder)) {
            auto folder = lunar::ClassRegistry::create("Folder");
            folder->setName(std::string(kChatFolder));
            for (std::string_view name : {kSayMessage, kChatMessage}) {
                auto ev = lunar::ClassRegistry::create("RemoteEvent");
                ev->setName(std::string(name));
                ev->setParent(folder.get());
            }
            folder->setParent(storage);
        }
        // Two parts the physics path (0x85) drives, created before the
        // ReplicationServer so they are part of the join and have ids.
        auto* workspace = game->getService("Workspace");
        auto makeBall = [&](std::string_view name, lunar::Vector3 at) -> lunar::Instance* {
            auto ball = lunar::ClassRegistry::create("Part");
            ball->setName(std::string(name));
            ball->set("CFrame", lunar::Value(lunar::CFrame(at)));
            lunar::Instance* raw = ball.get();
            ball->setParent(workspace);
            return raw;
        };
        serverBall = makeBall(kServerBall, {10, 2, 0});
        clientBall = makeBall(kClientBall, {-10, 2, 0});
        // The services the join carries must exist before the ReplicationServer
        // looks for them (a tiny place brings no Players, and the Player we
        // install later needs its parent replicated).
        for (const std::string& name : replicatedServices())
            if (lunar::ClassRegistry::find(name))
                game->getService(name);
        replication = std::make_unique<lunar::net::ReplicationServer>(game, replicatedServices());
        Peer::Options o;
        o.describer = lunar::net::robloxDescriber(logReplica);
        peer = std::make_unique<Peer>(Peer::Role::Server, std::move(o));
        if (auto ok = peer->listen(host, port); !ok)
            return ok;
        std::println("server: {} replicates {} instances ({} classes in the schema), listening on {}",
                     place.filename().string(), replication->replicatedCount(), replication->schema().classes.size(),
                     peer->localAddress().toString());
        return {};
    }

    // The Request Character item's answer: a character Model under Workspace,
    // set as the Player's Character (GUESS at what the real server sends; it
    // replicates the character's instances and Player.Character like any others).
    void spawnCharacter(int connection)
    {
        auto p = players.find(connection);
        if (p == players.end())
            return;
        auto model = lunar::ClassRegistry::create("Model");
        model->setName(p->second->name());
        auto root = lunar::ClassRegistry::create("Part");
        root->setName("HumanoidRootPart");
        root->set("CFrame", lunar::Value(lunar::CFrame(lunar::Vector3{0, 5, 0})));
        root->setParent(model.get());
        model->setParent(game->getService("Workspace"));
        peer->send(connection, replication->addInstance(model));
        peer->send(connection, replication->addInstance(root));
        p->second->set("Character", lunar::Value(model));
        ++charactersSpawned;
        std::println("server: spawned {} for client {}", model->fullName(), connection);
    }

    // ID_KICK with a metadata JSON (capability 50 isn't in the base set, so just the message).
    void kick(int connection, std::string_view why)
    {
        lunar::net::KickMessage k;
        k.message = std::string(why);
        k.metadataJson = R"({"reason":"loopback"})";
        peer->send(connection, lunar::net::encodeKick(k, lunar::net::kBaseCapabilities));
        ++kicks;
    }

    void update(int waitMs)
    {
        peer->update(waitMs);
        while (auto e = peer->poll()) {
            if (e->kind == Peer::Event::Kind::Connected) {
                std::println("server: client {} connected, waiting for its join request", e->connection);
            } else if (e->kind == Peer::Event::Kind::Message && !e->data.empty()
                       && e->data[0] == lunar::net::kJoinRequestId) {
                auto request = lunar::net::decodeJoinRequest(e->data, lunar::net::kBaseCapabilities);
                std::println("server: join request from client {}: {}", e->connection,
                             request ? request->joinInfo : request.error());
                clients.push_back(e->connection);
                sessions.emplace(e->connection, lunar::net::ServerSession());
                // Schema first, then the place id verdict, then the tree and a marker (ROBLOX-LAYER.md 6).
                auto join = replication->joinMessages();
                std::vector<std::uint8_t> verified{lunar::net::kPlaceIdVerifiedId, 1};
                join.insert(join.begin() + 1, std::move(verified));
                // "Install Player": a Player for this client, replicated before the join's marker.
                if (auto player = makeInstance("Player")) {
                    player->setName(std::format("client{}", e->connection));
                    player->setParent(game->getService("Players"));
                    players[e->connection] = player.get();
                    join.insert(join.end() - 1, replication->addInstance(player));
                }
                std::size_t bytes = 0;
                for (auto& m : join) {
                    bytes += m.size();
                    peer->send(e->connection, std::move(m), Reliability::ReliableOrdered);
                }
                std::println("server: sending the join: {} messages, {} bytes", join.size(), bytes);
                // Hand ClientBall's physics ownership to this client (goal 5):
                // a server -> client AssignOwnership event naming the part.
                if (auto* ev = game->getService("ReplicatedStorage")->findFirstChild(kOwnershipEvent))
                    peer->send(e->connection,
                               replication->invoke(*ev, "OnClientEvent", {lunar::Value(std::string(kClientBall))}));
            } else if (e->kind == Peer::Event::Kind::Disconnected) {
                std::erase(clients, e->connection);
                std::println("server: client {} left ({})", e->connection, e->reason);
            } else if (e->kind == Peer::Event::Kind::Message && !e->data.empty() && e->data[0] == 0x1b) {
                // The owner client's physics for ClientBall (0x85, client -> server).
                auto packet = lunar::net::decodePhysicsPacket(e->data, /*serverToClient=*/false);
                if (packet)
                    for (const auto& rec : packet->records)
                        if (rec.mechanism && !rec.mechanism->parts.empty() && !rec.mechanism->parts[0].frames.empty()
                            && clientBall && replication->idOf(clientBall) == rec.part) {
                            lastClientBallPv = rec.mechanism->parts[0].frames[0].pv;
                            ++physicsFromClient;
                        }
            } else if (e->kind == Peer::Event::Kind::Message && !e->data.empty()
                       && (e->data[0] == lunar::net::msg::ChatAll || e->data[0] == lunar::net::msg::ChatTeam
                           || e->data[0] == lunar::net::msg::ChatGame || e->data[0] == lunar::net::msg::ChatPlayer)) {
                // A legacy chat packet: the sender must be a Player (the client's check too), then
                // it goes to everyone as it came.
                auto chat = lunar::net::decodeChat(e->data);
                auto sender = chat ? replication->find(chat->sender) : nullptr;
                if (chat && (chat->id == lunar::net::msg::ChatGame || (sender && sender->className() == "Player"))) {
                    std::println("server: chat packet {:#04x} from {}: \"{}\"", chat->id,
                                 sender ? sender->fullName() : "game", chat->text);
                    for (int c : clients)
                        peer->send(c, e->data);
                    ++chatPackets;
                }
            } else if (e->kind == Peer::Event::Kind::Message && !e->data.empty() && e->data[0] == 0x83) {
                auto items = lunar::net::decodeData(e->data, replication->schema(), replication->context(), nullptr,
                                                    {}, lunar::net::ItemContext{false, lunar::net::kBaseCapabilities});
                if (!items)
                    continue;
                for (const auto& item : *items) {
                    if (const auto* m = std::get_if<lunar::net::MarkerItem>(&item))
                        std::println("server: client {} echoed marker {}: it has the whole join", e->connection,
                                     m->marker);
                    if (const auto* r = std::get_if<lunar::net::RuntimeItem>(&item)) {
                        // Pings get PingBacks (and PingBacks an RTT); a character request spawns one.
                        if (auto s = sessions.find(e->connection); s != sessions.end())
                            s->second.onItem(*r, nowMs());
                        if (std::get_if<lunar::net::RequestCharacterItem>(&r->body))
                            spawnCharacter(e->connection);
                        continue;
                    }
                    const auto* inv = std::get_if<lunar::net::EventInvocationItem>(&item);
                    if (!inv)
                        continue;
                    auto target = replication->find(inv->id);
                    const auto& ev = replication->schema().events[inv->eventId];
                    std::string args;
                    for (const auto& a : inv->arguments)
                        args += (args.empty() ? "" : ", ") + lunar::toString(a);
                    std::println("server: {}.{} from client {}: ({})", target ? target->fullName() : "?", ev.name,
                                 e->connection, args);
                    // A legacy chat message (goal 6): SayMessageRequest ->
                    // broadcast ReceiveMessage (OnMessageDoneFiltering) to all.
                    if (target && ev.name == "OnRemoteServerEvent" && target->name() == kSayMessage) {
                        auto* folder = game->getService("ReplicatedStorage")->findFirstChild(kChatFolder);
                        auto* out = folder ? folder->findFirstChild(kChatMessage) : nullptr;
                        if (out) {
                            std::vector<lunar::Value> say{lunar::Value(std::format("client{}", e->connection))};
                            say.insert(say.end(), inv->arguments.begin(), inv->arguments.end());
                            auto m = replication->invoke(*out, "OnClientEvent", std::move(say));
                            for (int c : clients)
                                peer->send(c, m);
                            ++chats;
                        }
                    } else if (target && ev.name == "OnRemoteServerEvent") {
                        std::vector<lunar::Value> reply{lunar::Value(std::string("echo"))};
                        reply.insert(reply.end(), inv->arguments.begin(), inv->arguments.end());
                        peer->send(e->connection, replication->invoke(*target, "OnClientEvent", std::move(reply)));
                        ++echoes;
                    }
                    // RemoteFunction (goal 6): OnServerInvoke adds the numeric
                    // arguments after the invocation id and the player, and returns the sum.
                    if (target && ev.name == "RemoteOnInvokeServer" && !inv->arguments.empty()) {
                        double sum = 0;
                        for (std::size_t i = 2; i < inv->arguments.size(); ++i)
                            if (const auto* d = std::get_if<double>(&inv->arguments[i]))
                                sum += *d;
                        std::vector<lunar::Value> result{inv->arguments.front(), lunar::Value(sum)};
                        peer->send(e->connection,
                                   replication->invoke(*target, "RemoteOnInvokeSuccess", std::move(result)));
                        ++invokes;
                    }
                }
            }
        }
        // The session layer's own traffic: a Ping every 500 ms and the answers to the client's.
        for (auto& [connection, session] : sessions) {
            auto due = session.take(nowMs());
            if (due.empty())
                continue;
            std::vector<lunar::net::Item> items(due.begin(), due.end());
            peer->send(connection,
                       lunar::net::encodeData(items, replication->schema(), replication->context(), {},
                                              lunar::net::ItemContext{true, lunar::net::kBaseCapabilities}));
        }
        // Something visible changing on the server: the clock, every second.
        if (!clients.empty() && Clock::now() - lastChange > std::chrono::seconds(1)) {
            lastChange = Clock::now();
            auto* lighting = game->getService("Lighting");
            const double t = std::get<double>(lighting->get("ClockTime"));
            lighting->set("ClockTime", lunar::Value(std::fmod(t + 0.5, 24.0)));
            ++changes;
            for (auto& m : replication->takeChanges())
                for (int c : clients)
                    peer->send(c, m, Reliability::ReliableOrdered);
        }
        // Stream ServerBall's motion to every client at ~15 Hz (goal 5). The
        // part's transform travels as 0x85 physics, not as a property change,
        // so it is never set on the server's own DataModel here.
        if (!clients.empty() && serverBall && nowMs() - lastPhysicsMs >= kPhysicsPeriodMs) {
            lastPhysicsMs = nowMs();
            if (!freezePhysics)
                lastServerBallPv = orbit(static_cast<double>(nowMs() - startMs) / 1000.0, 10.0, 2.0, 1.5);
            const auto id = replication->idOf(serverBall);
            auto packet = lunar::net::encodePhysicsPacket(
                lunar::net::onePartPacket(nowMs(), physicsSeq++, /*serverToClient=*/true, id, lastServerBallPv));
            for (int c : clients)
                peer->send(c, packet, Reliability::Unreliable);
            ++physicsSent;
        }
    }
};

// --net-client's state: the replica it builds.
struct Client {
    std::shared_ptr<lunar::net::Replica> replica = std::make_shared<lunar::net::Replica>();
    std::shared_ptr<lunar::net::Replica> logReplica = std::make_shared<lunar::net::Replica>();
    std::unique_ptr<Peer> peer;
    int connection = -1;
    bool joined = false, echoed = false, failed = false;
    Clock::time_point joinStart = Clock::now();
    // The session layer (part 3): pings, PingBacks, marker echoes and the
    // character request come from here; the real chat packet goes out once.
    lunar::net::ClientSession session{[] {
        lunar::net::ClientSessionOptions o;
        o.appearance = "lunarstone";
        return o;
    }()};
    bool chatPacketSent = false, chatPacketReceived = false, characterSeen = false;
    // Physics (goal 5): applies the server's ServerBall stream; once it is
    // given ownership, streams its own ClientBall motion back.
    std::string ownedBall;
    std::uint64_t physicsSeq = 0;
    std::uint64_t lastPhysicsMs = 0;
    std::uint64_t startMs = nowMs();
    int physicsApplied = 0;
    int physicsSent = 0;
    bool freezePhysics = false; // the drain phase: resend the same pose so the server converges
    PhysicsPV lastServerBallPv; // decoded ServerBall motion we applied
    PhysicsPV lastClientBallPv; // our ClientBall motion we last sent

    std::expected<void, std::string> start(const lunar::net::SystemAddress& server)
    {
        Peer::Options o;
        o.describer = lunar::net::robloxDescriber(logReplica);
        peer = std::make_unique<Peer>(Peer::Role::Client, std::move(o));
        return peer->connect(server);
    }

    void summarize() const
    {
        std::println("client: join complete: {} instances", replica->instanceCount());
        for (const auto& name : replicatedServices()) {
            if (auto* service = replica->game()->findService(name))
                std::println("client:   {}: {} descendants", name, service->descendants().size());
        }
    }

    void update(int waitMs)
    {
        peer->update(waitMs);
        while (auto e = peer->poll()) {
            if (e->kind == Peer::Event::Kind::Connected) {
                connection = e->connection;
                joinStart = Clock::now();
                lunar::net::JoinRequest request;
                request.luauMin = 0; // GUESS: the Luau bytecode versions the client loads
                request.luauMax = 6;
                request.joinInfo = R"({"Client":"Lunarstone","UserId":1,"UserName":"LunarstoneTester"})";
                peer->send(connection, lunar::net::encodeJoinRequest(request, lunar::net::kBaseCapabilities));
                std::println("client: connected, sent the join request");
            } else if (e->kind == Peer::Event::Kind::Failed) {
                std::println("client: {}", e->reason);
                failed = true;
            } else if (e->kind == Peer::Event::Kind::Disconnected) {
                std::println("client: disconnected ({})", e->reason);
                failed = true;
            } else if (e->kind == Peer::Event::Kind::Message && !e->data.empty() && e->data[0] == 0x1b) {
                // ServerBall's motion from the server (0x85, server -> client): apply it.
                auto packet = lunar::net::decodePhysicsPacket(e->data, /*serverToClient=*/true);
                if (packet)
                    for (const auto& rec : packet->records)
                        if (rec.mechanism && !rec.mechanism->parts.empty() && !rec.mechanism->parts[0].frames.empty()) {
                            const auto& pv = rec.mechanism->parts[0].frames[0].pv;
                            if (auto part = replica->find(rec.part)) {
                                applyPv(part.get(), pv);
                                lastServerBallPv = pv;
                                ++physicsApplied;
                            }
                        }
            } else if (e->kind == Peer::Event::Kind::Message) {
                const std::size_t before = replica->events.size();
                const std::size_t chatsBefore = replica->runtime.chats.size();
                session.onMessage(e->data);
                replica->handle(e->data);
                if (replica->runtime.chats.size() > chatsBefore) {
                    const auto& c = replica->runtime.chats.back();
                    auto sender = replica->find(c.sender);
                    std::println("client: chat packet {:#04x} from {}: \"{}\"", c.id,
                                 sender ? sender->fullName() : "?", c.text);
                    chatPacketReceived = true;
                }
                if (!joined && !replica->markers.empty()) {
                    joined = true; // the session echoes the marker on its next tick
                    std::println("client: join took {:.0f} ms",
                                 std::chrono::duration<double, std::milli>(Clock::now() - joinStart).count());
                    summarize();
                    fireEcho();
                    fireInvoke();
                    fireChat();
                }
                for (std::size_t i = std::max(before, replica->events.first()); i < replica->events.size(); ++i) {
                    const auto& inv = replica->events[i];
                    std::string args;
                    for (const auto& a : inv.arguments)
                        args += (args.empty() ? "" : ", ") + lunar::toString(a);
                    auto target = replica->find(inv.id);
                    const std::string& eventName = replica->schema()->events[inv.eventId].name;
                    std::println("client: {}.{}: ({})", target ? target->fullName() : "?", eventName, args);
                    // The server handing us ClientBall's physics ownership.
                    if (target && target->name() == kOwnershipEvent && !inv.arguments.empty())
                        if (const auto* name = std::get_if<std::string>(&inv.arguments.front())) {
                            ownedBall = *name;
                            std::println("client: now the physics owner of Workspace.{}", ownedBall);
                        }
                    // A broadcast chat message arriving (goal 6).
                    if (target && target->name() == kChatMessage) {
                        chatReceived = true;
                        std::println("client: chat received");
                    }
                    // The RemoteFunction's return (goal 6): id then the result.
                    if (eventName == "RemoteOnInvokeSuccess" && inv.arguments.size() >= 2) {
                        const auto* id = std::get_if<double>(&inv.arguments[0]);
                        const auto* value = std::get_if<double>(&inv.arguments[1]);
                        if (id && value && *id == pendingInvokeId) {
                            invokeReturned = true;
                            invokeResult = *value;
                            std::println("client: LunarAdd:InvokeServer returned {}", *value);
                        }
                    } else {
                        echoed = true;
                    }
                }
            }
        }
        // The session's due messages: PingBacks, pings, the marker echo, the character request.
        if (connection >= 0)
            for (auto& m : session.tick(*replica, nowMs()))
                peer->send(connection, m);
        // Our Player and character, once the server made them; then one legacy chat packet.
        if (auto* me = localPlayer()) {
            const lunar::Value character = me->get("Character");
            if (const auto* model = std::get_if<lunar::InstancePtr>(&character); model && *model)
                characterSeen = true;
            if (joined && !chatPacketSent)
                if (const auto id = replica->idOf(me)) {
                    peer->send(connection, lunar::net::ClientSession::chat(*id, kChatLine));
                    chatPacketSent = true;
                    std::println("client: sent chat packet 0x87 from {}", me->fullName());
                }
        }
        // Stream the part we own back to the server at ~15 Hz (goal 5).
        auto* workspace = replica->game() ? replica->game()->findService("Workspace") : nullptr;
        auto* ball = (!ownedBall.empty() && workspace) ? workspace->findFirstChild(ownedBall) : nullptr;
        if (connection >= 0 && ball && nowMs() - lastPhysicsMs >= kPhysicsPeriodMs) {
            lastPhysicsMs = nowMs();
            if (!freezePhysics)
                lastClientBallPv = orbit(static_cast<double>(nowMs() - startMs) / 1000.0, 10.0, 2.0, -1.5);
            applyPv(ball, lastClientBallPv);
            if (const auto id = replica->idOf(ball))
                peer->send(connection,
                           lunar::net::encodePhysicsPacket(lunar::net::onePartPacket(
                               nowMs(), physicsSeq++, /*serverToClient=*/false, *id, lastClientBallPv)),
                           Reliability::Unreliable);
            ++physicsSent;
        }
    }

    lunar::Instance* localPlayer() const
    {
        auto* players = replica->game() ? replica->game()->findService("Players") : nullptr;
        if (!players)
            return nullptr;
        for (const auto& child : players->children())
            if (child->className() == "Player")
                return child.get();
        return nullptr;
    }

    // RemoteFunction round trip (goal 6): LunarAdd:InvokeServer(2, 3) -> 5.
    double pendingInvokeId = 1;
    bool invokeReturned = false;
    double invokeResult = 0;

    void fireInvoke()
    {
        auto* storage = replica->game()->findService("ReplicatedStorage");
        auto* fn = storage ? storage->findFirstChild(kAddFunction) : nullptr;
        const auto* schema = replica->schema();
        if (!fn || !schema)
            return;
        const auto cls = schema->classId("RemoteFunction");
        const auto ev = cls ? schema->eventId(*cls, "RemoteOnInvokeServer") : std::nullopt;
        const auto id = replica->idOf(fn);
        if (!ev || !id)
            return;
        auto* me = localPlayer(); // the client sends its own Player (0x1045eb5b6)
        std::vector<lunar::Value> args{lunar::Value(pendingInvokeId),
                                       me ? lunar::Value(me->shared_from_this()) : lunar::Value(), lunar::Value(2.0),
                                       lunar::Value(3.0)};
        const std::array<lunar::net::Item, 1> items{lunar::net::EventInvocationItem{*id, *ev, std::move(args)}};
        peer->send(connection, lunar::net::encodeData(items, *schema, replica->context()));
        std::println("client: called {}:InvokeServer(2, 3)", fn->fullName());
    }

    bool chatReceived = false;

    void fireChat()
    {
        auto* storage = replica->game()->findService("ReplicatedStorage");
        auto* folder = storage ? storage->findFirstChild(kChatFolder) : nullptr;
        auto* say = folder ? folder->findFirstChild(kSayMessage) : nullptr;
        const auto* schema = replica->schema();
        if (!say || !schema)
            return;
        const auto cls = schema->classId("RemoteEvent");
        const auto ev = cls ? schema->eventId(*cls, "OnRemoteServerEvent") : std::nullopt;
        const auto id = replica->idOf(say);
        if (!ev || !id)
            return;
        std::vector<lunar::Value> args{lunar::Value(std::string("hello from lunarstone")),
                                       lunar::Value(std::string("All"))};
        const std::array<lunar::net::Item, 1> items{lunar::net::EventInvocationItem{*id, *ev, std::move(args)}};
        peer->send(connection, lunar::net::encodeData(items, *schema, replica->context()));
        std::println("client: said \"hello from lunarstone\" via {}", say->fullName());
    }

    void fireEcho()
    {
        auto* storage = replica->game()->findService("ReplicatedStorage");
        auto* echo = storage ? storage->findFirstChild(kEchoEvent) : nullptr;
        const auto* schema = replica->schema();
        if (!echo || !schema)
            return;
        const auto cls = schema->classId("RemoteEvent");
        const auto ev = cls ? schema->eventId(*cls, "OnRemoteServerEvent") : std::nullopt;
        const auto id = replica->idOf(echo);
        if (!ev || !id)
            return;
        std::vector<lunar::Value> args{lunar::Value(std::string("hello from lunarstone")), lunar::Value(42.0),
                                       lunar::Value(lunar::Vector3{1, 2, 3})};
        const std::array<lunar::net::Item, 1> items{lunar::net::EventInvocationItem{*id, *ev, std::move(args)}};
        peer->send(connection, lunar::net::encodeData(items, *schema, replica->context()));
        std::println("client: fired {}:FireServer(\"hello from lunarstone\", 42, Vector3(1, 2, 3))", echo->fullName());
    }
};

std::expected<double, int> parseSeconds(std::span<const std::string_view> args, std::size_t from, double fallback,
                                        std::uint16_t* port = nullptr)
{
    double seconds = fallback;
    for (std::size_t i = from; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size())
            seconds = std::stod(std::string(args[++i]));
        else if (port && args[i] == "--port" && i + 1 < args.size())
            *port = static_cast<std::uint16_t>(std::stoi(std::string(args[++i])));
        else
            return std::unexpected(-1);
    }
    return seconds;
}

int netServer(std::span<const std::string_view> args)
{
    std::uint16_t port = 53640;
    auto seconds = parseSeconds(args, 2, 0.0, &port);
    if (!seconds || args.size() < 2)
        return -1;
    Server server;
    if (auto ok = server.start(args[1], "0.0.0.0", port); !ok) {
        std::println(stderr, "{}", ok.error());
        return 1;
    }
    const auto start = Clock::now();
    while (*seconds <= 0 || secondsSince(start) < *seconds)
        server.update(5);
    return 0;
}

int netClient(std::span<const std::string_view> args)
{
    auto seconds = parseSeconds(args, 2, 5.0);
    if (!seconds || args.size() < 2)
        return -1;
    auto address = lunar::net::resolve(args[1], 53640);
    if (!address) {
        std::println(stderr, "{}", address.error());
        return 1;
    }
    Client client;
    if (auto ok = client.start(*address); !ok) {
        std::println(stderr, "{}", ok.error());
        return 1;
    }
    const auto start = Clock::now();
    while (secondsSince(start) < *seconds && !client.failed)
        client.update(5);
    return client.joined ? 0 : 1;
}

// Server and client in this process, then a comparison of the two DataModels.
int netLoopback(std::span<const std::string_view> args)
{
    auto seconds = parseSeconds(args, 2, 3.0);
    if (!seconds || args.size() < 2)
        return -1;
    Server server;
    if (auto ok = server.start(args[1], "127.0.0.1", 0); !ok) {
        std::println(stderr, "{}", ok.error());
        return 1;
    }
    Client client;
    if (auto ok = client.start(server.peer->localAddress()); !ok) {
        std::println(stderr, "{}", ok.error());
        return 1;
    }
    const auto start = Clock::now();
    while (secondsSince(start) < *seconds && !client.failed) {
        client.update(1);
        server.update(1);
    }
    // Let the last changes arrive. Freeze both balls so each side converges on
    // the other's final pose (physics is unreliable, so it is simply resent).
    server.freezePhysics = true;
    client.freezePhysics = true;
    for (int c : server.clients)
        server.kick(c, "loopback done");
    for (int i = 0; i < 80; ++i) {
        client.update(1);
        server.update(1);
    }

    int problems = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::println("loopback: {} {}", ok ? "ok      " : "MISMATCH", what);
        problems += ok ? 0 : 1;
    };
    check(client.joined, "join completed (marker received)");
    check(client.replica->instanceCount() == server.replication->replicatedCount(),
          std::format("instances: server replicated {}, client has {}", server.replication->replicatedCount(),
                      client.replica->instanceCount()));
    for (const auto& name : replicatedServices()) {
        auto* a = server.game->findService(name);
        auto* b = client.replica->game()->findService(name);
        if (!a)
            continue;
        const std::size_t na = a->descendants().size(), nb = b ? b->descendants().size() : 0;
        check(na == nb, std::format("{}: {} descendants on the server, {} on the client", name, na, nb));
    }
    // Every property of every replicated instance, matched through the instance ids.
    std::size_t compared = 0, different = 0;
    std::string firstDifference;
    for (const auto& name : replicatedServices()) {
        auto* service = server.game->findService(name);
        if (!service)
            continue;
        for (const auto& a : service->descendants()) {
            // The two physics balls move via 0x85, not property replication;
            // their motion is checked separately below.
            if (a->name() == kServerBall || a->name() == kClientBall)
                continue;
            auto b = client.replica->find(server.replication->idOf(a.get()));
            if (!b)
                continue;
            for (const auto* p : a->properties()) {
                if (p->readOnly() || p->name == "Parent")
                    continue;
                const lunar::Value va = a->get(p->name), vb = b->get(p->name);
                ++compared;
                if (lunar::toString(va) != lunar::toString(vb)) {
                    ++different;
                    if (std::getenv("LUNAR_NET_DIFFS"))
                        std::println("diff {} {}.{}: {} vs {}", a->className(), a->fullName(), p->name,
                                     lunar::toString(va), lunar::toString(vb));
                    if (firstDifference.empty())
                        firstDifference = std::format("{}.{}: {} vs {}", a->fullName(), p->name, lunar::toString(va),
                                                      lunar::toString(vb));
                }
            }
        }
    }
    check(different == 0, std::format("{} property values compared, {} differ{}", compared, different,
                                      firstDifference.empty() ? "" : " (first: " + firstDifference + ")"));
    const auto clockA = lunar::toString(server.game->getService("Lighting")->get("ClockTime"));
    const auto clockB = lunar::toString(client.replica->game()->getService("Lighting")->get("ClockTime"));
    check(server.changes > 0 && clockA == clockB,
          std::format("Lighting.ClockTime after {} server-side changes: {} vs {}", server.changes, clockA, clockB));
    check(server.echoes > 0 && client.echoed, "RemoteEvent round trip (FireServer -> OnServerEvent -> OnClientEvent)");
    check(server.invokes > 0 && client.invokeReturned && client.invokeResult == 5.0,
          std::format("RemoteFunction round trip (LunarAdd:InvokeServer(2, 3) -> {})", client.invokeResult));
    check(server.chats > 0 && client.chatReceived,
          "legacy chat round trip (SayMessageRequest -> OnMessageDoneFiltering)");
    // The session layer (part 3).
    std::size_t serverRtts = 0, serverPingBacks = 0;
    for (const auto& [c, s] : server.sessions) {
        serverRtts += s.roundTripsMs.size();
        serverPingBacks += s.pingBacksSent;
    }
    check(client.session.pingsSent > 0 && serverPingBacks > 0 && !client.session.roundTripsMs.empty(),
          std::format("Ping/PingBack client -> server -> client: {} pings, {} answered, last RTT {} ms",
                      client.session.pingsSent, serverPingBacks,
                      client.session.roundTripsMs.empty() ? 0u : client.session.roundTripsMs.back()));
    check(client.session.pingBacksSent > 0 && serverRtts > 0,
          std::format("Ping/PingBack server -> client -> server: {} answered by the client, {} RTTs on the server",
                      client.session.pingBacksSent, serverRtts));
    check(client.session.markersEchoed > 0, "marker echoed by the session");
    check(client.session.characterRequested && server.charactersSpawned > 0 && client.characterSeen,
          std::format("Request Character item -> character spawned ({}), Player.Character replicated",
                      server.charactersSpawned));
    check(client.chatPacketSent && server.chatPackets > 0 && client.chatPacketReceived,
          "legacy chat packet 0x87 (client -> server -> all clients)");
    check(server.kicks > 0 && client.session.kicked && client.session.kicked->message == "loopback done",
          "ID_KICK received with its message");

    // Physics (goal 5): the client received and applied ServerBall's motion,
    // and the server received the client's ClientBall motion, both within a
    // quantization step of what the sender meant.
    auto near = [](lunar::Vector3 x, lunar::Vector3 y, float tol) {
        return std::abs(x.x - y.x) <= tol && std::abs(x.y - y.y) <= tol && std::abs(x.z - y.z) <= tol;
    };
    check(server.physicsSent > 0 && client.physicsApplied > 0
              && near(client.lastServerBallPv.cframe.position, server.lastServerBallPv.cframe.position, 0.05f),
          std::format("ServerBall physics (0x85) server -> client: {} sent, {} applied, client at ({}, {}, {})",
                      server.physicsSent, client.physicsApplied, client.lastServerBallPv.cframe.position.x,
                      client.lastServerBallPv.cframe.position.y, client.lastServerBallPv.cframe.position.z));
    check(client.physicsSent > 0 && server.physicsFromClient > 0 && !client.ownedBall.empty()
              && near(server.lastClientBallPv.cframe.position, client.lastClientBallPv.cframe.position, 0.05f),
          std::format("ClientBall physics owner client -> server: {} sent, {} received, server at ({}, {}, {})",
                      client.physicsSent, server.physicsFromClient, server.lastClientBallPv.cframe.position.x,
                      server.lastClientBallPv.cframe.position.y, server.lastClientBallPv.cframe.position.z));
    return problems == 0 ? 0 : 1;
}

int netDecode(std::span<const std::string_view> args)
{
    if (args.size() < 2)
        return -1;
    const std::filesystem::path capture(args[1]);
    std::filesystem::path keylog;
    lunar::net::DescribeOptions options;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--keylog" && i + 1 < args.size())
            keylog = args[++i];
        else if (args[i] == "--hex")
            options.fullHex = true;
        else if (args[i] == "--plain")
            options.plaintext = true;
        else
            return -1;
    }
    auto datagrams = lunar::net::readCapture(capture);
    if (!datagrams) {
        std::println(stderr, "{}", datagrams.error());
        return 1;
    }
    lunar::net::CaptureDecoder decoder(options);
    if (!keylog.empty())
        std::println("keylog: {} session(s) from {}", decoder.loadKeylog(keylog), keylog.string());
    std::println("{}: {} UDP datagram(s)", capture.string(), datagrams->size());
    for (std::size_t i = 0; i < datagrams->size(); ++i)
        std::print("{}", lunar::net::render(decoder.decode((*datagrams)[i], i)));
    return 0;
}

// A live game's script Source (Roblox's protected bytecode) as Luau bytecode
// with scrambled opcodes, for ScriptContext::setNetworkBytecode.
std::optional<std::string> protectedScriptCode(std::string_view source)
{
    auto code = lunar::net::decodeProtectedBytecode(source);
    return code ? std::optional(std::move(*code)) : std::nullopt;
}

// Where each assembly the server moves is (its model's name and root
// position), for --motion: one line per root.
void printMotion(const lunar::net::Replica& replica, double sinceStart)
{
    for (const lunar::net::InstanceId id : replica.motion().rootIds()) {
        const auto part = std::dynamic_pointer_cast<lunar::BasePart>(replica.find(id));
        if (!part)
            continue;
        const lunar::Instance* model = part->parent();
        const lunar::Vector3 p = part->cframe().position;
        const lunar::Vector3 v = part->assemblyLinearVelocity();
        std::println("motion: {:8.3f} s  {:<40} ({:9.3f}, {:8.3f}, {:9.3f})  speed {:5.2f}", sinceStart,
                     std::format("{}.{}#{}", model ? model->name() : "-", part->name(), id.index), p.x, p.y, p.z,
                     v.magnitude());
    }
}

// What --net-replay --walk sends: each message decoded back as the server
// would read it, summarised; false when one doesn't decode.
bool checkOutgoing(const lunar::net::Replica& replica, const lunar::net::CharacterSync::Outgoing& out, double time,
                   std::map<std::string, int>& seen)
{
    const std::vector<std::uint8_t>& m = out.message;
    if (!m.empty() && m[0] == 0x1b) {
        auto packet = lunar::net::decodePhysicsPacket(m, /*serverToClient=*/false, replica.framing);
        if (!packet || packet->records.size() != 1 || !packet->records[0].mechanism) {
            std::println("replay: walk: {:.2f}s physics packet did not decode: {}", time,
                         packet ? "no mechanism" : packet.error());
            return false;
        }
        const auto& mech = *packet->records[0].mechanism;
        if (++seen["physics"] <= 3 || seen["physics"] % 15 == 0)
            std::println("replay: walk: {:.2f}s 0x85 #{} root {}:{} state {} {}", time, packet->sequence,
                         packet->records[0].part.scope, packet->records[0].part.index, mech.humanoidState,
                         lunar::net::pvText(mech.parts.at(0).frames.at(0).pv));
        return true;
    }
    // Our character's touches (ID_TOUCHES), by the parts' names.
    if (auto plain = lunar::net::unframe(m, replica.framing); plain && !plain->empty() && (*plain)[0] == 0x86) {
        auto touches = lunar::net::decodeTouches(*plain);
        if (!touches) {
            std::println("replay: walk: {:.2f}s ID_TOUCHES did not decode", time);
            return false;
        }
        for (const lunar::net::TouchEvent& t : touches->touches) {
            const auto a = replica.find(t.partA), b = replica.find(t.partB);
            std::println("replay: walk: {:.2f}s touch state {}: {} / {}", time, t.state,
                         a ? a->fullName() : std::string("?"), b ? b->fullName() : std::string("?"));
        }
        ++seen["touches"];
        return true;
    }
    const lunar::net::Schema* schema = replica.schema();
    auto items = schema ? lunar::net::decodeData(m, *schema, replica.context(), nullptr, replica.framing,
                                                 lunar::net::ItemContext{false, replica.capabilities})
                        : std::unexpected(std::string("no schema"));
    if (!items) {
        std::println("replay: walk: {:.2f}s ID_DATA did not decode: {}", time, items.error());
        return false;
    }
    for (const lunar::net::Item& item : *items) {
        std::string line;
        if (const auto* c = std::get_if<lunar::net::ChangePropertyItem>(&item))
            line = std::format("{} = {}", schema->properties.at(c->propertyId).name, lunar::toString(c->value));
        else if (const auto* e = std::get_if<lunar::net::EventInvocationItem>(&item)) {
            line = schema->events.at(e->eventId).name + "(";
            for (const lunar::Value& v : e->arguments)
                line += lunar::toString(v) + (&v == &e->arguments.back() ? ")" : ", ");
        } else
            continue;
        const std::string key = line.substr(0, line.find_first_of("=("));
        if (++seen[key] <= 12 || std::getenv("LUNAR_WALK_ALL"))
            std::println("replay: walk: {:.2f}s {}", time, line);
    }
    return true;
}

struct TimedSound {
    bool on = false;
    bool mute = false;
};

// --net-replay <prefix> --timed --run <seconds> [--sound [--mute]] [--render out.png]: the capture played back in
// its own time with the client's scripts running alongside from when the
// recorded client started them, as --net-play --replay does but without the
// window: what the server sent later (its answers to InvokeServer, remote
// events, instances it took away) comes when it came. Plain --run starts the
// scripts on what the whole capture left, which misses anything the server
// removed once the client had it (Adonis's client, deleted once loaded).
// What the scripts send is only logged; our character stands.
int netReplayTimed(const std::string& prefix, std::vector<lunar::net::CapturedDatagram> datagrams, double runSeconds,
                   double fps, const std::string& platform, const TimedSound& sound,
                   const std::filesystem::path& renderPath, const lunar::render::GraphicsSettings& graphics)
{
    // Only the session with the server, in time order (as ReplayFeed).
    const auto pairOf = [](const lunar::net::CapturedDatagram& d) {
        const std::string a = d.from.toString(), b = d.to.toString();
        return std::min(a, b) + "|" + std::max(a, b);
    };
    std::map<std::string, std::size_t> counts;
    for (const auto& d : datagrams)
        ++counts[pairOf(d)];
    const std::string session = std::ranges::max_element(counts, {}, [](const auto& c) { return c.second; })->first;
    std::erase_if(datagrams, [&](const lunar::net::CapturedDatagram& d) { return pairOf(d) != session; });
    std::ranges::stable_sort(datagrams, {}, &lunar::net::CapturedDatagram::time);
    const RecordedJoin join = recordedJoin(prefix);
    lunar::net::CaptureDecoder decoder(lunar::net::DescribeOptions{.quiet = true});
    decoder.loadKeylog(prefix + ".keys");
    // The first datagram makes the session's flow and its replica.
    std::size_t next = 0;
    while (next < datagrams.size() && decoder.replicas().empty()) {
        decoder.decode(datagrams[next], next);
        ++next;
    }
    if (decoder.replicas().empty()) {
        std::println(stderr, "replay: no session in {}.pcap", prefix);
        return 1;
    }
    std::shared_ptr<lunar::net::Replica> replica = decoder.replicas().front();
    const bool world = std::getenv("LUNAR_REPLAY_WORLD") != nullptr;
    replica->motionFromClients = !world;
    replica->game()->assignJob(join.jobId, join.placeId);
    lunar::assets::AssetCache::setSessionPlaceId(static_cast<std::uint64_t>(join.placeId));
    LiveClient live(replica, LiveClient::Options{.userId = join.userId,
                                                 .platform = platform,
                                                 .note =
                                                     [](const std::string& line) {
                                                         std::println("replay: {}", line);
                                                         std::fflush(stdout);
                                                     },
                                                 // research: LUNAR_REPLAY_WORLD=1 gives the replay the live
                                                 // window's physics World (our character standing, what the
                                                 // server hands us simulated), as the live client has it.
                                                 .simulateCharacter = world});
    const std::vector<double> rates = lunar::platform::displayRefreshRates();
    const double dt = 1.0 / (fps > 0 ? fps : rates.empty() ? 60.0 : std::ranges::max(rates));
    const double first = datagrams.front().time;
    std::uint64_t frames = 0;
    // --sound: the game's Sounds too, as the live client has them, heard
    // from the camera (or mixed and thrown away with --mute), and the replay
    // kept to the wall clock so their downloads take the time they take.
    std::unique_ptr<lunar::audio::SoundSystem> sounds;
    std::vector<float> block;
    const auto wallStart = std::chrono::steady_clock::now();
    double mixed = 0.0; // seconds of sound mixed by hand (--mute)
    for (double t = 0; t < runSeconds; t += dt) {
        while (next < datagrams.size() && datagrams[next].time - first <= t) {
            decoder.decode(datagrams[next], next);
            ++next;
        }
        replica->runtime.pendingPings.clear(); // nobody answers the server's pings here
        if (sound.on)
            std::this_thread::sleep_until(wallStart + std::chrono::duration<double>(t));
        if (!live.start(next == datagrams.size()))
            continue;
        live.step(dt);
        ++frames;
        if (sound.on) {
            lunar::DataModel& game = *replica->game();
            if (!sounds) {
                auto cache = std::make_shared<lunar::assets::AssetCache>();
                cache->setPlaceId(static_cast<std::uint64_t>(join.placeId));
                sounds = std::make_unique<lunar::audio::SoundSystem>(game, std::move(cache), !sound.mute);
            }
            const lunar::Camera* camera = game.workspace() ? game.workspace()->currentCamera() : nullptr;
            sounds->update(camera ? camera->cframe() : lunar::CFrame());
            if (sound.mute || !sounds->mixer().hasOutput()) {
                // No device to pull the mix: it's made here, in step with the game.
                for (; mixed + 0.01 <= t + dt; mixed += 0.01) {
                    block.assign(std::size_t{lunar::audio::Mixer::rate / 100} * 2, 0.0f);
                    sounds->mixer().mix(block);
                }
            }
            if (frames % static_cast<std::uint64_t>(5.0 / dt) == 0) {
                const auto s = sounds->stats();
                std::println("replay: {:.0f} s: sound: {} playing of {}, {} loaded ({:.0f} MB), {} failed", t,
                             s.playing, s.sounds, s.clips, static_cast<double>(s.bytes) / (1 << 20), s.failed);
            }
        }
        (void)live.remotes().take(); // a recording has no server to hear them
    }
    const lunar::net::RemoteBridge& remotes = live.remotes();
    std::println("replay: {} of {} datagrams played over {:.1f} s, {} client frames", next, datagrams.size(),
                 runSeconds, frames);
    std::println("replay: remotes: {} sent (to nobody), {} received, {} dropped", remotes.sent, remotes.received,
                 remotes.dropped);
    for (const auto& [what, n] : remotes.counts())
        std::println("replay:   {} x{}", what, n);
    // --render: the picture where the run ended, mid-effect if the run
    // stopped in one (a plain --run replays every remote event at once).
    if (!renderPath.empty())
        return renderReplica(replica->game(), renderPath, join.userId, graphics);
    return 0;
}

// --net-replay <prefix> [--scripts] [--run seconds [--say text | --say-preset phrase]] [--render out.png]: what a session recorded with
// --net-join --out <prefix> left the client with, rebuilt offline from the
// capture, its keylog and the masked join script. With --run, --equip and co.
// move our tools as --net-join does, and what that would send is decoded.
// --at (or --until) <seconds> stops the capture that far in, to see the
// client's state at that moment; --motion <seconds> plays the
// server's physics back as a client would while decoding and prints where
// each moving assembly is every that many seconds. --bench-replay times the
// frames instead (BenchReplay.cpp) and leaves the rest of the capture alone.
int netReplay(std::span<const std::string_view> args)
{
    if (args.size() < 2)
        return -1;
    const std::string prefix(args[1]);
    std::filesystem::path renderPath;
    bool listScripts = false;
    std::filesystem::path dumpDir;
    double runSeconds = 0;
    std::optional<WalkPlan> walk;
    double atSeconds = -1;
    double motionEvery = 0;
    std::string scriptPlatform = "Linux"; // what --net-join says by default
    double fps = 0;
    bool timed = false;
    TimedSound timedSound;
    std::string say;       // a chat message the client sends 2 s in (only logged: nobody listens)
    std::string sayPreset; // a preset chat phrase, the same way
    lunar::render::GraphicsSettings graphics;
    // What the graphics flags asked for, which is what the benchmark draws
    // with: the Max preset unless --graphics names another (BenchGraphics.hpp
    // has its own reading of them, since parseGraphicsFlag below takes a
    // flag's value with it and the two must not share one pass).
    GraphicsChoice graphicsChoice;
    BenchOptions bench;
    std::string graphicsError;
    ToolActions toolActions;
    bool benchmarking = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        std::string benchError;
        graphicsChoice.parse(args, i, benchError); // leaves i where it is
        if (parseGraphicsFlag(args, i, graphics, graphicsError) || toolActions.parse(args, i, graphicsError)) {
            if (!graphicsError.empty() || !benchError.empty()) {
                std::println(stderr, "replay: {}", graphicsError.empty() ? benchError : graphicsError);
                return 1;
            }
            continue;
        }
        if (!benchError.empty()) {
            std::println(stderr, "replay: {}", benchError);
            return 1;
        }
        if (args[i] == "--render" && i + 1 < args.size())
            renderPath = args[++i];
        else if (args[i] == "--run" && i + 1 < args.size())
            runSeconds = std::stod(std::string(args[++i])); // the client's scripts, for that much game time
        else if (args[i] == "--at" && i + 1 < args.size())
            atSeconds = std::stod(std::string(args[++i])); // only the first that many seconds of the capture
        else if (args[i] == "--motion" && i + 1 < args.size())
            motionEvery = std::stod(std::string(args[++i])); // print the moving assemblies every that many seconds
        else if (args[i] == "--script-platform" && i + 1 < args.size())
            scriptPlatform = args[++i]; // an Enum.Platform name for UserInputService:GetPlatform()
        else if (args[i] == "--fps" && i + 1 < args.size()) {
            const std::string_view value = args[++i]; // frames a second; vsync: the display's
            fps = value == "vsync" ? 0.0 : std::stod(std::string(value));
        }
        else if (args[i] == "--until" && i + 1 < args.size())
            atSeconds = std::stod(std::string(args[++i])); // the replica as it was that many seconds in, as --at
        else if (args[i] == "--walk" && i + 1 < args.size()) {
            walk = WalkPlan::parse(args[++i]); // our character walks (LiveCharacter), what it would send is checked
            if (!walk)
                return -1;
        } else if (args[i] == "--scripts")
            listScripts = true; // every script the server sent, and whether its code opens
        else if (args[i] == "--say" && i + 1 < args.size())
            say = args[++i];
        else if (args[i] == "--say-preset" && i + 1 < args.size())
            sayPreset = args[++i];
        else if (args[i] == "--dump-scripts" && i + 1 < args.size())
            dumpDir = args[++i]; // research: each script's Source as the server sent it (and unwrapped), a file each
        else if (args[i] == "--timed")
            timed = true; // with --run: the scripts run alongside the capture, in its time
        else if (args[i] == "--sound")
            timedSound.on = true; // with --timed: the game's Sounds play (in real time)
        else if (args[i] == "--mute")
            timedSound.mute = true; // with --sound: mixed, but not to the sound device
        else if (args[i] == "--bench-replay" && i + 1 < args.size()) {
            benchmarking = true;
            bench.frames = std::max(1, std::stoi(std::string(args[++i]))); // frames to measure, drawn offscreen
        }
        else if (args[i] == "--bench-warmup" && i + 1 < args.size())
            bench.warmup = std::max(0, std::stoi(std::string(args[++i]))); // frames before them, timed but left out
        else if (args[i] == "--bench-size" && i + 1 < args.size()) {
            // "1920x1080", the size every frame is drawn and timed at.
            const std::string_view size = args[++i];
            const std::size_t x = size.find('x');
            unsigned width = 0, height = 0;
            const auto number = [&size](std::string_view digits, unsigned& into) {
                const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), into);
                return ec == std::errc{} && end == digits.data() + digits.size();
            };
            if (x == std::string_view::npos || !number(size.substr(0, x), width) || !number(size.substr(x + 1), height)
                || !width || !height) {
                std::println(stderr, "replay: --bench-size {}: a width and a height, 1920x1080", size);
                return -1;
            }
            bench.width = width;
            bench.height = height;
        } else if (args[i] == "--bench-at" && i + 1 < args.size())
            bench.at = std::max(0.0, std::stod(std::string(args[++i]))); // seconds in before the first frame
        else if (args[i] == "--bench-out" && i + 1 < args.size())
            bench.out = args[++i]; // the results as JSON, for tools/bench_compare.py
        else
            return -1;
    }
    if (benchmarking) {
        bench.recording = prefix;
        bench.graphics = graphicsChoice.settings(lunar::render::GraphicsPreset::Max);
        bench.preset = graphicsChoice.name(lunar::render::GraphicsPreset::Max); // what it will really draw with
        bench.scriptPlatform = scriptPlatform;
        bench.fps = fps > 0 ? fps : 60.0; // a fixed timestep, or two runs are not comparable
        return benchReplay(bench);
    }
    auto datagrams = lunar::net::readCapture(prefix + ".pcap");
    if (!datagrams) {
        std::println(stderr, "{}", datagrams.error());
        return 1;
    }
    if (timed) {
        if (runSeconds <= 0 || datagrams->empty())
            return -1;
        return netReplayTimed(prefix, std::move(*datagrams), runSeconds, fps, scriptPlatform, timedSound, renderPath,
                              graphics);
    }
    const auto [userId, jobId, placeId] = recordedJoin(prefix);
    lunar::net::CaptureDecoder decoder(lunar::net::DescribeOptions{.quiet = true});
    decoder.loadKeylog(prefix + ".keys");
    const double start = datagrams->empty() ? 0.0 : datagrams->front().time;
    double end = start; // the capture time decoded up to: the motion playback's clock from here on
    double nextMotion = start + motionEvery;
    auto biggest = [&decoder] {
        std::shared_ptr<lunar::net::Replica> best;
        for (const auto& r : decoder.replicas())
            if (!best || r->instanceCount() > best->instanceCount())
                best = r;
        return best;
    };
    std::size_t keptAll = 0;
    for (std::size_t i = 0; i < datagrams->size(); ++i) {
        const double t = (*datagrams)[i].time;
        if (atSeconds >= 0 && t - start > atSeconds)
            break;
        // The physics playback runs as a client's would: stepped at each
        // print time with what had arrived by then.
        for (; motionEvery > 0 && t >= nextMotion; nextMotion += motionEvery)
            if (const auto r = biggest()) {
                r->stepMotion(nextMotion);
                printMotion(*r, nextMotion - start);
            }
        decoder.decode((*datagrams)[i], i);
        end = t;
        // --run replays every remote event of the capture to the scripts:
        // the replicas keep them all.
        if (i == 0 || decoder.replicas().size() != keptAll) {
            for (const auto& r : decoder.replicas())
                r->events.setLimit(0);
            keptAll = decoder.replicas().size();
        }
    }
    std::shared_ptr<lunar::net::Replica> replica = biggest();
    if (!replica || !replica->game()) {
        std::println(stderr, "replay: no session in {}.pcap", prefix);
        return 1;
    }
    if (motionEvery > 0)
        std::println("replay: physics: {} samples for {} moving assemblies, {} for instances not streamed in",
                     replica->motion().received(), replica->motion().roots(), replica->motionForUnknown);
    replica->game()->assignJob(jobId, placeId);
    lunar::assets::AssetCache::setSessionPlaceId(static_cast<std::uint64_t>(placeId));
    std::println("replay: {} datagrams, {} instances, user {}", datagrams->size(), replica->instanceCount(), userId);
    if (const lunar::Instance* ws = replica->game()->workspace())
        if (const auto* terrain = dynamic_cast<const lunar::Terrain*>(ws->findFirstChildOfClass("Terrain")))
            std::println("replay: terrain: {} chunks", terrain->chunks().size());
    if (!dumpDir.empty()) {
        std::filesystem::create_directories(dumpDir);
        int dumped = 0;
        for (const auto& d : replica->game()->descendants())
            if (const auto* s = dynamic_cast<const lunar::LuaSourceContainer*>(d.get()); s && !s->source().empty()) {
                std::string name = s->fullName();
                std::ranges::replace(name, '/', '_');
                std::ofstream(dumpDir / (name + ".rsb"), std::ios::binary) << s->source();
                // And unwrapped: Luau bytecode (opcodes still scrambled), its strings readable.
                if (auto code = lunar::net::decodeProtectedBytecode(s->source()))
                    std::ofstream(dumpDir / (name + ".luauc"), std::ios::binary) << *code;
                ++dumped;
            }
        std::println("replay: {} scripts in {}", dumped, dumpDir.string());
    }
    if (listScripts)
        for (const auto& d : replica->game()->descendants())
            if (const auto* s = dynamic_cast<const lunar::LuaSourceContainer*>(d.get())) {
                const std::string& source = s->source();
                auto code = lunar::net::decodeProtectedBytecode(source);
                std::println("  {:<12} {:<70} {:>6} bytes  {}", s->className(), s->fullName(), source.size(),
                             source.empty() ? "empty"
                             : code         ? std::format("bytecode v{}, {} bytes", code->empty() ? 0 : int(static_cast<unsigned char>((*code)[0])), code->size())
                                            : code.error());
            }
    LiveAnimation animation(replica); // outlives the client, which plays into it
    ChatBubbles bubbles(replica->game()); // what was said over the heads, for --render
    double gameTime = 0;
    double clock = end; // the scripts' frames run on from the end of the capture
    animation.setClock([&clock] { return clock; });
    std::unique_ptr<lunar::ClientSession> client;
    if (runSeconds > 0) {
        client = std::make_unique<lunar::ClientSession>(replica->game());
        animation.attach(*client);
        animation.setWaitForClips(true);
        client->client().setNetworkBytecode(protectedScriptCode);
        if (!client->client().setPlatform(scriptPlatform)) {
            std::println(stderr, "replay: Enum.Platform has no {}", scriptPlatform);
            return 1;
        }
        client->client().setOutput([](lunar::MessageType type, std::string_view text) {
            std::println("[client] {}{}", type == lunar::MessageType::Error     ? "error: "
                                          : type == lunar::MessageType::Warning ? "warn: "
                                                                                : "",
                         text);
            std::fflush(stdout); // a script that hangs the client still leaves what it said
        });
        client->client().setChatSink([&](const lunar::ChatLine& line) {
            std::println("{}", describeChatLine(line));
            bubbles.add(line, gameTime);
        });
        lunar::Player* ours = ourPlayer(*replica, userId);
        if (!ours || !client->start(*ours)) {
            std::println(stderr, "replay: no player with user id {} in the replica", userId);
            return 1;
        }
        // research: LUNAR_NET_CLIENT_LUA=<file> runs that Luau in the client's context too, as --net-join does.
        if (const char* path = std::getenv("LUNAR_NET_CLIENT_LUA"); path && *path) {
            if (auto source = lunar::readFile(path); !source)
                std::println(stderr, "probe: {}", source.error());
            else if (auto ran = client->client().run(*source, "Probe"); !ran)
                std::println(stderr, "probe: {}", ran.error().message);
        }
        // The recording's remote events reach the scripts as if they had just
        // come; what the scripts send is only logged.
        lunar::net::RemoteBridge remotes(replica);
        remotes.log = [](const std::string& line) { std::println("{}", line); };
        remotes.logLimit = 1000;
        remotes.replay(replica->events.kept());
        client->client().setRemoteSink(
            [&remotes](lunar::Instance& remote, std::string_view event, std::vector<lunar::RemoteValue> values) {
                remotes.send(remote, event, std::move(values));
            });
        // What our tools would send, decoded as the server would read it.
        lunar::net::ToolReplicator tools(replica);
        tools.log = [](const std::string& line) { std::println("{}", line); };
        auto showTools = [&] {
            for (const auto& message : tools.take()) {
                lunar::net::Node tree("to the server: ID_DATA");
                auto items = lunar::net::decodeData(message, *replica->schema(), replica->context(), &tree,
                                                    replica->framing, lunar::net::ItemContext{false});
                std::print("{}", lunar::net::render(tree));
                if (!items)
                    std::println("replay: our own message did not decode: {}", items.error());
            }
        };
        remotes.setReceiver(
            [&client](lunar::Instance& remote, std::string_view event, std::span<const lunar::RemoteValue> values) {
                client->client().receiveRemote(remote, event, values);
            });
        // --walk: our character, simulated and told to nobody; what it
        // would send is decoded back and summarised.
        std::unique_ptr<LiveCharacter> self;
        if (walk) {
            self = std::make_unique<LiveCharacter>(replica, animation.system());
            self->attach(*client);
        }
        std::optional<double> walkFrom;
        std::map<std::string, int> walkSeen;
        int walkBad = 0;
        double nextReport = 0;
        // Vsync: the display's refresh rate, as --net-join runs them.
        const std::vector<double> rates = lunar::platform::displayRefreshRates();
        const double dt = 1.0 / (fps > 0 ? fps : rates.empty() ? 60.0 : std::ranges::max(rates));
        const auto stepping = std::chrono::steady_clock::now();
        TimedClicks clicks = TimedClicks::fromEnvironment(); // research: LUNAR_NET_CLICK
        for (double t = 0; t < runSeconds; t += dt) {
            gameTime = t;
            toolActions.step(*client, t, [](std::string line) { std::println("replay: {}", line); });
            clicks.step(ourPlayer(*replica, userId), t, [](std::string line) { std::println("replay: {}", line); });
            clock += dt;
            if (self) {
                if (!walkFrom && self->character())
                    walkFrom = t;
                const WalkPlan::Input in = walk->at(walkFrom ? t - *walkFrom : 0.0);
                self->setMoveVector(walkFrom ? in.move : lunar::Vector3{}, false);
                self->setJump(walkFrom && in.jump);
            }
            client->step(dt);
            if (self) {
                for (const auto& out : self->take(1'000'000 + static_cast<std::uint64_t>(t * 1000.0)))
                    walkBad += checkOutgoing(*replica, out, t, walkSeen) ? 0 : 1;
                if (t >= nextReport) {
                    std::println("replay: walk: {:.2f}s {}", t, self->describe());
                    nextReport += 1.0;
                }
            }
            bubbles.update(t);
            if (!say.empty() && t >= 2.0) {
                if (auto sent = client->client().sendChat(say))
                    std::println("chat: said \"{}\" (message id {})", say, *sent);
                else
                    std::println("chat: can't say it: {}", sent.error());
                say.clear();
            }
            if (!sayPreset.empty() && t >= 2.0) {
                const lunar::assets::RobloxWeb web;
                auto said = sayPresetPhrase(client->client(), web, sayPreset);
                std::println("chat: {}", said ? *said : "can't say the preset: " + said.error());
                sayPreset.clear();
            }
            remotes.take();
            showTools();
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - stepping).count();
        std::println("replay: ran the client's scripts for {:.1f} s at {:.0f} frames a second in {:.2f} s ({:.1f} us a "
                     "frame); remotes: {} sent, {} received, {} dropped",
                     runSeconds, 1.0 / dt, wall, wall / (runSeconds / dt) * 1e6, remotes.sent, remotes.received,
                     remotes.dropped);
        for (const auto& [what, n] : remotes.counts())
            std::println("replay:   {} x{}", what, n);
        std::println("replay: tools: {} items, {} dropped", tools.sent, tools.dropped);
        if (self)
            std::println("replay: walk {}: {}; {} messages did not decode", walk->text(), self->describe(), walkBad);
        for (const std::string& line : animation.describe())
            std::println("replay: animation: {}", line);
        if (bubbles.count())
            std::println("replay: {} chat bubbles showing", bubbles.count());
    }
    if (!renderPath.empty())
        return renderReplica(replica->game(), renderPath, userId, graphics);
    return 0;
}

// --net-play --replay: a recorded session played back as it happened, for
// the window to show as if it were live. Only the datagrams between the
// client and the server it talked to most are fed, each at the time it was
// recorded (CaptureDecoder builds the replica as the recorded client did,
// its own messages included); what our client sends goes nowhere.
class ReplayFeed final : public GameFeed {
public:
    ReplayFeed(std::vector<lunar::net::CapturedDatagram> datagrams, const std::string& prefix, std::string platform)
        : datagrams_(std::move(datagrams)), join_(recordedJoin(prefix))
    {
        decoder_.loadKeylog(prefix + ".keys");
        // The first datagram makes the session's flow, and with it the
        // replica the window draws from the start.
        while (next_ < datagrams_.size() && decoder_.replicas().empty()) {
            decoder_.decode(datagrams_[next_], next_);
            ++next_;
        }
        replica_ = decoder_.replicas().empty() ? std::make_shared<lunar::net::Replica>() : decoder_.replicas().front();
        replica_->game()->assignJob(join_.jobId, join_.placeId);
        lunar::assets::AssetCache::setSessionPlaceId(static_cast<std::uint64_t>(join_.placeId));
        live_ = std::make_unique<LiveClient>(replica_,
                                             LiveClient::Options{.userId = join_.userId,
                                                                 .platform = std::move(platform),
                                                                 .note = [this](std::string line) { note(line); },
                                                                 .configureClient = [this](lunar::ClientSession& clientSession) {
                                                                     if (configureClient) configureClient(clientSession);
                                                                 }});
        std::println("replay: {} datagrams over {:.1f} s, user {}", datagrams_.size(), duration(), join_.userId);
    }

    void pump() override
    {
        // What was recorded by now, but not more than 8 ms of decoding at a
        // time while the join's burst is caught up, and frameMessageBudget
        // once the game is going, as the live feed takes it.
        const double now = secondsSince(start_);
        const double first = datagrams_.front().time;
        const auto until
            = Clock::now()
              + (live_->started() ? std::chrono::duration_cast<Clock::duration>(frameMessageBudget)
                                  : std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(8)));
        while (next_ < datagrams_.size() && datagrams_[next_].time - first <= now && Clock::now() < until) {
            decoder_.decode(datagrams_[next_], next_);
            ++next_;
        }
        // Nobody answers the server's pings here.
        replica_->runtime.pendingPings.clear();
        if (next_ == datagrams_.size() && !ended_) {
            ended_ = true;
            note(std::format("the recording ends ({} instances); the game goes on without a server",
                             replica_->instanceCount()));
        }
    }
    void frame(double dt) override
    {
        if (!live_->start(ended_))
            return;
        live_->step(dt);
        ++frames_;
        // A recording has no server to hear them; what our character would
        // send is made all the same, as a live session makes it.
        if (LiveCharacter* self = live_->character())
            (void)self->take(static_cast<std::uint64_t>(secondsSince(start_) * 1000.0));
        for (auto& out : live_->remotes().take())
            (void)out;
    }
    const std::shared_ptr<lunar::net::Replica>& replica() const override { return replica_; }
    LiveClient* client() override { return live_->started() ? live_.get() : nullptr; }
    std::int64_t userId() const override { return join_.userId; }
    std::optional<std::string> ended() const override { return std::nullopt; }
    void leave() override
    {
        std::println("replay: {} of {} datagrams played in {:.1f} s, {} client frames; {} instances", next_,
                     datagrams_.size(), secondsSince(start_), frames_, replica_->instanceCount());
        std::println("replay: remotes: {} sent (to nobody), {} received, {} dropped", live_->remotes().sent,
                     live_->remotes().received, live_->remotes().dropped);
        if (live_->animation())
            for (const std::string& line : live_->animation()->describe())
                std::println("replay: animation: {}", line);
    }

private:
    double duration() const { return datagrams_.back().time - datagrams_.front().time; }
    void note(const std::string& line) { std::println("replay: {:7.3f}s {}", secondsSince(start_), line); }

    std::vector<lunar::net::CapturedDatagram> datagrams_;
    RecordedJoin join_;
    lunar::net::CaptureDecoder decoder_{lunar::net::DescribeOptions{.quiet = true}};
    std::shared_ptr<lunar::net::Replica> replica_;
    std::unique_ptr<LiveClient> live_;
    Clock::time_point start_ = Clock::now();
    std::size_t next_ = 0;
    std::uint64_t frames_ = 0;
    bool ended_ = false;
};

} // namespace

std::expected<std::unique_ptr<GameFeed>, std::string> replayLive(const std::string& prefix,
                                                                 const std::string& scriptPlatform)
{
    auto datagrams = lunar::net::readCapture(prefix + ".pcap");
    if (!datagrams)
        return std::unexpected(datagrams.error());
    // The session: the pair of addresses with the most datagrams between them.
    auto pairOf = [](const lunar::net::CapturedDatagram& d) {
        const std::string a = d.from.toString(), b = d.to.toString();
        return std::min(a, b) + "|" + std::max(a, b);
    };
    std::map<std::string, std::size_t> counts;
    for (const auto& d : *datagrams)
        ++counts[pairOf(d)];
    if (counts.empty())
        return std::unexpected(std::format("{}.pcap has no datagrams", prefix));
    const std::string session = std::ranges::max_element(counts, {}, [](const auto& c) { return c.second; })->first;
    std::erase_if(*datagrams, [&](const lunar::net::CapturedDatagram& d) { return pairOf(d) != session; });
    std::ranges::stable_sort(*datagrams, {}, &lunar::net::CapturedDatagram::time);
    return std::make_unique<ReplayFeed>(std::move(*datagrams), prefix,
                                        scriptPlatform.empty() ? std::string("Linux") : scriptPlatform);
}

// Who the recorded client was and where, from the masked join script
// --net-join --out saved next to the capture (NetTools.hpp).
RecordedJoin recordedJoin(const std::string& prefix)
{
    RecordedJoin join;
    if (auto text = lunar::readFile(prefix + ".joinscript.json")) {
        if (auto doc = lunar::assets::json::parse(*text)) {
            if (const auto* v = (*doc)["UserId"]; v && v->integer())
                join.userId = *v->integer();
            if (const auto* v = (*doc)["PlaceId"]; v && v->integer())
                join.placeId = static_cast<double>(*v->integer());
            if (const auto* v = (*doc)["GameId"]; v && v->string())
                join.jobId = *v->string();
        }
    }
    return join;
}

int runNetCommand(std::span<const std::string_view> args)
{
    if (args.empty())
        return -1;
    if (args[0] == "--net-decode")
        return netDecode(args);
    if (args[0] == "--net-loopback")
        return netLoopback(args);
    if (args[0] == "--net-server")
        return netServer(args);
    if (args[0] == "--net-client")
        return netClient(args);
    if (args[0] == "--net-replay")
        return netReplay(args);
    if (args[0] == "--net-join")
        return runNetJoin(args);
    if (args[0] == "--net-play")
        return runNetPlay(args);
    if (args[0] == "--net-challenge") {
        // research: --net-challenge <protected bytecode> <first arg> <second arg> [job id] [place id]
        // runs a saved 0x9b challenge the way --net-join answers it.
        if (args.size() < 4)
            return -1;
        auto blob = lunar::readFile(std::string(args[1]));
        if (!blob) {
            std::println(stderr, "{}", blob.error());
            return 1;
        }
        auto bytecode = lunar::net::decodeProtectedBytecode(*blob);
        if (!bytecode) {
            std::println(stderr, "{}", bytecode.error());
            return 1;
        }
        auto game = lunar::DataModel::create();
        game->assignJob(args.size() > 4 ? std::string(args[4]) : std::string(),
                        args.size() > 5 ? std::stod(std::string(args[5])) : 0.0);
        lunar::ScriptContext scripts(game, lunar::ScriptRole::Client);
        scripts.setTimeout(2.0);
        const std::array<double, 2> callArgs{std::stod(std::string(args[2])), std::stod(std::string(args[3]))};
        const char* chunk = std::getenv("LUNAR_NET_CHUNK");
        auto results = scripts.callBytecode(std::move(*bytecode), chunk ? chunk : "challenge", callArgs, true);
        if (!results) {
            std::println(stderr, "challenge error: {}", results.error().message);
            return 1;
        }
        for (const double r : *results)
            std::println("result {} (u32 {})", r, static_cast<std::uint32_t>(static_cast<std::int64_t>(r)));
        return 0;
    }
    return -1;
}
