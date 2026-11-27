#include <lunar/net/BitStream.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace lunar::net {

namespace {

bool bitAt(const std::vector<std::uint8_t>& data, std::size_t i)
{
    return ((static_cast<unsigned>(data[i >> 3]) >> (7u - static_cast<unsigned>(i & 7))) & 1u) != 0;
}

} // namespace

void BitStream::reserveBits(std::size_t more)
{
    const std::size_t need = (bits_ + more + 7) / 8;
    if (data_.size() < need)
        data_.resize(std::max({need, data_.size() * 2, std::size_t{64}}));
}

void BitStream::writeBit(bool bit)
{
    reserveBits(1);
    const std::size_t byte = bits_ >> 3;
    const unsigned shift = 7u - static_cast<unsigned>(bits_ & 7);
    if ((bits_ & 7) == 0)
        data_[byte] = 0;
    if (bit)
        data_[byte] = static_cast<std::uint8_t>(data_[byte] | (1u << shift));
    ++bits_;
}

void BitStream::writeBits(std::uint64_t value, unsigned count)
{
    reserveBits(count);
    for (unsigned i = count; i-- > 0;)
        writeBit(((value >> i) & 1u) != 0);
}

void BitStream::writeFloat(float v)
{
    writeU32(std::bit_cast<std::uint32_t>(v));
}

void BitStream::writeU24(std::uint32_t v)
{
    alignWrite();
    writeU8(static_cast<std::uint8_t>(v));
    writeU8(static_cast<std::uint8_t>(v >> 8));
    writeU8(static_cast<std::uint8_t>(v >> 16));
}

void BitStream::writeBytes(std::span<const std::uint8_t> bytes)
{
    alignWrite();
    reserveBits(bytes.size() * 8);
    if (!bytes.empty())
        std::memcpy(data_.data() + (bits_ >> 3), bytes.data(), bytes.size());
    bits_ += bytes.size() * 8;
}

void BitStream::writeBytes(std::string_view bytes)
{
    writeBytes(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

void BitStream::writeBitsFrom(const BitStream& other, std::size_t count)
{
    reserveBits(count);
    for (std::size_t i = 0; i < count; ++i)
        writeBit(bitAt(other.data_, i));
}

void BitStream::padToBytes(std::size_t bytes)
{
    alignWrite();
    while (bytesUsed() < bytes)
        writeU8(0);
}

void BitStream::patchU8(std::size_t byteOffset, std::uint8_t v)
{
    if (byteOffset < data_.size())
        data_[byteOffset] = v;
}

void BitStream::patchU16(std::size_t byteOffset, std::uint16_t v)
{
    patchU8(byteOffset, static_cast<std::uint8_t>(v >> 8));
    patchU8(byteOffset + 1, static_cast<std::uint8_t>(v));
}

bool BitStream::readBit(bool& bit)
{
    if (readOffset_ + 1 > bits_) {
        bad_ = true;
        bit = false;
        return false;
    }
    bit = bitAt(data_, readOffset_);
    ++readOffset_;
    return true;
}

bool BitStream::readBits(std::uint64_t& value, unsigned count)
{
    value = 0;
    if (readOffset_ + count > bits_) {
        bad_ = true;
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        bool bit = false;
        readBit(bit);
        value = (value << 1) | (bit ? 1u : 0u);
    }
    return true;
}

bool BitStream::readU8(std::uint8_t& v)
{
    std::uint64_t x = 0;
    const bool ok = readBits(x, 8);
    v = static_cast<std::uint8_t>(x);
    return ok;
}

bool BitStream::readU16(std::uint16_t& v)
{
    std::uint64_t x = 0;
    const bool ok = readBits(x, 16);
    v = static_cast<std::uint16_t>(x);
    return ok;
}

bool BitStream::readU32(std::uint32_t& v)
{
    std::uint64_t x = 0;
    const bool ok = readBits(x, 32);
    v = static_cast<std::uint32_t>(x);
    return ok;
}

bool BitStream::readU64(std::uint64_t& v)
{
    return readBits(v, 64);
}

bool BitStream::readFloat(float& v)
{
    std::uint32_t x = 0;
    const bool ok = readU32(x);
    v = std::bit_cast<float>(x);
    return ok;
}

bool BitStream::readU24(std::uint32_t& v)
{
    alignRead();
    std::uint8_t b[3]{};
    if (!readBytes(b)) {
        v = 0;
        return false;
    }
    v = static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8
        | static_cast<std::uint32_t>(b[2]) << 16;
    return true;
}

bool BitStream::readBytes(std::span<std::uint8_t> out)
{
    alignRead();
    if (readOffset_ + out.size() * 8 > bits_) {
        bad_ = true;
        return false;
    }
    if (!out.empty())
        std::memcpy(out.data(), data_.data() + (readOffset_ >> 3), out.size());
    readOffset_ += out.size() * 8;
    return true;
}

bool BitStream::readBytes(std::size_t count, std::string& out)
{
    alignRead();
    if (readOffset_ + count * 8 > bits_) {
        bad_ = true;
        out.clear();
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data_.data() + (readOffset_ >> 3)), count);
    readOffset_ += count * 8;
    return true;
}

bool BitStream::skipBits(std::size_t count)
{
    if (readOffset_ + count > bits_) {
        bad_ = true;
        readOffset_ = bits_;
        return false;
    }
    readOffset_ += count;
    return true;
}

} // namespace lunar::net
