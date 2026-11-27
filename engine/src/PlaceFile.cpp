#include <lunar/PlaceFile.hpp>

#include <lunar/Rbxl.hpp>
#include <lunar/Rbxlx.hpp>

#include <format>
#include <fstream>
#include <iterator>

namespace lunar {

namespace {

bool isBinary(std::string_view bytes)
{
    return bytes.starts_with("<roblox!");
}

} // namespace

std::expected<LoadedPlace, std::string> loadPlace(std::string_view bytes)
{
    return isBinary(bytes) ? rbxl::loadPlace(bytes) : rbxlx::loadPlace(bytes);
}

std::expected<LoadedModel, std::string> loadModel(std::string_view bytes)
{
    return isBinary(bytes) ? rbxl::loadModel(bytes) : rbxlx::loadModel(bytes);
}

std::expected<std::string, std::string> readFile(const std::filesystem::path& path)
{
    // At its size in one read: through istreambuf_iterator, a character at a
    // time, it was more than ten times slower. What isn't seekable (a pipe)
    // is still read to its end.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return std::unexpected(std::format("can't open {}", path.string()));
    const std::streamoff size = file.tellg();
    if (size < 0) {
        file.clear();
        return std::string(std::istreambuf_iterator<char>(file), {});
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    file.read(bytes.data(), size);
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return bytes;
}

} // namespace lunar
