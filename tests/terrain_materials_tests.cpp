#include "framework.hpp"

// The client's terrain material settings (terrain/*.json in PlatformContent).

#include <lunar/assets/AssetCache.hpp>
#include <lunar/terrain/TerrainMaterials.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace lunar;
using namespace lunar::terrain;

namespace {

bool near(float a, float b)
{
    return std::abs(a - b) < 1e-4f;
}

} // namespace

TEST(terrain_materials_json_parses)
{
    // The shapes materials2022.json uses: per direction textures, one
    // texture for all, a material level blend.
    constexpr std::string_view json = R"({
        "platform": "pc",
        "atlas": {"pc": {"width": 2048, "tileSize": 128}},
        "materials": [
            {"name": "Air"},
            {"name": "Grass", "base_color": [185, 202, 87],
             "texture_top": {"tiling": 0.5, "detiling": 3.717, "rotation": 0.5,
                             "blend": {"min": 0.032, "max": 0.308, "scale": 0.603}},
             "texture_side": {"tiling": 0.25, "blend": {"min": 0.196, "max": 0.419, "scale": 0.611}}},
            {"name": "rock", "texture": {"tiling": 0.5, "blend": {"min": -0.096, "max": 0.297, "scale": 0.518}}},
            {"name": "Sand", "blend": {"min": 0, "max": 1, "scale": 1}, "texture_top": {"tiling": 1}},
            {"name": "Unobtainium", "texture": {"tiling": 9}}
        ]
    })";
    auto table = parseTerrainMaterials(json);
    CHECK(table.has_value());
    const TerrainMaterialParams& grass = table.value()[2];
    CHECK_EQ(grass.name, std::string("Grass"));
    CHECK(grass.baseColor == Color3::fromRGB(185, 202, 87));
    CHECK(near(grass.top.blendMax, 0.308f));
    CHECK(near(grass.top.rotation, 0.5f));
    CHECK(near(grass.top.studsPerTile(), 8.0f));
    CHECK(near(grass.side.studsPerTile(), 16.0f));
    CHECK(near(grass.bottom.blendMin, 0.196f)); // no bottom: the side's

    const TerrainMaterialParams& rock = table.value()[8];
    CHECK(near(rock.top.blendMin, -0.096f));
    CHECK(near(rock.side.blendScale, 0.518f));

    const TerrainMaterialParams& sand = table.value()[6];
    CHECK(near(sand.top.tiling, 1.0f));
    CHECK(near(sand.side.tiling, 0.5f)); // default

    CHECK(!parseTerrainMaterials("{\"materials\": [").has_value());
    CHECK(!parseTerrainMaterials("[]").has_value());
}

TEST(terrain_transitions_json_parses)
{
    constexpr std::string_view json = R"({"materials": [
        {"name": "glacier", "transition": {"dominance": 0.802, "noiseSize": 10, "materialEdgeBlur": 0}},
        {"name": "crackedlava", "texture": {"hasEmissiveMask": true, "emissiveStrength": 2},
         "transition": {"dominance": 0.899}}
    ]})";
    TerrainMaterialTable table{};
    CHECK(parseTerrainTransitions(json, table).has_value());
    CHECK(near(table[9].transition.dominance, 0.802f));
    CHECK(near(table[9].transition.materialEdgeBlur, 0.0f));
    CHECK(near(table[15].transition.emissiveStrength, 2.0f));
    CHECK(near(table[2].transition.noiseSize, 9.859f)); // untouched default
}

TEST(terrain_materials_from_the_client)
{
    // The real files, when the client's content is next to the source tree.
    const std::filesystem::path dir
        = assets::AssetConfig::fromEnvironment().contentDir.parent_path() / "PlatformContent/pc/terrain";
    std::ifstream file(dir / "materials2022.json", std::ios::binary);
    if (!file)
        SKIP("no client content");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto table = parseTerrainMaterials(text);
    CHECK(table.has_value());
    CHECK_EQ(table.value()[10].name, std::string("Snow"));
    CHECK(table.value()[10].baseColor == Color3::fromRGB(255, 255, 255));
    CHECK(near(table.value()[2].top.blendMin, 0.032f));
}
