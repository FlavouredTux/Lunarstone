#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Session.hpp>
#include <lunar/net/Streaming.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

std::string toHex(std::span<const std::uint8_t> bytes)
{
    std::string out;
    for (std::uint8_t b : bytes)
        out += std::format("{:02x}", b);
    return out;
}

std::vector<std::uint8_t> fromHex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

// The body bytes of one runtime item (type byte included), as the client writes them.
std::vector<std::uint8_t> itemBytes(const RuntimeItem& item, const ItemContext& ctx)
{
    NetworkStream out;
    out.writeU8(runtimeItemType(item));
    CHECK(writeRuntimeItem(out, item, {}, ctx));
    return std::move(out).take();
}

std::optional<RuntimeItem> readItem(std::span<const std::uint8_t> bytes, const ItemContext& ctx,
                                    std::string* err = nullptr)
{
    NetworkStream in(bytes);
    const std::uint8_t type = in.readU8();
    std::string error;
    auto r = readRuntimeItem(in, type, {}, ctx, nullptr, error);
    if (err)
        *err = error;
    if (r)
        CHECK_EQ(in.left(), std::size_t{0});
    return r;
}

void roundTrip(const RuntimeItem& item, const ItemContext& ctx)
{
    const auto bytes = itemBytes(item, ctx);
    std::string error;
    auto back = readItem(bytes, ctx, &error);
    if (!back)
        lt::fail(std::format("item {:#04x} did not read back: {}", runtimeItemType(item), error),
                 std::source_location::current());
    CHECK(*back == item);
}

std::optional<assets::json::Value> pingVectors()
{
    const std::filesystem::path path = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors/ping_items.json";
    std::ifstream f(path);
    if (!f)
        return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = assets::json::parse(ss.str());
    if (!doc)
        return std::nullopt;
    return *doc;
}

std::uint64_t hexU64(const assets::json::Value& v)
{
    const std::string* s = v.string();
    return s ? std::stoull(*s, nullptr, 16) : 0;
}

const ItemContext kClient{false, kBaseCapabilities};
const ItemContext kServer{true, kBaseCapabilities};

std::optional<assets::json::Value> vectors(const char* name)
{
    const std::filesystem::path path = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors" / name;
    std::ifstream f(path);
    if (!f)
        return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = assets::json::parse(ss.str());
    if (!doc)
        return std::nullopt;
    return *doc;
}

// The first wire string of a writer's vector list.
std::string wireOf(const assets::json::Value& doc, const char* key, std::size_t index = 0)
{
    const auto* list = doc[key] ? doc[key]->array() : nullptr;
    if (!list || index >= list->size())
        return "(missing " + std::string(key) + ")";
    const std::string* s = (*list)[index].string();
    return s ? *s : "";
}

} // namespace

// The client's own item writers were run under emulation on hand-filled
// items (emu/t_streamitems.py, vectors/stream_items.json); ours must produce
// the same bytes, and our readers must take the client's readers' inputs.
TEST(net_stream_items_match_client)
{
    const auto doc = vectors("stream_items.json");
    if (!doc)
        SKIP("stream_items.json not present");
    const ItemContext none{false, 0};
    const ItemContext cap56{false, caps::StreamInfoExtra};
    const ItemContext cap39{false, caps::FocusExtra};
    const ItemContext cap37{false, caps::CapacityUpdateExtra};
    // Region ids and instance refs.
    for (const auto& c : *(*doc)["region"]->array()) {
        const auto* xyz = c["xyz"]->array();
        RegionId id{static_cast<std::int32_t>(xyz->at(0).number().value_or(0)),
                    static_cast<std::int32_t>(xyz->at(1).number().value_or(0)),
                    static_cast<std::int32_t>(xyz->at(2).number().value_or(0))};
        NetworkStream out;
        writeRegionId(out, id);
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());
        NetworkStream in(out.bytes());
        CHECK(readRegionId(in) == id);
    }
    for (const auto& c : *(*doc)["ref"]->array()) {
        const auto scope = static_cast<std::uint32_t>(c["scope"]->number().value_or(0));
        const auto index = static_cast<std::uint32_t>(c["index"]->number().value_or(0));
        if (scope == 0 || scope == 0xffffffff)
            continue; // scope 0 is the client's own (written as its peer id), -1 is null: not how our ids work
        NetworkStream out;
        writeInstanceId(out, {scope, index});
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());
    }
    CHECK_EQ(toHex(itemBytes(RuntimeItem{StreamFocusRemoveItem{0x01020304}}, none)), wireOf(*doc, "focus_remove"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{StreamFocusFrustumRemoveItem{0x0a0b0c0d}}, none)),
             wireOf(*doc, "frustum_remove"));
    StreamFocusFrustumItem fr;
    fr.focusId = 7;
    for (std::size_t i = 0; i < 15; ++i)
        fr.frustum[i] = static_cast<float>(i) * 0.5f;
    CHECK_EQ(toHex(itemBytes(RuntimeItem{fr}, none)), wireOf(*doc, "frustum"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{IsrTimestampItem{-3}}, none)), wireOf(*doc, "isr_timestamp"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{RegionTerrainLodRemovalItem{{1, 2, 3}, 2}}, none)),
             wireOf(*doc, "region_terrain_lod", 0));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{RegionTerrainLodRemovalItem{{300, -1, 0}, 1}}, none)),
             wireOf(*doc, "region_terrain_lod", 1));
    // The 8 bytes after the position are RequestStreamAroundAsync's timeout, an f64 (50 s here).
    CHECK_EQ(toHex(itemBytes(RuntimeItem{StreamPrefetchRequestItem{{1.5f, 2.5f, 3.5f}, 50.0, 8}}, none)),
             wireOf(*doc, "prefetch_request"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{StreamPrefetchInfoItem{{0, 2, 0}, 3, 4}}, none)),
             wireOf(*doc, "prefetch_info"));
    StreamingInfoItem si{{1, 1, 1}, 3, 100, -5};
    CHECK_EQ(toHex(itemBytes(RuntimeItem{si}, none)), wireOf(*doc, "streaming_info"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{si}, cap56)), wireOf(*doc, "streaming_info_cap56"));
    StreamFocusItem f;
    f.focusId = 1;
    f.region = {-1, 0, 1};
    f.completedRadius = 4;
    f.radius = 256.0f;
    f.flagA = 1;
    f.flagB = 0;
    f.extra = 3;
    CHECK_EQ(toHex(itemBytes(RuntimeItem{f}, none)), wireOf(*doc, "focus"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{f}, cap39)), wireOf(*doc, "focus_cap39"));
    f.radius.reset();
    CHECK_EQ(toHex(itemBytes(RuntimeItem{f}, none)), wireOf(*doc, "focus_noradius"));
    ClientCapacityUpdateItem cu{1, 1000, 7, 2, 99};
    CHECK_EQ(toHex(itemBytes(RuntimeItem{cu}, none)), wireOf(*doc, "capacity_update"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{cu}, cap37)), wireOf(*doc, "capacity_update_cap37"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{InstanceRemovalItem{{3, 4}}}, none)), wireOf(*doc, "instance_removal"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{PersistentPlayerItem{true, {1, 7}}}, none)),
             wireOf(*doc, "persistent_player_add"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{PersistentPlayerItem{false, {1, 7}}}, none)),
             wireOf(*doc, "persistent_player_remove"));
    InstanceIntentItem ii;
    ii.intents = {{{1, 4}, 2}, {{1, 5}, 0}};
    CHECK_EQ(toHex(itemBytes(RuntimeItem{ii}, none)), wireOf(*doc, "instance_intent"));
    CHECK_EQ(toHex(itemBytes(RuntimeItem{ClientQoSItem{1, 0, "report"}}, none)), wireOf(*doc, "client_qos"));
    // What the client's readers made of their inputs is what ours make.
    auto readerCase = [&](const char* key) -> std::optional<std::vector<std::uint8_t>> {
        const auto* list = (*doc)[key] ? (*doc)[key]->array() : nullptr;
        if (!list || list->empty() || !(*list)[0]["wire"] || !(*list)[0]["wire"]->string())
            return std::nullopt;
        return fromHex(*(*list)[0]["wire"]->string());
    };
    auto readAs = [&](std::uint8_t type, const std::vector<std::uint8_t>& body, const ItemContext& ctx) {
        std::vector<std::uint8_t> bytes{type};
        bytes.insert(bytes.end(), body.begin(), body.end());
        std::string error;
        auto r = readItem(bytes, ctx, &error);
        if (!r)
            lt::fail(std::format("reader vector for {:#04x}: {}", type, error), std::source_location::current());
        return *r;
    };
    if (auto w = readerCase("r_streaming_info"))
        CHECK(std::get<StreamingInfoItem>(readAs(item::StreamingInfo, *w, none).body)
              == StreamingInfoItem{{1, 1, 1}, 3, 100, 0});
    if (auto w = readerCase("r_focus")) {
        const RuntimeItem ri = readAs(item::StreamFocus, *w, none);
        const auto& got = std::get<StreamFocusItem>(ri.body);
        CHECK(got.focusId == 1 && got.region == RegionId{-1, 0, 1} && got.completedRadius == 4 && got.radius == 256.0f
              && got.flagA == 1 && got.flagB == 0);
    }
    if (auto w = readerCase("r_focus_noradius"))
        CHECK(!std::get<StreamFocusItem>(readAs(item::StreamFocus, *w, none).body).radius);
    if (auto w = readerCase("r_destroy")) {
        const RuntimeItem ri = readAs(item::Destroy, *w, none);
        const auto& got = std::get<DestroyItem>(ri.body);
        CHECK_EQ(got.instances.size(), std::size_t{2}); // the null ref is dropped
        CHECK_EQ(got.instances[0].scope, 2u);
        CHECK_EQ(got.instances[0].index, 5u);
    }
    if (auto w = readerCase("r_instance_intent")) {
        const RuntimeItem ri = readAs(item::InstanceIntent, *w, none);
        const auto& got = std::get<InstanceIntentItem>(ri.body);
        CHECK(got.intents.size() == 2 && got.intents[0].first == InstanceId{2, 4} && got.intents[0].second == 2);
    }
    // The emulated reader that takes the fps triple: the ServerReplicator reading a client's ping.
    if (auto w = readerCase("r_ping_server")) {
        const RuntimeItem ri = readAs(item::Ping, *w, kClient);
        const auto& got = std::get<PingItem>(ri.body);
        CHECK(got.flags == 6 && got.fps[1] == 240.0f && got.extra == 0xaabbccdd && got.kind == 1
              && got.xorred == 0x12345678);
    }
    if (auto w = readerCase("r_pingback")) {
        const RuntimeItem ri = readAs(item::PingBack, *w, kServer);
        const auto& got = std::get<PingBackItem>(ri.body);
        CHECK(got.recvMs == 1 && got.lastPacketHigh == 6 && got.mask == 7 && got.extra == 8);
    }
    // The region list of a stream data item: explicit, delta, explicit.
    if (const auto* rl = (*doc)["region_list"]) {
        StreamDataItem sd;
        sd.regions = {StreamRegionRef{0, {1, 2, 3}}, StreamRegionRef{1, {}}, StreamRegionRef{0, {300, -1, 0}}};
        const auto bytes = itemBytes(RuntimeItem{sd}, none);
        CHECK_EQ(toHex(std::span(bytes).subspan(5)), *(*rl)["wire"]->string());
    }
}

// The client's own PingItem, PingBackItem and ClientQoSItem writers were run
// under emulation (emu/t_pingitems.py); ours must produce the same bytes.
TEST(net_stream_ping_items_match_client)
{
    const auto doc = pingVectors();
    if (!doc)
        SKIP("ping_items.json not present");
    const auto* pings = (*doc)["ping"] ? (*doc)["ping"]->array() : nullptr;
    const auto* backs = (*doc)["pingback"] ? (*doc)["pingback"]->array() : nullptr;
    CHECK(pings && backs);
    for (const auto& c : *pings) {
        PingItem p;
        p.flags = 0;
        p.time = hexU64(*c["time"]);
        p.sendMs = 0x11223344; // RakNet::GetTimeMS under emulation
        p.mask = 0;            // every flag global zero in the emulator
        p.extra = 0xaabbccdd;  // item+0x58
        p.kind = 7;            // item+0x5c (the emulation used a random byte; readers want < 2)
        // The emulated writer had no real replicator, so its class check skipped the fps
        // triple; a live client's pings carry it (genuine capture), here as zeros after sendMs.
        std::string want = *c["out"]->string();
        want.insert(2 * (1 + 1 + 8 + 4), std::string(2 * 12, '0'));
        CHECK_EQ(toHex(itemBytes(RuntimeItem{p}, kClient)), want);
        // The reader takes everything but the out-of-range kind: patch it and read back.
        auto bytes = fromHex(want);
        bytes.back() = 1;
        auto back = readItem(bytes, kClient); // a client wrote it: with the fps triple
        CHECK(back);
        const auto* r = std::get_if<PingItem>(&back->body);
        CHECK(r && r->time == p.time && r->sendMs == p.sendMs && r->extra == p.extra && r->kind == 1);
    }
    for (const auto& c : *backs) {
        const auto* fields = c["fields"]->array();
        CHECK(fields && fields->size() == 7);
        PingBackItem b;
        b.flags = 1;
        b.time = hexU64(*c["time"]);
        b.pingSendMs = static_cast<std::uint32_t>(hexU64((*fields)[0]));
        b.recvMs = static_cast<std::uint32_t>(hexU64((*fields)[1]));
        b.zero = static_cast<std::uint32_t>(hexU64((*fields)[2]));
        b.nowMs = 0x11223344;
        b.counter = static_cast<std::uint32_t>(hexU64((*fields)[3]));
        b.lastPacketBytes = static_cast<std::uint32_t>(hexU64((*fields)[4]));
        b.lastPacketHigh = static_cast<std::uint32_t>(hexU64((*fields)[5]));
        b.mask = 0;
        b.extra = static_cast<std::uint32_t>(hexU64((*fields)[6])); // inverted on the wire when time & 0x20
        const std::string want = *c["out"]->string();
        b.kind = fromHex(want).back();
        CHECK_EQ(toHex(itemBytes(RuntimeItem{b}, kClient)), want);
        auto bytes = fromHex(want);
        bytes.back() = 0;
        std::string error;
        auto back = readItem(bytes, kServer, &error);
        if (!back)
            lt::fail("PingBack vector did not read back: " + error, std::source_location::current());
        const auto* r = std::get_if<PingBackItem>(&back->body);
        CHECK(r);
        CHECK_EQ(r->extra, b.extra);
        CHECK_EQ(r->counter, b.counter);
        CHECK_EQ(r->lastPacketHigh, b.lastPacketHigh);
    }
    const std::string* qos = (*doc)["clientqos"] ? (*doc)["clientqos"]->string() : nullptr;
    CHECK(qos);
    ClientQoSItem q;
    q.kind = 2;
    q.flag = 1;
    CHECK_EQ(toHex(itemBytes(RuntimeItem{q}, kClient)), *qos);
}

// A client's ping carries the fps triple (only the ServerReplicator reads it);
// a server's doesn't. Same struct, the sender's role picks the body.
TEST(net_stream_ping_body_depends_on_sender)
{
    PingItem client = makePing(0x1000000020ULL, false); // time bit 5 set: `extra` is inverted on the wire
    client.extra = 5;
    client.fps = {60.0f, 240.0f, 60.0f};
    const auto cb = itemBytes(RuntimeItem{client}, kClient);
    CHECK_EQ(cb.size(), std::size_t{1 + 1 + 8 + 4 + 12 + 4 + 4 + 1}); // client pings carry the fps triple
    CHECK_EQ(toHex(std::span(cb).subspan(30, 4)), "faffffff");         // ~5
    roundTrip(RuntimeItem{client}, kClient);

    PingItem server = makePing(123456, true);
    server.xorred = 0x12345678;
    CHECK_EQ(server.flags, std::uint8_t{6});
    const auto sb = itemBytes(RuntimeItem{server}, kServer);
    CHECK_EQ(sb.size(), std::size_t{1 + 1 + 8 + 4 + 4 + 4 + 1 + 4}); // server pings don't
    roundTrip(RuntimeItem{server}, kServer);
    // The answer echoes time, send ms and kind; the RTT is now - low 32 bits of time.
    const PingBackItem back = answerPing(server, 123500, 123510);
    CHECK_EQ(back.time, server.time);
    CHECK_EQ(back.pingSendMs, server.sendMs);
    CHECK_EQ(pingRoundTripMs(back, 123520), std::uint32_t{64});
    roundTrip(RuntimeItem{back}, kClient);
    // Readers reject what the client rejects.
    auto bad = cb;
    bad[34] = 2; // kind
    CHECK(!readItem(bad, kClient));
}

TEST(net_stream_region_ids)
{
    for (RegionId id : {RegionId{0, 0, 0}, RegionId{-128, 127, 5}, RegionId{128, 0, 0}, RegionId{-70000, 3, 99999}}) {
        NetworkStream out;
        writeRegionId(out, id);
        const bool small = id.x >= -128 && id.x <= 127 && id.y >= -128 && id.y <= 127 && id.z >= -128 && id.z <= 127;
        CHECK_EQ(out.bytes().size(), small ? std::size_t{4} : std::size_t{13});
        CHECK_EQ(out.bytes()[0], small ? 0 : 1);
        NetworkStream in(out.bytes());
        CHECK(readRegionId(in) == id);
    }
    NetworkStream in(fromHex("00fe0105"));
    CHECK(readRegionId(in) == RegionId{-2, 1, 5});
}

// Every item round-trips in both roles, with the capability-gated fields on and off.
TEST(net_stream_items_round_trip)
{
    const ItemContext noCaps{true, 0};
    const ItemContext allCaps{true, ~0ULL & ~caps::Tracer};
    RequestCharacterItem rc;
    rc.playerFlags = 7;
    rc.appearance = "rbxassetid://1";
    rc.player = {2, 10};
    CFrameAckItem ack;
    ack.instance = {1, 9};
    ack.propertyId = 300;
    ack.hasVersion = true;
    ack.version = 3;
    ack.step = 42;
    StreamDataItem sd;
    sd.count = 2;
    sd.blob = {1, 2, 3, 4, 5, 6, 7, 8};
    sd.regions = {StreamRegionRef{0, {1, 2, 3}}, StreamRegionRef{1, {}}, StreamRegionRef{0, {300, -1, 0}}};
    RegionRemovalItem rr;
    rr.region = {4, 5, 6};
    rr.sequence = 9;
    rr.removed = {{1, 12}, {1, 13}, {2, 400}};
    rr.flags = 2;
    rr.extra = 77;
    RegionRemovalItem rrMany; // more refs than travel inline: one compressed block of them
    rrMany.region = {-1, 0, 300};
    rrMany.sequence = 10;
    for (std::uint32_t i = 0; i < 300; ++i)
        rrMany.removed.push_back({1, 1000 + i});
    StreamingInfoItem si{{1, 1, 1}, 3, 100, -5};
    StreamPrefetchInfoItem pi{{0, 2, 0}, 3, 4};
    StreamPrefetchRequestItem pr{{1.5f, 2.5f, 3.5f}, 50.0, 8};
    StreamFocusItem f;
    f.focusId = 1;
    f.region = {-1, 0, 1};
    f.completedRadius = 4;
    f.radius = 256.0f;
    f.flagA = 1;
    f.flagB = 0;
    f.extra = 3;
    StreamFocusItem fNone = f;
    fNone.radius.reset();
    StreamFocusFrustumItem fr;
    fr.focusId = 2;
    for (std::size_t i = 0; i < fr.frustum.size(); ++i)
        fr.frustum[i] = static_cast<float>(i) * 0.5f;
    DestroyItem d;
    d.instances = {{1, 5}, {1, 6}};
    ClientCapacityUpdateItem cu{1, 1000, 7, 2, 99};
    InstanceIntentItem ii;
    ii.intents = {{{1, 4}, 2}, {{1, 5}, 0}};
    StreamPinDataItem pin;
    pin.data = sd;
    pin.instance = {1, 3};
    pin.value = -2;
    pin.flag = 1;
    const std::vector<RuntimeItem> items{
        RuntimeItem{rc},
        RuntimeItem{PmcConfigItem{4}},
        RuntimeItem{ack},
        RuntimeItem{sd},
        RuntimeItem{rr},
        RuntimeItem{rrMany},
        RuntimeItem{InstanceRemovalItem{{3, 4}}},
        RuntimeItem{si},
        RuntimeItem{pi},
        RuntimeItem{pr},
        RuntimeItem{f},
        RuntimeItem{fNone},
        RuntimeItem{StreamFocusRemoveItem{5}},
        RuntimeItem{fr},
        RuntimeItem{StreamFocusFrustumRemoveItem{6}},
        RuntimeItem{d},
        RuntimeItem{cu},
        RuntimeItem{PersistentPlayerItem{true, {1, 7}}},
        RuntimeItem{PersistentPlayerItem{false, {1, 8}}},
        RuntimeItem{ClientQoSItem{1, 0, "report"}},
        RuntimeItem{IsrTimestampItem{-3}},
        RuntimeItem{pin},
        RuntimeItem{ii},
        RuntimeItem{RegionTerrainLodRemovalItem{{1, 2, 3}, 2}},
    };
    for (const RuntimeItem& it : items) {
        roundTrip(it, allCaps);
        // Without the capabilities the gated fields are dropped, the rest survives.
        auto back = readItem(itemBytes(it, noCaps), noCaps);
        CHECK(back);
        CHECK_EQ(runtimeItemType(*back), runtimeItemType(it));
    }
    // The sizes the client's writers produce (fixed layouts, capabilities off).
    CHECK_EQ(itemBytes(RuntimeItem{ack}, noCaps).size(), std::size_t{1 + 1 + 4 + 2 + 1 + 1}); // version 3: zigzag 6
    CHECK_EQ(itemBytes(RuntimeItem{ack}, allCaps).size(), std::size_t{1 + 1 + 4 + 2 + 1 + 1 + 4});
    CHECK_EQ(itemBytes(RuntimeItem{cu}, noCaps).size(), std::size_t{1 + 1 + 4 + 2});
    CHECK_EQ(itemBytes(RuntimeItem{cu}, allCaps).size(), std::size_t{1 + 1 + 4 + 2 + 1 + 4});
    CHECK_EQ(itemBytes(RuntimeItem{fr}, noCaps).size(), std::size_t{1 + 4 + 60});
    CHECK_EQ(toHex(itemBytes(RuntimeItem{StreamFocusRemoveItem{5}}, noCaps)), "2405000000");
    CHECK_EQ(toHex(itemBytes(RuntimeItem{IsrTimestampItem{-3}}, noCaps)), "23fdffffff");
    CHECK_EQ(toHex(itemBytes(RuntimeItem{PmcConfigItem{4}}, noCaps)), "0904");
    CHECK_EQ(toHex(itemBytes(RuntimeItem{InstanceRemovalItem{{3, 4}}}, noCaps)), "0f0304000000");
    CHECK_EQ(toHex(itemBytes(RuntimeItem{rc}, noCaps)),
             "08" "07000000" "0e000000"
                 + toHex(std::span(reinterpret_cast<const std::uint8_t*>(rc.appearance.data()), rc.appearance.size()))
                 + "02" "0a000000");
    // Region Removal: region, sequence, ref count, flags, then the refs inline
    // (up to 250) or as one compressed block (flag bit 0) [code: 0x1056d57ee].
    RegionRemovalItem rrSmall;
    rrSmall.region = {1, -1, 2};
    rrSmall.sequence = 3;
    rrSmall.removed = {{1, 7}, {1, 8}};
    CHECK_EQ(toHex(itemBytes(RuntimeItem{rrSmall}, noCaps)),
             "0e" "0001ff02" "03000000" "02000000" "00" "0107000000" "0108000000");
    const auto rrb = itemBytes(RuntimeItem{rrMany}, noCaps);
    NetworkStream rin(rrb);
    CHECK_EQ(rin.readU8(), item::RegionRemoval);
    CHECK(readRegionId(rin) == rrMany.region);
    CHECK_EQ(rin.readU32(), std::uint32_t{10});
    CHECK_EQ(rin.readU32(), std::uint32_t{300});
    CHECK_EQ(rin.readU8(), 0x01);
    auto refs = readCompressedBlock(rin);
    CHECK(refs && refs->size() == 300 * 5);
    CHECK_EQ(rin.left(), std::size_t{0});
    // Stream data: count, compressed blob, then the region list with its "more" bits.
    const auto sdb = itemBytes(RuntimeItem{sd}, noCaps);
    NetworkStream in(sdb);
    CHECK_EQ(in.readU8(), item::StreamData);
    CHECK_EQ(in.readU32(), std::uint32_t{2});
    auto blob = readCompressedBlock(in);
    CHECK(blob && *blob == sd.blob);
    CHECK_EQ(in.readU8(), 0x08); // explicit region, more follow
    CHECK(readRegionId(in) == RegionId{1, 2, 3});
    CHECK_EQ(in.readU8(), 0x09); // delta code 1, more follow
    CHECK_EQ(in.readU8(), 0x00); // explicit, last
    CHECK(readRegionId(in) == RegionId{300, -1, 0});
    CHECK_EQ(in.left(), std::size_t{0});
    // An empty stream data (count 0) has no block.
    StreamDataItem empty;
    empty.regions = {StreamRegionRef{4, {}}};
    CHECK_EQ(toHex(itemBytes(RuntimeItem{empty}, noCaps)), "0d0000000004");
    // Stats items keep their tail as bytes.
    auto stats = readItem(fromHex("11" "02000000" "0001020304"), kServer);
    CHECK(stats);
    const auto* st = std::get_if<StatsItem>(&stats->body);
    CHECK(st && st->version == 2 && st->rest.size() == 5);
}

// The runtime items travel inside ID_DATA next to the others, through both framings.
TEST(net_stream_items_in_data_messages)
{
    const Schema schema = Schema::fromEngine();
    for (bool remap : {false, true}) {
        Framing framing;
        framing.remapIds = remap;
        const std::vector<Item> items{MarkerItem{7}, Item{RuntimeItem{makePing(1000, true)}},
                                      Item{RuntimeItem{DestroyItem{{{1, 2}}}}},
                                      Item{RuntimeItem{StreamFocusRemoveItem{3}}}};
        const auto message = encodeData(items, schema, {}, framing, kServer);
        Node tree;
        auto back = decodeData(message, schema, {}, &tree, framing, kServer);
        CHECK(back);
        CHECK_EQ(back->size(), std::size_t{4});
        CHECK(std::get<MarkerItem>((*back)[0]).marker == 7);
        CHECK(std::get<RuntimeItem>((*back)[1]) == std::get<RuntimeItem>(items[1]));
        CHECK(std::get<RuntimeItem>((*back)[2]) == std::get<RuntimeItem>(items[2]));
        const std::string text = render(tree);
        CHECK(text.find("Ping") != std::string::npos && text.find("Destroy") != std::string::npos);
    }
    // A body the client rejects stops the read, with the rest dumped (not thrown).
    NetworkStream bad;
    bad.writeU8(0x83);
    bad.writeU8(item::PmcConfig);
    bad.writeU8(9);
    bad.writeU8(item::End);
    Node tree;
    auto r = decodeData(bad.bytes(), schema, {}, &tree, {}, kServer);
    CHECK(!r);
    CHECK(render(tree).find("stopped") != std::string::npos);
}

TEST(net_stream_kick_chat_challenge_packets)
{
    // Kick: string32 message, plus a string32 metadata JSON with capability 50.
    KickMessage kick{"You have been kicked", std::nullopt};
    auto bytes = encodeKick(kick, 0);
    CHECK_EQ(bytes.size(), std::size_t{1 + 4 + kick.message.size()});
    CHECK(decodeKick(bytes, 0) == kick);
    KickMessage withMeta{"bye", std::string(R"({"reason":"test"})")};
    bytes = encodeKick(withMeta, caps::KickMetadata);
    CHECK(decodeKick(bytes, caps::KickMetadata) == withMeta);
    CHECK(decodeKick(bytes, 0)->message == "bye");
    // Chat: ref sender, string32, two optional flag bytes; whispers add a target; game chat has no sender.
    for (std::uint8_t id : {msg::ChatAll, msg::ChatTeam, msg::ChatPlayer, msg::ChatGame}) {
        ChatMessage c;
        c.id = id;
        c.sender = {2, 5};
        c.target = id == msg::ChatPlayer ? InstanceId{2, 9} : InstanceId{};
        c.text = "hello";
        c.flagA = 0;
        c.flagB = 1;
        auto wire = encodeChat(c);
        if (id == msg::ChatGame)
            c.sender = {};
        CHECK(decodeChat(wire) == c);
        if (id == msg::ChatAll)
            CHECK_EQ(toHex(wire), "87" "0205000000" "0500000068656c6c6f" "00" "01");
    }
    ChatMessage noFlags;
    noFlags.id = msg::ChatAll;
    noFlags.sender = {2, 5};
    noFlags.text = "x";
    CHECK(decodeChat(encodeChat(noFlags)) == noFlags);
    // Bytecode challenge both ways.
    BytecodeChallenge ch{11, 22, "\x1b\x00\x00bytecode"};
    CHECK(decodeBytecodeChallenge(encodeBytecodeChallenge(ch)) == ch);
    BytecodeResponse resp{22, 0xdeadbeef};
    CHECK(decodeBytecodeResponse(encodeBytecodeResponse(resp)) == resp);
    CHECK_EQ(toHex(encodeBytecodeResponse(resp)), "9b" "16000000" "efbeadde");
    // Touches: ref pairs with a state byte, a null ref ends the list.
    TouchesMessage touches;
    touches.touches = {{{1, 5}, {1, 6}, 0}, {{1, 5}, {1, 7}, 0x81}};
    CHECK_EQ(toHex(encodeTouches(touches)), "86" "0105000000" "0106000000" "00" "0105000000" "0107000000" "81" "00");
    CHECK(decodeTouches(encodeTouches(touches)) == touches);
    // Shared string hashes: u16 count, 16 bytes each (or kind-tagged).
    SharedStringHashesMessage hashes;
    hashes.hashes = {std::array<std::uint8_t, 16>{1, 2, 3}, std::array<std::uint8_t, 16>{9}};
    CHECK_EQ(encodeSharedStringHashes(hashes).size(), std::size_t{1 + 2 + 32});
    CHECK(decodeSharedStringHashes(encodeSharedStringHashes(hashes)) == hashes);
    auto tagged = fromHex("a7" "0100" "00" "000102030405060708090a0b0c0d0e0f");
    CHECK(decodeSharedStringHashes(tagged) && decodeSharedStringHashes(tagged)->hashes[0][15] == 15);
    // 0x9c: kind 0 carries an i16-counted blob.
    CHECK(decodeScriptBlob(fromHex("9c" "00" "01000000" "0300" "aabbcc")).has_value());
    auto blob = decodeScriptBlob(fromHex("9c" "00" "01000000" "0300" "aabbcc"));
    CHECK(blob && blob->id == 1 && blob->blob.size() == 3);
    CHECK(decodeScriptBlob(fromHex("9c" "01" "02000000"))->blob.empty());
    // Microprofiler: two ints and a compressed string.
    NetworkStream inner;
    inner.writeString32("<html>profile</html>");
    NetworkStream mp;
    mp.writeU8(msg::Microprofiler);
    mp.writeI32(5);
    mp.writeI32(20);
    writeCompressedBlock(mp, inner.bytes());
    auto m = decodeMicroprofiler(mp.bytes());
    CHECK(m && m->kind == 5 && m->html == "<html>profile</html>");
    // Every one of these describes itself (and the undecoded ones hex-dump).
    for (const auto& wire :
         {encodeKick(withMeta, caps::KickMetadata), encodeChat(noFlags), encodeBytecodeChallenge(ch),
          std::vector<std::uint8_t>{msg::Isr, 1, 2, 3},
          std::vector<std::uint8_t>{msg::Rna3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}, encodeTouches(touches)}) {
        Node n;
        CHECK(describeSessionMessage(wire, n, caps::KickMetadata));
        CHECK(!n.children.empty());
    }
    Node none;
    CHECK(!describeSessionMessage(std::vector<std::uint8_t>{0x83}, none, 0));
}

TEST(net_stream_security_channel_ack)
{
    // A live server's 0xa2 answering the genuine client's first report.
    const std::vector<std::uint8_t> ack{msg::Rna2, 0x46, 0xa5, 0x71, 0xcb, 0x8d, 0x4a, 0xe3,
                                        0x97,      0xec, 0xff, 0xff, 0xdf, 0xff};
    Node n;
    CHECK(describeSessionMessage(ack, n, 0));
    CHECK(render(n).find("security channel: ack of report 1") != std::string::npos);
}

// ID_CLUSTER as a 0.743 server sends terrain [live: a chapter of The Mimic,
// 876 chunks]: 16-cell chunks, each a header, a position delta, a byte
// that's 1 for all air, the runs, and a 0. These are one packet's bytes,
// decompressed: a chunk with some mud on its bottom layer, then one all air.
TEST(net_stream_cluster_packets)
{
    ClusterMessage raw;
    raw.terrain = {1, 3};
    raw.undecoded = fromHex(std::string("040c00fd0080bfcc82014c75800ccc8202800ccc8202800ccc820280ff80ff80ff")
                            + "80ff80ff80ff80ff80ff80ff80ff80ff80ff80ff80ff80ff800c00040100000100");
    const auto c = decodeCluster(encodeCluster(raw));
    CHECK(c.has_value());
    if (!c)
        return;
    CHECK(c->undecoded.empty());
    CHECK_EQ(c->chunks.size(), std::size_t{2});
    if (c->chunks.size() != 2)
        return;
    CHECK(c->chunks[0].position == (RegionId{12, 0, -3}));
    CHECK(!c->chunks[0].empty);
    CHECK(c->chunks[1].position == (RegionId{13, 0, -3}));
    CHECK(c->chunks[1].empty);
    CHECK(decodeCluster(encodeCluster(*c)) == c);

    // As cells: 192 of air, then mud (material 12) 130/255 full, ... 4096 in all.
    const auto block = clusterBlock(c->chunks[0]);
    CHECK(block.has_value());
    if (!block)
        return;
    CHECK_EQ(block->side, 16);
    CHECK(block->origin == (std::array<int, 3>{192, 0, -48}));
    std::size_t cells = 0;
    for (const TerrainChunk::Run& run : block->runs)
        cells += run.count;
    CHECK_EQ(cells, std::size_t{4096});
    CHECK(block->runs[0].material == 0 && block->runs[0].count == 192);
    CHECK(block->runs[1].material == 12 && block->runs[1].occupancy == 130 && block->runs[1].count == 2);
    const auto air = clusterBlock(c->chunks[1]);
    CHECK(air && air->runs.empty() && air->origin == (std::array<int, 3>{208, 0, -48}));

    // Applied to a replica: its Terrain has the mud where it was sent, the
    // 193rd cell (X fastest, then Z, then Y): x 0, z 12, y 0 of the chunk.
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    auto* terrain = workspace->findFirstChildOfClass("Terrain");
    if (!terrain) {
        auto made = ClassRegistry::find("Terrain")->factory();
        made->setParent(workspace);
        terrain = made.get();
    }
    // The replica learns ids from New Instance items; give Terrain one by hand through a join.
    ReplicationServer server(game, {"Workspace"});
    Replica client;
    for (auto& m : server.joinMessages())
        client.handle(m);
    ClusterMessage forClient = *c;
    forClient.terrain = server.idOf(terrain);
    Node tree;
    CHECK(client.handle(encodeCluster(forClient), &tree));
    const auto* t
        = dynamic_cast<const Terrain*>(client.game()->getService("Workspace")->findFirstChildOfClass("Terrain"));
    CHECK(t != nullptr);
    if (!t)
        return;
    CHECK_EQ(t->chunks().size(), std::size_t{1});
    CHECK(t->voxelAt(192, 0, -36).material == Enum::Material::Mud);
    CHECK_EQ(t->voxelAt(192, 0, -36).occupancy, 130);
    CHECK(t->voxelAt(192, 1, -36).material == Enum::Material::Air);
    CHECK_EQ(client.runtime.clusters.size(), std::size_t{1});
    CHECK(render(tree).find("chunk at [12, 0, -3]") != std::string::npos);
}

// The client's side of a session: it answers the server's pings, pings
// itself, echoes markers, asks for its character once the place id is
// verified, and sends its quota once something streams in.
TEST(net_stream_client_session)
{
    auto game = DataModel::create();
    auto* players = game->getService("Players");
    auto player = ClassRegistry::find("Player")->factory(); // Player isn't script-creatable
    player->setName("Tester");
    player->setParent(players);
    ReplicationServer server(game, {"Workspace", "Players"});
    Replica replica;
    for (auto& m : server.joinMessages())
        replica.handle(m);
    CHECK_EQ(replica.markers.size(), std::size_t{1});
    ClientSessionOptions options;
    options.appearance = "look";
    ClientSession session(options);
    ServerSession serverSide;
    const ValueContext ctx = server.context();
    auto deliverToClient = [&](const std::vector<RuntimeItem>& items) {
        std::vector<Item> wrapped(items.begin(), items.end());
        const auto m = encodeData(wrapped, server.schema(), ctx, {}, ItemContext{true, kBaseCapabilities});
        replica.handle(m);
    };
    auto deliverToServer = [&](const std::vector<std::uint8_t>& m, std::uint64_t now) {
        if (m.empty() || m[0] != 0x83)
            return;
        auto items = decodeData(m, server.schema(), ctx, nullptr, {}, ItemContext{false, kBaseCapabilities});
        CHECK(items);
        for (const Item& it : *items)
            if (const auto* r = std::get_if<RuntimeItem>(&it))
                serverSide.onItem(*r, now);
    };
    // t=1000: the server pings; the client's first tick answers it, pings and echoes the marker.
    deliverToClient(serverSide.take(1000));
    CHECK_EQ(serverSide.pingsSent, std::size_t{1});
    CHECK_EQ(replica.runtime.pendingPings.size(), std::size_t{1});
    auto out = session.tick(replica, 1005);
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(session.pingBacksSent, std::size_t{1});
    CHECK_EQ(session.pingsSent, std::size_t{1});
    CHECK_EQ(session.markersEchoed, std::size_t{1});
    CHECK(!session.characterRequested);
    for (auto& m : out)
        deliverToServer(m, 1010);
    CHECK_EQ(serverSide.roundTripsMs.size(), std::size_t{1});
    CHECK_EQ(serverSide.roundTripsMs[0], std::uint32_t{10});
    CHECK_EQ(serverSide.pingBacksSent, std::size_t{1});
    // The PingBack comes back: an RTT for the client.
    deliverToClient(serverSide.take(1010));
    out = session.tick(replica, 1020);
    CHECK(out.empty()); // 15 ms since the last ping: nothing due
    CHECK_EQ(session.roundTripsMs.size(), std::size_t{1});
    CHECK_EQ(session.roundTripsMs[0], std::uint32_t{15});
    // The place id verdict triggers the character request, naming the Player.
    session.onMessage(std::vector<std::uint8_t>{kPlaceIdVerifiedId, 1});
    out = session.tick(replica, 1600);
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK(session.characterRequested);
    for (auto& m : out)
        deliverToServer(m, 1600);
    CHECK_EQ(serverSide.characterRequests.size(), std::size_t{1});
    CHECK_EQ(serverSide.characterRequests[0].appearance, "look");
    CHECK(server.find(serverSide.characterRequests[0].player).get() == player.get());
    CHECK_EQ(session.pingsSent, std::size_t{2}); // 600 ms later: another ping
    // Stream data arriving makes the client send its quota once.
    StreamDataItem sd;
    sd.regions = {StreamRegionRef{4, {}}};
    deliverToClient({RuntimeItem{sd}});
    CHECK_EQ(replica.runtime.streamDataItems, std::size_t{1});
    out = session.tick(replica, 1700);
    CHECK(session.capacityUpdateSent);
    CHECK(session.tick(replica, 1800).empty());
    // A kick is noticed.
    session.onMessage(encodeKick({"bye", std::nullopt}, kBaseCapabilities));
    CHECK(session.kicked && session.kicked->message == "bye");
    // Destroy items remove instances from the replica.
    const InstanceId playerId = server.idOf(player.get());
    CHECK(replica.find(playerId));
    CHECK(replica.find(playerId)->parent() == replica.game()->findService("Players"));
    // An instance added after the join arrives parented too.
    auto late = ClassRegistry::find("Player")->factory();
    late->setName("Late");
    late->setParent(players);
    replica.handle(server.addInstance(late));
    auto lateOnClient = replica.find(server.idOf(late.get()));
    CHECK(lateOnClient);
    if (lateOnClient)
        CHECK_EQ(lateOnClient->fullName(), std::string("Players.Late"));
    deliverToClient({RuntimeItem{DestroyItem{{playerId}}}});
    CHECK(!replica.find(playerId));
    CHECK_EQ(replica.runtime.destroyed, std::size_t{1});
    // A challenge is answered only when a runner is given.
    BytecodeChallenge ch{1, 2, "code"};
    session.onMessage(encodeBytecodeChallenge(ch));
    CHECK(session.tick(replica, 1900).empty());
    ClientSessionOptions answering;
    answering.sendPings = false;
    answering.echoMarkers = false;
    answering.requestCharacter = false;
    answering.sendCapacityUpdate = false;
    answering.challenge = [](const BytecodeChallenge& c) { return std::optional<std::uint32_t>(c.a + c.b); };
    ClientSession answerer(answering);
    answerer.onMessage(encodeBytecodeChallenge(ch));
    out = answerer.tick(replica, 2000);
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK(decodeBytecodeResponse(out[0]) == BytecodeResponse{2, 3});
}

// A Replica describes the new packets for the log and --net-decode.
TEST(net_stream_replica_describes_packets)
{
    Replica replica;
    Node tree;
    CHECK(replica.handle(encodeKick({"bye", std::nullopt}, kBaseCapabilities), &tree));
    CHECK(render(tree).find("bye") != std::string::npos);
    CHECK_EQ(replica.runtime.kicks.size(), std::size_t{1});
    ChatMessage chat;
    chat.sender = {1, 1};
    chat.text = "hi";
    Node t2;
    CHECK(replica.handle(encodeChat(chat), &t2));
    CHECK_EQ(replica.runtime.chats.size(), std::size_t{1});
    Node t3;
    CHECK(replica.handle(std::vector<std::uint8_t>{msg::Rna2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}, &t3));
    CHECK(!t3.children.empty());
    // The message names.
    CHECK_EQ(std::string(messageIdName(0x9b)), "ID_BYTECODE_CHALLENGE");
    CHECK_EQ(std::string(messageIdName(0xa7)), "ID_SHARED_STRING_HASHES");
}

// The genuine client's CFrame Acknowledgement (0x0a) for a part the server
// moved: the version is the client's count of the server's changes to it.
TEST(net_stream_cframe_ack_reads_the_genuine_bytes)
{
    const ItemContext caps{true, caps::CFrameAckStep};
    const auto bytes = fromHex("0a01554c47004a26019e0101030000");
    const auto item = readItem(bytes, caps);
    CHECK(item.has_value());
    const auto* ack = item ? std::get_if<CFrameAckItem>(&item->body) : nullptr;
    CHECK(ack != nullptr);
    if (!ack)
        return;
    CHECK(ack->hasVersion);
    CHECK_EQ(ack->version, 79);
    CHECK_EQ(ack->step, std::uint32_t{769});
    CHECK_EQ(ack->propertyId, std::uint16_t{0x264a});
    CHECK(itemBytes(*item, caps) == bytes);
}

// What a session only records (touches, terrain clusters) keeps its latest
// few hundred, not every one it was ever sent.
TEST(net_session_records_stay_bounded)
{
    Replica replica;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        TouchesMessage touches;
        touches.touches = {{{1, i}, {1, i + 1}, 0}};
        CHECK(handleSessionMessage(replica, encodeTouches(touches), nullptr));
    }
    CHECK(replica.runtime.touches.size() < RuntimeState::recordLimit * 2);
    CHECK(replica.runtime.touches.size() >= RuntimeState::recordLimit);
    CHECK_EQ(replica.runtime.touches.back().touches[0].partA.index, 999u);
}

// The event log numbers what came from 0 and keeps the latest: readers keep
// their place by number; with no limit it keeps them all.
TEST(net_event_log_keeps_the_latest)
{
    EventLog log;
    log.setLimit(10);
    for (std::uint16_t i = 0; i < 25; ++i)
        log.push_back(EventInvocationItem{.id = {1, i}, .eventId = i});
    CHECK_EQ(log.size(), std::size_t{25});
    CHECK(log.first() >= 5u && log.first() <= 15u);
    CHECK_EQ(log.kept().size(), log.size() - log.first());
    for (std::size_t i = log.first(); i < log.size(); ++i)
        CHECK_EQ(log[i].eventId, i);
    CHECK_EQ(log.back().eventId, 24u);
    EventLog all;
    all.setLimit(0);
    for (std::uint16_t i = 0; i < 25000; ++i)
        all.push_back(EventInvocationItem{.eventId = i});
    CHECK_EQ(all.first(), std::size_t{0});
    CHECK_EQ(all.kept().size(), std::size_t{25000});
}
