#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

// RBX::Network::NetworkStream (0x1016e8xxx-0x1016ebxxx): what the Roblox
// layer above RakNet writes its packets with. Byte-oriented, integers raw
// little-endian, plus LEB128 varints and zigzag (ROBLOX-LAYER.md section 4;
// varint/zigzag/string pinned to the client under emulation). Reads past the
// end return zeros and mark the stream bad instead of throwing.
class NetworkStream {
public:
    NetworkStream() = default;
    explicit NetworkStream(std::span<const std::uint8_t> data) : data_(data.begin(), data.end()) {}

    void writeU8(std::uint8_t v) { data_.push_back(v); }
    void writeU16(std::uint16_t v) { writeLe(v, 2); }
    void writeU32(std::uint32_t v) { writeLe(v, 4); }
    void writeU64(std::uint64_t v) { writeLe(v, 8); }
    void writeI16(std::int16_t v) { writeU16(static_cast<std::uint16_t>(v)); }
    void writeI32(std::int32_t v) { writeU32(static_cast<std::uint32_t>(v)); }
    void writeF32(float v);
    void writeF64(double v);
    void writeVarint(std::uint64_t v);
    void writeZigzag32(std::int32_t v);
    void writeZigzag64(std::int64_t v);
    void writeString(std::string_view s);   // varint length + bytes
    void writeString32(std::string_view s); // u32 length + bytes
    void writeBytes(std::span<const std::uint8_t> b) { data_.insert(data_.end(), b.begin(), b.end()); }

    std::uint8_t readU8();
    std::uint16_t readU16() { return static_cast<std::uint16_t>(readLe(2)); }
    std::uint32_t readU32() { return static_cast<std::uint32_t>(readLe(4)); }
    std::uint64_t readU64() { return readLe(8); }
    std::int16_t readI16() { return static_cast<std::int16_t>(readU16()); }
    std::int32_t readI32() { return static_cast<std::int32_t>(readU32()); }
    float readF32();
    double readF64();
    std::uint64_t readVarint();
    std::int32_t readZigzag32();
    std::int64_t readZigzag64();
    std::string readString(std::size_t maxLength = 1u << 24);
    std::string readString32(std::size_t maxLength = 1u << 24);
    std::vector<std::uint8_t> readBytes(std::size_t count);

    std::size_t offset() const { return offset_; }
    void seek(std::size_t offset) { offset_ = offset; }
    std::size_t left() const { return offset_ < data_.size() ? data_.size() - offset_ : 0; }
    bool bad() const { return bad_; }
    std::span<const std::uint8_t> bytes() const { return data_; }
    std::vector<std::uint8_t> take() && { return std::move(data_); }

private:
    void writeLe(std::uint64_t v, unsigned n);
    std::uint64_t readLe(unsigned n);

    std::vector<std::uint8_t> data_;
    std::size_t offset_ = 0;
    bool bad_ = false;
};

} // namespace lunar::net
