#pragma once

#include <lunar/net/Crypto.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lunar::net {

// The handshake before a RakNet connection exists, Roblox's replacement for
// RakNet's ID_OPEN_CONNECTION_REQUEST_1/2 (PROTOCOL.md section 2). All of it
// from RobloxPlayer 0.742: builders 0x102d70436 / 0x102d75ef4 / 0x102d765f2 /
// 0x102d77722, parsers 0x102d6b6c4 / 0x102d6b90a / 0x102d6bd08 / 0x102d6d19c.

inline constexpr std::array<std::uint8_t, 16> kOfflineMagic{0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
                                                            0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};
inline constexpr std::uint8_t kProtocolVersion = 5;  // RbxOpenRequest1 byte 17
inline constexpr std::uint8_t kHandshakeVersion = 3; // RbxOpenRequest2 byte 17
// MTU minus the UDP payload the client fills: it pads to MTU - 40.
inline constexpr std::size_t kMtuOverhead = 40;

namespace offline {
inline constexpr std::uint8_t OpenRequest1 = 0x7b;
inline constexpr std::uint8_t OpenReply1 = 0x7e;
inline constexpr std::uint8_t OpenRequest2 = 0x78;
inline constexpr std::uint8_t OpenReply2 = 0x7d;
// Rejections reuse RakNet's ids.
inline constexpr std::uint8_t OurSystemRequiresSecurity = 0x0b; // no usable cipher
inline constexpr std::uint8_t AlreadyConnected = 0x12;
inline constexpr std::uint8_t NoFreeIncomingConnections = 0x14;
inline constexpr std::uint8_t ConnectionBanned = 0x17;
inline constexpr std::uint8_t InvalidPassword = 0x18;
inline constexpr std::uint8_t IncompatibleProtocolVersion = 0x19;
inline constexpr std::uint8_t IpRecentlyConnected = 0x1a;
} // namespace offline

// RakNet SystemAddress, IPv4 only: u8 4, u32 ~address, u16 port (network order).
struct SystemAddress {
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;

    std::string toString() const;
    bool operator==(const SystemAddress&) const = default;
};

class BitStream;
void writeAddress(BitStream& out, const SystemAddress& address);
bool readAddress(BitStream& in, SystemAddress& address);

// Capability bits the client always sets (0x102d5bc9d); see PROTOCOL.md 2.7.
// Bits 17 and 30 were found by emulating the function with every FFlag off
// (emu/t_request2.py): 0x2c350b70892 in all.
inline constexpr std::uint64_t kBaseCapabilities = 0x20000000082ULL | 0x4000b10810ULL | 0x40000ULL | 0x10000000ULL
                                                   | 0x8300000000ULL | 0x20000ULL | 0x40000000ULL;
namespace caps {
inline constexpr std::uint64_t NoDatagramU16 = 1ULL << 7; // when negotiated, data headers drop their trailing u16
inline constexpr std::uint64_t HeaderFlagA = 1ULL << 21;
inline constexpr std::uint64_t HeaderFlagB = 1ULL << 23;
} // namespace caps

struct OpenRequest1 {
    std::uint8_t protocol = kProtocolVersion;
    bool ruppOptIn = true;
    std::size_t mtu = 1492; // the datagram is padded to mtu - 40 bytes
};

struct OpenReply1 {
    std::uint64_t serverGuid = 0;
    std::uint8_t security = 0;
    std::uint16_t mtu = 0;
};

// The clear part of RbxOpenRequest2.
struct OpenRequest2Header {
    std::uint8_t version = kHandshakeVersion;
    std::uint8_t aadSize = 0;  // filled by encode
    std::uint16_t boxSize = 0; // filled by encode
    std::uint16_t keyExchangeVersion = 0;
    Key clientPublic{};
    std::optional<std::string> extra; // FFlag-gated string, not sent by Lunarstone
    std::optional<std::pair<std::uint8_t, std::string>> preAuth;
};

// The encrypted part of RbxOpenRequest2.
struct OpenRequest2Box {
    std::uint64_t capabilities = kBaseCapabilities;
    std::uint64_t clientGuid = 0;
    std::uint16_t mtu = 0;
    std::uint8_t cipher = 1;
    SystemAddress serverAddress;
    std::optional<SystemAddress> secondAddress; // FFlag-gated, not sent by Lunarstone
    std::string token;
};

struct OpenReply2Header {
    std::uint8_t tokenReplied = 0;
    std::uint8_t aadSize = 0;
    std::uint16_t boxSize = 0;
};

// The box of an RbxOpenReply2, decrypted. The box always starts right after
// the 21-byte header; boxSize is its length. A live server that echoed the
// Rupp token (tokenReplied 1) sends the first aadSize - 21 bytes of the box
// in the clear as part of the authenticated header, encrypts the rest padded
// with zeros, and puts the nonce and tag last [live, 2026-10-12]; a classic
// one has aadSize 21 and the whole box encrypted.
inline constexpr std::size_t kOpenReply2HeaderSize = 21;
struct OpenReply2Header;
std::optional<std::vector<std::uint8_t>> openOpenReply2BoxBytes(std::span<const std::uint8_t> data,
                                                                const OpenReply2Header& header, const Key& key);

struct OpenReply2Box {
    Key serverSessionPublic{};
    std::uint64_t capabilities = kBaseCapabilities;
    std::uint64_t serverGuid = 0;
    std::uint16_t mtu = 0;
    std::uint8_t cipher = 1;
    SystemAddress clientAddress;
};

struct Rejection {
    std::uint8_t id = 0;
    std::uint64_t guid = 0;
    std::uint8_t protocol = 0; // 0x19 from classic RakNet servers only (26-byte form)
};

std::vector<std::uint8_t> encodeOpenRequest1(const OpenRequest1& m);
std::expected<OpenRequest1, std::string> decodeOpenRequest1(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> encodeOpenReply1(const OpenReply1& m);
std::expected<OpenReply1, std::string> decodeOpenReply1(std::span<const std::uint8_t> data);

// `key` is the client's tx key from kxClient(clientKeys, server early-ephemeral public key);
// `counter` the handshake nonce counter (0 for the first box of a connection).
std::vector<std::uint8_t> encodeOpenRequest2(OpenRequest2Header header, const OpenRequest2Box& box, const Key& key,
                                             std::uint64_t counter, std::size_t padTo);
std::expected<OpenRequest2Header, std::string> decodeOpenRequest2Header(std::span<const std::uint8_t> data);
std::expected<OpenRequest2Box, std::string> openOpenRequest2Box(std::span<const std::uint8_t> data,
                                                                const OpenRequest2Header& header, const Key& key);
// The box once decrypted (the decoder prints its fields).
std::expected<OpenRequest2Box, std::string> decodeOpenRequest2Box(std::span<const std::uint8_t> box);

std::vector<std::uint8_t> encodeOpenReply2(OpenReply2Header header, const OpenReply2Box& box, const Key& key,
                                           std::uint64_t counter);
std::expected<OpenReply2Header, std::string> decodeOpenReply2Header(std::span<const std::uint8_t> data);
std::expected<OpenReply2Box, std::string> openOpenReply2Box(std::span<const std::uint8_t> data,
                                                            const OpenReply2Header& header, const Key& key);
std::expected<OpenReply2Box, std::string> decodeOpenReply2Box(std::span<const std::uint8_t> box);

std::vector<std::uint8_t> encodeRejection(const Rejection& m);
std::expected<Rejection, std::string> decodeRejection(std::span<const std::uint8_t> data);
bool isRejectionId(std::uint8_t id);
bool isOfflineId(std::uint8_t id);

} // namespace lunar::net
