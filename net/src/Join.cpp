#include <lunar/net/Join.hpp>

#include <lunar/assets/Json.hpp>
#include <lunar/net/Describe.hpp>

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <random>

namespace lunar::net {

namespace {

using assets::json::Value;

// -- a scanner over JSON text, to keep members byte for byte

std::size_t skipSpace(std::string_view t, std::size_t p)
{
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r'))
        ++p;
    return p;
}

// The end (one past) of the string starting at the quote at `p`; npos when unterminated.
std::size_t skipString(std::string_view t, std::size_t p)
{
    if (p >= t.size() || t[p] != '"')
        return std::string_view::npos;
    for (++p; p < t.size(); ++p) {
        if (t[p] == '\\')
            ++p;
        else if (t[p] == '"')
            return p + 1;
    }
    return std::string_view::npos;
}

// The end of the value starting at `p` (any kind); npos when malformed.
std::size_t skipValue(std::string_view t, std::size_t p)
{
    p = skipSpace(t, p);
    if (p >= t.size())
        return std::string_view::npos;
    if (t[p] == '"')
        return skipString(t, p);
    if (t[p] == '{' || t[p] == '[') {
        int depth = 0;
        for (; p < t.size(); ++p) {
            if (t[p] == '"') {
                p = skipString(t, p);
                if (p == std::string_view::npos)
                    return p;
                --p;
            } else if (t[p] == '{' || t[p] == '[') {
                ++depth;
            } else if (t[p] == '}' || t[p] == ']') {
                if (--depth == 0)
                    return p + 1;
            }
        }
        return std::string_view::npos;
    }
    while (p < t.size() && t[p] != ',' && t[p] != '}' && t[p] != ']' && t[p] != ' ' && t[p] != '\n'
           && t[p] != '\r' && t[p] != '\t')
        ++p;
    return p;
}

// Calls fn(keyText, valueStart, valueEnd) for every member of the object at
// the start of `t`; stops when fn returns false.
template <class Fn>
void forEachMember(std::string_view t, Fn&& fn)
{
    std::size_t p = skipSpace(t, 0);
    if (p >= t.size() || t[p] != '{')
        return;
    ++p;
    for (;;) {
        p = skipSpace(t, p);
        if (p >= t.size() || t[p] == '}')
            return;
        if (t[p] == ',') {
            ++p;
            continue;
        }
        const std::size_t keyEnd = skipString(t, p);
        if (keyEnd == std::string_view::npos)
            return;
        const std::string_view key = t.substr(p + 1, keyEnd - p - 2);
        p = skipSpace(t, keyEnd);
        if (p >= t.size() || t[p] != ':')
            return;
        const std::size_t valueStart = skipSpace(t, p + 1);
        const std::size_t valueEnd = skipValue(t, valueStart);
        if (valueEnd == std::string_view::npos)
            return;
        if (!fn(key, valueStart, valueEnd))
            return;
        p = valueEnd;
    }
}

std::string str(const Value* v)
{
    const std::string* s = v ? v->string() : nullptr;
    return s ? *s : std::string();
}
std::int64_t integer(const Value* v)
{
    return v ? v->integer().value_or(0) : 0;
}
bool boolean(const Value* v)
{
    return v && v->boolean().value_or(false);
}

std::string urlDecode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            unsigned v = 0;
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(text[i + 1]), lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                v = static_cast<unsigned>(hi * 16 + lo);
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

} // namespace

std::string joinStatusName(int status)
{
    // The client's table at 0x105237294 [code].
    switch (status) {
    case 0:
        return "Waiting for an available server";
    case 1:
        return "Server found, loading...";
    case 2:
        return "Joining server";
    case 3:
        return "This experience is disabled";
    case 4:
        return "Cannot find experience server";
    case 5:
        return "This experience has ended, or the server became unavailable";
    case 6:
        return "Requested experience is full";
    case 10:
        return "Followed user has left the experience";
    case 11:
    case 27:
    case 28:
        return "This experience is restricted";
    case 12:
        return "Not authorized to join this experience";
    case 13:
        return "Server is busy";
    case 14:
        return "Hash Expired";
    case 15:
        return "Hash Exception";
    case 16:
        return "Your party is too large to fit";
    case 17:
        return "A Http error has occurred";
    case 21:
        return "Not authorized to join this experience due to privacy setting";
    case 22:
        return "Requested experience is full. Waiting in queue...";
    case 23:
        return "You were banned from this experience";
    case 29:
        return "You can join once the host is in this private server";
    case 30:
        return "Complete an age check to see if you can play this game";
    case 31:
        return "Ask your parent to unlock this game for you";
    case 32:
        return "Based on your age, you cannot join this game";
    default:
        return std::format("Unknown Status ({})", status);
    }
}

bool joinStatusRetryable(int status)
{
    return status == joinstatus::Waiting || status == joinstatus::Loading || status == joinstatus::GameFull
           || status == joinstatus::Queued;
}

double joinRetrySeconds(int status, std::optional<int> queuePosition, int attempt)
{
    if (status == joinstatus::Queued && queuePosition && *queuePosition >= 1)
        return *queuePosition == 1 ? 2.0 : *queuePosition <= 5 ? 5.0 : 10.0;
    // (1 + 3/10)^n - 1 + 1, clamped to [1, 10] (AppBridgePlaceLaunch* defaults 3 / 1 / 10).
    double delay = 1.0;
    for (int i = 0; i < attempt; ++i)
        delay *= 1.3;
    return std::clamp(delay, 1.0, 10.0);
}

std::string_view rawJsonMember(std::string_view object, std::string_view key)
{
    std::string_view out;
    forEachMember(object, [&](std::string_view k, std::size_t start, std::size_t end) {
        if (k != key)
            return true;
        out = object.substr(start, end - start);
        return false;
    });
    return out;
}

std::expected<JoinResponse, std::string> parseJoinResponse(std::string_view body)
{
    auto doc = assets::json::parse(body);
    if (!doc || !doc->object())
        return std::unexpected("the join response isn't a JSON object");
    const Value& d = *doc;
    JoinResponse r;
    r.jobId = str(d["jobId"]);
    const auto status = d["status"] ? d["status"]->integer() : std::nullopt;
    if (!status)
        return std::unexpected("the join response has no status");
    r.status = static_cast<int>(*status);
    r.message = str(d["message"]);
    r.joinScriptUrl = str(d["joinScriptUrl"]);
    r.authenticationTicket = str(d["authenticationTicket"]);
    if (const Value* q = d["queuePosition"]; q && q->integer())
        r.queuePosition = static_cast<int>(*q->integer());
    const Value* js = d["joinScript"];
    if (js && js->object()) {
        JoinScript s;
        s.raw = std::string(rawJsonMember(body, "joinScript"));
        s.doc = std::make_shared<const Value>(*js);
        const Value& j = *js;
        s.machineAddress = str(j["MachineAddress"]);
        s.serverPort = static_cast<std::uint16_t>(integer(j["ServerPort"]));
        if (const Value* ends = j["UdmuxEndpoints"]; ends && ends->array())
            for (const Value& e : *ends->array())
                s.udmuxEndpoints.push_back(
                    {str(e["Address"]), static_cast<std::uint16_t>(integer(e["Port"]))});
        s.directServerReturn = boolean(j["DirectServerReturn"]);
        s.tokenGenAlgorithm = static_cast<int>(integer(j["TokenGenAlgorithm"]));
        s.pepperId = integer(j["PepperId"]);
        s.tokenValue = str(j["TokenValue"]);
        s.userName = str(j["UserName"]);
        s.displayName = str(j["DisplayName"]);
        s.userId = integer(j["UserId"]);
        s.placeId = integer(j["PlaceId"]);
        s.universeId = integer(j["UniverseId"]);
        s.placeVersion = integer(j["PlaceVersion"]);
        s.dataCenterId = integer(j["DataCenterId"]);
        s.accountAge = integer(j["AccountAge"]);
        s.gameId = str(j["GameId"]);
        s.membershipType = str(j["MembershipType"]);
        s.countryCode = str(j["CountryCode"]);
        s.rccVersion = str(j["RccVersion"]);
        s.characterAppearance = str(j["CharacterAppearance"]);
        s.clientPublicKeyData = str(j["ClientPublicKeyData"]);
        s.ephemeralEarlyPubKey = str(j["EphemeralEarlyPubKey"]);
        s.sessionId = str(j["SessionId"]);
        s.clientTicket = str(j["ClientTicket"]);
        if (const Value* t = d["joinTicket"]; t && std::holds_alternative<std::string>(t->data))
            s.joinTicket = str(t);
        else if (t && !std::holds_alternative<std::monostate>(t->data))
            s.joinTicket = std::string(rawJsonMember(body, "joinTicket"));
        r.joinScript = std::move(s);
    }
    return r;
}

std::string makeUuid()
{
    std::random_device rd;
    std::array<std::uint8_t, 16> b{};
    for (std::size_t i = 0; i < b.size(); i += 4) {
        const std::uint32_t v = rd();
        b[i] = static_cast<std::uint8_t>(v);
        b[i + 1] = static_cast<std::uint8_t>(v >> 8);
        b[i + 2] = static_cast<std::uint8_t>(v >> 16);
        b[i + 3] = static_cast<std::uint8_t>(v >> 24);
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3f) | 0x80);
    std::string out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += std::format("{:02x}", b[i]);
    }
    return out;
}

std::string TeleportTicket::accessCode() const
{
    return !reservedServerId.empty() ? reservedServerId
           : !vipServerId.empty()    ? vipServerId
                                     : reservedServerAccessCode;
}

std::optional<TeleportTicket> teleportTicket(const RemoteValue& table)
{
    if (!table.table || !table.table->dictionary)
        return std::nullopt;
    TeleportTicket t;
    for (const auto& [key, member] : table.table->members) {
        const lunar::Value& v = member.value;
        const auto* text = std::get_if<std::string>(&v);
        const auto* number = std::get_if<double>(&v);
        auto setText = [&](std::string& out) {
            if (text)
                out = *text;
        };
        if (key == "placeId" && number)
            t.placeId = static_cast<std::uint64_t>(*number);
        else if (key == "UniverseId" && number)
            t.universeId = static_cast<std::uint64_t>(*number);
        else if (key == "teleportType" && number)
            t.teleportType = static_cast<int>(*number);
        else if (key == "reservedServerId")
            setText(t.reservedServerId);
        else if (key == "vipServerId")
            setText(t.vipServerId);
        else if (key == "reservedServerAccessCode")
            setText(t.reservedServerAccessCode);
        else if (key == "instanceId")
            setText(t.instanceId);
        else if (key == "teleportToken")
            setText(t.teleportToken);
        else if (key == "joinData")
            setText(t.joinData);
        else if (key == "auth")
            setText(t.auth);
        else if (key == "partyData")
            setText(t.partyData);
        else if (key == "spawnName")
            setText(t.spawnName);
    }
    if (t.placeId == 0)
        return std::nullopt;
    return t;
}

std::string teleportRawJoinData(const TeleportTicket& ticket, std::int64_t startMs)
{
    auto quoted = [](std::string_view text) { return writeJson(Value{std::string(text)}); };
    std::string out = "{";
    auto add = [&](std::string_view key, const std::string& encoded) {
        out += (out.size() == 1 ? "" : ",") + quoted(key) + ":" + encoded;
    };
    if (!ticket.auth.empty())
        add("auth", quoted(ticket.auth));
    if (!ticket.joinData.empty())
        add("joinData", quoted(ticket.joinData));
    if (!ticket.partyData.empty())
        add("partyData", quoted(ticket.partyData));
    add("teleportClientStartMsSinceEpoch", std::to_string(startMs));
    return out + "}";
}

assets::HttpRequest joinRequest(const JoinOptions& options)
{
    assets::HttpRequest req;
    req.method = "POST";
    req.url = "https://gamejoin.roblox.com/v1/join-game";
    // What the client's HttpRequest ctor (0x1027191ea) and HttpClient::send
    // (0x1026f27bc) add [code]; the Cookie comes from RobloxWeb::withCookie.
    req.headers = {"Content-Type: application/json", "User-Agent: " + options.userAgent, "Requester: Client",
                   "Cache-Control: no-cache"};
    if (const TeleportTicket* t = options.teleport) {
        // The teleport joins' bodies, keys in the client's order (0x105243af8, 0x1052432dc).
        auto quoted = [](std::string_view text) { return writeJson(Value{std::string(text)}); };
        const std::string attempt = options.gameJoinAttemptId.empty() ? makeUuid() : options.gameJoinAttemptId;
        const std::string token = t->teleportToken.empty() ? "" : ",\"teleportToken\":" + quoted(t->teleportToken);
        if (!t->accessCode().empty()) {
            req.url = "https://gamejoin.roblox.com/v1/join-reserved-game";
            req.body = std::format(R"({{"placeId":{},"isTeleport":true,"accessCode":{},"isImmersiveAdsTeleport":false{},)"
                                   R"("gameJoinAttemptId":"{}"}})",
                                   t->placeId, quoted(t->accessCode()), token, attempt);
        } else if (!t->instanceId.empty()) {
            req.url = "https://gamejoin.roblox.com/v1/join-game-instance";
            req.body = std::format(R"({{"placeId":{},"isTeleport":true,"gameId":{},"isImmersiveAdsTeleport":false{},)"
                                   R"("gameJoinAttemptId":"{}"}})",
                                   t->placeId, quoted(t->instanceId), token, attempt);
        } else {
            req.body = std::format(R"({{"placeId":{},"isTeleport":true{},"gameJoinAttemptId":"{}"}})", t->placeId,
                                   token, attempt);
        }
        return req;
    }
    if (!options.linkCode.empty()) {
        auto quoted = [](std::string_view text) { return writeJson(Value{std::string(text)}); };
        req.url = "https://gamejoin.roblox.com/v1/join-private-game";
        req.body = std::format(R"({{"placeId":{},"isTeleport":false,"linkCode":{},"gameJoinAttemptId":"{}"}})",
                               options.placeId, quoted(options.linkCode),
                               options.gameJoinAttemptId.empty() ? makeUuid() : options.gameJoinAttemptId);
        return req;
    }
    if (!options.gameId.empty()) {
        auto quoted = [](std::string_view text) { return writeJson(Value{std::string(text)}); };
        req.url = "https://gamejoin.roblox.com/v1/join-game-instance";
        req.body = std::format(R"({{"placeId":{},"isTeleport":false,"gameId":{},"gameJoinAttemptId":"{}"}})",
                               options.placeId, quoted(options.gameId),
                               options.gameJoinAttemptId.empty() ? makeUuid() : options.gameJoinAttemptId);
        return req;
    }
    req.body = std::format(R"({{"placeId":{},"isTeleport":{},"gameJoinAttemptId":"{}"{}}})", options.placeId,
                           options.isTeleport ? "true" : "false",
                           options.gameJoinAttemptId.empty() ? makeUuid() : options.gameJoinAttemptId,
                           options.joinOrigin.empty() ? "" : std::format(R"(,"joinOrigin":"{}")", options.joinOrigin));
    return req;
}

namespace {

// A query parameter's value in a URL; empty when it has none.
std::string queryValue(std::string_view url, std::string_view name)
{
    for (std::size_t at = url.find('?'); at != std::string_view::npos && at < url.size();) {
        const std::size_t start = at + 1;
        const std::size_t end = std::min(url.find('&', start), url.size());
        const std::string_view pair = url.substr(start, end - start);
        if (pair.size() > name.size() && pair.starts_with(name) && pair[name.size()] == '=')
            return std::string(pair.substr(name.size() + 1));
        at = end;
    }
    return {};
}

bool linkCodeChars(std::string_view code)
{
    return !code.empty() && code.size() <= 128
           && std::ranges::all_of(code, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

} // namespace

std::optional<ServerLink> parseServerLink(std::string_view link)
{
    ServerLink out;
    if (const std::string share = queryValue(link, "code"); !share.empty() && link.find("/share") != std::string_view::npos) {
        if (queryValue(link, "type") != "Server" || !linkCodeChars(share))
            return std::nullopt;
        out.shareCode = share;
        return out;
    }
    out.linkCode = queryValue(link, "privateServerLinkCode");
    const std::size_t games = link.find("/games/");
    if (out.linkCode.empty() || !linkCodeChars(out.linkCode) || games == std::string_view::npos)
        return std::nullopt;
    const std::string_view digits = link.substr(games + 7);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), out.placeId);
    if (error != std::errc{} || out.placeId == 0)
        return std::nullopt;
    return out;
}

std::expected<ServerLink, std::string> resolveServerLink(const assets::RobloxWeb& web, std::string_view link)
{
    auto parsed = parseServerLink(link);
    if (!parsed)
        return std::unexpected("that isn't a private server link (roblox.com/share?code=...&type=Server or "
                               "roblox.com/games/<id>/...?privateServerLinkCode=...)");
    if (parsed->shareCode.empty())
        return *parsed;
    assets::HttpRequest request{
        .method = "POST",
        .url = "https://apis.roblox.com/sharelinks/v1/resolve-link",
        .headers = {"Content-Type: application/json", "Accept: application/json"},
        .body = std::format(R"({{"linkId":{},"linkType":"Server"}})", writeJson(Value{parsed->shareCode})),
    };
    // A signed-in POST wants the CSRF token Roblox hands out in a 403.
    auto reply = web.withCookie(request);
    if (reply && reply->status == 403)
        if (const std::string* token = reply->header("x-csrf-token"); token && !token->empty()) {
            request.headers.push_back(std::format("X-CSRF-TOKEN: {}", *token));
            reply = web.withCookie(request);
        }
    if (!reply)
        return std::unexpected("resolve-link: " + reply.error());
    if (reply->status != 200)
        return std::unexpected(std::format("resolve-link: HTTP {}", reply->status));
    const auto doc = assets::json::parse(reply->body);
    const Value* invite = doc ? (*doc)["privateServerInviteData"] : nullptr;
    if (!invite || !invite->object())
        return std::unexpected("resolve-link: the answer names no private server");
    if (const std::string status = str((*invite)["status"]); !status.empty() && status != "Valid")
        return std::unexpected(std::format("resolve-link: the link is {}", status));
    ServerLink out;
    out.placeId = static_cast<std::uint64_t>((*invite)["placeId"] ? (*invite)["placeId"]->integer().value_or(0) : 0);
    out.linkCode = str((*invite)["linkCode"]);
    if (out.placeId == 0 || out.linkCode.empty())
        return std::unexpected("resolve-link: the answer has no place or link code");
    return out;
}

std::expected<JoinResponse, std::string> requestJoin(const assets::RobloxWeb& web, const JoinOptions& options)
{
    auto response = web.withCookie(joinRequest(options));
    if (!response)
        return std::unexpected("join-game: " + response.error());
    if (response->status != 200)
        return std::unexpected("join-game: " + assets::describeHttpFailure(*response));
    if (const char* dump = std::getenv("LUNAR_NET_JOIN_DUMP"); dump && *dump) {
        // research: the whole response, secrets masked
        std::ofstream f(dump, std::ios::binary | std::ios::trunc);
        f << maskJoinResponse(response->body) << '\n';
    }
    auto parsed = parseJoinResponse(response->body);
    if (!parsed)
        return std::unexpected("join-game: " + parsed.error());
    return parsed;
}

std::optional<ClientPlatform> clientPlatform(std::string_view name)
{
    if (name == "mac")
        return ClientPlatform{};
    if (name == "linux")
        return ClientPlatform{"Linux", "Linux", robloxLinuxUserAgent};
    return std::nullopt;
}

assets::HttpRequest playerAttributesRequest(std::string_view userAgent)
{
    assets::HttpRequest req;
    req.url = "https://apis.roblox.com/player-hydration-service/v1/players/signed";
    // Without a Content-Type the API answers 415 [live].
    req.headers = {"Content-Type: application/json", "User-Agent: " + std::string(userAgent), "Requester: Client"};
    return req;
}

std::expected<PlayerAttributes, std::string> parsePlayerAttributes(std::string_view body)
{
    auto doc = assets::json::parse(body);
    if (!doc || !doc->object())
        return std::unexpected("the player attributes aren't a JSON object");
    const Value* info = (*doc)["playerInfo"];
    if (!info || !info->object())
        return std::unexpected("the player attributes have no playerInfo");
    const Value& i = *info;
    PlayerAttributes a;
    a.ageBracket = str(i["ageBracket"]);
    a.gender = str(i["gender"]);
    a.platform = str(i["platform"]);
    a.os = str(i["os"]);
    if (const Value* v = i["isOriginalUser"]; v && std::holds_alternative<bool>(v->data))
        a.isOriginalUser = std::get<bool>(v->data);
    // A string in the answer, a number in the join info.
    if (const Value* v = i["originalAccountCreationTimestampMs"]; v && v->integer()) {
        a.originalAccountCreationTimestampMs = *v->integer();
    } else {
        const std::string t = str(v);
        std::from_chars(t.data(), t.data() + t.size(), a.originalAccountCreationTimestampMs);
    }
    return a;
}

std::expected<PlayerAttributes, std::string> requestPlayerAttributes(const assets::RobloxWeb& web,
                                                                      std::string_view userAgent)
{
    auto response = web.withCookie(playerAttributesRequest(userAgent));
    if (!response)
        return std::unexpected("player-hydration-service: " + response.error());
    if (response->status != 200)
        return std::unexpected("player-hydration-service: " + assets::describeHttpFailure(*response));
    auto parsed = parsePlayerAttributes(response->body);
    if (!parsed)
        return std::unexpected("player-hydration-service: " + parsed.error());
    return parsed;
}

// -- keys

std::optional<std::vector<std::uint8_t>> decodeBase64(std::string_view text)
{
    const std::string plain = urlDecode(text);
    std::vector<std::uint8_t> out(plain.size());
    std::size_t len = 0;
    if (sodium_base642bin(out.data(), out.size(), plain.data(), plain.size(), " \t\r\n", &len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL_NO_PADDING)
            != 0
        && sodium_base642bin(out.data(), out.size(), plain.data(), plain.size(), " \t\r\n", &len, nullptr,
                             sodium_base64_VARIANT_ORIGINAL)
               != 0)
        return std::nullopt;
    out.resize(len);
    return out;
}

std::optional<Key> decodeBase64Key(std::string_view text)
{
    auto bytes = decodeBase64(text);
    if (!bytes || bytes->size() != 32)
        return std::nullopt;
    Key k{};
    std::ranges::copy(*bytes, k.begin());
    return k;
}

std::optional<Key> VersionedKey::key(int id) const
{
    for (const auto& [i, k] : versions)
        if (i == id)
            return k;
    return std::nullopt;
}

std::expected<VersionedKey, std::string> parseVersionedKey(std::string_view json, std::string_view application)
{
    auto doc = assets::json::parse(json);
    if (!doc)
        return std::unexpected(std::format("{}: not JSON", application));
    const Value* apps = (*doc)["applications"];
    const Value* app = apps ? (*apps)[application] : nullptr;
    if (!app)
        return std::unexpected(std::format("{}: no such application in the key document", application));
    VersionedKey out;
    out.send = static_cast<int>(integer((*app)["send"]));
    out.revert = static_cast<int>(integer((*app)["revert"]));
    const Value* versions = (*app)["versions"];
    if (!versions || !versions->array())
        return std::unexpected(std::format("{}: no versions", application));
    for (const Value& v : *versions->array()) {
        const Value* allowed = v["allowed"];
        if (allowed && allowed->boolean() == false)
            continue;
        auto key = decodeBase64Key(str(v["value"]));
        if (!key)
            return std::unexpected(std::format("{}: version {} has no 32-byte key", application, integer(v["id"])));
        out.versions.emplace_back(static_cast<int>(integer(v["id"])), *key);
    }
    if (!out.key(out.send))
        return std::unexpected(std::format("{}: the send version {} isn't in the list", application, out.send));
    return out;
}

std::expected<ServerKeys, std::string> serverKeysOf(const JoinScript& script)
{
    auto early = parseVersionedKey(script.clientPublicKeyData, "RakNetEarlyPublicKey");
    if (!early)
        return std::unexpected("ClientPublicKeyData: " + early.error());
    auto ephemeral = decodeBase64Key(script.ephemeralEarlyPubKey);
    if (!ephemeral)
        return std::unexpected("EphemeralEarlyPubKey isn't a base64 32-byte key");
    ServerKeys keys;
    keys.keyExchangeVersion = static_cast<std::uint16_t>(early->send);
    keys.earlyPublic = *early->key(early->send);
    keys.ephemeralEarlyPublic = *ephemeral;
    if (auto preAuth = preAuthOfClientTicket(script.clientTicket))
        keys.preAuth = std::move(*preAuth);
    return keys;
}

std::expected<PreAuth, std::string> preAuthOfClientTicket(std::string_view clientTicket)
{
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const std::size_t semi = clientTicket.find(';', start);
        parts.push_back(clientTicket.substr(start, semi == std::string_view::npos ? semi : semi - start));
        if (semi == std::string_view::npos)
            break;
        start = semi + 1;
    }
    if (parts.size() < 5)
        return std::unexpected(std::format("ClientTicket has {} parts, a pre-auth needs 5", parts.size()));
    auto blob = decodeBase64(parts[2]);
    auto token = decodeBase64(parts[3]);
    if (!blob || !token)
        return std::unexpected("ClientTicket pre-auth or token isn't base64");
    if (blob->size() > 255 || token->size() > 255)
        return std::unexpected("ClientTicket pre-auth or token is too long for a u8 length");
    int kind = 0;
    const auto [end, ec] = std::from_chars(parts[4].data(), parts[4].data() + parts[4].size(), kind);
    if (ec != std::errc{} || end != parts[4].data() + parts[4].size() || kind < 1 || kind > 255)
        return std::unexpected("ClientTicket pre-auth kind isn't a number in 1..255");
    PreAuth p;
    p.kind = static_cast<std::uint8_t>(kind);
    p.blob.assign(blob->begin(), blob->end());
    p.token.assign(token->begin(), token->end());
    return p;
}

std::string writeJson(const Value& v)
{
    if (const auto* b = std::get_if<bool>(&v.data))
        return *b ? "true" : "false";
    if (const auto* i = std::get_if<std::int64_t>(&v.data))
        return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&v.data)) {
        if (*d == static_cast<double>(static_cast<std::int64_t>(*d)))
            return std::to_string(static_cast<std::int64_t>(*d));
        return std::format("{}", *d);
    }
    if (const std::string* s = v.string()) {
        std::string out = "\"";
        for (const char c : *s) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else
                    out += c;
            }
        }
        return out + "\"";
    }
    if (const auto* a = v.array()) {
        std::string out = "[";
        for (const Value& e : *a)
            out += (out.size() == 1 ? "" : ",") + writeJson(e);
        return out + "]";
    }
    if (const auto* o = v.object()) {
        std::string out = "{";
        for (const auto& [k, e] : *o)
            out += (out.size() == 1 ? "" : ",") + writeJson(Value{k}) + ":" + writeJson(e);
        return out + "}";
    }
    return "null";
}

std::string buildJoinInfo(const JoinScript& script, const ClientInfo& client)
{
    // The keys of the genuine 0.741 client's join info, in its order, with
    // the values it sent [gdb dump of its 0x90]. "script:X" copies the join
    // script's member X; the rest are the client's own values. Not sent: the
    // four rbx<16 hex> blobs the genuine client adds (built at runtime, likely
    // integrity data).
    const Value* doc = script.doc.get();
    auto member = [&](std::string_view name) -> const Value* {
        const Value* v = doc ? (*doc)[name] : nullptr;
        return v && !std::holds_alternative<std::monostate>(v->data) ? v : nullptr;
    };
    auto quoted = [](std::string_view text) { return writeJson(Value{std::string(text)}); };
    std::string out = "{";
    auto add = [&](std::string_view key, const std::string& encoded) {
        out += (out.size() == 1 ? "" : ",") + quoted(key) + ":" + encoded;
    };
    auto copy = [&](std::string_view key, std::string_view source, std::string fallback) {
        const Value* v = member(source);
        add(key, v ? writeJson(*v) : std::move(fallback));
    };
    const std::int64_t accountAge = member("AccountAge") && member("AccountAge")->integer()
                                        ? *member("AccountAge")->integer()
                                        : 0;
    const bool under13 = member("IsUnknownOrUnder13") && std::holds_alternative<bool>(member("IsUnknownOrUnder13")->data)
                             ? std::get<bool>(member("IsUnknownOrUnder13")->data)
                             : false;
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

    copy("UserId", "UserId", "0");
    copy("VerifiedAMP", "VerifiedAMP", "0");
    add("VrDevice", quoted(""));
    const PlayerAttributes* attrs = script.playerAttributes ? &*script.playerAttributes : nullptr;
    add("AgeBracket", quoted(attrs ? attrs->ageBracket : "AgeUnchecked"));
    add("IsTeleportedIn", client.teleportedIn ? "true" : "false");
    copy("FollowUserId", "FollowUserId", "0");
    add("UserAgentStr", quoted(client.platform.userAgent));
    copy("HasVerifiedBadge", "HasVerifiedBadge", "false");
    copy("SuperSafeChat", "SuperSafeChat", "false");
    add("ClientDeviceRamMegabytes", std::to_string(client.ramMegabytes));
    copy("DisplayName", "DisplayName", quoted(""));
    copy("PartyId", "PartyId", quoted(""));
    copy("UserName", "UserName", quoted(""));
    add("TextChatRestrictionStatus", quoted("NotRestricted"));
    copy("WhoCanWhisperChatWithMeInExperiences", "WhoCanWhisperChatWithMeInExperiences", quoted("NoOne"));
    add("VoiceMutingPlatformType", "0");
    copy("CharacterAppearance", "CharacterAppearance", quoted(""));
    add("PreviousUniverseId", std::to_string(client.previousUniverseId));
    // A string in the genuine request although the join script has a number.
    if (const Value* v = member("AgeChecked"); v && v->integer())
        add("AgeChecked", quoted(std::to_string(*v->integer())));
    else
        copy("AgeChecked", "AgeChecked", quoted("0"));
    copy("UserIdMode", "UserIdMode", "0");
    copy("DomainUserId", "DomainUserId", "0");
    copy("GameChatType", "GameChatType", quoted(""));
    copy("AccountAge", "AccountAge", "0");
    copy("Rm3Tier0CadenceSeconds", "Rm3Tier0CadenceSeconds", "0");
    copy("PrivateServerID", "PrivateServerID", quoted(""));
    copy("ServerClaimedTime", "ServerClaimedTime", "0");
    copy("RandomSeed1", "RandomSeed1", quoted(""));
    copy("UdmuxToken", "UdmuxToken", "0");
    add("ClientRobloxChannel", quoted(client.channel));
    copy("APIsecurityToken", "APIsecurityToken", quoted(""));
    copy("PrivateServerOwnerID", "PrivateServerOwnerID", "0");
    add("Locale2IdRef", quoted(client.localeIdRef));
    add("LocaleIdInternal", quoted(client.locale));
    add("Locale2Info", quoted("14.0.0"));
    copy("MatchmakingAttributes", "MatchmakingAttributes", quoted(""));
    add("LocaleId", quoted(client.locale));
    copy("MembershipType", "MembershipType", quoted("None"));
    copy("CountryRegionCode", "CountryCode", quoted(""));
    add("OsPlatform", quoted(client.platform.name));
    add("OsPlatformName", quoted(client.platform.displayName));
    copy("HasRobloxSubscription", "HasRobloxSubscription", "false");
    add("ClientRobloxVersion", quoted(client.robloxVersion));
    add("RawJoinData", quoted(client.rawJoinData));
    add("ClientJoinData", quoted(R"({"LaunchData":"","ReferredByPlayerId":0})"));
    add("GameJoinContext", quoted("{}"));
    if (!script.joinTicket.empty())
        add("__joinTicket", quoted(script.joinTicket));
    add("IsUnder13", under13 ? "true" : "false");
    copy("CharacterAppearanceId", "characterAppearanceId", "0");
    // Without the hydration answer: GUESS from the account's age in days.
    add("OriginalAccountCreationTimestampMs",
        std::to_string(attrs ? attrs->originalAccountCreationTimestampMs : nowMs - accountAge * 86'400'000));
    add("Os", quoted(attrs ? attrs->os : client.platform.name));
    add("Gender", quoted(attrs ? attrs->gender : client.gender));
    add("Platform", quoted(attrs ? attrs->platform : "Computer"));
    add("IsOriginalUser", attrs && !attrs->isOriginalUser ? "false" : "true");
    return out + "}";
}

JoinRequest clientJoinRequest(const JoinScript& script, const ClientInfo& client)
{
    JoinRequest r;
    r.flags = 1;         // [code] 0x1054b767e
    r.clientValue = 0x24; // [code] Peer+0x140, the protocol version
    r.luauMin = 3;       // [code] the Luau bytecode versions this client accepts
    r.luauMax = 14;
    r.joinInfo = buildJoinInfo(script, client);
    return r;
}

std::vector<std::uint8_t> encodeClientJoinRequest(const JoinRequest& request, std::uint64_t negotiatedCapabilities,
                                                ClientBuild build)
{
    std::vector<std::uint8_t> out = encodeJoinRequest(request, negotiatedCapabilities);
    constexpr std::array oldTrailer{0x2bc0aba3u, 0x2bc0aba3u, 0xa73b9f9du, 0x2bc0aba3u, 0xf5ada9c9u, 0x323dacccu,
                                     0x852cabcbu};
    // Spelunk 0.745: 0x1055159b0..0x105515ac0. The duplicate is after
    // scramble1; scrambler 0x1057ed533 still uses seed 0x752fab0b.
    constexpr std::array currentTrailer{0x85cb1f5eu, 0xbf907b98u, 0x8ab1a79eu, 0x85cb1f5eu, 0x11496016u, 0xcbeffe78u};
    const std::span<const std::uint32_t> trailer = build == ClientBuild::Roblox742
                                                    ? std::span<const std::uint32_t>(oldTrailer)
                                                    : std::span<const std::uint32_t>(currentTrailer);
    for (const std::uint32_t w : trailer) {
        out.push_back(static_cast<std::uint8_t>(w));
        out.push_back(static_cast<std::uint8_t>(w >> 8));
        out.push_back(static_cast<std::uint8_t>(w >> 16));
        out.push_back(static_cast<std::uint8_t>(w >> 24));
    }
    return out;
}

std::vector<std::uint8_t> encodeClientVerification(std::uint32_t random, ClientBuild build)
{
    // 0.745's 0x1057ed6df: scramble2 ^ (0x1f5e0000 | low16(scramble1)).
    const std::uint32_t kB = build == ClientBuild::Roblox742 ? 0x999e0505u : 0x0e17c788u;
    const std::uint64_t value = (static_cast<std::uint64_t>(random) << 32) | ((random ^ kB) & 0xffffffffu);
    NetworkStream out;
    out.writeU8(kPlaceIdVerifiedId);
    out.writeZigzag64(static_cast<std::int64_t>(value));
    return std::move(out).take();
}

std::uint32_t xxhash32(std::span<const std::uint8_t> d, std::uint32_t seed)
{
    constexpr std::uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    auto rotl = [](std::uint32_t v, int r) { return (v << r) | (v >> (32 - r)); };
    auto read32 = [&](std::size_t i) {
        return static_cast<std::uint32_t>(d[i]) | static_cast<std::uint32_t>(d[i + 1]) << 8
               | static_cast<std::uint32_t>(d[i + 2]) << 16 | static_cast<std::uint32_t>(d[i + 3]) << 24;
    };
    std::size_t i = 0;
    std::uint32_t h;
    if (d.size() >= 16) {
        std::uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        for (; i + 16 <= d.size(); i += 16) {
            v1 = rotl(v1 + read32(i) * P2, 13) * P1;
            v2 = rotl(v2 + read32(i + 4) * P2, 13) * P1;
            v3 = rotl(v3 + read32(i + 8) * P2, 13) * P1;
            v4 = rotl(v4 + read32(i + 12) * P2, 13) * P1;
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    } else {
        h = seed + P5;
    }
    h += static_cast<std::uint32_t>(d.size());
    for (; i + 4 <= d.size(); i += 4)
        h = rotl(h + read32(i) * P3, 17) * P4;
    for (; i < d.size(); ++i)
        h = rotl(h + d[i] * P5, 11) * P1;
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

std::uint32_t ticketHashMix(std::uint32_t x, ClientBuild build)
{
    // The build's seed is still 0x752fab0b. The older closed form is pinned
    // by the 0.742 emulation vectors; 0.745 is from Spelunk's 0x105516501.
    constexpr std::uint32_t seed = 0x752fab0bu, C = 0x557bb5d7u;
    auto rol = [](std::uint32_t v, int r) { return (v << r) | (v >> (32 - r)); };
    // 0.745's 0x105516501 chooses branches with seed * 0x0729bbaa
    // (0x555b9e4e), replacing 0.742's seed * 0x91ecbba4 (0xcd9e9c0c).
    const bool old = build == ClientBuild::Roblox742;
    std::uint32_t r14 = rol(x + C, old ? 25 : 7) - seed;
    std::uint32_t r13 = rol(seed * r14, 19);
    std::uint32_t ebx = rol((old ? C : seed) ^ (C - r13), 15);
    std::uint32_t r8 = rol(ebx + (0u - seed) + (old ? 0u - C : C), 23);
    std::uint32_t r12 = seed - r8;
    std::uint32_t edi = rol((0u - r12) + C, 3) ^ C;
    return edi;
}

std::vector<std::uint8_t> encodeTicketSubmission(std::int64_t userId, std::string_view ticket,
                                                 std::string_view sessionId, std::string_view platform, ClientBuild build)
{
    NetworkStream out;
    out.writeU8(0x8a);
    out.writeZigzag64(userId);
    out.writeString(ticket);
    out.writeU32(0x24);
    // s1: a ROT13-hidden prefix, "!", the release's constants and scrambles
    // as masked hex bytes, ".".
    // 0.745: prefix at 0x1057ee9a1, formatter 0x1057ed9f4. Byte masks
    // 0xdf / 0xff; scrambles 0x8ab1a79e / 0x6133538c.
    out.writeString(build == ClientBuild::Roblox742
                        ? "530610627598173cdbca04acbb9e26c60358c4e7!a39dc935ab9fa9cbc03bad4d2ba7f564."
                        : "2c2d778eadb4263fc76bf47d2f3f6717eb2afe41!5e989e8c1f7ba753cb90b13385bf8a61.");
    out.writeString(platform);
    out.writeString("?");
    const std::uint32_t h = ticketHashMix(
        xxhash32(std::span(reinterpret_cast<const std::uint8_t*>(ticket.data()), ticket.size()), 1), build);
    out.writeVarint(h);
    out.writeVarint(static_cast<std::uint32_t>(h + 0xf4520ff3u));
    out.writeString("");
    out.writeString(sessionId);
    out.writeU32(0xc001cafeu);
    return std::move(out).take();
}

BlockKey ticketSubmissionKey(std::int64_t placeId, ClientBuild build)
{
    const bool old = build == ClientBuild::Roblox742;
    const std::string text = std::format("{}{}{}{}", static_cast<std::int32_t>(placeId),
                                         static_cast<std::int32_t>(old ? 0x2bc0aba3u : 0x85cb1f5eu),
                                         static_cast<std::int32_t>(old ? 0xf5ada9c9u : 0x8ab1a79eu),
                                         static_cast<std::int32_t>(old ? 0xa73b9f9du : 0xbf907b98u));
    BlockKey key{};
    std::copy_n(text.begin(), std::min(text.size(), key.size()), key.begin());
    return key;
}

std::vector<std::uint8_t> encodePreferredSpawn(std::string_view name)
{
    NetworkStream out;
    out.writeU8(0x8f);
    out.writeString(name);
    return std::move(out).take();
}

// -- printing

bool isSecretJoinKey(std::string_view key)
{
    static constexpr std::array secrets{
        std::string_view("ClientTicket"),     std::string_view("SessionId"),
        std::string_view("authenticationTicket"), std::string_view("APIsecurityToken"),
        std::string_view("TokenValue"),       std::string_view("NetStackTokenValue"),
        std::string_view("RandomSeed1"),      std::string_view("joinScriptUrl"),
        std::string_view("joinTicket"),       std::string_view("SerializedClientFields"),
        std::string_view("EncryptedServerFields"),
    };
    return std::ranges::contains(secrets, key);
}

std::string maskJoinResponse(std::string_view body)
{
    // Members at any depth: rewrite recursively on the raw text.
    std::string out;
    std::size_t last = 0;
    bool any = false;
    forEachMember(body, [&](std::string_view key, std::size_t start, std::size_t end) {
        any = true;
        out.append(body.substr(last, start - last));
        if (isSecretJoinKey(key))
            out += "\"<masked>\"";
        else if (body[start] == '{')
            out += maskJoinResponse(body.substr(start, end - start));
        else
            out.append(body.substr(start, end - start));
        last = end;
        return true;
    });
    if (!any)
        return std::string(body);
    out.append(body.substr(last));
    return out;
}

std::string describeJoinScript(const JoinScript& s)
{
    std::string ends;
    for (const auto& e : s.udmuxEndpoints)
        ends += std::format("{}{}:{}", ends.empty() ? "" : ", ", e.address, e.port);
    std::string out = std::format("server {}:{} behind udmux [{}]{}, place {} v{} universe {}, job {}\n",
                                  s.machineAddress, s.serverPort, ends, s.directServerReturn ? " (direct return)" : "",
                                  s.placeId, s.placeVersion, s.universeId, s.gameId);
    out += std::format("player {} ({}, id {}, {} days, {}), country {}, datacenter {}, rcc {}\n", s.userName,
                       s.displayName, s.userId, s.accountAge, s.membershipType, s.countryCode, s.dataCenterId,
                       s.rccVersion);
    out += std::format("rupp token algorithm {}, pepper {}, token {} base64 chars; ticket {} chars, session id {} chars",
                       s.tokenGenAlgorithm, s.pepperId, s.tokenValue.size(), s.clientTicket.size(),
                       s.sessionId.size());
    if (auto keys = serverKeysOf(s))
        out += std::format("\nkey exchange version {}, early key {}, ephemeral early key {}", keys->keyExchangeVersion,
                           hexString(keys->earlyPublic), hexString(keys->ephemeralEarlyPublic));
    else
        out += "\nkeys: " + keys.error();
    return out;
}

} // namespace lunar::net
