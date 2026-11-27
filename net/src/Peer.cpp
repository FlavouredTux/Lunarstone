#include <lunar/net/Peer.hpp>

#include <lunar/net/BitStream.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Messages.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <algorithm>
#include <random>

namespace lunar::net {

namespace {

constexpr std::uint64_t kPingIntervalMs = 5000;    // GUESS: RakNetPingFrequencyMillisecond
// The client's connect schedule (RakPeer 0x102d7051a / 0x102d6feae with
// DFInts SendConnectionAttemptCount = 6, TimeBetweenSendConnectionAttemptsMS
// = 1000, registered at 0x1054c380e) [code]: RbxOpenRequest1 every second,
// MTU index = requestsMade / (attempts / mtuCount), give up when
// requestsMade == attempts + 1. There is no Request2 timer: every Reply1
// triggers a fresh Request2 (next handshake nonce counter).
constexpr std::uint64_t kHandshakeRetryMs = 1000;
constexpr int kConnectionAttempts = 6;
constexpr std::uint64_t kConnectTimeoutMs = 10000; // GUESS (connected-state timeout)

std::uint64_t randomGuid()
{
    std::random_device rd;
    return static_cast<std::uint64_t>(rd()) << 32 | rd();
}

const SystemAddress kUnassigned{{255, 255, 255, 255}, 0};

} // namespace

struct Peer::Connection {
    enum class State : std::uint8_t { AwaitingRequest, AwaitingAccept, AwaitingNewIncoming, Connected };
    int id = 0;
    SystemAddress address;
    std::uint64_t remoteGuid = 0;
    State state = State::AwaitingRequest;
    DatagramCipher cipher;
    std::uint64_t negotiated = 0;
    std::unique_ptr<ReliabilityLayer> rl;
    std::uint64_t lastPingMs = 0;
    std::uint64_t createdMs = 0;
    std::vector<std::uint8_t> reply2; // server: resent when Request2 comes again
    bool notified = false;
    std::string closeReason; // set when the peer said goodbye; closed after the datagram
};

struct Peer::Pending {
    SystemAddress server;
    bool reply1Seen = false;
    std::size_t mtuIndex = 0;
    int tries = 0;            // requestsMade
    std::uint64_t counter = 0; // handshake nonce counter of the next Request2
    std::uint64_t nextSendMs = 0;
    std::uint64_t startMs = 0;
    std::uint16_t mtu = 0;
    std::uint64_t serverGuid = 0;
    KeyPair eph;
    SessionKeys handshake;
    std::vector<std::uint8_t> request2;
};

Peer::Peer(Role role, Options options) : role_(role), options_(std::move(options))
{
    if (options_.guid == 0)
        options_.guid = randomGuid();
    if (options_.logLevel < 0) {
        const char* env = std::getenv("LUNAR_NET_LOG");
        options_.logLevel = env ? static_cast<int>(std::strtol(env, nullptr, 10)) : 0;
    }
    if (options_.name.empty())
        options_.name = role_ == Role::Client ? "client" : "server";
    if (const char* p = std::getenv("LUNAR_NET_PCAP"); p && *p) {
        // Client and server in one process (loopback) share the env var: the
        // server gets its own file.
        std::string path = p;
        if (role_ == Role::Server)
            path += ".server.pcap";
        pcap_ = std::make_unique<PcapWriter>(path);
    }
    if (const char* k = std::getenv("LUNAR_NET_KEYLOG"); k && *k)
        keylogPath_ = k;
}

Peer::~Peer()
{
    // Say goodbye to whoever is still connected; nothing here may throw out of a destructor.
    try {
        std::vector<int> ids;
        for (auto& [id, c] : connections_)
            if (c->state == Connection::State::Connected)
                ids.push_back(id);
        for (int id : ids)
            disconnect(id, 0);
    } catch (...) { // NOLINT(bugprone-empty-catch): best effort
    }
}

std::uint64_t Peer::nowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Peer::log(const std::string& text) const
{
    if (options_.logLevel >= 1)
        std::println(stderr, "[net {}] {}", options_.name, text);
}

void Peer::logTree(const std::string& title, const Node& tree) const
{
    if (options_.logLevel < 1)
        return;
    std::print(stderr, "[net {}] {}\n{}", options_.name, title, render(tree, 1));
}

void Peer::writeKeylog(const Connection& c) const
{
    if (keylogPath_.empty())
        return;
    std::FILE* f = std::fopen(keylogPath_.c_str(), "a");
    if (!f)
        return;
    // c2s/s2c from the client's point of view of who encrypts with what.
    const SessionKeys& k = c.cipher.keys();
    const bool server = role_ == Role::Server;
    const SystemAddress self = wireAddress();
    const SystemAddress client = server ? c.address : self, srv = server ? self : c.address;
    std::println(f, "LUNARNET_SESSION client={} server={} cipher={} caps={:#x} c2s={} s2c={}", client.toString(),
                 srv.toString(), static_cast<int>(c.cipher.cipher()), c.negotiated, hexString(server ? k.rx : k.tx),
                 hexString(server ? k.tx : k.rx));
    std::fclose(f);
}

std::expected<void, std::string> Peer::listen(std::string_view host, std::uint16_t port)
{
    auto s = UdpSocket::bind(host, port);
    if (!s)
        return std::unexpected(s.error());
    socket_ = std::move(*s);
    log(std::format("listening on {}", socket_.localAddress().toString()));
    return {};
}

std::expected<void, std::string> Peer::connect(const SystemAddress& server)
{
    if (!socket_.valid()) {
        auto s = UdpSocket::bind("", 0);
        if (!s)
            return std::unexpected(s.error());
        socket_ = std::move(*s);
    }
    pending_ = std::make_unique<Pending>();
    pending_->server = server;
    pending_->startMs = nowMs();
    pending_->nextSendMs = 0;
    log(std::format("connecting to {} from {}", server.toString(), socket_.localAddress().toString()));
    return {};
}

std::optional<SystemAddress> Peer::stunMappedAddress(const SystemAddress& stun, int timeoutMs)
{
    if (!socket_.valid()) {
        auto s = UdpSocket::bind("", 0);
        if (!s)
            return std::nullopt;
        socket_ = std::move(*s);
    }
    std::vector<std::uint8_t> req{0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    std::random_device rd;
    for (int i = 0; i < 12; ++i)
        req.push_back(static_cast<std::uint8_t>(rd()));
    sendRaw(stun, req);
    const std::uint64_t deadline = nowMs() + static_cast<std::uint64_t>(timeoutMs);
    while (nowMs() < deadline) {
        socket_.wait(50);
        auto d = socket_.receive();
        if (!d || d->first != stun)
            continue;
        const auto& m = d->second;
        if (pcap_)
            pcap_->write(d->first, wireAddress(), m);
        if (m.size() < 20 || m[0] != 0x01 || m[1] != 0x01 || !std::equal(req.begin() + 4, req.end(), m.begin() + 4))
            continue;
        std::size_t p = 20;
        while (p + 4 <= m.size()) {
            const unsigned type = unsigned{m[p]} << 8 | m[p + 1], len = unsigned{m[p + 2]} << 8 | m[p + 3];
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && m[p + 5] == 1) {
                SystemAddress a;
                a.port = static_cast<std::uint16_t>(m[p + 6] << 8 | m[p + 7]);
                for (int i = 0; i < 4; ++i)
                    a.ip[static_cast<std::size_t>(i)] = m[p + 8 + static_cast<std::size_t>(i)];
                if (type == 0x0020) {
                    a.port ^= 0x2112;
                    const std::uint8_t cookie[4] = {0x21, 0x12, 0xa4, 0x42};
                    for (int i = 0; i < 4; ++i)
                        a.ip[static_cast<std::size_t>(i)] ^= cookie[i];
                }
                return a;
            }
            p += 4 + ((len + 3) & ~3u);
        }
    }
    return std::nullopt;
}

SystemAddress Peer::wireAddress() const
{
    // A socket bound to any address shows up as 127.0.0.1 in captures and
    // keylogs (right for the local loop, which is what they are for).
    SystemAddress a = socket_.localAddress();
    if (a.ip == std::array<std::uint8_t, 4>{})
        a.ip = {127, 0, 0, 1};
    return a;
}

void Peer::sendRaw(const SystemAddress& to, std::span<const std::uint8_t> data)
{
    if (pcap_)
        pcap_->write(wireAddress(), to, data);
    socket_.sendTo(to, data);
}

void Peer::sendOffline(const SystemAddress& to, std::span<const std::uint8_t> data)
{
    if (!options_.ruppHeader) {
        sendRaw(to, data);
        return;
    }
    std::vector<std::uint8_t> wire = options_.ruppHeader();
    wire.insert(wire.end(), data.begin(), data.end());
    sendRaw(to, wire);
}

Peer::Connection* Peer::find(const SystemAddress& addr)
{
    for (auto& [id, c] : connections_)
        if (c->address == addr)
            return c.get();
    return nullptr;
}

bool Peer::connected(int connection) const
{
    auto it = connections_.find(connection);
    return it != connections_.end() && it->second->state == Connection::State::Connected;
}

std::uint64_t Peer::negotiated(int connection) const
{
    auto it = connections_.find(connection);
    return it != connections_.end() ? it->second->negotiated : 0;
}

const ReliabilityLayer* Peer::reliability(int connection) const
{
    auto it = connections_.find(connection);
    return it == connections_.end() ? nullptr : it->second->rl.get();
}

void Peer::send(int connection, std::vector<std::uint8_t> message, Reliability reliability, Priority priority,
                std::uint8_t channel)
{
    auto it = connections_.find(connection);
    if (it == connections_.end() || !it->second->rl)
        return;
    it->second->rl->send(std::move(message), reliability, priority, channel);
}

void Peer::disconnect(int connection, std::uint32_t reason)
{
    auto it = connections_.find(connection);
    if (it == connections_.end() || !it->second->rl)
        return;
    BitStream m;
    m.writeU8(msg::DisconnectionNotification);
    m.writeU32(reason);
    it->second->rl->send(std::move(m).take(), Reliability::ReliableOrdered, Priority::Low, 0);
    sendDatagrams(*it->second);
    it->second->state = Connection::State::AwaitingRequest;
    events_.push_back({Event::Kind::Disconnected, connection, {}, "local disconnect"});
    connections_.erase(it);
}

void Peer::close(Connection& c, const std::string& reason, bool notify)
{
    log(std::format("connection {} to {} closed: {}", c.id, c.address.toString(), reason));
    if (notify)
        events_.push_back({Event::Kind::Disconnected, c.id, {}, reason});
    connections_.erase(c.id);
}

std::optional<Peer::Event> Peer::poll()
{
    if (events_.empty())
        return std::nullopt;
    Event e = std::move(events_.front());
    events_.pop_front();
    return e;
}

void Peer::sendDatagrams(Connection& c)
{
    if (!c.rl)
        return;
    for (std::vector<std::uint8_t>& plain : c.rl->flush(nowMs())) {
        if (options_.logLevel >= 1) {
            DescribeOptions o;
            o.fullHex = options_.logLevel >= 2;
            o.roblox = &options_.describer;
            o.physicsServerToClient = role_ == Role::Server; // what this peer sends
            const Datagram d = parseDatagram(plain, headerOptions(c.negotiated));
            logTree(std::format("-> {} ({} bytes on the wire)", c.address.toString(), plain.size() + kDatagramOverhead),
                    describeDatagram(d, plain.size(), o));
        }
        const std::vector<std::uint8_t> wire = c.cipher.seal(plain);
        if (options_.ruppOnConnected)
            sendOffline(c.address, wire);
        else
            sendRaw(c.address, wire);
    }
}

void Peer::update(int waitMs)
{
    if (!socket_.valid())
        return;
    const std::uint64_t now = nowMs();

    // Client handshake timers.
    if (pending_ && now >= pending_->nextSendMs) {
        Pending& p = *pending_;
        if (p.tries >= kConnectionAttempts + 1) {
            // 0x102d7082a: id 0x0D (never a Reply1) or 0x11 (Reply1 but no Reply2).
            events_.push_back({Event::Kind::Failed, -1, {},
                               p.reply1Seen ? std::format("server {} reached, but the handshake failed (no "
                                                          "RbxOpenReply2 after {} attempts)",
                                                          p.server.toString(), p.tries)
                                            : std::format("no reply from {} after {} attempts", p.server.toString(),
                                                          p.tries)});
            pending_.reset();
        } else {
            const std::size_t perMtu = std::max<std::size_t>(1, kConnectionAttempts / options_.mtus.size());
            p.mtuIndex = std::min(static_cast<std::size_t>(p.tries) / perMtu, options_.mtus.size() - 1);
            OpenRequest1 r;
            r.ruppOptIn = options_.ruppOptIn;
            r.mtu = options_.mtus[p.mtuIndex];
            // The Rupp header counts toward the MTU the server computes
            // (PROTOCOL.md 2.1: ruppHeaderLen + len + 40), so pad less by it.
            if (options_.ruppHeader)
                r.mtu -= options_.ruppHeader().size();
            const auto bytes = encodeOpenRequest1(r);
            if (options_.logLevel >= 1)
                logTree(std::format("-> {} ({} bytes)", p.server.toString(), bytes.size()),
                        describeOffline(bytes, {options_.logLevel >= 2}));
            sendOffline(p.server, bytes);
            ++p.tries;
            p.nextSendMs = now + kHandshakeRetryMs;
        }
    }

    if (waitMs > 0)
        socket_.wait(waitMs);
    while (auto d = socket_.receive()) {
        if (pcap_)
            pcap_->write(d->first, wireAddress(), d->second);
        onDatagram(d->first, std::move(d->second));
    }

    // Per-connection timers and output.
    std::vector<int> dead;
    for (auto& [id, cp] : connections_) {
        Connection& c = *cp;
        if (!c.rl)
            continue;
        if (c.rl->dead(nowMs())
            || (c.state != Connection::State::Connected && nowMs() - c.createdMs > kConnectTimeoutMs)) {
            dead.push_back(id);
            continue;
        }
        if (c.state == Connection::State::Connected && nowMs() - c.lastPingMs >= kPingIntervalMs) {
            c.lastPingMs = nowMs();
            BitStream ping;
            ping.writeU8(msg::ConnectedPing);
            ping.writeU64(nowMs());
            c.rl->send(std::move(ping).take(), Reliability::Unreliable, Priority::Immediate);
        }
        sendDatagrams(c);
    }
    for (int id : dead)
        if (auto it = connections_.find(id); it != connections_.end())
            close(*it->second, "timed out", it->second->notified);
}

void Peer::onDatagram(const SystemAddress& from, std::vector<std::uint8_t> data)
{
    if (data.empty())
        return;
    if (options_.ruppReplyLength) {
        const std::size_t skip = options_.ruppReplyLength(data);
        if (skip >= data.size())
            return;
        if (skip > 0) {
            log(std::format("<- {}: {} byte rupp header skipped", from.toString(), skip));
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(skip));
        }
    }
    if (Connection* c = find(from); c && c->rl) {
        if (auto plain = c->cipher.open(data)) {
            Datagram parsed;
            c->rl->onDatagram(*plain, nowMs(), &parsed);
            if (options_.logLevel >= 1) {
                DescribeOptions o;
                o.fullHex = options_.logLevel >= 2;
                o.roblox = &options_.describer;
                o.physicsServerToClient = role_ == Role::Client; // what this peer receives
                logTree(std::format("<- {} ({} bytes on the wire, nonce {:#x})", from.toString(), data.size(),
                                    c->cipher.rxCounter()),
                        describeDatagram(parsed, plain->size(), o));
            }
            while (c->closeReason.empty()) {
                auto m = c->rl->receive();
                if (!m)
                    break;
                onConnectedMessage(*c, std::move(*m));
            }
            if (!c->closeReason.empty())
                close(*c, c->closeReason, c->notified);
            return;
        }
        if (!isOfflineId(data[0])) {
            log(std::format("<- {}: {} bytes that don't decrypt, dropped", from.toString(), data.size()));
            return;
        }
    }
    onOffline(from, data);
}

void Peer::onOffline(const SystemAddress& from, std::span<const std::uint8_t> data)
{
    const std::uint8_t id = data[0];
    std::vector<std::uint8_t> opened;
    const std::vector<std::uint8_t>* openedPtr = nullptr;

    if (role_ == Role::Server && id == offline::OpenRequest1) {
        if (options_.logLevel >= 1)
            logTree(std::format("<- {} ({} bytes)", from.toString(), data.size()),
                    describeOffline(data, {options_.logLevel >= 2}));
        auto r = decodeOpenRequest1(data);
        if (!r || data.size() + kMtuOverhead <= 575)
            return;
        if (r->protocol != kProtocolVersion) {
            sendRaw(from, encodeRejection({offline::IncompatibleProtocolVersion, options_.guid, 0}));
            return;
        }
        OpenReply1 reply;
        reply.serverGuid = options_.guid;
        reply.mtu = static_cast<std::uint16_t>(data.size() + kMtuOverhead);
        const auto bytes = encodeOpenReply1(reply);
        if (options_.logLevel >= 1)
            logTree(std::format("-> {} ({} bytes)", from.toString(), bytes.size()),
                    describeOffline(bytes, {options_.logLevel >= 2}));
        sendRaw(from, bytes);
        return;
    }

    if (role_ == Role::Server && id == offline::OpenRequest2) {
        if (Connection* existing = find(from); existing && !existing->reply2.empty()) {
            sendRaw(from, existing->reply2); // the client missed our Reply2
            return;
        }
        auto h = decodeOpenRequest2Header(data);
        if (!h) {
            log(std::format("<- {}: {}", from.toString(), h.error()));
            return;
        }
        if (h->keyExchangeVersion != options_.keyExchangeVersion) {
            log(std::format("RbxOpenRequest2 key exchange version {} incorrect for this server (expects {})",
                            h->keyExchangeVersion, options_.keyExchangeVersion));
            return;
        }
        const KeyPair early
            = options_.serverEphemeralEarlyPair.value_or(KeyPair::fromSecret(localkeys::ephemeralEarlySecret));
        const SessionKeys hs = kxServer(early, h->clientPublic);
        auto boxBytes = openHandshake(data, h->aadSize, h->boxSize, hs.rx);
        if (boxBytes) {
            opened = *boxBytes;
            openedPtr = &opened;
        }
        if (options_.logLevel >= 1)
            logTree(std::format("<- {} ({} bytes)", from.toString(), data.size()),
                    describeOffline(data, {options_.logLevel >= 2}, openedPtr));
        if (!boxBytes) {
            log("RbxOpenRequest2 decrypt failed");
            return;
        }
        auto box = decodeOpenRequest2Box(*boxBytes);
        if (!box || box->mtu <= 575)
            return;
        if (connections_.size() >= options_.maxConnections) {
            sendRaw(from, encodeRejection({offline::NoFreeIncomingConnections, options_.guid, 0}));
            return;
        }
        std::uint8_t cipher = box->cipher & 0x7f;
        if ((cipher & 2) && !aesGcmAvailable())
            cipher = 1;
        if (cipher != 1 && cipher != 2) {
            sendRaw(from, encodeRejection({offline::OurSystemRequiresSecurity, options_.guid, 0}));
            return;
        }
        const KeyPair session = KeyPair::generate();
        auto c = std::make_unique<Connection>();
        c->id = nextConnection_++;
        c->address = from;
        c->remoteGuid = box->clientGuid;
        c->createdMs = nowMs();
        c->negotiated = options_.capabilities & box->capabilities;
        c->cipher = DatagramCipher(static_cast<Cipher>(cipher), kxServer(session, h->clientPublic));
        ReliabilityLayer::Options ro;
        ro.mtu = box->mtu;
        ro.header = headerOptions(c->negotiated);
        c->rl = std::make_unique<ReliabilityLayer>(ro);

        OpenReply2Box rb;
        rb.serverSessionPublic = session.publicKey;
        rb.capabilities = options_.capabilities;
        rb.serverGuid = options_.guid;
        rb.mtu = box->mtu;
        rb.cipher = cipher;
        rb.clientAddress = from;
        c->reply2 = encodeOpenReply2({}, rb, hs.tx, 0);
        if (options_.logLevel >= 1) {
            auto rh = decodeOpenReply2Header(c->reply2);
            std::vector<std::uint8_t> plainBox;
            if (rh)
                if (auto b = openHandshake(c->reply2, rh->aadSize, rh->boxSize, hs.tx))
                    plainBox = *b;
            // (the server's own tx key opens what it sealed; for the printout only)
            logTree(std::format("-> {} ({} bytes)", from.toString(), c->reply2.size()),
                    describeOffline(c->reply2, {options_.logLevel >= 2}, plainBox.empty() ? nullptr : &plainBox));
        }
        sendRaw(from, c->reply2);
        writeKeylog(*c);
        log(std::format("connection {} from {} (guid {:016x}), cipher {}, capabilities {:#x}", c->id, from.toString(),
                        c->remoteGuid, cipherName(static_cast<Cipher>(cipher)), c->negotiated));
        connections_.emplace(c->id, std::move(c));
        return;
    }

    if (role_ == Role::Client && pending_ && from == pending_->server) {
        Pending& p = *pending_;
        if (id == offline::OpenReply1) {
            if (options_.logLevel >= 1)
                logTree(std::format("<- {} ({} bytes)", from.toString(), data.size()),
                        describeOffline(data, {options_.logLevel >= 2}));
            auto r = decodeOpenReply1(data);
            if (!r)
                return;
            p.mtu = r->mtu;
            p.serverGuid = r->serverGuid;
            if (!p.reply1Seen) {
                // One ephemeral pair per connection attempt (the RCS's crypto
                // object, 0x102d61912); later Reply1s reuse it.
                p.reply1Seen = true;
                p.eph = KeyPair::generate();
                p.handshake
                    = kxClient(p.eph, options_.serverEphemeralEarlyPublic.value_or(localkeys::ephemeralEarlyPublic));
                if (const char* swap = std::getenv("LUNAR_NET_KX_SWAP"); swap && *swap == '1')
                    std::swap(p.handshake.rx, p.handshake.tx); // research: try the other role's keys
            }
            OpenRequest2Header h;
            h.keyExchangeVersion = options_.keyExchangeVersion;
            h.clientPublic = p.eph.publicKey;
            h.preAuth = options_.preAuth;
            OpenRequest2Box box;
            box.token = options_.preAuthToken;
            box.capabilities = options_.capabilities;
            box.clientGuid = options_.guid;
            box.mtu = p.mtu;
            box.cipher = options_.cipher != 0 ? options_.cipher : aesGcmAvailable() ? 2 : 1;
            box.serverAddress = p.server;
            const std::size_t ruppLen = options_.ruppHeader ? options_.ruppHeader().size() : 0;
            const std::size_t padTo = options_.request2Padding == 2   ? 0
                                      : options_.request2Padding == 1 ? p.mtu - kMtuOverhead
                                                                      : p.mtu - kMtuOverhead - ruppLen;
            p.request2 = encodeOpenRequest2(h, box, p.handshake.tx, p.counter++, padTo);
            if (options_.logLevel >= 1)
                logTree(std::format("-> {} ({} bytes, nonce counter {})", p.server.toString(), p.request2.size(),
                                    p.counter - 1),
                        describeOffline(p.request2, {options_.logLevel >= 2}));
            sendOffline(p.server, p.request2);
            return;
        }
        if (id == offline::OpenReply2 && p.reply1Seen) {
            auto h = decodeOpenReply2Header(data);
            std::optional<std::vector<std::uint8_t>> boxBytes;
            if (h)
                boxBytes = openOpenReply2BoxBytes(data, *h, p.handshake.rx);
            if (options_.logLevel >= 1)
                logTree(std::format("<- {} ({} bytes)", from.toString(), data.size()),
                        describeOffline(data, {options_.logLevel >= 2}, boxBytes ? &*boxBytes : nullptr));
            if (!boxBytes) {
                log("RbxOpenReply2 decrypt failed");
                return;
            }
            auto box = decodeOpenReply2Box(*boxBytes);
            if (!box)
                return;
            const std::uint8_t cipher = box->cipher;
            if ((cipher & 0x80) || (cipher != 1 && cipher != 2)) {
                events_.push_back({Event::Kind::Failed, -1, {}, "server picked no usable cipher"});
                pending_.reset();
                return;
            }
            auto c = std::make_unique<Connection>();
            c->id = nextConnection_++;
            c->address = from;
            c->remoteGuid = box->serverGuid;
            c->createdMs = nowMs();
            c->negotiated = options_.capabilities & box->capabilities;
            c->cipher = DatagramCipher(static_cast<Cipher>(cipher), kxClient(p.eph, box->serverSessionPublic));
            ReliabilityLayer::Options ro;
            ro.mtu = box->mtu;
            ro.header = headerOptions(c->negotiated);
            c->rl = std::make_unique<ReliabilityLayer>(ro);
            c->state = Connection::State::AwaitingAccept;
            // ID_CONNECTION_REQUEST, RELIABLE, IMMEDIATE (0x102d7808e).
            BitStream req;
            req.writeU8(msg::ConnectionRequest);
            req.writeU64(options_.guid);
            req.writeU64(nowMs());
            req.writeU8(0);
            if (!options_.password.empty())
                req.writeBytes(options_.password);
            c->rl->send(std::move(req).take(), Reliability::Reliable, Priority::Immediate, 0);
            writeKeylog(*c);
            log(std::format("handshake done with {} (guid {:016x}), cipher {}, capabilities {:#x}", from.toString(),
                            c->remoteGuid, cipherName(static_cast<Cipher>(cipher)), c->negotiated));
            sendDatagrams(*c);
            connections_.emplace(c->id, std::move(c));
            pending_.reset();
            return;
        }
        if (isRejectionId(id)) {
            if (options_.logLevel >= 1)
                logTree(std::format("<- {} ({} bytes)", from.toString(), data.size()),
                        describeOffline(data, {options_.logLevel >= 2}));
            events_.push_back({Event::Kind::Failed, -1, {}, std::format("rejected: {}", messageName(id))});
            pending_.reset();
            return;
        }
    }
    if (options_.logLevel >= 1)
        logTree(std::format("<- {} ({} bytes, ignored)", from.toString(), data.size()),
                describeOffline(data, {options_.logLevel >= 2}));
}

void Peer::onConnectedMessage(Connection& c, std::vector<std::uint8_t> message)
{
    if (message.empty())
        return;
    const std::uint8_t id = message[0];
    BitStream in(message);
    in.skipBits(8);
    switch (id) {
    case msg::ConnectedPing:
    case 0x51:
    case 0x52:
    case 0x53: {
        // The genuine 0.741 client answers 0x00 with 0x03 and 0x51..0x53 with
        // 0x54..0x56: u64 echoed time, u64 own time (ms), u64 wall clock (us
        // since the epoch); a padded (MTU-sized, reliable) ping gets a pong
        // padded to the same length, sent reliably [live capture].
        std::uint64_t t = 0;
        in.readU64(t);
        BitStream pong;
        pong.writeU8(id == msg::ConnectedPing ? msg::ConnectedPong : static_cast<std::uint8_t>(id + 3));
        pong.writeU64(t);
        pong.writeU64(nowMs());
        pong.writeU64(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                     std::chrono::system_clock::now().time_since_epoch())
                                                     .count()));
        std::vector<std::uint8_t> bytes = std::move(pong).take();
        const bool padded = message.size() > 64;
        if (padded && message.size() > bytes.size())
            bytes.resize(message.size(), 0);
        c.rl->send(std::move(bytes), padded ? Reliability::Reliable : Reliability::Unreliable, Priority::Immediate);
        return;
    }
    case msg::ConnectedPong:
        return;
    case msg::ConnectionRequest: {
        if (role_ != Role::Server)
            return;
        std::uint64_t guid = 0, time = 0;
        in.readU64(guid);
        in.readU64(time);
        BitStream acc;
        acc.writeU8(msg::ConnectionRequestAccepted);
        writeAddress(acc, c.address);
        acc.writeU16(static_cast<std::uint16_t>(c.id));
        for (int i = 0; i < 10; ++i)
            writeAddress(acc, i == 0 ? socket_.localAddress() : kUnassigned);
        acc.writeU64(time);
        acc.writeU64(nowMs());
        acc.writeU64(nowMs()); // GUESS: third time field
        c.rl->send(std::move(acc).take(), Reliability::Reliable, Priority::Immediate, 0);
        c.state = Connection::State::AwaitingNewIncoming;
        return;
    }
    case msg::ConnectionRequestAccepted: {
        if (role_ != Role::Client || c.state != Connection::State::AwaitingAccept)
            return;
        SystemAddress self;
        readAddress(in, self);
        std::uint16_t index = 0;
        in.readU16(index);
        SystemAddress ignore;
        for (int i = 0; i < 10; ++i)
            readAddress(in, ignore);
        std::uint64_t requestTime = 0, acceptTime = 0;
        in.readU64(requestTime);
        in.readU64(acceptTime);
        BitStream nic;
        nic.writeU8(msg::NewIncomingConnection);
        writeAddress(nic, c.address);
        for (int i = 0; i < 10; ++i)
            writeAddress(nic, i == 0 ? socket_.localAddress() : kUnassigned);
        nic.writeU64(acceptTime);
        nic.writeU64(nowMs());
        nic.writeU64(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count()));
        c.rl->send(std::move(nic).take(), Reliability::ReliableOrdered, Priority::Immediate, 0);
        c.state = Connection::State::Connected;
        c.notified = true;
        c.lastPingMs = nowMs();
        log(std::format("connected to {} as {} (system index {})", c.address.toString(), self.toString(), index));
        events_.push_back({Event::Kind::Connected, c.id, {}, {}});
        return;
    }
    case msg::NewIncomingConnection:
        if (role_ == Role::Server && c.state != Connection::State::Connected) {
            c.state = Connection::State::Connected;
            c.notified = true;
            c.lastPingMs = nowMs();
            log(std::format("connection {} from {} is up", c.id, c.address.toString()));
            events_.push_back({Event::Kind::Connected, c.id, {}, {}});
        }
        return;
    case msg::DisconnectionNotification: {
        std::uint32_t reason = 0;
        in.readU32(reason);
        c.closeReason = std::format("peer disconnected (reason {})", reason);
        return;
    }
    default:
        events_.push_back({Event::Kind::Message, c.id, std::move(message), {}});
        return;
    }
}

} // namespace lunar::net
