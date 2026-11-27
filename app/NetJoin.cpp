#include "NetJoin.hpp"

#include "LiveChat.hpp"
#include "LiveClient.hpp"
#include "ToolActions.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Instance.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/net/Crypto.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Join.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Motion.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Peer.hpp>
#include <lunar/net/Remotes.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Rupp.hpp>
#include <lunar/net/Session.hpp>
#include <lunar/net/Socket.hpp>
#include <lunar/net/Streaming.hpp>
#include <lunar/net/StreamingClient.hpp>
#include <lunar/net/ToolReplication.hpp>
#include <lunar/platform/Display.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;
using lunar::net::Peer;
using lunar::net::Reliability;

double secondsSince(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

std::uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Vsync's pace for a client with no window: the fastest display's refresh
// rate (the one a game would be played on, GUESS), 60 without a display.
double vsyncRate()
{
    const std::vector<double> rates = lunar::platform::displayRefreshRates();
    std::string list;
    for (const double rate : rates)
        list += std::format("{}{:.2f} Hz", list.empty() ? "" : ", ", rate);
    if (!rates.empty())
        std::println("join: displays: {}", list);
    return rates.empty() ? 60.0 : std::ranges::max(rates);
}

// The join as it goes: the web step, then the session with the server.
struct JoinSession {
    lunar::net::JoinScript script;
    lunar::net::ClientPlatform platform;
    // --scripts: the client's LocalScripts over the replica, from when the
    // join data is in and our Player is there, stepped at scriptRate.
    bool runScripts = false;
    std::function<void(lunar::ClientSession&)> configureClient;
    std::string scriptPlatform; // what UserInputService:GetPlatform() tells the scripts
    double scriptsStartedAt = -1, scriptTime = 0;
    // A window's frames: what arrives is taken in this long a frame at most
    // once the game is going (update). Zero takes everything at once.
    std::chrono::microseconds messageBudget{0};
    double scriptRate = 60.0; // frames a second the scripts run at (vsync: the display's refresh rate)
    std::uint64_t framesStepped = 0;
    // --say: a chat message sent `sayAfter` seconds into the scripts, as the chat bar sends it.
    std::string say;
    std::string sayPreset; // --say-preset: a phrase of preset chat's catalog instead
    double sayAfter = 8;
    int sayTries = 0;
    const lunar::assets::RobloxWeb* web = nullptr;  // for the preset catalog
    double steppingSeconds = 0, networkSeconds = 0; // where the loop's time went
    // Our client: its scripts, their remotes both ways and the animations
    // (made with the first stepScripts).
    std::unique_ptr<LiveClient> live;
    ToolActions toolActions;                                // --equip and co.
    TimedClicks clicks = TimedClicks::fromEnvironment();    // research: LUNAR_NET_CLICK
    TimedPrompts prompts = TimedPrompts::fromEnvironment(); // research: LUNAR_NET_PROMPT
    // --walk: the controls our character gets, from when it is ours to move.
    std::optional<WalkPlan> walkPlan;
    double walkFrom = -1, walkReported = 0;
    std::shared_ptr<lunar::net::Replica> replica = std::make_shared<lunar::net::Replica>();
    std::shared_ptr<lunar::net::Replica> logReplica = std::make_shared<lunar::net::Replica>();
    // What the join request says beyond the join script (a teleport's
    // IsTeleportedIn and RawJoinData), and the teleport the server sent us
    // off on: the session stops and the owner joins where it says.
    lunar::net::ClientInfo clientInfo;
    std::optional<lunar::net::TeleportTicket> teleport;
    std::shared_ptr<lunar::net::RuppGenerator> rupp;
    std::unique_ptr<Peer> peer;
    int connection = -1;
    bool done = false; // kicked, disconnected or failed
    std::string outcome;
    Clock::time_point start = Clock::now();
    double connectedAt = -1, firstMessageAt = -1, schemaAt = -1, verifiedAt = -1;
    // When the replica last grew: a join still coming in isn't one that never finishes.
    double lastGrowthAt = -1;
    std::size_t lastInstanceCount = 0;

    // What came in.
    std::map<std::uint8_t, std::pair<int, std::size_t>> received; // id -> count, bytes
    std::size_t markersEchoed = 0, pingsAnswered = 0, dataItemsUndecoded = 0;
    std::vector<std::string> notes;
    std::optional<bool> placeVerified;
    std::string kickText, kickMetadata;

    // The names of the server's 0x93 flag pairs, which the client echoes in
    // the join request's u16 list; the request waits for them (up to 2 s).
    std::vector<std::string> syncedFlags;
    bool joinRequestSent = false;

    std::string variant; // research: one of the handshake variants of runNetJoin
    std::string variantNote;

    // Content streaming (StreamingEnabled games): the quota, the focus's
    // stream-around requests and streaming far regions out. --focus moves the
    // focus along a path, by session time (the character stays where it is).
    std::unique_ptr<lunar::net::StreamingClient> streaming;
    struct FocusStep {
        lunar::Vector3 at;
        double seconds = 0;
    };
    std::vector<FocusStep> focusPath;
    std::size_t focusStep = 0;
    double streamingSince = -1, streamingReportAt = 0;
    lunar::net::StreamingClient::Stats streamingSeen;
    std::vector<std::pair<std::uint64_t, std::int32_t>> scriptStreamArounds; // the scripts' request, ours

    void stepStreaming()
    {
        if (!streaming || connection < 0 || !replica->globals)
            return;
        const double now = secondsSince(start);
        while (focusStep < focusPath.size() && focusPath[focusStep].seconds <= now) {
            const lunar::Vector3 at = focusPath[focusStep++].at;
            streaming->setFocus(at);
            note(std::format("streaming: focus at ({}, {}, {}), region {}", at.x, at.y, at.z,
                             lunar::net::regionText(lunar::net::regionOf(at))));
        }
        for (auto& m : streaming->tick(*replica, nowMs()))
            peer->send(connection, std::move(m), Reliability::ReliableOrdered);
        // Scripts waiting in RequestStreamAroundAsync go on once the server answered.
        std::erase_if(scriptStreamArounds, [&](const auto& r) {
            if (!streaming->streamAroundDone(r.second) || !live || !live->session())
                return false;
            live->session()->client().streamAroundDone(r.first);
            return true;
        });
        if (!streaming->active())
            return;
        const auto& st = streaming->stats();
        if (streamingSince < 0) {
            streamingSince = now;
            note("streaming: the server streams (StreamingEnabled); quota and radius sent");
        }
        if (st.streamAroundSent != streamingSeen.streamAroundSent)
            note(std::format("streaming: {} stream-around request(s) sent", st.streamAroundSent));
        if (st.streamAroundDone != streamingSeen.streamAroundDone)
            note(std::format("streaming: {} stream-around request(s) answered", st.streamAroundDone));
        if (st.regionsRemoved != streamingSeen.regionsRemoved)
            note(std::format("streaming: streamed out {} region(s) so far, {} instances", st.regionsRemoved,
                             st.instancesRemoved));
        if (st.lastInfo && (!streamingSeen.lastInfo || !(*st.lastInfo == *streamingSeen.lastInfo)))
            note(std::format("streaming: Streaming Info: around {}, radius {} done",
                             lunar::net::regionText(st.lastInfo->region), st.lastInfo->radius));
        if (now >= streamingReportAt) {
            streamingReportAt = now + 5;
            note(std::format(
                "streaming: {} instances in {} Stream Data items; holding {} regions, {} parts; " "replica {} " "instan"
                                                                                                                "ces",
                st.instancesStreamed, st.batches, streaming->regionsHeld(), streaming->partsHeld(),
                replica->instanceCount()));
        }
        streamingSeen = st;
    }

    std::expected<void, std::string> start_(const lunar::net::SystemAddress& server, const lunar::net::ServerKeys& keys)
    {
        Peer::Options o;
        if (variant == "flags1" && rupp)
            rupp->flags = 1;
        if (variant == "traceid" && rupp) {
            std::random_device rd;
            std::array<std::uint8_t, 16> id{};
            for (auto& b : id)
                b = static_cast<std::uint8_t>(rd());
            rupp->traceId = id;
        }
        if (variant == "padfull")
            o.request2Padding = 1;
        if (variant == "padnone")
            o.request2Padding = 2;
        if (variant == "optin0")
            o.ruppOptIn = false;
        o.describer = lunar::net::robloxDescriber(logReplica);
        o.keyExchangeVersion = keys.keyExchangeVersion;
        o.serverEphemeralEarlyPublic = keys.ephemeralEarlyPublic;
        o.password = std::string(lunar::net::kServerConnectionPassword);
        if (keys.preAuth && std::getenv("LUNAR_NET_NO_PREAUTH") == nullptr) {
            o.preAuth = std::pair{keys.preAuth->kind, keys.preAuth->blob};
            o.preAuthToken = keys.preAuth->token;
        }
        if (const char* caps = std::getenv("LUNAR_NET_CAPS"); caps && *caps)
            o.capabilities = std::stoull(caps, nullptr, 16); // research: the capability set advertised
        if (const char* c = std::getenv("LUNAR_NET_CIPHER"); c && *c)
            o.cipher = static_cast<std::uint8_t>(std::atoi(c)); // research: force the cipher asked for
        if (rupp) {
            o.ruppHeader = [rupp = rupp] { return rupp->header(nowMs()); };
            o.ruppOnConnected = rupp->onConnected;
            o.ruppReplyLength = [rupp = rupp](std::span<const std::uint8_t> d) {
                const std::size_t n = lunar::net::ruppReplyLength(d);
                if (n > 4)
                    rupp->onReply(d.first(n));
                return n;
            };
        }
        peer = std::make_unique<Peer>(Peer::Role::Client, std::move(o));
        if (variant == "reverse" && rupp) {
            // Our public mapping for this socket, as a type-6 TLV.
            auto stun = lunar::net::resolve("stun.sipgate.net:3478");
            auto mapped = stun ? peer->stunMappedAddress(*stun) : std::nullopt;
            if (!mapped)
                return std::unexpected("no STUN answer, can't build the reverse endpoint");
            rupp->reverseEndpoint = *mapped;
            variantNote = "reverse endpoint " + mapped->toString();
        }
        return peer->connect(server);
    }

    void note(std::string text)
    {
        std::println("join: {:7.3f}s {}", secondsSince(start), text);
        notes.push_back(std::move(text));
    }

    // What the client sends on ID_CONNECTION_REQUEST_ACCEPTED (0x1054b8ef0):
    // the join request, the 0x92 verification value, [0x8a ticket], 0x8f.
    // research: LUNAR_NET_SEND="90,92" sends only the listed ones ("none" for
    // none of them), to find which one the server can't parse.
    void sendJoinRequest()
    {
        joinRequestSent = true;
        const char* only = std::getenv("LUNAR_NET_SEND");
        auto wanted = [&](std::string_view id) {
            return only == nullptr || std::string_view(only).find(id) != std::string_view::npos;
        };
        if (wanted("90")) {
            lunar::net::ClientInfo info = clientInfo;
            info.platform = platform;
            lunar::net::JoinRequest request = lunar::net::clientJoinRequest(script, info);
            request.extra = syncedFlags;
            if (std::getenv("LUNAR_NET_TINY90")) {
                // research: a 0x90 that fits one datagram (no split)
                request.extra.clear();
                request.joinInfo = "{}";
            }
            std::vector<std::uint8_t> joinRequest
                = lunar::net::encodeClientJoinRequest(request, lunar::net::kBaseCapabilities);
            // research: LUNAR_NET_NO_EXTRA=1 drops the (empty) u16-counted extra
            // list after the two Luau bytes, to test whether the server reads it.
            if (const char* e = std::getenv("LUNAR_NET_NO_EXTRA");
                e && *e == '1' && joinRequest.size() > 10 && joinRequest[8] == 0 && joinRequest[9] == 0) {
                joinRequest.erase(joinRequest.begin() + 8, joinRequest.begin() + 10);
                note("research: 0x90 sent without the extra list");
            }
            // research: LUNAR_NET_TRAILER=741 swaps our 7 trailing words for the
            // 6 the genuine 0.741 client sends.
            if (const char* t = std::getenv("LUNAR_NET_TRAILER"); t && std::string_view(t) == "741") {
                joinRequest = lunar::net::encodeJoinRequest(request, lunar::net::kBaseCapabilities);
                for (const std::uint32_t w :
                     {0x192979a7u, 0xefd33eb9u, 0x192979a7u, 0x13644eb9u, 0x18bd58ceu, 0xc3d1d9a9u})
                    for (int b = 0; b < 32; b += 8)
                        joinRequest.push_back(static_cast<std::uint8_t>(w >> b));
                note("research: 0x90 sent with the genuine 0.741 trailer");
            }
            if (const char* path = std::getenv("LUNAR_NET_DUMP90"); path && *path) {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(joinRequest.data()),
                        static_cast<std::streamsize>(joinRequest.size()));
            }
            note("0x90 head: " + lunar::net::hexString(std::span(joinRequest).first(16)));
            // The client seals the body with RakNet's block cipher; the server
            // drops a 0x90 it can't open with 260.
            if (std::getenv("LUNAR_NET_PLAIN90") == nullptr)
                joinRequest = lunar::net::sealMessageBody(lunar::net::kJoinRequestKey, joinRequest);
            peer->send(connection, std::move(joinRequest));
            note(
                std::format(
                    "sent the join request (0x90): flags {:#x}, value {:#x}, luau {}..{}, {} synced flags, " "{} bytes "
                                                                                                             "of " "joi"
                                                                                                                   "n "
                                                                                                                   "inf"
                                                                                                                   "o",
                    request.flags, request.clientValue, request.luauMin, request.luauMax, request.extra.size(),
                    request.joinInfo.size()));
        }
        if (wanted("92")) {
            std::random_device rd;
            peer->send(connection, lunar::net::encodeClientVerification(rd()));
            note("sent 0x92 (client verification value)");
        }
        if (wanted("8a")) {
            auto ticket = lunar::net::encodeTicketSubmission(script.userId, script.clientTicket, script.sessionId,
                                                             platform.name);
            // Sealed like 0x90, with a key made from the place id; the server
            // drops a plain one with 262.
            if (std::getenv("LUNAR_NET_PLAIN8A") == nullptr)
                ticket = lunar::net::sealMessageBody(lunar::net::ticketSubmissionKey(script.placeId), ticket);
            peer->send(connection, std::move(ticket));
            note("sent 0x8a (ticket)");
        }
        if (wanted("8f")) {
            peer->send(connection, lunar::net::encodePreferredSpawn(""));
            note("sent 0x8f (no preferred spawn)");
        }
    }

    // The bytecode challenge, answered the way the client does (0x1054c829a):
    // run the bytecode in the game's script context as a function of two
    // numbers and send back u8 0x9b, u32 (the first argument), u32 (the first
    // result). The first argument is b: a live server keeps the session on
    // the (b, a) answer and drops an (a, b) one with 268 (LUNAR_NET_9B_ORDER=ab
    // swaps them).
    void answerChallenge(const lunar::net::BytecodeChallenge& c)
    {
        auto bytecode = lunar::net::decodeProtectedBytecode(c.bytecode);
        if (!bytecode) {
            note("0x9b: " + bytecode.error());
            return;
        }
        // A context for this call only: one that stayed would run its own
        // ToolSystem over the replica, a server's, welding every tool in
        // every character, ours too (a second RightGrip went out [live]).
        lunar::ScriptContext scripts(replica->game(), lunar::ScriptRole::Client);
        scripts.setTimeout(2.0);
        const char* order = std::getenv("LUNAR_NET_9B_ORDER");
        const bool ab = order && std::string_view(order) == "ab";
        const std::array<double, 2> args{static_cast<double>(ab ? c.a : c.b), static_cast<double>(ab ? c.b : c.a)};
        auto results = scripts.callBytecode(std::move(*bytecode), "challenge", args, true);
        if (!results) {
            note("0x9b: the challenge failed: " + results.error().message);
            return;
        }
        if (results->empty() || std::isnan(results->front())) {
            note("0x9b: the challenge returned no number");
            return;
        }
        const auto result = static_cast<std::uint32_t>(static_cast<std::int64_t>(results->front()));
        peer->send(connection, lunar::net::encodeBytecodeResponse({static_cast<std::uint32_t>(args[0]), result}));
        note(std::format("0x9b: answered {} -> {} ({} order)", static_cast<std::uint32_t>(args[0]), result,
                         ab ? "a,b" : "b,a"));
    }

    // A Marker item back to the server (ID_DATA 0x83, item 0x04, u32, End).
    void echoMarker(std::uint32_t marker)
    {
        lunar::net::NetworkStream out;
        out.writeU8(lunar::net::msg::Data);
        out.writeU8(lunar::net::item::Marker);
        out.writeU32(marker);
        out.writeU8(lunar::net::item::End);
        peer->send(connection, std::move(out).take());
        ++markersEchoed;
    }

    void onMessage(const std::vector<std::uint8_t>& data)
    {
        if (data.empty())
            return;
        if (firstMessageAt < 0)
            firstMessageAt = secondsSince(start);
        const std::uint8_t id = data[0];
        auto& [count, bytes] = received[id];
        ++count;
        bytes += data.size();
        using namespace lunar::net;
        switch (id) {
        case 0x93: {
            // u8 id, u8 (skipped), u16 n, n x {u16-length key, u16-length value}
            // (ClientReplicator vfn_175 0x1054d8eb4).
            NetworkStream in(data);
            in.readU8();
            in.readU8();
            const std::uint16_t n = in.left() >= 2 ? in.readU16() : 0;
            for (std::uint16_t i = 0; i < n && !in.bad(); ++i) {
                const auto k = in.readBytes(in.readU16());
                in.readBytes(in.readU16());
                syncedFlags.emplace_back(k.begin(), k.end());
            }
            // The genuine 0.741 client also lists this one, registered on its own side.
            syncedFlags.emplace_back("IAS_InstancePointersHoldAcrossReplication3");
            note(std::format("0x93: {} synced flags{}", n, in.bad() ? " (truncated)" : ""));
            if (!joinRequestSent && connection >= 0)
                sendJoinRequest();
            return;
        }
        case lunar::net::msg::BytecodeChallenge: {
            auto c = lunar::net::decodeBytecodeChallenge(data);
            if (!c) {
                note("0x9b bytecode challenge did not decode");
                return;
            }
            note(std::format("0x9b bytecode challenge: a {}, b {}, {} bytes of bytecode, head {}", c->a, c->b,
                             c->bytecode.size(),
                             lunar::net::hexString(std::span(reinterpret_cast<const std::uint8_t*>(c->bytecode.data()),
                                                             std::min<std::size_t>(c->bytecode.size(), 16)))));
            if (const char* path = std::getenv("LUNAR_NET_DUMP9B"); path && *path) {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write(c->bytecode.data(), static_cast<std::streamsize>(c->bytecode.size()));
            }
            if (std::getenv("LUNAR_NET_NO_9B") == nullptr)
                answerChallenge(*c);
            return;
        }
        case kKickId: {
            NetworkStream in(data);
            in.readU8();
            kickText = in.readString();
            if (in.left() > 0)
                kickMetadata = in.readString();
            outcome
                = std::format("kicked: \"{}\"{}", kickText, kickMetadata.empty() ? "" : " metadata " + kickMetadata);
            note(outcome);
            done = true;
            return;
        }
        case msg::ProtocolMismatch:
            outcome = "ID_PROTOCOL_MISMATCH from the server";
            note(outcome + ": " + hexString(data));
            return;
        case msg::SecurityKeyMismatch:
            outcome = "ID_SECURITYKEY_MISMATCH from the server";
            note(outcome + ": " + hexString(data));
            return;
        case kPlaceIdVerifiedId:
            placeVerified = data.size() >= 2 && data[1] != 0;
            verifiedAt = secondsSince(start);
            note(std::format("place id verification: {}", *placeVerified ? "ok" : "FAILED"));
            return;
        case kMarkerId: {
            NetworkStream in(data);
            in.readU8();
            const std::uint32_t m = in.readU32();
            echoMarker(m);
            note(std::format("marker {} (ID_MARKER), echoed", m));
            return;
        }
        case kSchemaPacketId: {
            replica->handle(data);
            if (const auto* s = replica->schema()) {
                schemaAt = secondsSince(start);
                note(std::format("schema: {} enums, {} classes, {} properties, {} events ({} bytes on the wire)",
                                 s->enums.size(), s->classes.size(), s->properties.size(), s->events.size(),
                                 data.size()));
            } else {
                note(std::format("schema packet of {} bytes did not decode", data.size()));
            }
            return;
        }
        case msg::SetGlobals:
            replica->handle(data);
            note(std::format("ID_SET_GLOBALS, {} bytes{}", data.size(), replica->globals ? "" : " (did not decode)"));
            return;
        case msg::Timestamp: // physics (0x85), and ISR: the motion of what others own
        case msg::Isr:
            replica->handle(data);
            return;
        case msg::Data: {
            const std::size_t markersBefore = replica->markers.size();
            const std::size_t before = replica->instanceCount();
            replica->handle(data);
            for (std::size_t i = markersBefore; i < replica->markers.size(); ++i) {
                echoMarker(replica->markers[i]);
                note(std::format("marker {} (Marker item), echoed", replica->markers[i]));
            }
            if (replica->instanceCount() != before && (replica->instanceCount() / 100 != before / 100))
                note(std::format("{} instances so far", replica->instanceCount()));
            // The server's Ping items (0x05), answered with PingBacks (0x06) as the
            // client writes them (answerPing).
            if (!replica->runtime.pendingPings.empty() && replica->schema()) {
                std::vector<lunar::net::Item> backs;
                const std::uint64_t now = nowMs();
                for (const auto& p : replica->runtime.pendingPings)
                    backs.emplace_back(lunar::net::RuntimeItem{
                        lunar::net::answerPing(p, static_cast<std::uint32_t>(now), static_cast<std::uint32_t>(now))});
                pingsAnswered += replica->runtime.pendingPings.size();
                replica->runtime.pendingPings.clear();
                peer->send(connection,
                           lunar::net::encodeData(backs, *replica->schema(), replica->context(), replica->framing,
                                                  lunar::net::ItemContext{false, lunar::net::kBaseCapabilities}),
                           Reliability::Unreliable);
            }
            return;
        }
        default:
            return;
        }
    }

    // Our client from the first call, and its scripts once the join is in
    // and our Player is there, or when the join never finishes: ten seconds
    // after the schema with nothing new for twenty. A slow server that goes
    // quiet for a while mid-join isn't that [live: a game
    // lobby sent nothing for 12 s after the schema; started at 10 s, its
    // scripts found no ReplicatedStorage.Modules, never told the server they
    // had loaded, and no character came]. true once they run.
    bool startScripts()
    {
        if (!runScripts || !replica->game())
            return false;
        if (const std::size_t count = replica->instanceCount(); count != lastInstanceCount || lastGrowthAt < 0) {
            lastInstanceCount = count;
            lastGrowthAt = secondsSince(start);
        }
        if (!live)
            live = std::make_unique<LiveClient>(
                replica,
                LiveClient::Options{
                    .userId = script.userId,
                    .platform = scriptPlatform,
                    .note = [this](std::string line) { note(std::move(line)); },
                    .streamAround =
                        [this](std::uint64_t request, lunar::Vector3 at, double timeout) {
                            if (!streaming || !streaming->active()) {
                                live->session()->client().streamAroundDone(request); // everything is here
                                return;
                            }
                            scriptStreamArounds.emplace_back(request, streaming->requestStreamAround(at, timeout));
                            note(std::format("streaming: a script asks for the area around ({}, {}, {})", at.x, at.y,
                                             at.z));
                        },
                    .teleport =
                        [this](const lunar::RemoteValue& table) {
                            if (teleport)
                                return;
                            if (auto ticket = lunar::net::teleportTicket(table)) {
                                note(std::format("teleport: the server sends us to place {}{}", ticket->placeId,
                                                 ticket->accessCode().empty() ? "" : " (a reserved server)"));
                                teleport = std::move(*ticket);
                            } else {
                                note("teleport: a ticket without a place; staying");
                            }
                        },
                    .configureClient =
                        [this](lunar::ClientSession& clientSession) {
                            clientSession.client().setNetworkPing([this] {
                                const auto* reliability = peer ? peer->reliability(connection) : nullptr;
                                return reliability ? static_cast<double>(reliability->rttMs()) / 1000.0 : 0.0;
                            });
                            // A script's own teleport (TeleportService), as the server's are taken.
                            clientSession.client().setLaunchHandler(
                                [this](const lunar::ScriptContext::LaunchRequest& request) {
                                    if (teleport || request.placeId == 0)
                                        return;
                                    lunar::net::TeleportTicket ticket;
                                    ticket.placeId = request.placeId;
                                    ticket.instanceId = request.gameInstanceId;
                                    note(std::format("teleport: a script sends us to place {}{}", ticket.placeId,
                                                     ticket.instanceId.empty() ? "" : ", server " + ticket.instanceId));
                                    teleport = std::move(ticket);
                                });
                            if (configureClient)
                                configureClient(clientSession);
                        },
                });
        const bool neverFinishes
            = schemaAt >= 0 && secondsSince(start) - schemaAt > 10 && secondsSince(start) - lastGrowthAt > 20;
        if (!live->start(neverFinishes))
            return false;
        if (scriptsStartedAt < 0)
            scriptsStartedAt = secondsSince(start);
        return true;
    }

    // What our character did, what our tools did, then what the scripts sent through their remotes.
    void sendRemotes()
    {
        if (LiveCharacter* self = live->character())
            for (auto& out : self->take(nowMs()))
                peer->send(connection, std::move(out.message), out.reliability);
        if (auto* tools = live->tools())
            for (auto& message : tools->take())
                peer->send(connection, std::move(message), Reliability::ReliableOrdered);
        for (auto& out : live->remotes().take())
            peer->send(connection, std::move(out.message),
                       out.reliable ? Reliability::ReliableOrdered : Reliability::Unreliable);
    }

    void stepScripts()
    {
        if (!startScripts())
            return;
        // Catching up, but never for more than 20 ms at a time: the network
        // keeps its turn when frames cost more than the rate allows (the
        // scripts then fall behind instead, and run slower than asked).
        const double dt = 1.0 / scriptRate;
        const double until = secondsSince(start) + 0.02;
        const double stepFrom = secondsSince(start);
        while (scriptTime + dt <= secondsSince(start) - scriptsStartedAt && secondsSince(start) < until) {
            toolActions.step(*live->session(), scriptTime, [this](std::string line) { note(std::move(line)); });
            clicks.step(ourPlayer(*replica, script.userId), scriptTime,
                        [this](std::string line) { note(std::move(line)); });
            prompts.step(live->session(), scriptTime, [this](std::string line) { note(std::move(line)); });
            walkStep();
            live->step(dt);
            scriptTime += dt;
            ++framesStepped;
        }
        steppingSeconds += secondsSince(start) - stepFrom;
        sayIfDue();
        if (secondsSince(start) - scriptsStartedAt - scriptTime > 1.0)
            scriptTime = secondsSince(start) - scriptsStartedAt; // more than a second behind: drop it
        sendRemotes();
    }

    // --walk: the plan's controls for this frame, from when our character
    // is ours to move; where it got to every two seconds.
    void walkStep()
    {
        LiveCharacter* self = walkPlan ? live->character() : nullptr;
        if (!self)
            return;
        if (walkFrom < 0 && self->character()) {
            walkFrom = scriptTime;
            note(std::format("self: took over our character at {}, walking {}", self->describe(), walkPlan->text()));
        }
        const WalkPlan::Input in = walkPlan->at(walkFrom < 0 ? 0.0 : scriptTime - walkFrom);
        self->setMoveVector(walkFrom < 0 ? lunar::Vector3{} : in.move, false);
        self->setJump(walkFrom >= 0 && in.jump);
        if (walkFrom >= 0 && scriptTime - walkFrom >= walkReported) {
            note("self: " + self->describe());
            walkReported += 2.0;
        }
    }

    // --net-play: one client frame of dt seconds, the window's.
    void stepFrame(double dt)
    {
        if (!startScripts())
            return;
        const double stepFrom = secondsSince(start);
        live->step(dt);
        scriptTime += dt;
        ++framesStepped;
        steppingSeconds += secondsSince(start) - stepFrom;
        sendRemotes();
    }

    // --say, once the scripts have run a while: until the server has given
    // us a TextSource to speak with, a try a second, ten at most.
    void sayIfDue()
    {
        const double ran = secondsSince(start) - scriptsStartedAt;
        if ((say.empty() && sayPreset.empty()) || sayTries >= 10 || ran < sayAfter + sayTries)
            return;
        ++sayTries;
        if (!sayPreset.empty()) {
            sayPresetNow();
            return;
        }
        if (auto sent = live->session()->client().sendChat(say)) {
            note(std::format("chat: said \"{}\" (message id {})", say, *sent));
            say.clear();
        } else {
            note(std::format("chat: can't say it yet: {}", sent.error()));
        }
    }

    // --say-preset: the phrase from the catalog the server's config names,
    // once the server has said whether we may.
    void sayPresetNow()
    {
        const lunar::PresetChatAccess access = live->session()->client().presetChatAccess();
        if (access.access.empty()) {
            note("chat: the server hasn't said yet whether we may use preset chat");
            return;
        }
        auto said = sayPresetPhrase(live->session()->client(), *web, sayPreset);
        note("chat: " + (said ? *said : "can't say the preset: " + said.error()));
        sayPreset.clear();
    }

    void update(int waitMs)
    {
        if (connection >= 0 && !joinRequestSent && secondsSince(start) - connectedAt > 2) {
            note("no 0x93 within 2 s, sending the join request anyway");
            sendJoinRequest();
        }
        peer->update(waitMs);
        // Once the game is going, a window takes in what arrived for
        // messageBudget a frame at most and leaves the rest for the next
        // (frameMessageBudget). The peer has acknowledged it all already, so
        // the server sees no delay; the join is taken in as fast as it comes.
        const auto until = std::chrono::steady_clock::now() + messageBudget;
        while (messageBudget.count() == 0 || scriptsStartedAt < 0 || std::chrono::steady_clock::now() < until) {
            auto e = peer->poll();
            if (!e)
                break;
            if (e->kind == Peer::Event::Kind::Connected) {
                connection = e->connection;
                connectedAt = secondsSince(start);
                note("connected (ID_CONNECTION_REQUEST_ACCEPTED)");
                // The optional item fields follow what was negotiated: a CFrame
                // Ack without the step its bit 33 asks for gets us kicked (261).
                if (const std::uint64_t negotiated = peer->negotiated(connection)) {
                    replica->capabilities = negotiated;
                    logReplica->capabilities = negotiated;
                }
                if (std::getenv("LUNAR_NET_A7")) {
                    // research: an empty shared-string hash cache (u8 0xa7, u16 0),
                    // which the genuine client sends first (with its cache)
                    peer->send(connection, std::vector<std::uint8_t>{0xa7, 0x00, 0x00});
                    note("research: sent an empty 0xa7");
                }
                if (!syncedFlags.empty() || std::getenv("LUNAR_NET_NO_WAIT93"))
                    sendJoinRequest();
            } else if (e->kind == Peer::Event::Kind::Failed) {
                outcome = "connection failed: " + e->reason;
                note(outcome);
                done = true;
            } else if (e->kind == Peer::Event::Kind::Disconnected) {
                if (outcome.empty())
                    outcome = "disconnected: " + e->reason;
                note("disconnected: " + e->reason);
                done = true;
            } else if (e->kind == Peer::Event::Kind::Message) {
                onMessage(e->data);
            }
        }
        stepStreaming();
    }

    void summarize() const
    {
        std::println("join: summary");
        // Current servers send no 0x92 to the genuine client either and
        // spawn the character regardless, so "not sent" is the usual case.
        std::println("join:   handshake {}, first message {}, schema {}, place id verified {}",
                     connectedAt < 0 ? "never" : std::format("{:.3f}s", connectedAt),
                     firstMessageAt < 0 ? "never" : std::format("{:.3f}s", firstMessageAt),
                     schemaAt < 0 ? "never" : std::format("{:.3f}s", schemaAt),
                     verifiedAt < 0 ? "not sent" : std::format("{:.3f}s", verifiedAt));
        std::println("join:   outcome: {}", outcome.empty() ? "stayed until the end" : outcome);
        if (scriptsStartedAt >= 0) {
            const double ran = secondsSince(start) - scriptsStartedAt;
            std::println(
                "join:   scripts: {} frames in {:.1f} s ({:.0f} a second, {}); {:.1f} us a frame, " "{:.1f} s " "steppi"
                                                                                                                "ng, "
                                                                                                                "{:.1f}"
                                                                                                                " " "s "
                                                                                                                    "in"
                                                                                                                    " t"
                                                                                                                    "he"
                                                                                                                    " n"
                                                                                                                    "et"
                                                                                                                    "wo"
                                                                                                                    "r"
                                                                                                                    "k",
                framesStepped, ran, static_cast<double>(framesStepped) / std::max(ran, 1e-9),
                scriptRate > 0 ? std::format("asked {:.0f}", scriptRate) : "the window's frames",
                steppingSeconds / static_cast<double>(std::max<std::uint64_t>(framesStepped, 1)) * 1e6, steppingSeconds,
                networkSeconds);
        }
        std::size_t total = 0;
        for (const auto& [id, cb] : received)
            total += static_cast<std::size_t>(cb.first);
        std::println("join:   {} messages received:", total);
        for (const auto& [id, cb] : received)
            std::println("join:     {:#04x} {:<32} {:6} x {:9} bytes", id, lunar::net::messageName(id), cb.first,
                         cb.second);
        std::println("join:   {} markers echoed, {} pings answered", markersEchoed, pingsAnswered);
        if (const auto* s = replica->schema())
            std::println("join:   schema: {} enums, {} classes, {} properties, {} events", s->enums.size(),
                         s->classes.size(), s->properties.size(), s->events.size());
        std::println("join:   replica: {} instances", replica->instanceCount());
        std::println("join:   physics: {} samples for {} moving assemblies, {} for instances not streamed in",
                     replica->motion().received(), replica->motion().roots(), replica->motionForUnknown);
        if (replica->decodeFailures) {
            std::println("join:   {} ID_DATA messages did not decode", replica->decodeFailures);
            for (const auto& e : replica->decodeErrors)
                std::println("join:     {}", e.substr(0, 300));
        }
        std::map<std::string, int, std::less<>> byClass;
        std::function<void(const lunar::Instance&)> walk = [&](const lunar::Instance& i) {
            for (const auto& c : i.children()) {
                ++byClass[std::string(c->className())];
                walk(*c);
            }
        };
        // Everything replicated, by class, and what isn't under the game.
        std::map<std::string, int, std::less<>> allClasses;
        std::vector<std::string> loose;
        for (const auto& [id, inst] : replica->instances()) {
            if (!inst)
                continue;
            ++allClasses[std::string(inst->className())];
            if (!inst->parent() && loose.size() < 15)
                loose.push_back(std::format("{} ({})", inst->name(), inst->className()));
        }
        std::vector<std::pair<int, std::string>> top;
        for (const auto& [name, n] : allClasses)
            top.emplace_back(n, name);
        std::ranges::sort(top, std::greater<>());
        std::string all;
        for (std::size_t i = 0; i < top.size() && i < 25; ++i)
            all += std::format("{}{} {}", all.empty() ? "" : ", ", top[i].first, top[i].second);
        std::println("join:   all replicated classes: {}", all);
        for (const auto& l : loose)
            std::println("join:     unparented: {}", l);
        // research: which parents never arrived, and how many wait for each.
        if (std::getenv("LUNAR_NET_ORPHANS"))
            for (const auto& [parentKey, children] : replica->pendingParents()) {
                std::string names;
                int alive = 0;
                for (const auto& weak : children)
                    if (auto child = weak.lock(); child && !child->parent()) {
                        if (++alive <= 4)
                            names += std::format("{}{} ({})", names.empty() ? "" : ", ", child->name(),
                                                 child->className());
                    }
                if (alive)
                    std::println("join:     waiting for parent {:#x}: {} children: {}", parentKey, alive, names);
            }
        // Where the parts are, and what Workspace holds.
        std::map<std::string, int, std::less<>> partHomes;
        for (const auto& [id, inst] : replica->instances()) {
            if (!inst || !inst->isA("BasePart"))
                continue;
            std::string path;
            for (const lunar::Instance* p = inst->parent(); p; p = p->parent())
                path = std::string(p->name()) + (path.empty() ? "" : "/" + path);
            ++partHomes[path.empty() ? "<no parent>" : path];
        }
        for (const auto& [home, n] : partHomes)
            std::println("join:     {} parts under {}", n, home);
        // What the renderer can draw: Workspace's parts by class, the ones that
        // need a downloaded asset, and the terrain.
        if (replica->game())
            if (auto* ws = replica->game()->findFirstChildOfClass("Workspace")) {
                std::map<std::string, int, std::less<>> partClasses;
                int meshNoId = 0, invisible = 0;
                for (const auto& d : ws->descendants()) {
                    if (!d->isA("BasePart"))
                        continue;
                    ++partClasses[std::string(d->className())];
                    try {
                        if (d->isA("MeshPart") && std::get<std::string>(d->get("MeshId")).empty())
                            ++meshNoId;
                        if (std::get<double>(d->get("Transparency")) >= 1.0)
                            ++invisible;
                    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): a property this part lacks
                    }
                }
                std::string parts;
                for (const auto& [name, n] : partClasses)
                    parts += std::format("{}{} {}", parts.empty() ? "" : ", ", n, name);
                std::println("join:   Workspace parts: {}; {} MeshParts without MeshId, {} fully transparent", parts,
                             meshNoId, invisible);
                // research: LUNAR_NET_MODEL=<name> lists the parts of a Workspace child with how they draw.
                if (const char* model = std::getenv("LUNAR_NET_MODEL"); model && *model)
                    if (auto* m = ws->findFirstChild(model)) {
                        std::map<std::string, int> kinds;
                        for (const auto& d : m->descendants())
                            if (d->isA("BasePart")) {
                                std::string k = std::format("{} transp {} mat {} top {}", d->className(),
                                                            lunar::toString(d->get("Transparency")),
                                                            lunar::toString(d->get("Material")),
                                                            lunar::toString(d->get("TopSurface")));
                                ++kinds[k];
                            }
                        for (const auto& [k, n] : kinds)
                            std::println("join:   {} part: {} x {}", model, n, k);
                    }
                // research: LUNAR_NET_UNDER=1 lists the parts under the first character's feet.
                if (std::getenv("LUNAR_NET_UNDER"))
                    for (const auto& c : ws->children())
                        if (c->isA("Model") && c->findFirstChildOfClass("Humanoid"))
                            if (auto* root = c->findFirstChild("HumanoidRootPart")) {
                                const auto p = std::get<lunar::Vector3>(root->get("Position"));
                                std::println("join:   under {} at {}:", c->name(),
                                             lunar::toString(root->get("Position")));
                                int listed = 0;
                                for (const auto& d : ws->descendants()) {
                                    if (!d->isA("BasePart") || d->isDescendantOf(c.get()))
                                        continue;
                                    const auto q = std::get<lunar::Vector3>(d->get("Position"));
                                    const auto sz = std::get<lunar::Vector3>(d->get("Size"));
                                    if (std::abs(q.x - p.x) <= sz.x / 2 + 2 && std::abs(q.z - p.z) <= sz.z / 2 + 2
                                        && q.y < p.y && q.y > p.y - 40 && listed++ < 10)
                                        std::println("join:     {} ({}) at {}, size {}, transparency {}", d->fullName(),
                                                     d->className(), lunar::toString(d->get("Position")),
                                                     lunar::toString(d->get("Size")),
                                                     lunar::toString(d->get("Transparency")));
                                }
                                break;
                            }
                // research: LUNAR_NET_BIG=1 lists the largest parts (the sea, floors ...).
                if (std::getenv("LUNAR_NET_BIG")) {
                    std::vector<std::pair<double, const lunar::Instance*>> big;
                    for (const auto& d : ws->descendants())
                        if (d->isA("BasePart"))
                            try {
                                const auto sz = std::get<lunar::Vector3>(d->get("Size"));
                                big.emplace_back(double(sz.x) * double(sz.z), d.get());
                            } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
                            }
                    std::ranges::sort(big, std::greater<>{}, &std::pair<double, const lunar::Instance*>::first);
                    for (std::size_t i = 0; i < big.size() && i < 8; ++i)
                        std::println("join:   big part {} ({}): size {}, at {}, transparency {}, material {}",
                                     big[i].second->fullName(), big[i].second->className(),
                                     lunar::toString(big[i].second->get("Size")),
                                     lunar::toString(big[i].second->get("Position")),
                                     lunar::toString(big[i].second->get("Transparency")),
                                     lunar::toString(big[i].second->get("Material")));
                }
                // Characters: Workspace models with a Humanoid, and where their root part is.
                int shown = 0;
                for (const auto& c : ws->children()) {
                    if (shown == 4 || !c->isA("Model") || !c->findFirstChildOfClass("Humanoid"))
                        continue;
                    if (auto* root = c->findFirstChild("HumanoidRootPart")) {
                        std::println("join:   character {}: root at {}", c->name(),
                                     lunar::toString(root->get("Position")));
                        ++shown;
                        // research: LUNAR_NET_HEADS shows what each character's head is made of.
                        if (std::getenv("LUNAR_NET_HEADS")) {
                            if (auto* head = c->findFirstChild("Head")) {
                                std::string mesh;
                                for (const auto& h : head->children())
                                    mesh += std::format(" [{} {}]", h->className(), h->name());
                                if (auto* torso = c->findFirstChild("UpperTorso"))
                                    std::println("join:     head at {}, color {}, upper torso at {}",
                                                 lunar::toString(head->get("Position")),
                                                 lunar::toString(head->get("Color")),
                                                 lunar::toString(torso->get("Position")));
                                std::println(
                                    "join:     head {} size {} transp {} mesh '{}' texture '{}'{}", head->className(),
                                    lunar::toString(head->get("Size")), lunar::toString(head->get("Transparency")),
                                    head->isA("MeshPart") ? lunar::toString(head->get("MeshId")) : "",
                                    head->isA("MeshPart") ? lunar::toString(head->get("TextureID")) : "", mesh);
                            } else {
                                std::println("join:     no Head; children: {}", c->children().size());
                            }
                        }
                    }
                }
                if (auto* t = ws->findFirstChildOfClass("Terrain"))
                    std::println("join:   Terrain: {} chunks", static_cast<const lunar::Terrain*>(t)->chunks().size());
            }
        // research: LUNAR_NET_EVENT=<name> prints the schema's parameter types for events of that name.
        if (const char* ev = std::getenv("LUNAR_NET_EVENT"); ev && replica->schema()) {
            const auto& sc = *replica->schema();
            for (const auto& e : sc.events) {
                if (e.name != ev)
                    continue;
                std::string params;
                for (const auto& [type, enumId] : e.params)
                    params += std::format("{}{} ({:#04x})", params.empty() ? "" : ", ", lunar::net::netTypeName(type),
                                          type);
                std::println("join:   event {}.{}({})", e.owner < sc.classes.size() ? sc.classes[e.owner].name : "?",
                             e.name, params);
            }
        }
        // research: LUNAR_NET_PROPS="Name:Prop,Prop;Name2:Prop" prints properties of the first instance by that name.
        if (const char* spec = std::getenv("LUNAR_NET_PROPS"); spec && replica->game()) {
            std::string_view rest(spec);
            while (!rest.empty()) {
                const auto semi = rest.find(';');
                const std::string_view entry = rest.substr(0, semi);
                rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
                const auto colon = entry.find(':');
                const std::string name(entry.substr(0, colon));
                const lunar::Instance* found = nullptr;
                for (const auto& [id, inst] : replica->instances())
                    if (inst && inst->name() == name) {
                        found = inst.get();
                        break;
                    }
                if (!found) {
                    std::println("join:   {}: not found", name);
                    continue;
                }
                std::string_view props = colon == std::string_view::npos ? "" : entry.substr(colon + 1);
                while (!props.empty()) {
                    const auto comma = props.find(',');
                    const std::string prop(props.substr(0, comma));
                    props = comma == std::string_view::npos ? std::string_view{} : props.substr(comma + 1);
                    try {
                        std::println("join:   {}.{} = {}", name, prop, lunar::toString(found->get(prop)));
                    } catch (const std::exception& e) {
                        std::println("join:   {}.{}: {}", name, prop, e.what());
                    }
                }
            }
        }
        if (replica->game())
            if (auto* ws = replica->game()->findFirstChildOfClass("Workspace"))
                std::println("join:   Workspace.StreamingEnabled {}",
                             static_cast<const lunar::Workspace*>(ws)->streamingEnabled());
        if (replica->game())
            if (auto* ws = replica->game()->findService("Workspace"))
                for (const auto& c : ws->children())
                    std::println("join:     Workspace child: {} ({})", c->name(), c->className());
        if (replica->game()) {
            walk(*replica->game());
            for (const auto& child : replica->game()->children())
                std::println("join:     {} ({}): {} descendants", child->name(), child->className(),
                             child->descendants().size());
            if (auto* players = replica->game()->findService("Players"))
                for (const auto& p : players->children())
                    std::println("join:     player {} ({})", p->name(), p->className());
        }
        std::string classes;
        int shown = 0;
        std::vector<std::pair<int, std::string>> sorted;
        for (const auto& [name, n] : byClass)
            sorted.emplace_back(n, name);
        std::ranges::sort(sorted, std::greater<>());
        for (const auto& [n, name] : sorted) {
            if (shown++ == 12)
                break;
            classes += std::format("{}{} {}", classes.empty() ? "" : ", ", n, name);
        }
        if (!classes.empty())
            std::println("join:   classes: {}", classes);
        if (!kickText.empty())
            std::println("join:   kick: \"{}\" {}", kickText, kickMetadata);
        if (streaming && streaming->active()) {
            const auto& st = streaming->stats();
            std::
                println(
                    "join:   streaming: {} Stream Data items, {} instances; quota {} in {} updates; {} " "stream-"
                                                                                                         "around " "req"
                                                                                                                   "ues"
                                                                                                                   "ts,"
                                                                                                                   " {}"
                                                                                                                   " " "answered; " "streamed out {} " "regions, {} " "instances",
                    st.batches, st.instancesStreamed, st.quota, st.quotaUpdates, st.streamAroundSent,
                    st.streamAroundDone, st.regionsRemoved, st.instancesRemoved);
            const auto rows = streaming->describe(streaming->focus());
            std::println("join:   streaming: holding {} regions{}", rows.size(),
                         streaming->focus() ? " (nearest the focus first)" : "");
            for (std::size_t i = 0; i < rows.size() && i < 12; ++i)
                std::println("join:     {}", rows[i]);
        }
    }

    // The end of the session: what the remotes did, leaving the server if
    // we're still on it (saying why), the summary and the animations.
    void finish(std::string_view why)
    {
        if (live && live->tools())
            std::println("join: tools: {} items sent, {} dropped", live->tools()->sent, live->tools()->dropped);
        if (live) {
            std::println("join: remotes: {} sent, {} received, {} dropped", live->remotes().sent,
                         live->remotes().received, live->remotes().dropped);
            for (const auto& [what, n] : live->remotes().counts())
                std::println("join:   {} x{}", what, n);
        }
        if (!done && connection >= 0) {
            std::println("join: {}, leaving", why);
            peer->disconnect(connection);
        }
        if (connectedAt < 0 && outcome.empty())
            outcome = "the handshake never completed";
        summarize();
        if (live && live->animation())
            for (const std::string& line : live->animation()->describe())
                std::println("join: animation: {}", line);
        if (LiveCharacter* self = live ? live->character() : nullptr)
            std::println("join: self: {}{}", walkPlan ? std::format("walked {}: ", walkPlan->text()) : "",
                         self->describe());
    }
};

// The web side of a join: the join API (with retries), the signed player
// attributes and the server's address and keys from the join script.
struct WebJoin {
    lunar::net::JoinScript script;
    std::string jobId; // the API's, for a script that names no game
    lunar::net::ServerKeys keys;
    lunar::net::SystemAddress server;
};

struct WebJoinOptions {
    std::uint64_t placeId = 0;
    lunar::net::ClientPlatform platform;
    int attempts = 4;
    std::string out; // --out: the masked join script goes to <out>.joinscript.json
    std::string kx;  // research: "<version>:<early|ephemeral>" overrides the handshake key choice
    const lunar::net::TeleportTicket* teleport = nullptr; // a teleport's join (JoinOptions::teleport)
    std::string linkCode;                                 // a private server's (JoinOptions::linkCode)
    std::string gameId;                                   // one server's (JoinOptions::gameId)
    std::stop_token stop;                                 // cuts the pause between tries short, and the join with it
    // What the join waits on between tries, said for a loading screen.
    std::function<void(std::string)> progress;
};

// What a join that's tried again waits on, for the player: a server that's
// starting, a full one, a queue.
std::string joinWait(const lunar::net::JoinResponse& response, int attempt, int attempts)
{
    namespace status = lunar::net::joinstatus;
    const std::string tries = std::format(" (try {} of {})", attempt, attempts);
    switch (response.status) {
    case status::Waiting:
        return "Waiting for a server" + tries;
    case status::Loading:
        return "The server is starting" + tries;
    case status::GameFull:
        return "The server is full" + tries;
    case status::Queued:
        return (response.queuePosition ? std::format("In the queue, position {}", *response.queuePosition)
                                       : std::string("In the queue"))
               + tries;
    default:
        return "Trying again" + tries;
    }
}

// Errors say what went wrong; the caller says it on stderr.
std::expected<WebJoin, std::string> webJoin(lunar::assets::RobloxWeb& web, const WebJoinOptions& options)
{
    const std::uint64_t placeId = options.placeId;
    const int maxAttempts = options.attempts;
    const lunar::net::ClientPlatform& platform = options.platform;
    // The web join, with a pause between tries: the server may be starting
    // ("Waiting"), and the API rate-limits.
    std::optional<lunar::net::JoinResponse> response;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        lunar::net::JoinOptions jo;
        jo.placeId = placeId;
        jo.userAgent = platform.userAgent;
        jo.teleport = options.teleport;
        jo.linkCode = options.linkCode;
        jo.gameId = options.gameId;
        std::println("join: asking gamejoin.roblox.com for {}place {} (attempt {} of {})",
                     !options.linkCode.empty() ? "a private server of "
                     : !options.gameId.empty() ? "server " + options.gameId + " of "
                                               : "",
                     placeId, attempt, maxAttempts);
        auto r = lunar::net::requestJoin(web, jo);
        if (!r)
            return std::unexpected(r.error());
        std::println("join: status {} ({}){}{}", r->status, lunar::net::joinStatusName(r->status),
                     r->message.empty() ? "" : ": " + r->message,
                     r->queuePosition ? std::format(", queue position {}", *r->queuePosition) : "");
        if (r->status == lunar::net::joinstatus::Joining && r->joinScript) {
            response = std::move(*r);
            break;
        }
        if (!lunar::net::joinStatusRetryable(r->status))
            return std::unexpected(std::format("the API won't let us in ({})", lunar::net::joinStatusName(r->status)));
        if (attempt < maxAttempts) {
            // The client's own pacing, with a floor of 3 s to go easy on the API.
            const double pause = std::max(3.0, lunar::net::joinRetrySeconds(r->status, r->queuePosition, attempt));
            std::println("join: trying again in {:.0f} s", pause);
            if (options.progress)
                options.progress(joinWait(*r, attempt + 1, maxAttempts));
            std::mutex mutex;
            std::condition_variable_any never;
            std::unique_lock lock(mutex);
            never.wait_for(lock, options.stop,
                           std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(pause)),
                           [] { return false; });
            if (options.stop.stop_requested())
                return std::unexpected("given up");
        }
    }
    if (!response)
        return std::unexpected(std::format("no join script after {} attempts", maxAttempts));
    WebJoin join;
    join.jobId = response->jobId;
    join.script = std::move(*response->joinScript);
    lunar::net::JoinScript& script = join.script;
    // The signed player attributes the genuine client adds to the join info.
    if (auto attrs = lunar::net::requestPlayerAttributes(web, platform.userAgent)) {
        std::println("join: player attributes: age bracket {}, platform {}, os {}, original user {}", attrs->ageBracket,
                     attrs->platform, attrs->os, attrs->isOriginalUser);
        script.playerAttributes = std::move(*attrs);
    } else {
        std::println("join: no player attributes ({}); guessing them", attrs.error());
    }
    std::println("join: {}", lunar::net::describeJoinScript(script));
    if (!options.out.empty()) {
        // The join script for the research notes, secrets masked.
        std::ofstream f(options.out + ".joinscript.json", std::ios::binary | std::ios::trunc);
        f << lunar::net::maskJoinResponse(script.raw) << '\n';
    }
    auto keys = lunar::net::serverKeysOf(script);
    if (!keys)
        return std::unexpected(keys.error());
    if (const std::string& kx = options.kx; !kx.empty()) {
        const auto colon = kx.find(':');
        keys->keyExchangeVersion = static_cast<std::uint16_t>(std::stoi(kx.substr(0, colon)));
        if (colon != std::string::npos && kx.substr(colon + 1) == "early")
            keys->ephemeralEarlyPublic = keys->earlyPublic;
        std::println("join: research override: key exchange version {}, sealing with the {} key",
                     keys->keyExchangeVersion, colon != std::string::npos ? kx.substr(colon + 1) : "ephemeral");
    }
    join.keys = std::move(*keys);
    if (script.udmuxEndpoints.empty() && script.machineAddress.empty())
        return std::unexpected("the join script names no server");
    const lunar::net::JoinEndpoint endpoint = script.udmuxEndpoints.empty()
                                                  ? lunar::net::JoinEndpoint{script.machineAddress, script.serverPort}
                                                  : script.udmuxEndpoints.front();
    auto server = lunar::net::resolve(std::format("{}:{}", endpoint.address, endpoint.port));
    if (!server)
        return std::unexpected(server.error());
    join.server = *server;
    return join;
}

// Starts a session's handshake with the server of a web join; false when
// it can't (said on stderr).
bool openSession(JoinSession& session, const WebJoin& join, const lunar::net::ClientPlatform& platform,
                 const std::string& variant)
{
    const lunar::net::JoinScript& script = join.script;
    session.variant = variant;
    session.script = script;
    session.platform = platform;
    // game.JobId and game.PlaceId, as the client has them from the join
    // script; the 0x9b challenge adds up the JobId's bytes.
    session.replica->game()->assignJob(script.gameId.empty() ? join.jobId : script.gameId,
                                       static_cast<double>(script.placeId));
    session.replica->game()->assignUniverse(static_cast<double>(script.universeId));
    // Assets downloaded from here on are asked for as this place's.
    lunar::assets::AssetCache::setSessionPlaceId(static_cast<std::uint64_t>(script.placeId));
    session.rupp = lunar::net::RuppGenerator::fromJoinScript(script);
    if (session.rupp)
        std::println("join: rupp header: {}", session.rupp->describe());
    else
        std::println("join: no rupp header (the join script has no token)");
    if (!session.variant.empty())
        std::println("join: === variant {} ===", session.variant);
    if (auto ok = session.start_(join.server, join.keys); !ok) {
        std::println(stderr, "join: {}", ok.error());
        return false;
    }
    if (!session.variantNote.empty())
        std::println("join: {}", session.variantNote);
    std::println("join: connecting to {} (key exchange version {})", join.server.toString(),
                 join.keys.keyExchangeVersion);
    if (join.keys.preAuth)
        std::println("join: pre-auth from ClientTicket: kind {}, {} + {} bytes", join.keys.preAuth->kind,
                     join.keys.preAuth->blob.size(), join.keys.preAuth->token.size());
    else
        std::println("join: no pre-auth in ClientTicket ({})",
                     lunar::net::preAuthOfClientTicket(script.clientTicket).error());
    return true;
}

// A teleport's start: its log line, and hop n's capture and keys apart
// from the last server's (at <out>.hop<n>). Returns that prefix, empty
// without --out.
std::string beginHop(const lunar::net::TeleportTicket& ticket, const std::string& out, int hop)
{
    const std::string hopOut = out.empty() ? "" : std::format("{}.hop{}", out, hop);
    if (!hopOut.empty()) {
        setenv("LUNAR_NET_PCAP", (hopOut + ".pcap").c_str(), 1);
        setenv("LUNAR_NET_KEYLOG", (hopOut + ".keys").c_str(), 1);
    }
    std::println("join: === teleport {} to place {} ===", hop, ticket.placeId);
    return hopOut;
}

// A teleport's web join, as the genuine Mac client: gamejoin's teleport
// validation turns a Linux User-Agent away (status 12, "Teleport
// validation failed"), though the first join takes it [live: every hop
// with the Mac agent got in, every one with the Linux agent didn't]. The
// game server still hears we run on the session's platform. The ticket
// must outlive the join.
WebJoinOptions hopJoinOptions(const lunar::net::TeleportTicket& ticket, int attempts, const std::string& hopOut)
{
    return {.placeId = ticket.placeId,
            .platform = *lunar::net::clientPlatform("mac"),
            .attempts = attempts,
            .out = hopOut,
            .teleport = &ticket};
}

// The teleport's session with its new server: the handshake saying we
// were teleported in, with the ticket's RawJoinData. Null when it can't
// start (said on stderr).
std::unique_ptr<JoinSession> hopSession(const lunar::net::TeleportTicket& ticket, const WebJoin& join,
                                        const lunar::net::ClientPlatform& platform)
{
    auto session = std::make_unique<JoinSession>();
    session->clientInfo.teleportedIn = true;
    session->clientInfo.rawJoinData = lunar::net::teleportRawJoinData(ticket, static_cast<std::int64_t>(nowMs()));
    session->clientInfo.previousUniverseId = ticket.universeId;
    if (!openSession(*session, join, platform, ""))
        return nullptr;
    return session;
}

// A teleport's next session, all at once: the web join the ticket asks
// for, then its session. Null when it fails (said on stderr).
std::unique_ptr<JoinSession> hopTo(lunar::assets::RobloxWeb& web, const lunar::net::TeleportTicket& ticket,
                                   const lunar::net::ClientPlatform& platform, int attempts, const std::string& out,
                                   int hop)
{
    const auto next = webJoin(web, hopJoinOptions(ticket, attempts, beginHop(ticket, out, hop)));
    if (!next) {
        std::println(stderr, "join: {}", next.error());
        return nullptr;
    }
    return hopSession(ticket, *next, platform);
}

// --net-play's game: a session with the server, pumped and stepped by the
// window's frames. The web side of a join (the first, and each teleport's)
// runs on a thread of its own, so the window goes on drawing its loading
// screen while gamejoin finds us a server; the first session's replica is
// there from the start, empty until the server fills it.
class LiveJoin final : public GameFeed {
public:
    std::unique_ptr<JoinSession> session = std::make_unique<JoinSession>();
    // For the first join and teleports: where to ask, as what, and the
    // session's settings.
    lunar::assets::RobloxWeb web;
    lunar::net::ClientPlatform platform;
    int attempts = 4;
    std::string out;
    std::string scriptPlatform;

    // The web join for a place, or for the private server of a link (whose
    // place it resolves first), on the join thread; pump() starts the
    // session with the server once it's in.
    void startWebJoin(std::uint64_t placeId, std::string privateLink, std::string gameId)
    {
        startJoinThread(
            {.placeId = placeId, .platform = platform, .attempts = attempts, .out = out, .gameId = std::move(gameId)},
                        std::move(privateLink), std::nullopt, "Finding a server");
    }

    // Sets a session up for the window: its scripts stepped by the frames.
    void configure(JoinSession& s) const
    {
        s.configureClient = [this](lunar::ClientSession& clientSession) {
            if (configureClient)
                configureClient(clientSession);
        };
        s.runScripts = true;
        s.scriptRate = 0; // the window's frames step them
        s.streaming = std::make_unique<lunar::net::StreamingClient>(
            lunar::net::StreamingClientOptions{.userId = s.script.userId});
        s.scriptPlatform = scriptPlatform.empty() ? platform.name : scriptPlatform;
        s.messageBudget = frameMessageBudget;
    }

    void pump() override
    {
        if (joining_ && !takeWebJoin())
            return;
        if (session->teleport && !session->done && !hopTicket_)
            hop();
        if (hopTicket_ || session->done) // the server we're leaving has stopped
            return;
        const double from = secondsSince(session->start);
        session->update(0);
        session->networkSeconds += secondsSince(session->start) - from;
    }
    void service() override
    {
        // Through a teleport too: the server we're leaving still hears from
        // us (acks, pings) until the new one has taken us.
        if (!session->done && session->peer)
            session->peer->update(0);
    }
    void frame(double dt) override
    {
        if (!hopTicket_ && !session->done) // a game we're teleporting from has stopped
            session->stepFrame(dt);
    }
    const std::shared_ptr<lunar::net::Replica>& replica() const override { return session->replica; }
    LiveClient* client() override { return session->live && session->live->started() ? session->live.get() : nullptr; }
    std::int64_t userId() const override { return session->script.userId; }
    std::optional<std::string> ended() const override
    {
        if (!session->done || hopTicket_)
            return std::nullopt;
        return session->outcome.empty() ? std::string("disconnected") : session->outcome;
    }
    std::uint64_t generation() const override { return generation_; }
    void releasePrevious() override { previous_.reset(); }
    bool teleporting() const override { return hopTicket_ || (session->teleport && !session->done); }
    std::optional<std::string> joining() const override
    {
        if (!joining_)
            return std::nullopt;
        const std::scoped_lock lock(joinMutex_);
        return joinStatus_;
    }
    void leave() override
    {
        if (left_)
            return;
        left_ = true;
        if (joining_) {
            // Before there was a server to leave (or between servers): the
            // web join gives up.
            joinThread_.request_stop();
            joining_ = false;
            hopTicket_.reset();
            session->done = true;
            session->outcome = "the window closed";
            std::println("join: the window closed before the join was in");
            return;
        }
        if (session->peer) // not a web join that failed: there was no session
            session->finish("the window closed");
    }

private:
    // A web join on the join thread, saying `status` until it says what it
    // waits on: for a place, a private server's link (its place resolved
    // first), or a teleport's ticket. pump() takes the answer in.
    void startJoinThread(WebJoinOptions options, std::string privateLink,
                         std::optional<lunar::net::TeleportTicket> ticket, std::string status)
    {
        joining_ = true;
        joinStatus_ = std::move(status); // no join thread runs: the last one was joined
        options.progress = [this](std::string waiting) {
            const std::scoped_lock lock(joinMutex_);
            joinStatus_ = std::move(waiting);
        };
        joinThread_
            = std::jthread([this, joinWeb = web, options = std::move(options), privateLink = std::move(privateLink),
                            ticket = std::move(ticket)](const std::stop_token& stop) mutable {
                  const lunar::assets::HttpStopScope downloads(stop);
                  options.stop = stop;
                  if (ticket)
                      options.teleport = &*ticket; // the thread's own copy
                  auto joined = [&]() -> std::expected<WebJoin, std::string> {
                      if (!privateLink.empty()) {
                          auto link = lunar::net::resolveServerLink(joinWeb, privateLink);
                          if (!link)
                              return std::unexpected(link.error());
                          options.placeId = link->placeId;
                          options.linkCode = link->linkCode;
                          std::println("join: a private server of place {}", options.placeId);
                      }
                      return webJoin(joinWeb, options);
                  }();
                  const std::scoped_lock lock(joinMutex_);
                  webJoined_ = std::move(joined);
              });
    }

    // The web join's answer, once it's in: the session with its server
    // starts, or the game is over before it began (saying why). False while
    // it's still out.
    bool takeWebJoin()
    {
        std::optional<std::expected<WebJoin, std::string>> joined;
        {
            const std::scoped_lock lock(joinMutex_);
            joined = std::exchange(webJoined_, std::nullopt);
        }
        if (!joined)
            return false;
        joining_ = false;
        joinThread_.join();
        if (hopTicket_) {
            landHop(*std::exchange(hopTicket_, std::nullopt), *joined);
            return true;
        }
        if (!*joined) {
            std::println(stderr, "join: {}", joined->error());
            session->done = true;
            session->outcome = joined->error();
            return true;
        }
        // The session's clock runs from the handshake, as without a window.
        session->start = Clock::now();
        if (!openSession(*session, **joined, platform, "")) {
            session->done = true;
            session->outcome = "the handshake didn't start";
            return true;
        }
        configure(*session);
        return true;
    }

    // The server teleported us: join where the ticket says on the join
    // thread (landHop takes it from there), and leave once that's in. The
    // genuine client stays connected meanwhile, and gamejoin turns a reserved
    // server's teleport away (status 12, "Teleport validation failed") when
    // the player has already left the server that sent it [live: Ball Tower
    // Defense's matches]. The game we're leaving stops; the window goes on
    // under its loading screen.
    void hop()
    {
        const lunar::net::TeleportTicket ticket = *session->teleport;
        if (hops_ >= 3) {
            session->finish(std::format("teleporting to place {}", ticket.placeId));
            session->done = true;
            session->outcome = std::format("teleporting to place {}, a fourth hop", ticket.placeId);
            return;
        }
        hopTicket_ = ticket;
        const std::string hopOut = beginHop(*hopTicket_, out, ++hops_);
        startJoinThread(hopJoinOptions(*hopTicket_, attempts, hopOut), "", ticket, "Teleporting");
    }

    // A teleport's web join is in: the new server's session takes over, and
    // the window moves over to its game (generation). When it failed, the
    // game we left is over, saying so.
    void landHop(const lunar::net::TeleportTicket& ticket, const std::expected<WebJoin, std::string>& joined)
    {
        session->finish(std::format("teleporting to place {}", ticket.placeId));
        if (!joined)
            std::println(stderr, "join: {}", joined.error());
        auto next = joined ? hopSession(ticket, *joined, platform) : nullptr;
        if (!next) {
            session->done = true;
            session->outcome
                = std::format("the teleport to place {} failed{}", ticket.placeId, joined ? "" : ": " + joined.error());
            return;
        }
        configure(*next);
        // The last session (finished) stays until the window has let go of
        // its game: the chat window and the scene still hold its client.
        previous_ = std::exchange(session, std::move(next));
        ++generation_;
    }

    std::unique_ptr<JoinSession> previous_;
    bool left_ = false;
    int hops_ = 0;
    std::uint64_t generation_ = 0;
    // A join's web side (the first's, or a teleport's): on until pump()
    // takes its answer in.
    bool joining_ = false;
    std::optional<lunar::net::TeleportTicket> hopTicket_; // the teleport underway
    mutable std::mutex joinMutex_;                        // the join thread's status and answer
    std::string joinStatus_;
    std::optional<std::expected<WebJoin, std::string>> webJoined_;
    std::jthread joinThread_; // last: gone (stopped and joined) before the rest
};

} // namespace

int runNetJoin(std::span<const std::string_view> args)
{
    if (args.size() < 2)
        return -1;
    std::uint64_t placeId = 0;
    std::string privateLink; // --private <link> in place of the place id
    std::size_t first = 2;
    if (args[1] == "--private" && args.size() >= 3) {
        privateLink = args[2];
        first = 3;
    } else {
        try {
            placeId = std::stoull(std::string(args[1]));
        } catch (...) {
            return -1;
        }
    }
    double seconds = 20;
    int maxAttempts = 4;
    std::string out, kx, variants, renderPath;
    lunar::net::ClientPlatform platform = *lunar::net::clientPlatform("linux");
    bool runScripts = false;
    std::string scriptPlatform;
    std::optional<WalkPlan> walk;
    double fps = 0;        // --fps: frames a second the scripts run at; 0 for vsync
    std::string say;       // --say: a chat message to send
    std::string sayPreset; // --say-preset: a preset chat phrase to send
    double sayAfter = 8;   // --say-after: seconds into the scripts
    lunar::net::StreamingClientOptions streamingOptions;
    std::vector<std::pair<lunar::Vector3, double>> focusPath; // --focus x,y,z[@seconds]
    lunar::render::GraphicsSettings graphics;                 // for --render
    std::string graphicsError;
    ToolActions toolActions; // --equip, --activate, --unequip, --tool-at
    for (std::size_t i = first; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size())
            seconds = std::stod(std::string(args[++i]));
        else if (args[i] == "--attempts" && i + 1 < args.size())
            maxAttempts = std::stoi(std::string(args[++i]));
        else if (args[i] == "--out" && i + 1 < args.size())
            out = args[++i];
        else if (args[i] == "--render" && i + 1 < args.size())
            renderPath = args[++i]; // renders the replica when the session ends
        else if (args[i] == "--variants" && i + 1 < args.size())
            variants = args[++i]; // research: comma-separated handshake variants, each its own session
        else if (parseGraphicsFlag(args, i, graphics, graphicsError)) {
            if (!graphicsError.empty()) {
                std::println(stderr, "join: {}", graphicsError);
                return 1;
            }
        } else if (toolActions.parse(args, i, graphicsError)) {
            if (!graphicsError.empty()) {
                std::println(stderr, "join: {}", graphicsError);
                return 1;
            }
        } else if (args[i] == "--scripts")
            runScripts = true; // runs the game's LocalScripts as the client would
        else if (args[i] == "--fps" && i + 1 < args.size()) {
            const std::string_view value = args[++i];
            fps = value == "vsync" ? 0.0 : std::stod(std::string(value));
            if (fps < 0)
                return -1;
        } else if (args[i] == "--focus" && i + 1 < args.size()) {
            // x,y,z[@seconds]: the streaming focus from then on (repeat for a path)
            const std::string spec(args[++i]);
            const auto atSign = spec.find('@');
            lunar::Vector3 at;
            if (std::sscanf(spec.substr(0, atSign).c_str(), "%f,%f,%f", &at.x, &at.y, &at.z) != 3)
                return -1;
            focusPath.emplace_back(at, atSign == std::string::npos ? 0.0 : std::stod(spec.substr(atSign + 1)));
        } else if (args[i] == "--stream-out" && i + 1 < args.size())
            streamingOptions.streamOutStuds = std::stof(std::string(args[++i])); // studs; 0 never, -1 the place's
        else if (args[i] == "--stream-radius" && i + 1 < args.size())
            // research: the farthest region radius asked for (default unlimited)
            streamingOptions.maxRegionRadius = static_cast<std::int16_t>(std::stoi(std::string(args[++i])));
        else if (args[i] == "--walk" && i + 1 < args.size()) {
            walk = WalkPlan::parse(args[++i]); // our character walks this (LiveCharacter); runs the scripts
            if (!walk)
                return -1;
            runScripts = true;
        } else if (args[i] == "--script-platform" && i + 1 < args.size())
            scriptPlatform = args[++i]; // an Enum.Platform name for GetPlatform(); the join itself is unchanged
        else if (args[i] == "--say" && i + 1 < args.size())
            say = args[++i]; // needs --scripts: TextChatService runs in the client's context
        else if (args[i] == "--say-preset" && i + 1 < args.size())
            sayPreset = args[++i]; // a phrase of the preset catalog ("Good luck"), for accounts without chat
        else if (args[i] == "--say-after" && i + 1 < args.size())
            sayAfter = std::stod(std::string(args[++i]));
        else if (args[i] == "--platform" && i + 1 < args.size()) {
            // linux (what we run on) or mac (the genuine build's values, to diff against its captures)
            auto p = lunar::net::clientPlatform(args[++i]);
            if (!p)
                return -1;
            platform = std::move(*p);
        } else if (args[i] == "--kx" && i + 1 < args.size())
            kx = args[++i]; // research: "<version>:<early|ephemeral>" overrides the handshake key choice
        else
            return -1;
    }
    if (!out.empty()) {
        // The capture and keylog of this run, unless the environment names others.
        setenv("LUNAR_NET_PCAP", (out + ".pcap").c_str(), 0);
        setenv("LUNAR_NET_KEYLOG", (out + ".keys").c_str(), 0);
    }

    lunar::assets::RobloxWeb web;
    if (!web.hasCookie()) {
        std::println(stderr, "join: {}", web.noLoginHint());
        return 1;
    }

    std::println("join: saying we run on {} ({})", platform.displayName, platform.name);
    const double scriptRate = fps > 0 ? fps : vsyncRate();
    if (runScripts)
        std::println("join: the scripts run at {:.0f} frames a second{}", scriptRate, fps > 0 ? "" : " (vsync)");

    std::string linkCode;
    if (!privateLink.empty()) {
        auto link = lunar::net::resolveServerLink(web, privateLink);
        if (!link) {
            std::println(stderr, "join: {}", link.error());
            return 1;
        }
        placeId = link->placeId;
        linkCode = link->linkCode;
        std::println("join: a private server of place {}", placeId);
    }
    const auto join = webJoin(web, {.placeId = placeId,
                                    .platform = platform,
                                    .attempts = maxAttempts,
                                    .out = out,
                                    .kx = kx,
                                    .linkCode = linkCode});
    if (!join) {
        std::println(stderr, "join: {}", join.error());
        return 1;
    }

    // One web join, then one handshake per variant (the research runs), or
    // the plain session.
    std::vector<std::string> runs{""};
    if (!variants.empty()) {
        runs.clear();
        std::size_t p = 0;
        while (p <= variants.size()) {
            const std::size_t c = variants.find(',', p);
            runs.push_back(variants.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (c == std::string::npos)
                break;
            p = c + 1;
        }
    }
    // A session's settings, the same on every server it hops to.
    auto configure = [&](JoinSession& session) {
        session.runScripts = runScripts;
        session.walkPlan = walk;
        session.toolActions = toolActions;
        session.scriptRate = scriptRate;
        lunar::net::StreamingClientOptions so = streamingOptions;
        so.userId = session.script.userId;
        session.streaming = std::make_unique<lunar::net::StreamingClient>(so);
        for (const auto& [at, when] : focusPath)
            session.focusPath.push_back({at, when});
        std::ranges::stable_sort(session.focusPath, {}, &JoinSession::FocusStep::seconds);
        session.scriptPlatform = scriptPlatform.empty() ? platform.name : scriptPlatform;
        session.say = say;
        session.sayPreset = sayPreset;
        session.web = &web;
        session.sayAfter = sayAfter;
    };
    int code = 1;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        auto session = std::make_unique<JoinSession>();
        if (!openSession(*session, *join, platform, runs[i]))
            continue;
        configure(*session);
        std::int64_t userId = join->script.userId;
        double remaining = seconds;
        // Teleports: leave, join where the ticket says, go on there (a few
        // hops at most, and not in the research variants).
        for (int hops = 0;; ++hops) {
            while (secondsSince(session->start) < remaining && !session->done && !session->teleport) {
                const double from = secondsSince(session->start);
                session->update(5);
                session->networkSeconds += secondsSince(session->start) - from;
                session->stepScripts();
            }
            if (!session->teleport || !runs[i].empty() || hops >= 3)
                break;
            const lunar::net::TeleportTicket ticket = *session->teleport;
            remaining -= secondsSince(session->start);
            session->finish(std::format("teleporting to place {}", ticket.placeId));
            auto hop = hopTo(web, ticket, platform, maxAttempts, out, hops + 1);
            if (!hop)
                break;
            configure(*hop);
            userId = hop->script.userId;
            session = std::move(hop);
        }
        session->finish("time is up");
        if (!renderPath.empty() && session->replica->game()) {
            std::println("join: rendering the replica to {}", renderPath);
            renderReplica(session->replica->game(), renderPath, userId, graphics);
        }
        code = session->outcome.empty() || session->placeVerified.value_or(false) ? 0 : 1;
        if (session->connectedAt >= 0)
            break; // a variant that connects is the answer
        if (i + 1 < runs.size())
            std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    return code;
}

std::expected<std::unique_ptr<GameFeed>, std::string> joinLive(const LiveJoinOptions& options)
{
    auto platform = lunar::net::clientPlatform(options.platform);
    if (!platform)
        return std::unexpected(std::format("no client platform {}", options.platform));
    if (!options.out.empty()) {
        // The capture and keylog of this run, unless the environment names others.
        setenv("LUNAR_NET_PCAP", (options.out + ".pcap").c_str(), 0);
        setenv("LUNAR_NET_KEYLOG", (options.out + ".keys").c_str(), 0);
    }
    auto live = std::make_unique<LiveJoin>();
    if (!live->web.hasCookie())
        return std::unexpected(live->web.noLoginHint());
    std::println("join: saying we run on {} ({})", platform->displayName, platform->name);
    live->platform = *platform;
    live->attempts = options.attempts;
    live->out = options.out;
    live->scriptPlatform = options.scriptPlatform;
    live->startWebJoin(options.placeId, options.privateLink, options.gameId);
    return live;
}
