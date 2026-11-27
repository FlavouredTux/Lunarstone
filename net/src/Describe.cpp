#include <lunar/net/Describe.hpp>

#include <lunar/net/BitStream.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Physics.hpp>

#include <cstdlib>
#include <format>

namespace lunar::net {

namespace {

MessageDescriber& robloxDescriber()
{
    static MessageDescriber d;
    return d;
}

std::string guidText(std::uint64_t g)
{
    return std::format("{:016x}", g);
}

} // namespace

void setRobloxDescriber(MessageDescriber describer)
{
    robloxDescriber() = std::move(describer);
}

std::string render(const Node& node, int indent)
{
    std::string out(static_cast<std::size_t>(indent) * 2, ' ');
    out += node.text;
    out += '\n';
    for (const Node& c : node.children)
        out += render(c, indent + 1);
    return out;
}

std::string hexString(std::span<const std::uint8_t> bytes)
{
    std::string s;
    s.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes)
        s += std::format("{:02x}", b);
    return s;
}

std::string hexString(const Key& key)
{
    return hexString(std::span<const std::uint8_t>(key));
}

void addHex(Node& parent, std::span<const std::uint8_t> bytes, std::size_t baseOffset, std::size_t limit)
{
    const std::size_t shown = std::min(bytes.size(), limit);
    for (std::size_t i = 0; i < shown; i += 16) {
        std::string line = std::format("+0x{:04x} ", baseOffset + i);
        std::string ascii;
        for (std::size_t j = i; j < i + 16; ++j) {
            if (j < shown) {
                line += std::format(" {:02x}", bytes[j]);
                ascii += bytes[j] >= 0x20 && bytes[j] < 0x7f ? static_cast<char>(bytes[j]) : '.';
            } else {
                line += "   ";
            }
        }
        line += "  |";
        line += ascii;
        line += '|';
        parent.add(std::move(line));
    }
    if (shown < bytes.size())
        parent.add(std::format("... {} more bytes", bytes.size() - shown));
}

std::string messageName(std::uint8_t id)
{
    if (const char* n = messageIdName(id))
        return n;
    return std::format("ID_UNKNOWN_{:#04x}", id);
}

Node describeMessage(std::span<const std::uint8_t> payload, const DescribeOptions& options)
{
    if (payload.empty())
        return Node("(empty message)");
    const std::uint8_t id = payload[0];
    Node n(std::format("{} ({:#04x}), {} bytes", messageName(id), id, payload.size()));
    BitStream in(payload);
    in.skipBits(8);
    auto rest = [&] {
        const std::size_t at = in.readOffset() / 8;
        if (at < payload.size()) {
            Node& r = n.add(std::format("unparsed tail at +{}", at));
            addHex(r, payload.subspan(at), at, options.fullHex ? SIZE_MAX : 256);
        }
    };
    switch (id) {
    case msg::ConnectedPing: {
        std::uint64_t t = 0;
        in.readU64(t);
        n.add(std::format("pingTime: {} ms", t));
        rest();
        return n;
    }
    case msg::ConnectedPong: {
        std::uint64_t ping = 0, pong = 0;
        in.readU64(ping);
        in.readU64(pong);
        n.add(std::format("pingTime: {} ms (echo)", ping));
        n.add(std::format("pongTime: {} ms", pong));
        rest();
        return n;
    }
    case msg::ConnectionRequest: {
        std::uint64_t guid = 0, time = 0;
        std::uint8_t security = 0;
        in.readU64(guid);
        in.readU64(time);
        in.readU8(security);
        n.add(std::format("clientGuid: {}", guidText(guid)));
        n.add(std::format("time: {} ms", time));
        n.add(std::format("doSecurity: {}", security));
        if (in.bitsLeft() > 0) {
            std::string password;
            in.readBytes(in.bitsLeft() / 8, password);
            n.add(std::format("password: \"{}\" ({} bytes)", password, password.size()));
        }
        return n;
    }
    case msg::ConnectionRequestAccepted:
    case msg::NewIncomingConnection: {
        SystemAddress a;
        readAddress(in, a);
        n.add(std::format("{}: {}", id == msg::ConnectionRequestAccepted ? "clientAddress" : "serverAddress",
                          a.toString()));
        if (id == msg::ConnectionRequestAccepted) {
            std::uint16_t index = 0;
            in.readU16(index);
            n.add(std::format("systemIndex: {}", index));
        }
        Node& internal = n.add("internalAddresses (10)");
        for (int i = 0; i < 10; ++i) {
            readAddress(in, a);
            internal.add(std::format("[{}] {}", i, a.toString()));
        }
        std::uint64_t t1 = 0, t2 = 0, t3 = 0;
        in.readU64(t1);
        in.readU64(t2);
        in.readU64(t3);
        if (id == msg::ConnectionRequestAccepted) {
            n.add(std::format("requestTime: {} ms (echo of ID_CONNECTION_REQUEST)", t1));
            n.add(std::format("acceptTime: {} ms", t2));
            n.add(std::format("serverClock: {} (GUESS)", t3));
        } else {
            n.add(std::format("acceptTime: {} ms (echo)", t1));
            n.add(std::format("time: {} ms", t2));
            n.add(std::format("wallclock: {} ms", t3));
        }
        rest();
        return n;
    }
    case msg::DisconnectionNotification: {
        std::uint32_t reason = 0;
        if (in.readU32(reason))
            n.add(std::format("reason: {}", reason));
        rest();
        return n;
    }
    case msg::Timestamp: {
        // ID_TIMESTAMP in front of a physics packet (0x85). A replica's
        // describer takes it in its flow's direction (and plays the motion);
        // otherwise, as the direction isn't on the wire, the one that parses
        // (the caller's guess first).
        if (options.roblox && *options.roblox && (*options.roblox)(payload, n))
            return n;
        for (const bool s2c : {options.physicsServerToClient, !options.physicsServerToClient}) {
            Node attempt;
            if (decodePhysicsPacket(payload, s2c, {}, &attempt)) {
                for (Node& child : attempt.children)
                    n.add(std::move(child));
                return n;
            }
        }
        break;
    }
    default:
        break;
    }
    // research: LUNAR_NET_MSGHEX=<n> adds each message's first n bytes as decoded (0: all of them).
    if (const char* hex = std::getenv("LUNAR_NET_MSGHEX"); hex && *hex) {
        const long limit = std::atol(hex);
        addHex(n.add("message bytes"), payload, 0, limit > 0 ? static_cast<std::size_t>(limit) : SIZE_MAX);
    }
    if (options.roblox && *options.roblox && (*options.roblox)(payload, n))
        return n;
    if (!options.roblox && robloxDescriber() && robloxDescriber()(payload, n))
        return n;
    Node& raw = n.add("payload (not decoded)");
    addHex(raw, payload, 0, options.fullHex ? SIZE_MAX : 256);
    return n;
}

Node describeDatagram(const Datagram& d, std::size_t plainBytes, const DescribeOptions& options)
{
    const DatagramHeader& h = d.header;
    Node n;
    if (h.isAck || h.isNak) {
        n.text = std::format("{} datagram, {} bytes plaintext", h.isAck ? "ACK" : "NAK", plainBytes);
        if (h.isAck)
            n.add(std::format("flags: hasBAndAS={} ackExtra={}", h.hasBAndAs, h.ackExtra));
        Node& r = n.add(std::format("ranges ({})", d.ranges.size()));
        for (auto [lo, hi] : d.ranges)
            r.add(lo == hi ? std::format("#{}", lo) : std::format("#{}..#{}", lo, hi));
    } else {
        n.text = std::format("DATA datagram #{}, {} bytes plaintext, {} packet(s)", h.datagramNumber, plainBytes,
                             d.packets.size());
        n.add(std::format("flags: A={} B={} needsBAndAs={} packetPair={} continuousSend={}", h.flagA, h.flagB,
                          h.needsBAndAs, h.isPacketPair, h.isContinuousSend));
        for (const InternalPacket& p : d.packets) {
            std::string head = std::format("packet {} {} bits", reliabilityName(p.reliability), p.bitLength);
            if (isReliable(p.reliability))
                head += std::format(" rel#{}", p.reliableMessageNumber);
            if (isSequenced(p.reliability))
                head += std::format(" seq#{}", p.sequencingIndex);
            if (isOrderedOrSequenced(p.reliability))
                head += std::format(" ord#{} ch{}", p.orderingIndex, p.orderingChannel);
            Node& pn = n.add(head);
            if (p.split) {
                pn.add(std::format("split id {} piece {}/{}", p.splitPacketId, p.splitPacketIndex + 1,
                                   p.splitPacketCount));
                if (p.splitPacketIndex == 0)
                    pn.add(
                        std::format("first piece starts with {}", messageName(p.payload.empty() ? 0 : p.payload[0])));
                addHex(pn, p.payload, 0, options.fullHex ? SIZE_MAX : 64);
            } else {
                pn.add(describeMessage(p.payload, options));
            }
        }
    }
    if (!d.error.empty())
        n.add("parse stopped: " + d.error);
    return n;
}

Node describeOffline(std::span<const std::uint8_t> udp, const DescribeOptions& options,
                     const std::vector<std::uint8_t>* openedBox)
{
    if (udp.empty())
        return Node("(empty datagram)");
    const std::uint8_t id = udp[0];
    Node n;
    switch (id) {
    case offline::OpenRequest1: {
        n.text = std::format("RbxOpenRequest1 (0x7b), {} bytes", udp.size());
        if (auto m = decodeOpenRequest1(udp)) {
            n.add("magic: RakNet offline id");
            n.add(std::format("protocol: {}", m->protocol));
            n.add(std::format("ruppOptIn: {}", m->ruppOptIn));
            n.add(std::format("padding to {} bytes: MTU probe {}", udp.size(), m->mtu));
        } else {
            n.add(m.error());
        }
        break;
    }
    case offline::OpenReply1: {
        n.text = std::format("RbxOpenReply1 (0x7e), {} bytes", udp.size());
        if (auto m = decodeOpenReply1(udp)) {
            n.add(std::format("serverGuid: {}", guidText(m->serverGuid)));
            n.add(std::format("security: {}", m->security));
            n.add(std::format("mtu: {}", m->mtu));
        } else {
            n.add(m.error());
        }
        break;
    }
    case offline::OpenRequest2: {
        n.text = std::format("RbxOpenRequest2 (0x78), {} bytes", udp.size());
        auto h = decodeOpenRequest2Header(udp);
        if (!h) {
            n.add(h.error());
            break;
        }
        Node& aad = n.add(std::format("clear part (AAD), {} bytes", h->aadSize));
        aad.add(std::format("version: {}", h->version));
        aad.add(std::format("aadSize: {}  boxSize: {}", h->aadSize, h->boxSize));
        aad.add(std::format("keyExchangeVersion: {}{}", h->keyExchangeVersion,
                            h->keyExchangeVersion == 0 ? " (local server keys)" : ""));
        aad.add("clientPublicKey: " + hexString(h->clientPublic));
        if (h->preAuth)
            aad.add(std::format("preAuth: kind {} \"{}\"", h->preAuth->first, h->preAuth->second));
        else
            aad.add("preAuth: none");
        Node& box = n.add(std::format("encrypted box, {} bytes + nonce + tag", h->boxSize));
        const std::size_t ct = h->aadSize;
        if (openedBox) {
            if (auto b = decodeOpenRequest2Box(*openedBox)) {
                box.add("(decrypted with the local server keys)");
                box.add(std::format("capabilities: {:#018x}", b->capabilities));
                box.add(std::format("clientGuid: {}", guidText(b->clientGuid)));
                box.add(std::format("mtu: {}", b->mtu));
                box.add(std::format("cipher: {} ({})", b->cipher, cipherName(static_cast<Cipher>(b->cipher))));
                box.add("serverAddress: " + b->serverAddress.toString());
                if (b->secondAddress)
                    box.add("secondAddress: " + b->secondAddress->toString());
                box.add(std::format("token: \"{}\"", b->token));
            }
        } else {
            addHex(box, udp.subspan(ct, std::min<std::size_t>(h->boxSize, udp.size() - ct)), ct,
                   options.fullHex ? SIZE_MAX : 64);
        }
        const std::size_t nonceAt = ct + h->boxSize;
        if (nonceAt + kHandshakeOverhead <= udp.size()) {
            box.add("nonce: " + hexString(udp.subspan(nonceAt, 12)));
            box.add("tag: " + hexString(udp.subspan(nonceAt + 12, 16)));
            n.add(std::format("zero padding: {} bytes (MTU probe size)", udp.size() - nonceAt - kHandshakeOverhead));
        }
        break;
    }
    case offline::OpenReply2: {
        n.text = std::format("RbxOpenReply2 (0x7d), {} bytes", udp.size());
        auto h = decodeOpenReply2Header(udp);
        if (!h) {
            n.add(h.error());
            break;
        }
        Node& aad = n.add(std::format("clear part (AAD), {} bytes", h->aadSize));
        aad.add(std::format("tokenReplied: {}", h->tokenReplied));
        aad.add(std::format("aadSize: {}  boxSize: {}", h->aadSize, h->boxSize));
        Node& box = n.add(std::format("encrypted box, {} bytes + nonce + tag", h->boxSize));
        if (openedBox) {
            if (auto b = decodeOpenReply2Box(*openedBox)) {
                box.add("(decrypted with the local server keys)");
                box.add("serverSessionPublicKey: " + hexString(b->serverSessionPublic));
                box.add(std::format("capabilities: {:#018x}", b->capabilities));
                box.add(std::format("serverGuid: {}", guidText(b->serverGuid)));
                box.add(std::format("mtu: {}", b->mtu));
                box.add(std::format("cipher: {} ({})", b->cipher, cipherName(static_cast<Cipher>(b->cipher))));
                box.add("clientAddress: " + b->clientAddress.toString());
            }
        } else {
            addHex(box, udp.subspan(h->aadSize, h->boxSize), h->aadSize, options.fullHex ? SIZE_MAX : 64);
        }
        const std::size_t nonceAt = std::size_t{h->aadSize} + h->boxSize;
        box.add("nonce: " + hexString(udp.subspan(nonceAt, 12)));
        box.add("tag: " + hexString(udp.subspan(nonceAt + 12, 16)));
        break;
    }
    default:
        if (auto r = decodeRejection(udp)) {
            n.text = std::format("rejection {} ({:#04x}), {} bytes", messageName(id), id, udp.size());
            n.add(std::format("guid: {}", guidText(r->guid)));
        } else {
            n.text = std::format("unknown offline message {:#04x}, {} bytes", id, udp.size());
            addHex(n, udp, 0, options.fullHex ? SIZE_MAX : 128);
        }
        break;
    }
    if (options.fullHex) {
        Node& w = n.add("wire bytes");
        addHex(w, udp);
    }
    return n;
}

} // namespace lunar::net
