#include <lunar/net/NetworkStream.hpp>

#include <bit>

namespace lunar::net {

void NetworkStream::writeLe(std::uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        data_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint64_t NetworkStream::readLe(unsigned n)
{
    if (left() < n) {
        bad_ = true;
        offset_ = data_.size();
        return 0;
    }
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= static_cast<std::uint64_t>(data_[offset_ + i]) << (8 * i);
    offset_ += n;
    return v;
}

void NetworkStream::writeF32(float v)
{
    writeU32(std::bit_cast<std::uint32_t>(v));
}

void NetworkStream::writeF64(double v)
{
    writeU64(std::bit_cast<std::uint64_t>(v));
}

void NetworkStream::writeVarint(std::uint64_t v)
{
    do {
        std::uint8_t b = v & 0x7f;
        v >>= 7;
        if (v)
            b |= 0x80;
        data_.push_back(b);
    } while (v);
}

void NetworkStream::writeZigzag32(std::int32_t v)
{
    const auto u = static_cast<std::uint32_t>(v);
    writeVarint((u << 1) ^ static_cast<std::uint32_t>(v >> 31));
}

void NetworkStream::writeZigzag64(std::int64_t v)
{
    const auto u = static_cast<std::uint64_t>(v);
    writeVarint((u << 1) ^ static_cast<std::uint64_t>(v >> 63));
}

void NetworkStream::writeString(std::string_view s)
{
    writeVarint(s.size());
    data_.insert(data_.end(), s.begin(), s.end());
}

void NetworkStream::writeString32(std::string_view s)
{
    writeU32(static_cast<std::uint32_t>(s.size()));
    data_.insert(data_.end(), s.begin(), s.end());
}

std::uint8_t NetworkStream::readU8()
{
    return static_cast<std::uint8_t>(readLe(1));
}

float NetworkStream::readF32()
{
    return std::bit_cast<float>(readU32());
}

double NetworkStream::readF64()
{
    return std::bit_cast<double>(readU64());
}

std::uint64_t NetworkStream::readVarint()
{
    // sub_1016e996c: at most 10 bytes.
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (left() == 0) {
            bad_ = true;
            return v;
        }
        const std::uint8_t b = data_[offset_++];
        v |= static_cast<std::uint64_t>(b & 0x7f) << (7 * i);
        if (!(b & 0x80))
            return v;
    }
    bad_ = true;
    return v;
}

std::int32_t NetworkStream::readZigzag32()
{
    const auto u = static_cast<std::uint32_t>(readVarint());
    return static_cast<std::int32_t>((u >> 1) ^ (0u - (u & 1)));
}

std::int64_t NetworkStream::readZigzag64()
{
    const std::uint64_t u = readVarint();
    return static_cast<std::int64_t>((u >> 1) ^ (0ULL - (u & 1)));
}

std::string NetworkStream::readString(std::size_t maxLength)
{
    const std::uint64_t n = readVarint();
    if (n > maxLength || n > left()) {
        bad_ = true;
        offset_ = data_.size();
        return {};
    }
    std::string s(reinterpret_cast<const char*>(data_.data() + offset_), static_cast<std::size_t>(n));
    offset_ += static_cast<std::size_t>(n);
    return s;
}

std::string NetworkStream::readString32(std::size_t maxLength)
{
    const std::uint32_t n = readU32();
    if (n > maxLength || n > left()) {
        bad_ = true;
        offset_ = data_.size();
        return {};
    }
    std::string s(reinterpret_cast<const char*>(data_.data() + offset_), n);
    offset_ += n;
    return s;
}

std::vector<std::uint8_t> NetworkStream::readBytes(std::size_t count)
{
    if (count > left()) {
        bad_ = true;
        offset_ = data_.size();
        return {};
    }
    std::vector<std::uint8_t> b(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
    offset_ += count;
    return b;
}

} // namespace lunar::net
