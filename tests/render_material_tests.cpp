#include "framework.hpp"

// Built-in material texture slots: which TexturePack each material draws
// with, as RobloxPlayer 0.742 registers them.

#include <lunar/render/BuiltinMaterials.hpp>

#include <cstddef>
#include <optional>
#include <set>

using namespace lunar;
using namespace lunar::render;

TEST(builtin_material_slots_follow_the_client_order)
{
    // Spot checks of the slot table at 0x1066d1570.
    CHECK_EQ(builtinMaterialSlot(Enum::Material::Plastic), std::optional<std::size_t>{0});
    CHECK_EQ(builtinMaterialSlot(Enum::Material::Snow), std::optional<std::size_t>{2});
    CHECK_EQ(builtinMaterialSlot(Enum::Material::Brick), std::optional<std::size_t>{11});
    CHECK_EQ(builtinMaterialSlot(Enum::Material::Grass), std::optional<std::size_t>{17});
    CHECK_EQ(builtinMaterialSlot(Enum::Material::LeafyGrass), std::optional<std::size_t>{28});
    CHECK_EQ(builtinMaterialSlot(Enum::Material::Asphalt), std::optional<std::size_t>{34});
    CHECK_EQ(builtinMaterialSlot(Enum::Material::Rubber), std::optional<std::size_t>{42});
    // Air and Water have no slot.
    CHECK(!builtinMaterialSlot(Enum::Material::Air));
    CHECK(!builtinMaterialSlot(Enum::Material::Water));

    std::set<Enum::Material> seen;
    for (std::size_t slot = 0; slot < builtinMaterialSlots; ++slot) {
        CHECK(seen.insert(builtinMaterial(slot).material).second);
        CHECK_EQ(builtinMaterialSlot(builtinMaterial(slot).material), std::optional<std::size_t>{slot});
    }
}

TEST(builtin_material_packs_per_set)
{
    // The pack a material draws with in a set, 0 for none.
    auto pack = [](Enum::Material material, bool use2022) {
        const std::optional<BuiltinMaterialPack> found = builtinMaterialPack(material, use2022);
        return found ? found->pack : std::uint64_t{0};
    };
    // Grass: checked against the colour maps of both packs.
    CHECK_EQ(pack(Enum::Material::Grass, false), std::uint64_t{7547169285});
    CHECK_EQ(pack(Enum::Material::Grass, true), std::uint64_t{106637943980559});
    CHECK_EQ(builtinMaterialPack(Enum::Material::Grass, false).value_or(BuiltinMaterialPack{}).tiling, 2.0f);

    // The 2022 bricks keep their mortar's own colour.
    CHECK_EQ(pack(Enum::Material::Brick, true), std::uint64_t{85077012310177});
    CHECK(builtinMaterialPack(Enum::Material::Brick, true).value_or(BuiltinMaterialPack{}).tintMask);
    CHECK_EQ(pack(Enum::Material::Pavement, false), std::uint64_t{9873322398});
    CHECK_EQ(pack(Enum::Material::CrackedLava, true), std::uint64_t{121276091081821});

    // Plain materials draw without textures in either set.
    for (const Enum::Material plain :
         {Enum::Material::Plastic, Enum::Material::SmoothPlastic, Enum::Material::Neon, Enum::Material::ForceField}) {
        CHECK(!builtinMaterialPack(plain, false));
        CHECK(!builtinMaterialPack(plain, true));
    }
}

TEST(glass_parts_take_their_whole_colour)
{
    // Glass's packs say alphamode 2, but its maps are white with a clear
    // alpha and the client's glass shaders multiply them by the part colour:
    // The game's room key (Glass, a tan Color) draws tan, not white.
    for (const bool use2022 : {false, true}) {
        const std::optional<BuiltinMaterialPack> glass = builtinMaterialPack(Enum::Material::Glass, use2022);
        CHECK(glass.has_value());
        CHECK(glass.value_or(BuiltinMaterialPack{}).tintMask);
        CHECK(!partTintMask(Enum::Material::Glass, glass.value_or(BuiltinMaterialPack{})));
    }
    // The other materials keep their pack's mask.
    const BuiltinMaterialPack brick = builtinMaterialPack(Enum::Material::Brick, true).value_or(BuiltinMaterialPack{});
    CHECK(partTintMask(Enum::Material::Brick, brick));
    const BuiltinMaterialPack metal = builtinMaterialPack(Enum::Material::Metal, false).value_or(BuiltinMaterialPack{});
    CHECK(!partTintMask(Enum::Material::Metal, metal));
}
