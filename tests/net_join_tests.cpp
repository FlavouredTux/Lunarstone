#include "fake_web.hpp"
#include "framework.hpp"

#include <lunar/assets/Json.hpp>
#include <lunar/net/Crypto.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/Join.hpp>
#include <lunar/net/Peer.hpp>
#include <lunar/net/Rupp.hpp>
#include <lunar/net/Socket.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace lunar;
using namespace lunar::net;
using namespace fakeweb;

namespace {

// Shaped like a real join-game answer (place 1818, 2026) with made-up
// tickets and ids; the two public keys are the ones that answer came with.
const std::string kResponse = R"({"jobId":"a18fd930-77fd-4f63-9da0-611906f020ac","status":2,"statusData":null,)"
                              R"("joinScriptUrl":"https://assetgame.roblox.com/Game/Join.ashx?ticket=x&signature=y",)"
                              R"("authenticationUrl":"","authenticationTicket":"AUTH-TICKET-SECRET","message":null,)"
                              R"("joinScript":{"ClientPort":0,"MachineAddress":"10.9.6.20","ServerPort":64318,)"
                              R"("ServerConnections":[{"Address":"10.9.6.20","Port":64318}],)"
                              R"("UdmuxEndpoints":[{"Address":"128.116.44.33","Port":64318}],"DirectServerReturn":true,)"
                              R"("TokenGenAlgorithm":1,"PepperId":1791754125,"TokenValue":"AAECAwQFBgcICQoLDA0ODw==",)"
                              R"("PingUrl":"","PingInterval":120,"UserName":"tester","DisplayName":"Tester",)"
                              R"("UserId":123456,"CharacterAppearance":"https://api.roblox.com/v1.1/avatar-fetch/?placeId=1818&userId=123456",)"
                              R"("ClientTicket":"CLIENT-TICKET-SECRET","GameId":"a18fd930-77fd-4f63-9da0-611906f020ac","PlaceId":1818,)"
                              R"("MembershipType":"None","AccountAge":176,"SessionId":"{\"SessionId\":\"SESSION-SECRET\",\"PlaceId\":1818}",)"
                              R"("DataCenterId":25853,"UniverseId":13058,"CountryCode":"CH",)"
                              R"("ClientPublicKeyData":"{\"creationTime\":\"11:55 12/6/2022\",\"applications\":{\"RakNetEarlyPublicKey\":{\"versions\":[{\"id\":5,\"value\":\"Jy0uXN86HFuODRudIzEzZeLfCd8/z+ER6CMO0wAa5XY=\",\"allowed\":true}],\"send\":5,\"revert\":5}}}",)"
                              R"("RccVersion":"0.742.0.7421058","EphemeralEarlyPubKey":"mgdV6IyMLPFxVPutP9RTzfZCyw2EPHwcO1diT7yjOFo%3D",)"
                              R"("PlaceVersion":96,"NetStackPort":55600,"NetStackTokenValue":"TdJCfbwdJN9yTvDtPn0DXw=="},)"
                              R"("joinTicket":{"SerializedClientFields":"eyJB","EncryptedServerFields":{"nonce":"x","ciphertext":"y"}},"queuePosition":0})";

} // namespace

TEST(net_join_parse_response)
{
    auto r = parseJoinResponse(kResponse);
    CHECK(r.has_value());
    CHECK_EQ(r->status, 2);
    CHECK_EQ(r->jobId, "a18fd930-77fd-4f63-9da0-611906f020ac");
    CHECK_EQ(r->authenticationTicket, "AUTH-TICKET-SECRET");
    CHECK(r->joinScript.has_value());
    const JoinScript& s = *r->joinScript;
    CHECK_EQ(s.machineAddress, "10.9.6.20");
    CHECK_EQ(s.serverPort, 64318);
    CHECK_EQ(s.udmuxEndpoints.size(), 1u);
    CHECK_EQ(s.udmuxEndpoints[0].address, "128.116.44.33");
    CHECK_EQ(s.udmuxEndpoints[0].port, 64318);
    CHECK(s.directServerReturn);
    CHECK_EQ(s.tokenGenAlgorithm, 1);
    CHECK_EQ(s.pepperId, 1791754125);
    CHECK_EQ(s.tokenValue, "AAECAwQFBgcICQoLDA0ODw==");
    CHECK_EQ(s.userId, 123456);
    CHECK_EQ(s.userName, "tester");
    CHECK_EQ(s.placeId, 1818);
    CHECK_EQ(s.universeId, 13058);
    CHECK_EQ(s.placeVersion, 96);
    CHECK_EQ(s.rccVersion, "0.742.0.7421058");
    CHECK_EQ(s.clientTicket, "CLIENT-TICKET-SECRET");
    CHECK(s.sessionId.starts_with("{\"SessionId\""));
    // The raw text is the object exactly as sent.
    CHECK(s.raw.starts_with("{\"ClientPort\":0,\"MachineAddress\""));
    CHECK(s.raw.ends_with("\"NetStackTokenValue\":\"TdJCfbwdJN9yTvDtPn0DXw==\"}"));
    CHECK(!parseJoinResponse("[]").has_value());
    CHECK(!parseJoinResponse("{\"jobId\":\"x\"}").has_value());
    auto waiting = parseJoinResponse(R"({"jobId":null,"status":0,"joinScript":null,"message":"Waiting"})");
    CHECK(waiting.has_value());
    CHECK_EQ(waiting->status, 0);
    CHECK(!waiting->joinScript.has_value());
    CHECK(joinStatusRetryable(0));
    CHECK(joinStatusRetryable(22));
    CHECK(!joinStatusRetryable(2));
    CHECK(!joinStatusRetryable(12));
    CHECK_EQ(joinStatusName(2), "Joining server");
    CHECK_EQ(joinRetrySeconds(22, 1, 3), 2.0);
    CHECK_EQ(joinRetrySeconds(22, 9, 3), 10.0);
    CHECK_EQ(joinRetrySeconds(0, 0, 0), 1.0);
    CHECK(joinRetrySeconds(0, 0, 3) > 2.1 && joinRetrySeconds(0, 0, 3) < 2.3);
    CHECK_EQ(joinRetrySeconds(6, std::nullopt, 40), 10.0);
}

TEST(net_join_raw_member)
{
    const std::string_view o = R"({"a":1, "b":"x\"y", "c":{"d":[1,{"e":2}]}, "f":null})";
    CHECK_EQ(std::string(rawJsonMember(o, "a")), "1");
    CHECK_EQ(std::string(rawJsonMember(o, "b")), R"("x\"y")");
    CHECK_EQ(std::string(rawJsonMember(o, "c")), R"({"d":[1,{"e":2}]})");
    CHECK_EQ(std::string(rawJsonMember(o, "f")), "null");
    CHECK_EQ(std::string(rawJsonMember(o, "zz")), "");
}

TEST(net_join_server_keys)
{
    auto r = parseJoinResponse(kResponse);
    CHECK(r.has_value() && r->joinScript.has_value());
    auto keys = serverKeysOf(*r->joinScript);
    CHECK(keys.has_value());
    CHECK_EQ(keys->keyExchangeVersion, 5);
    CHECK_EQ(hexString(keys->earlyPublic), "272d2e5cdf3a1c5b8e0d1b9d23313365e2df09df3fcfe111e8230ed3001ae576");
    CHECK_EQ(hexString(keys->ephemeralEarlyPublic),
             "9a0755e88c8c2cf17154fbad3fd453cdf642cb0d843c7c1c3b57624fbca3385a");
    // Base64 forms: padded, unpadded, URL-encoded.
    CHECK(decodeBase64Key("Jy0uXN86HFuODRudIzEzZeLfCd8/z+ER6CMO0wAa5XY=").has_value());
    CHECK(decodeBase64Key("Jy0uXN86HFuODRudIzEzZeLfCd8/z+ER6CMO0wAa5XY").has_value());
    CHECK(decodeBase64Key("Jy0uXN86HFuODRudIzEzZeLfCd8%2Fz%2BER6CMO0wAa5XY%3D").has_value());
    CHECK(!decodeBase64Key("AAEC").has_value());
    CHECK(!decodeBase64Key("not base64!").has_value());
    auto bytes = decodeBase64("AAECAwQFBgcICQoLDA0ODw==");
    CHECK(bytes.has_value() && bytes->size() == 16 && (*bytes)[15] == 15);
    auto doc = parseVersionedKey(
        R"({"applications":{"X":{"versions":[{"id":1,"value":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","allowed":false},{"id":2,"value":"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=","allowed":true}],"send":2,"revert":1}}})",
        "X");
    CHECK(doc.has_value());
    CHECK_EQ(doc->send, 2);
    CHECK(!doc->key(1).has_value()); // not allowed
    CHECK(doc->key(2).has_value());
    CHECK(!parseVersionedKey("{}", "X").has_value());
}

// A ClientTicket shaped like a live one (timestamp; 258-byte signature;
// 33-byte pre-auth; 66-byte token; kind 17), made-up bytes.
const std::string kTicket = "2026-10-12T06:40:00.1234567Z;AAcOFRwjKjE4P0ZNVFtiaXB3foWMk5qhqK+2vcTL0tng5+71/AMKERgfJi00O0JJUFdeZWxzeoGIj5adpKuyucDHztXc4+rx+P8GDRQbIikwNz5FTFNaYWhvdn2Ei5KZoKeutbzDytHY3+bt9PsCCRAXHiUsMzpBSE9WXWRrcnmAh46VnKOqsbi/xs3U2+Lp8Pf+BQwTGiEoLzY9REtSWWBnbnV8g4qRmJ+mrbS7wsnQ197l7PP6AQgPFh0kKzI5QEdOVVxjanF4f4aNlJuiqbC3vsXM09rh6O/2/QQLEhkgJy41PENKUVhfZm10e4KJkJeepayzusHIz9bd5Ovy+QAH;AAECAwQFBgcICQoLDA0ODwZkZWZnaGlqa2xtbm9wcXJz;AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+P0BB;17";

TEST(net_join_preauth_from_client_ticket)
{
    auto p = preAuthOfClientTicket(kTicket);
    CHECK(p.has_value());
    CHECK_EQ(int(p->kind), 17);
    CHECK_EQ(p->blob.size(), std::size_t{33});
    CHECK_EQ(int(static_cast<std::uint8_t>(p->blob[16])), 6); // the PreAuthKey id
    CHECK_EQ(p->token.size(), std::size_t{66});
    CHECK_EQ(int(static_cast<std::uint8_t>(p->token[65])), 65);
    CHECK(!preAuthOfClientTicket("CLIENT-TICKET-SECRET").has_value());
    CHECK(!preAuthOfClientTicket("t;s;not base64!;AAEC;17").has_value());
    CHECK(!preAuthOfClientTicket("t;s;AAEC;AAEC;x").has_value());
    CHECK(!preAuthOfClientTicket("t;s;AAEC;AAEC;0").has_value());
    // The test response's ticket has none, so the handshake goes without.
    auto r = parseJoinResponse(kResponse);
    CHECK(r.has_value() && r->joinScript.has_value());
    auto keys = serverKeysOf(*r->joinScript);
    CHECK(keys.has_value() && !keys->preAuth.has_value());
}

TEST(net_join_server_connection_password)
{
    // The six bytes the live 0.741 client hands RakPeer::Connect.
    CHECK_EQ(hexString(std::span(reinterpret_cast<const std::uint8_t*>(kServerConnectionPassword.data()),
                                 kServerConnectionPassword.size())),
             "374f5e116c45");
}

TEST(net_join_request2_with_preauth_matches_live_sizes)
{
    // The 0.741 client's live Request2 (notes-join.md 2026-10-12): AAD 90 =
    // 55 + u8 kind + u8 33 + 33, box 93 = 27 + the 66-byte token.
    auto p = preAuthOfClientTicket(kTicket);
    CHECK(p.has_value());
    OpenRequest2Header h;
    h.keyExchangeVersion = 5;
    h.preAuth = std::pair{p->kind, p->blob};
    OpenRequest2Box box;
    box.clientGuid = 1;
    box.mtu = 1200;
    box.serverAddress = SystemAddress{{128, 116, 5, 33}, 65042};
    box.token = p->token;
    const Key key{};
    const auto bytes = encodeOpenRequest2(h, box, key, 0, 1160);
    CHECK_EQ(int(bytes[18]), 90);
    CHECK_EQ((int(bytes[19]) << 8) | bytes[20], 93);
    CHECK_EQ(bytes.size(), std::size_t{1160});
    auto back = decodeOpenRequest2Header(bytes);
    CHECK(back.has_value() && back->preAuth.has_value());
    CHECK_EQ(int(back->preAuth->first), 17);
    CHECK(back->preAuth->second == p->blob);
    // Without the pre-auth: the 57 / 27 Lunarstone used to send.
    OpenRequest2Header bare;
    bare.keyExchangeVersion = 5;
    box.token.clear();
    const auto old = encodeOpenRequest2(bare, box, key, 0, 1160);
    CHECK_EQ(int(old[18]), 57);
    CHECK_EQ((int(old[19]) << 8) | old[20], 27);
}

TEST(net_join_reply2_live_layout)
{
    // A live server that echoed the Rupp token sends the first aadSize - 21
    // bytes of the box in the clear inside the authenticated header, the
    // rest sealed and zero-padded, nonce and tag last [live 2026-10-12].
    Key key{};
    key[0] = 7;
    OpenReply2Box want;
    want.serverSessionPublic[0] = 0x5a;
    want.serverSessionPublic[31] = 0x3d;
    want.capabilities = 0x2c350b70892;
    want.serverGuid = 0x55813739832cf574;
    want.mtu = 1200;
    want.cipher = 2;
    want.clientAddress = SystemAddress{{0, 0, 0, 0}, 52685};
    // The classic encoding gives the 21-byte header + 58-byte box to split.
    const auto classic = encodeOpenReply2({}, want, key, 0);
    CHECK_EQ(int(classic[18]), 21);
    const std::size_t boxSize = (std::size_t(classic[19]) << 8) | classic[20];
    CHECK_EQ(boxSize, std::size_t{58});
    auto h0 = decodeOpenReply2Header(classic);
    CHECK(h0.has_value());
    auto logical = openOpenReply2BoxBytes(classic, *h0, key);
    CHECK(logical.has_value() && logical->size() == 58);

    std::vector<std::uint8_t> aad(classic.begin(), classic.begin() + 21);
    aad[17] = 1;  // tokenReplied
    aad[18] = 44; // 21 + 23 clear box bytes
    aad.insert(aad.end(), logical->begin(), logical->begin() + 23);
    std::vector<std::uint8_t> sealed(logical->begin() + 23, logical->end());
    sealed.resize(sealed.size() + 28, 0);
    const auto live = sealHandshake(aad, sealed, key, 0);
    CHECK_EQ(live.size(), std::size_t{135}); // the live Reply2 length
    auto h = decodeOpenReply2Header(live);
    CHECK(h.has_value() && h->tokenReplied == 1 && h->aadSize == 44 && h->boxSize == 58);
    auto box = openOpenReply2Box(live, *h, key);
    CHECK(box.has_value());
    CHECK_EQ(box->serverGuid, want.serverGuid);
    CHECK_EQ(box->capabilities, want.capabilities);
    CHECK_EQ(int(box->mtu), 1200);
    CHECK_EQ(int(box->cipher), 2);
    CHECK(box->serverSessionPublic == want.serverSessionPublic);
    CHECK(box->clientAddress == want.clientAddress);
    Key wrong = key;
    wrong[1] = 1;
    CHECK(!openOpenReply2Box(live, *h, wrong).has_value());
}

TEST(net_join_rupp_adopts_reply_token_kind)
{
    RuppGenerator g;
    g.tokenValue.assign(16, 0x11);
    g.server = SystemAddress{{10, 0, 0, 1}, 61987};
    CHECK_EQ(int(g.current(0).tlvs[0].value[0]), 1);
    // The live Reply2's header: 01 00 00 17 | 01 11 02 <16-byte token>.
    std::vector<std::uint8_t> reply{1, 0, 0, 0x17, 1, 0x11, 2};
    reply.insert(reply.end(), 16, 0x66);
    CHECK(g.onReply(reply));
    const RuppHeader h = g.current(0);
    CHECK_EQ(int(h.tlvs[0].value[0]), 2);
    CHECK_EQ(int(h.tlvs[0].value[1]), 0x66);
    CHECK(!g.onReply(reply)); // same token and kind: no change
}

TEST(net_join_masking)
{
    auto r = parseJoinResponse(kResponse);
    CHECK(r.has_value() && r->joinScript.has_value());
    const std::string masked = maskJoinResponse(kResponse);
    CHECK(!masked.contains("SECRET"));
    CHECK(!masked.contains("signature=y"));
    CHECK(!masked.contains("eyJB"));
    CHECK(masked.contains("\"MachineAddress\":\"10.9.6.20\""));
    CHECK(masked.contains("\"ClientTicket\":\"<masked>\""));
    CHECK(masked.contains("\"authenticationTicket\":\"<masked>\""));
    // Still JSON, and the join script still parses.
    auto again = parseJoinResponse(masked);
    CHECK(again.has_value() && again->joinScript.has_value());
    CHECK_EQ(again->joinScript->machineAddress, "10.9.6.20");
    const std::string described = describeJoinScript(*r->joinScript);
    CHECK(!described.contains("SECRET"));
    CHECK(described.contains("128.116.44.33:64318"));
    CHECK(described.contains("key exchange version 5"));
}

TEST(net_join_request_over_fake_web)
{
    FakeServer fake;
    fake.signedInAnswers["https://gamejoin.roblox.com/v1/join-game"] = {200, kResponse};
    assets::RobloxWeb web(fake.transport(), fakeLogin());
    JoinOptions o;
    o.placeId = 1818;
    o.gameJoinAttemptId = "11111111-2222-4333-8444-555555555555";
    auto r = requestJoin(web, o);
    CHECK(r.has_value());
    CHECK_EQ(r->status, 2);
    CHECK_EQ(fake.seen.size(), 1u);
    const auto& req = fake.seen[0];
    CHECK_EQ(req.method, "POST");
    CHECK_EQ(req.body, R"({"placeId":1818,"isTeleport":false,"gameJoinAttemptId":"11111111-2222-4333-8444-555555555555",)"
                       R"("joinOrigin":"ExperienceDetailsPage"})");
    CHECK(hasHeader(req, "User-Agent: " + std::string(lunar::robloxMacUserAgent)));
    CHECK(hasHeader(req, "Content-Type: application/json"));
    CHECK(hasHeader(req, "Cookie: "));
    CHECK(!req.followRedirects);
    fake.checkNoLeaks();
    // No login: refused before any request.
    assets::RobloxWeb anonymous(fake.transport(), std::make_shared<const assets::Credentials>());
    CHECK(!requestJoin(anonymous, o).has_value());
    CHECK_EQ(fake.seen.size(), 1u);
    // An HTTP error names the status, never the body's secrets.
    fake.signedInAnswers["https://gamejoin.roblox.com/v1/join-game"]
        = {429, R"({"errors":[{"code":0,"message":"Too many requests"}]})"};
    auto limited = requestJoin(web, o);
    CHECK(!limited.has_value());
    CHECK(limited.error().contains("429"));
    const std::string id = makeUuid();
    CHECK_EQ(id.size(), 36u);
    CHECK_EQ(id[14], '4');
    CHECK(id != makeUuid());
}

TEST(net_join_rupp_header_codec)
{
    RuppHeader h;
    h.byte0 = 1;
    h.byte1 = 0;
    h.tlvs.push_back({1, {0xaa, 0xbb, 0xcc}});
    h.tlvs.push_back({2, {}});
    h.tlvs.push_back({7, std::vector<std::uint8_t>(16, 0x11)});
    const auto bytes = h.encode();
    CHECK_EQ(bytes.size(), 4u + 5 + 2 + 18);
    CHECK_EQ(hexString(std::span(bytes).first(9)), "0100001d0103aabbcc");
    CHECK_EQ(bytes[2], 0);
    CHECK_EQ(bytes[3], 29); // 4 + 5 + 2 + 18
    auto back = decodeRuppHeader(bytes);
    CHECK(back.has_value());
    CHECK_EQ(back->tlvs.size(), 3u);
    CHECK_EQ(back->tlvs[0].type, 1);
    CHECK_EQ(back->tlvs[0].value.size(), 3u);
    CHECK_EQ(back->tlvs[2].value.size(), 16u);
    CHECK_EQ(back->encode(), bytes);
    // Truncated or inconsistent headers are refused.
    CHECK(!decodeRuppHeader(std::span(bytes).first(20)).has_value());
    std::vector<std::uint8_t> bad = bytes;
    std::fill_n(bad.begin() + 5, 1, 200); // a TLV longer than the header
    CHECK(!decodeRuppHeader(bad).has_value());
}

// A client with version-N keys reaches a server holding that version's
// early-ephemeral pair, in-process, and both sides then exchange a message.
TEST(net_join_version_n_handshake)
{
    const KeyPair serverEarly = KeyPair::generate();
    Peer::Options so;
    so.logLevel = 0;
    so.keyExchangeVersion = 5;
    so.serverEphemeralEarlyPair = serverEarly;
    Peer server(Peer::Role::Server, so);
    if (!server.listen("127.0.0.1", 0))
        SKIP("can't bind a UDP socket here");
    Peer::Options co;
    co.logLevel = 0;
    co.keyExchangeVersion = 5;
    co.serverEphemeralEarlyPublic = serverEarly.publicKey;
    Peer client(Peer::Role::Client, co);
    CHECK(client.connect(server.localAddress()).has_value());
    bool serverGot = false, clientConnected = false;
    for (int i = 0; i < 400 && !serverGot; ++i) {
        client.update(1);
        server.update(1);
        while (auto e = server.poll())
            if (e->kind == Peer::Event::Kind::Message)
                serverGot = e->data == std::vector<std::uint8_t>{0x90, 1, 2, 3};
        while (auto e = client.poll()) {
            if (e->kind == Peer::Event::Kind::Connected) {
                clientConnected = true;
                client.send(e->connection, {0x90, 1, 2, 3});
            }
            CHECK(e->kind != Peer::Event::Kind::Failed);
        }
    }
    CHECK(clientConnected);
    CHECK(serverGot);

    // The wrong version (the local keys) is ignored by that server.
    Peer::Options wrong;
    wrong.logLevel = 0;
    Peer other(Peer::Role::Client, wrong);
    CHECK(other.connect(server.localAddress()).has_value());
    bool otherConnected = false;
    for (int i = 0; i < 60; ++i) {
        other.update(1);
        server.update(1);
        while (auto e = other.poll())
            otherConnected |= e->kind == Peer::Event::Kind::Connected;
    }
    CHECK(!otherConnected);
}

// The Rupp hooks: the client's offline messages carry the header, and a
// server that strips it (as the proxy would) completes the handshake.
TEST(net_join_rupp_hooks_on_the_loop)
{
    Peer::Options so;
    so.logLevel = 0;
    so.ruppReplyLength = [](std::span<const std::uint8_t> d) {
        auto h = decodeRuppHeader(d);
        return h && h->byte0 == 1 ? h->length() : std::size_t{0};
    };
    Peer server(Peer::Role::Server, so);
    if (!server.listen("127.0.0.1", 0))
        SKIP("can't bind a UDP socket here");
    Peer::Options co;
    co.logLevel = 0;
    int headers = 0;
    co.ruppHeader = [&headers] {
        ++headers;
        RuppHeader h;
        h.byte0 = 1;
        h.tlvs.push_back({1, {1, 2, 3, 4}});
        return h.encode();
    };
    co.ruppOnConnected = true;
    Peer client(Peer::Role::Client, co);
    CHECK(client.connect(server.localAddress()).has_value());
    bool serverGot = false;
    for (int i = 0; i < 400 && !serverGot; ++i) {
        client.update(1);
        server.update(1);
        while (auto e = server.poll())
            if (e->kind == Peer::Event::Kind::Message)
                serverGot = e->data == std::vector<std::uint8_t>{0x90, 9};
        while (auto e = client.poll())
            if (e->kind == Peer::Event::Kind::Connected)
                client.send(e->connection, {0x90, 9});
    }
    CHECK(serverGot);
    CHECK(headers > 0);
}

// The client's own Rupp writer (0x102430bb4) run under emulation, from the
// research directory's vectors: our header is the same bytes.
TEST(net_join_rupp_header_matches_emulated_client)
{
    const std::filesystem::path path
        = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors" / "rupp_header.json";
    std::ifstream f(path);
    if (!f)
        SKIP("no research vectors here");
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = assets::json::parse(ss.str());
    CHECK(doc.has_value() && doc->array());
    int checked = 0;
    for (const auto& v : *doc->array()) {
        const std::string* token = v["token"] ? v["token"]->string() : nullptr;
        const std::string* ip = v["ip"] ? v["ip"]->string() : nullptr;
        const std::string* expected = v["header"] ? v["header"]->string() : nullptr;
        const auto kind = v["kind"] ? v["kind"]->integer() : std::nullopt;
        const auto port = v["port"] ? v["port"]->integer() : std::nullopt;
        if (!token || !ip || !expected || !kind || !port)
            continue;
        std::vector<std::uint8_t> tokenBytes;
        for (std::size_t i = 0; i + 1 < token->size(); i += 2)
            tokenBytes.push_back(static_cast<std::uint8_t>(std::stoi(token->substr(i, 2), nullptr, 16)));
        auto address = resolve(std::format("{}:{}", *ip, *port));
        CHECK(address.has_value());
        RuppHeader h;
        h.byte0 = 1;
        RuppTlv t;
        t.type = 1;
        t.value.push_back(static_cast<std::uint8_t>(*kind));
        t.value.insert(t.value.end(), tokenBytes.begin(), tokenBytes.end());
        h.tlvs.push_back(std::move(t));
        RuppTlv a;
        a.type = 2;
        a.value.assign(address->ip.begin(), address->ip.end());
        a.value.push_back(static_cast<std::uint8_t>(address->port >> 8));
        a.value.push_back(static_cast<std::uint8_t>(address->port));
        h.tlvs.push_back(std::move(a));
        CHECK_EQ(hexString(h.encode()), *expected);
        if (*kind == 1) {
            // The generator builds the same from a join script.
            JoinScript s;
            s.machineAddress = *ip;
            s.serverPort = static_cast<std::uint16_t>(*port);
            std::string b64;
            {
                // base64 of the token bytes, the join script's form
                static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::size_t i = 0;
                for (; i + 2 < tokenBytes.size(); i += 3) {
                    const std::uint32_t n = std::uint32_t{tokenBytes[i]} << 16 | std::uint32_t{tokenBytes[i + 1]} << 8 | tokenBytes[i + 2];
                    b64 += alphabet[n >> 18];
                    b64 += alphabet[(n >> 12) & 63];
                    b64 += alphabet[(n >> 6) & 63];
                    b64 += alphabet[n & 63];
                }
                const std::uint32_t n = std::uint32_t{tokenBytes[i]} << 16;
                b64 += alphabet[n >> 18];
                b64 += alphabet[(n >> 12) & 63];
                b64 += "==";
            }
            s.tokenValue = b64;
            auto g = RuppGenerator::fromJoinScript(s);
            CHECK(g != nullptr);
            CHECK_EQ(hexString(g->header(0)), *expected);
        }
        ++checked;
    }
    CHECK(checked >= 3);
}

// The client's 0x90 writer (0x1054b7644) run under emulation: same bytes.
TEST(net_join_request_matches_emulated_client)
{
    const std::filesystem::path path
        = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors" / "join_request.json";
    std::ifstream f(path);
    if (!f)
        SKIP("no research vectors here");
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = assets::json::parse(ss.str());
    CHECK(doc.has_value() && doc->array());
    int checked = 0;
    for (const auto& v : *doc->array()) {
        if ((v["perf_flags"] && v["perf_flags"]->boolean() == true) || (v["luau_debug"] && v["luau_debug"]->boolean() == true))
            continue; // FFlag-gated variants Lunarstone doesn't send
        const std::string* info = v["join_info"] ? v["join_info"]->string() : nullptr;
        const std::string* expected = v["out"] ? v["out"]->string() : nullptr;
        const auto caps = v["caps"] ? v["caps"]->integer() : std::nullopt;
        if (!info || !expected || !caps)
            continue;
        JoinRequest r;
        r.flags = 1;
        r.clientValue = 0x24;
        r.luauMin = 3;
        r.luauMax = 14;
        r.joinInfo = *info;
        if (const auto* strings = v["strings"] ? v["strings"]->array() : nullptr)
            for (const auto& s : *strings)
                r.extra.push_back(*s.string());
        CHECK_EQ(hexString(encodeClientJoinRequest(r, static_cast<std::uint64_t>(*caps), ClientBuild::Roblox742)),
                 *expected);
        ++checked;
    }
    CHECK(checked >= 3);
    // The defaults clientJoinRequest fills, and a join info that carries the player.
    auto resp = parseJoinResponse(kResponse);
    CHECK(resp.has_value() && resp->joinScript.has_value());
    const JoinRequest r = clientJoinRequest(*resp->joinScript);
    CHECK_EQ(r.flags, 1);
    CHECK_EQ(r.clientValue, 0x24u);
    CHECK_EQ(r.luauMin, 3);
    CHECK_EQ(r.luauMax, 14);
    // The genuine 0.741 client's key order: UserId first; no SessionId.
    CHECK(r.joinInfo.starts_with("{\"UserId\":123456,"));
    CHECK(r.joinInfo.contains("\"UserName\":\"tester\""));
    CHECK(r.joinInfo.contains(std::format("\"ClientRobloxVersion\":\"{}\"", lunar::robloxVersion)));
    auto back = assets::json::parse(r.joinInfo);
    CHECK(back.has_value() && !(*back)["SessionId"] && (*back)["AgeChecked"] && (*back)["AgeChecked"]->string());
    CHECK(r.joinInfo.contains("\"OsPlatform\":\"OSX\",\"OsPlatformName\":\"MacOS\""));
    // Saying Linux: the join info's platform fields and User-Agent.
    auto linuxPlatform = clientPlatform("linux");
    CHECK(linuxPlatform.has_value() && linuxPlatform->name == "Linux" && linuxPlatform->displayName == "Linux");
    CHECK(!clientPlatform("dos"));
    ClientInfo onLinux;
    onLinux.platform = *linuxPlatform;
    const JoinRequest l = clientJoinRequest(*resp->joinScript, onLinux);
    CHECK(l.joinInfo.contains("\"OsPlatform\":\"Linux\",\"OsPlatformName\":\"Linux\""));
    CHECK(l.joinInfo.contains("\"UserAgentStr\":\"Roblox/LinuxRobloxApp/"));
    // 0x92: zigzag64 of (A << 32 | (A ^ B)).
    const auto ver = encodeClientVerification(0x11223344);
    CHECK_EQ(ver[0], 0x92);
    CHECK_EQ(hexString(encodePreferredSpawn("")), "8f00");
    CHECK_EQ(hexString(encodePreferredSpawn("Spawn")), "8f055370" "61776e");
}

// 0x8a as the client's writer produced it under emulation (emu/t_ticket.py).
TEST(net_join_ticket_submission_matches_emulated_client)
{
    const std::string_view ticket = "TICKET";
    CHECK_EQ(xxhash32(std::span(reinterpret_cast<const std::uint8_t*>(ticket.data()), ticket.size()), 1), 0x75b26fb7u);
    CHECK_EQ(ticketHashMix(0x75b26fb7u, ClientBuild::Roblox742), 0xc0350890u);
    const std::string expected
        = "8af2c001065449434b4554240000004a"
          "3533303631303632373539383137336364626361303461636262396532366336303335386334653721613339646339333561623966"
          "613963626330336261643464326261376635363 42e"
          "034f5358013f9091d4810c83b19ca40b0000feca01c0";
    std::string e;
    for (char c : expected)
        if (c != ' ')
            e += c;
    CHECK_EQ(hexString(encodeTicketSubmission(12345, ticket, "", "OSX", ClientBuild::Roblox742)), e);
    std::string e2 = e.substr(0, e.size() - 10) + "03616263" + "feca01c0";
    CHECK_EQ(hexString(encodeTicketSubmission(12345, ticket, "abc", "OSX", ClientBuild::Roblox742)), e2);
    // The platform string ("OSX" above, 03 4f5358) is the build's Enum.Platform name.
    const std::string onLinux = hexString(encodeTicketSubmission(12345, ticket, "", "Linux"));
    CHECK(onLinux.contains("054c696e7578013f"));
    CHECK(!onLinux.contains("034f5358"));
}

// The client's RbxOpenRequest2 builder (0x102d765f2) run under emulation
// with known keys: Rupp header, AAD, box and padding are the same bytes.
TEST(net_join_request2_matches_emulated_client)
{
    const std::filesystem::path path
        = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors" / "open_request2.json";
    std::ifstream f(path);
    if (!f)
        SKIP("no research vectors here");
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = assets::json::parse(ss.str());
    CHECK(doc.has_value() && doc->array());
    auto unhex = [](const std::string& h) {
        std::vector<std::uint8_t> out;
        for (std::size_t i = 0; i + 1 < h.size(); i += 2)
            out.push_back(static_cast<std::uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
        return out;
    };
    auto key = [&](const std::string& h) {
        Key k{};
        const auto v = unhex(h);
        std::copy_n(v.begin(), 32, k.begin());
        return k;
    };
    int checked = 0;
    for (const auto& v : *doc->array()) {
        auto str = [&](const char* k) { return v[k] && v[k]->string() ? *v[k]->string() : std::string(); };
        auto num = [&](const char* k) { return v[k] && v[k]->integer() ? *v[k]->integer() : 0; };
        const KeyPair client = KeyPair::fromSecret(key(str("client_secret")));
        CHECK_EQ(hexString(client.publicKey), str("client_public"));
        const SessionKeys hs = kxClient(client, key(str("server_public")));
        CHECK_EQ(hexString(hs.tx), str("tx"));
        CHECK_EQ(hexString(hs.rx), str("rx"));
        OpenRequest2Header h;
        h.keyExchangeVersion = static_cast<std::uint16_t>(num("version"));
        h.clientPublic = client.publicKey;
        OpenRequest2Box box;
        box.capabilities = std::stoull(str("capabilities"), nullptr, 16);
        box.clientGuid = std::stoull(str("guid"), nullptr, 16);
        box.mtu = static_cast<std::uint16_t>(num("mtu"));
        box.cipher = static_cast<std::uint8_t>(num("cipher"));
        box.serverAddress = *resolve(std::format("{}:{}", str("server_ip"), num("server_port")));
        RuppGenerator rupp;
        rupp.tokenValue = unhex(str("rupp_token"));
        rupp.server = *resolve(std::format("{}:{}", str("rupp_ip"), num("rupp_port")));
        std::vector<std::uint8_t> datagram = rupp.header(0);
        const auto request = encodeOpenRequest2(h, box, hs.tx, static_cast<std::uint64_t>(num("nonce_counter")),
                                                static_cast<std::size_t>(num("mtu")) - kMtuOverhead - datagram.size());
        datagram.insert(datagram.end(), request.begin(), request.end());
        CHECK_EQ(hexString(datagram), str("datagram"));
        if (num("version") == 5)
            CHECK_EQ(box.capabilities, kBaseCapabilities); // what the client sends with every FFlag off
        ++checked;
    }
    CHECK(checked >= 5);
}

// PlayerHydrationService's answer, shaped like the live one, and where its
// values land in the join info.
TEST(net_join_player_attributes)
{
    const auto a = parsePlayerAttributes(
        R"({"playerInfo":{"lastPerformed":"1791791414000","userId":"123456","ageBracket":"AgeUnchecked",)"
        R"("gender":"Female","platform":"Computer","os":"OSX","isOriginalUser":false,)"
        R"("originalAccountCreationTimestampMs":"1776000000000"},"signature":"c2ln"})");
    CHECK(a.has_value());
    CHECK_EQ(a->gender, "Female");
    CHECK_EQ(a->os, "OSX");
    CHECK(!a->isOriginalUser);
    CHECK_EQ(a->originalAccountCreationTimestampMs, 1776000000000);
    CHECK(!parsePlayerAttributes(R"({"signature":"c2ln"})").has_value());

    auto resp = parseJoinResponse(kResponse);
    CHECK(resp.has_value() && resp->joinScript.has_value());
    JoinScript script = *resp->joinScript;
    script.playerAttributes = *a;
    const std::string info = buildJoinInfo(script);
    CHECK(info.contains("\"Gender\":\"Female\""));
    CHECK(info.contains("\"IsOriginalUser\":false"));
    CHECK(info.contains("\"OriginalAccountCreationTimestampMs\":1776000000000"));
    CHECK_EQ(playerAttributesRequest("UA").url, "https://apis.roblox.com/player-hydration-service/v1/players/signed");
}

// The 0x90 body cipher (RakNet's DataBlockEncryptor with the join request
// key). The vector comes from a reference implementation that opens a
// genuine 0.741 0x90 from a capture, with fixed bytes for the random ones.
TEST(net_join_request_block_cipher)
{
    auto unhex = [](std::string_view h) {
        std::vector<std::uint8_t> out;
        for (std::size_t i = 0; i + 1 < h.size(); i += 2)
            out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
        return out;
    };
    const auto plain = unhex("0124000000030e0000027b7d4c756e617273746f6e65206a6f696e2072657175657374207465737420766563746f7221");
    const auto sealed = unhex("6f007b93315dfc2459dd56062154aeef807c8d32d42bd1e58785f9c042c25ae8c77dc1c5cfe3aed9d476b96791e552d1"
                              "76fc6df16b6027691664db5b40d3153b");
    const auto opened = blockDecrypt(kJoinRequestKey, sealed);
    CHECK(opened.has_value());
    CHECK(*opened == plain);
    // A flipped bit fails the checksum.
    auto bad = sealed;
    bad[40] ^= 1;
    CHECK(!blockDecrypt(kJoinRequestKey, bad).has_value());
    // Our own sealing opens again, at every padding length.
    for (std::size_t n = 0; n < 40; ++n) {
        std::vector<std::uint8_t> body(n, static_cast<std::uint8_t>(n));
        const auto s = blockEncrypt(kJoinRequestKey, body);
        CHECK_EQ(s.size() % 16, 0u);
        const auto back = blockDecrypt(kJoinRequestKey, s);
        CHECK(back.has_value() && *back == body);
    }
    const std::vector<std::uint8_t> msg{0x90, 1, 2, 3};
    const auto m = sealMessageBody(kJoinRequestKey, msg);
    CHECK_EQ(m[0], 0x90);
    CHECK(blockDecrypt(kJoinRequestKey, std::span(m).subspan(1)) == std::vector<std::uint8_t>({1, 2, 3}));
}

// 0x8a's key: the place id and this build's constants as decimal text.
TEST(net_join_ticket_submission_key)
{
    const auto text = [](const BlockKey& k) { return std::string(k.begin(), k.end()); };
    CHECK_EQ(text(ticketSubmissionKey(1818, ClientBuild::Roblox742)), "1818734047139-17");
    // int32 of the place id, as the client's %d reads it.
    CHECK_EQ(text(ticketSubmissionKey(107535308163741)).substr(0, 11), "-2082993507");
}

// Spelunk's 0.745 Mac player disassembly: writer 0x105515752,
// verification 0x1057ed6df, ticket formatter 0x1057ed9f4 and mix 0x105516501.
TEST(net_join_current_build_wire_fields)
{
    JoinRequest request;
    request.joinInfo = "{}";
    const auto joined = encodeClientJoinRequest(request, kBaseCapabilities);
    CHECK_EQ(hexString(std::span(joined).last(24)),
             "5e1fcb85987b90bf9ea7b18a5e1fcb851660491178feefcb");
    CHECK_EQ(joined.size(), encodeJoinRequest(request, kBaseCapabilities).size() + 24);

    const auto verified = encodeClientVerification(0x11223344);
    NetworkStream verification(verified);
    CHECK_EQ(verification.readU8(), 0x92);
    CHECK_EQ(verification.readZigzag64(), 0x112233441f35f4ccll);
    CHECK(!verification.bad() && verification.left() == 0);

    const auto submitted = encodeTicketSubmission(12345, "TICKET", "session", "Linux");
    NetworkStream ticket(submitted);
    CHECK_EQ(ticket.readU8(), 0x8a);
    CHECK_EQ(ticket.readZigzag64(), 12345);
    CHECK_EQ(ticket.readString(), "TICKET");
    CHECK_EQ(ticket.readU32(), 0x24u);
    CHECK_EQ(ticket.readString(),
             "2c2d778eadb4263fc76bf47d2f3f6717eb2afe41!5e989e8c1f7ba753cb90b13385bf8a61.");
    CHECK_EQ(ticket.readString(), "Linux");
    CHECK_EQ(ticket.readString(), "?");
    CHECK_EQ(ticket.readVarint(), 0xd37471e6u);
    CHECK_EQ(ticket.readVarint(), 0xc7c681d9u);
    CHECK_EQ(ticket.readString(), "");
    CHECK_EQ(ticket.readString(), "session");
    CHECK_EQ(ticket.readU32(), 0xc001cafeu);
    CHECK(!ticket.bad() && ticket.left() == 0);

    const auto key = ticketSubmissionKey(1818);
    CHECK_EQ(std::string(key.begin(), key.end()), "1818-2050285730-");
}

// A teleport: the ticket out of Player.OnTeleportInternal's table, the
// reserved-server join it makes and what the new server's join info says.
TEST(net_join_teleport_ticket_request_and_join_info)
{
    auto member = [](std::string key, lunar::Value value) {
        return std::pair{std::move(key), remoteValue(std::move(value))};
    };
    auto table = std::make_shared<RemoteTable>();
    table->dictionary = true;
    table->members = {member("joinData", std::string(R"({"userId":1})")),
                      member("auth", std::string(R"({"mac":"bWFj","ver":5})")),
                      member("reservedServerAccessCode", std::string("code-1")),
                      member("teleportToken", std::string("token-1")),
                      member("teleportType", 2.0),
                      member("UniverseId", 123456002.0),
                      member("placeId", 123456003.0)};
    const RemoteValue value{lunar::Value(), table};
    const auto ticket = teleportTicket(value);
    CHECK(ticket.has_value());
    CHECK_EQ(ticket->placeId, 123456003u);
    CHECK_EQ(ticket->accessCode(), "code-1");
    CHECK_EQ(ticket->universeId, 123456002u);

    JoinOptions options;
    options.teleport = &*ticket;
    options.gameJoinAttemptId = "attempt";
    const auto request = joinRequest(options);
    CHECK_EQ(request.url, "https://gamejoin.roblox.com/v1/join-reserved-game");
    CHECK_EQ(request.body, R"({"placeId":123456003,"isTeleport":true,"accessCode":"code-1",)"
                           R"("isImmersiveAdsTeleport":false,"teleportToken":"token-1","gameJoinAttemptId":"attempt"})");

    const std::string raw = teleportRawJoinData(*ticket, 1234);
    CHECK_EQ(raw, R"({"auth":"{\"mac\":\"bWFj\",\"ver\":5}","joinData":"{\"userId\":1}",)"
                  R"("teleportClientStartMsSinceEpoch":1234})");
    auto resp = parseJoinResponse(kResponse);
    CHECK(resp.has_value() && resp->joinScript.has_value());
    ClientInfo client;
    client.teleportedIn = true;
    client.rawJoinData = raw;
    client.previousUniverseId = ticket->universeId;
    const std::string info = buildJoinInfo(*resp->joinScript, client);
    CHECK(info.contains("\"IsTeleportedIn\":true"));
    CHECK(info.contains("\"PreviousUniverseId\":123456002"));
    CHECK(info.contains(R"("RawJoinData":"{\"auth\":)"));

    // A table that names no place isn't a ticket.
    auto empty = std::make_shared<RemoteTable>();
    empty->dictionary = true;
    CHECK(!teleportTicket(RemoteValue{lunar::Value(), empty}).has_value());
}

TEST(net_join_private_server_links)
{
    // The old link names the place and the code; a share link has to be resolved.
    auto old = lunar::net::parseServerLink(
        "https://www.roblox.com/games/123456004/Sample-Experience?privateServerLinkCode=1234567890");
    CHECK(old.has_value());
    CHECK_EQ(old->placeId, std::uint64_t{123456004});
    CHECK_EQ(old->linkCode, std::string("1234567890"));
    CHECK(old->shareCode.empty());
    auto share = lunar::net::parseServerLink("https://www.roblox.com/share?code=ab12cd34ef&type=Server");
    CHECK(share.has_value());
    CHECK_EQ(share->shareCode, std::string("ab12cd34ef"));
    CHECK(!lunar::net::parseServerLink("https://www.roblox.com/share?code=ab12&type=ExperienceDetails").has_value());
    CHECK(!lunar::net::parseServerLink("https://www.roblox.com/games/123456004").has_value());
    CHECK(!lunar::net::parseServerLink("https://www.roblox.com/share?code=a\"b&type=Server").has_value());

    // Joined with join-private-game and the code.
    lunar::net::JoinOptions options;
    options.placeId = 123456004;
    options.linkCode = "1234567890";
    options.gameJoinAttemptId = "attempt";
    const auto request = lunar::net::joinRequest(options);
    CHECK_EQ(request.url, std::string("https://gamejoin.roblox.com/v1/join-private-game"));
    CHECK_EQ(request.body,
             std::string(R"({"placeId":123456004,"isTeleport":false,"linkCode":"1234567890","gameJoinAttemptId":"attempt"})"));
}
