#pragma once

#include <lunar/GameInvite.hpp>

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::assets {

class RobloxWeb;

// Fixed Roblox endpoints used by the core invitation prompt and Player APIs.
// Permission is returned by Roblox, including per-recipient restrictions.
std::expected<bool, std::string> canPromptGameInvite(const RobloxWeb& web, std::int64_t placeId,
                                                     std::int64_t recipient = 0);
std::expected<std::vector<GameInviteFriend>, std::string> getFriends(const RobloxWeb& web, std::int64_t userId);
std::expected<std::vector<GameInviteFriend>, std::string> getFriendsOnline(const RobloxWeb& web, std::int64_t userId);
std::expected<GameInviteFriend, std::string> getInviteUser(const RobloxWeb& web, std::int64_t userId);
std::expected<void, std::string> sendGameInvite(const RobloxWeb& web, std::int64_t placeId, std::int64_t recipient,
                                                const GameInviteOptions& options, std::string_view idempotencyKey,
                                                std::function<bool()> cancelled = {});

} // namespace lunar::assets
