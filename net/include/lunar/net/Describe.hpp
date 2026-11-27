#pragma once

#include <lunar/net/Crypto.hpp>
#include <lunar/net/Datagram.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace lunar::net {

// The human-readable side: every datagram and message as an indented tree of
// named fields. Nothing is skipped silently; whatever isn't understood is
// printed as hex with its offset.
struct Node {
    std::string text;
    std::vector<Node> children;

    Node() = default;
    explicit Node(std::string t) : text(std::move(t)) {}
    Node& add(std::string t)
    {
        children.emplace_back(std::move(t));
        return children.back();
    }
    Node& add(Node n)
    {
        children.push_back(std::move(n));
        return children.back();
    }
};
std::string render(const Node& node, int indent = 0);

// Hex lines ("+0x0010  00 11 22 ...  |..."|) under `parent`; with `limit`
// set, the rest is summarised as "... N more bytes".
void addHex(Node& parent, std::span<const std::uint8_t> bytes, std::size_t baseOffset = 0,
            std::size_t limit = SIZE_MAX);
std::string hexString(std::span<const std::uint8_t> bytes);
std::string hexString(const Key& key);

// Describes a Roblox-layer message into `into`; false when it can't.
using MessageDescriber = std::function<bool(std::span<const std::uint8_t> payload, Node& into)>;

// How much to print.
struct DescribeOptions {
    bool fullHex = false;                     // LUNAR_NET_LOG=2: wire and payload bytes in full
    const MessageDescriber* roblox = nullptr; // per-connection Roblox describer (else the global one)
    bool plaintext = false;                   // --net-decode --plain: connected datagrams are already decrypted
    // A physics packet's direction isn't on the wire; the decoder tries this
    // one first (the flow direction in a capture, the role in a live log).
    bool physicsServerToClient = true;
    // CaptureDecoder: only build the flows' replicas, with no readable tree
    // (decode returns an empty node). The replays and the benchmark, which
    // throw the tree away, take the path a live client's messages take.
    bool quiet = false;
};

// Describes one application message (the payload of an internal packet):
// RakNet's own ids and, through the hook, the Roblox layer.
void setRobloxDescriber(MessageDescriber describer);
Node describeMessage(std::span<const std::uint8_t> payload, const DescribeOptions& options = {});
// The name of a message id ("ID_CONNECTION_REQUEST", "ID_ROBLOX_DATA" ...).
std::string messageName(std::uint8_t id);

// A plaintext connected datagram: header, ACK/NAK ranges or packets with their messages.
Node describeDatagram(const Datagram& datagram, std::size_t plainBytes, const DescribeOptions& options = {});

// An offline (handshake) datagram. When the box was opened, pass it to have its fields printed.
Node describeOffline(std::span<const std::uint8_t> udp, const DescribeOptions& options = {},
                     const std::vector<std::uint8_t>* openedBox = nullptr);

} // namespace lunar::net
