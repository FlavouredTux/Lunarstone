#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace lunar::assets {

// Where a Content string (MeshId, TextureID, ...) points.
struct ContentId {
    enum class Kind : std::uint8_t {
        Builtin, // rbxasset://path, a file in the client's content folder
        Asset,   // rbxassetid://N and the www.roblox.com/asset/?id=N URLs
        File,    // a local path or file:// URL, for tools and tests
        // rbxthumb://type=T&id=N&w=W&h=H: a thumbnail Roblox renders (an
        // avatar's headshot, a game's icon); path is the type.
        Thumbnail,
        // An https URL on Roblox's CDN (*.rbxcdn.com), as the Lua app hands
        // an image it already looked up; path is the URL.
        Cdn,
    };

    Kind kind = Kind::Asset;
    std::uint64_t id = 0;                // Asset, Thumbnail
    std::string path;                    // Builtin (relative, '/'-separated), File, Cdn, or a Thumbnail's type
    std::uint32_t width = 0, height = 0; // Thumbnail
    bool circular = false;               // Thumbnail: filters=circular

    // One spelling per asset, so equal ids share a cache entry:
    // "rbxassetid://N", "rbxasset://path", the file path, or
    // "rbxthumb://type=T&id=N&w=W&h=H" (with "&filters=circular"), or the CDN URL.
    std::string key() const;

    friend bool operator==(const ContentId&, const ContentId&) = default;
};

// Accepts rbxasset://, rbxassetid://N, http(s)://(www.)roblox.com/asset/?id=N
// (and asset?id=, assetdelivery.roblox.com/v1/asset/?id=, extra query
// parameters, any case), rbxthumb:// thumbnails, https URLs on Roblox's CDN,
// file:// URLs and plain paths. Empty strings, rbxhttp:// and other web
// addresses are errors.
std::expected<ContentId, std::string> parseContentId(std::string_view uri);

} // namespace lunar::assets
