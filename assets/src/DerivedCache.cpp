#include <lunar/assets/DerivedCache.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace lunar::assets {

std::shared_ptr<const MappedFile> MappedFile::open(const std::filesystem::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        close(fd);
        return nullptr;
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    void* base = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED)
        return nullptr;
    // Start reading it into the page cache; the process takes the pages
    // only once something reads them.
    madvise(base, size, MADV_WILLNEED);
    return std::shared_ptr<const MappedFile>(new MappedFile(static_cast<const std::uint8_t*>(base), size));
}

MappedFile::~MappedFile()
{
    munmap(const_cast<std::uint8_t*>(base_), size_);
}

void MappedFile::evict(const std::uint8_t*, std::size_t) const
{
    madvise(const_cast<std::uint8_t*>(base_), size_, MADV_DONTNEED);
}

DerivedCache::DerivedCache(std::filesystem::path dir, std::uint64_t maxBytes)
    : dir_(std::move(dir)), maxBytes_(maxBytes)
{
    if (!enabled())
        return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    trim();
}

std::array<std::uint8_t, 16> DerivedCache::key(std::string_view source)
{
    const XXH128_hash_t hash = XXH3_128bits(source.data(), source.size());
    std::array<std::uint8_t, 16> key{};
    for (int i = 0; i < 8; ++i) {
        key[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(hash.high64 >> (56 - 8 * i));
        key[static_cast<std::size_t>(8 + i)] = static_cast<std::uint8_t>(hash.low64 >> (56 - 8 * i));
    }
    return key;
}

std::string DerivedCache::name(std::string_view source, std::string_view kind)
{
    std::string name;
    name.reserve(33 + kind.size());
    for (std::uint8_t byte : key(source))
        name += std::format("{:02x}", byte);
    name += '-';
    name += kind;
    return name;
}

std::shared_ptr<const MappedFile> DerivedCache::open(std::string_view name) const
{
    if (!enabled())
        return nullptr;
    const std::filesystem::path file = path(name);
    auto mapped = MappedFile::open(file);
    if (mapped) {
        // Used now: the last to go when the directory is trimmed.
        std::error_code ec;
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
    }
    return mapped;
}

bool writeWhole(const std::filesystem::path& file, std::span<const std::span<const std::uint8_t>> parts)
{
    // A name no other writer has, in this process (the counter) or another
    // (its pid), created afresh: two writers of one file never write the
    // same temporary, and nobody sees it until it's whole and renamed.
    static std::atomic<std::uint64_t> next{0};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path temporary = file;
    temporary += std::format(".{}.{}.part", getpid(), next++);
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool written = true;
    for (std::span<const std::uint8_t> part : parts) {
        while (written && !part.empty()) {
            const ssize_t n = ::write(fd, part.data(), part.size());
            if (n <= 0)
                written = false;
            else
                part = part.subspan(static_cast<std::size_t>(n));
        }
    }
    written = close(fd) == 0 && written;
    if (written)
        std::filesystem::rename(temporary, file, ec);
    if (!written || ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool DerivedCache::write(std::string_view name, std::span<const std::span<const std::uint8_t>> parts) const
{
    return enabled() && writeWhole(path(name), parts);
}

void DerivedCache::trim() const
{
    if (!enabled())
        return;
    struct File {
        std::filesystem::path path;
        std::filesystem::file_time_type used;
        std::uint64_t bytes = 0;
    };
    std::vector<File> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        std::error_code fileError;
        if (!entry.is_regular_file(fileError))
            continue;
        File file{entry.path(), entry.last_write_time(fileError), entry.file_size(fileError)};
        if (fileError)
            continue;
        // A part a crashed writer left: an hour on, nobody is writing it.
        if (file.path.extension() == ".part") {
            if (std::filesystem::file_time_type::clock::now() - file.used > std::chrono::hours(1))
                std::filesystem::remove(file.path, fileError);
            continue;
        }
        total += file.bytes;
        files.push_back(std::move(file));
    }
    if (total <= maxBytes_)
        return;
    // Oldest first, down to nine tenths so it isn't trimmed again at once.
    // Removing a file another process has mapped leaves its mapping whole.
    std::ranges::sort(files, {}, &File::used);
    const std::uint64_t target = maxBytes_ / 10 * 9;
    for (const File& file : files) {
        if (total <= target)
            break;
        std::error_code removeError;
        if (std::filesystem::remove(file.path, removeError))
            total -= file.bytes;
    }
}

} // namespace lunar::assets
