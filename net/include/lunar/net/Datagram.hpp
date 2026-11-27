#pragma once

#include <lunar/net/BitStream.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lunar::net {

// The plaintext of a connected datagram, RobloxPlayer 0.742's ReliabilityLayer
// (PROTOCOL.md section 4; every layout pinned to the client's own serializers
// under emulation, emu/vectors/).

enum class Reliability : std::uint8_t {
    Unreliable = 0,
    UnreliableSequenced = 1,
    Reliable = 2,
    ReliableOrdered = 3,
    ReliableSequenced = 4,
    UnreliableWithAckReceipt = 5,
    ReliableWithAckReceipt = 6,
    ReliableOrderedWithAckReceipt = 7,
};
const char* reliabilityName(Reliability r);
inline bool isReliable(Reliability r)
{
    return ((0xdcu >> static_cast<unsigned>(r)) & 1u) != 0;
}
inline bool isSequenced(Reliability r)
{
    return r == Reliability::UnreliableSequenced || r == Reliability::ReliableSequenced;
}
inline bool isOrderedOrSequenced(Reliability r)
{
    return ((0x9au >> static_cast<unsigned>(r)) & 1u) != 0;
}

enum class Priority : std::uint8_t { Immediate = 0, High = 1, Medium = 2, Low = 3 };

inline constexpr unsigned kOrderingChannels = 32;

// Which optional header fields the negotiated capabilities switch on.
struct HeaderOptions {
    bool flagA = true;        // capability bit 21
    bool flagB = true;        // capability bit 23
    bool trailingU16 = false; // capability bit 7 NOT negotiated
    bool timestamp = false;   // never on in 0.742 (the congestion controllers say no)
    bool tracer = false;      // internal packets carry a tracer bit
};
HeaderOptions headerOptions(std::uint64_t negotiatedCapabilities);

// DatagramHeaderFormat (0x102d8219c / 0x102d82612).
struct DatagramHeader {
    bool isAck = false;
    bool isNak = false;
    // ACK
    bool hasBAndAs = false;
    bool ackExtra = false; // GUESS: Roblox flag, meaning unknown
    float as = 0.0f;
    // data
    bool flagA = false;
    bool flagB = false;
    bool needsBAndAs = false;
    bool isPacketPair = false;
    bool isContinuousSend = false;
    std::uint32_t sourceTime = 0;
    std::uint32_t datagramNumber = 0; // uint24
    std::uint16_t trailing = 0;

    void write(BitStream& out, const HeaderOptions& options) const;
    bool read(BitStream& in, const HeaderOptions& options);
};

// RangeList<uint24> (ACK/NAK payload): inclusive [min, max] runs.
using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
// 0x102d8aa04: serializes as many ranges as fit in maxBits, removes them from
// `ranges` and returns how many went out.
std::size_t writeRanges(BitStream& out, Ranges& ranges, std::size_t maxBits = SIZE_MAX);
bool readRanges(BitStream& in, Ranges& ranges);
// Turns sorted numbers into runs.
Ranges toRanges(std::vector<std::uint32_t> numbers);

// One message (or one split piece) inside a datagram.
struct InternalPacket {
    Reliability reliability = Reliability::Unreliable;
    std::uint32_t reliableMessageNumber = 0;
    std::uint32_t sequencingIndex = 0;
    std::uint32_t orderingIndex = 0;
    std::uint8_t orderingChannel = 0;
    bool split = false;
    std::uint32_t splitPacketCount = 0;
    std::uint16_t splitPacketId = 0;
    std::uint32_t splitPacketIndex = 0;
    std::size_t bitLength = 0; // payload bits; payload.size() == ceil(bitLength / 8)
    std::vector<std::uint8_t> payload;

    // 0x102d8c2e8 / 0x102d87c66.
    void write(BitStream& out, bool tracer = false) const;
    // false on a malformed packet; `error` says why.
    bool read(BitStream& in, bool tracer, std::string& error);
    // Header bytes this packet takes on the wire (without payload).
    std::size_t headerBytes() const;
};

// A whole datagram plaintext, parsed.
struct Datagram {
    DatagramHeader header;
    Ranges ranges;                       // ACK or NAK
    std::vector<InternalPacket> packets; // data
    std::string error;                   // set when parsing stopped early
};
Datagram parseDatagram(std::span<const std::uint8_t> plaintext, const HeaderOptions& options);

} // namespace lunar::net
