#pragma once

#include <lunar/net/Capture.hpp>
#include <lunar/net/Crypto.hpp>
#include <lunar/net/Datagram.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Replication.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lunar::net {

// Turns a capture into the readable tree, datagram by datagram (--net-decode).
// Follows each client/server flow: the handshake is decrypted with the
// version-0 local server keys when it used them, connected datagrams with the
// session keys from a keylog (LUNAR_NET_KEYLOG when the capture was made);
// split messages are reassembled and printed whole once complete.
class CaptureDecoder {
public:
    explicit CaptureDecoder(DescribeOptions options = {}) : options_(options) {}

    // Lines "LUNARNET_SESSION client=... server=... cipher=N caps=0x... c2s=hex s2c=hex".
    std::size_t loadKeylog(const std::filesystem::path& path);

    // The tree for one datagram, in capture order (an empty node when quiet).
    Node decode(const CapturedDatagram& datagram, std::size_t index);

    // What each flow's client would have built so far, flows in the order seen.
    std::vector<std::shared_ptr<Replica>> replicas() const;

private:
    struct Direction {
        std::optional<DatagramCipher> cipher;
        std::map<std::uint16_t, std::vector<std::optional<std::vector<std::uint8_t>>>> splits;
    };
    struct Flow {
        SystemAddress client, server;
        std::optional<Key> clientPublic;
        std::optional<SessionKeys> handshakeServer; // kxServer(local early-ephemeral, client key)
        std::uint64_t capabilities = kBaseCapabilities;
        Direction toServer, toClient;
        std::shared_ptr<Replica> replica = std::make_shared<Replica>(); // what the client would build
        MessageDescriber describer = robloxDescriber(replica);
    };
    struct Session {
        Cipher cipher = Cipher::ChaCha20Poly1305;
        std::uint64_t capabilities = kBaseCapabilities;
        Key c2s{}, s2c{};
    };

    static void reassemble(const Datagram& dg, Direction& dir, Node& n, const DescribeOptions& options);
    // The quiet decode of a connected datagram: its messages straight to the replica.
    static void deliver(const Datagram& dg, Direction& dir, Replica& replica);
    Flow& flowFor(const SystemAddress& from, const SystemAddress& to, std::uint8_t firstByte);
    void attachKeys(Flow& flow);

    DescribeOptions options_;
    std::vector<Flow> flows_;
    std::map<std::string, Session> keylog_; // "client|server"
    std::set<std::string> ruppFlows_;       // endpoint pairs seen with a Rupp header
};

} // namespace lunar::net
