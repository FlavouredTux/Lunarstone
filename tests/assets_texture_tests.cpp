#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/DerivedCache.hpp>
#include <lunar/assets/ImageDecode.hpp>
#include <lunar/assets/MeshLoader.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/render/Image.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace lunar;
using namespace lunar::assets;

namespace {

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> next = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / std::format("lunarstone-textures-{}-{}", stamp, next++);
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
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string redPng(std::uint32_t width, std::uint32_t height)
{
    render::PixelImage image;
    image.width = width;
    image.height = height;
    image.pixels.assign(std::size_t{width} * height, {255, 0, 0, 255});
    return render::encodePng(image);
}

// Offline, with the cache in dir: rbxassetid://N reads dir/N.
std::shared_ptr<AssetCache> offlineCache(const TempDir& dir)
{
    return std::make_shared<AssetCache>(
        AssetConfig{.contentDir = dir.path(), .cacheDir = dir.path(), .download = false});
}

constexpr std::string_view decalModel = R"(<roblox version="4">
  <Item class="Decal" referent="RBX0">
    <Properties>
      <string name="Name">Decal</string>
      <Content name="Texture"><url>http://www.roblox.com/asset/?id=123</url></Content>
    </Properties>
  </Item>
</roblox>)";

constexpr std::string_view texturePack = R"(<roblox>
  <texturepack_version>2</texturepack_version>
  <usage>3</usage>
  <color>123</color>
  <normal>124</normal>
</roblox>)";

} // namespace

TEST(texture_indirections)
{
    CHECK_EQ(followImageIndirection(decalModel), "http://www.roblox.com/asset/?id=123");
    CHECK_EQ(followImageIndirection(texturePack), "rbxassetid://123");
    CHECK_EQ(followImageIndirection(redPng(1, 1)), "");
    CHECK_EQ(followImageIndirection("<roblox>garbage"), "");
}

TEST(texture_loader_loads_once_and_shares)
{
    TempDir dir;
    writeFile(dir.path() / "123", redPng(3, 2));
    writeFile(dir.path() / "200", decalModel);
    writeFile(dir.path() / "300", texturePack);
    writeFile(dir.path() / "400", "not an image");
    TextureLoader loader(offlineCache(dir), 2);
    auto provider = loader.provider();

    (void)provider("rbxassetid://123"); // starts the load, null until it finishes
    loader.request("rbxassetid://200");
    loader.request("http://www.roblox.com/asset/?id=300");
    loader.request("rbxassetid://400");
    loader.request("rbxassetid://999");
    loader.request("rbxthumb://type=Asset&id=1&w=150&h=150");
    loader.wait();
    CHECK_EQ(loader.pending(), 0u);
    CHECK(loader.generation() >= 5u);

    auto image = provider("rbxassetid://123");
    CHECK(image != nullptr);
    CHECK_EQ(image->width, 3u);
    CHECK_EQ(image->rgba[0], 255);
    // Every spelling of an id shares one image; the provider returns it again.
    CHECK(provider("rbxassetid://123") == image);
    CHECK(provider("https://www.roblox.com/asset/?id=123") != nullptr);
    loader.wait();
    CHECK(provider("https://www.roblox.com/asset/?id=123") == image);
    const auto generation = loader.generation();
    CHECK(provider("rbxassetid://123&bust=4172") == image);
    CHECK(provider("rbxassetid://123&bust=9653") == image);
    CHECK_EQ(loader.generation(), generation);
    CHECK_EQ(loader.pending(), 0u);
    // A Decal asset and a TexturePack lead to their image.
    CHECK_EQ(provider("rbxassetid://200")->width, 3u);
    CHECK_EQ(provider("http://www.roblox.com/asset/?id=300")->height, 2u);
    // Failures are remembered with a reason.
    CHECK_EQ(loader.state("rbxassetid://400"), TextureState::Failed);
    CHECK(loader.error("rbxassetid://400").contains("not an image"));
    CHECK_EQ(loader.state("rbxassetid://999"), TextureState::Failed);
    CHECK(provider("rbxassetid://999") == nullptr);
    CHECK_EQ(loader.state("rbxthumb://type=Asset&id=1&w=150&h=150"), TextureState::Failed);

    // load() blocks; callbacks fire for finished and new loads.
    auto loaded = loader.load("rbxassetid://123");
    CHECK(loaded.has_value() && *loaded == image);
    int calls = 0;
    loader.request("rbxassetid://123",
                   [&](std::string_view, const std::shared_ptr<const Image>& got, std::string_view error) {
                       calls += got == image && error.empty() ? 1 : 100;
                   });
    CHECK_EQ(calls, 1);
}

TEST(texture_loader_halves_big_images_to_fit)
{
    TempDir dir;
    writeFile(dir.path() / "123", redPng(2048, 600));
    writeFile(dir.path() / "124", redPng(1024, 1024));
    TextureLoader loader(offlineCache(dir), 1);
    auto big = loader.load("rbxassetid://123");
    CHECK(big.has_value());
    CHECK_EQ((*big)->width, 1024u);
    CHECK_EQ((*big)->height, 300u);
    CHECK_EQ((*big)->rgba[0], 255);
    CHECK_EQ((*big)->rgba[1], 0);
    auto fits = loader.load("rbxassetid://124");
    CHECK(fits.has_value());
    CHECK_EQ((*fits)->width, 1024u);
}

TEST(image_halving_averages_blocks)
{
    Image image;
    image.width = 3;
    image.height = 2;
    // Rows: 0 100 50 / 200 60 50 (grey levels, opaque).
    for (const int level : {0, 100, 50, 200, 60, 50})
        image.rgba.insert(image.rgba.end(), {static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(level),
                                             static_cast<std::uint8_t>(level), 255});
    const Image half = limitImageSize(image, 2);
    CHECK_EQ(half.width, 1u);
    CHECK_EQ(half.height, 1u);
    CHECK_EQ(half.rgba[0], 90); // (0 + 100 + 200 + 60 + 2) / 4
    CHECK_EQ(half.rgba[3], 255);
    CHECK_EQ(limitImageSize(image, 3).width, 3u);
}

TEST(texture_loader_stops_indirection_loops)
{
    TempDir dir;
    // A Decal whose texture is itself.
    std::string loop(decalModel);
    loop.replace(loop.find("id=123"), 6, "id=500");
    writeFile(dir.path() / "500", loop);
    TextureLoader loader(offlineCache(dir), 1);
    auto result = loader.load("rbxassetid://500");
    CHECK(!result.has_value());
    CHECK(result.error().contains("too long"));
}

namespace {

// A width x height image of one colour, as a PNG; the pattern's x makes rows differ.
std::string patternPng(std::uint32_t width, std::uint32_t height, std::uint8_t blue)
{
    render::PixelImage image;
    image.width = width;
    image.height = height;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            image.pixels.push_back({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), blue, 255});
    return render::encodePng(image);
}

std::shared_ptr<AssetCache> decodedCache(const TempDir& dir, std::uint64_t maxBytes = std::uint64_t{1} << 30)
{
    return std::make_shared<AssetCache>(AssetConfig{.contentDir = dir.path(),
                                                    .cacheDir = dir.path(),
                                                    .download = false,
                                                    .decodedDir = dir.path() / "decoded",
                                                    .decodedCacheBytes = maxBytes});
}

bool samePattern(const Image& image, std::uint32_t width, std::uint32_t height, std::uint8_t blue)
{
    if (image.width != width || image.height != height || image.rgba.size() != image.byteSize())
        return false;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t* p = image.rgba.data() + (std::size_t{y} * width + x) * 4;
            if (p[0] != static_cast<std::uint8_t>(x) || p[1] != static_cast<std::uint8_t>(y) || p[2] != blue
                || p[3] != 255)
                return false;
        }
    return true;
}

std::size_t decodedFiles(const TempDir& dir)
{
    std::size_t files = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "decoded", ec))
        files += entry.path().extension() == ".rgba" ? 1u : 0u;
    return files;
}

} // namespace

// A decoded texture is kept on disk and mapped back the next time its bytes
// come, the same pixels; new bytes under the same id decode afresh, a file
// that isn't whole is decoded again, and small images stay in memory.
TEST(texture_loader_keeps_decoded_textures)
{
    TempDir dir;
    writeFile(dir.path() / "1", patternPng(200, 150, 7));
    writeFile(dir.path() / "2", patternPng(16, 16, 9)); // 1 KiB of pixels: not worth a file
    {
        TextureLoader loader(decodedCache(dir), 1);
        auto image = loader.load("rbxassetid://1");
        CHECK(image.has_value() && samePattern(**image, 200, 150, 7));
        CHECK((*image)->rgba.isExternal()); // decoded, kept, and mapped back
        auto small = loader.load("rbxassetid://2");
        CHECK(small.has_value() && samePattern(**small, 16, 16, 9) && !(*small)->rgba.isExternal());
    }
    CHECK_EQ(decodedFiles(dir), std::size_t{1});
    {
        TextureLoader loader(decodedCache(dir), 1);
        auto image = loader.load("rbxassetid://1");
        CHECK(image.has_value() && (*image)->rgba.isExternal() && samePattern(**image, 200, 150, 7));
        // Dropped from memory, still there to read.
        (*image)->rgba.evict();
        CHECK(samePattern(**image, 200, 150, 7));
        // A copy owns its pixels, and writing it leaves the file alone.
        Image copy = **image;
        CHECK(!copy.rgba.isExternal());
        copy.rgba[0] = 99;
        CHECK_EQ((*image)->rgba[0], 0);
    }
    // The asset changes: its new bytes are another key.
    writeFile(dir.path() / "1", patternPng(200, 150, 40));
    {
        TextureLoader loader(decodedCache(dir), 1);
        auto image = loader.load("rbxassetid://1");
        CHECK(image.has_value() && samePattern(**image, 200, 150, 40));
    }
    CHECK_EQ(decodedFiles(dir), std::size_t{2});
    // A file cut short is no image: decoded again, and kept whole.
    for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "decoded"))
        std::filesystem::resize_file(entry.path(), 1000);
    {
        TextureLoader loader(decodedCache(dir), 1);
        auto image = loader.load("rbxassetid://1");
        CHECK(image.has_value() && samePattern(**image, 200, 150, 40) && (*image)->rgba.isExternal());
    }
    // Under another size limit the image is another one.
    {
        TextureLoader loader(decodedCache(dir), 1, 64);
        auto image = loader.load("rbxassetid://1");
        CHECK(image.has_value() && (*image)->width == 50u && (*image)->height == 37u);
    }
}

// The directory keeps to its size: files used longest ago go first.
TEST(texture_loader_trims_decoded_textures)
{
    TempDir dir;
    for (int i = 1; i <= 4; ++i)
        writeFile(dir.path() / std::to_string(i), patternPng(200, 200, static_cast<std::uint8_t>(i)));
    const std::uint64_t fileBytes = 200 * 200 * 4 + 64;
    {
        TextureLoader loader(decodedCache(dir), 1);
        for (int i = 1; i <= 4; ++i) {
            CHECK(loader.load(std::format("rbxassetid://{}", i)).has_value());
            // The order they were used in, a second apart.
            for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "decoded"))
                if (entry.last_write_time() > std::filesystem::file_time_type::clock::now() - std::chrono::seconds(1))
                    std::filesystem::last_write_time(entry.path(), std::filesystem::file_time_type::clock::now()
                                                                       - std::chrono::hours(10 - i));
        }
    }
    CHECK_EQ(decodedFiles(dir), std::size_t{4});
    // Room for two and a half: the two used last stay.
    TextureLoader loader(decodedCache(dir, fileBytes * 5 / 2), 1);
    CHECK_EQ(decodedFiles(dir), std::size_t{2});
    for (int i = 3; i <= 4; ++i) {
        auto image = loader.load(std::format("rbxassetid://{}", i));
        CHECK(image.has_value() && (*image)->rgba.isExternal()
              && samePattern(**image, 200, 200, static_cast<std::uint8_t>(i)));
    }
}

// Two processes at once (a game and a benchmark): writers of the same file
// never mix their bytes, a reader maps either nothing or the whole file,
// and trimming a file another one has mapped leaves the mapping whole.
TEST(derived_cache_holds_up_to_other_processes)
{
    TempDir dir;
    std::vector<std::uint8_t> bytes(std::size_t{4} << 20);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i * 31 + (i >> 12));
    const std::string name = DerivedCache::name("source", "test");
    const std::array<std::span<const std::uint8_t>, 1> parts{std::span<const std::uint8_t>(bytes)};
    std::vector<pid_t> writers;
    for (int w = 0; w < 3; ++w) {
        const pid_t pid = fork();
        if (pid == 0) {
            const DerivedCache cache(dir.path(), std::uint64_t{1} << 40);
            for (int i = 0; i < 60; ++i)
                (void)cache.write(name, parts);
            _exit(0);
        }
        writers.push_back(pid);
    }
    const DerivedCache cache(dir.path(), std::uint64_t{1} << 40);
    int seen = 0;
    bool whole = true;
    auto running = [&] {
        for (pid_t pid : writers)
            if (waitpid(pid, nullptr, WNOHANG) == 0)
                return true;
        return false;
    };
    while (running()) {
        if (auto file = cache.open(name)) {
            ++seen;
            whole = whole && file->bytes().size() == bytes.size()
                    && std::memcmp(file->bytes().data(), bytes.data(), bytes.size()) == 0;
        }
    }
    for (pid_t pid : writers)
        waitpid(pid, nullptr, 0);
    CHECK(whole);
    auto file = cache.open(name);
    CHECK(file != nullptr);
    if (!file)
        return;
    CHECK(file->bytes().size() == bytes.size() && std::memcmp(file->bytes().data(), bytes.data(), bytes.size()) == 0);
    std::size_t parts_left = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path()))
        parts_left += entry.path().extension() == ".part" ? 1u : 0u;
    CHECK_EQ(parts_left, std::size_t{0});

    // Trimmed away under it (another process's cache, a tiny one): what's
    // mapped stays readable, dropped from memory or not, and is whole.
    const DerivedCache tiny(dir.path(), 1);
    CHECK(tiny.open(name) == nullptr);
    file->evict(nullptr, 0);
    CHECK(std::memcmp(file->bytes().data(), bytes.data(), bytes.size()) == 0);
    (void)seen;
}

namespace {

// A 10 x 10 grid of quads (200 triangles) as a version 1.00 text mesh.
std::string gridMesh()
{
    std::string faces;
    int count = 0;
    auto corner = [&](int x, int z) {
        faces += std::format("[{},{},{}][0,1,0][{},{},0]", x, (x * z) % 3, z, x / 10.0, z / 10.0);
    };
    for (int x = 0; x < 10; ++x)
        for (int z = 0; z < 10; ++z) {
            corner(x, z);
            corner(x, z + 1);
            corner(x + 1, z);
            corner(x + 1, z);
            corner(x, z + 1);
            corner(x + 1, z + 1);
            count += 2;
        }
    return std::format("version 1.00\r\n{}\r\n{}", count, faces);
}

bool sameMesh(const MeshFile& a, const MeshFile& b)
{
    auto bytes = [](const auto& x, const auto& y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return a.version == b.version && bytes(a.mesh.vertices, b.mesh.vertices) && bytes(a.mesh.indices, b.mesh.indices)
           && bytes(a.mesh.lodIndices, b.mesh.lodIndices) && bytes(a.mesh.lodEnds, b.mesh.lodEnds)
           && bytes(a.mesh.lodErrors, b.mesh.lodErrors) && std::memcmp(&a.boundsMin, &b.boundsMin, 12) == 0
           && std::memcmp(&a.boundsMax, &b.boundsMax, 12) == 0 && a.lodCount == b.lodCount && a.boneCount == b.boneCount
           && a.hasFacs == b.hasFacs && a.operands.size() == b.operands.size();
}

std::size_t keptMeshes(const TempDir& dir)
{
    std::size_t files = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "decoded", ec))
        files += entry.path().extension() == ".mesh" ? 1u : 0u;
    return files;
}

} // namespace

// A parsed mesh, with the levels generateLods made, is kept and read back
// the same to the byte; a file cut short is parsed again; a loader without
// levels doesn't take one with them.
TEST(mesh_loader_keeps_parsed_meshes)
{
    TempDir dir;
    writeFile(dir.path() / "1", gridMesh());
    auto fresh = MeshLoader(offlineCache(dir), 1).load("rbxassetid://1"); // no decodedDir: parsed
    CHECK(fresh.has_value() && !(*fresh)->mesh.lodEnds.empty());
    {
        auto made = MeshLoader(decodedCache(dir), 1).load("rbxassetid://1");
        CHECK(made.has_value() && sameMesh(**made, **fresh));
    }
    CHECK_EQ(keptMeshes(dir), std::size_t{1});
    auto kept = MeshLoader(decodedCache(dir), 1).load("rbxassetid://1");
    CHECK(kept.has_value() && sameMesh(**kept, **fresh));
    auto plain = MeshLoader(decodedCache(dir), 1, false).load("rbxassetid://1");
    CHECK(plain.has_value() && (*plain)->mesh.lodEnds.empty());
    CHECK_EQ(keptMeshes(dir), std::size_t{2});
    for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "decoded"))
        std::filesystem::resize_file(entry.path(), 200);
    auto again = MeshLoader(decodedCache(dir), 1).load("rbxassetid://1");
    CHECK(again.has_value() && sameMesh(**again, **fresh));
}
