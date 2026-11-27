#pragma once

#include "LiveClient.hpp"

#include <lunar/classes/World.hpp>
#include <lunar/render/Graphics.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// --net-join <placeId> [--seconds N] [--out <prefix>] [--attempts N]
// [--scripts] [--walk square|line|n3,e3,...]: joins a
// real Roblox game server with the signed-in account (the join API, then
// the RakNet handshake with the server's keys through its UDMUX proxy, the
// join request, and the join as the server replicates it), stays for N
// seconds and summarises what came in. With --walk our character walks
// that way (LiveCharacter) and the server hears it as from any client.
// Returns the exit code, -1 for bad arguments.
int runNetJoin(std::span<const std::string_view> args);

struct LiveJoinOptions {
    std::uint64_t placeId = 0;
    int attempts = 3;
    std::string platform = "linux"; // what the join says we run on: linux or mac
    std::string scriptPlatform;     // an Enum.Platform name for GetPlatform(); empty: the platform's
    std::string out;                // a capture, keylog and masked join script of the session, at <out>.*
    std::string privateLink;        // a private server's link to join it by (net::parseServerLink); its place wins
    std::string gameId;             // one server of the place to join, by its JobId (a friend's)
};
// --net-play's game, for a window to pump each frame: the web join as
// --net-join does it, on a thread of its own (GameFeed::joining() says how
// it goes), then a session with the server. Returns at once, failing only
// for what's wrong before asking (no login, no such platform); a join that
// fails later ends the feed with the reason (GameFeed::ended()), also said
// on stderr.
std::expected<std::unique_ptr<GameFeed>, std::string> joinLive(const LiveJoinOptions& options);

// Renders a joined replica to outPath (main.cpp): through the default
// Roblox camera behind the character of the player with localUserId, with
// that player's GUI on top, or as LUNAR_NET_LOOK aims it.
int renderReplica(const std::shared_ptr<lunar::DataModel>& game, const std::filesystem::path& outPath,
                  std::int64_t localUserId, const lunar::render::GraphicsSettings& graphics = {});

// --graphics, --render-scale, --raytracing, --reflect-everything, --ambient-occlusion and --shadow-all-lights at
// args[i] (main.cpp): true when it was one (i past its value); error says
// what's wrong with a bad value.
bool parseGraphicsFlag(std::span<const std::string_view> args, std::size_t& i,
                       lunar::render::GraphicsSettings& graphics, std::string& error);
