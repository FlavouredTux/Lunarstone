#pragma once

// The "Rupp" header (RbxTransport / UDMUX proxy header) that goes in front of
// datagrams to a real game server's UDMUX endpoint, from RobloxPlayer 0.742
// (writer 0x102430bb4, length 0x1024308fe, RBX::Rupp::ClientRuppGenerator).
//
//   u8  byte0, u8 byte1          (header version / flags, see Rupp.cpp)
//   u16 BE totalLength           4 + sum of (2 + length) over the TLVs
//   TLV*: u8 type, u8 length, bytes
//
// Which TLVs the client sends and what they hold is in the research notes
// (notes-join.md); the generator below builds them from the join script's
// token material (TokenGenAlgorithm, PepperId, TokenValue).

#include <lunar/net/Join.hpp>
#include <lunar/net/Offline.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lunar::net {

struct RuppTlv {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> value;
};

struct RuppHeader {
    std::uint8_t byte0 = 0;
    std::uint8_t byte1 = 0;
    std::vector<RuppTlv> tlvs;

    std::size_t length() const;
    std::vector<std::uint8_t> encode() const;
};
// Reads one header from the start of a datagram; nullopt when it isn't one
// (then the datagram starts with the RakNet message itself).
std::optional<RuppHeader> decodeRuppHeader(std::span<const std::uint8_t> data);
// How many bytes of Rupp header precede the RakNet data in a received
// datagram, 0 when none.
std::size_t ruppReplyLength(std::span<const std::uint8_t> data);

// Builds the client's header for a session.
class RuppGenerator {
public:
    // Null when the join script carries no token material.
    static std::shared_ptr<RuppGenerator> fromJoinScript(const JoinScript& script);

    // The header to send now (`nowMs` lets the token rotate as the client's does).
    std::vector<std::uint8_t> header(std::uint64_t nowMs);
    RuppHeader current(std::uint64_t nowMs);
    std::string describe() const;
    // A reply's header: a token TLV (kind != 0) replaces the token, as the
    // client does (0x102d6df78 -> 0x102430a72); true when it changed.
    bool onReply(std::span<const std::uint8_t> header);

    bool onConnected = true; // [code] every datagram, encrypted ones included (0x102d8a3d6)

    // Research variants (notes-join.md 5): the flags byte, a type-6 reverse
    // endpoint (our public address) and a type-8 trace id.
    std::uint8_t flags = 0;
    std::optional<SystemAddress> reverseEndpoint;
    std::optional<std::array<std::uint8_t, 16>> traceId;

    int tokenGenAlgorithm = 0;
    std::int64_t pepperId = 0;
    std::vector<std::uint8_t> tokenValue; // decoded TokenValue
    std::uint8_t tokenKind = 1;           // 1 for the join script's token; a reply's (2 live) once replaced
    SystemAddress server;                 // MachineAddress:ServerPort, the real server behind the proxy

    int tokenUpdates = 0;
};

} // namespace lunar::net
