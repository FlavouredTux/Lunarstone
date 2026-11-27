#include <lunar/net/CaptureDecoder.hpp>

#include <lunar/net/Datagram.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Rupp.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

namespace lunar::net {

namespace {

std::optional<Key> keyFromHex(std::string_view hex)
{
    if (hex.size() != 64)
        return std::nullopt;
    Key k{};
    for (std::size_t i = 0; i < 32; ++i) {
        unsigned v = 0;
        auto [p, ec] = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, v, 16);
        if (ec != std::errc{})
            return std::nullopt;
        k[i] = static_cast<std::uint8_t>(v);
    }
    return k;
}

std::string flowKey(const SystemAddress& client, const SystemAddress& server)
{
    std::string key = client.toString();
    key += '|';
    key += server.toString();
    return key;
}

} // namespace

std::vector<std::shared_ptr<Replica>> CaptureDecoder::replicas() const
{
    std::vector<std::shared_ptr<Replica>> out;
    for (const Flow& flow : flows_)
        out.push_back(flow.replica);
    return out;
}

std::size_t CaptureDecoder::loadKeylog(const std::filesystem::path& path)
{
    std::ifstream f(path);
    std::string line;
    std::size_t n = 0;
    while (std::getline(f, line)) {
        if (!line.starts_with("LUNARNET_SESSION "))
            continue;
        std::istringstream fields(line.substr(17));
        std::string field, client, server;
        Session s;
        bool haveC2s = false, haveS2c = false;
        while (fields >> field) {
            const auto eq = field.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string name = field.substr(0, eq), value = field.substr(eq + 1);
            if (name == "client")
                client = value;
            else if (name == "server")
                server = value;
            else if (name == "cipher")
                s.cipher = value == "2" ? Cipher::Aes256Gcm : Cipher::ChaCha20Poly1305;
            else if (name == "caps")
                s.capabilities = std::stoull(value, nullptr, 16);
            else if (name == "c2s")
                if (auto k = keyFromHex(value)) {
                    s.c2s = *k;
                    haveC2s = true;
                }
            if (name == "s2c")
                if (auto k = keyFromHex(value)) {
                    s.s2c = *k;
                    haveS2c = true;
                }
        }
        if (!client.empty() && !server.empty() && haveC2s && haveS2c) {
            std::string key = client;
            key += '|';
            key += server;
            keylog_[std::move(key)] = s;
            ++n;
        }
    }
    return n;
}

CaptureDecoder::Flow& CaptureDecoder::flowFor(const SystemAddress& from, const SystemAddress& to, std::uint8_t first)
{
    for (Flow& f : flows_)
        if ((f.client == from && f.server == to) || (f.client == to && f.server == from))
            return f;
    Flow f;
    // Who is the client: the side that sends RbxOpenRequest1/2, or whatever
    // the keylog says; failing both, the first sender.
    const bool fromIsServer = first == offline::OpenReply1 || first == offline::OpenReply2 || isRejectionId(first)
                              || keylog_.contains(flowKey(to, from));
    f.client = fromIsServer ? to : from;
    f.server = fromIsServer ? from : to;
    flows_.push_back(std::move(f));
    attachKeys(flows_.back());
    return flows_.back();
}

void CaptureDecoder::attachKeys(Flow& flow)
{
    auto it = keylog_.find(flowKey(flow.client, flow.server));
    if (it == keylog_.end())
        return;
    const Session& s = it->second;
    flow.capabilities = s.capabilities;
    flow.toServer.cipher = DatagramCipher(s.cipher, SessionKeys{s.c2s, s.s2c});
    flow.toClient.cipher = DatagramCipher(s.cipher, SessionKeys{s.s2c, s.c2s});
}

namespace {

// The whole messages the datagram's split pieces complete, each to `complete`
// with its split id and piece count.
template <class Complete>
void completeSplits(const Datagram& dg,
                    std::map<std::uint16_t, std::vector<std::optional<std::vector<std::uint8_t>>>>& splits,
                    Complete&& complete)
{
    for (const InternalPacket& p : dg.packets) {
        if (!p.split)
            continue;
        auto& pieces = splits[p.splitPacketId];
        if (pieces.empty())
            pieces.resize(p.splitPacketCount);
        if (p.splitPacketIndex < pieces.size())
            pieces[p.splitPacketIndex] = p.payload;
        bool done = true;
        std::vector<std::uint8_t> whole;
        for (const auto& piece : pieces) {
            if (!piece) {
                done = false;
                break;
            }
            whole.insert(whole.end(), piece->begin(), piece->end());
        }
        if (done) {
            complete(p.splitPacketId, pieces.size(), std::move(whole));
            splits.erase(p.splitPacketId);
        }
    }
}

// What describeMessage hands the Roblox describer: every message but
// RakNet's own pings and the connection's setup and end, which it describes
// itself and stops at.
void toReplica(std::span<const std::uint8_t> payload, Replica& replica)
{
    if (payload.empty())
        return;
    switch (payload[0]) {
    case msg::ConnectedPing:
    case msg::ConnectedPong:
    case msg::ConnectionRequest:
    case msg::ConnectionRequestAccepted:
    case msg::NewIncomingConnection:
    case msg::DisconnectionNotification:
        return;
    default:
        (void)replica.handle(payload);
    }
}

} // namespace

void CaptureDecoder::reassemble(const Datagram& dg, Direction& dir, Node& n, const DescribeOptions& options)
{
    // Whole messages out of split pieces.
    completeSplits(dg, dir.splits, [&](std::uint16_t id, std::size_t count, std::vector<std::uint8_t> whole) {
        Node& w = n.add(std::format("split message {} complete ({} pieces, {} bytes):", id, count, whole.size()));
        w.add(describeMessage(whole, options));
    });
}

// The messages in the order the described decode hands them over: the ones
// the datagram's last pieces complete, then its whole ones (describeDatagram
// describes the messages of DATA datagrams only).
void CaptureDecoder::deliver(const Datagram& dg, Direction& dir, Replica& replica)
{
    completeSplits(dg, dir.splits,
                   [&](std::uint16_t, std::size_t, std::vector<std::uint8_t> whole) { toReplica(whole, replica); });
    if (dg.header.isAck || dg.header.isNak)
        return;
    for (const InternalPacket& p : dg.packets)
        if (!p.split)
            toReplica(p.payload, replica);
}

Node CaptureDecoder::decode(const CapturedDatagram& d0, std::size_t index)
{
    // A Rupp header (UDMUX proxy, Rupp.hpp): recognised in front of an
    // offline message, and from then on in front of every datagram between
    // the same two endpoints (the proxy and the client put one on the
    // encrypted ones too). Skipped, and printed once the datagram is placed.
    CapturedDatagram d = d0;
    std::optional<RuppHeader> rupp;
    const std::string pair = std::min(d.from.toString(), d.to.toString()) + "|"
                             + std::max(d.from.toString(), d.to.toString());
    if (d.payload.size() > 4 && !isOfflineId(d.payload[0]))
        if (auto h = decodeRuppHeader(d.payload); h && h->length() < d.payload.size()
                                                     && (isOfflineId(d.payload[h->length()])
                                                         || ruppFlows_.contains(pair))) {
            rupp = h;
            ruppFlows_.insert(pair);
            d.payload.erase(d.payload.begin(), d.payload.begin() + static_cast<std::ptrdiff_t>(h->length()));
        }
    const std::uint8_t first = d.payload.empty() ? 0 : d.payload[0];
    Flow& flow = flowFor(d.from, d.to, first);
    const bool toServer = d.to == flow.server;
    DescribeOptions options = options_;
    options.roblox = &flow.describer;
    options.physicsServerToClient = !toServer; // server -> client datagrams carry frame lists
    flow.replica->framing = Framing::fromCapabilities(flow.capabilities);
    flow.replica->capabilities = flow.capabilities;
    flow.replica->decodingServerToClient = !toServer;
    flow.replica->receiveTime = d.time; // physics samples arrive at the capture's times
    Direction& dir = toServer ? flow.toServer : flow.toClient;
    const bool quiet = options_.quiet;
    Node n(quiet ? std::string()
                 : std::format("#{} t={:.6f} {} -> {} ({}) {} bytes", index, d.time, d.from.toString(), d.to.toString(),
                               toServer ? "client to server" : "server to client", d0.payload.size()));
    if (rupp && !quiet) {
        Node& r = n.add(std::format("rupp header, {} bytes: byte0 {:#04x}, byte1 {:#04x}, {} TLV(s)", rupp->length(),
                                    rupp->byte0, rupp->byte1, rupp->tlvs.size()));
        for (const RuppTlv& t : rupp->tlvs)
            r.add(std::format("type {:#04x}, {} bytes: {}", t.type, t.value.size(), hexString(t.value)));
    }
    if (d.payload.empty())
        return n;

    // Connected datagram?
    if (dir.cipher) {
        DatagramCipher probe = *dir.cipher;
        if (auto plain = probe.open(d.payload)) {
            if (quiet) {
                *dir.cipher = probe;
                deliver(parseDatagram(*plain, headerOptions(flow.capabilities)), dir, *flow.replica);
                return n;
            }
            const std::size_t len = d.payload.size() - kDatagramOverhead;
            const auto low = static_cast<std::uint16_t>(d.payload[len] | d.payload[len + 1] << 8);
            Node& crypto
                = n.add(std::format("session crypto: {}, nonce counter {:#x} (wire low 16 bits {:#06x}), tag {}",
                                    cipherName(probe.cipher()), dir.cipher->rxCounterFor(low), low,
                                    hexString(std::span(d.payload).subspan(len + 2, 16))));
            (void)crypto;
            *dir.cipher = probe;
            const Datagram dg = parseDatagram(*plain, headerOptions(flow.capabilities));
            // A message completed by this datagram's last piece (the schema, say) is
            // described first, so the flow's replica knows it before the whole
            // messages that follow it in the same datagram.
            reassemble(dg, dir, n, options);
            n.add(describeDatagram(dg, plain->size(), options));
            if (options_.fullHex) {
                Node& w = n.add("wire bytes");
                addHex(w, d.payload);
            }
            return n;
        }
    }

    if (isOfflineId(first)) {
        std::optional<std::vector<std::uint8_t>> box;
        if (first == offline::OpenRequest2) {
            if (auto h = decodeOpenRequest2Header(d.payload); h && h->keyExchangeVersion == 0) {
                flow.clientPublic = h->clientPublic;
                flow.handshakeServer = kxServer(KeyPair::fromSecret(localkeys::ephemeralEarlySecret), h->clientPublic);
                box = openHandshake(d.payload, h->aadSize, h->boxSize, flow.handshakeServer->rx);
                if (box)
                    if (auto b = decodeOpenRequest2Box(*box))
                        flow.capabilities &= b->capabilities;
            }
        } else if (first == offline::OpenReply2 && flow.handshakeServer) {
            if (auto h = decodeOpenReply2Header(d.payload)) {
                box = openHandshake(d.payload, h->aadSize, h->boxSize, flow.handshakeServer->tx);
                if (box)
                    if (auto b = decodeOpenReply2Box(*box))
                        flow.capabilities &= b->capabilities;
            }
        }
        if (quiet)
            return n;
        n.add(describeOffline(d.payload, options_, box ? &*box : nullptr));
        if (first == offline::OpenRequest2 || first == offline::OpenReply2) {
            if (!box) {
                std::uint16_t version = 0;
                if (first == offline::OpenRequest2)
                    if (auto h = decodeOpenRequest2Header(d.payload))
                        version = h->keyExchangeVersion;
                n.add(version == 0 ? "(box not decrypted: not the version-0 local server keys)"
                                   : std::format("(box not decrypted: key exchange version {}, the server's "
                                                 "secret key isn't ours; the session keys come from the keylog)",
                                                 version));
            }
            else if (!dir.cipher)
                n.add("(session keys are ephemeral: connected datagrams need a keylog, LUNAR_NET_KEYLOG)");
        }
        return n;
    }

    if (options_.plaintext) {
        n.add("(plaintext datagram, --plain)");
        const Datagram dg = parseDatagram(d.payload, headerOptions(flow.capabilities));
        n.add(describeDatagram(dg, d.payload.size(), options));
        reassemble(dg, dir, n, options);
        return n;
    }
    if (quiet)
        return n;
    Node& e = n.add(std::format("encrypted connected datagram, {} bytes ciphertext",
                                d.payload.size() - std::min(d.payload.size(), kDatagramOverhead)));
    if (d.payload.size() >= kDatagramOverhead) {
        const std::size_t len = d.payload.size() - kDatagramOverhead;
        e.add(std::format("nonce low 16 bits: {:#06x}", d.payload[len] | d.payload[len + 1] << 8));
        e.add("tag: " + hexString(std::span(d.payload).subspan(len + 2, 16)));
    }
    e.add(dir.cipher ? "(the keylog's keys don't open it)" : "(no session keys for this flow: pass --keylog)");
    addHex(e, d.payload, 0, options_.fullHex ? SIZE_MAX : 64);
    return n;
}

} // namespace lunar::net
