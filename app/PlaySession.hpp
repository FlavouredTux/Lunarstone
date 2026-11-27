#pragma once

#include "GameWindow.hpp"

#include <lunar/Classes.hpp>
#include <lunar/avatar/UserAvatar.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/ui/WorldMap.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Builds a player's character: a Model with a Humanoid and a
// HumanoidRootPart, unparented. The play session places it at a spawn,
// parents it to the Workspace and drives it. Swap this for the real avatar
// rig; the default is physics::buildPlaceholderCharacter.
using CharacterBuilder = std::function<std::shared_ptr<lunar::Model>(lunar::Player& player)>;

// Who the local player is: Player1 with the default avatar unless a real
// user was asked for (--user, --me).
struct LocalUser {
    std::string name = "Player1";
    std::string displayName; // empty: the name
    std::int64_t userId = -1;
    // Their avatar, already fetched; characters of players whose
    // CharacterAppearanceId is another real user are fetched when they spawn.
    std::shared_ptr<const lunar::avatar::UserAvatar> avatar;
};

struct PlayOptions {
    bool runScripts = true;                   // start the place's server Scripts
    bool shiftLock = false;                   // the user's Shift Lock setting (off in the real client)
    bool vsync = true;                        // false: as many frames as it can draw (it may tear)
    int fpsCap = 0;                           // frames a second at most; 0 for no cap
    lunar::render::GraphicsSettings graphics; // --graphics and --render-scale
    int players = 1;                          // with scripts: the others join the server without clients
    CharacterBuilder buildCharacter;          // null: the placeholder rig
    LocalUser user;
    lunar::ui::MapSettings map; // Lunarstone's minimap and full-screen map
    std::vector<Screenshot> screenshots; // --screenshot: pictures of the window as it plays
    double seconds = 0.0;                // --seconds: close the window after this long; 0 for never
    bool mute = false;                   // --mute: no sound output
    std::optional<std::uint16_t> api;    // --api: the Luau tab over HTTP on this port (ConsoleApi)
};

// Play Solo: loads a place, spawns the local player's character at a
// SpawnLocation (or the origin) and runs physics, the Humanoid and the
// default camera and controls until the window closes. WASD/arrows walk,
// Space jumps, the right mouse button orbits, the wheel zooms, Esc quits.
int playPlace(const std::filesystem::path& path, const PlayOptions& options = {});

// The same session without a window, walking forward and jumping now and
// then for `seconds`; prints where the character goes and frame times.
struct SimulateOptions {
    float seconds = 5.0f;
    bool scripts = false; // run the place's scripts too
    bool idle = false;    // stand still instead of walking and jumping
    // With scripts: a copy of the first Tool by this name goes into the
    // Backpack; it's equipped after half a second and clicked at one.
    std::string give;
    int players = 1; // with scripts: the others join the server without clients
    // With scripts: start a battle royale round a second in (what /gogogo
    // does), report the delivery vehicle as it flies and press Space to jump
    // out once it's Ready.
    bool goGoGo = false;
    // With scripts: six seconds in, stand the character on the first
    // VehicleSeat in the Workspace (it sits down), hold W for five seconds,
    // then jump out, reporting the vehicle as it goes.
    bool drive = false;
    // Keep Lunarstone's map up to date as in --play (without drawing it)
    // and report what that costs and does.
    bool map = false;
    LocalUser user;
};
int simulatePlace(const std::filesystem::path& path, const SimulateOptions& options);

// Collision check on a real map: finds an anchored MeshPart whose name
// starts with `prefix` (a tree), casts rays at it, tests points inside its
// box, and walks the character into it with and without its collision data.
int treeTest(const std::filesystem::path& path, std::string_view prefix);

// Where a character spawns: on top of the first enabled SpawnLocation,
// keeping its facing, else above the origin. The CFrame is for the
// character's feet.
lunar::CFrame spawnPoint(lunar::Workspace& workspace);

// LUNAR_PROFILE: 0 when unset, 1 for where the scripts' frames go by phase,
// 2 for the busiest scripts too; and the printing of it, to stderr.
int scriptProfileLevel();
void printScriptProfile(const lunar::SimulationProfile& profile, int level);
