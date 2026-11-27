#pragma once

#include <lunar/Math.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lunar/Value.hpp>
#include <lunar/classes/World.hpp>

namespace lunar::detail {

// Axis-aligned rotations are stored as one byte by the binary format and the
// attribute format alike. Null for ids that aren't valid rotations.
std::optional<CFrame> basicRotation(std::uint8_t id);

// The AttributesSerialize blob both file formats use, in file order.
std::expected<std::vector<std::pair<std::string, Value>>, std::string> decodeAttributes(std::string_view bytes);

// The Tags blob: names separated by NUL bytes.
std::vector<std::string> decodeTags(std::string_view bytes);

// Workspace collision groups, in the old text form ("Default^0^-1\\Fixtures^1^-2",
// name^id^mask) and the newer CollisionGroupData blob.
std::expected<std::vector<CollisionGroup>, std::string> decodeCollisionGroups(std::string_view text);
std::expected<std::vector<CollisionGroup>, std::string> decodeCollisionGroupData(std::string_view bytes);

// Terrain.SmoothGrid: the voxels, keeping only chunks that aren't all air.
std::expected<std::vector<TerrainChunk>, std::string> decodeSmoothGrid(std::string_view bytes);
// Terrain.MaterialColors: an RGB byte triple per terrain material.
std::expected<std::array<Color3, Terrain::materials.size()>, std::string> decodeMaterialColors(std::string_view bytes);

} // namespace lunar::detail
