#include "framework.hpp"

// Properties stored as binary blobs: attributes, tags, terrain voxels.

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace std::string_literals;

namespace {

// Builds AttributesSerialize blobs.
struct Blob {
    std::string out;
    Blob& u8(std::uint8_t v)
    {
        out += static_cast<char>(v);
        return *this;
    }
    Blob& u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Blob& f32(float v) { return u32(std::bit_cast<std::uint32_t>(v)); }
    Blob& f64(double v)
    {
        auto bits = std::bit_cast<std::uint64_t>(v);
        for (int i = 0; i < 8; ++i)
            u8(static_cast<std::uint8_t>(bits >> (8 * i)));
        return *this;
    }
    Blob& str(std::string_view s)
    {
        u32(static_cast<std::uint32_t>(s.size()));
        out += s;
        return *this;
    }
};

std::string base64(std::string_view bytes)
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk = 0;
        std::size_t n = std::min<std::size_t>(3, bytes.size() - i);
        for (std::size_t j = 0; j < 3; ++j)
            chunk = (chunk << 8) | (j < n ? static_cast<std::uint8_t>(bytes[i + j]) : 0u);
        for (std::size_t j = 0; j < 4; ++j)
            out += j <= n ? digits[(chunk >> (18 - 6 * j)) & 63] : '=';
    }
    return out;
}

std::string sampleAttributes()
{
    Blob b;
    b.u32(7);
    b.str("Label").u8(0x02).str("hello");
    b.str("On").u8(0x03).u8(1);
    b.str("Speed").u8(0x06).f64(16.5);
    b.str("Offset").u8(0x11).f32(1).f32(2).f32(3);
    b.str("Spot").u8(0x14).f32(4).f32(5).f32(6).u8(0x02); // identity rotation
    b.str("Look").u8(0x15).str("Material").u32(288);
    b.str("Tint").u8(0x0F).f32(1).f32(0.5f).f32(0);
    return b.out;
}

} // namespace

TEST(attributes_set_get_remove)
{
    Folder folder;
    std::vector<std::string> changes;
    folder.attributeChanged.connect([&](std::string_view name) { changes.emplace_back(name); });

    CHECK(std::holds_alternative<std::monostate>(folder.getAttribute("Missing")));
    folder.setAttribute("Score", 10.0);
    folder.setAttribute("Score", 10.0); // no change, no event
    folder.setAttribute("Owner", "me"s);
    CHECK(folder.getAttribute("Score") == Value(10.0));
    CHECK_EQ(folder.attributes().size(), 2u);
    folder.setAttribute("Score", Value());
    CHECK(std::holds_alternative<std::monostate>(folder.getAttribute("Score")));
    CHECK_EQ(changes, (std::vector<std::string>{"Score", "Owner", "Score"}));

    CHECK_THROWS(folder.setAttribute("has space", 1.0), Error);
    CHECK_THROWS(folder.setAttribute("", 1.0), Error);
    CHECK_THROWS(folder.setAttribute("Self", Value(std::make_shared<Folder>())), Error);
}

TEST(tags_add_remove)
{
    Part part;
    part.addTag("Lava");
    part.addTag("Lava");
    part.addTag("Hot");
    CHECK(part.hasTag("Lava"));
    CHECK_EQ(part.tags(), (std::vector<std::string>{"Lava", "Hot"}));
    part.removeTag("Lava");
    CHECK(!part.hasTag("Lava"));
}

TEST(clone_copies_attributes_and_tags)
{
    auto part = std::make_shared<Part>();
    part->setAttribute("Damage", 5.0);
    part->addTag("Weapon");
    InstancePtr copy = part->clone();
    CHECK(copy->getAttribute("Damage") == Value(5.0));
    CHECK(copy->hasTag("Weapon"));
}

TEST(xml_files_carry_attributes_and_tags)
{
    std::string xml = R"(<roblox version="4"><Item class="Folder" referent="A"><Properties>
        <BinaryString name="AttributesSerialize">)"
                      + base64(sampleAttributes()) + R"(</BinaryString>
        <BinaryString name="Tags">cmVkAGJsdWU=</BinaryString>
        </Properties></Item></roblox>)";
    auto model = loadModel(xml);
    CHECK(model.has_value());
    CHECK(model->warnings.empty());
    const Instance& folder = *model->roots[0];
    CHECK(folder.getAttribute("Label") == Value("hello"s));
    CHECK(folder.getAttribute("On") == Value(true));
    CHECK(folder.getAttribute("Speed") == Value(16.5));
    CHECK(folder.getAttribute("Offset") == Value(Vector3{1, 2, 3}));
    CHECK(folder.getAttribute("Spot") == Value(CFrame(4, 5, 6)));
    CHECK(folder.getAttribute("Look") == Value(enums::item(Enum::Material::Neon)));
    CHECK(folder.getAttribute("Tint") == Value(Color3{1, 0.5f, 0}));
    CHECK_EQ(folder.tags(), (std::vector<std::string>{"red", "blue"}));
}

TEST(network_instances_carry_attributes_as_attributes_replicate)
{
    auto folder = std::make_shared<Folder>();
    const Property* property = findSerializedProperty(*folder, "AttributesReplicate");
    CHECK(property != nullptr);
    property->set(*folder, Value(sampleAttributes()));
    CHECK(folder->getAttribute("Label") == Value("hello"s));
    CHECK(folder->getAttribute("Speed") == Value(16.5));
}

TEST(bad_attribute_data_is_a_warning)
{
    std::string truncated = sampleAttributes();
    truncated.resize(truncated.size() - 3);
    std::string xml = R"(<roblox version="4"><Item class="Folder"><Properties>
        <BinaryString name="AttributesSerialize">)"
                      + base64(truncated) + R"(</BinaryString></Properties></Item></roblox>)";
    auto model = loadModel(xml);
    CHECK(model.has_value());
    CHECK_EQ(model->warnings, (std::vector<std::string>{"Folder.AttributesSerialize: attribute data is truncated"}));
}

namespace {

// A SmoothGrid chunk header: coordinate delta as big-endian int32 byte planes.
void chunkDelta(Blob& b, int x, int y, int z)
{
    std::array<std::uint32_t, 3> axes{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                      static_cast<std::uint32_t>(z)};
    for (int plane = 3; plane >= 0; --plane)
        for (std::uint32_t v : axes)
            b.u8(static_cast<std::uint8_t>(v >> (8 * plane)));
}

void airCells(Blob& b, std::size_t count)
{
    for (; count > 0;) {
        std::size_t n = std::min<std::size_t>(count, 256);
        b.u8(0x80).u8(static_cast<std::uint8_t>(n - 1));
        count -= n;
    }
}

} // namespace

TEST(terrain_voxels_and_colors_load)
{
    constexpr std::size_t cells = std::size_t{32} * 32 * 32;
    Blob grid;
    grid.u8(1).u8(5);
    chunkDelta(grid, 1, -2, 3);
    grid.u8(0xC2).u8(12).u8(0).u8(100); // Grass, occupancy 12, one cell, water 100
    airCells(grid, 31);
    grid.u8(19); // LeafyGrass, full: cell 32 is x 0, z 1, y 0
    airCells(grid, cells - 33);
    chunkDelta(grid, 1, 0, 0); // an all-air chunk at (2, -2, 3), dropped
    airCells(grid, cells);

    std::string colors(6, '\0');
    for (int i = 2; i < 23; ++i)
        colors += std::string{static_cast<char>(i), 0, static_cast<char>(255)};

    std::string xml = R"(<roblox version="4"><Item class="Workspace"><Item class="Terrain"><Properties>
        <BinaryString name="SmoothGrid">)"
                      + base64(grid.out) + R"(</BinaryString>
        <BinaryString name="MaterialColors">)"
                      + base64(colors) + R"(</BinaryString>
        </Properties></Item></Item></roblox>)";
    auto place = loadPlace(xml);
    CHECK(place.has_value());
    CHECK(place->warnings.empty());
    Terrain* terrain = place->game->workspace()->terrain();
    CHECK(terrain != nullptr);
    CHECK_EQ(terrain->chunks().size(), 1u);
    CHECK(terrain->chunks()[0].position == (std::array<int, 3>{1, -2, 3}));

    TerrainVoxel grass = terrain->voxelAt(32, -64, 96);
    CHECK(grass.material == Enum::Material::Grass);
    CHECK_EQ(grass.occupancy, 12);
    CHECK_EQ(grass.water, 100);
    CHECK_EQ(grass.occupancyFraction(), 13.0f / 256.0f);
    TerrainVoxel leafy = terrain->voxelAt(32, -64, 97);
    CHECK(leafy.material == Enum::Material::LeafyGrass);
    CHECK_EQ(leafy.occupancy, 255);
    CHECK(terrain->voxelAt(33, -64, 96) == TerrainVoxel{});
    CHECK(terrain->voxelAt(64, -64, 96) == TerrainVoxel{}); // the dropped air chunk
    CHECK(terrain->voxelAt(-1, -1, -1) == TerrainVoxel{});

    CHECK(terrain->getMaterialColor(Enum::Material::Grass) == Color3::fromRGB(2, 0, 255));
    CHECK(terrain->getMaterialColor(Enum::Material::Pavement) == Color3::fromRGB(22, 0, 255));
    CHECK_THROWS(terrain->getMaterialColor(Enum::Material::Neon), Error);
    CHECK_THROWS(terrain->setMaterialColor(Enum::Material::Water, Color3{}), Error);
}

TEST(bad_terrain_data_is_a_warning)
{
    Blob grid;
    grid.u8(1).u8(5);
    chunkDelta(grid, 0, 0, 0);
    grid.u8(0x3F); // material index 63
    std::string xml = R"(<roblox version="4"><Item class="Terrain"><Properties>
        <BinaryString name="SmoothGrid">)"
                      + base64(grid.out) + R"(</BinaryString></Properties></Item></roblox>)";
    auto model = loadModel(xml);
    CHECK_EQ(model->warnings,
             (std::vector<std::string>{"Terrain.SmoothGrid: SmoothGrid material index 63 isn't known"}));
}
