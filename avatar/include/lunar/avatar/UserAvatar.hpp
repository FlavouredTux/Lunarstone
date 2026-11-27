#pragma once

// Real users' avatars from Roblox's web APIs, the way the client gets them
// for Players:GetHumanoidDescriptionFromUserId and character loading: the
// avatar-fetch endpoint (avatar.roblox.com/v2/avatar/avatar-fetch/?placeId=
// &userId=, 0x100a8e7c3), whose reply becomes a HumanoidDescription. Avatars
// are public, so all of this is anonymous except the signed-in user lookup.

#include <lunar/Classes.hpp>
#include <lunar/assets/Web.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::avatar {

struct RobloxUser {
    std::int64_t id = 0;
    std::string name;
    std::string displayName;
};

// A user by id ("156", users.roblox.com/v1/users/{id}) or by username
// ("builderman", POST users.roblox.com/v1/usernames/users). Anonymous.
std::expected<RobloxUser, std::string> findUser(const assets::RobloxWeb& web, std::string_view idOrName);

// Whoever the cookie belongs to (users.roblox.com/v1/users/authenticated).
std::expected<RobloxUser, std::string> authenticatedUser(const assets::RobloxWeb& web);

struct AvatarEmote {
    std::int64_t assetId = 0;
    std::string name;
    int position = 0; // slot on the emote wheel, 1-8
};

struct UserAvatar {
    // resolvedAvatarType: what the user picked (the game's settings may
    // still override it).
    Enum::HumanoidRigType rigType = Enum::HumanoidRigType::R15;
    std::shared_ptr<HumanoidDescription> description;
    std::vector<AvatarEmote> emotes;
    std::vector<std::int64_t> gear; // equipped gear asset ids, not part of the description
};

// An avatar-fetch reply as a HumanoidDescription: body colours, scales,
// body parts (a DynamicHead is the Head), clothing, the face, rigid
// accessories in the per-slot lists, layered clothing and eyebrows/eyelashes
// as AccessoryDescription children, and the animation pack.
std::expected<UserAvatar, std::string> parseAvatarFetch(std::string_view text);

// Fetches and parses a user's avatar (placeId 0 outside a game).
std::expected<UserAvatar, std::string> fetchUserAvatar(const assets::RobloxWeb& web, std::int64_t userId,
                                                       std::int64_t placeId = 0);

// GetCharacterAppearanceInfoAsync's reply as JSON
// (avatar.roblox.com/v2/avatar/users/{id}/avatar, 0x104e29e26).
std::expected<std::string, std::string> fetchCharacterAppearanceInfo(const assets::RobloxWeb& web, std::int64_t userId);

} // namespace lunar::avatar
