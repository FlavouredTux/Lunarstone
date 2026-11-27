#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Session.hpp>
#include <lunar/net/Streaming.hpp>
#include <lunar/net/StreamingClient.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

InstancePtr make(std::string_view className, std::string_view name, Instance* parent)
{
    auto instance = ClassRegistry::find(className)->factory();
    instance->setName(std::string(name));
    if (parent)
        instance->setParent(parent->shared_from_this());
    return instance;
}

InstancePtr part(std::string_view name, Vector3 at, Instance* parent)
{
    auto p = make("Part", name, parent);
    dynamic_cast<BasePart&>(*p).setPosition(at);
    return p;
}

// The runtime items of the client's messages, as the server reads them.
std::vector<RuntimeItem> serverReads(ReplicationServer& server, const std::vector<std::vector<std::uint8_t>>& messages)
{
    std::vector<RuntimeItem> out;
    for (const auto& m : messages) {
        auto items = decodeData(m, server.schema(), server.context(), nullptr, {}, ItemContext{false, kBaseCapabilities});
        CHECK(items);
        if (!items)
            continue;
        for (const Item& item : *items)
            if (const auto* r = std::get_if<RuntimeItem>(&item))
                out.push_back(*r);
    }
    return out;
}

template <class T>
std::vector<T> itemsOf(const std::vector<RuntimeItem>& items)
{
    std::vector<T> out;
    for (const RuntimeItem& r : items)
        if (const auto* t = std::get_if<T>(&r.body))
            out.push_back(*t);
    return out;
}

} // namespace

// Regions are 64-stud cubes, floored; Stream Data lists step from the region before.
TEST(net_streaming_regions)
{
    CHECK(regionOf({769.7f, -0.4f, -45.3f}) == RegionId{12, -1, -1}); // Flex's spawn [live]
    CHECK(regionOf({0, 0, 0}) == RegionId{0, 0, 0});
    CHECK(regionOf({-0.01f, 64.0f, 63.99f}) == RegionId{-1, 1, 0});
    const Vector3 c = regionCenter({1, -1, 0});
    CHECK(c.x == 96.0f && c.y == -32.0f && c.z == 32.0f);
    const RegionId previous{11, -2, -2};
    CHECK(resolveRegion({3, {}}, previous) == RegionId{12, -2, -2});
    CHECK(resolveRegion({1, {}}, previous) == RegionId{11, -1, -2});
    CHECK(resolveRegion({2, {}}, previous) == RegionId{11, -2, -1});
    CHECK(resolveRegion({4, {}}, previous) == kNoRegion);
    CHECK(resolveRegion({0, {5, 6, 7}}, previous) == RegionId{5, 6, 7});
}

// The quota goes up in changes, the radius is unlimited, and the focus is
// asked for with stream-around requests the server answers.
TEST(net_streaming_client_quota_and_stream_around)
{
    auto game = DataModel::create();
    auto me = make("Player", "Tester", game->getService("Players"));
    dynamic_cast<Player&>(*me).setUserId(42);
    ReplicationServer server(game, {"Workspace", "Players"});
    Replica replica;
    for (auto& m : server.joinMessages())
        replica.handle(m);
    StreamingClientOptions options;
    options.userId = 42;
    options.streamOutStuds = 0;
    StreamingClient streaming(options);
    // Nothing streams yet: nothing to say.
    CHECK(streaming.tick(replica, 1000).empty());
    CHECK(!streaming.active());
    // The first Stream Data starts it: the initial quota, from 0, no radius limit.
    auto near = part("Near", {10, 5, 10}, game->getService("Workspace"));
    replica.handle(server.streamData(std::vector{near}, {StreamRegionRef{0, regionOf({10, 5, 10})}}));
    auto items = serverReads(server, streaming.tick(replica, 1100));
    auto quota = itemsOf<ClientCapacityUpdateItem>(items);
    CHECK_EQ(quota.size(), std::size_t{1});
    CHECK(!quota.empty() && quota[0].quotaDelta == 2000 && quota[0].maxRegionRadius == kUnlimitedRegionRadius
          && quota[0].memoryLimited == 0);
    CHECK(streaming.active());
    // Within 0.125 s nothing more; then the rest up to the steady quota, as a change.
    CHECK(streaming.tick(replica, 1150).empty());
    quota = itemsOf<ClientCapacityUpdateItem>(serverReads(server, streaming.tick(replica, 1300)));
    CHECK(quota.size() == 1 && quota[0].quotaDelta == 8000);
    CHECK_EQ(streaming.stats().quota, 10000);
    CHECK(streaming.tick(replica, 1500).empty()); // unchanged: nothing
    // A focus far away: a stream-around request (Player:RequestStreamAroundAsync's item).
    streaming.setFocus(Vector3{2000, 10, 0});
    auto requests = itemsOf<StreamPrefetchRequestItem>(serverReads(server, streaming.tick(replica, 1600)));
    CHECK_EQ(requests.size(), std::size_t{1});
    CHECK(!requests.empty() && requests[0].id == 1 && requests[0].position.x == 2000.0f
          && requests[0].timeout == options.streamAroundTimeout);
    CHECK(!streaming.streamAroundDone(1));
    // The server's answer names the request.
    const std::array<Item, 1> answer{RuntimeItem{StreamPrefetchInfoItem{regionOf({2000, 10, 0}), 2, 1}}};
    replica.handle(encodeData(answer, server.schema(), server.context()));
    CHECK(streaming.tick(replica, 1700).empty());
    CHECK(streaming.streamAroundDone(1));
    CHECK_EQ(streaming.stats().streamAroundDone, std::size_t{1});
    // Moving a little asks nothing new; moving a region away asks again.
    streaming.setFocus(Vector3{2010, 10, 0});
    CHECK(itemsOf<StreamPrefetchRequestItem>(serverReads(server, streaming.tick(replica, 1800))).empty());
    streaming.setFocus(Vector3{2100, 10, 0});
    requests = itemsOf<StreamPrefetchRequestItem>(serverReads(server, streaming.tick(replica, 1900)));
    CHECK(requests.size() == 1 && requests[0].id == 2);
    // Scripts' own requests travel the same way.
    const std::int32_t id = streaming.requestStreamAround({-500, 0, 0}, 5);
    requests = itemsOf<StreamPrefetchRequestItem>(serverReads(server, streaming.tick(replica, 2000)));
    CHECK(requests.size() == 1 && requests[0].id == id && requests[0].timeout == 5.0);
}

// Far regions leave the replica and the server hears which instances went;
// the character, persistent models and what the focus keeps stay.
TEST(net_streaming_client_streams_out_far_regions)
{
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    auto me = make("Player", "Tester", game->getService("Players"));
    dynamic_cast<Player&>(*me).setUserId(42);
    auto character = make("Model", "Tester", workspace);
    auto root = part("HumanoidRootPart", {0, 5, 0}, character.get());
    dynamic_cast<Player&>(*me).setCharacter(dynamic_cast<Model*>(character.get()));
    // A stale Player of ours with no character, listed after the live one [live]: the character is still found.
    auto stale = make("Player", "Tester", game->getService("Players"));
    dynamic_cast<Player&>(*stale).setUserId(42);
    ReplicationServer server(game, {"Workspace", "Players"});
    Replica replica;
    for (auto& m : server.joinMessages())
        replica.handle(m);
    // Streamed after the join: a near part, a far part with a decal, a far
    // part in a persistent model, a far part in an ordinary model.
    auto a = part("A", {10, 5, 10}, workspace);
    auto b = part("B", {3000, 5, 0}, workspace);
    auto decal = make("Decal", "Face", b.get());
    auto persistent = make("Model", "Kept", workspace);
    dynamic_cast<Model&>(*persistent).setModelStreamingMode(Enum::ModelStreamingMode::Persistent);
    auto c = part("C", {3000, 5, 200}, persistent.get());
    auto model = make("Model", "M", workspace);
    auto d = part("D", {-3000, 5, 0}, model.get());
    // A plate far bigger than its region, centered far away but reaching under us: it stays.
    auto plate = part("Plate", {1200, -10, 0}, workspace);
    dynamic_cast<BasePart&>(*plate).setSize({2400, 4, 100});
    replica.handle(server.streamData(std::vector{a}, {StreamRegionRef{0, regionOf({10, 5, 10})}}));
    replica.handle(server.streamData(std::vector{b, decal}, {StreamRegionRef{0, regionOf({3000, 5, 0})}}));
    replica.handle(server.streamData(std::vector{persistent, c}, {StreamRegionRef{0, regionOf({3000, 5, 200})}}));
    replica.handle(server.streamData(std::vector{model, d}, {StreamRegionRef{0, regionOf({-3000, 5, 0})}}));
    replica.handle(server.streamData(std::vector{plate}, {StreamRegionRef{0, regionOf({1200, -10, 0})}}));
    const InstanceId idPlate = server.idOf(plate.get());
    const InstanceId idA = server.idOf(a.get()), idB = server.idOf(b.get()), idDecal = server.idOf(decal.get()),
                     idC = server.idOf(c.get()), idD = server.idOf(d.get()), idM = server.idOf(model.get()),
                     idRoot = server.idOf(root.get());
    CHECK(replica.find(idB) && replica.find(idDecal) && replica.find(idD) && replica.find(idRoot));
    StreamingClientOptions options;
    options.userId = 42;
    options.streamOutStuds = 512;
    StreamingClient streaming(options);
    const auto messages = streaming.tick(replica, 1000);
    const auto items = serverReads(server, messages);
    auto removals = itemsOf<RegionRemovalItem>(items);
    CHECK_EQ(removals.size(), std::size_t{2});
    std::ranges::sort(removals, {}, &RegionRemovalItem::sequence);
    CHECK(removals.size() == 2 && removals[0].sequence == 1 && removals[1].sequence == 2);
    bool sawB = false, sawD = false;
    for (const RegionRemovalItem& r : removals) {
        if (r.region == regionOf({3000, 5, 0})) {
            sawB = true;
            CHECK(r.removed == (std::vector<InstanceId>{idB, idDecal}));
        } else if (r.region == regionOf({-3000, 5, 0})) {
            sawD = true;
            CHECK(r.removed == std::vector<InstanceId>{idD});
        }
    }
    CHECK(sawB && sawD);
    CHECK(!replica.find(idB) && !replica.find(idDecal) && !replica.find(idD));
    CHECK(replica.find(idA) && replica.find(idC) && replica.find(idRoot) && replica.find(idPlate));
    CHECK(replica.find(idM)); // the model stays (the server still counts it as ours), empty
    CHECK_EQ(streaming.stats().regionsRemoved, std::size_t{2});
    CHECK_EQ(streaming.stats().instancesRemoved, std::size_t{3});
    // A replay's replica (a decoder following the client's messages) drops them too.
    Replica decoder;
    for (auto& m : server.joinMessages())
        decoder.handle(m);
    decoder.handle(server.streamData(std::vector{b, decal}, {StreamRegionRef{0, regionOf({3000, 5, 0})}}));
    CHECK(decoder.find(idB));
    decoder.decodingServerToClient = false;
    for (const auto& m : messages)
        decoder.handle(m);
    CHECK(!decoder.find(idB) && !decoder.find(idDecal));
    // Streamed again (the focus went there): the same ids come back as new instances.
    streaming.setFocus(Vector3{3000, 5, 0});
    replica.handle(server.streamData(std::vector{b, decal}, {StreamRegionRef{0, regionOf({3000, 5, 0})}}));
    auto again = replica.find(idB);
    CHECK(again && again->parent() == replica.game()->getService("Workspace"));
    CHECK(replica.find(idDecal) && replica.find(idDecal)->parent() == again.get());
    // The focus keeps it: nothing goes (the prefetch for the focus goes out instead).
    const auto later = serverReads(server, streaming.tick(replica, 3000));
    CHECK(itemsOf<RegionRemovalItem>(later).empty());
    CHECK_EQ(itemsOf<StreamPrefetchRequestItem>(later).size(), std::size_t{1});
    CHECK(replica.find(idB));
    // Back home once the server has answered the request (a live request keeps its area too):
    // it goes again, with the next sequence number.
    const std::array<Item, 1> answer{RuntimeItem{StreamPrefetchInfoItem{regionOf({3000, 5, 0}), 2, 1}}};
    replica.handle(encodeData(answer, server.schema(), server.context()));
    streaming.setFocus(Vector3{0, 5, 0});
    removals = itemsOf<RegionRemovalItem>(serverReads(server, streaming.tick(replica, 4000)));
    CHECK(removals.size() == 1 && removals[0].sequence == 3 && removals[0].removed.size() == 2);
    CHECK(!replica.find(idB));
    // The server's own Destroy and scripts' changes don't confuse it: a destroyed part is just gone.
    replica.handle(server.streamData(std::vector{b}, {StreamRegionRef{0, regionOf({3000, 5, 0})}}));
    replica.remove(idB);
    CHECK(itemsOf<RegionRemovalItem>(serverReads(server, streaming.tick(replica, 5000))).empty());
}

// A LocalScript's Player:RequestStreamAroundAsync goes to the host's hook
// (which sends the Stream Prefetch Request) and waits for the answer.
TEST(script_request_stream_around_waits_for_the_server)
{
    auto run = [](bool hooked) {
        auto game = DataModel::create();
        auto me = make("Player", "Me", game->getService("Players"));
        dynamic_cast<Player&>(*me).setUserId(42);
        // In our PlayerGui, where a live server puts the StarterGui copy.
        auto script = make("LocalScript", "Ask", make("PlayerGui", "PlayerGui", me.get()).get());
        dynamic_cast<LocalScript&>(*script).setSource(R"(
            local me = game:GetService("Players").LocalPlayer
            print("asking")
            me:RequestStreamAroundAsync(Vector3.new(100, 5, -20), 7)
            print("streamed")
        )");
        struct Result {
            std::vector<std::string> lines;
            std::vector<std::pair<std::uint64_t, double>> asked;
            Vector3 at;
        };
        auto result = std::make_shared<Result>();
        auto session = std::make_shared<lunar::ClientSession>(game);
        session->client().setOutput(
            [result](MessageType, std::string_view text) { result->lines.emplace_back(text); });
        if (hooked)
            session->client().setStreamAroundHook([result](std::uint64_t request, Vector3 at, double timeout) {
                result->asked.emplace_back(request, timeout);
                result->at = at;
            });
        CHECK(session->start(42));
        session->step(1.0 / 60.0);
        return std::pair{session, result};
    };
    auto has = [](const auto& lines, std::string_view text) { return std::ranges::find(lines, text) != lines.end(); };
    auto [session, result] = run(true);
    CHECK_EQ(result->asked.size(), std::size_t{1});
    CHECK(!result->asked.empty() && result->asked[0].second == 7.0);
    CHECK(result->at.x == 100.0f && result->at.y == 5.0f && result->at.z == -20.0f);
    CHECK(has(result->lines, "asking") && !has(result->lines, "streamed"));
    session->step(1.0 / 60.0);
    CHECK(!has(result->lines, "streamed")); // still waiting
    if (!result->asked.empty())
        session->client().streamAroundDone(result->asked[0].first);
    session->step(1.0 / 60.0);
    session->step(1.0 / 60.0);
    CHECK(has(result->lines, "streamed"));
    // Without a host to ask (nothing streams), the call returns at once.
    auto [plain, plainResult] = run(false);
    CHECK(has(plainResult->lines, "streamed"));
}

// Without a character in sight, the center the server streams around (its
// Streaming Info) keeps what it streamed there: dropping it would only bring it back.
TEST(net_streaming_client_keeps_the_server_stream_center)
{
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    ReplicationServer server(game, {"Workspace", "Players"});
    Replica replica;
    for (auto& m : server.joinMessages())
        replica.handle(m);
    auto home = part("Home", {600, 0, -200}, workspace);
    replica.handle(server.streamData(std::vector{home}, {StreamRegionRef{0, regionOf({600, 0, -200})}}));
    const std::array<Item, 1> info{RuntimeItem{StreamingInfoItem{regionOf({600, 0, -200}), 16, 0, 0}}};
    replica.handle(encodeData(info, server.schema(), server.context()));
    StreamingClientOptions options;
    options.streamOutStuds = 300;
    StreamingClient streaming(options);
    streaming.setFocus(Vector3{3000, 5, 3000});
    const auto items = serverReads(server, streaming.tick(replica, 1000));
    CHECK(itemsOf<RegionRemovalItem>(items).empty());
    CHECK(replica.find(server.idOf(home.get())));
    CHECK(streaming.stats().lastInfo && streaming.stats().lastInfo->radius == 16);
}

// A region the server sends straight back after we dropped it is left alone a while.
TEST(net_streaming_client_leaves_bouncing_regions_alone)
{
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    ReplicationServer server(game, {"Workspace", "Players"});
    Replica replica;
    for (auto& m : server.joinMessages())
        replica.handle(m);
    auto far = part("Far", {2000, 0, 0}, workspace);
    const std::vector<StreamRegionRef> regions{StreamRegionRef{0, regionOf({2000, 0, 0})}};
    replica.handle(server.streamData(std::vector{far}, regions));
    StreamingClientOptions options;
    options.streamOutStuds = 300;
    StreamingClient streaming(options);
    streaming.setFocus(Vector3{0, 0, 0});
    CHECK_EQ(itemsOf<RegionRemovalItem>(serverReads(server, streaming.tick(replica, 1000))).size(), std::size_t{1});
    CHECK(!replica.find(server.idOf(far.get())));
    replica.handle(server.streamData(std::vector{far}, regions)); // back 1.5 s later
    CHECK(itemsOf<RegionRemovalItem>(serverReads(server, streaming.tick(replica, 2500))).empty());
    CHECK_EQ(streaming.stats().bounced, std::size_t{1});
    CHECK(itemsOf<RegionRemovalItem>(serverReads(server, streaming.tick(replica, 60000))).empty());
    CHECK(replica.find(server.idOf(far.get())));
    // After the hold it goes again.
    CHECK_EQ(itemsOf<RegionRemovalItem>(serverReads(server, streaming.tick(replica, 2500 + 120001))).size(),
             std::size_t{1});
}
