#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {

struct GameInviteOptions {
    std::int64_t inviteUser = 0;
    std::string promptMessage;
    std::string inviteMessageId;
    std::string launchData;
};

struct GameInviteFriend {
    std::int64_t userId = 0;
    std::string username;
    std::string displayName;
    bool online = false;
    int locationType = 0;
    std::int64_t placeId = 0;
    std::int64_t universeId = 0;
    std::string gameId;
    std::string lastLocation;
    std::string lastOnline;
};

enum class GameInviteStatus { Available, Sending, Sent, Failed };

// A single core prompt. Poll and the user actions run on the UI thread;
// implementations perform HTTP in the background and retain no raw VM pointer.
class GameInviteSession {
public:
    virtual ~GameInviteSession() = default;
    virtual const GameInviteOptions& options() const = 0;
    virtual void poll() = 0;
    virtual bool loading() const = 0;
    virtual std::string_view error() const = 0;
    virtual const std::vector<GameInviteFriend>& friends() const = 0;
    virtual GameInviteStatus status(std::int64_t userId) const = 0;
    virtual bool sending() const = 0;
    virtual void invite(std::int64_t userId) = 0;
    virtual void retry() = 0;
    virtual void close() = 0;
    virtual bool closed() const = 0;
};

} // namespace lunar
