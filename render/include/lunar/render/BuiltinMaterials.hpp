#pragma once

#include <lunar/Enums.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace lunar::render {

// Roblox's built-in material TexturePacks, as RobloxPlayer 0.742 registers
// them. The client keeps one table of 43 slots per material set (pre-2022 and
// 2022; sub_104030da1 fills them, 0x48 bytes a slot), and slot i holds the
// material whose Enum.Material value is entry i of the uint32 table at
// 0x1066d1570, the table sub_10403019c walks to turn a material into a slot.
// That order is neither Enum.Material's nor the 45-entry list at 0x1066f4850.
//
// Each slot names a TexturePack (an XML list of colour, normal, roughness
// and metalness map ids). The normal map ids below were read from those
// packs; the packs themselves are what the colour maps are loaded through.
// Every pack's colour map was looked at to check the material it belongs to.
struct BuiltinMaterialPack {
    std::uint64_t pack = 0;   // TexturePack asset id; 0 when the slot has none
    std::uint64_t normal = 0; // the pack's normal map
    float tiling = 1.0f;      // the pack's <tiling>: repeats per material tile
    bool tintMask = false;    // <alphamode>2</alphamode>: alpha says where the part colour applies
};

struct BuiltinMaterial {
    Enum::Material material = Enum::Material::Plastic;
    BuiltinMaterialPack pre2022; // MaterialService.Use2022Materials off
    BuiltinMaterialPack v2022;   // ...and on
};

inline constexpr std::size_t builtinMaterialSlots = 43;

// The slot a material's textures sit in, if it has one.
std::optional<std::size_t> builtinMaterialSlot(Enum::Material material);

// The packs for a slot. Plastic, SmoothPlastic, Neon and ForceField have none.
const BuiltinMaterial& builtinMaterial(std::size_t slot); // slot < builtinMaterialSlots

// The pack a material draws with in the given set, if any.
std::optional<BuiltinMaterialPack> builtinMaterialPack(Enum::Material material, bool use2022);

// Whether a part of this material lets its pack's colour map alpha mask the
// part colour (the pack's tintMask), or multiplies the whole map by it. Glass
// draws with its own shaders (DefaultUnifiedGlass*FS), which take the colour
// map times the part colour and its alpha only as opacity, whatever the
// pack's alphamode: its maps are white with a clear alpha, so as a tint mask
// every glass part would draw white.
bool partTintMask(Enum::Material material, const BuiltinMaterialPack& pack);

} // namespace lunar::render
