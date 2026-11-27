#pragma once

#include <lunar/Image.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lunar::assets {

// A file mapped read-only into memory. As ExternalPixels, evict drops its
// pages from the process; they read back in from the file when touched.
class MappedFile final : public ExternalPixels {
public:
    // The whole file at path, or null.
    static std::shared_ptr<const MappedFile> open(const std::filesystem::path& path);
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() override;

    std::span<const std::uint8_t> bytes() const { return {base_, size_}; }
    void evict(const std::uint8_t* data, std::size_t size) const override;

private:
    MappedFile(const std::uint8_t* base, std::size_t size) : base_(base), size_(size) {}
    const std::uint8_t* base_;
    std::size_t size_;
};

// Writes the parts one after another as the file at path: under a name of
// this writer's own (its process and a count), renamed into place once
// whole, so readers and other writers, in this process or another, never
// see a part-written file or each other's bytes. False if it couldn't.
bool writeWhole(const std::filesystem::path& path, std::span<const std::span<const std::uint8_t>> parts);

// A directory of files made from assets (decoded textures, parsed meshes)
// so that the next time the same bytes come they are read instead of made
// again. A file's name comes from what it was made from and how (name()),
// so different bytes or a different maker never share one.
//
// Files are written whole under a name of the writer's own and renamed into
// place, and never changed after: a reader opens either nothing or a whole
// file, and a file mapped stays as it was while other processes write the
// same name again (a new file) or trim it away (the mapping keeps it). The
// directory keeps to maxBytes: when one of these is made, the files used
// longest ago go (opening one counts as using it).
class DerivedCache {
public:
    // Off when dir is empty.
    DerivedCache(std::filesystem::path dir, std::uint64_t maxBytes);

    bool enabled() const { return !dir_.empty(); }

    // A 32-digit hex key of the source's bytes (XXH3-128), then "-" and
    // kind: what was made and how (its format's version, its parameters).
    static std::string name(std::string_view source, std::string_view kind);
    // The 16 bytes behind name()'s hex, for a file to repeat in its header.
    static std::array<std::uint8_t, 16> key(std::string_view source);

    std::filesystem::path path(std::string_view name) const { return dir_ / name; }
    // The file, mapped; null when there's none (or the cache is off).
    std::shared_ptr<const MappedFile> open(std::string_view name) const;
    // Writes the parts one after another as the file; false if it couldn't.
    bool write(std::string_view name, std::span<const std::span<const std::uint8_t>> parts) const;
    // Removes the files used longest ago until the rest fit in maxBytes
    // (to nine tenths of it), and parts a crashed writer left an hour ago.
    void trim() const;

private:
    std::filesystem::path dir_;
    std::uint64_t maxBytes_ = 0;
};

} // namespace lunar::assets
