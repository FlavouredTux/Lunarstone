#pragma once

#include <atomic>
#include <lunar/assets/ContentId.hpp>
#include <lunar/assets/Web.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lunar::assets {

struct AssetConfig {
    std::filesystem::path contentDir; // what rbxasset:// paths are relative to (then ../PlatformContent/pc)
    std::filesystem::path cacheDir;   // downloaded assets, one file per id
    bool download = true;             // fetch cache misses from Roblox (needs LUNAR_WITH_CURL)
    // How downloads reach Roblox; null makes one over libcurl with the
    // default login (see Web.hpp).
    std::shared_ptr<const RobloxWeb> web;
    // Textures as TextureLoader decoded them, raw RGBA keyed by the content
    // of what they were decoded from, to map in rather than decode again;
    // empty for none. At most decodedCacheBytes of them stay, the least
    // recently used going first.
    std::filesystem::path decodedDir;
    std::uint64_t decodedCacheBytes = std::uint64_t{6} << 30;
    // The console's game:HttpGet answers that may be kept (HttpCache); empty
    // for none.
    std::filesystem::path httpDir;

    // contentDir comes from LUNAR_CONTENT_DIR or the configured build default.
    // cacheDir comes from LUNAR_ASSET_CACHE or
    // $XDG_CACHE_HOME/lunarstone/assets or ~/.cache/lunarstone/assets.
    // LUNAR_OFFLINE=1 turns downloads off. decodedDir is "decoded" next to
    // cacheDir, or LUNAR_DECODED_CACHE ("off" for none), and its size
    // LUNAR_DECODED_CACHE_MB. httpDir is "http" next to cacheDir, or
    // LUNAR_HTTP_CACHE ("off" for none).
    static AssetConfig fromEnvironment();
};

// rbxasset://customassets/<file>: a file the console's getcustomasset put in
// the cache folder's customassets, read from there instead of the content
// folder (the real client's, which stays as it came).
inline constexpr std::string_view customAssetFolder = "customassets";

// The raw bytes of an asset, shared between everyone who asked for it.
using AssetBytes = std::shared_ptr<const std::string>;

// Finds an asset's bytes: built-in ones in the content folder, numbered ones
// in the disk cache, downloading those into it from Roblox's asset delivery
// when allowed. Blocking but thread-safe, so a pool of loaders can share one;
// a download that failed isn't tried again by the same cache, unless it
// failed for now (transientFailure).
//
// Anonymous downloads work for assets Roblox shares publicly. Others need a
// login: the .ROBLOSECURITY cookie (--cookie-file, LUNAR_ROBLOSECURITY or
// ~/.config/lunarstone/cookie) or an Open Cloud key with asset read access
// in LUNAR_ROBLOX_API_KEY. Those are only sent to Roblox's API hosts, never
// to the CDN, and never printed (RobloxWeb in Web.hpp).
// A fetch's error that may go away by asking again later: Roblox limiting
// how fast we download (HTTP 429, which a burst of a game's sounds at the
// join gets [live: Midnight Racing, 14 of them]) or its servers' trouble (5xx).
bool transientFailure(std::string_view error);

class AssetCache {
public:
    explicit AssetCache(AssetConfig config = AssetConfig::fromEnvironment());

    std::expected<AssetBytes, std::string> fetch(const ContentId& id);
    std::expected<AssetBytes, std::string> fetch(std::string_view uri);

    // One representation of an asset (RobloxWeb::downloadAssetFormat), cached
    // next to the plain one as "<id>.<format>".
    std::expected<AssetBytes, std::string> fetchFormat(std::uint64_t assetId, std::string_view format);

    const AssetConfig& config() const { return config_; }
    // The place assets are downloaded for (RobloxWeb::downloadAsset), once
    // it's known; 0 for none, which falls back to the session's.
    void setPlaceId(std::uint64_t placeId) { placeId_ = placeId; }
    // The place this process is in (a join or a replay sets it): every cache
    // downloads for it unless told another. Much of a game's content (its
    // animations, audio, some meshes) is shared with its place only.
    static void setSessionPlaceId(std::uint64_t placeId);
    static std::uint64_t sessionPlaceId();
    std::filesystem::path cachePath(std::uint64_t assetId) const;

    // Whether this build can download at all.
    static bool canDownload();

private:
    std::expected<AssetBytes, std::string> fetchAsset(std::uint64_t assetId);
    std::expected<AssetBytes, std::string> fetchThumbnail(const ContentId& id);
    std::expected<AssetBytes, std::string> fetchCdn(const ContentId& id);
    std::expected<AssetBytes, std::string>
    fetchCached(const std::filesystem::path& path, std::string key,
                const std::function<std::expected<std::string, std::string>()>& download);

    AssetConfig config_;
    bool customWeb_ = false;
    std::atomic<std::uint64_t> placeId_ = 0;
    std::mutex mutex_;
    std::unordered_map<std::string, std::string> failedDownloads_;
};

} // namespace lunar::assets
