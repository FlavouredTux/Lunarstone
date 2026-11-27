#pragma once

#include <lunar/PlaceFile.hpp>

#include <expected>
#include <string>
#include <string_view>

// Roblox XML place (.rbxlx) and model (.rbxmx) files.
namespace lunar::rbxlx {

std::expected<LoadedPlace, std::string> loadPlace(std::string_view xml);
std::expected<LoadedModel, std::string> loadModel(std::string_view xml);

} // namespace lunar::rbxlx
