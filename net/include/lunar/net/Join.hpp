#pragma once

#include <lunar/RobloxVersion.hpp>

// Joining a real Roblox game server: the web join API (gamejoin.roblox.com)
// and the "join script" it answers with, which names the server (behind its
// UDMUX proxy), the key exchange version's public keys, the Rupp token and
// the player, and is what the 0x90 join request carries to the server.
//
// Secrets: the join script holds the ClientTicket, the SessionId (a signed
// blob) and the response an authentication ticket. None of them is ever
// printed, logged or put into an error message; describeJoinScript() and
// maskJoinResponse() are the printable forms.

#include <lunar/RemoteValue.hpp>
#include <lunar/assets/Json.hpp>
#include <lunar/assets/Web.hpp>
#include <lunar/net/Crypto.hpp>
#include <lunar/net/Replication.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::net {

struct JoinEndpoint {
    std::string address;
    std::uint16_t port = 0;
};

// The fields Lunarstone uses of the join script; `raw` is the whole object's
// text as the API sent it (for the join request, which carries it whole).
// PlayerHydrationService's signed player attributes (0.742 0x104c1196d):
// GET apis.roblox.com/player-hydration-service/v1/players/signed with the
// cookie answers {"playerInfo":{...},"signature":base64} [live]. The client
// copies them into the join info (AgeBracket, Gender, Platform, Os,
// IsOriginalUser, OriginalAccountCreationTimestampMs).
struct PlayerAttributes {
    std::string ageBracket;
    std::string gender;
    std::string platform;
    std::string os; // what the API derives from the User-Agent ("OSX" for the mac one)
    bool isOriginalUser = false;
    std::int64_t originalAccountCreationTimestampMs = 0;
};

struct JoinScript {
    std::string raw;
    std::shared_ptr<const assets::json::Value> doc; // the parsed object, for the fields not listed below

    std::string machineAddress; // the game server's own (internal) address
    std::uint16_t serverPort = 0;
    std::vector<JoinEndpoint> udmuxEndpoints; // where the client actually sends
    bool directServerReturn = false;

    // The Rupp (UDMUX proxy) token material.
    int tokenGenAlgorithm = 0;
    std::int64_t pepperId = 0;
    std::string tokenValue; // base64

    std::string userName, displayName;
    std::int64_t userId = 0;
    std::int64_t placeId = 0, universeId = 0, placeVersion = 0, dataCenterId = 0, accountAge = 0;
    std::string gameId; // the job id (a UUID)
    std::string membershipType, countryCode, rccVersion;
    std::string characterAppearance; // a URL
    std::string clientPublicKeyData; // JSON: {"applications":{"RakNetEarlyPublicKey":{"versions":[{"id":N,"value":b64}],"send":N}}}
    std::string ephemeralEarlyPubKey; // base64, possibly URL-encoded (%3D)
    std::string sessionId;            // secret
    std::string clientTicket;         // secret
    // The response's top-level joinTicket (a JSON string, or its member's raw
    // JSON when it is an object): the join request's __joinTicket. Secret.
    std::string joinTicket;
    // Filled by the caller from requestPlayerAttributes; buildJoinInfo
    // guesses the values without it.
    std::optional<PlayerAttributes> playerAttributes;
};

struct JoinResponse {
    std::string jobId;
    int status = -1;
    std::string message;
    std::string joinScriptUrl;
    std::string authenticationTicket; // secret
    std::optional<int> queuePosition;
    std::optional<JoinScript> joinScript;
};

// Join statuses of the API, as the client's status switch (0x10523a728)
// and message table (0x105237135) treat them [code]. 2 is the only one
// that comes with a join script.
namespace joinstatus {
inline constexpr int Waiting = 0;  // "Waiting for an available server": retried
inline constexpr int Loading = 1;  // "Server found, loading...": retried
inline constexpr int Joining = 2;  // success
inline constexpr int Disabled = 3;
inline constexpr int Error = 4;    // "Cannot find experience server"
inline constexpr int GameEnded = 5;
inline constexpr int GameFull = 6; // retried
inline constexpr int UserLeft = 10;
inline constexpr int Restricted = 11;
inline constexpr int Unauthorized = 12;
inline constexpr int Flooded = 13; // "Server is busy"
inline constexpr int HashExpired = 14;
inline constexpr int HashException = 15;
inline constexpr int PartyCannotFit = 16;
inline constexpr int HttpError = 17;
inline constexpr int PrivacyUnauthorized = 21;
inline constexpr int Queued = 22; // "Requested experience is full. Waiting in queue...": retried by queue position
inline constexpr int Banned = 23;
} // namespace joinstatus
// The client's message for a status.
std::string joinStatusName(int status);
// Whether the client asks again (0, 1, 6 and 22), and after how long
// (0x10523b4d8: queue position 1 -> 2 s, 2..5 -> 5 s, 6+ -> 10 s; else
// about 1.3^attempt seconds clamped to 1..10).
bool joinStatusRetryable(int status);
double joinRetrySeconds(int status, std::optional<int> queuePosition, int attempt);

// Reads the API's JSON. Fails on anything that isn't a join-game response.
std::expected<JoinResponse, std::string> parseJoinResponse(std::string_view body);

// The value text of a top-level member of a JSON object, exactly as written
// (used to keep the join script's text byte for byte). Empty when absent.
std::string_view rawJsonMember(std::string_view object, std::string_view key);

// -- teleports (research notes-join.md, "Teleports")

// What a teleport hands the client to go with: the table of
// Player.OnTeleportInternal(state, table, loadingGui) [live], the parts the
// hop uses. The genuine client's handler is 0x104669f6c.
struct TeleportTicket {
    std::uint64_t placeId = 0;
    // A reserved or VIP server: join-reserved-game's accessCode, the first
    // of reservedServerId, vipServerId, reservedServerAccessCode that's set
    // (0x105240e72).
    std::string reservedServerId, vipServerId, reservedServerAccessCode;
    std::string instanceId;    // TeleportToPlaceInstance: join-game-instance's gameId
    std::string teleportToken; // goes with every teleport join (0x105240ed0)
    std::string joinData;      // the signed join data (JSON text) the new server reads
    std::string auth;          // its MAC (JSON text: mac, ver)
    std::string partyData;
    std::uint64_t universeId = 0; // the universe it leaves
    int teleportType = 0;         // 2: TeleportToPrivateServer
    std::string spawnName;

    std::string accessCode() const;
};
// Null unless the table names a place to go to.
std::optional<TeleportTicket> teleportTicket(const RemoteValue& table);
// The join info's RawJoinData after a teleport (0x10466b38d, "Save
// joinData"): a JSON object of the ticket's auth, joinData and partyData and
// when the client started the teleport. The new server verifies joinData
// with auth (its onUserJoin, 0x10466e924). GUESS: the keys sorted, as the
// client's table writes them.
std::string teleportRawJoinData(const TeleportTicket& ticket, std::int64_t startMs);

struct JoinOptions {
    std::uint64_t placeId = 0;
    // A teleport's join: join-reserved-game with the access code, or
    // join-game-instance with the instance, isTeleport and the token
    // (0x105243af8, 0x1052432dc); placeId is the ticket's then.
    const TeleportTicket* teleport = nullptr;
    std::string gameJoinAttemptId; // empty: a fresh UUID
    bool isTeleport = false;
    // The mac client's (builder 0x104081311; the genuine 0.741 one sends
    // "Roblox/DarwinRobloxApp/0.741.0.7411056 (GlobalDist; RobloxDirectDownload)"),
    // so the web join and the join request agree on the platform. The API also
    // accepts Windows' "Roblox/WinInet" [live].
    std::string userAgent = robloxMacUserAgent;
    std::string joinOrigin = "ExperienceDetailsPage"; // what the genuine client sends from a game's page
    // A private (VIP) server's link code: join-private-game instead.
    std::string linkCode;
    // One server of the place, by its JobId (a friend's, say): join-game-instance
    // instead, as joining someone from their profile does.
    std::string gameId;
};

// A private server's link: either it names the place and the link code
// (roblox.com/games/<id>/...?privateServerLinkCode=<code>), or it's a share
// link (roblox.com/share?code=<code>&type=Server) to resolve first.
struct ServerLink {
    std::uint64_t placeId = 0;
    std::string linkCode;
    std::string shareCode; // a share link's, before it's resolved
};
std::optional<ServerLink> parseServerLink(std::string_view link);
// The link as join-private-game takes it, a share link resolved with the
// login (POST apis.roblox.com/sharelinks/v1/resolve-link, as the client's
// "resolve-link" does).
std::expected<ServerLink, std::string> resolveServerLink(const assets::RobloxWeb& web, std::string_view link);
assets::HttpRequest playerAttributesRequest(std::string_view userAgent);
std::expected<PlayerAttributes, std::string> parsePlayerAttributes(std::string_view body);
std::expected<PlayerAttributes, std::string> requestPlayerAttributes(const assets::RobloxWeb& web,
                                                                      std::string_view userAgent);
// What requestJoin sends (for tests and the log; the cookie is added by
// RobloxWeb::withCookie, never here).
assets::HttpRequest joinRequest(const JoinOptions& options);
// POST https://gamejoin.roblox.com/v1/join-game with the login. Fails on
// transport errors, HTTP errors and unreadable bodies; a status other than 2
// is a successful call with no join script (the caller decides to retry).
std::expected<JoinResponse, std::string> requestJoin(const assets::RobloxWeb& web, const JoinOptions& options);

// A random version-4 UUID in the API's lowercase hyphenated form.
std::string makeUuid();

// -- key material (PROTOCOL.md 2.4)

// base64, standard alphabet, optional padding, optionally URL-encoded
// ("%3D" for "=", "%2B", "%2F"). Null when it isn't 32 bytes.
std::optional<Key> decodeBase64Key(std::string_view text);
std::optional<std::vector<std::uint8_t>> decodeBase64(std::string_view text);

// One versioned key document {"applications":{"<name>":{"versions":[{"id":N,"value":"..","allowed":true}],"send":N,"revert":N}}}.
struct VersionedKey {
    int send = 0;
    int revert = 0;
    std::vector<std::pair<int, Key>> versions; // id, key (only those with "allowed": true)

    std::optional<Key> key(int id) const;
};
std::expected<VersionedKey, std::string> parseVersionedKey(std::string_view json, std::string_view application);

// The RakNet password a real server expects in ID_CONNECTION_REQUEST; without
// it the server answers ID_INVALID_PASSWORD [live: the bytes the 0.741 client
// passes to RakPeer::Connect (0x1054ec8bf in 0.742), which builds them in a
// scattered static initialiser (0x105797611..) from "7", "^", "l" and more].
inline constexpr std::string_view kServerConnectionPassword{"7O^\x11lE", 6};

// The pre-auth a live client puts in RbxOpenRequest2, taken from the join
// script's ClientTicket ("timestamp;signature;preauth;token;kind", the three
// binary parts base64) [live: matches a capture of the 0.741 client]: the
// clear header carries u8 kind (17), u8 33, the 33-byte pre-auth (16 bytes,
// u8 key id, 16-byte MAC the server checks against its PreAuthKey ring,
// 0x102d94598), and the box's token string is the 66-byte token. A real
// server silently drops a Request2 without them (no RbxOpenReply2).
struct PreAuth {
    std::uint8_t kind = 0;
    std::string blob;  // the 33-byte pre-auth
    std::string token; // the 66-byte box token
};
std::expected<PreAuth, std::string> preAuthOfClientTicket(std::string_view clientTicket);

// What the handshake needs from a join script: the wire key exchange version,
// the server's early public keys and the pre-auth.
struct ServerKeys {
    std::uint16_t keyExchangeVersion = 0;
    Key earlyPublic{};          // RakNetEarlyPublicKey (ClientPublicKeyData)
    Key ephemeralEarlyPublic{}; // EphemeralEarlyPubKey
    std::optional<PreAuth> preAuth; // from ClientTicket, when it has one
};
std::expected<ServerKeys, std::string> serverKeysOf(const JoinScript& script);

// Release-specific join fields. The older profile pins captured/emulated
// 0.742 packets; live joins use the supported 0.745 build.
enum class ClientBuild : std::uint8_t { Roblox742, Roblox745 };

// -- what the client sends once ID_CONNECTION_REQUEST_ACCEPTED arrives
// (Client 0x1054b8ef0, in this order: [0xa7 shared string hashes, skipped
// when empty], 0x90 join request, 0x92, 0x8a ticket, 0x8f spawn name).
// The key that seals the 0x8a body: four signed decimal int32s (place id,
// build word 1, scramble1, build word 2), cut or NUL-padded to 16 bytes.
// 0.745: 0x1055161f8..0x105516237; 0.742: 0x1054b80db..0x1054b815c.
BlockKey ticketSubmissionKey(std::int64_t placeId, ClientBuild build = ClientBuild::Roblox745);

// The join request as Client 0x1054b7644 writes it [code+emu]: flags 1,
// value 0x24 (the protocol version, Peer+0x140), Luau bytecode versions
// 3..14, no dynamic fast-variable names on a fresh client, then the join
// info JSON. The JSON is PlayerConfigurer's table (0x1055969f6) serialised:
// the player and client fields, most copied from the join script [GUESS
// for the exact value set; the server reads UserId and PreloadedVersion].
// What the client says it runs on [code]: its build's Enum.Platform name
// (0x102f024da; the 0x8a ticket's string and the join info's OsPlatform),
// the display name (0x102f024f9; OsPlatformName) and the User-Agent of its
// web calls, from which the player attributes API derives the signed Os.
struct ClientPlatform {
    std::string name = "OSX";
    std::string displayName = "MacOS";
    std::string userAgent = robloxMacUserAgent;
};
// "mac": the supported Mac build's values. "linux": Enum.Platform.Linux (13),
// what Lunarstone runs on; the attributes API knows no Linux User-Agent and
// signs Os "Unknown" for it [live]. Nothing for other names.
std::optional<ClientPlatform> clientPlatform(std::string_view name);

struct ClientInfo {
    // What the genuine 0.741 mac client sends, with this build's version.
    std::string robloxVersion = lunar::robloxVersion;
    std::string channel = "live";
    ClientPlatform platform;
    std::string locale = "de_de";
    std::string localeIdRef = "RBX80DBD3EBF4F9406187A35732030C781F"; // de_de's, as the genuine client sent it
    std::string gender = "Male"; // GUESS, for when the player attributes are missing
    int ramMegabytes = 16384;
    // After a teleport: IsTeleportedIn, the ticket's RawJoinData
    // (teleportRawJoinData) and PreviousUniverseId, the universe left.
    bool teleportedIn = false;
    std::string rawJoinData;
    std::uint64_t previousUniverseId = 0;
};
std::string buildJoinInfo(const JoinScript& script, const ClientInfo& client = {});
JoinRequest clientJoinRequest(const JoinScript& script, const ClientInfo& client = {});
// encodeJoinRequest plus the release's trailer (six words in 0.745, seven in
// 0.742). Spelunk: 0.745 Mac Client::sendJoinRequest at 0x105515752.
std::vector<std::uint8_t> encodeClientJoinRequest(const JoinRequest& request, std::uint64_t negotiatedCapabilities,
                                                ClientBuild build = ClientBuild::Roblox745);
// 0x92 client -> server: u8 0x92, zigzag64((A << 32) | (A ^ B)), A random.
// B is release-specific: 0x0e17c788 in 0.745, 0x999e0505 in 0.742.
std::vector<std::uint8_t> encodeClientVerification(std::uint32_t random, ClientBuild build = ClientBuild::Roblox745);
// 0x8a ticket submission (0.745 0x105515f5e; 0.742 0x1054b7e50): u8 0x8a,
// zigzag64 userId, string ClientTicket, u32 LE 0x24, string s1 (a build
// constant), string platform (ClientPlatform::name), string "?", varint H, varint H + 0xf4520ff3
// (H = mix(xxhash32(ticket, seed 1))), string "" (an empty global), string
// SessionId, u32 LE 0xc001cafe.
std::vector<std::uint8_t> encodeTicketSubmission(std::int64_t userId, std::string_view ticket,
                                                 std::string_view sessionId, std::string_view platform = "OSX",
                                                 ClientBuild build = ClientBuild::Roblox745);
std::uint32_t xxhash32(std::span<const std::uint8_t> data, std::uint32_t seed);
std::uint32_t ticketHashMix(std::uint32_t x, ClientBuild build = ClientBuild::Roblox745);
// 0x8f preferred spawn name (0x1054ba6f8): u8 0x8f, string.
std::vector<std::uint8_t> encodePreferredSpawn(std::string_view name);
// A compact JSON serialisation of a parsed value (keys in map order).
std::string writeJson(const assets::json::Value& value);

// -- printing without secrets
std::string describeJoinScript(const JoinScript& script);
// The response text with every secret member's value replaced by
// "<masked>" (for saving examples). Works on the raw text so the layout stays.
std::string maskJoinResponse(std::string_view body);
bool isSecretJoinKey(std::string_view key);

} // namespace lunar::net
