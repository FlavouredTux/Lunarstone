#include <lunar/net/Datagram.hpp>

#include <lunar/net/Offline.hpp>

#include <algorithm>
#include <bit>
#include <format>

namespace lunar::net {

const char* reliabilityName(Reliability r)
{
    switch (r) {
    case Reliability::Unreliable:
        return "UNRELIABLE";
    case Reliability::UnreliableSequenced:
        return "UNRELIABLE_SEQUENCED";
    case Reliability::Reliable:
        return "RELIABLE";
    case Reliability::ReliableOrdered:
        return "RELIABLE_ORDERED";
    case Reliability::ReliableSequenced:
        return "RELIABLE_SEQUENCED";
    case Reliability::UnreliableWithAckReceipt:
        return "UNRELIABLE_WITH_ACK_RECEIPT";
    case Reliability::ReliableWithAckReceipt:
        return "RELIABLE_WITH_ACK_RECEIPT";
    case Reliability::ReliableOrderedWithAckReceipt:
        return "RELIABLE_ORDERED_WITH_ACK_RECEIPT";
    }
    return "?";
}

HeaderOptions headerOptions(std::uint64_t negotiated)
{
    HeaderOptions o;
    o.flagA = (negotiated & caps::HeaderFlagA) != 0;
    o.flagB = (negotiated & caps::HeaderFlagB) != 0;
    o.trailingU16 = (negotiated & caps::NoDatagramU16) == 0;
    return o;
}

void DatagramHeader::write(BitStream& out, const HeaderOptions& options) const
{
    out.writeBit(true); // isValid
    if (isAck) {
        out.writeBit(true);
        out.writeBit(hasBAndAs);
        out.writeBit(ackExtra);
        out.alignWrite();
        if (options.timestamp)
            out.writeU32(sourceTime);
        if (hasBAndAs)
            out.writeFloat(as);
        return;
    }
    out.writeBit(false);
    if (isNak) {
        out.writeBit(true);
        out.alignWrite();
        return;
    }
    out.writeBit(false);
    if (options.flagA)
        out.writeBit(flagA);
    if (options.flagB)
        out.writeBit(flagB);
    out.writeBit(false); // needsBAndAs: the client always writes 0
    out.writeBit(isPacketPair);
    out.writeBit(isContinuousSend);
    out.alignWrite();
    if (options.timestamp)
        out.writeU32(sourceTime);
    out.writeU24(datagramNumber);
    if (options.trailingU16)
        out.writeU16(trailing);
}

bool DatagramHeader::read(BitStream& in, const HeaderOptions& options)
{
    bool valid = false;
    in.readBit(valid);
    in.readBit(isAck);
    if (isAck) {
        in.readBit(hasBAndAs);
        in.readBit(ackExtra);
        in.alignRead();
        if (options.timestamp)
            in.readU32(sourceTime);
        if (hasBAndAs)
            in.readFloat(as);
        return valid && !in.bad();
    }
    in.readBit(isNak);
    if (isNak) {
        in.alignRead();
        return valid && !in.bad();
    }
    if (options.flagA)
        in.readBit(flagA);
    if (options.flagB)
        in.readBit(flagB);
    in.readBit(needsBAndAs);
    in.readBit(isPacketPair);
    in.readBit(isContinuousSend);
    in.alignRead();
    if (options.timestamp)
        in.readU32(sourceTime);
    in.readU24(datagramNumber);
    if (options.trailingU16)
        in.readU16(trailing);
    return valid && !in.bad();
}

std::size_t writeRanges(BitStream& out, Ranges& ranges, std::size_t maxBits)
{
    // 0x102d8aa04 builds the entries in a scratch stream while its size
    // estimate (81 bits + 40 or 72 per range) stays within maxBits.
    BitStream entries;
    std::size_t count = 0, estimate = 0;
    if (maxBits >= 0x51) {
        while (count < ranges.size() && estimate + 0x51 <= maxBits) {
            const auto [lo, hi] = ranges[count];
            entries.writeU8(lo == hi ? 1 : 0);
            entries.writeU24(lo);
            if (lo != hi)
                entries.writeU24(hi);
            estimate += lo == hi ? 0x28 : 0x48;
            ++count;
        }
    }
    out.alignWrite();
    out.writeU16(static_cast<std::uint16_t>(count));
    out.writeBitsFrom(entries, entries.bitsUsed());
    ranges.erase(ranges.begin(), ranges.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

bool readRanges(BitStream& in, Ranges& ranges)
{
    ranges.clear();
    in.alignRead();
    std::uint16_t count = 0;
    if (!in.readU16(count))
        return false;
    for (unsigned i = 0; i < count; ++i) {
        std::uint8_t equal = 0;
        std::uint32_t lo = 0, hi = 0;
        if (!in.readU8(equal) || !in.readU24(lo))
            return false;
        hi = lo;
        if (!equal && !in.readU24(hi))
            return false;
        if (lo > hi)
            return false; // "minIndex > maxIndex"
        ranges.emplace_back(lo, hi);
    }
    return true;
}

Ranges toRanges(std::vector<std::uint32_t> numbers)
{
    std::ranges::sort(numbers);
    auto [first, last] = std::ranges::unique(numbers);
    numbers.erase(first, last);
    Ranges out;
    for (std::uint32_t n : numbers) {
        if (!out.empty() && out.back().second + 1 == n)
            out.back().second = n;
        else
            out.emplace_back(n, n);
    }
    return out;
}

void InternalPacket::write(BitStream& out, bool tracer) const
{
    out.alignWrite();
    // *_WITH_ACK_RECEIPT goes out as its plain counterpart.
    Reliability wire = reliability;
    if (wire == Reliability::UnreliableWithAckReceipt)
        wire = Reliability::Unreliable;
    else if (wire == Reliability::ReliableWithAckReceipt)
        wire = Reliability::Reliable;
    else if (wire == Reliability::ReliableOrderedWithAckReceipt)
        wire = Reliability::ReliableOrdered;
    out.writeBits(static_cast<std::uint8_t>(wire), 3);
    out.writeBit(split);
    if (tracer)
        out.writeBit(false);
    out.alignWrite();
    out.writeU16(static_cast<std::uint16_t>(bitLength));
    if (isReliable(reliability))
        out.writeU24(reliableMessageNumber);
    if (isSequenced(reliability))
        out.writeU24(sequencingIndex);
    if (isOrderedOrSequenced(reliability)) {
        out.writeU24(orderingIndex);
        out.writeU8(orderingChannel);
    }
    if (split) {
        out.writeU32(splitPacketCount);
        out.writeU16(splitPacketId);
        out.writeU32(splitPacketIndex);
    }
    out.writeBytes(std::span(payload.data(), (bitLength + 7) / 8));
}

std::size_t InternalPacket::headerBytes() const
{
    std::size_t n = 1 + 2;
    if (isReliable(reliability))
        n += 3;
    if (isSequenced(reliability))
        n += 3;
    if (isOrderedOrSequenced(reliability))
        n += 4;
    if (split)
        n += 10;
    return n;
}

bool InternalPacket::read(BitStream& in, bool tracer, std::string& error)
{
    in.alignRead();
    std::uint64_t rel = 0;
    in.readBits(rel, 3);
    reliability = static_cast<Reliability>(rel);
    in.readBit(split);
    if (tracer) {
        bool hasTracer = false;
        in.readBit(hasTracer);
        if (hasTracer) {
            error = "network tracer blob (not decoded)";
            return false;
        }
    }
    in.alignRead();
    std::uint16_t bits = 0;
    in.readU16(bits);
    bitLength = bits;
    if (isReliable(reliability) && rel <= 4)
        in.readU24(reliableMessageNumber);
    if (isSequenced(reliability))
        in.readU24(sequencingIndex);
    if (isOrderedOrSequenced(reliability)) {
        in.readU24(orderingIndex);
        in.readU8(orderingChannel);
    }
    if (split) {
        in.readU32(splitPacketCount);
        in.readU16(splitPacketId);
        in.readU32(splitPacketIndex);
    }
    if (in.bad()) {
        error = "truncated internal packet header";
        return false;
    }
    if (bitLength == 0) {
        error = "zero-length internal packet";
        return false;
    }
    if (orderingChannel >= kOrderingChannels) {
        error = "orderingChannel >= NUMBER_OF_ORDERED_STREAMS";
        return false;
    }
    if (split && (splitPacketCount >= 500001 || splitPacketIndex >= splitPacketCount)) {
        error = "bad split fields";
        return false;
    }
    payload.assign((bitLength + 7) / 8, 0);
    if (!in.readBytes(payload)) {
        error = std::format("payload of {} bytes runs past the datagram", payload.size());
        return false;
    }
    return true;
}

Datagram parseDatagram(std::span<const std::uint8_t> plaintext, const HeaderOptions& options)
{
    Datagram d;
    BitStream in(plaintext);
    if (!d.header.read(in, options)) {
        d.error = "bad datagram header";
        return d;
    }
    if (d.header.isAck || d.header.isNak) {
        if (!readRanges(in, d.ranges))
            d.error = d.header.isAck ? "incomingAcks.Deserialize failed" : "incomingNAKs.Deserialize failed";
        return d;
    }
    // CreateInternalPacketFromBitStream gives up below 32 bits.
    while (in.bitsLeft() >= 32) {
        InternalPacket p;
        if (!p.read(in, options.tracer, d.error))
            break;
        d.packets.push_back(std::move(p));
    }
    return d;
}

} // namespace lunar::net
