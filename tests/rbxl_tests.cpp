#include "framework.hpp"

#include <lunar/Rbxl.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace std::string_literals;

namespace {

// --- A tiny writer for the binary format ----------------------------------------

struct Writer {
    std::string out;

    Writer& u8(std::uint8_t v)
    {
        out += static_cast<char>(v);
        return *this;
    }
    Writer& u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8)); }
    Writer& i32(std::int32_t v) { return u32(static_cast<std::uint32_t>(v)); }
    Writer& f32(float v) { return u32(std::bit_cast<std::uint32_t>(v)); }
    Writer& raw(std::string_view s)
    {
        out += s;
        return *this;
    }
    Writer& str(std::string_view s) { return u32(static_cast<std::uint32_t>(s.size())).raw(s); }

    // Big-endian byte planes, the way the format stores number arrays.
    template <class U>
    Writer& interleaved(const std::vector<U>& values)
    {
        for (int plane = static_cast<int>(sizeof(U)) - 1; plane >= 0; --plane)
            for (U v : values)
                u8(static_cast<std::uint8_t>(v >> (8 * plane)));
        return *this;
    }
    Writer& ints(std::initializer_list<std::int32_t> values)
    {
        std::vector<std::uint32_t> encoded;
        for (std::int32_t v : values)
            encoded.push_back((static_cast<std::uint32_t>(v) << 1) ^ static_cast<std::uint32_t>(v >> 31));
        return interleaved(encoded);
    }
    Writer& floats(std::initializer_list<float> values)
    {
        std::vector<std::uint32_t> encoded;
        for (float v : values)
            encoded.push_back(std::rotl(std::bit_cast<std::uint32_t>(v), 1));
        return interleaved(encoded);
    }
    Writer& refs(std::initializer_list<std::int32_t> ids)
    {
        std::vector<std::uint32_t> encoded;
        std::int32_t previous = 0;
        for (std::int32_t id : ids) {
            std::int32_t delta = id - previous;
            previous = id;
            encoded.push_back((static_cast<std::uint32_t>(delta) << 1) ^ static_cast<std::uint32_t>(delta >> 31));
        }
        return interleaved(encoded);
    }
};

std::string header(std::int32_t classes, std::int32_t instances)
{
    return Writer{}
        .raw("<roblox!\x89\xff\r\n\x1a\n"s)
        .u8(0)
        .u8(0)
        .i32(classes)
        .i32(instances)
        .raw(std::string(8, '\0'))
        .out;
}

// A chunk stored uncompressed, or as one literal-only LZ4 sequence.
std::string chunk(std::string_view name, const Writer& body, bool compressed = false)
{
    Writer w;
    w.raw(name);
    if (name.size() == 3)
        w.u8(0);
    if (!compressed)
        return w.u32(0).u32(static_cast<std::uint32_t>(body.out.size())).u32(0).raw(body.out).out;

    Writer block;
    std::size_t n = body.out.size();
    block.u8(static_cast<std::uint8_t>(std::min<std::size_t>(n, 15) << 4));
    if (n >= 15) {
        for (n -= 15; n >= 255; n -= 255)
            block.u8(255);
        block.u8(static_cast<std::uint8_t>(n));
    }
    block.raw(body.out);
    return w.u32(static_cast<std::uint32_t>(block.out.size()))
        .u32(static_cast<std::uint32_t>(body.out.size()))
        .u32(0)
        .raw(block.out)
        .out;
}

std::string inst(std::int32_t id, std::string_view className, std::initializer_list<std::int32_t> refs,
                 bool service = false)
{
    Writer w;
    w.i32(id).str(className).u8(service ? 1 : 0).u32(static_cast<std::uint32_t>(refs.size())).refs(refs);
    if (service)
        for (std::size_t i = 0; i < refs.size(); ++i)
            w.u8(1);
    return chunk("INST", w);
}

Writer column(std::int32_t classId, std::string_view name, std::uint8_t type)
{
    Writer w;
    w.i32(classId).str(name).u8(type);
    return w;
}

std::string end()
{
    return chunk("END", Writer{}.raw("</roblox>"));
}

// A ZSTD frame holding `data` as one raw block, as small as the format
// allows (single segment, a one-byte content size, no checksum).
std::string zstdFrame(std::string_view data)
{
    const auto block = static_cast<std::uint32_t>(1 | data.size() << 3); // last, raw, its size
    return Writer{}
        .raw("\x28\xb5\x2f\xfd"s)
        .u8(0x20)
        .u8(static_cast<std::uint8_t>(data.size()))
        .u8(static_cast<std::uint8_t>(block))
        .u8(static_cast<std::uint8_t>(block >> 8))
        .u8(static_cast<std::uint8_t>(block >> 16))
        .raw(data)
        .out;
}

// Three parts and a folder in a model.
std::string sampleModel()
{
    constexpr float h = 0.0f;
    std::string file = header(3, 5);
    file += chunk("META", Writer{}.u32(1).str("ExplicitAutoJoints").str("true"));
    file += inst(0, "Part", {0, 1, 4});
    file += inst(1, "Model", {2});
    file += inst(2, "Folder", {3});
    file += chunk("PROP", column(0, "Name", 0x01).str("Base").str("Top").str("Tilted"));
    file += chunk("PROP", column(0, "Anchored", 0x02).u8(1).u8(0).u8(1), true);
    file += chunk("PROP", column(0, "size", 0x0E).floats({8, 2, 1}).floats({1, 2, 1}).floats({8, 2, 1}));
    // Identity by id, a raw matrix (90 degrees about Y), then basic rotation id 0x03.
    Writer cframes = column(0, "CFrame", 0x10);
    cframes.u8(0x02).u8(0x00);
    for (float v : {h, h, 1.0f, h, 1.0f, h, -1.0f, h, h})
        cframes.f32(v);
    cframes.u8(0x03).floats({0, 0, 5}).floats({0.5f, 10, 0}).floats({0, 0, -3});
    file += chunk("PROP", cframes, true);
    file += chunk("PROP", column(0, "Transparency", 0x04).floats({0.25f, 0, -0.5f}));
    file
        += chunk("PROP", column(0, "Color3uint8", 0x1A).raw("\xFF\x00\x0A"s).raw("\x00\x80\x14"s).raw("\x00\xFF\x1E"s));
    file += chunk("PROP", column(0, "Material", 0x12).interleaved(std::vector<std::uint32_t>{256, 256, 1088}));
    file += chunk("PROP", column(0, "Health", 0x03).ints({-1, 0, 300}));
    file += chunk("PROP", column(0, "Big", 0x1B).interleaved(std::vector<std::uint64_t>{3, 0, 0x2000000000}));
    file += chunk("PROP", column(1, "Name", 0x01).str("Tower"));
    file += chunk("PROP", column(1, "PrimaryPart", 0x13).refs({1}));
    file += chunk("PROP", column(2, "Name", 0x08).raw("whatever"));
    file += chunk("PRNT", Writer{}.u8(0).u32(5).refs({0, 1, 4, 2, 3}).refs({2, 2, 2, -1, -1}));
    file += end();
    return file;
}

std::string samplePlace()
{
    std::string file = header(3, 3);
    file += inst(0, "Workspace", {0}, true);
    file += inst(1, "Part", {1});
    file += inst(2, "Lighting", {2}, true);
    file += chunk("PROP", column(0, "Name", 0x01).str("Workspace"));
    file += chunk("PROP", column(1, "Name", 0x01).str("Floor"));
    file += chunk("PROP", column(2, "Name", 0x01).str("Lighting"));
    file += chunk("PRNT", Writer{}.u8(0).u32(3).refs({1, 0, 2}).refs({0, -1, -1}));
    file += end();
    return file;
}

// Two folders with a column of every newer data type.
std::string typesModel()
{
    std::string file = header(1, 2);
    file += chunk(
        "SSTR", Writer{}.u32(0).u32(2).raw(std::string(16, 'h')).str("first").raw(std::string(16, 'i')).str("second"));
    file += inst(0, "Folder", {0, 1});
    file += chunk("PROP", column(0, "UDim", 0x06).floats({0.5f, 1}).ints({-3, 7}));
    file += chunk("PROP", column(0, "UDim2", 0x07).floats({1, 2}).floats({3, 4}).ints({5, 6}).ints({7, 8}));
    file += chunk("PROP", column(0, "Faces", 0x09).u8(0b000001).u8(0b111111));
    file += chunk("PROP", column(0, "Axes", 0x0A).u8(1).u8(6));
    file += chunk("PROP", column(0, "BrickColor", 0x0B).interleaved(std::vector<std::uint32_t>{21, 4}));
    file += chunk("PROP", column(0, "Vector2", 0x0D).floats({1, 2}).floats({3, 4}));
    file += chunk("PROP", column(0, "Int16", 0x14).u16(1).u16(2).u16(3).u16(0xFFFF).u16(0).u16(0));
    Writer sequence = column(0, "NumberSequence", 0x15);
    sequence.u32(2).f32(0).f32(1).f32(0).f32(1).f32(2).f32(0.5f);
    sequence.u32(2).f32(0).f32(5).f32(0).f32(1).f32(5).f32(0);
    file += chunk("PROP", sequence);
    Writer colors = column(0, "ColorSequence", 0x16);
    for (int i = 0; i < 2; ++i)
        colors.u32(2).f32(0).f32(1).f32(0).f32(0).f32(0).f32(1).f32(0).f32(0).f32(1).f32(0);
    file += chunk("PROP", colors);
    file += chunk("PROP", column(0, "NumberRange", 0x17).f32(1).f32(2).f32(-1).f32(1));
    file += chunk("PROP", column(0, "Rect", 0x18).floats({0, 1}).floats({0, 1}).floats({10, 11}).floats({20, 21}));
    file += chunk("PROP", column(0, "Physics", 0x19).u8(0).u8(1).f32(2).f32(0.1f).f32(0.2f).f32(3).f32(4));
    file += chunk("PROP", column(0, "Shared", 0x1C).interleaved(std::vector<std::uint32_t>{1, 0}));
    Writer pivot = column(0, "Pivot", 0x1E);
    pivot.u8(0x10).u8(0x02).u8(0x02).floats({1, 0}).floats({2, 0}).floats({3, 0}).u8(0x02).u8(1).u8(0);
    file += chunk("PROP", pivot);
    file += chunk("PROP", column(0, "Font", 0x20)
                              .str("rbxasset://a.json")
                              .u16(700)
                              .u8(1)
                              .str("")
                              .str("rbxasset://b.json")
                              .u16(400)
                              .u8(0)
                              .str("cached"));
    file += chunk("PROP", column(0, "Content", 0x22).ints({1, 0}).u32(1).str("rbxassetid://5").u32(0).u32(0));
    // Two UniqueIds, one byte plane at a time: 00 01 .. 0f and ff 00 00 01 12 34 56 78 80 00 .. 01.
    Writer unique = column(0, "UniqueId", 0x1F);
    for (std::uint8_t b : std::initializer_list<std::uint8_t>{
             0x00, 0xff, 0x01, 0x00, 0x02, 0x00, 0x03, 0x01, 0x04, 0x12, 0x05, 0x34, 0x06, 0x56, 0x07, 0x78,
             0x08, 0x80, 0x09, 0x00, 0x0a, 0x00, 0x0b, 0x00, 0x0c, 0x00, 0x0d, 0x00, 0x0e, 0x00, 0x0f, 0x01})
        unique.u8(b);
    file += chunk("PROP", unique);
    file += chunk("PRNT", Writer{}.u8(0).u32(2).refs({0, 1}).refs({-1, -1}));
    file += end();
    return file;
}

bool hasWarning(const std::vector<std::string>& warnings, std::string_view text)
{
    return std::ranges::find(warnings, text) != warnings.end();
}

bool near(Vector3 a, Vector3 b)
{
    return std::abs(a.x - b.x) < 1e-6f && std::abs(a.y - b.y) < 1e-6f && std::abs(a.z - b.z) < 1e-6f;
}

} // namespace

TEST(rbxl_parses_chunks)
{
    auto doc = rbxl::parse(sampleModel());
    CHECK(doc.has_value());
    CHECK_EQ(doc->instanceCount, 5u);
    CHECK_EQ(doc->metadata.size(), 1u);
    CHECK_EQ(doc->metadata[0].first, "ExplicitAutoJoints");
    CHECK_EQ(doc->classes.size(), 3u);
    CHECK_EQ(doc->classes[0].name, "Part");
    CHECK_EQ(doc->classes[0].referents, (std::vector<std::int32_t>{0, 1, 4}));
    CHECK_EQ(doc->parents.size(), 5u);
    CHECK(doc->parents[3] == (std::pair<std::int32_t, std::int32_t>{2, -1}));
}

TEST(rbxl_decodes_numbers)
{
    auto doc = rbxl::parse(sampleModel());
    const auto& part = doc->classes[0];
    auto column
        = [&](std::string_view name) { return *std::ranges::find(part.properties, name, &rbxl::PropertyColumn::name); };
    CHECK(column("Health").values == (std::vector<Value>{-1.0, 0.0, 300.0}));
    CHECK(column("Big").values == (std::vector<Value>{-2.0, 0.0, 68719476736.0}));
    CHECK(column("Material").values == (std::vector<Value>{256.0, 256.0, 1088.0}));
    CHECK(column("Transparency").values == (std::vector<Value>{0.25, 0.0, -0.5}));
    CHECK(!doc->classes[2].properties[0].decoded); // type 0x08, Ray
}

TEST(rbxl_decodes_data_types)
{
    auto doc = rbxl::parse(typesModel());
    CHECK(doc.has_value());
    const auto& folder = doc->classes[0];
    auto values = [&](std::string_view name) {
        const auto& column = *std::ranges::find(folder.properties, name, &rbxl::PropertyColumn::name);
        CHECK(column.decoded);
        return column.values;
    };
    CHECK(values("UDim") == (std::vector<Value>{UDim{0.5f, -3}, UDim{1, 7}}));
    CHECK(values("UDim2") == (std::vector<Value>{UDim2{{1, 5}, {3, 7}}, UDim2{{2, 6}, {4, 8}}}));
    CHECK(values("Faces") == (std::vector<Value>{Faces{.right = true}, Faces::fromBits(63)}));
    CHECK(values("Axes") == (std::vector<Value>{Axes{.x = true}, Axes{.y = true, .z = true}}));
    CHECK(values("BrickColor") == (std::vector<Value>{BrickColor{21}, BrickColor{194}}));
    CHECK(values("Vector2") == (std::vector<Value>{Vector2{1, 3}, Vector2{2, 4}}));
    CHECK(values("Int16") == (std::vector<Value>{Vector3int16{1, 2, 3}, Vector3int16{-1, 0, 0}}));
    CHECK(values("NumberSequence")
          == (std::vector<Value>{NumberSequence({{0, 1, 0}, {1, 2, 0.5f}}), NumberSequence(5.0f)}));
    CHECK(values("ColorSequence")[1] == Value(ColorSequence({{0, {1, 0, 0}}, {1, {0, 0, 1}}})));
    CHECK(values("NumberRange") == (std::vector<Value>{NumberRange{1, 2}, NumberRange{-1, 1}}));
    CHECK(values("Rect") == (std::vector<Value>{Rect{{0, 0}, {10, 20}}, Rect{{1, 1}, {11, 21}}}));
    CHECK(values("Physics") == (std::vector<Value>{Value(), PhysicalProperties{2, 0.1f, 0.2f, 3, 4}}));
    // Shared strings stay indices into the document's one copy of each.
    const auto& shared
        = std::ranges::find(folder.properties, std::string_view("Shared"), &rbxl::PropertyColumn::name)->shared;
    CHECK(shared == (std::vector<std::uint32_t>{1, 0}));
    CHECK(*doc->sharedStrings[1] == "second");
    CHECK(values("Pivot") == (std::vector<Value>{CFrame(1, 2, 3), Value()}));
    CHECK(values("Font")[0] == Value(Font{"rbxasset://a.json", Enum::FontWeight::Bold, Enum::FontStyle::Italic}));
    CHECK(values("Font")[1] == Value(Font{"rbxasset://b.json"}));
    CHECK(values("Content") == (std::vector<Value>{"rbxassetid://5"s, ""s}));
    // UniqueIds read as the XML files write them: random (rotated right by one), time, index.
    CHECK(values("UniqueId")
          == (std::vector<Value>{"84048505860687070405060700010203"s, "c00000000000000012345678ff000001"s}));
}

TEST(rbxl_loads_a_model)
{
    auto model = rbxl::loadModel(sampleModel());
    CHECK(model.has_value());
    CHECK_EQ(model->roots.size(), 2u);
    CHECK_EQ(model->roots[0]->name(), "Tower");
    CHECK_EQ(model->roots[1]->className(), "Folder");

    auto* tower = dynamic_cast<Model*>(model->roots[0].get());
    CHECK(tower != nullptr);
    CHECK_EQ(tower->children().size(), 3u);
    auto* base = dynamic_cast<Part*>(tower->findFirstChild("Base"));
    auto* top = dynamic_cast<Part*>(tower->findFirstChild("Top"));
    auto* tilted = dynamic_cast<Part*>(tower->findFirstChild("Tilted"));
    CHECK(base && top && tilted);
    CHECK(tower->primaryPart() == top);

    CHECK(base->anchored());
    CHECK(!top->anchored());
    CHECK(base->size() == (Vector3{8, 1, 8}));
    CHECK_EQ(base->transparency(), 0.25f);
    CHECK_EQ(tilted->transparency(), -0.5f);
    CHECK(base->color() == (Color3{1, 0, 0}));
    CHECK(top->color() == Color3::fromRGB(0, 128, 255));

    CHECK(base->cframe() == CFrame(0, 0.5f, 0));
    CHECK(top->position() == (Vector3{0, 10, 0}));
    CHECK(near(top->cframe().lookVector(), {-1, 0, 0}));
    // id 0x03: columns +X, +Z, -Y, so it looks up.
    CHECK(tilted->position() == (Vector3{5, 0, -3}));
    CHECK(near(tilted->cframe().rightVector(), {1, 0, 0}));
    CHECK(near(tilted->cframe().upVector(), {0, 0, 1}));
    CHECK(near(tilted->cframe().lookVector(), {0, 1, 0}));
}

TEST(rbxl_reports_skipped_things)
{
    auto model = rbxl::loadModel(sampleModel());
    const auto& warnings = model->warnings;
    CHECK(hasWarning(warnings, "Part.Health isn't supported yet"));
    CHECK(hasWarning(warnings, "Folder.Name: binary type 0x08 isn't supported yet"));
    CHECK_EQ(warnings.size(), 3u); // plus Part.Big
}

TEST(rbxl_loads_a_place)
{
    auto place = rbxl::loadPlace(samplePlace());
    CHECK(place.has_value());
    auto& game = *place->game;
    CHECK_EQ(game.children().size(), 2u);
    Workspace* workspace = game.workspace();
    CHECK(workspace->findFirstChild("Floor") != nullptr);
    CHECK(game.findFirstChild("Lighting") == game.findService("Lighting"));
    CHECK(dynamic_cast<Lighting*>(game.findFirstChild("Lighting")) != nullptr);
    CHECK_THROWS(workspace->setParent(nullptr), Error);
    CHECK(place->warnings.empty());
}

TEST(load_place_picks_the_format)
{
    auto binary = loadPlace(samplePlace());
    CHECK(binary.has_value());
    CHECK(binary->game->workspace()->findFirstChild("Floor") != nullptr);

    auto xml = loadPlace(R"(<roblox version="4"><Item class="Workspace"><Item class="Part"/></Item></roblox>)");
    CHECK(xml.has_value());
    CHECK(xml->game->workspace()->findFirstChild("Part") != nullptr);
}

// The client's own models (UniversalApp.rbxm) compress their chunks with
// ZSTD, and keep a script's Source compiled as a column of type 0x1D.
TEST(rbxl_reads_zstd_chunks_and_source_bytecode)
{
    const std::string meta = Writer{}.u32(1).str("ExplicitAutoJoints").str("true").out;
    const std::string frame = zstdFrame(meta);
    const std::string bytecode = "\x52\x7a\x10\x4a\x5b\x32\x09\xe0\x09\x03"s;
    std::string file = header(1, 1)
                       + Writer{}
                             .raw("META")
                             .u32(static_cast<std::uint32_t>(frame.size()))
                             .u32(static_cast<std::uint32_t>(meta.size()))
                             .u32(0)
                             .raw(frame)
                             .out
                       + inst(0, "ModuleScript", {0}) + chunk("PROP", column(0, "Source", 0x1D).str(bytecode))
                       + end();
    auto doc = rbxl::parse(file);
    CHECK(doc.has_value());
    CHECK_EQ(doc->metadata.size(), 1u);
    CHECK_EQ(doc->metadata[0].second, "true");
    auto model = loadModel(file);
    CHECK(model.has_value());
    CHECK_EQ(model->roots.size(), 1u);
    CHECK(model->roots[0]->get("Source") == Value(bytecode));
}

TEST(rbxl_rejects_bad_files)
{
    CHECK_EQ(rbxl::parse("<roblox>").error(), "not a binary Roblox file");
    CHECK_EQ(rbxl::parse("<roblox!\x89\xff\r\n\x1a\n\x01\x00"s + std::string(16, '\0')).error(),
             "unsupported format version 1");

    std::string truncated = sampleModel();
    truncated.resize(truncated.size() / 2);
    CHECK(!rbxl::parse(truncated).has_value());

    std::string noEnd = header(0, 0);
    CHECK_EQ(rbxl::parse(noEnd).error(), "missing END chunk");

    std::string zstd = header(0, 0) + Writer{}.raw("META").u32(4).u32(10).u32(0).raw("\x28\xb5\x2f\xfd"s).out;
    CHECK_EQ(rbxl::parse(zstd).error(), "META chunk: zstd: Src size is incorrect");

    std::string badLz4 = header(0, 0) + Writer{}.raw("META").u32(4).u32(10).u32(0).raw("\x10" "a" "\x09\x00"s).out;
    CHECK_EQ(rbxl::parse(badLz4).error(), "META chunk: match offset 9 points before the start");

    std::string badRotation = header(1, 1) + inst(0, "Part", {0})
                              + chunk("PROP", column(0, "CFrame", 0x10).u8(0x04).floats({0}).floats({0}).floats({0}))
                              + end();
    auto rotationDoc = rbxl::parse(badRotation);
    CHECK(rotationDoc.has_value()); // a bad column only loses that property
    CHECK(!rotationDoc->classes[0].properties[0].decoded);
    CHECK_EQ(rotationDoc->classes[0].properties[0].error, "bad CFrame rotation id 0x04");
    CHECK(hasWarning(rbxl::loadModel(badRotation)->warnings, "Part.CFrame: bad CFrame rotation id 0x04"));

    std::string orphanProp = header(0, 0) + chunk("PROP", column(7, "Name", 0x01)) + end();
    CHECK_EQ(rbxl::parse(orphanProp).error(), "PROP chunk for unknown class id 7");

    std::string badReferent = header(1, 1) + inst(0, "Part", {5}) + end();
    CHECK_EQ(rbxl::loadModel(badReferent).error(), "bad referent 5 for class Part");
}

TEST(rbxl_shares_collision_blobs)
{
    // Three MeshParts, two pointing at one shared string: they share the
    // bytes instead of each getting a copy. PhysicsData is a plain string.
    std::string file = header(1, 3);
    file += chunk("SSTR", Writer{}
                              .u32(0)
                              .u32(2)
                              .raw(std::string(16, 'a'))
                              .str("CSGPHS\0\0\0\0BLOCK"s)
                              .raw(std::string(16, 'b'))
                              .str("CSGPHS\x06\0\0\0"s));
    file += inst(0, "MeshPart", {0, 1, 2});
    file += chunk("PROP", column(0, "PhysicalConfigData", 0x1C).interleaved(std::vector<std::uint32_t>{0, 1, 0}));
    file += chunk("PROP", column(0, "PhysicsData", 0x01).str("").str("").str("legacy"));
    file += chunk("PRNT", Writer{}.u8(0).u32(3).refs({0, 1, 2}).refs({-1, -1, -1}));
    file += end();
    auto model = rbxl::loadModel(file);
    CHECK(model.has_value());
    CHECK(model->warnings.empty());
    CHECK_EQ(model->roots.size(), 3u);
    auto blob = [&](std::size_t i) { return static_cast<const MeshPart&>(*model->roots[i]).physicalConfigData(); };
    CHECK(blob(0) && blob(1) && blob(2));
    CHECK(blob(0) == blob(2)); // the same bytes, not equal copies
    CHECK(blob(0) != blob(1));
    CHECK(*blob(0) == "CSGPHS\0\0\0\0BLOCK"s);
    CHECK(*static_cast<const MeshPart&>(*model->roots[2]).physicsData() == "legacy");
    // Copies made by Clone keep pointing at the same blob.
    auto copy = model->roots[0]->clone();
    CHECK(static_cast<const MeshPart&>(*copy).physicalConfigData() == blob(0));
}
