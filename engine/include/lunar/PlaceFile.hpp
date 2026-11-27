#pragma once

#include <lunar/Classes.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {

struct LoadedPlace {
    std::shared_ptr<DataModel> game;
    std::vector<std::string> warnings; // what got skipped, each reported once
};

struct LoadedModel {
    std::vector<InstancePtr> roots; // unparented
    std::vector<std::string> warnings;
};

// Roblox place (.rbxl/.rbxlx) and model (.rbxm/.rbxmx) files, binary or XML,
// told apart by their first bytes. Top-level items of a place become services
// (or children) of a fresh DataModel. Classes the engine doesn't implement yet
// load as UnknownInstance placeholders.
std::expected<LoadedPlace, std::string> loadPlace(std::string_view bytes);
std::expected<LoadedModel, std::string> loadModel(std::string_view bytes);

std::expected<std::string, std::string> readFile(const std::filesystem::path& path);

// The settable property a serialized property name stands for: renamed
// properties ("size" -> Size, "Color3uint8" -> Color ...) and the
// serialized-only ones (Terrain.SmoothGrid, Tags ...), as the file loaders
// apply them. The network uses the same names. Null when there is none.
const Property* findSerializedProperty(const Instance& instance, std::string_view serializedName);

} // namespace lunar
