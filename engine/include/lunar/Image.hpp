#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace lunar {

// Texels in a GPU block-compressed format as a DDS or KTX2 file stored
// them: every mip level the file had, the full size first. A renderer that
// can sample the format uploads these as they are.
struct BlockCompressed {
    enum class Format : std::uint8_t { Bc1, Bc2, Bc3, Bc4, Bc5, Bc7 };
    Format format = Format::Bc1;
    std::vector<std::vector<std::uint8_t>> levels;
};

// Read-only bytes a PixelBuffer shows without owning them (a file mapped
// into memory, say), kept alive by the buffers that show them. evict tells
// the owner nobody needs them in memory for now: they must stay readable,
// as when a mapped file's pages are dropped and read back on the next touch.
class ExternalPixels {
public:
    virtual ~ExternalPixels() = default;
    virtual void evict(const std::uint8_t* data, std::size_t size) const = 0;
};

// An image's bytes: its own, like a std::vector (most of whose interface it
// has), or read-only bytes something else holds (ExternalPixels). Reading
// is the same either way; anything that writes, through a non-const
// accessor, first takes a copy of external bytes to own. Copies of a buffer
// own their bytes; moves keep what they move.
class PixelBuffer {
public:
    using value_type = std::uint8_t;
    using iterator = std::uint8_t*;
    using const_iterator = const std::uint8_t*;

    PixelBuffer() = default;
    PixelBuffer(std::vector<std::uint8_t> bytes) : owned_(std::move(bytes)) { sync(); } // NOLINT: as a vector
    PixelBuffer(std::initializer_list<std::uint8_t> bytes) : owned_(bytes) { sync(); }
    PixelBuffer(const PixelBuffer& other) : owned_(other.begin(), other.end()) { sync(); }
    PixelBuffer(PixelBuffer&& other) noexcept { take(std::move(other)); }
    PixelBuffer& operator=(const PixelBuffer& other)
    {
        if (this != &other) {
            std::vector<std::uint8_t> copy(other.begin(), other.end());
            *this = PixelBuffer(std::move(copy));
        }
        return *this;
    }
    PixelBuffer& operator=(PixelBuffer&& other) noexcept
    {
        if (this != &other)
            take(std::move(other));
        return *this;
    }
    PixelBuffer& operator=(std::vector<std::uint8_t> bytes) { return *this = PixelBuffer(std::move(bytes)); }
    PixelBuffer& operator=(std::initializer_list<std::uint8_t> bytes) { return *this = PixelBuffer(bytes); }
    ~PixelBuffer() = default;

    // size bytes at data, which owner keeps alive and readable.
    static PixelBuffer external(const std::uint8_t* data, std::size_t size, std::shared_ptr<const ExternalPixels> owner)
    {
        PixelBuffer buffer;
        buffer.keep_ = std::move(owner);
        buffer.view_ = data;
        buffer.size_ = size;
        return buffer;
    }
    bool isExternal() const { return keep_ != nullptr; }
    // Nobody needs external bytes in memory for now (they stay readable);
    // nothing for owned ones.
    void evict() const
    {
        if (keep_)
            keep_->evict(view_, size_);
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::size_t capacity() const { return keep_ ? size_ : owned_.capacity(); }
    const std::uint8_t* data() const { return view_; }
    std::uint8_t* data()
    {
        own();
        return owned_.data();
    }
    const std::uint8_t& operator[](std::size_t i) const { return view_[i]; }
    std::uint8_t& operator[](std::size_t i)
    {
        own();
        return owned_[i];
    }
    const_iterator begin() const { return view_; }
    const_iterator end() const { return view_ + size_; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator begin() { return data(); }
    iterator end() { return data() + size_; }

    void assign(std::size_t count, std::uint8_t value)
    {
        drop();
        owned_.assign(count, value);
        sync();
    }
    // An iterator range (as std::vector, so assign(16, 0) is a count and a value).
    template <std::input_iterator It>
    void assign(It first, It last)
    {
        std::vector<std::uint8_t> bytes(first, last);
        *this = PixelBuffer(std::move(bytes));
    }
    void assign(std::initializer_list<std::uint8_t> bytes) { *this = PixelBuffer(bytes); }
    void resize(std::size_t count)
    {
        own();
        owned_.resize(count);
        sync();
    }
    void resize(std::size_t count, std::uint8_t value)
    {
        own();
        owned_.resize(count, value);
        sync();
    }
    void reserve(std::size_t count)
    {
        own();
        owned_.reserve(count);
        sync();
    }
    void clear()
    {
        drop();
        owned_.clear();
        sync();
    }
    void push_back(std::uint8_t value)
    {
        own();
        owned_.push_back(value);
        sync();
    }
    template <class It>
    iterator insert(const_iterator at, It first, It last)
    {
        const auto offset = at - view_;
        own();
        auto it = owned_.insert(owned_.begin() + offset, first, last);
        sync();
        return owned_.data() + (it - owned_.begin());
    }
    iterator insert(const_iterator at, std::initializer_list<std::uint8_t> bytes)
    {
        return insert(at, bytes.begin(), bytes.end());
    }

    friend bool operator==(const PixelBuffer& a, const PixelBuffer& b)
    {
        return a.size_ == b.size_ && (a.size_ == 0 || std::memcmp(a.view_, b.view_, a.size_) == 0);
    }
    friend bool operator==(const PixelBuffer& a, const std::vector<std::uint8_t>& b)
    {
        return a.size_ == b.size() && (a.size_ == 0 || std::memcmp(a.view_, b.data(), a.size_) == 0);
    }

private:
    void sync()
    {
        view_ = owned_.data();
        size_ = owned_.size();
    }
    // External bytes become owned ones, for writing.
    void own()
    {
        if (!keep_)
            return;
        std::vector<std::uint8_t> copy(view_, view_ + size_);
        keep_.reset();
        owned_ = std::move(copy);
        sync();
    }
    void drop()
    {
        keep_.reset();
        sync();
    }
    void take(PixelBuffer&& other) noexcept
    {
        owned_ = std::move(other.owned_);
        keep_ = std::move(other.keep_);
        if (keep_) {
            view_ = other.view_;
            size_ = other.size_;
        } else {
            sync();
        }
        other.owned_.clear();
        other.keep_.reset();
        other.sync();
    }

    std::vector<std::uint8_t> owned_;
    std::shared_ptr<const ExternalPixels> keep_;
    const std::uint8_t* view_ = nullptr; // owned_.data(), or the external bytes
    std::size_t size_ = 0;
};

// A decoded 8-bit RGBA image, rows top to bottom, straight (not premultiplied)
// alpha. What image decoders produce and the renderer uploads as a texture.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelBuffer rgba; // width * height * 4 bytes
    // The same image still compressed, when it came that way; rgba is
    // always there too.
    std::shared_ptr<const BlockCompressed> compressed;

    bool empty() const { return width == 0 || height == 0; }
    std::size_t byteSize() const { return static_cast<std::size_t>(width) * height * 4; }
};

} // namespace lunar
