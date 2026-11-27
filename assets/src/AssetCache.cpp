#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/DerivedCache.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <system_error>

namespace lunar::assets {

namespace {

std::string_view env(const char* name)
{
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): nothing here sets variables
    return value ? std::string_view(value) : std::string_view();
}

std::expected<std::string, std::string> readWhole(const std::filesystem::path& path)
{
    // At its size in one read, as lunar::readFile does: a character at a
    // time it was most of a texture loader's time outside the decoders.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return std::unexpected(std::format("can't open {}", path.string()));
    std::string bytes;
    if (const std::streamoff size = file.tellg(); size >= 0) {
        bytes.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(bytes.data(), size);
        bytes.resize(static_cast<std::size_t>(file.gcount()));
    } else {
        file.clear();
        bytes.assign(std::istreambuf_iterator<char>(file), {});
    }
    if (file.bad())
        return std::unexpected(std::format("can't read {}", path.string()));
    return bytes;
}

// Through a temporary name of this writer's own, so a crash, another thread
// or a second process never sees half a file or mixes its bytes into it.
void writeCacheFile(const std::filesystem::path& path, std::string_view bytes)
{
    const std::array<std::span<const std::uint8_t>, 1> parts{
        std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())};
    (void)writeWhole(path, parts); // a cache that can't be written only costs a later download
}

} // namespace

AssetConfig AssetConfig::fromEnvironment()
{
    AssetConfig config;
    config.contentDir = env("LUNAR_CONTENT_DIR").empty() ? std::filesystem::path(LUNAR_DEFAULT_CONTENT_DIR)
                                                         : std::filesystem::path(env("LUNAR_CONTENT_DIR"));
    if (!env("LUNAR_ASSET_CACHE").empty())
        config.cacheDir = env("LUNAR_ASSET_CACHE");
    else if (!env("XDG_CACHE_HOME").empty())
        config.cacheDir = std::filesystem::path(env("XDG_CACHE_HOME")) / "lunarstone/assets";
    else if (!env("HOME").empty())
        config.cacheDir = std::filesystem::path(env("HOME")) / ".cache/lunarstone/assets";
    config.download = env("LUNAR_OFFLINE").empty() || env("LUNAR_OFFLINE") == "0";
    if (const std::string_view decoded = env("LUNAR_DECODED_CACHE"); decoded == "off" || decoded == "0")
        config.decodedDir.clear();
    else if (!decoded.empty())
        config.decodedDir = decoded;
    else if (!config.cacheDir.empty())
        config.decodedDir = config.cacheDir.parent_path() / "decoded";
    if (const std::string_view http = env("LUNAR_HTTP_CACHE"); http == "off" || http == "0")
        config.httpDir.clear();
    else if (!http.empty())
        config.httpDir = http;
    else if (!config.cacheDir.empty())
        config.httpDir = config.cacheDir.parent_path() / "http";
    if (const std::string_view megabytes = env("LUNAR_DECODED_CACHE_MB"); !megabytes.empty()) {
        std::uint64_t value = 0;
        if (std::from_chars(megabytes.data(), megabytes.data() + megabytes.size(), value).ec == std::errc{})
            config.decodedCacheBytes = value << 20;
    }
    return config;
}

AssetCache::AssetCache(AssetConfig config) : config_(std::move(config)), customWeb_(config_.web != nullptr)
{
    if (!config_.web)
        config_.web = std::make_shared<const RobloxWeb>();
}

bool AssetCache::canDownload()
{
    return curlAvailable();
}

std::filesystem::path AssetCache::cachePath(std::uint64_t assetId) const
{
    return config_.cacheDir / std::to_string(assetId);
}

std::expected<AssetBytes, std::string> AssetCache::fetch(std::string_view uri)
{
    auto id = parseContentId(uri);
    if (!id)
        return std::unexpected(id.error());
    return fetch(*id);
}

std::expected<AssetBytes, std::string> AssetCache::fetch(const ContentId& id)
{
    auto share = [](std::expected<std::string, std::string> bytes) -> std::expected<AssetBytes, std::string> {
        if (!bytes)
            return std::unexpected(bytes.error());
        return std::make_shared<const std::string>(std::move(*bytes));
    };
    switch (id.kind) {
    case ContentId::Kind::Builtin:
        if (std::string_view(id.path).starts_with(customAssetFolder)
            && std::string_view(id.path).substr(customAssetFolder.size()).starts_with('/'))
            return share(readWhole(config_.cacheDir / id.path));
        if (config_.contentDir.empty())
            return std::unexpected(std::format("{}: no content folder set (LUNAR_CONTENT_DIR)", id.key()));
        {
            // The client looks in PlatformContent/pc next to content too, where
            // the platform's textures (sky, studs) and terrain settings live,
            // and in ExtraContent, where its Lua app's textures and packages
            // are (textures/ui/LuaApp, LuaPackages). GUESS: in that order.
            auto bytes = readWhole(config_.contentDir / id.path);
            std::error_code ec;
            for (const char* overlay : {"PlatformContent/pc", "ExtraContent"}) {
                const std::filesystem::path other = config_.contentDir.parent_path() / overlay / id.path;
                if (!bytes && std::filesystem::is_regular_file(other, ec))
                    bytes = readWhole(other);
            }
            return share(std::move(bytes));
        }
    case ContentId::Kind::File:
        return share(readWhole(id.path));
    case ContentId::Kind::Asset:
        return fetchAsset(id.id);
    case ContentId::Kind::Thumbnail:
        return fetchThumbnail(id);
    case ContentId::Kind::Cdn:
        return fetchCdn(id);
    }
    return std::unexpected("unknown content id");
}

bool transientFailure(std::string_view error)
{
    return error.contains("HTTP 429") || error.contains("HTTP 5");
}

namespace {
std::atomic<std::uint64_t> sessionPlace{0};
} // namespace

void AssetCache::setSessionPlaceId(std::uint64_t placeId)
{
    sessionPlace = placeId;
}

std::uint64_t AssetCache::sessionPlaceId()
{
    return sessionPlace.load();
}

std::expected<AssetBytes, std::string> AssetCache::fetchAsset(std::uint64_t assetId)
{
    return fetchCached(cachePath(assetId), std::format("asset {}", assetId),
                       [&] {
                           const std::uint64_t place = placeId_.load();
                           return config_.web->downloadAsset(assetId, place != 0 ? place : sessionPlace.load());
                       });
}

// Kept like assets, by type, id and size. GUESS: a thumbnail doesn't
// change once made (an avatar's does when it's changed; the cache is wiped by hand).
std::expected<AssetBytes, std::string> AssetCache::fetchThumbnail(const ContentId& id)
{
    // The type is a file name here, so letters and digits only.
    if (id.path.empty() || !std::ranges::all_of(id.path, [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        }))
        return std::unexpected(std::format("{}: no such thumbnail type", id.key()));
    const std::filesystem::path path
        = config_.cacheDir / "thumbnails"
          / std::format("{}-{}-{}x{}{}.png", id.path, id.id, id.width, id.height, id.circular ? "-circular" : "");
    return fetchCached(path, id.key(), [&] {
        return config_.web->downloadThumbnail(id.path, id.id, id.width, id.height, id.circular);
    });
}

// Kept under cdn/, named after the URL's path (letters and digits kept, the
// rest as '_'). Anonymous, as anyone can fetch from the CDN.
std::expected<AssetBytes, std::string> AssetCache::fetchCdn(const ContentId& id)
{
    std::string name = id.path.substr(std::min(id.path.size(), std::string_view("https://").size()));
    for (char& c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            c = '_';
    if (name.size() > 200)
        name.resize(200);
    return fetchCached(config_.cacheDir / "cdn" / name, id.key(), [&]() -> std::expected<std::string, std::string> {
        auto reply = config_.web->anonymous({.url = id.path, .followRedirects = true});
        if (!reply)
            return std::unexpected(reply.error());
        if (reply->status != 200)
            return std::unexpected(describeHttpFailure(*reply));
        return std::move(reply->body);
    });
}

std::expected<AssetBytes, std::string> AssetCache::fetchFormat(std::uint64_t assetId, std::string_view format)
{
    std::filesystem::path path = cachePath(assetId);
    path += std::format(".{}", format);
    return fetchCached(path, std::format("asset {} as {}", assetId, format),
                       [&] { return config_.web->downloadAssetFormat(assetId, format); });
}

std::expected<AssetBytes, std::string>
AssetCache::fetchCached(const std::filesystem::path& path, std::string key,
                        const std::function<std::expected<std::string, std::string>()>& download)
{
    if (!config_.cacheDir.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            auto bytes = readWhole(path);
            if (bytes)
                return std::make_shared<const std::string>(std::move(*bytes));
        }
    }
    const bool online = config_.download && (customWeb_ || canDownload());
    if (!online)
        return std::unexpected(std::format(
            "{} isn't cached{}", key, config_.download ? " (this build can't download)" : " and downloads are off"));

    {
        const std::scoped_lock lock(mutex_);
        if (auto failed = failedDownloads_.find(key); failed != failedDownloads_.end())
            return std::unexpected(failed->second);
    }
    auto bytes = download();
    if (!bytes) {
        std::string message = std::format("{}: {}", key, bytes.error());
        if (!transientFailure(message)) {
            const std::scoped_lock lock(mutex_);
            failedDownloads_.emplace(std::move(key), message);
        }
        return std::unexpected(std::move(message));
    }
    if (!config_.cacheDir.empty())
        writeCacheFile(path, *bytes);
    return std::make_shared<const std::string>(std::move(*bytes));
}

} // namespace lunar::assets
