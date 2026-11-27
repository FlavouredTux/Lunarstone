#pragma once

// Players and what each one owns. Nothing here comes from place files; the
// engine makes players as they join. Included through <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/Services.hpp>
#include <lunar/classes/World.hpp>

#include <cstdint>
#include <string>

namespace lunar {

class Player final : public InstanceImpl<Player, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_FIELD(std::int64_t, userId, UserId, 0)
    LUNAR_FIELD(std::string, displayName, DisplayName)
    LUNAR_REF(Model, character, Character)
    LUNAR_REF(Team, team, Team)
    LUNAR_FIELD(BrickColor, teamColor, TeamColor, BrickColor{1})
    LUNAR_FIELD(bool, neutral, Neutral, true)
    LUNAR_REF(Instance, respawnLocation, RespawnLocation)
    LUNAR_FIELD(bool, autoJumpEnabled, AutoJumpEnabled, true)
    LUNAR_FIELD(Enum::CameraMode, cameraMode, CameraMode, Enum::CameraMode::Classic)
    LUNAR_FIELD(float, cameraMaxZoomDistance, CameraMaxZoomDistance, 128.0f)
    LUNAR_FIELD(float, cameraMinZoomDistance, CameraMinZoomDistance, 0.5f)
    LUNAR_FIELD(float, healthDisplayDistance, HealthDisplayDistance, 100.0f)
    LUNAR_FIELD(float, nameDisplayDistance, NameDisplayDistance, 100.0f)
    LUNAR_FIELD(bool, canLoadCharacterAppearance, CanLoadCharacterAppearance, true)
    LUNAR_FIELD(std::int64_t, characterAppearanceId, CharacterAppearanceId, 0)
    // StarterPlayer.EnableMouseLockOption as the server gives it to the
    // player; the PlayerModule's MouseLockController allows shift lock only
    // while it's true.
    LUNAR_FIELD(bool, devEnableMouseLock, DevEnableMouseLock, true)
};

class Players final : public InstanceImpl<Players, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, characterAutoLoads, CharacterAutoLoads, true)
    LUNAR_FIELD(float, respawnTime, RespawnTime, 5.0f)
    LUNAR_FIELD(int, maxPlayers, MaxPlayers, 12)
    LUNAR_FIELD(int, preferredPlayers, PreferredPlayers, 12)
    LUNAR_FIELD(bool, banningEnabled, BanningEnabled, true)
    LUNAR_FIELD(bool, useStrafingAnimations, UseStrafingAnimations, false)

    // The player this client plays as; read-only for scripts.
    LUNAR_PROPERTY() Player* localPlayer() const { return localPlayer_.get(); }
    void assignLocalPlayer(Player* player)
    {
        if (player == localPlayer())
            return;
        localPlayer_.reset(player);
        changed.fire("LocalPlayer");
    }

private:
    Ref<Player> localPlayer_;
};

// Containers each Player gets.
class PlayerGui final : public InstanceImpl<PlayerGui, BasePlayerGui, ClassKind::NotCreatable> {};
// The tools a player carries; a new one comes with every character.
class Backpack final : public InstanceImpl<Backpack, Instance> {};
// A player's own tools, copied into the Backpack on every spawn after StarterPack's.
class StarterGear final : public InstanceImpl<StarterGear, Instance> {};
class PlayerScripts final : public InstanceImpl<PlayerScripts, Instance, ClassKind::NotCreatable> {};

} // namespace lunar
