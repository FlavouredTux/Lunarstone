#pragma once

#include <lunar/PlaceFile.hpp>
#include <lunar/Value.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Roblox binary place (.rbxl) and model (.rbxm) files.
namespace lunar::rbxl {

// One PROP chunk: a property's values for every instance of a class.
struct PropertyColumn {
    std::string name;
    std::uint8_t type = 0;               // binary type id
    bool decoded = false;                // false for types we can't read yet, or on error
    std::string error;                   // why decoding failed, empty for unsupported types
    std::vector<Value> values;           // one per instance, in the class's order
    std::vector<std::int32_t> referents; // instead of values for Ref properties, -1 is nil
    // Instead of values for SharedString properties: indices into
    // Document::sharedStrings, so a blob thousands of parts share stays one copy.
    std::vector<std::uint32_t> shared;
};

// One INST chunk plus its properties.
struct ClassChunk {
    std::int32_t id = 0;
    std::string name;
    bool isService = false;
    std::vector<std::int32_t> referents;
    std::vector<PropertyColumn> properties;
};

// The file's contents as stored, before any instances exist.
struct Document {
    std::size_t instanceCount = 0;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<SharedBytes> sharedStrings; // the SSTR chunk, never null
    std::vector<ClassChunk> classes;
    std::vector<std::pair<std::int32_t, std::int32_t>> parents; // child, parent (-1 for none)
};

std::expected<Document, std::string> parse(std::string_view bytes);

std::expected<LoadedPlace, std::string> loadPlace(std::string_view bytes);
std::expected<LoadedModel, std::string> loadModel(std::string_view bytes);

} // namespace lunar::rbxl
