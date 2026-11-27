#pragma once

// Commands and options that talk to Roblox's web APIs: real users' avatars
// (--user, --me) and place downloads (--download-place).

#include <lunar/avatar/UserAvatar.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// --user <id or username> or --me (whoever the cookie belongs to).
struct UserChoice {
    std::string user;
    bool me = false;

    bool chosen() const { return me || !user.empty(); }
};

// Takes --user X or --me at args[i] (moving i past a value); false if
// args[i] is neither.
bool parseUserFlag(std::span<const std::string_view> args, std::size_t& i, UserChoice& choice);

// The user and their avatar, fetched from Roblox.
struct ChosenUser {
    lunar::avatar::RobloxUser user;
    std::shared_ptr<const lunar::avatar::UserAvatar> avatar;
};
std::expected<ChosenUser, std::string> resolveUser(const UserChoice& choice);

// --avatar-info: who the user is and what their avatar wears.
int avatarInfo(const UserChoice& choice);

// --download-place <placeId> <out.rbxl>: the place file through asset
// delivery, anonymously if Roblox allows, else with the login (any
// signed-in user may download an uncopylocked place).
int downloadPlace(std::string_view placeId, const std::filesystem::path& outPath);
