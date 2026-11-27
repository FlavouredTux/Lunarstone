#pragma once

// Services: one of each per game, made on demand by GetService. Most have no
// behavior yet; they exist so places load whole and GetService finds them.
// Included through <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/Core.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar {

class Humanoid;
class TextBox;

class Lighting final : public InstanceImpl<Lighting, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(Color3, ambient, Ambient, Color3::fromRGB(70, 70, 70))
    LUNAR_FIELD(float, brightness, Brightness, 3.0f)
    LUNAR_FIELD(Color3, colorShiftBottom, ColorShift_Bottom)
    LUNAR_FIELD(Color3, colorShiftTop, ColorShift_Top)
    LUNAR_FIELD(float, environmentDiffuseScale, EnvironmentDiffuseScale, 1.0f)
    LUNAR_FIELD(float, environmentSpecularScale, EnvironmentSpecularScale, 1.0f)
    LUNAR_FIELD(float, exposureCompensation, ExposureCompensation, 0.0f)
    LUNAR_FIELD(Color3, fogColor, FogColor, Color3::fromRGB(192, 192, 192))
    LUNAR_FIELD(float, fogEnd, FogEnd, 100000.0f)
    LUNAR_FIELD(float, fogStart, FogStart, 0.0f)
    LUNAR_FIELD(float, geographicLatitude, GeographicLatitude, 0.0f)
    LUNAR_FIELD(bool, globalShadows, GlobalShadows, true)
    LUNAR_FIELD(Color3, outdoorAmbient, OutdoorAmbient, Color3::fromRGB(70, 70, 70))
    LUNAR_FIELD(bool, outlines, Outlines, false)
    LUNAR_FIELD(float, shadowSoftness, ShadowSoftness, 0.2f)
    LUNAR_FIELD(Enum::Technology, technology, Technology, Enum::Technology::ShadowMap)
    LUNAR_FIELD(bool, prioritizeLightingQuality, PrioritizeLightingQuality, false)
    LUNAR_FIELD(Enum::LightingStyle, lightingStyle, LightingStyle, Enum::LightingStyle::Realistic)

    // Hours since midnight, 0 to 24. TimeOfDay is the same as "HH:MM:SS".
    LUNAR_PROPERTY() double clockTime() const { return clockTime_; }
    void setClockTime(double hours);
    LUNAR_PROPERTY() std::string timeOfDay() const;
    // Accepts "H", "H:M" or "H:M:S", wrapping out-of-range values like Roblox.
    void setTimeOfDay(std::string time);

private:
    double clockTime_ = 14.0;
};

class StarterPlayer final : public InstanceImpl<StarterPlayer, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, allowCustomAnimations, AllowCustomAnimations, true)
    LUNAR_FIELD(bool, autoJumpEnabled, AutoJumpEnabled, true)
    LUNAR_FIELD(float, cameraMaxZoomDistance, CameraMaxZoomDistance, 128.0f)
    LUNAR_FIELD(float, cameraMinZoomDistance, CameraMinZoomDistance, 0.5f)
    LUNAR_FIELD(Enum::CameraMode, cameraMode, CameraMode, Enum::CameraMode::Classic)
    LUNAR_FIELD(bool, characterBreakJointsOnDeath, CharacterBreakJointsOnDeath, true)
    LUNAR_FIELD(float, characterJumpHeight, CharacterJumpHeight, 7.2f)
    LUNAR_FIELD(float, characterJumpPower, CharacterJumpPower, 50.0f)
    LUNAR_FIELD(float, characterMaxSlopeAngle, CharacterMaxSlopeAngle, 89.0f)
    LUNAR_FIELD(bool, characterUseJumpPower, CharacterUseJumpPower, false)
    LUNAR_FIELD(float, characterWalkSpeed, CharacterWalkSpeed, 16.0f)
    LUNAR_FIELD(bool, classicDeath, ClassicDeath, true)
    LUNAR_FIELD(bool, createDefaultPlayerModule, CreateDefaultPlayerModule, true)
    LUNAR_FIELD(Enum::DevCameraOcclusionMode, devCameraOcclusionMode, DevCameraOcclusionMode,
                Enum::DevCameraOcclusionMode::Zoom)
    LUNAR_FIELD(Enum::DevComputerCameraMovementMode, devComputerCameraMovementMode, DevComputerCameraMovementMode,
                Enum::DevComputerCameraMovementMode::UserChoice)
    LUNAR_FIELD(Enum::DevComputerMovementMode, devComputerMovementMode, DevComputerMovementMode,
                Enum::DevComputerMovementMode::UserChoice)
    LUNAR_FIELD(Enum::DevTouchCameraMovementMode, devTouchCameraMovementMode, DevTouchCameraMovementMode,
                Enum::DevTouchCameraMovementMode::UserChoice)
    LUNAR_FIELD(Enum::DevTouchMovementMode, devTouchMovementMode, DevTouchMovementMode,
                Enum::DevTouchMovementMode::UserChoice)
    LUNAR_FIELD(bool, enableMouseLockOption, EnableMouseLockOption, true)
    LUNAR_FIELD(float, healthDisplayDistance, HealthDisplayDistance, 100.0f)
    LUNAR_FIELD(float, nameDisplayDistance, NameDisplayDistance, 100.0f)
    LUNAR_FIELD(bool, loadCharacterAppearance, LoadCharacterAppearance, true)
    LUNAR_FIELD(bool, userEmotesEnabled, UserEmotesEnabled, true)
    LUNAR_FIELD(bool, ragdollDeath, RagdollDeath, false)

    // What LoadCharacter does to a fresh character: the Character* settings
    // (and the display distances) replace the Humanoid's defaults, so a game's
    // WalkSpeed and jump apply from the first frame.
    void applyCharacterSettings(Humanoid& humanoid) const;

    // Game Settings > Avatar: what player avatars are made into.
    LUNAR_FIELD(Enum::GameAvatarType, gameSettingsAvatar, GameSettingsAvatar, Enum::GameAvatarType::R15)
    LUNAR_FIELD(Enum::R15CollisionType, gameSettingsR15Collision, GameSettingsR15Collision,
                Enum::R15CollisionType::OuterBox)
    LUNAR_FIELD(NumberRange, gameSettingsScaleRangeBodyType, GameSettingsScaleRangeBodyType, NumberRange(0.0f, 1.0f))
    LUNAR_FIELD(NumberRange, gameSettingsScaleRangeHead, GameSettingsScaleRangeHead, NumberRange(0.95f, 1.0f))
    LUNAR_FIELD(NumberRange, gameSettingsScaleRangeHeight, GameSettingsScaleRangeHeight, NumberRange(0.9f, 1.05f))
    LUNAR_FIELD(NumberRange, gameSettingsScaleRangeProportion, GameSettingsScaleRangeProportion,
                NumberRange(0.0f, 1.0f))
    LUNAR_FIELD(NumberRange, gameSettingsScaleRangeWidth, GameSettingsScaleRangeWidth, NumberRange(0.7f, 1.0f))
    // Asset overrides per body part and clothing; 0 keeps the player's own.
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdFace, GameSettingsAssetIDFace, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdHead, GameSettingsAssetIDHead, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdTorso, GameSettingsAssetIDTorso, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdLeftArm, GameSettingsAssetIDLeftArm, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdRightArm, GameSettingsAssetIDRightArm, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdLeftLeg, GameSettingsAssetIDLeftLeg, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdRightLeg, GameSettingsAssetIDRightLeg, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdShirt, GameSettingsAssetIDShirt, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdPants, GameSettingsAssetIDPants, 0)
    LUNAR_FIELD(std::int64_t, gameSettingsAssetIdTeeShirt, GameSettingsAssetIDTeeShirt, 0)
};

// Script containers StarterPlayer copies into each player and character.
class StarterPlayerScripts : public InstanceImpl<StarterPlayerScripts, Instance, ClassKind::NotCreatable> {};
class StarterCharacterScripts final
    : public InstanceImpl<StarterCharacterScripts, StarterPlayerScripts, ClassKind::NotCreatable> {};

// Holds ScreenGuis: StarterGui (the template) and each player's PlayerGui.
class BasePlayerGui : public InstanceImpl<BasePlayerGui, Instance, ClassKind::Abstract> {};

// The client's own GUI, out of the game's reach in Roblox: the chat bubbles
// (app/LiveChat's ChatBubbles) live here. The game window draws it over PlayerGui.
class CoreGui final : public InstanceImpl<CoreGui, BasePlayerGui, ClassKind::Service> {};

// The client's own Lua packages (React, the app's chunks), out of the game's
// reach like CoreGui; UniversalApp.rbxm fills it for the Lua app.
class CorePackages final : public InstanceImpl<CorePackages, Instance, ClassKind::Service> {};

class StarterGui final : public InstanceImpl<StarterGui, BasePlayerGui, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, resetPlayerGuiOnSpawn, ResetPlayerGuiOnSpawn, true)
    LUNAR_FIELD(bool, showDevelopmentGui, ShowDevelopmentGui, true)
    LUNAR_FIELD(Enum::ScreenOrientation, screenOrientation, ScreenOrientation, Enum::ScreenOrientation::LandscapeSensor)

    // StarterGui:SetCoreGuiEnabled / GetCoreGuiEnabled: which parts of the
    // core GUI show. All sets every one; asking about All means all are on.
    bool coreGuiEnabled(Enum::CoreGuiType type) const;
    void setCoreGuiEnabled(Enum::CoreGuiType type, bool enabled);

    // SetCore/GetCore communicate with the native core UI through this state.
    // It is local to this client and is not a replicated property bag.
    struct CoreSettings {
        bool topbarEnabled = true;
        bool chatActive = true;
        bool chatBarDisabled = false;
        bool devConsoleVisible = false;
        bool pointsNotificationsActive = true;
        bool badgesNotificationsActive = true;
        bool avatarContextMenuEnabled = false;
        std::optional<UDim2> chatWindowPosition;
        std::optional<UDim2> chatWindowSize;
        Value resetButtonCallback = true;
    };
    CoreSettings& coreSettings() { return coreSettings_; }
    const CoreSettings& coreSettings() const { return coreSettings_; }
    struct SystemChatMessage {
        std::string text;
        Color3 color{1, 1, 1};
    };
    void postSystemChat(SystemChatMessage message);
    std::vector<SystemChatMessage> takeSystemChat();

    struct Notification {
        std::string title;
        std::string text;
        std::string icon;
        double duration = 5;
        std::vector<std::string> buttons;
        std::function<void(std::string)> choose;
    };
    void postNotification(Notification notification);
    std::vector<Notification> takeNotifications();

private:
    std::uint32_t coreGuiDisabled_ = 0; // a bit per CoreGuiType
    CoreSettings coreSettings_;
    std::vector<SystemChatMessage> systemChat_;
    std::vector<Notification> notifications_;
};

class StarterPack final : public InstanceImpl<StarterPack, Instance, ClassKind::Service> {};
class ReplicatedStorage final : public InstanceImpl<ReplicatedStorage, Instance, ClassKind::Service> {};
class ReplicatedFirst final : public InstanceImpl<ReplicatedFirst, Instance, ClassKind::Service> {};
class ServerStorage final : public InstanceImpl<ServerStorage, Instance, ClassKind::Service> {};

class ServerScriptService final : public InstanceImpl<ServerScriptService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, loadStringEnabled, LoadStringEnabled, false)
};

class SoundService final : public InstanceImpl<SoundService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(float, distanceFactor, DistanceFactor, 3.33f)
    LUNAR_FIELD(float, dopplerScale, DopplerScale, 1.0f)
    LUNAR_FIELD(float, rolloffScale, RolloffScale, 1.0f)
    LUNAR_FIELD(bool, respectFilteringEnabled, RespectFilteringEnabled, true)
    LUNAR_FIELD(Enum::ReverbType, ambientReverb, AmbientReverb, Enum::ReverbType::NoReverb)
    // Where sounds are heard from, as SoundService:SetListener sets it.
    LUNAR_FIELD(Enum::ListenerType, listenerType, ListenerType, Enum::ListenerType::Camera)
    LUNAR_FIELD(CFrame, listenerCFrame, ListenerCFrame)
    LUNAR_REF(Instance, listenerObject, ListenerObject)
};

class Teams final : public InstanceImpl<Teams, Instance, ClassKind::Service> {};

class Team final : public InstanceImpl<Team, Instance> {
public:
    LUNAR_FIELD(bool, autoAssignable, AutoAssignable, true)
    LUNAR_FIELD(BrickColor, teamColor, TeamColor, BrickColor{1})
};

class Chat final : public InstanceImpl<Chat, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, bubbleChatEnabled, BubbleChatEnabled, false)
    LUNAR_FIELD(bool, loadDefaultChat, LoadDefaultChat, true)
};

class TextChatService final : public InstanceImpl<TextChatService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(Enum::ChatVersion, chatVersion, ChatVersion, Enum::ChatVersion::TextChatService)
    LUNAR_FIELD(bool, createDefaultCommands, CreateDefaultCommands, true)
    LUNAR_FIELD(bool, createDefaultTextChannels, CreateDefaultTextChannels, true)
};

// A chat channel under TextChatService.TextChannels (RBXGeneral, RBXSystem,
// RBXTeam..., RBXWhisper:...). The server makes the default ones and adds a
// TextSource per player who may read it.
class TextChannel final : public InstanceImpl<TextChannel, Instance> {};

class TextChatCommand final : public InstanceImpl<TextChatCommand, Instance> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, autocompleteVisible, AutocompleteVisible, true)
    LUNAR_FIELD(std::string, primaryAlias, PrimaryAlias)
    LUNAR_FIELD(std::string, secondaryAlias, SecondaryAlias)
};

// A player in a TextChannel. The server replicates UserId as
// UserIdReplicated, and leaves CanSend off the wire where it's true.
class TextSource final : public InstanceImpl<TextSource, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_FIELD(bool, canSend, CanSend, true)
    LUNAR_FIELD(std::int64_t, userId, UserId, 0)
};

// One chat message as the client's scripts see it (TextChannel:SendAsync's
// result, MessageReceived's argument). Text and PrefixText are rich text.
class TextChatMessage final : public InstanceImpl<TextChatMessage, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_FIELD(std::string, messageId, MessageId)
    LUNAR_FIELD(std::string, metadata, Metadata)
    LUNAR_FIELD(std::string, prefixText, PrefixText)
    LUNAR_FIELD(std::string, text, Text)
    LUNAR_FIELD(std::string, translation, Translation)
    LUNAR_FIELD(Enum::TextChatMessageStatus, status, Status, Enum::TextChatMessageStatus::Unknown)
    LUNAR_REF(TextChannel, textChannel, TextChannel)
    LUNAR_REF(TextSource, textSource, TextSource)
};

// What OnIncomingMessage returns to change how a message shows.
class TextChatMessageProperties final : public InstanceImpl<TextChatMessageProperties, Instance> {
public:
    LUNAR_FIELD(std::string, prefixText, PrefixText)
    LUNAR_FIELD(std::string, text, Text)
    LUNAR_FIELD(std::string, translation, Translation)
};

class Debris final : public InstanceImpl<Debris, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(int, maxItems, MaxItems, 1000)
};

class HttpService final : public InstanceImpl<HttpService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, httpEnabled, HttpEnabled, false)
};

class InsertService final : public InstanceImpl<InsertService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, allowClientInsertModels, AllowClientInsertModels, false)
    LUNAR_FIELD(bool, allowInsertFreeModels, AllowInsertFreeModels, false)
};

class DataStoreService final : public InstanceImpl<DataStoreService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, automaticRetry, AutomaticRetry, true)
    LUNAR_FIELD(bool, legacyNamingScheme, LegacyNamingScheme, false)
};

// "Hold E to open": an action a player takes near its parent (a BasePart,
// an Attachment or a Model), shown by PromptSystem on the client. Style
// Default draws Roblox's prompt; Custom leaves that to the game's scripts,
// which hear PromptShown and PromptHidden.
class ProximityPrompt final : public InstanceImpl<ProximityPrompt, Instance> {
public:
    LUNAR_FIELD(std::string, actionText, ActionText, "Interact")
    LUNAR_FIELD(std::string, objectText, ObjectText)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(float, holdDuration, HoldDuration, 0.0f)
    LUNAR_FIELD(Enum::KeyCode, keyboardKeyCode, KeyboardKeyCode, Enum::KeyCode::E)
    LUNAR_FIELD(Enum::KeyCode, gamepadKeyCode, GamepadKeyCode, Enum::KeyCode::ButtonX)
    LUNAR_FIELD(float, maxActivationDistance, MaxActivationDistance, 10.0f)
    LUNAR_FIELD(float, maxIndicatorDistance, MaxIndicatorDistance, 0.0f)
    LUNAR_FIELD(bool, requiresLineOfSight, RequiresLineOfSight, true)
    LUNAR_FIELD(Enum::ProximityPromptExclusivity, exclusivity, Exclusivity,
                Enum::ProximityPromptExclusivity::OnePerButton)
    LUNAR_FIELD(Enum::ProximityPromptStyle, style, Style, Enum::ProximityPromptStyle::Default)
    LUNAR_FIELD(Vector2, uiOffset, UIOffset)
    LUNAR_FIELD(bool, clickablePrompt, ClickablePrompt, true)
    LUNAR_FIELD(bool, autoLocalize, AutoLocalize, true)
    LUNAR_REF(Instance, rootLocalizationTable, RootLocalizationTable)
};

class ProximityPromptService final : public InstanceImpl<ProximityPromptService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(int, maxPromptsVisible, MaxPromptsVisible, 16)
    LUNAR_FIELD(int, maxIndicatorsVisible, MaxIndicatorsVisible, 16)
};

// Which MaterialVariant overrides each built-in material, by name (empty for none).
class MaterialService final : public InstanceImpl<MaterialService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, use2022Materials, Use2022Materials, false)
    LUNAR_FIELD(std::string, asphaltName, AsphaltName, "Asphalt")
    LUNAR_FIELD(std::string, basaltName, BasaltName, "Basalt")
    LUNAR_FIELD(std::string, brickName, BrickName, "Brick")
    LUNAR_FIELD(std::string, cardboardName, CardboardName, "Cardboard")
    LUNAR_FIELD(std::string, carpetName, CarpetName, "Carpet")
    LUNAR_FIELD(std::string, ceramicTilesName, CeramicTilesName, "CeramicTiles")
    LUNAR_FIELD(std::string, clayRoofTilesName, ClayRoofTilesName, "ClayRoofTiles")
    LUNAR_FIELD(std::string, cobblestoneName, CobblestoneName, "Cobblestone")
    LUNAR_FIELD(std::string, concreteName, ConcreteName, "Concrete")
    LUNAR_FIELD(std::string, corrodedMetalName, CorrodedMetalName, "CorrodedMetal")
    LUNAR_FIELD(std::string, crackedLavaName, CrackedLavaName, "CrackedLava")
    LUNAR_FIELD(std::string, diamondPlateName, DiamondPlateName, "DiamondPlate")
    LUNAR_FIELD(std::string, fabricName, FabricName, "Fabric")
    LUNAR_FIELD(std::string, foilName, FoilName, "Foil")
    LUNAR_FIELD(std::string, glacierName, GlacierName, "Glacier")
    LUNAR_FIELD(std::string, graniteName, GraniteName, "Granite")
    LUNAR_FIELD(std::string, grassName, GrassName, "Grass")
    LUNAR_FIELD(std::string, groundName, GroundName, "Ground")
    LUNAR_FIELD(std::string, iceName, IceName, "Ice")
    LUNAR_FIELD(std::string, leafyGrassName, LeafyGrassName, "LeafyGrass")
    LUNAR_FIELD(std::string, leatherName, LeatherName, "Leather")
    LUNAR_FIELD(std::string, limestoneName, LimestoneName, "Limestone")
    LUNAR_FIELD(std::string, marbleName, MarbleName, "Marble")
    LUNAR_FIELD(std::string, metalName, MetalName, "Metal")
    LUNAR_FIELD(std::string, mudName, MudName, "Mud")
    LUNAR_FIELD(std::string, pavementName, PavementName, "Pavement")
    LUNAR_FIELD(std::string, pebbleName, PebbleName, "Pebble")
    LUNAR_FIELD(std::string, plasterName, PlasterName, "Plaster")
    LUNAR_FIELD(std::string, plasticName, PlasticName, "Plastic")
    LUNAR_FIELD(std::string, rockName, RockName, "Rock")
    LUNAR_FIELD(std::string, roofShinglesName, RoofShinglesName, "RoofShingles")
    LUNAR_FIELD(std::string, rubberName, RubberName, "Rubber")
    LUNAR_FIELD(std::string, saltName, SaltName, "Salt")
    LUNAR_FIELD(std::string, sandName, SandName, "Sand")
    LUNAR_FIELD(std::string, sandstoneName, SandstoneName, "Sandstone")
    LUNAR_FIELD(std::string, slateName, SlateName, "Slate")
    LUNAR_FIELD(std::string, smoothPlasticName, SmoothPlasticName, "SmoothPlastic")
    LUNAR_FIELD(std::string, snowName, SnowName, "Snow")
    LUNAR_FIELD(std::string, woodName, WoodName, "Wood")
    LUNAR_FIELD(std::string, woodPlanksName, WoodPlanksName, "WoodPlanks")
};

// A look of its own for a built-in material, picked by name: parts of its
// BaseMaterial whose MaterialVariant names it, or all of them when
// MaterialService's <Material>Name does. Its maps tile StudsPerTile studs
// apart; the part's Color tints the colour map.
class MaterialVariant final : public InstanceImpl<MaterialVariant, Instance> {
public:
    LUNAR_FIELD(Enum::Material, baseMaterial, BaseMaterial, Enum::Material::Plastic)
    LUNAR_FIELD(std::string, colorMap, ColorMap)
    LUNAR_FIELD(std::string, normalMap, NormalMap)
    LUNAR_FIELD(std::string, roughnessMap, RoughnessMap)
    LUNAR_FIELD(std::string, metalnessMap, MetalnessMap)
    LUNAR_FIELD(std::string, texturePack, TexturePack)
    LUNAR_FIELD(float, studsPerTile, StudsPerTile, 10.0f)
    LUNAR_FIELD(Enum::MaterialPattern, materialPattern, MaterialPattern, Enum::MaterialPattern::Regular)
    LUNAR_FIELD(Enum::AlphaMode, alphaMode, AlphaMode, Enum::AlphaMode::Overlay)
    LUNAR_FIELD(std::string, emissiveMaskContent, EmissiveMaskContent)
    LUNAR_FIELD(float, emissiveStrength, EmissiveStrength, 1.0f)
    LUNAR_FIELD(Color3, emissiveTint, EmissiveTint, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(std::optional<PhysicalProperties>, customPhysicalProperties, CustomPhysicalProperties)
};

// The MaterialVariant a part of this material draws with: the one its own
// MaterialVariant names, else the one MaterialService names for the material.
// Either has to be in MaterialService (folders count) with that BaseMaterial.
// Null: the built-in look.
const MaterialVariant* findMaterialVariant(const MaterialService& service, Enum::Material material,
                                           std::string_view partVariant = {});

class AssetService final : public InstanceImpl<AssetService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(bool, allowInsertFreeAssets, AllowInsertFreeAssets, false)
};

class AnalyticsService final : public InstanceImpl<AnalyticsService, Instance, ClassKind::Service> {
public:
    LUNAR_FIELD(std::string, apiKey, ApiKey)
};

// UserSettings():GetService("UserGameSettings"), the player's own settings.
// RotationType is the engine's: CameraRelative turns our character to face
// the camera (Humanoid 0x103641a87), whoever sets it (the CameraModule's
// mouse lock, first person, or a game's own Shift Lock defined in Luau).
class UserGameSettings final : public InstanceImpl<UserGameSettings, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_FIELD(Enum::RotationType, rotationType, RotationType, Enum::RotationType::MovementRelative)
};

// What the player's devices can do and how the mouse behaves; the script
// layer feeds it input and the platform layer reads MouseBehavior back.
class UserInputService final : public InstanceImpl<UserInputService, Instance, ClassKind::Service> {
public:
    TextBox* focusedTextBox() const;
    void assignFocusedTextBox(TextBox* box);
    LUNAR_PROPERTY() Enum::PreferredInput preferredInput() const { return Enum::PreferredInput::KeyboardAndMouse; }
    LUNAR_FIELD(Enum::MouseBehavior, mouseBehavior, MouseBehavior, Enum::MouseBehavior::Default)
    LUNAR_FIELD(bool, mouseIconEnabled, MouseIconEnabled, true)
    LUNAR_FIELD(float, mouseDeltaSensitivity, MouseDeltaSensitivity, 1.0f)
    LUNAR_FIELD(std::string, mouseIcon, MouseIcon)
    LUNAR_FIELD(bool, modalEnabled, ModalEnabled, false)
    LUNAR_FIELD(bool, mouseEnabled, MouseEnabled, true)
    LUNAR_FIELD(bool, keyboardEnabled, KeyboardEnabled, true)
    LUNAR_FIELD(bool, touchEnabled, TouchEnabled, false)
    LUNAR_FIELD(bool, gamepadEnabled, GamepadEnabled, false)
    LUNAR_FIELD(bool, accelerometerEnabled, AccelerometerEnabled, false)
    LUNAR_FIELD(bool, gyroscopeEnabled, GyroscopeEnabled, false)
    LUNAR_FIELD(bool, vrEnabled, VREnabled, false)
private:
    Ref<TextBox> focusedTextBox_;
};

// One input event as UserInputService and ContextActionService hand it to scripts.
class InputObject final : public InstanceImpl<InputObject, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_FIELD(Enum::KeyCode, keyCode, KeyCode, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::UserInputType, userInputType, UserInputType, Enum::UserInputType::None)
    LUNAR_FIELD(Enum::UserInputState, userInputState, UserInputState, Enum::UserInputState::None)
    LUNAR_FIELD(Vector3, position, Position)
    LUNAR_FIELD(Vector3, delta, Delta)
};

// clang-format off
class CollectionService final : public InstanceImpl<CollectionService, Instance, ClassKind::Service> {};
class TweenService final : public InstanceImpl<TweenService, Instance, ClassKind::Service> {};
class RunService final : public InstanceImpl<RunService, Instance, ClassKind::Service> {};
class ContextActionService final : public InstanceImpl<ContextActionService, Instance, ClassKind::Service> {};
class GuiService final : public InstanceImpl<GuiService, Instance, ClassKind::Service> {
public:
    LUNAR_PROPERTY() bool menuIsOpen() const { return menuIsOpen_; }
    void assignMenuIsOpen(bool open)
    {
        if (menuIsOpen_ == open)
            return;
        menuIsOpen_ = open;
        changed.fire("MenuIsOpen");
        fireEvent(open ? "MenuOpened" : "MenuClosed");
    }
    // The part of the top bar a game may use (TopbarPlus puts its icons
    // there and reads its Height): under Lunarstone's classic 36-pixel bar,
    // all of it but the health bar's corner on the right. The GUI engine
    // sets it as the screen changes; read-only for scripts.
    LUNAR_PROPERTY() Rect topbarInset() const { return topbarInset_; }
    void assignTopbarInset(Rect inset)
    {
        if (inset == topbarInset_)
            return;
        topbarInset_ = inset;
        changed.fire("TopbarInset");
    }

    // SetGlobalGuiInset(left, top, right, bottom): what an app's own bars
    // (the Lua app's navigation rail) keep out of every ScreenGui that
    // keeps the GUI inset, besides the top bar. None in a game.
    struct Inset {
        float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        friend bool operator==(const Inset&, const Inset&) = default;
    };
    const Inset& globalGuiInset() const { return globalGuiInset_; }
    void assignGlobalGuiInset(Inset inset) { globalGuiInset_ = inset; }

private:
    bool menuIsOpen_ = false;
    Rect topbarInset_{{0.0f, 0.0f}, {0.0f, 36.0f}};
    Inset globalGuiInset_;
};
// Assets load in the background; none are ever waiting in a queue a
// script could watch, so loading screens that count it down move on.
class ContentProvider final : public InstanceImpl<ContentProvider, Instance, ClassKind::Service> {
public:
    LUNAR_PROPERTY() int requestQueueSize() const { return 0; }
    // The website every web address is made from (the Lua app's API calls).
    LUNAR_PROPERTY() std::string_view baseUrl() const { return "https://www.roblox.com/"; }
};
class PhysicsService final : public InstanceImpl<PhysicsService, Instance, ClassKind::Service> {};
class PathfindingService final : public InstanceImpl<PathfindingService, Instance, ClassKind::Service> {};
class TeleportService final : public InstanceImpl<TeleportService, Instance, ClassKind::Service> {};
class MarketplaceService final : public InstanceImpl<MarketplaceService, Instance, ClassKind::Service> {};
class GamePassService final : public InstanceImpl<GamePassService, Instance, ClassKind::Service> {};
class BadgeService final : public InstanceImpl<BadgeService, Instance, ClassKind::Service> {};

class LocalizationService final : public InstanceImpl<LocalizationService, Instance, ClassKind::Service> {
public:
    // The client's language and the system's. GUESS: en-us for both; the
    // client takes them from the user's settings and the OS (the Lua app
    // changes the first with SetRobloxLocaleId).
    LUNAR_FIELD(std::string, robloxLocaleId, RobloxLocaleId, "en-us")
    LUNAR_FIELD(std::string, systemLocaleId, SystemLocaleId, "en-us")
};
class LanguageService final : public InstanceImpl<LanguageService, Instance, ClassKind::Service> {};

class TouchInputService final : public InstanceImpl<TouchInputService, Instance, ClassKind::Service> {};
class VRService final : public InstanceImpl<VRService, Instance, ClassKind::Service> {};
class VoiceChatService final : public InstanceImpl<VoiceChatService, Instance, ClassKind::Service> {};
class VideoService final : public InstanceImpl<VideoService, Instance, ClassKind::Service> {};
class VideoCaptureService final : public InstanceImpl<VideoCaptureService, Instance, ClassKind::Service> {};
class GamepadService final : public InstanceImpl<GamepadService, Instance, ClassKind::Service> {};
class LuaWebService final : public InstanceImpl<LuaWebService, Instance, ClassKind::Service> {};
class CookiesService final : public InstanceImpl<CookiesService, Instance, ClassKind::Service> {};
class TimerService final : public InstanceImpl<TimerService, Instance, ClassKind::Service> {};
class ScriptService final : public InstanceImpl<ScriptService, Instance, ClassKind::Service> {};
class TestService final : public InstanceImpl<TestService, Instance, ClassKind::Service> {};
class PermissionsService final : public InstanceImpl<PermissionsService, Instance, ClassKind::Service> {};
class PlayerEmulatorService final : public InstanceImpl<PlayerEmulatorService, Instance, ClassKind::Service> {};
class ServiceVisibilityService final : public InstanceImpl<ServiceVisibilityService, Instance, ClassKind::Service> {};
class Selection final : public InstanceImpl<Selection, Instance, ClassKind::Service> {};
class StudioData final : public InstanceImpl<StudioData, Instance, ClassKind::Service> {};
class VirtualInputManager final : public InstanceImpl<VirtualInputManager, Instance, ClassKind::Service> {};
// Services that dedupe shared data (CSG meshes) across instances.
class FlyweightService : public InstanceImpl<FlyweightService, Instance, ClassKind::Abstract> {};
class CSGDictionaryService final : public InstanceImpl<CSGDictionaryService, FlyweightService, ClassKind::Service> {};
class NonReplicatedCSGDictionaryService final
    : public InstanceImpl<NonReplicatedCSGDictionaryService, FlyweightService, ClassKind::Service> {};
class LodDataService final : public InstanceImpl<LodDataService, Instance, ClassKind::Service> {};
class ProcessInstancePhysicsService final
    : public InstanceImpl<ProcessInstancePhysicsService, Instance, ClassKind::Service> {};
class UGCAvatarService final : public InstanceImpl<UGCAvatarService, Instance, ClassKind::Service> {};
class GuidRegistryService final : public InstanceImpl<GuidRegistryService, Instance, ClassKind::Service> {};
class Geometry final : public InstanceImpl<Geometry, Instance, ClassKind::Service> {};
class RenderHooksService final : public InstanceImpl<RenderHooksService, Instance, ClassKind::Service> {};
// clang-format on

} // namespace lunar
