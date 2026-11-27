#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

// RakNet's BitStream as RobloxPlayer 0.742 builds it (0x102d5434a and
// friends): bits are packed most significant first, aligned reads and writes
// start at the next byte boundary, integers written with Write<T> are
// big-endian (the client runs on little-endian hosts, where RakNet swaps)
// except uint24_t (reliable/ordering indices, datagram numbers), which RakNet
// writes little-endian (0x102d82536). Reads never throw: a read past the end
// returns false (or zero) and marks the stream bad, so decoders can keep
// going and report how far they got.
class BitStream {
public:
    BitStream() = default;
    explicit BitStream(std::span<const std::uint8_t> data) : data_(data.begin(), data.end()), bits_(data.size() * 8) {}
    explicit BitStream(std::string_view data) : data_(data.begin(), data.end()), bits_(data.size() * 8) {}

    // -- writing
    void writeBit(bool bit);
    // The low `count` bits of `value`, most significant first.
    void writeBits(std::uint64_t value, unsigned count);
    void writeU8(std::uint8_t v) { writeBits(v, 8); }
    void writeU16(std::uint16_t v) { writeBits(v, 16); }
    void writeU32(std::uint32_t v) { writeBits(v, 32); }
    void writeU64(std::uint64_t v) { writeBits(v, 64); }
    void writeFloat(float v);
    // Aligned little-endian 24-bit value (RakNet uint24_t).
    void writeU24(std::uint32_t v);
    void writeBytes(std::span<const std::uint8_t> bytes); // aligned
    void writeBytes(std::string_view bytes);
    // Unaligned copy of `count` bits from another stream's start.
    void writeBitsFrom(const BitStream& other, std::size_t count);
    void alignWrite() { bits_ = (bits_ + 7) & ~std::size_t{7}; }
    void padToBytes(std::size_t bytes);
    // Overwrites bytes already written (handshake length fields are patched in afterwards).
    void patchU8(std::size_t byteOffset, std::uint8_t v);
    void patchU16(std::size_t byteOffset, std::uint16_t v);

    // -- reading
    bool readBit(bool& bit);
    bool readBits(std::uint64_t& value, unsigned count);
    bool readU8(std::uint8_t& v);
    bool readU16(std::uint16_t& v);
    bool readU32(std::uint32_t& v);
    bool readU64(std::uint64_t& v);
    bool readFloat(float& v);
    bool readU24(std::uint32_t& v);
    bool readBytes(std::span<std::uint8_t> out); // aligned
    bool readBytes(std::size_t count, std::string& out);
    bool skipBits(std::size_t count);
    void alignRead() { readOffset_ = (readOffset_ + 7) & ~std::size_t{7}; }

    std::size_t bitsUsed() const { return bits_; }
    std::size_t bytesUsed() const { return (bits_ + 7) / 8; }
    std::size_t readOffset() const { return readOffset_; }
    void setReadOffset(std::size_t bits) { readOffset_ = bits; }
    std::size_t bitsLeft() const { return readOffset_ < bits_ ? bits_ - readOffset_ : 0; }
    bool bad() const { return bad_; }

    std::span<const std::uint8_t> bytes() const { return {data_.data(), bytesUsed()}; }
    std::span<std::uint8_t> mutableBytes() { return {data_.data(), bytesUsed()}; }
    std::vector<std::uint8_t> take() &&
    {
        data_.resize(bytesUsed());
        return std::move(data_);
    }

private:
    void reserveBits(std::size_t more);

    std::vector<std::uint8_t> data_;
    std::size_t bits_ = 0;
    std::size_t readOffset_ = 0;
    bool bad_ = false;
};

} // namespace lunar::net
