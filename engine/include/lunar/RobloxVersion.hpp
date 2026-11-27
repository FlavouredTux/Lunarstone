#pragma once

#include <string>

namespace lunar {

// The release the client and its generated API metadata target. Keep the
// handshake, HTTP agents and Luau's version() on the same release.
// https://clientsettingscdn.roblox.com/v2/client-version/MacPlayer/channel/LIVE
inline constexpr char robloxVersion[] = "0.745.0.7451137";
inline const std::string robloxMacUserAgent
    = std::string("Roblox/DarwinRobloxApp/") + robloxVersion + " (GlobalDist; RobloxDirectDownload)";
inline const std::string robloxLinuxUserAgent
    = std::string("Roblox/LinuxRobloxApp/") + robloxVersion + " (GlobalDist; RobloxDirectDownload)";

} // namespace lunar
