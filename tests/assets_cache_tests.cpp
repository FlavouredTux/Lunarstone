#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/ContentId.hpp>
#include <lunar/assets/MeshLoader.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace lunar;
using namespace lunar::assets;

namespace {

ContentId parsed(std::string_view uri)
{
    auto id = parseContentId(uri);
    if (!id)
        lt::fail(std::format("{}: {}", uri, id.error()), std::source_location::current());
    return *id;
}

// A fresh directory under the system temp dir, removed again afterwards.
class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> next = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / std::format("lunarstone-assets-{}-{}", stamp, next++);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// One triangle as a version 2.00 mesh.
std::string triangleMesh(float size)
{
    std::string out = "version 2.00\n";
    auto put = [&](auto value) {
        char raw[sizeof(value)];
        std::memcpy(raw, &value, sizeof(value));
        out.append(raw, sizeof(value));
    };
    put(std::uint16_t{12});
    put(std::uint8_t{36});
    put(std::uint8_t{12});
    put(std::uint32_t{3});
    put(std::uint32_t{1});
    for (int i = 0; i < 3; ++i) {
        put(i == 1 ? size : 0.0f);
        put(i == 2 ? size : 0.0f);
        put(0.0f);
        for (float f : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f})
            put(f);
    }
    for (std::uint32_t i : {0u, 1u, 2u})
        put(i);
    return out;
}

struct Fixture {
    TempDir dir;
    std::shared_ptr<AssetCache> cache;

    Fixture()
    {
        cache = std::make_shared<AssetCache>(
            AssetConfig{.contentDir = dir.path() / "content", .cacheDir = dir.path() / "cache", .download = false});
    }
};

} // namespace

TEST(content_id_forms)
{
    CHECK_EQ(parsed("rbxassetid://12345").id, 12345u);
    CHECK_EQ(parsed("rbxassetid://12345").kind, ContentId::Kind::Asset);
    CHECK_EQ(parsed("  RBXASSETID://12345?version=2 ").id, 12345u);
    CHECK_EQ(parsed("rbxassetid://584555920&bust=5172").key(), "rbxassetid://584555920");
    CHECK_EQ(parsed("rbxassetid://12345&bust=1&id=67890").id, 12345u);
    CHECK_EQ(parsed("http://www.roblox.com/asset/?id=12345").id, 12345u);
    CHECK_EQ(parsed("https://www.roblox.com/asset?id=12345").id, 12345u);
    CHECK_EQ(parsed("http://roblox.com/asset/?ID=12345").id, 12345u);
    CHECK_EQ(parsed("https://assetdelivery.roblox.com/v1/asset/?id=12345").id, 12345u);
    CHECK_EQ(parsed("http://www.roblox.com/asset/?version=1&id=12345&x=1").id, 12345u);
    CHECK_EQ(parsed("https://www.roblox.com/asset/?id=12345").key(), "rbxassetid://12345");
    CHECK_EQ(parsed("http://www.roblox.com/Thumbs/Asset.ashx?format=png&width=250&height=250&assetId=238446940&bust=71")
                 .key(),
             "rbxthumb://type=Asset&id=238446940&w=250&h=250");
    CHECK_EQ(parsed("https://www.roblox.com/thumbs/asset.ashx?ASSETID=42&Width=150&Height=150").kind,
             ContentId::Kind::Thumbnail);

    const ContentId builtin = parsed("rbxasset://fonts\\torso.mesh");
    CHECK_EQ(builtin.kind, ContentId::Kind::Builtin);
    CHECK_EQ(builtin.path, "fonts/torso.mesh");
    CHECK_EQ(builtin.key(), "rbxasset://fonts/torso.mesh");
    CHECK_EQ(parsed("rbxasset:///avatar/meshes/leftarm.mesh").path, "avatar/meshes/leftarm.mesh");

    CHECK_EQ(parsed("/tmp/a.mesh").kind, ContentId::Kind::File);
    CHECK_EQ(parsed("file:///tmp/a.mesh").path, "/tmp/a.mesh");
}

TEST(content_id_rejects)
{
    CHECK(!parseContentId("").has_value());
    CHECK(!parseContentId("   ").has_value());
    CHECK(!parseContentId("rbxassetid://").has_value());
    CHECK(!parseContentId("rbxassetid://0").has_value());
    CHECK(!parseContentId("rbxassetid://12a").has_value());
    CHECK(!parseContentId("rbxassetid://12a&bust=1").has_value());
    CHECK(!parseContentId("rbxassetid://&bust=1").has_value());
    CHECK(!parseContentId("rbxassetid://99999999999999999999999").has_value());
    CHECK(!parseContentId("rbxasset://").has_value());
    CHECK(!parseContentId("rbxasset://../../etc/passwd").has_value());
    CHECK(!parseContentId("rbxasset://fonts/../../x").has_value());
    CHECK(!parseContentId("https://example.com/asset/?id=1").has_value());
    CHECK(!parseContentId("https://www.roblox.com.evil.com/asset/?id=1").has_value());
    CHECK(!parseContentId("https://www.roblox.com/asset/").has_value());
    CHECK(!parseContentId("https://www.roblox.com/asset/?name=1").has_value());
    CHECK(!parseContentId("https://www.roblox.com/Thumbs/Asset.ashx?width=250&height=250&assetId=x").has_value());
    CHECK(!parseContentId("https://www.roblox.com/Thumbs/Asset.ashx?width=99999&height=250&assetId=42").has_value());
    CHECK(!parseContentId("https://www.roblox.com/Thumbs/Asset.ashx?width=250&assetId=42").has_value());
    // rbxthumb:// names a thumbnail; its other parameters go.
    auto thumb = parseContentId("rbxthumb://type=AvatarHeadShot&id=42&w=48&h=48&filters=circular&timestamp=9");
    CHECK(thumb.has_value());
    CHECK(thumb->kind == ContentId::Kind::Thumbnail);
    CHECK_EQ(thumb->key(), std::string("rbxthumb://type=AvatarHeadShot&id=42&w=48&h=48&filters=circular"));
    CHECK_EQ(parseContentId("rbxthumb://type=GameIcon&id=7&w=150&h=150")->key(),
             std::string("rbxthumb://type=GameIcon&id=7&w=150&h=150"));
    CHECK(!parseContentId("rbxthumb://type=Asset&id=1").has_value());
    CHECK(!parseContentId("rbxthumb://id=1&w=150&h=150").has_value());
    CHECK(!parseContentId("rbxthumb://type=Asset&id=x&w=150&h=150").has_value());
    CHECK(!parseContentId("rbxthumb://type=Asset&id=1&w=99999&h=150").has_value());
    // Images on Roblox's CDN, over https only, on its own hosts.
    auto cdn = parseContentId("https://tr.rbxcdn.com/180DAY-0123abcd/150/150/Image/Png/noFilter");
    CHECK(cdn.has_value());
    CHECK(cdn->kind == ContentId::Kind::Cdn);
    CHECK(!parseContentId("http://tr.rbxcdn.com/180DAY-0123abcd/150/150/Image/Png/noFilter").has_value());
    CHECK(!parseContentId("https://tr.rbxcdn.com.evil.example/x").has_value());
    CHECK(!parseContentId("https://evil.example/?https://tr.rbxcdn.com/x").has_value());
    CHECK(!parseContentId("https://x@tr.rbxcdn.com/x").has_value());
}

TEST(asset_cache_finds_content_and_cached_assets)
{
    Fixture f;
    writeFile(f.dir.path() / "content/avatar/meshes/arm.mesh", "builtin");
    writeFile(f.dir.path() / "cache/42", "cached");

    auto builtin = f.cache->fetch("rbxasset://avatar/meshes/arm.mesh");
    CHECK(builtin.has_value());
    CHECK_EQ(**builtin, "builtin");
    auto cached = f.cache->fetch("http://www.roblox.com/asset/?id=42");
    CHECK(cached.has_value());
    CHECK_EQ(**cached, "cached");
    CHECK_EQ(f.cache->cachePath(42), f.dir.path() / "cache/42");

    CHECK(!f.cache->fetch("rbxasset://avatar/missing.mesh").has_value());
    CHECK_EQ(f.cache->fetch("rbxassetid://43").error(), "asset 43 isn't cached and downloads are off");
}

TEST(mesh_loader_loads_in_the_background)
{
    Fixture f;
    writeFile(f.dir.path() / "cache/7", triangleMesh(2.0f));
    MeshLoader loader(f.cache, 4);

    std::atomic<int> calls = 0;
    loader.request("rbxassetid://7",
                   [&](std::string_view id, const std::shared_ptr<const MeshFile>& file, std::string_view error) {
                       CHECK_EQ(id, "rbxassetid://7");
                       CHECK(file != nullptr);
                       CHECK(error.empty());
                       ++calls;
                   });
    loader.wait();
    CHECK_EQ(calls.load(), 1);
    CHECK_EQ(loader.pending(), 0u);
    CHECK_EQ(loader.state("rbxassetid://7"), MeshState::Ready);

    // Other spellings of the same id share the load.
    const std::uint64_t generation = loader.generation();
    auto mesh = loader.mesh("http://www.roblox.com/asset/?id=7");
    CHECK(mesh != nullptr);
    CHECK_EQ(mesh->indices.size(), 3u);
    CHECK_EQ(loader.generation(), generation);
    CHECK(loader.file("rbxassetid://7")->size() == (Vector3{2, 2, 0}));
    CHECK_EQ(mesh.get(), &loader.file("rbxassetid://7")->mesh);

    // Asking again after it's done calls back straight away.
    loader.request("rbxassetid://7",
                   [&](std::string_view, const std::shared_ptr<const MeshFile>&, std::string_view) { ++calls; });
    CHECK_EQ(calls.load(), 2);

    // The provider is what the renderer holds on to.
    const MeshProvider provider = loader.provider();
    CHECK_EQ(provider("rbxassetid://7").get(), mesh.get());
}

TEST(mesh_loader_remembers_failures)
{
    Fixture f;
    MeshLoader loader(f.cache, 2);

    CHECK(loader.mesh("rbxassetid://8") == nullptr);
    loader.wait();
    CHECK_EQ(loader.state("rbxassetid://8"), MeshState::Failed);
    CHECK(loader.error("rbxassetid://8").contains("isn't cached"));

    // Showing up later doesn't matter: failures aren't retried.
    writeFile(f.dir.path() / "cache/8", triangleMesh(1.0f));
    CHECK(loader.mesh("rbxassetid://8") == nullptr);
    CHECK_EQ(loader.pending(), 0u);

    // Bad ids and files that aren't meshes fail too.
    CHECK(!loader.load("rbxthumb://nope").has_value());
    CHECK_EQ(loader.state("rbxthumb://nope"), MeshState::Failed);
    CHECK(!loader.load("rbxthumb://type=Asset&id=1&w=150&h=150").has_value()); // an image, not a mesh
    writeFile(f.dir.path() / "cache/9", "<roblox!");
    auto notMesh = loader.load("rbxassetid://9");
    CHECK(!notMesh.has_value());
    CHECK(notMesh.error().contains("not a mesh file"));
}

TEST(mesh_loader_many_requests)
{
    Fixture f;
    for (int i = 1; i <= 200; ++i)
        writeFile(f.dir.path() / std::format("cache/{}", i), triangleMesh(static_cast<float>(i)));
    MeshLoader loader(f.cache, 8);
    for (int round = 0; round < 3; ++round)
        for (int i = 1; i <= 200; ++i)
            loader.request(std::format("rbxassetid://{}", i));
    // Loading on this thread while workers run is fine too.
    auto direct = loader.load("rbxassetid://100");
    CHECK(direct.has_value());
    CHECK((*direct)->size() == (Vector3{100, 100, 0}));
    loader.wait();
    CHECK_EQ(loader.generation(), 200u);
    for (int i = 1; i <= 200; ++i)
        CHECK_EQ(loader.state(std::format("rbxassetid://{}", i)), MeshState::Ready);
}

TEST(asset_download)
{
    const char* enabled = std::getenv("LUNAR_NETWORK_TESTS"); // NOLINT(concurrency-mt-unsafe)
    if (!enabled || std::string_view(enabled) != "1")
        SKIP("set LUNAR_NETWORK_TESTS=1 to download from Roblox");
    if (!AssetCache::canDownload())
        SKIP("built without LUNAR_WITH_CURL");

    TempDir dir;
    auto cache = std::make_shared<AssetCache>(AssetConfig{.contentDir = {}, .cacheDir = dir.path(), .download = true});
    MeshLoader loader(cache, 2);
    // A public mesh that anonymous requests may fetch.
    auto file = loader.load("rbxassetid://13160225");
    if (!file)
        lt::fail(file.error(), std::source_location::current());
    CHECK_EQ((*file)->version, "1.00");
    CHECK(std::filesystem::is_regular_file(dir.path() / "13160225"));
}

// A teleport tears the scene's loaders down while their workers sit idle;
// every worker has to wake and the destructor return, every time.
TEST(mesh_loader_shuts_down_idle_workers)
{
    TempDir dir;
    auto cache = std::make_shared<AssetCache>(AssetConfig{.contentDir = {}, .cacheDir = dir.path(), .download = false});
    for (int i = 0; i < 2000; ++i) {
        MeshLoader loader(cache, 4);
        if (i % 2 == 0)
            std::this_thread::yield(); // some workers waiting, some not yet
    }
    CHECK(true);
}
