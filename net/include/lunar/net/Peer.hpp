#pragma once

#include <lunar/net/Capture.hpp>
#include <lunar/net/Crypto.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Reliability.hpp>
#include <lunar/net/Socket.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

// RakPeer for one socket, as client or server: the Roblox handshake
// (RbxOpenRequest1/2 with the version-0 "local" keys), the session cipher,
// RakNet's connection messages and a ReliabilityLayer per connection. Driven
// by update(); single-threaded.
//
// Logging: LUNAR_NET_LOG=1 prints every datagram and message as a tree,
// =2 adds the bytes in full. LUNAR_NET_PCAP=<file> writes a pcap of the
// wire, LUNAR_NET_KEYLOG=<file> the session keys so --net-decode can read it.
class Peer {
public:
    enum class Role : std::uint8_t { Client, Server };

    struct Options {
        std::uint64_t guid = 0; // 0: random
        std::uint64_t capabilities = kBaseCapabilities;
        std::vector<std::size_t> mtus{1492, 1200, 576}; // GUESS: RakNetMtuValue1/2/3InBytes
        int logLevel = -1;                              // -1: from LUNAR_NET_LOG
        std::string name;                               // log prefix ("client", "server")
        std::string password;                           // ID_CONNECTION_REQUEST password bytes
        unsigned maxConnections = 64;
        MessageDescriber describer; // Roblox-layer messages in the log

        // Joining a real server (Join.hpp): the key exchange version and the
        // server's early public key that seals RbxOpenRequest2 (0 and the
        // hard-coded local key when unset), and the Rupp header (Rupp.hpp)
        // put in front of what goes to the UDMUX proxy. `ruppHeader` is
        // asked for every send, so a provider may rotate its token.
        std::uint16_t keyExchangeVersion = 0;
        std::uint8_t cipher = 0; // the cipher a client asks for: 0 = AES-GCM when available, else ChaCha
        std::optional<Key> serverEphemeralEarlyPublic;
        // The ClientTicket pre-auth (Join.hpp PreAuth): kind and blob in the
        // Request2 header, token in its box. Unset: none (a local server).
        std::optional<std::pair<std::uint8_t, std::string>> preAuth;
        std::string preAuthToken;
        // A server's own early-ephemeral pair for that version (the local keys when unset).
        std::optional<KeyPair> serverEphemeralEarlyPair;
        std::function<std::vector<std::uint8_t>()> ruppHeader;
        bool ruppOnConnected = false; // also in front of connected datagrams
        // How many bytes of proxy header to skip at the start of a received
        // datagram (0: none). Called before anything else looks at it.
        std::function<std::size_t(std::span<const std::uint8_t>)> ruppReplyLength;
        // Research knobs for the live handshake experiments (notes-join.md 5).
        bool ruppOptIn = true;    // RbxOpenRequest1 byte 18
        int request2Padding = 0;  // 0: pad to mtu-40 counting the Rupp header, 1: mtu-40 ignoring it, 2: none
    };

    struct Event {
        enum class Kind : std::uint8_t { Connected, Disconnected, Message, Failed };
        Kind kind = Kind::Message;
        int connection = -1;
        std::vector<std::uint8_t> data; // Message: the payload, byte 0 the id
        std::string reason;
    };

    Peer(Role role, Options options);
    ~Peer();
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    std::expected<void, std::string> listen(std::string_view host, std::uint16_t port);
    std::expected<void, std::string> connect(const SystemAddress& server);
    // Binds the socket (if not yet) and asks a STUN server (RFC 5389 binding
    // request) what this socket's public address is; nullopt on no answer.
    std::optional<SystemAddress> stunMappedAddress(const SystemAddress& stun, int timeoutMs = 2000);

    // Reads the socket, runs timers, sends what's due. Waits up to `waitMs`
    // for traffic first.
    void update(int waitMs = 0);
    std::optional<Event> poll();

    void send(int connection, std::vector<std::uint8_t> message, Reliability reliability = Reliability::ReliableOrdered,
              Priority priority = Priority::High, std::uint8_t channel = 0);
    void disconnect(int connection, std::uint32_t reason = 0);

    SystemAddress localAddress() const { return socket_.localAddress(); }
    std::uint64_t guid() const { return options_.guid; }
    bool connected(int connection) const;
    // The capability bits both ends set (0 before the handshake is done).
    std::uint64_t negotiated(int connection) const;
    std::size_t connectionCount() const { return connections_.size(); }
    const ReliabilityLayer* reliability(int connection) const;

private:
    struct Connection;
    struct Pending; // client: a handshake in progress

    void onDatagram(const SystemAddress& from, std::vector<std::uint8_t> data);
    void onOffline(const SystemAddress& from, std::span<const std::uint8_t> data);
    void onConnectedMessage(Connection& c, std::vector<std::uint8_t> message);
    void sendRaw(const SystemAddress& to, std::span<const std::uint8_t> data);
    void sendOffline(const SystemAddress& to, std::span<const std::uint8_t> data);
    SystemAddress wireAddress() const;
    void sendDatagrams(Connection& c);
    Connection* find(const SystemAddress& addr);
    void close(Connection& c, const std::string& reason, bool notify);
    void log(const std::string& text) const;
    void logTree(const std::string& title, const Node& tree) const;
    void writeKeylog(const Connection& c) const;
    static std::uint64_t nowMs();

    Role role_;
    Options options_;
    UdpSocket socket_;
    std::map<int, std::unique_ptr<Connection>> connections_;
    int nextConnection_ = 1;
    std::unique_ptr<Pending> pending_;
    std::deque<Event> events_; // taken from the front: a burst of thousands of messages a frame
    std::unique_ptr<PcapWriter> pcap_;
    std::string keylogPath_;
};

} // namespace lunar::net
