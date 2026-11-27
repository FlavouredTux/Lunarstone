#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/net/BitStream.hpp>
#include <lunar/net/CaptureDecoder.hpp>
#include <lunar/net/Crypto.hpp>
#include <lunar/net/Datagram.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Offline.hpp>
#include <lunar/net/Peer.hpp>
#include <lunar/net/Reliability.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Values.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace lunar;
using namespace lunar::net;

namespace {

std::vector<std::uint8_t> unhex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

Key keyFromHex(std::string_view hex)
{
    Key k{};
    const auto v = unhex(hex);
    std::copy_n(v.begin(), std::min(v.size(), k.size()), k.begin());
    return k;
}

// Emulator vectors from the optional test reference directory,
// written by running RobloxPlayer 0.742's own serializers under unicorn.
std::optional<assets::json::Value> vectors(const char* name)
{
    const std::filesystem::path path = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors" / name;
    std::ifstream f(path);
    if (!f)
        return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return assets::json::parse(ss.str());
}

std::int64_t num(const assets::json::Value& v, const char* key)
{
    const auto* x = v[key];
    return x && x->number() ? static_cast<std::int64_t>(*x->number()) : 0;
}

std::string str(const assets::json::Value& v, const char* key)
{
    const auto* x = v[key];
    return x && x->string() ? *x->string() : std::string{};
}

} // namespace

TEST(net_bitstream_layout)
{
    BitStream b;
    b.writeBit(true);
    b.writeBit(false);
    b.writeBits(0b101, 3);
    b.alignWrite();
    b.writeU16(0x1234);
    b.writeU24(0xabcdef); // little-endian, aligned
    b.writeU32(0xdeadbeef);
    const auto bytes = std::vector<std::uint8_t>(b.bytes().begin(), b.bytes().end());
    CHECK(bytes == unhex("a81234efcdabdeadbeef"));

    BitStream r(bytes);
    bool bit = false;
    std::uint64_t three = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u24 = 0, u32 = 0;
    CHECK(r.readBit(bit));
    CHECK(bit);
    CHECK(r.readBit(bit));
    CHECK(!bit);
    CHECK(r.readBits(three, 3));
    CHECK_EQ(three, 5u);
    r.alignRead();
    CHECK(r.readU16(u16));
    CHECK_EQ(u16, 0x1234);
    CHECK(r.readU24(u24));
    CHECK_EQ(u24, 0xabcdefu);
    CHECK(r.readU32(u32));
    CHECK_EQ(u32, 0xdeadbeefu);
    std::uint8_t past = 0;
    CHECK(!r.readU8(past));
    CHECK(r.bad());
}

TEST(net_datagram_header_matches_client)
{
    // A few cases from emu/vectors/datagram_header.json (0x102d8219c), embedded.
    struct Case {
        bool ack, nak, b12, pp, cs, f16, fa, fb, optA, optB, u16;
        const char* hex;
    };
    const Case cases[] = {
        {false, false, false, false, false, false, false, false, false, false, false, "80efcdab"},
        {false, false, false, false, false, false, false, false, false, false, true, "80efcdab99aa"},
        {false, false, false, true, false, false, false, false, false, false, false, "88efcdab"},
        {false, false, false, true, true, false, true, true, true, true, false, "9befcdab"},
        {false, false, false, false, true, false, false, true, true, true, false, "89efcdab"},
    };
    for (const Case& c : cases) {
        DatagramHeader h;
        h.isAck = c.ack;
        h.isNak = c.nak;
        h.hasBAndAs = c.b12;
        h.isPacketPair = c.pp;
        h.isContinuousSend = c.cs;
        h.ackExtra = c.f16;
        h.flagA = c.fa;
        h.flagB = c.fb;
        h.datagramNumber = 0xabcdef;
        h.trailing = 0x99aa;
        HeaderOptions o;
        o.flagA = c.optA;
        o.flagB = c.optB;
        o.trailingU16 = c.u16;
        BitStream b;
        h.write(b, o);
        CHECK_EQ(hexString(b.bytes()), std::string(c.hex));
    }

    auto v = vectors("datagram_header.json");
    if (!v)
        SKIP("no emulator vectors here");
    std::size_t checked = 0;
    for (const auto& e : *v->array()) {
        if (num(e, "hasTime"))
            continue; // the client never sends timestamps
        DatagramHeader h;
        h.isAck = num(e, "isACK") != 0;
        h.isNak = !h.isAck && num(e, "isNAK") != 0;
        h.hasBAndAs = num(e, "f12") != 0;
        h.as = std::bit_cast<float>(0x55667788u);
        h.isPacketPair = num(e, "f13") != 0;
        h.isContinuousSend = num(e, "f14") != 0;
        h.ackExtra = num(e, "f16") != 0;
        h.flagA = num(e, "f1a") != 0;
        h.flagB = num(e, "f1b") != 0;
        h.datagramNumber = 0xabcdef;
        h.trailing = 0x99aa;
        HeaderOptions o;
        o.flagA = num(e, "flagA") != 0;
        o.flagB = num(e, "flagB") != 0;
        o.trailingU16 = num(e, "flagU16") != 0;
        BitStream b;
        h.write(b, o);
        // The client's u16 goes out even on ACK/NAK headers' tails; ours only on data.
        if (h.isAck || h.isNak) {
            if (!o.trailingU16)
                CHECK_EQ(hexString(b.bytes()), str(e, "bytes"));
        } else {
            CHECK_EQ(hexString(b.bytes()), str(e, "bytes"));
        }
        BitStream r(b.bytes());
        DatagramHeader back;
        CHECK(back.read(r, o));
        CHECK_EQ(back.isAck, h.isAck);
        if (!h.isAck && !h.isNak)
            CHECK_EQ(back.datagramNumber, h.datagramNumber);
        ++checked;
    }
    CHECK(checked > 1000);
}

TEST(net_internal_packets_match_client)
{
    auto v = vectors("internal_packet.json");
    if (!v)
        SKIP("no emulator vectors here");
    for (const auto& e : *v->array()) {
        InternalPacket p;
        p.reliability = static_cast<Reliability>(num(e, "reliability"));
        p.reliableMessageNumber = static_cast<std::uint32_t>(num(e, "reliableMessageNumber"));
        p.orderingIndex = static_cast<std::uint32_t>(num(e, "orderingIndex"));
        p.sequencingIndex = static_cast<std::uint32_t>(num(e, "sequencingIndex"));
        p.orderingChannel = static_cast<std::uint8_t>(num(e, "orderingChannel"));
        p.splitPacketCount = static_cast<std::uint32_t>(num(e, "splitPacketCount"));
        p.split = p.splitPacketCount != 0;
        p.splitPacketId = static_cast<std::uint16_t>(num(e, "splitPacketId"));
        p.splitPacketIndex = static_cast<std::uint32_t>(num(e, "splitPacketIndex"));
        p.bitLength = static_cast<std::size_t>(num(e, "bitLength"));
        p.payload = unhex(str(e, "data"));
        BitStream b;
        p.write(b);
        CHECK_EQ(hexString(b.bytes()), str(e, "out"));
        CHECK_EQ(b.bitsUsed(), static_cast<std::size_t>(num(e, "outBits")));

        BitStream r(b.bytes());
        InternalPacket back;
        std::string error;
        CHECK(back.read(r, false, error));
        CHECK_EQ(back.bitLength, p.bitLength);
        CHECK(back.payload == p.payload);
    }
}

TEST(net_ack_ranges_match_client)
{
    auto v = vectors("range_list.json");
    if (!v)
        SKIP("no emulator vectors here");
    for (const auto& e : *v->array()) {
        Ranges ranges;
        for (const auto& r : *e["ranges"]->array())
            ranges.emplace_back(static_cast<std::uint32_t>((*r.array())[0].number().value_or(0)),
                                static_cast<std::uint32_t>((*r.array())[1].number().value_or(0)));
        const std::size_t total = ranges.size();
        BitStream b;
        writeRanges(b, ranges, static_cast<std::size_t>(num(e, "maxBits")));
        CHECK_EQ(hexString(b.bytes()), str(e, "out"));
        CHECK_EQ(ranges.size(), static_cast<std::size_t>(num(e, "remaining")));

        BitStream r(b.bytes());
        Ranges back;
        CHECK(readRanges(r, back));
        CHECK_EQ(back.size() + ranges.size(), total);
    }
}

TEST(net_crypto_matches_libsodium_vectors)
{
    // The local server key pairs are X25519 pairs.
    CHECK(KeyPair::fromSecret(localkeys::earlySecret).publicKey == localkeys::earlyPublic);
    CHECK(KeyPair::fromSecret(localkeys::ephemeralEarlySecret).publicKey == localkeys::ephemeralEarlyPublic);

    auto v = vectors("crypto.json");
    if (!v)
        SKIP("no emulator vectors here");
    for (const auto& e : *v->array()) {
        if (str(e, "kind") != "kx")
            continue;
        KeyPair client{keyFromHex(str(e, "client_pk")), keyFromHex(str(e, "client_sk"))};
        const SessionKeys k = kxClient(client, keyFromHex(str(e, "server_pk")));
        CHECK_EQ(hexString(k.rx), str(e, "client_rx"));
        CHECK_EQ(hexString(k.tx), str(e, "client_tx"));
    }
}

TEST(net_handshake_and_datagram_ciphers)
{
    const KeyPair client = KeyPair::generate();
    const KeyPair early = KeyPair::fromSecret(localkeys::ephemeralEarlySecret);
    const SessionKeys c = kxClient(client, localkeys::ephemeralEarlyPublic);
    const SessionKeys s = kxServer(early, client.publicKey);
    CHECK(c.tx == s.rx);

    OpenRequest2Header h;
    h.clientPublic = client.publicKey;
    OpenRequest2Box box;
    box.clientGuid = 0x1122334455667788ULL;
    box.mtu = 1492;
    box.cipher = 1;
    box.serverAddress = {{127, 0, 0, 1}, 53640};
    box.token = "hello";
    const auto wire = encodeOpenRequest2(h, box, c.tx, 0, 1492 - kMtuOverhead);
    CHECK_EQ(wire.size(), std::size_t{1452});
    auto hh = decodeOpenRequest2Header(wire);
    CHECK(hh.has_value());
    CHECK(hh->clientPublic == client.publicKey);
    auto opened = openOpenRequest2Box(wire, *hh, s.rx);
    CHECK(opened.has_value());
    CHECK_EQ(opened->clientGuid, box.clientGuid);
    CHECK_EQ(opened->token, std::string("hello"));
    CHECK(opened->serverAddress == box.serverAddress);
    // A flipped AAD byte breaks the tag.
    auto bad = wire;
    bad[30] ^= 1;
    CHECK(!openOpenRequest2Box(bad, *hh, s.rx).has_value());

    for (Cipher cipher : {Cipher::ChaCha20Poly1305, Cipher::Aes256Gcm}) {
        if (cipher == Cipher::Aes256Gcm && !aesGcmAvailable())
            continue;
        DatagramCipher tx(cipher, c), rx(cipher, SessionKeys{s.rx, s.tx});
        std::vector<std::uint8_t> plain{1, 2, 3, 4, 5};
        auto w = tx.seal(plain);
        CHECK_EQ(w.size(), plain.size() + kDatagramOverhead);
        // The nonce's low 16 bits ride in the clear: "UniqueNu" -> 0x6e55.
        CHECK_EQ(w[5], 0x55);
        CHECK_EQ(w[6], 0x6e);
        auto back = rx.open(w);
        CHECK(back.has_value() && *back == plain);
        // Reordered and lost packets still open (the counter is rebuilt from 16 bits).
        std::vector<std::vector<std::uint8_t>> later;
        later.reserve(70000);
        for (int i = 0; i < 70000; ++i)
            later.push_back(tx.seal(plain));
        CHECK(rx.open(later[30000]).has_value());
        CHECK(rx.open(later[60000]).has_value());
        CHECK(rx.open(later[59990]).has_value());
        CHECK(!rx.open(later[100]).has_value()); // more than 0x8000 behind: the counter can't be rebuilt
    }
}

TEST(net_offline_messages_round_trip)
{
    OpenRequest1 r1;
    r1.mtu = 1200;
    const auto w1 = encodeOpenRequest1(r1);
    CHECK_EQ(w1.size(), std::size_t{1160});
    CHECK_EQ(w1[0], 0x7b);
    CHECK_EQ(w1[17], 5);
    CHECK_EQ(w1[18], 1);
    auto d1 = decodeOpenRequest1(w1);
    CHECK(d1.has_value());
    CHECK_EQ(d1.value().mtu, std::size_t{1200});

    OpenReply1 p1{0xfeedface01020304ULL, 0, 1200};
    const auto w2 = encodeOpenReply1(p1);
    CHECK_EQ(w2.size(), std::size_t{28});
    auto d2 = decodeOpenReply1(w2);
    CHECK(d2.has_value());
    CHECK_EQ(d2.value().serverGuid, p1.serverGuid);
    CHECK_EQ(d2.value().mtu, 1200);

    const auto rej = encodeRejection({offline::NoFreeIncomingConnections, 42, 0});
    CHECK_EQ(rej.size(), std::size_t{25});
    auto dr = decodeRejection(rej);
    CHECK(dr.has_value());
    CHECK_EQ(dr.value().guid, 42u);
    CHECK(isRejectionId(0x14));
    CHECK(isRejectionId(0x1a));
    CHECK(!isRejectionId(0x10));

    // Readable output names the fields.
    const std::string text = render(describeOffline(w2));
    CHECK(text.find("RbxOpenReply1") != std::string::npos);
    CHECK(text.find("feedface01020304") != std::string::npos);
}

TEST(net_reliability_orders_splits_and_survives_loss)
{
    ReliabilityLayer::Options o;
    o.mtu = 576;
    o.minRtoMs = 10;
    o.initialRtoMs = 20;
    ReliabilityLayer a(o), b(o);
    std::mt19937 rng(5);       // NOLINT(bugprone-random-generator-seed): reproducible loss pattern
    const unsigned kLoss = 25; // percent of datagrams dropped, both ways
    std::vector<std::vector<std::uint8_t>> sent;
    for (int i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> m(static_cast<std::size_t>(1 + (i * 37) % (i % 10 == 0 ? 3000 : 200)));
        for (std::size_t j = 0; j < m.size(); ++j)
            m[j] = static_cast<std::uint8_t>(static_cast<std::size_t>(i) + j);
        m[0] = 0x83;
        sent.push_back(m);
        a.send(m, Reliability::ReliableOrdered, Priority::High, static_cast<std::uint8_t>(i % 3));
    }
    std::vector<std::vector<std::uint8_t>> got[3];
    std::uint64_t now = 1000;
    for (int round = 0; round < 2000 && (got[0].size() + got[1].size() + got[2].size()) < sent.size(); ++round) {
        now += 5;
        for (auto& d : a.flush(now))
            if (rng() % 100 >= kLoss) // loss
                b.onDatagram(d, now);
        for (auto& d : b.flush(now))
            if (rng() % 100 >= kLoss)
                a.onDatagram(d, now);
        while (auto m = b.receive()) {
            const std::size_t i = static_cast<std::size_t>(&*std::ranges::find(sent, *m) - &sent[0]);
            got[i % 3].push_back(*m);
        }
    }
    CHECK_EQ(got[0].size() + got[1].size() + got[2].size(), sent.size());
    for (int ch = 0; ch < 3; ++ch) // in order within each channel
        for (std::size_t k = 0; k < got[ch].size(); ++k)
            CHECK(got[ch][k] == sent[k * 3 + static_cast<std::size_t>(ch)]);
    CHECK(a.stats().resends > 0);
    CHECK_EQ(b.stats().messagesReceived, sent.size());
}

TEST(net_reliability_sends_by_priority_then_in_turn)
{
    // A big Low message queued first, then an Immediate one and two High
    // ones: the Immediate goes out first, the High ones next in the order
    // they came, the big one's pieces last.
    ReliabilityLayer::Options o;
    o.mtu = 576;
    ReliabilityLayer a(o), b(o);
    const auto message = [](std::uint8_t tag, std::size_t size) {
        std::vector<std::uint8_t> m(size, tag);
        m[0] = 0x83;
        return m;
    };
    a.send(message(1, 20000), Reliability::Reliable, Priority::Low, 0);
    a.send(message(2, 10), Reliability::Reliable, Priority::High, 0);
    a.send(message(3, 10), Reliability::Reliable, Priority::Immediate, 0);
    a.send(message(4, 10), Reliability::Reliable, Priority::High, 0);
    std::vector<std::uint8_t> order;
    std::uint64_t now = 1000;
    for (int round = 0; round < 200 && order.size() < 4; ++round) {
        now += 5;
        for (auto& d : a.flush(now))
            b.onDatagram(d, now);
        for (auto& d : b.flush(now))
            a.onDatagram(d, now);
        while (auto m = b.receive())
            order.push_back((*m)[1]);
    }
    CHECK(order == std::vector<std::uint8_t>({3, 2, 4, 1}));
}

TEST(net_peer_loopback_connects_and_exchanges)
{
    Peer::Options so;
    so.logLevel = 0;
    // A server without one of our bits: the connection keeps only the shared ones.
    so.capabilities = lunar::net::kBaseCapabilities & ~(std::uint64_t{1} << 39);
    Peer server(Peer::Role::Server, so);
    if (!server.listen("127.0.0.1", 0))
        SKIP("can't bind a UDP socket here");
    Peer::Options co;
    co.logLevel = 0;
    Peer client(Peer::Role::Client, co);
    CHECK(client.connect(server.localAddress()).has_value());
    int serverConn = -1, clientConn = -1;
    std::vector<std::uint8_t> big(5000, 0x42);
    big[0] = 0x83;
    bool serverGot = false, clientGot = false;
    for (int i = 0; i < 400 && !(serverGot && clientGot); ++i) {
        client.update(1);
        server.update(1);
        while (auto e = server.poll()) {
            if (e->kind == Peer::Event::Kind::Connected) {
                serverConn = e->connection;
                server.send(serverConn, big);
            } else if (e->kind == Peer::Event::Kind::Message) {
                serverGot = e->data == std::vector<std::uint8_t>{0x83, 1, 2, 3};
            }
        }
        while (auto e = client.poll()) {
            if (e->kind == Peer::Event::Kind::Connected) {
                clientConn = e->connection;
                client.send(clientConn, {0x83, 1, 2, 3});
            } else if (e->kind == Peer::Event::Kind::Message) {
                clientGot = e->data == big;
            }
            CHECK(e->kind != Peer::Event::Kind::Failed);
        }
    }
    CHECK(serverConn > 0);
    CHECK(clientConn > 0);
    CHECK(serverGot);
    CHECK(clientGot);
    CHECK_EQ(client.negotiated(clientConn), so.capabilities);
    CHECK_EQ(server.negotiated(serverConn), so.capabilities);
}

TEST(net_open_request1_matches_client)
{
    auto v = vectors("open_request1.json");
    if (!v)
        SKIP("no emulator vectors here");
    for (const auto& e : *v->array()) {
        OpenRequest1 r;
        r.mtu = static_cast<std::size_t>(num(e, "padBytes")) + kMtuOverhead;
        CHECK_EQ(hexString(encodeOpenRequest1(r)), str(e, "out"));
    }
}

TEST(net_session_crypto_matches_client)
{
    auto v = vectors("session_crypto.json");
    if (!v)
        SKIP("no emulator vectors here");
    std::size_t checked = 0;
    for (const auto& e : *v->array()) {
        const Key key = keyFromHex(str(e, "key"));
        if (str(e, "kind") == "handshake") {
            const auto aad = unhex(str(e, "aad")), box = unhex(str(e, "box"));
            const auto out = sealHandshake(aad, box, key, static_cast<std::uint64_t>(num(e, "counter")));
            CHECK_EQ(hexString(out), str(e, "out"));
            ++checked;
        } else if (!str(e, "plain").empty()) {
            const auto plain = unhex(str(e, "plain"));
            DatagramCipher c(Cipher::ChaCha20Poly1305, SessionKeys{key, key});
            c.setCounters(std::stoull(str(e, "counterHex"), nullptr, 16), kNonceStart);
            CHECK_EQ(hexString(c.seal(plain)), str(e, "out"));
            ++checked;
        }
    }
    CHECK(checked >= 4);
}

// The readable text of a value is only made when asked for (the live
// client never asks): read with and without it, every value reads the same
// and leaves the stream at the same byte.
TEST(net_values_read_the_same_without_their_text)
{
    NumberSequence numbers({{0.0f, 1.0f, 0.0f}, {1.0f, 0.5f, 0.25f}});
    ColorSequence colors({{0.0f, Color3{1, 0, 0}}, {1.0f, Color3{0, 0, 1}}});
    const std::pair<std::uint8_t, Value> values[] = {
        {nettype::Bool, Value(true)},
        {nettype::Int, Value(-7.0)},
        {nettype::Float, Value(0.5)},
        {nettype::Double, Value(1.25)},
        {nettype::String, Value(std::string("hello"))},
        {nettype::UDim2, Value(UDim2{{0.5f, 3}, {0.25f, -4}})},
        {nettype::Color3, Value(Color3{0.1f, 0.2f, 0.3f})},
        {nettype::Color3uint8, Value(Color3::fromRGB(10, 20, 30))},
        {nettype::Vector3, Value(Vector3{1, 2, 3})},
        {nettype::Vector2, Value(Vector2{4, 5})},
        {nettype::CFrame, Value(CFrame::angles(0.3f, 0.2f, 0.1f))},
        {nettype::CFrameCompressed, Value(CFrame::angles(0.1f, 0.4f, 0.0f))},
        {nettype::BrickColor, Value(BrickColor::fromNumber(194))},
        {nettype::NumberSequence, Value(numbers)},
        {nettype::ColorSequence, Value(colors)},
        {nettype::NumberRange, Value(NumberRange{1, 2})},
        {nettype::Rect2D, Value(Rect{{0, 1}, {2, 3}})},
        {nettype::Object, Value()},
        {nettype::ContentId, Value(std::string("rbxassetid://1234"))},
        {nettype::DictionaryString, Value(std::string("a string not in any dictionary"))},
    };
    NetworkStream out;
    for (const auto& [type, value] : values)
        CHECK(writeValue(out, type, value));
    NetworkStream described(out.bytes()), plain(out.bytes());
    for (const auto& [type, value] : values) {
        std::string text;
        const auto a = readValue(described, type, {}, &text);
        const auto b = readValue(plain, type);
        CHECK(a.has_value() && b.has_value());
        CHECK(!text.empty());
        CHECK_EQ(plain.offset(), described.offset());
        CHECK_EQ(toString(b.value_or(Value())), toString(a.value_or(Value())));
    }
    CHECK_EQ(plain.left(), std::size_t{0});
}

TEST(net_values_match_client)
{
    auto v = vectors("values.json");
    if (!v)
        SKIP("no emulator vectors here");
    auto cases = [&](const char* name) -> const assets::json::Array& { return *(*v)[name]->array(); };
    for (const auto& e : cases("varint_u32")) {
        NetworkStream s;
        s.writeVarint(static_cast<std::uint64_t>(num(e, "input")));
        CHECK_EQ(hexString(s.bytes()), str(e, "out"));
    }
    for (const auto& e : cases("zigzag32")) {
        NetworkStream s;
        s.writeZigzag32(static_cast<std::int32_t>(num(e, "input")));
        CHECK_EQ(hexString(s.bytes()), str(e, "out"));
        NetworkStream r(s.bytes());
        CHECK_EQ(r.readZigzag32(), static_cast<std::int32_t>(num(e, "input")));
    }
    for (const auto& e : cases("string")) {
        const auto bytes = unhex(str(e, "input"));
        NetworkStream s;
        s.writeString(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        CHECK_EQ(hexString(s.bytes()), str(e, "out"));
    }
    for (const auto& e : cases("UDim")) {
        const auto& in = *e["input"]->array();
        NetworkStream s;
        writeValue(s, nettype::UDim,
                   Value(UDim{static_cast<float>(in[0].number().value_or(0)),
                              static_cast<std::int32_t>(in[1].number().value_or(0))}));
        CHECK_EQ(hexString(s.bytes()), str(e, "out"));
    }
    // Lossless through lunar::Value: decode the client's bytes, encode them again.
    const std::pair<const char*, std::uint8_t> lossless[] = {{"UDim2", nettype::UDim2},
                                                             {"Vector2", nettype::Vector2},
                                                             {"Vector3", nettype::Vector3},
                                                             {"Vector3int16", nettype::Vector3int16},
                                                             {"Rect2D", nettype::Rect2D},
                                                             {"NumberSequence", nettype::NumberSequence},
                                                             {"PhysicalProperties", nettype::PhysicalProperties}};
    for (auto [name, type] : lossless) {
        for (const auto& e : cases(name)) {
            const auto wire = unhex(str(e, "out"));
            NetworkStream r(wire);
            auto value = readValue(r, type);
            CHECK(value.has_value());
            CHECK_EQ(r.left(), std::size_t{0});
            NetworkStream w;
            CHECK(writeValue(w, type, *value));
            CHECK_EQ(hexString(w.bytes()), str(e, "out"));
        }
    }
    // The rest at least parse to the byte.
    const std::pair<const char*, std::uint8_t> parsed[] = {{"Ray", nettype::Ray},
                                                           {"Faces", nettype::Faces},
                                                           {"Axes", nettype::Axes},
                                                           {"BrickColor", nettype::BrickColor},
                                                           {"Color3uint8", nettype::Color3uint8},
                                                           {"Vector2int16", nettype::Vector2int16},
                                                           {"Region3int16", nettype::Region3int16},
                                                           {"ColorSequence", nettype::ColorSequence}};
    for (auto [name, type] : parsed) {
        for (const auto& e : cases(name)) {
            const auto wire = unhex(str(e, "out"));
            NetworkStream r(wire);
            CHECK(readValue(r, type).has_value());
            CHECK_EQ(r.left(), std::size_t{0});
        }
    }
}

TEST(net_cframes_match_client)
{
    auto v = vectors("cframe.json");
    if (!v)
        SKIP("no emulator vectors here");
    auto cframe = [](const assets::json::Value& e) {
        CFrame cf;
        const auto& rows = *e["rows"]->array();
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                cf.r[i][j] = static_cast<float>((*rows[i].array())[j].number().value_or(0));
        if (const auto* pos = e["pos"]) {
            cf.position = {static_cast<float>((*pos->array())[0].number().value_or(0)),
                           static_cast<float>((*pos->array())[1].number().value_or(0)),
                           static_cast<float>((*pos->array())[2].number().value_or(0))};
        } else {
            cf.position = {1.5f, -2.25f, 1000.0f};
        }
        return cf;
    };
    for (const auto& e : *(*v)["axis_aligned"]->array()) {
        for (auto [key, type] : {std::pair{"t26", nettype::CFrame}, std::pair{"t27", nettype::CFrameCompressed}}) {
            NetworkStream s;
            CHECK(writeValue(s, type, Value(cframe(e))));
            CHECK_EQ(hexString(s.bytes()), str(e, key));
        }
    }
    for (auto [key, type] :
         {std::pair{"random26", nettype::CFrame}, std::pair{"random27", nettype::CFrameCompressed}}) {
        for (const auto& e : *(*v)[key]->array()) {
            NetworkStream s;
            CHECK(writeValue(s, type, Value(cframe(e))));
            CHECK_EQ(hexString(s.bytes()), str(e, "out"));
            NetworkStream r(s.bytes());
            auto back = readValue(r, type);
            CHECK(back.has_value());
            const CFrame& b = std::get<CFrame>(*back);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    CHECK(std::abs(b.r[i][j] - cframe(e).r[i][j]) < 1e-3f);
        }
    }
}

TEST(net_framing_tables_and_hash)
{
    for (int i = 0; i < 256; ++i) {
        const auto b = static_cast<std::uint8_t>(i);
        CHECK_EQ(decodePacketId(encodePacketId(b)), b);
        CHECK_EQ(decodeItemType(encodeItemType(b)), b);
        if (i < 0x80)
            CHECK_EQ(encodePacketId(b), b);
    }
    CHECK_EQ(encodePacketId(0x83), 0xcc); // ROBLOX-LAYER.md 1.1
    CHECK_EQ(encodeItemType(0x02), 0xae); // 1.2
    const std::vector<std::uint8_t> a{'a'};
    CHECK_EQ(fnv1a32(a), 0xe40c292cu); // FNV-1a 32 of "a"
    const std::vector<std::uint8_t> message{0x83, 1, 2, 3};
    const auto framed = frame(message, {.remapIds = true, .hashed = true});
    CHECK_EQ(framed[0], 0xaa);
    CHECK_EQ(framed[5], 0xcc);
    auto back = unframe(framed, {.remapIds = true, .hashed = true});
    CHECK(back.has_value());
    CHECK(back.value() == message);
    auto broken = framed;
    broken[7] ^= 1;
    CHECK(!unframe(broken, {.remapIds = true, .hashed = true}).has_value());
}

// A capture's datagrams decoded quietly (the replays and the benchmark) build
// the same replica as the decode that describes every message: the messages
// go to it in the same order, the split ones once their last piece is in.
TEST(net_capture_decoder_quiet_builds_the_same_replica)
{
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    for (int i = 0; i < 40; ++i) {
        auto part = ClassRegistry::create("Part");
        part->setName(std::format("Part{}", i));
        part->set("Transparency", Value(i / 40.0));
        part->setParent(workspace);
    }
    auto echo = ClassRegistry::create("RemoteEvent");
    echo->setParent(game->getService("ReplicatedStorage"));
    ReplicationServer server(game, {"Workspace", "ReplicatedStorage"});
    std::vector<std::vector<std::uint8_t>> messages = server.joinMessages(512);
    messages.push_back(server.invoke(*echo, "OnClientEvent", {Value(std::string("hi")), Value(3.0)}));
    messages.push_back({msg::ConnectedPing, 0, 0, 0, 0, 0, 0, 0, 1}); // RakNet's own: not the replica's

    // Server to client, sealed with the keys a keylog names: two messages a
    // datagram, and any longer than 300 bytes split into pieces across them.
    const SystemAddress client{{10, 0, 0, 1}, 50000}, host{{10, 0, 0, 2}, 53640};
    const Key c2s = keyFromHex("0101010101010101010101010101010101010101010101010101010101010101");
    const Key s2c = keyFromHex("0202020202020202020202020202020202020202020202020202020202020202");
    const std::filesystem::path keylog = std::filesystem::temp_directory_path() / "lunarstone-quiet-capture.keys";
    std::ofstream(keylog) << std::format("LUNARNET_SESSION client={} server={} cipher=1 caps={:x} c2s={} s2c={}\n",
                                         client.toString(), host.toString(), kBaseCapabilities, hexString(c2s),
                                         hexString(s2c));
    DatagramCipher seal(Cipher::ChaCha20Poly1305, SessionKeys{c2s, s2c});
    const HeaderOptions header = headerOptions(kBaseCapabilities);
    std::vector<InternalPacket> packets;
    std::uint32_t reliable = 0;
    std::uint16_t splitId = 0;
    for (const auto& m : messages) {
        const std::size_t pieces = (m.size() + 299) / 300;
        for (std::size_t i = 0; i < pieces; ++i) {
            InternalPacket p;
            p.reliability = Reliability::Reliable;
            p.reliableMessageNumber = reliable++;
            p.payload.assign(m.begin() + static_cast<std::ptrdiff_t>(i * 300),
                             m.begin() + static_cast<std::ptrdiff_t>(std::min(m.size(), (i + 1) * 300)));
            p.bitLength = p.payload.size() * 8;
            if (pieces > 1) {
                p.split = true;
                p.splitPacketCount = static_cast<std::uint32_t>(pieces);
                p.splitPacketId = splitId;
                p.splitPacketIndex = static_cast<std::uint32_t>(i);
            }
            packets.push_back(std::move(p));
        }
        splitId = static_cast<std::uint16_t>(splitId + (pieces > 1 ? 1 : 0));
    }
    std::vector<CapturedDatagram> capture;
    for (std::size_t i = 0; i < packets.size(); i += 2) {
        BitStream out;
        DatagramHeader h;
        h.datagramNumber = static_cast<std::uint32_t>(capture.size());
        h.write(out, header);
        for (std::size_t k = i; k < std::min(packets.size(), i + 2); ++k)
            packets[k].write(out, header.tracer);
        capture.push_back({static_cast<double>(capture.size()) * 0.01, host, client, seal.seal(out.bytes())});
    }

    CaptureDecoder described, quiet(DescribeOptions{.quiet = true});
    described.loadKeylog(keylog);
    quiet.loadKeylog(keylog);
    std::filesystem::remove(keylog);
    for (std::size_t i = 0; i < capture.size(); ++i) {
        CHECK(!render(described.decode(capture[i], i)).empty());
        CHECK(quiet.decode(capture[i], i).children.empty());
    }
    CHECK_EQ(described.replicas().size(), std::size_t{1});
    CHECK_EQ(quiet.replicas().size(), std::size_t{1});
    const Replica& a = *described.replicas().front();
    const Replica& b = *quiet.replicas().front();
    CHECK_EQ(a.instanceCount(), server.replicatedCount());
    CHECK_EQ(b.instanceCount(), a.instanceCount());
    CHECK_EQ(b.markers.size(), a.markers.size());
    CHECK_EQ(b.events.size(), std::size_t{1});
    CHECK_EQ(b.events.size(), a.events.size());
    CHECK_EQ(b.decodeFailures, a.decodeFailures);
    const Instance* part = b.game()->workspace()->findFirstChild("Part39");
    CHECK(part != nullptr);
    CHECK(std::abs(std::get<double>(part->get("Transparency")) - 39 / 40.0) < 1e-6);
    CHECK_EQ(toString(part->get("Transparency")),
             toString(a.game()->workspace()->findFirstChild("Part39")->get("Transparency")));
}

TEST(net_schema_and_data_round_trip)
{
    const Schema schema = Schema::fromEngine();
    CHECK(schema.classes.size() > 100);
    auto back = decodeSchemaPacket(encodeSchemaPacket(schema));
    CHECK(back.has_value());
    CHECK_EQ(back.value().properties.size(), schema.properties.size());
    CHECK_EQ(back.value().classes[10].name, schema.classes[10].name);

    // A tiny place: a part with a weld pointing at a part that comes later.
    auto game = DataModel::create();
    auto* workspace = game->getService("Workspace");
    auto a = ClassRegistry::create("Part");
    a->setName("A");
    a->set("Transparency", Value(0.25));
    a->set("CFrame", Value(CFrame::angles(0.3f, 0.2f, 0.1f)));
    a->setParent(workspace);
    auto weld = ClassRegistry::create("WeldConstraint");
    weld->setParent(a);
    auto b = ClassRegistry::create("Part");
    b->setName("B");
    b->setParent(workspace);
    weld->set("Part0", Value(a));
    weld->set("Part1", Value(b));
    auto echo = ClassRegistry::create("RemoteEvent");
    echo->setParent(game->getService("ReplicatedStorage"));

    ReplicationServer server(game, {"Workspace", "ReplicatedStorage"});
    Replica client;
    for (const auto& m : server.joinMessages(64))
        CHECK(client.handle(m));
    CHECK_EQ(client.instanceCount(), server.replicatedCount());
    CHECK_EQ(client.markers.size(), std::size_t{1});
    auto ca = client.find(server.idOf(a.get()));
    auto cw = client.find(server.idOf(weld.get()));
    CHECK(ca != nullptr);
    CHECK(cw != nullptr);
    CHECK_EQ(toString(ca->get("Transparency")), toString(a->get("Transparency")));
    CHECK_EQ(toString(ca->get("CFrame")), toString(a->get("CFrame")));
    CHECK_EQ(toString(cw->get("Part1")), toString(weld->get("Part1"))); // a forward reference

    // A change, an event, a delete.
    a->set("Transparency", Value(0.75));
    for (const auto& m : server.takeChanges())
        CHECK(client.handle(m));
    CHECK_EQ(toString(ca->get("Transparency")), std::string("0.75"));
    CHECK(client.handle(server.invoke(*echo, "OnClientEvent", {Value(std::string("hi")), Value(3.0)})));
    CHECK_EQ(client.events.size(), std::size_t{1});
    CHECK_EQ(client.events[0].arguments.size(), std::size_t{2});
    Node tree;
    const std::array<Item, 1> del{DeleteItem{server.idOf(b.get())}};
    CHECK(client.handle(encodeData(del, server.schema(), server.context()), &tree));
    CHECK(client.find(server.idOf(b.get())) == nullptr);
    CHECK(render(tree).find("Delete") != std::string::npos);

    JoinRequest request;
    request.luauMax = 6;
    request.joinInfo = "{}";
    auto req = decodeJoinRequest(encodeJoinRequest(request, kBaseCapabilities), kBaseCapabilities);
    CHECK(req.has_value());
    CHECK_EQ(req.value().luauMax, 6);
    CHECK_EQ(req.value().joinInfo, std::string("{}"));
}

// A reference to an instance the same message creates, or a later one
// (servers send a character model and Player.Character together): the
// replica sets it once the instance is there.
TEST(net_replica_resolves_references_to_instances_not_seen_yet)
{
    auto game = DataModel::create();
    auto* storage = game->getService("ReplicatedStorage");
    auto pointer = ClassRegistry::create("ObjectValue");
    pointer->setParent(storage);
    ReplicationServer server(game, {"ReplicatedStorage"});
    Replica client;
    for (const auto& m : server.joinMessages(64))
        CHECK(client.handle(m));
    auto clientPointer = client.find(server.idOf(pointer.get()));
    CHECK(clientPointer != nullptr);
    auto pointsAt = [&]() -> InstancePtr {
        const Value value = clientPointer->get("Value");
        const auto* target = std::get_if<InstancePtr>(&value);
        return target ? *target : nullptr;
    };

    // The new instance and the reference to it in one message.
    auto first = ClassRegistry::create("Folder");
    first->setParent(storage);
    auto created = decodeData(server.addInstance(first), server.schema(), server.context());
    CHECK(created.has_value());
    pointer->set("Value", Value(first));
    auto changes = server.takeChanges();
    CHECK_EQ(changes.size(), std::size_t{1});
    auto changed = decodeData(changes[0], server.schema(), server.context());
    CHECK(changed.has_value());
    std::vector<Item> both = std::move(*created);
    for (Item& item : *changed)
        both.push_back(std::move(item));
    CHECK(client.handle(encodeData(both, server.schema(), server.context())));
    CHECK(pointsAt() != nullptr);
    CHECK(pointsAt() == client.find(server.idOf(first.get())));

    // The reference first, the instance in a later message.
    auto second = ClassRegistry::create("Folder");
    second->setParent(storage);
    const auto later = server.addInstance(second);
    pointer->set("Value", Value(second));
    for (const auto& m : server.takeChanges())
        CHECK(client.handle(m));
    CHECK(pointsAt() == client.find(server.idOf(first.get()))); // unchanged until it arrives
    CHECK(client.handle(later));
    CHECK(pointsAt() != nullptr);
    CHECK(pointsAt() == client.find(server.idOf(second.get())));
}
