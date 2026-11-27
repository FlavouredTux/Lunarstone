#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/Instance.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/JoinData.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/assets/Json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace lunar;
using namespace lunar::net;

namespace {

std::vector<std::uint8_t> unhex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

// A schema with one class whose properties use the join-time types.
Schema tinySchema()
{
    Schema s;
    s.classes.push_back({"Script", 0, 4, 0, 0, 0xffff});
    s.properties.push_back({"Name", nettype::String, 0xffff, 0, 0});
    s.properties.push_back({"Source", nettype::ProtectedString, 0xffff, 0, 0});
    s.properties.push_back({"LinkedSource", nettype::ContentId, 0xffff, 0, 0});
    s.properties.push_back({"RunContext", nettype::DictionaryString, 0xffff, 0, 0});
    s.contentPrefixes = {"", "rbxassetid://", "rbxasset://"};
    s.stringDictionary = {"Legacy", "Server", "Client"};
    return s;
}

// A DataModel with a few replicated instances under Workspace and ReplicatedStorage.
std::shared_ptr<DataModel> place()
{
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    auto part = ClassRegistry::create("Part");
    part->setName("Floor");
    part->set("CFrame", Value(CFrame(Vector3{0, -1, 0})));
    part->set("Anchored", Value(true));
    part->setParent(workspace);
    auto model = ClassRegistry::create("Model");
    model->setName("House");
    model->setParent(workspace);
    auto door = ClassRegistry::create("Part");
    door->setName("Door");
    door->setParent(model.get());
    auto weld = ClassRegistry::create("Weld");
    weld->set("Part0", Value(part));
    weld->set("Part1", Value(door));
    weld->setParent(door.get());
    auto folder = ClassRegistry::create("Folder");
    folder->setName("Remotes");
    folder->setParent(game->getService("ReplicatedStorage"));
    auto remote = ClassRegistry::create("RemoteEvent");
    remote->setParent(folder.get());
    return game;
}

} // namespace

TEST(net_joindata_shared_string_key_is_blake2b_128)
{
    CHECK_EQ(keyText(sharedStringKey("")), "cae66941d9efbd404e4d88758ea67670");
    CHECK_EQ(keyText(sharedStringKey("abc")), "cf4ab791c62b8d2b2109c90275287816");
    CHECK_EQ(keyText(sharedStringKey("hello")), "46fb7408d4f285228f4af516ea25851b");
    CHECK_EQ(keyText(sharedStringKey("The quick brown fox jumps over the lazy dog")),
             "249df9a49f517ddcd37f5c897620ec73");
}

TEST(net_joindata_shared_string_entries)
{
    NetworkStream out;
    writeSharedStringEntry(out, {sharedStringKey("hello"), 0, "hello"});
    writeSharedStringEntry(out, {sharedStringKey("abc"), 1, {}});
    const auto bytes = std::vector<std::uint8_t>(out.bytes().begin(), out.bytes().end());
    CHECK(bytes == unhex("46fb7408d4f285228f4af516ea25851b"
                         "00"
                         "0568656c6c6f"
                         "cf4ab791c62b8d2b2109c90275287816"
                         "01"));
    NetworkStream in(bytes);
    auto a = readSharedStringEntry(in);
    auto b = readSharedStringEntry(in);
    CHECK(a && b);
    CHECK_EQ(a->marker, 0);
    CHECK_EQ(a->content, "hello");
    CHECK_EQ(b->marker, 1);
    CHECK(b->content.empty());
    CHECK(in.left() == 0);
    NetworkStream bad(unhex("46fb7408d4f285228f4af516ea25851b02"));
    CHECK(!readSharedStringEntry(bad));

    SharedStringCache cache;
    const SharedStringKey k = cache.add("print(1)");
    CHECK(cache.contains(k));
    CHECK(cache.find(k) && *cache.find(k) == "print(1)");
    CHECK(!cache.contains(sharedStringKey("print(2)")));
}

TEST(net_joindata_set_globals_layout)
{
    SetGlobals g;
    g.streamingEnabled = true;
    g.notCloudEdit = true;
    g.settings = {false, true, false, true};
    g.flag8 = false;
    g.peerId = 300;
    g.containers.push_back({{1, 2}, 0x1234, true, false});
    g.containers.push_back({{1, 300}, 7, false, true});
    const auto bytes = encodeSetGlobals(g, kBaseCapabilities); // no capability bit 40
    CHECK(bytes == unhex("81"
                         "01" // streamingEnabled
                         "01" // notCloudEdit
                         "00010001"
                         "00"   // flag8
                         "ac02" // peer id 300 (varint)
                         "02"   // 2 top containers
                         "01" "02000000" "3412" "01" "00"
                         "01" "2c010000" "0700" "00" "01"));
    auto d = decodeSetGlobals(bytes, kBaseCapabilities);
    CHECK(d);
    CHECK_EQ(d->peerId, 300u);
    CHECK(d->streamingEnabled && d->notCloudEdit && !d->flag8);
    CHECK(d->settings == g.settings);
    CHECK(!d->workspaceValue);
    CHECK_EQ(d->containers.size(), 2u);
    CHECK(d->containers[0].id == InstanceId{1, 2} && d->containers[0].classId == 0x1234 && d->containers[0].flagA
          && !d->containers[0].flagB);
    CHECK(d->containers[1].id == InstanceId{1, 300} && !d->containers[1].flagA && d->containers[1].flagB);

    // Capability bit 40 adds the u32 after the peer id.
    g.workspaceValue = 0x01020304;
    const auto withValue = encodeSetGlobals(g, kBaseCapabilities | (1ULL << 40));
    CHECK_EQ(withValue.size(), bytes.size() + 4);
    auto d2 = decodeSetGlobals(withValue, kBaseCapabilities | (1ULL << 40));
    CHECK(d2 && d2->workspaceValue == 0x01020304u);
    CHECK(!decodeSetGlobals(unhex("8101"), kBaseCapabilities));
}

TEST(net_joindata_small_items_layout)
{
    const Schema schema = Schema::fromEngine();
    const ValueContext ctx;
    const std::vector<Item> items{
        TagItem{13},
        ServerTimeItem{-5},
        ResolveAtomicTreeItem{{1, 5}, {1, 6}},
        ChangeInstItem{7, {0xaa, 0xbb}},
    };
    const auto bytes = encodeData(items, schema, ctx);
    CHECK(bytes == unhex("83"
                         "10" "0d000000"           // Tag 13
                         "26" "09"                 // Server Time -5 (zigzag)
                         "13" "0105000000" "0106000000"
                         "20" "07000000" "02000000" "aabb"
                         "00"));
    Node tree("items");
    auto decoded = decodeData(bytes, schema, ctx, &tree);
    CHECK(decoded && decoded->size() == 4);
    CHECK(std::get<TagItem>((*decoded)[0]).tag == 13);
    CHECK(std::get<ServerTimeItem>((*decoded)[1]).time == -5);
    CHECK(std::get<ResolveAtomicTreeItem>((*decoded)[2]).other == InstanceId{1, 6});
    CHECK(std::get<ChangeInstItem>((*decoded)[3]).bytes == std::vector<std::uint8_t>({0xaa, 0xbb}));
    const std::string text = render(tree);
    CHECK(text.find("Tag: tag 13 (join data done)") != std::string::npos);
    CHECK(text.find("Server Time: -5") != std::string::npos);
}

TEST(net_joindata_keyed_and_dictionary_strings)
{
    const Schema schema = tinySchema();
    SharedStringCache sender, receiver;
    ValueContext wctx, rctx;
    wctx.sharedStrings = &sender;
    wctx.contentPrefixes = rctx.contentPrefixes = &schema.contentPrefixes;
    wctx.stringDictionary = rctx.stringDictionary = &schema.stringDictionary;
    rctx.sharedStrings = &receiver;

    // A New item: the Source key travels in pass 1, its content right after the passes.
    NewInstanceItem n;
    n.id = {1, 2};
    n.classId = 0;
    n.properties = {{0, Value(std::string("Main"))},
                    {1, Value(std::string("print(1)"))},
                    {2, Value(std::string("rbxassetid://123"))},
                    {3, Value(std::string("Server"))}};
    const std::vector<Item> items{n};
    const auto bytes = encodeData(items, schema, wctx);
    CHECK(bytes == unhex("83" "02" "0102000000" "0000"
                         "00" "044d61696e"                                 // Name "Main"
                         "01" + keyText(sharedStringKey("print(1)")) +    // Source: key only
                         "02" "02" "03313233"                              // LinkedSource: prefix 1 + "123"
                         "03" "81"                                         // RunContext: dictionary id 1
                         "ff" "ff"                                         // end of pass 1, empty pass 2
                         + keyText(sharedStringKey("print(1)")) + "00" "087072696e742831" "29" // deferred content
                         "00"                                               // parent nil
                         "00"));
    CHECK(sender.contains(sharedStringKey("print(1)")));
    Node tree("items");
    auto decoded = decodeData(bytes, schema, rctx, &tree);
    CHECK(decoded && decoded->size() == 1);
    const auto& d = std::get<NewInstanceItem>((*decoded)[0]);
    CHECK_EQ(d.properties.size(), 4u);
    CHECK(std::get<std::string>(d.properties[1].second) == "print(1)");
    CHECK(std::get<std::string>(d.properties[2].second) == "rbxassetid://123");
    CHECK(std::get<std::string>(d.properties[3].second) == "Server");
    CHECK(receiver.contains(sharedStringKey("print(1)")));
    CHECK(render(tree).find("content follows") != std::string::npos);

    // The second time both sides know the key: no content on the wire.
    const auto again = encodeData(items, schema, wctx);
    CHECK_EQ(again.size(), bytes.size() - 16 - 1 - 1 - 8);
    auto decoded2 = decodeData(again, schema, rctx);
    CHECK(decoded2 && std::get<std::string>(std::get<NewInstanceItem>((*decoded2)[0]).properties[1].second) == "print(1)");

    // Change Property carries content ids in prefix form and defers contents the same way.
    SharedStringCache s2, r2;
    wctx.sharedStrings = &s2;
    rctx.sharedStrings = &r2;
    const std::vector<Item> change{ChangePropertyItem{{1, 2}, 1, Value(std::string("x = 1"))},
                                   ChangePropertyItem{{1, 2}, 2, Value(std::string("rbxasset://a.png"))}};
    const auto cbytes = encodeData(change, schema, wctx);
    // Each with the flag byte (00) between the property id and the value.
    CHECK(cbytes == unhex("83" "03" "0102000000" "0100" "00" + keyText(sharedStringKey("x = 1"))
                          + keyText(sharedStringKey("x = 1")) + "00" "0578203d2031"
                          "03" "0102000000" "0200" "00" "04" "05612e706e67" "00"));
    auto cdecoded = decodeData(cbytes, schema, rctx);
    CHECK(cdecoded && cdecoded->size() == 2);
    CHECK(std::get<std::string>(std::get<ChangePropertyItem>((*cdecoded)[0]).value) == "x = 1");
    CHECK(std::get<std::string>(std::get<ChangePropertyItem>((*cdecoded)[1]).value) == "rbxasset://a.png");

    // A numeric suffix is a zigzag varint after the prefix (0x1016e9b43):
    // Animator.OnCombinedUpdate sends rbxassetid://507766388 so [capture].
    NetworkStream numeric;
    numeric.writeU8(0x03); // prefix 1, numeric
    numeric.writeVarint(1015532776);
    NetworkStream numericIn(numeric.bytes());
    ValueContext prefixed = rctx;
    prefixed.prefixedContent = true;
    std::string numericText;
    auto numericValue = readValue(numericIn, nettype::ContentId, prefixed, &numericText);
    CHECK(numericValue && std::get<std::string>(*numericValue) == "rbxassetid://507766388");
    CHECK_EQ(numericText, "\"rbxassetid://507766388\" (prefix 1 + 507766388)");

    // Dictionary strings: inline short, inline long, and ids past 0x7e.
    NetworkStream ds;
    ValueContext plain;
    writeValue(ds, nettype::DictionaryString, Value(std::string("ab")), plain);
    writeValue(ds, nettype::DictionaryString, Value(std::string(130, 'z')), plain);
    const auto dsBytes = std::vector<std::uint8_t>(ds.bytes().begin(), ds.bytes().end());
    CHECK(dsBytes.size() == 3 + 1 + 2 + 130);
    CHECK(dsBytes[0] == 2 && dsBytes[1] == 'a' && dsBytes[2] == 'b' && dsBytes[3] == 0x7f && dsBytes[4] == 0x82);
    std::vector<std::string> big(200, "x");
    big[150] = "far";
    ValueContext bigCtx;
    bigCtx.stringDictionary = &big;
    NetworkStream fs;
    writeValue(fs, nettype::DictionaryString, Value(std::string("far")), bigCtx);
    CHECK(std::vector<std::uint8_t>(fs.bytes().begin(), fs.bytes().end()) == unhex("ff17")); // 0x7f + 23
    std::string text;
    auto far = readValue(fs, nettype::DictionaryString, bigCtx, &text);
    CHECK(far && std::get<std::string>(*far) == "far");
}

TEST(net_joindata_attribute_values)
{
    const std::vector<Value> values{Value(std::string("hi")),
                                    Value(true),
                                    Value(2.5),
                                    Value(Vector3{1, 2, 3}),
                                    Value(Color3{0.5f, 0.25f, 1}),
                                    Value(CFrame(Vector3{4, 5, 6})),
                                    Value(UDim2{{0.5f, 10}, {1, -2}}),
                                    Value(NumberRange{1, 2}),
                                    Value(Rect{{1, 2}, {3, 4}}),
                                    Value(BrickColor::fromNumber(21))};
    for (const Value& v : values) {
        NetworkStream out;
        CHECK(encodeAttributeValue(out, v));
        std::string text;
        auto back = decodeAttributeValue(out.bytes(), &text);
        CHECK(back);
        CHECK_EQ(toString(*back), toString(v));
    }
    CHECK(!decodeAttributeValue(unhex("7700")));
}

TEST(net_joindata_schema_trailing_sections)
{
    Schema s = tinySchema();
    s.isrClasses.push_back({0, {1, 3}});
    s.lrTypeRanges.push_back({0x3b, 1, 0, 0});
    s.lrTypeRanges.push_back({0x3c, 2, 5, 9});
    auto d = Schema::decodeBody(s.encodeBody());
    CHECK(d);
    CHECK(d->contentPrefixes == s.contentPrefixes);
    CHECK(d->stringDictionary == s.stringDictionary);
    CHECK(d->isrClasses.size() == 1 && d->isrClasses[0].properties == std::vector<std::uint32_t>({1, 3}));
    CHECK(d->lrTypeRanges.size() == 2 && d->lrTypeRanges[1].c == 9);
    // Without the sections the body still decodes (they're optional on the wire).
    Schema bare = Schema::fromEngine();
    auto body = bare.encodeBody();
    body.resize(body.size() - 1 - 1 - 4 - 1); // drop the four empty sections
    auto d2 = Schema::decodeBody(body);
    CHECK(d2 && d2->classes.size() == bare.classes.size() && d2->contentPrefixes.size() == 1);
    // A property of a type the codec doesn't know names class, property and type.
    Schema odd = tinySchema();
    odd.properties[1].type = 0x46;
    NewInstanceItem n;
    n.id = {1, 1};
    n.properties = {{1, Value(std::string("x"))}};
    NetworkStream raw;
    raw.writeU8(0x83);
    raw.writeU8(item::New);
    writeInstanceId(raw, n.id);
    raw.writeU16(0);
    raw.writeU8(1);
    raw.writeBytes(unhex("00112233"));
    Node tree("items");
    auto r = decodeData(raw.bytes(), odd, {}, &tree);
    CHECK(!r);
    CHECK(render(tree).find("can't read Script.Source as ? (0x46)") != std::string::npos);
}

TEST(net_joindata_join_data_items_round_trip)
{
    auto game = place();
    ReplicationServer server(game, {"Workspace", "ReplicatedStorage"});
    const Schema& schema = server.schema();
    // Join Data V2 instance blobs and the older Join Data item carry the same instance layout.
    std::vector<NewInstanceItem> instances;
    for (const auto& inst : game->getService("Workspace")->descendants()) {
        NewInstanceItem n;
        n.id = server.idOf(inst.get());
        n.classId = *schema.classId(inst->className());
        n.properties = {{*schema.propertyId(n.classId, "Name"), Value(inst->name())}};
        n.parent = server.idOf(inst->parent());
        instances.push_back(std::move(n));
    }
    JoinDataV2Item v2;
    v2.instances = instances;
    v2.invalidRanges = {};
    JoinDataItem v1;
    v1.instances = instances;
    JoinDataV2Item strings;
    strings.mode = 1;
    strings.sharedStrings = {{sharedStringKey("abc"), 0, "abc"}};
    JoinDataV2Item assets;
    assets.mode = 5;
    assets.netAssets = {{3, 0x1122334455667788ull, 4, {1, 2, 3}}, {0xffff, 0, 0, {}}};
    const std::vector<Item> items{strings, v2, v1, assets};
    const auto bytes = encodeData(items, schema, server.context());
    // u8 0x16, mode 2, the block, then the count and an empty invalid list.
    CHECK(bytes[0] == 0x83 && bytes[1] == item::JoinDataV2 && bytes[2] == 1);
    Replica replica;
    Node tree("items");
    auto decoded = decodeData(bytes, schema, replica.context(), &tree);
    CHECK(decoded && decoded->size() == 4);
    const auto& s = std::get<JoinDataV2Item>((*decoded)[0]);
    CHECK(s.mode == 1 && s.sharedStrings.size() == 1 && s.sharedStrings[0].content == "abc");
    CHECK(replica.sharedStrings().contains(sharedStringKey("abc")));
    const auto& d2 = std::get<JoinDataV2Item>((*decoded)[1]);
    CHECK(d2.mode == 2 && d2.instances.size() == instances.size());
    CHECK(d2.instances[1].id == instances[1].id && d2.instances[1].parent == instances[1].parent);
    CHECK(std::get<std::string>(d2.instances[1].properties[0].second) == "House");
    const auto& d1 = std::get<JoinDataItem>((*decoded)[2]);
    CHECK(d1.instances.size() == instances.size());
    const auto& a = std::get<JoinDataV2Item>((*decoded)[3]);
    CHECK(a.mode == 5 && a.netAssets.size() == 2 && a.netAssets[0].guid == 0x1122334455667788ull
          && a.netAssets[0].bytes.size() == 3 && a.netAssets[1].handleType == 0xffff);
    const std::string text = render(tree);
    CHECK(text.find("mode 1 (shared strings)") != std::string::npos);
    CHECK(text.find("instance 1: Model 1:") != std::string::npos);

    // A hole the sender marked invalid is skipped: cut the second instance out of the blob.
    NetworkStream body;
    std::vector<std::uint32_t> offsets;
    for (const NewInstanceItem& n : instances) {
        offsets.push_back(static_cast<std::uint32_t>(body.bytes().size()));
        writeInstanceId(body, n.id);
        body.writeU16(n.classId);
        writePropertyList(body, n.properties, schema.classes[n.classId], schema, server.context());
        writeInstanceId(body, n.parent);
    }
    NetworkStream holed;
    holed.writeU8(0x83);
    holed.writeU8(item::JoinDataV2);
    holed.writeU8(3);
    writeCompressedBlock(holed, body.bytes());
    holed.writeU32(static_cast<std::uint32_t>(instances.size() - 1));
    holed.writeVarint(1);
    holed.writeVarint(offsets[1]);
    holed.writeVarint(offsets[2] - offsets[1]);
    holed.writeU8(0);
    auto hd = decodeData(holed.bytes(), schema, replica.context());
    CHECK(hd && hd->size() == 1);
    const auto& h = std::get<JoinDataV2Item>((*hd)[0]);
    CHECK(h.mode == 3 && h.instances.size() == instances.size() - 1 && h.invalidRanges.size() == 1);
    CHECK_EQ(h.instances[0].id.index, instances[0].id.index);
    CHECK_EQ(h.instances[1].id.index, instances[2].id.index);
    CHECK_EQ(h.instances[2].id.index, instances[3].id.index);

    // Holes listed out of order, as a live server lists them: each is skipped
    // when the reader gets to it.
    NetworkStream unsorted;
    unsorted.writeU8(0x83);
    unsorted.writeU8(item::JoinDataV2);
    unsorted.writeU8(4);
    writeCompressedBlock(unsorted, body.bytes());
    unsorted.writeU32(static_cast<std::uint32_t>(instances.size() - 2));
    unsorted.writeVarint(2);
    unsorted.writeVarint(offsets[3]);
    unsorted.writeVarint((offsets.size() > 4 ? offsets[4] : body.bytes().size()) - offsets[3]);
    unsorted.writeVarint(offsets[1]);
    unsorted.writeVarint(offsets[2] - offsets[1]);
    unsorted.writeU8(0);
    auto ud = decodeData(unsorted.bytes(), schema, replica.context());
    CHECK(ud && ud->size() == 1);
    const auto& u = std::get<JoinDataV2Item>((*ud)[0]);
    CHECK(u.instances.size() == instances.size() - 2);
    std::vector<std::uint32_t> kept;
    for (const NewInstanceItem& n : u.instances)
        kept.push_back(n.id.index);
    std::vector<std::uint32_t> expected;
    for (std::size_t i = 0; i < instances.size(); ++i)
        if (i != 1 && i != 3)
            expected.push_back(instances[i].id.index);
    CHECK(kept == expected);
}

TEST(net_joindata_join_through_replica)
{
    for (const auto style : {ReplicationServer::JoinStyle::JoinDataV2, ReplicationServer::JoinStyle::NewItems}) {
        auto game = place();
        ReplicationServer server(game, {"Workspace", "ReplicatedStorage"});
        Replica replica;
        const auto messages = server.joinMessages(64, style); // tiny batches: several JDIv2 items
        for (const auto& m : messages)
            CHECK(replica.handle(m));
        CHECK_EQ(replica.instanceCount(), server.replicatedCount());
        if (style == ReplicationServer::JoinStyle::JoinDataV2) {
            CHECK(messages.size() >= 5); // schema, globals, 2+ data, tag, marker
            CHECK(messages[1][0] == msg::SetGlobals);
            CHECK(replica.globals && replica.globals->peerId == 2 && replica.globals->containers.size() == 2);
            CHECK(replica.joinDataDone);
            CHECK(replica.tags == std::vector<std::uint32_t>{13});
        } else {
            CHECK(!replica.globals);
        }
        CHECK(replica.markers == std::vector<std::uint32_t>{1});
        auto* workspace = replica.game()->findService("Workspace");
        CHECK(workspace && workspace->descendants().size() == 4);
        auto* floor = workspace->findFirstChild("Floor");
        CHECK(floor && std::get<bool>(floor->get("Anchored")));
        auto* door = workspace->findFirstChild("House") ? workspace->findFirstChild("House")->findFirstChild("Door")
                                                        : nullptr;
        CHECK(door);
        auto* weld = door ? door->findFirstChild("Weld") : nullptr;
        CHECK(weld && std::get<InstancePtr>(weld->get("Part0")).get() == floor);
        CHECK(weld && std::get<InstancePtr>(weld->get("Part1")).get() == door);
        CHECK(replica.game()->findService("ReplicatedStorage")->findFirstChild("Remotes")->children().size() == 1);
        // The services are the top containers: their ids resolve on both sides.
        auto* serverWorkspace = game->getService("Workspace");
        CHECK(replica.find(server.idOf(serverWorkspace)).get() == workspace);

        // An attribute change after the join.
        auto* serverFloor = serverWorkspace->findFirstChild("Floor");
        serverFloor->setAttribute("Speed", Value(3.5));
        serverFloor->setAttribute("Label", Value(std::string("fast")));
        for (const auto& m : server.takeChanges())
            CHECK(replica.handle(m));
        CHECK(toString(floor->getAttribute("Speed")) == toString(Value(3.5)));
        CHECK(std::get<std::string>(floor->getAttribute("Label")) == "fast");
        serverFloor->setAttribute("Label", Value());
        for (const auto& m : server.takeChanges())
            CHECK(replica.handle(m));
        CHECK(std::holds_alternative<std::monostate>(floor->getAttribute("Label")));
    }
}

TEST(net_joindata_describes_0x93_string_pairs)
{
    // u8 0x93, a skipped byte, u16 count, pairs of u16-length strings (ClientReplicator::vfn_175).
    const auto bytes = unhex("93" "00" "0200" "0100" "61" "0200" "6262" "0000" "0100" "63");
    Replica replica;
    Node tree("message");
    CHECK(replica.handle(bytes, &tree));
    const std::string text = render(tree);
    CHECK(text.find("string pairs (2") != std::string::npos);
    CHECK(text.find("\"a\" = \"bb\"") != std::string::npos);
    CHECK(text.find("\"\" = \"c\"") != std::string::npos);
}

TEST(net_joindata_item_writers_match_client)
{
    // emu/vectors/join_items.json: TagItem, InitialServerTimeItem and ISRTimestampItem
    // writers of RobloxPlayer 0.742 under unicorn (t_joinitems.py). Each entry is the
    // item's bytes inside an ID_DATA message.
    const std::filesystem::path path
        = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors" / "join_items.json";
    std::ifstream f(path);
    if (!f)
        return; // the research checkout isn't next to the repo
    std::stringstream ss;
    ss << f.rdbuf();
    const auto parsed = assets::json::parse(ss.str());
    CHECK(parsed.has_value());
    const assets::json::Value& doc = *parsed;
    const Schema schema = Schema::fromEngine();
    auto itemBytes = [&](const Item& item) {
        auto bytes = encodeData(std::span(&item, 1), schema, {});
        return std::vector<std::uint8_t>(bytes.begin() + 1, bytes.end() - 1); // without 0x83 and End
    };
    std::size_t checked = 0;
    for (const auto& c : *doc["tag"]->array()) {
        CHECK(itemBytes(TagItem{static_cast<std::uint32_t>(*c["tag"]->number())}) == unhex(*c["out"]->string()));
        ++checked;
    }
    for (const auto& c : *doc["servertime"]->array()) {
        // int64 values past 2^53 don't survive JSON numbers: the harness stores them as strings.
        CHECK(itemBytes(ServerTimeItem{std::stoll(*c["time"]->string())}) == unhex(*c["out"]->string()));
        ++checked;
    }
    CHECK(checked >= 18);
}

TEST(net_joindata_unknown_top_container_classes)
{
    // A real server names services the engine doesn't have: they become placeholders, not errors.
    Schema schema = Schema::fromEngine();
    schema.classes.push_back({"VoiceChatService", 0, 0, 0, 0, 0xffff});
    const auto unknownClass = static_cast<std::uint16_t>(schema.classes.size() - 1);
    Replica replica;
    CHECK(replica.handle(encodeSchemaPacket(schema)));
    SetGlobals g;
    g.peerId = 2;
    g.containers.push_back({{1, 2}, *schema.classId("Workspace"), true, false});
    g.containers.push_back({{1, 3}, unknownClass, true, false});
    g.containers.push_back({{1, 4}, *schema.classId("Part"), true, false}); // not a service either
    CHECK(replica.handle(encodeSetGlobals(g, replica.capabilities)));
    CHECK(replica.find({1, 2}).get() == replica.game()->findService("Workspace"));
    CHECK(replica.find({1, 3}) && replica.find({1, 3})->className() == "VoiceChatService");
    CHECK(replica.find({1, 3})->parent() == replica.game().get());
    CHECK(replica.find({1, 4}) && replica.find({1, 4})->className() == "Part");
    CHECK(replica.idOf(replica.game().get()) == std::nullopt);
    CHECK(replica.instanceCount() == 4); // the three containers and the root
}


TEST(net_joindata_parent_change)
{
    // Parent has no schema entry: the server re-parents with the property id
    // one past the last, the flag byte and the new parent's id [live].
    auto game = place();
    ReplicationServer server(game, {"Workspace", "ReplicatedStorage"});
    Replica replica;
    for (const auto& m : server.joinMessages(64, ReplicationServer::JoinStyle::JoinDataV2))
        CHECK(replica.handle(m));
    const Schema& schema = *replica.schema();
    auto* serverWorkspace = game->getService("Workspace");
    auto* serverDoor = serverWorkspace->findFirstChild("House")->findFirstChild("Door");
    const InstanceId door = server.idOf(serverDoor);
    const InstanceId workspace = server.idOf(serverWorkspace);

    ChangePropertyItem move;
    move.id = door;
    move.parent = workspace;
    const auto bytes = encodeData(std::vector<Item>{move}, schema, server.context());
    NetworkStream expected;
    expected.writeU8(0x83);
    expected.writeU8(0x03);
    writeInstanceId(expected, door);
    expected.writeU16(static_cast<std::uint16_t>(schema.properties.size()));
    expected.writeU8(0);
    writeInstanceId(expected, workspace);
    expected.writeU8(0x00);
    CHECK(std::ranges::equal(bytes, expected.bytes()));

    // A later item in the same message still applies.
    ChangePropertyItem rename;
    rename.id = door;
    rename.propertyId = *schema.propertyId(*schema.classId("Part"), "Name");
    rename.value = Value(std::string("FrontDoor"));
    CHECK(replica.handle(encodeData(std::vector<Item>{move, rename}, schema, server.context())));
    auto* ws = replica.game()->findService("Workspace");
    auto* moved = ws->findFirstChild("FrontDoor");
    CHECK(moved && moved->isA("Part"));
    CHECK(ws->findFirstChild("House")->children().empty());
    CHECK(moved && moved->findFirstChild("Weld"));

    // To a parent not seen yet: unparented until it arrives, and gone with it
    // when it is deleted first.
    move.parent = InstanceId{2, 0x4000};
    CHECK(replica.handle(encodeData(std::vector<Item>{move}, schema, server.context())));
    CHECK(moved && moved->parent() == nullptr);
    CHECK(replica.handle(encodeData(std::vector<Item>{DeleteItem{{2, 0x4000}}}, schema, server.context())));
    CHECK(moved && moved->findFirstChild("Weld") == nullptr);
}

TEST(net_joindata_reference_before_instance)
{
    // A Change Property can name an instance that comes later: the server
    // sends Player.Character ahead of the character model, in the same
    // message [capture]. The property is set once the instance arrives.
    auto game = place();
    auto* workspace = game->getService("Workspace");
    auto holder = ClassRegistry::create("ObjectValue");
    holder->setName("Holder");
    holder->setParent(workspace);
    ReplicationServer server(game, {"Workspace", "ReplicatedStorage"});
    Replica replica;
    for (const auto& m : server.joinMessages(64, ReplicationServer::JoinStyle::JoinDataV2))
        CHECK(replica.handle(m));
    const Schema& schema = *replica.schema();

    auto late = ClassRegistry::create("Part");
    late->setName("Late");
    late->setParent(workspace);
    const auto added = server.addInstance(late);
    ChangePropertyItem change;
    change.id = server.idOf(holder.get());
    change.propertyId = *schema.propertyId(*schema.classId("ObjectValue"), "Value");
    change.value = Value(late);
    Node tree("items");
    CHECK(replica.handle(encodeData(std::vector<Item>{change}, schema, server.context()), &tree));
    CHECK(render(tree).find("(not known yet)") != std::string::npos);
    auto* copy = replica.game()->findService("Workspace")->findFirstChild("Holder");
    CHECK(copy && toString(copy->get("Value")) == "nil");
    CHECK(replica.handle(added));
    CHECK(copy && toString(copy->get("Value")) == "Late");
}
