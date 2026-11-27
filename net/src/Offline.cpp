#include <lunar/net/Offline.hpp>

#include <lunar/net/BitStream.hpp>

#include <algorithm>
#include <format>

namespace lunar::net {

namespace {

bool readMagic(BitStream& in)
{
    std::array<std::uint8_t, 16> magic{};
    return in.readBytes(magic) && magic == kOfflineMagic;
}

void writeString8(BitStream& out, const std::string& s)
{
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    out.writeU8(static_cast<std::uint8_t>(n));
    out.writeBytes(std::string_view(s).substr(0, n));
}

bool readString8(BitStream& in, std::string& s)
{
    std::uint8_t n = 0;
    return in.readU8(n) && in.readBytes(n, s);
}

std::unexpected<std::string> fail(std::string why)
{
    return std::unexpected(std::move(why));
}

} // namespace

std::string SystemAddress::toString() const
{
    return std::format("{}.{}.{}.{}:{}", ip[0], ip[1], ip[2], ip[3], port);
}

void writeAddress(BitStream& out, const SystemAddress& address)
{
    out.writeU8(4);
    for (std::uint8_t b : address.ip)
        out.writeU8(static_cast<std::uint8_t>(~b));
    out.writeU16(address.port);
}

bool readAddress(BitStream& in, SystemAddress& address)
{
    std::uint8_t version = 0;
    if (!in.readU8(version))
        return false;
    if (version != 4)
        return false; // GUESS: IPv6 (version 6) carries a sockaddr_in6; never seen from the client
    for (std::uint8_t& b : address.ip) {
        if (!in.readU8(b))
            return false;
        b = static_cast<std::uint8_t>(~b);
    }
    return in.readU16(address.port);
}

bool isRejectionId(std::uint8_t id)
{
    // 0x102d6aede: the ids < 0x1b in bitmask 0x7962800.
    return id <= 0x1a && ((0x7962800u >> id) & 1u) != 0;
}

bool isOfflineId(std::uint8_t id)
{
    return id == offline::OpenRequest1 || id == offline::OpenReply1 || id == offline::OpenRequest2
           || id == offline::OpenReply2 || isRejectionId(id);
}

std::vector<std::uint8_t> encodeOpenRequest1(const OpenRequest1& m)
{
    BitStream out;
    out.writeU8(offline::OpenRequest1);
    out.writeBytes(kOfflineMagic);
    out.writeU8(m.protocol);
    out.writeU8(m.ruppOptIn ? 1 : 0);
    out.padToBytes(m.mtu > kMtuOverhead ? m.mtu - kMtuOverhead : 0);
    return std::move(out).take();
}

std::expected<OpenRequest1, std::string> decodeOpenRequest1(std::span<const std::uint8_t> data)
{
    BitStream in(data);
    std::uint8_t id = 0;
    OpenRequest1 m;
    std::uint8_t opt = 0;
    if (data.size() < 18 || !in.readU8(id) || id != offline::OpenRequest1 || !readMagic(in) || !in.readU8(m.protocol))
        return fail(std::format("RbxOpenRequest1 has incorrect size {}", data.size()));
    if (in.readU8(opt))
        m.ruppOptIn = opt != 0;
    m.mtu = data.size() + kMtuOverhead;
    return m;
}

std::vector<std::uint8_t> encodeOpenReply1(const OpenReply1& m)
{
    BitStream out;
    out.writeU8(offline::OpenReply1);
    out.writeBytes(kOfflineMagic);
    out.writeU64(m.serverGuid);
    out.writeU8(m.security);
    out.writeU16(m.mtu);
    return std::move(out).take();
}

std::expected<OpenReply1, std::string> decodeOpenReply1(std::span<const std::uint8_t> data)
{
    if (data.size() != 28)
        return fail(std::format("RbxOpenReply1 has invalid size {}", data.size()));
    BitStream in(data);
    in.skipBits(8);
    OpenReply1 m;
    if (!readMagic(in) || !in.readU64(m.serverGuid) || !in.readU8(m.security) || !in.readU16(m.mtu))
        return fail("RbxOpenReply1: bad magic");
    return m;
}

std::vector<std::uint8_t> encodeOpenRequest2(OpenRequest2Header header, const OpenRequest2Box& box, const Key& key,
                                             std::uint64_t counter, std::size_t padTo)
{
    BitStream aad;
    aad.writeU8(offline::OpenRequest2);
    aad.writeBytes(kOfflineMagic);
    aad.writeU8(header.version);
    aad.writeU8(0);  // aad size, patched below
    aad.writeU16(0); // box size, patched below
    aad.writeU16(header.keyExchangeVersion);
    aad.writeBytes(header.clientPublic);
    if (header.extra)
        writeString8(aad, *header.extra);
    if (!header.preAuth) {
        aad.writeU16(0);
    } else {
        aad.writeU8(header.preAuth->first);
        writeString8(aad, header.preAuth->second);
    }

    BitStream sealed;
    sealed.writeU64(box.capabilities);
    sealed.writeU64(box.clientGuid);
    sealed.writeU16(box.mtu);
    sealed.writeU8(box.cipher);
    writeAddress(sealed, box.serverAddress);
    if (box.secondAddress)
        writeAddress(sealed, *box.secondAddress);
    writeString8(sealed, box.token);

    aad.patchU8(18, static_cast<std::uint8_t>(aad.bytesUsed()));
    aad.patchU16(19, static_cast<std::uint16_t>(sealed.bytesUsed()));
    std::vector<std::uint8_t> out = sealHandshake(aad.bytes(), sealed.bytes(), key, counter);
    if (out.size() < padTo)
        out.resize(padTo, 0);
    return out;
}

std::expected<OpenRequest2Header, std::string> decodeOpenRequest2Header(std::span<const std::uint8_t> data)
{
    if (data.size() < 0x6b)
        return fail(std::format("RbxOpenRequest2 has invalid size {}", data.size()));
    BitStream in(data);
    in.skipBits(8);
    OpenRequest2Header h;
    if (!readMagic(in) || !in.readU8(h.version) || !in.readU8(h.aadSize) || !in.readU16(h.boxSize)
        || !in.readU16(h.keyExchangeVersion) || !in.readBytes(h.clientPublic))
        return fail("RbxOpenRequest2: truncated header");
    // What follows up to aadSize is the optional string and the pre-auth
    // blob; which of them is present depends on FFlags the decoder can't
    // see, so read the common shape: no string, u8 kind + u8 length.
    if (in.readOffset() / 8 + 2 <= h.aadSize) {
        std::uint8_t kind = 0;
        std::string blob;
        if (in.readU8(kind) && readString8(in, blob) && (kind != 0 || !blob.empty()))
            h.preAuth = std::pair{kind, std::move(blob)};
    }
    if (h.aadSize <= 0x34)
        return fail(std::format("RbxOpenRequest2 unexpected aad size {}", h.aadSize));
    if (std::size_t{h.aadSize} + h.boxSize + kHandshakeOverhead > data.size())
        return fail(std::format("RbxOpenRequest2 declared box size {} exceeds received size", h.boxSize));
    return h;
}

std::expected<OpenRequest2Box, std::string> decodeOpenRequest2Box(std::span<const std::uint8_t> boxBytes)
{
    BitStream in(boxBytes);
    OpenRequest2Box box;
    if (!in.readU64(box.capabilities) || !in.readU64(box.clientGuid) || !in.readU16(box.mtu) || !in.readU8(box.cipher)
        || !readAddress(in, box.serverAddress))
        return fail("RbxOpenRequest2: truncated box");
    // Lunarstone never sends the FFlag-gated second address; a box with one
    // has more than the token's length byte left.
    if (in.bitsLeft() >= std::size_t{64}) {
        SystemAddress second;
        const std::size_t at = in.readOffset();
        std::uint8_t v = 0;
        if (in.readU8(v) && v == 4) {
            in.setReadOffset(at);
            if (readAddress(in, second))
                box.secondAddress = second;
        } else {
            in.setReadOffset(at);
        }
    }
    readString8(in, box.token);
    return box;
}

std::expected<OpenRequest2Box, std::string> openOpenRequest2Box(std::span<const std::uint8_t> data,
                                                                const OpenRequest2Header& header, const Key& key)
{
    auto box = openHandshake(data, header.aadSize, header.boxSize, key);
    if (!box)
        return fail("RbxOpenRequest2 decrypt failed");
    return decodeOpenRequest2Box(*box);
}

std::vector<std::uint8_t> encodeOpenReply2(OpenReply2Header header, const OpenReply2Box& box, const Key& key,
                                           std::uint64_t counter)
{
    BitStream aad;
    aad.writeU8(offline::OpenReply2);
    aad.writeBytes(kOfflineMagic);
    aad.writeU8(header.tokenReplied);
    aad.writeU8(0);
    aad.writeU16(0);

    BitStream sealed;
    sealed.writeBytes(box.serverSessionPublic);
    sealed.writeU64(box.capabilities);
    sealed.writeU64(box.serverGuid);
    sealed.writeU16(box.mtu);
    sealed.writeU8(box.cipher);
    writeAddress(sealed, box.clientAddress);

    aad.patchU8(18, static_cast<std::uint8_t>(aad.bytesUsed()));
    aad.patchU16(19, static_cast<std::uint16_t>(sealed.bytesUsed()));
    return sealHandshake(aad.bytes(), sealed.bytes(), key, counter);
}

std::expected<OpenReply2Header, std::string> decodeOpenReply2Header(std::span<const std::uint8_t> data)
{
    if (data.size() <= 0x6a)
        return fail(std::format("RbxOpenReply2 invalid size {}", data.size()));
    BitStream in(data);
    in.skipBits(8);
    OpenReply2Header h;
    if (!readMagic(in) || !in.readU8(h.tokenReplied) || !in.readU8(h.aadSize) || !in.readU16(h.boxSize))
        return fail("RbxOpenReply2: truncated header");
    if (std::size_t{h.aadSize} + h.boxSize + kHandshakeOverhead > data.size())
        return fail("RbxOpenReply2: box exceeds received size");
    return h;
}

std::optional<std::vector<std::uint8_t>> openOpenReply2BoxBytes(std::span<const std::uint8_t> data,
                                                                const OpenReply2Header& header, const Key& key)
{
    if (header.aadSize < kOpenReply2HeaderSize || data.size() < std::size_t{header.aadSize} + kHandshakeOverhead)
        return std::nullopt;
    if (header.aadSize == kOpenReply2HeaderSize)
        if (auto box = openHandshake(data, header.aadSize, header.boxSize, key))
            return box;
    // The encrypted part runs from the end of the header to the nonce, which
    // with the tag is the last 28 bytes.
    const std::size_t sealed = data.size() - header.aadSize - kHandshakeOverhead;
    auto tail = openHandshakeAt(data, header.aadSize, header.aadSize, sealed, key);
    if (!tail)
        return std::nullopt;
    std::vector<std::uint8_t> box(data.begin() + kOpenReply2HeaderSize, data.begin() + header.aadSize);
    box.insert(box.end(), tail->begin(), tail->end());
    if (box.size() < header.boxSize)
        return std::nullopt;
    box.resize(header.boxSize);
    return box;
}

std::expected<OpenReply2Box, std::string> decodeOpenReply2Box(std::span<const std::uint8_t> boxBytes)
{
    BitStream in(boxBytes);
    OpenReply2Box box;
    if (!in.readBytes(box.serverSessionPublic) || !in.readU64(box.capabilities) || !in.readU64(box.serverGuid)
        || !in.readU16(box.mtu) || !in.readU8(box.cipher) || !readAddress(in, box.clientAddress))
        return fail("RbxOpenReply2: truncated box");
    return box;
}

std::expected<OpenReply2Box, std::string> openOpenReply2Box(std::span<const std::uint8_t> data,
                                                            const OpenReply2Header& header, const Key& key)
{
    auto box = openOpenReply2BoxBytes(data, header, key);
    if (!box)
        return fail("RbxOpenReply2 decrypt failed");
    return decodeOpenReply2Box(*box);
}

std::vector<std::uint8_t> encodeRejection(const Rejection& m)
{
    BitStream out;
    // 0x102d75c88 writes no protocol byte, even for 0x19 (the client's
    // reader, RakNet style, skips one there anyway).
    out.writeU8(m.id);
    out.writeBytes(kOfflineMagic);
    out.writeU64(m.guid);
    return std::move(out).take();
}

std::expected<Rejection, std::string> decodeRejection(std::span<const std::uint8_t> data)
{
    BitStream in(data);
    Rejection m;
    if (!in.readU8(m.id) || !isRejectionId(m.id) || !readMagic(in))
        return fail("not a rejection");
    if (m.id == offline::IncompatibleProtocolVersion && data.size() >= 26 && !in.readU8(m.protocol))
        return fail("truncated rejection");
    if (!in.readU64(m.guid))
        return fail("truncated rejection");
    return m;
}

} // namespace lunar::net
