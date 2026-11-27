#pragma once

#include "LiveClient.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// The networking commands: --net-decode, --net-server, --net-client,
// --net-loopback and --net-join (NetJoin.hpp). `args` starts with the command. Returns the exit code, or -1
// when the arguments don't fit (the caller prints the usage).
int runNetCommand(std::span<const std::string_view> args);

// --net-play --replay <prefix>: a session recorded with --net-join --out
// <prefix>, played back in real time as if it were live (no network).
std::expected<std::unique_ptr<GameFeed>, std::string> replayLive(const std::string& prefix,
                                                                 const std::string& scriptPlatform);

// What a session's .joinscript.json left of the join it made: the user that
// joined, the job and the place it was for. The ticket itself stays in the
// file: nothing here reads it back out.
struct RecordedJoin {
    std::int64_t userId = 0;
    std::string jobId;
    double placeId = 0;
};
RecordedJoin recordedJoin(const std::string& prefix);
