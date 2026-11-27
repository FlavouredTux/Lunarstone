#pragma once

// Building a player's character the way the client's character loader does:
// the stock rig from the content folder, an Animator, the Animate object with
// its animation sets, body colours, clothing and a HumanoidDescription.

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lunar::avatar {

struct CharacterOptions {
    Enum::HumanoidRigType rigType = Enum::HumanoidRigType::R15;
    std::string name = "Player";
    // Where the HumanoidRootPart goes. The default stands the character on
    // the plane y = 0.
    std::optional<CFrame> rootCFrame;
    // What to wear and how to be built; null means the default avatar.
    const HumanoidDescription* description = nullptr;
    // Where rbxasset:// files and asset ids come from; null makes one from
    // the environment (AssetConfig::fromEnvironment).
    std::shared_ptr<assets::AssetCache> assets;
    // Put on the content folder's default shirt and pants when the
    // description has none.
    bool defaultClothing = true;
    // R15 with the old classic head (characterR15.rbxm) instead of the
    // dynamic head rig the client uses by default.
    bool classicHead = false;
    // Where the description's skipped problems go (applyDescription), if anywhere.
    std::vector<std::string>* problems = nullptr;
};

// A new, unparented character Model: HumanoidRootPart (its PrimaryPart),
// body parts, joints, Humanoid with an Animator, BodyColors, clothing and an
// "Animate" LocalScript holding the default animation sets (what the C++
// AnimateController reads; its Source stays empty). Part CFrames are solved
// from the joints.
std::expected<std::shared_ptr<Model>, std::string> createCharacter(const CharacterOptions& options = {});

// Player:LoadCharacter() for an offline game: a character named after the
// player, parented to the Workspace, set as Player.Character, standing at
// the first SpawnLocation (else the origin). Replaces any old character.
std::expected<std::shared_ptr<Model>, std::string> loadCharacter(Player& player, DataModel& game,
                                                                 CharacterOptions options = {});

// Colours each body part from the character's BodyColors.
void applyBodyColors(Model& character);

// Welds an accessory's Handle to the character (0x10405cb68): to the first
// body attachment named like one of the Handle's, as a Weld "AccessoryWeld"
// with Part0 = Handle, Part1 = the body part, C0 = the Handle attachment's
// CFrame and C1 = the body attachment's; without a match, a "HeadWeld" to the
// head from the accessory's AttachmentPoint. False if it has no Handle.
bool attachAccessory(Model& character, Accoutrement& accessory);

// Applies a HumanoidDescription to a character (Humanoid:ApplyDescription):
// body colours, scales, body parts (a dynamic head as its MeshPart form),
// clothing, face, the animation pack (into the Animate object's sets) and
// accessories. Assets are loaded through the cache (anonymously, else with
// the login); ones that can't be loaded are skipped. Returns the problems it
// skipped over.
std::vector<std::string> applyDescription(Model& character, const HumanoidDescription& description,
                                          assets::AssetCache& assets);

} // namespace lunar::avatar
