#pragma once

#include <cstdint>

namespace lunar::net {

// Message ids (byte 0 of a connected message). RakNet's own are RakNet 4's
// numbering, which the client still uses (its offline dispatcher's rejection
// bitmask and the connection handlers line up with it). Roblox's start at
// 0x81; see ROBLOX-LAYER.md for which of them were read from the binary.
namespace msg {
inline constexpr std::uint8_t ConnectedPing = 0x00;
inline constexpr std::uint8_t ConnectedPong = 0x03;
inline constexpr std::uint8_t ConnectionRequest = 0x09;
inline constexpr std::uint8_t ConnectionRequestAccepted = 0x10;
inline constexpr std::uint8_t ConnectionAttemptFailed = 0x11;
inline constexpr std::uint8_t NewIncomingConnection = 0x13;
inline constexpr std::uint8_t DisconnectionNotification = 0x15;
inline constexpr std::uint8_t ConnectionLost = 0x16;
inline constexpr std::uint8_t InvalidPassword = 0x18;
inline constexpr std::uint8_t Timestamp = 0x1b;

// Roblox.
inline constexpr std::uint8_t SetGlobals = 0x81;
inline constexpr std::uint8_t TeachDescriptorDictionaries = 0x82;
inline constexpr std::uint8_t Data = 0x83;
inline constexpr std::uint8_t Marker = 0x84;
inline constexpr std::uint8_t Physics = 0x85;
inline constexpr std::uint8_t ProtocolMismatch = 0x8e;
inline constexpr std::uint8_t PlaceIdVerification = 0x92;
inline constexpr std::uint8_t SecurityKeyMismatch = 0x95;
} // namespace msg

// "ID_CONNECTED_PING", ..., or nullptr for an id without a name.
const char* messageIdName(std::uint8_t id);

} // namespace lunar::net
