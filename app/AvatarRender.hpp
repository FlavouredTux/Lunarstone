#pragma once

#include <lunar/avatar/UserAvatar.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

struct AvatarRenderOptions {
    std::optional<lunar::Enum::HumanoidRigType> rig; // --r6/--r15: overrides the avatar's own
    std::string anim;                                // a default animation set to play directly ("walk", "idle", ...)
    std::optional<float> speed;                      // or drive the Animate logic as if running at this speed
    float time = 0.0f;                               // seconds to animate before the picture
    float yaw = 30.0f;                               // degrees the camera is turned from straight ahead
    std::uint32_t width = 800;
    std::uint32_t height = 800;
    // A real user's avatar (--user, --me) instead of the default one.
    std::shared_ptr<const lunar::avatar::UserAvatar> avatar;
};

// Builds the default character (or a user's), poses it and renders it offscreen to a PNG
// (or PPM), framed like an avatar thumbnail.
int renderAvatar(const std::filesystem::path& outPath, const AvatarRenderOptions& options);
