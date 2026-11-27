#include "Builder.hpp"

#include "BinaryFormat.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>

namespace lunar::detail {

namespace {

// Names Studio still writes for properties that were renamed, or whose
// serialized form has its own name.
struct Alias {
    std::string_view serialized;
    std::string_view property;
};

constexpr std::array aliases{
    Alias{"size", "Size"},
    Alias{"Color3uint8", "Color"},
    Alias{"shape", "Shape"},
    Alias{"formFactorRaw", "FormFactor"},
    Alias{"MaterialVariantSerialized", "MaterialVariant"},
    Alias{"Velocity", "AssemblyLinearVelocity"},
    Alias{"RotVelocity", "AssemblyAngularVelocity"},
    Alias{"MeshID", "MeshId"},
    Alias{"Health_XML", "Health"},
    Alias{"EmitterSize", "RollOffMinDistance"},
    Alias{"xmlRead_MaxDistance_3", "RollOffMaxDistance"},
    Alias{"MaxFrictionTorqueXml", "MaxFrictionTorque"},
    Alias{"Part0Internal", "Part0"},
    Alias{"Part1Internal", "Part1"},
    Alias{"MaxPlayersInternal", "MaxPlayers"},
    Alias{"PreferredPlayersInternal", "PreferredPlayers"},
    Alias{"Use2022MaterialsXml", "Use2022Materials"},
    // The Content-typed successors of the asset-id strings (newer files and the
    // network); the first one the class has wins.
    Alias{"MeshContent", "MeshId"},
    Alias{"TextureContent", "TextureID"},
    Alias{"TextureContent", "Texture"},
    Alias{"TextureContent", "TextureId"}, // a SpecialMesh's (FileMesh spells it with a small d)
    Alias{"AnimationContent", "AnimationId"},
    Alias{"AudioContent", "SoundId"},
    Alias{"ShirtTemplateContent", "ShirtTemplate"},
    Alias{"PantsTemplateContent", "PantsTemplate"},
    Alias{"ImageContent", "Image"},
    Alias{"ColorMapContent", "ColorMap"}, // SurfaceAppearance's and MaterialVariant's maps, on the network
    Alias{"NormalMapContent", "NormalMap"},
    Alias{"RoughnessMapContent", "RoughnessMap"},
    Alias{"MetalnessMapContent", "MetalnessMap"},
    Alias{"TexturePackContent", "TexturePack"},
    Alias{"Content", "AssetId"}, // a union's geometry asset, on the network
    Alias{"UserIdReplicated", "UserId"}, // a TextSource's, on the network
    // A Sound's, on the network: the server's Play and Stop, and where from;
    // and the old names of the roll-off distances and PlaybackSpeed.
    Alias{"PlayingReplicator", "Playing"},
    Alias{"TimePositionReplicator", "TimePosition"},
    Alias{"MinDistance", "RollOffMinDistance"},
    Alias{"MaxDistance", "RollOffMaxDistance"},
    Alias{"Pitch", "PlaybackSpeed"},
};

// Serialized properties a client can do without, skipped without a warning.
// An empty class matches every class; "*" matches every property.
struct Ignored {
    std::string_view className;
    std::string_view property;
};

constexpr std::array ignored{
    // Studio bookkeeping: package and collaboration ids, script GUIDs.
    Ignored{"", "SourceAssetId"},
    Ignored{"", "HistoryId"},
    Ignored{"", "UniqueId"},
    Ignored{"LuaSourceContainer", "ScriptGuid"},
    Ignored{"StudioData", "*"},
    Ignored{"PlayerEmulatorService", "*"},
    Ignored{"TestService", "*"},
    Ignored{"ServiceVisibilityService", "*"},
    Ignored{"KeyframeSequence", "GuidBinaryString"},
    Ignored{"Decal", "TexturePackMetadata"},
    Ignored{"Decal", "AutoLocalize"},
    // Script sandboxing, which isn't modeled.
    Ignored{"", "Capabilities"},
    Ignored{"", "DefinesCapabilities"},
    // Legacy part data superseded by newer properties.
    Ignored{"BasePart", "TopParamA"},
    Ignored{"BasePart", "TopParamB"},
    Ignored{"BasePart", "BottomParamA"},
    Ignored{"BasePart", "BottomParamB"},
    Ignored{"BasePart", "FrontParamA"},
    Ignored{"BasePart", "FrontParamB"},
    Ignored{"BasePart", "BackParamA"},
    Ignored{"BasePart", "BackParamB"},
    Ignored{"BasePart", "LeftParamA"},
    Ignored{"BasePart", "LeftParamB"},
    Ignored{"BasePart", "RightParamA"},
    Ignored{"BasePart", "RightParamB"},
    Ignored{"BasePart", "TopSurfaceInput"},
    Ignored{"BasePart", "BottomSurfaceInput"},
    Ignored{"BasePart", "FrontSurfaceInput"},
    Ignored{"BasePart", "BackSurfaceInput"},
    Ignored{"BasePart", "LeftSurfaceInput"},
    Ignored{"BasePart", "RightSurfaceInput"},
    Ignored{"BasePart", "Elasticity"},       // CustomPhysicalProperties
    Ignored{"BasePart", "Friction"},         // CustomPhysicalProperties
    Ignored{"BasePart", "CollisionGroupId"}, // CollisionGroup
    Ignored{"PartOperation", "FormFactor"},
    Ignored{"ParticleEmitter", "VelocitySpread"}, // SpreadAngle
    // Caches Roblox derives from the mesh or the parts, and the engine will too.
    Ignored{"TriangleMeshPart", "LODData"},
    Ignored{"TriangleMeshPart", "AeroMeshData"},
    Ignored{"TriangleMeshPart", "FluidFidelityInternal"},
    Ignored{"TriangleMeshPart", "UnscaledCofm"},
    Ignored{"TriangleMeshPart", "UnscaledVolume"},
    Ignored{"TriangleMeshPart", "UnscaledVolInertiaDiags"},
    Ignored{"TriangleMeshPart", "UnscaledVolInertiaOffDiags"},
    Ignored{"MeshPart", "VertexCount"},
    Ignored{"MeshPart", "HasJointOffset"},
    Ignored{"MeshPart", "JointOffset"},
    Ignored{"MeshPart", "HasSkinnedMesh"},
    Ignored{"MeshPart", "InertiaMigrated"},
    Ignored{"Model", "ModelMeshData"},
    Ignored{"Model", "ModelMeshSize"},
    Ignored{"Model", "ModelMeshCFrame"},
    Ignored{"Model", "ModelInPrimary"},
    Ignored{"Model", "SlimHash"},
    Ignored{"Model", "NeedsPivotMigration"},
    Ignored{"Humanoid", "InternalBodyScale"},
    Ignored{"Humanoid", "InternalHeadScale"},
    Ignored{"Motor6D", "IsAutoJoint"},
    Ignored{"WeldConstraint", "CFrame0"},
    Ignored{"WeldConstraint", "State"},
    Ignored{"Terrain", "PhysicsGrid"}, // legacy voxels
    Ignored{"Terrain", "SmoothVoxelsUpgraded"},
    Ignored{"Terrain", "AcquisitionMethod"},
    // Behavior switches for engine features being rolled out. The engine has
    // one behavior for each of these.
    Ignored{"Workspace", "AnimationWeightedBlendFix"},
    Ignored{"Workspace", "AuthorityMode"},
    Ignored{"Workspace", "AvatarUnificationMode"},
    Ignored{"Workspace", "ClientAnimatorThrottling"},
    Ignored{"Workspace", "EnableSLIMAvatars"},
    Ignored{"Workspace", "ExpSolverEnabled_Replicate"},
    Ignored{"Workspace", "ExplicitAutoJoints"},
    Ignored{"Workspace", "FluidForces"},
    Ignored{"Workspace", "HumanoidOnlySetCollisionsOnStateChange"},
    Ignored{"Workspace", "IKControlConstraintSupport"},
    Ignored{"Workspace", "ImprovedAnimationConstraint"},
    Ignored{"Workspace", "ImprovedPhysicsReplication"},
    Ignored{"Workspace", "InterpolationThrottling"},
    Ignored{"Workspace", "LayeredClothingCacheOptimizations"},
    Ignored{"Workspace", "LuauTypeCheckMode"},
    Ignored{"Workspace", "MeshPartHeadsAndAccessories"},
    Ignored{"Workspace", "MeshStreamingAndImprovedLods"},
    Ignored{"Workspace", "ModelStreamingBehavior"},
    Ignored{"Workspace", "MoverConstraintRootBehavior"},
    Ignored{"Workspace", "NextGenerationReplication"},
    Ignored{"Workspace", "PathfindingUseImprovedSearch"},
    Ignored{"Workspace", "PGSPhysicsSolverEnabled"},
    Ignored{"Workspace", "PhysicsImprovedSleep"},
    Ignored{"Workspace", "PhysicsSteppingMethod"},
    Ignored{"Workspace", "PlayerCharacterDestroyBehavior"},
    Ignored{"Workspace", "PlayerScriptsUseInputActionSystem"},
    Ignored{"Workspace", "PredictiveStreamingMode"},
    Ignored{"Workspace", "PrimalPhysicsSolver"},
    Ignored{"Workspace", "RejectCharacterDeletions"},
    Ignored{"Workspace", "RenderingCacheOptimizations"},
    Ignored{"Workspace", "ReplicateInstanceDestroySetting"},
    Ignored{"Workspace", "Retargeting"},
    Ignored{"Workspace", "SandboxedInstanceMode"},
    Ignored{"Workspace", "SignalBehavior"},
    Ignored{"Workspace", "SignalBehavior2"},
    Ignored{"Workspace", "StreamingIntegrityMode"},
    Ignored{"Workspace", "StreamingPauseMode"},
    Ignored{"Workspace", "StreamOutBehavior"},
    Ignored{"Workspace", "TerrainWeldsFixed"},
    Ignored{"Workspace", "TouchEventsUseCollisionGroups"},
    Ignored{"Workspace", "UseFixedSimulation"},
    Ignored{"Workspace", "UseNewLuauTypeSolver"},
    Ignored{"Workspace", "ValidateEnabledProximityPrompt"},
    Ignored{"Lighting", "ExtendLightRangeTo120"},
    Ignored{"SoundService", "AudioApiByDefault"},
    Ignored{"SoundService", "AudioPlayerVolumeFix"},
    Ignored{"SoundService", "CharacterSoundsUseNewApi"},
    Ignored{"SoundService", "IsNewExpForAudioApiByDefault"},
    Ignored{"SoundService", "VolumetricAudio"},
    Ignored{"SoundService", "DefaultListenerLocation"},
    Ignored{"SoundService", "AcousticSimulationEnabled"},
    Ignored{"SoundService", "DiffractionEnabled"},
    Ignored{"SoundService", "OcclusionEnabled"},
    Ignored{"SoundService", "ReverbEnabled"},
    Ignored{"StarterPlayer", "AvatarJointUpgrade_SerializedRollout"},
    Ignored{"StarterPlayer", "EnableDynamicHeads"},
    Ignored{"StarterPlayer", "LoadCharacterLayeredClothing"},
    Ignored{"StarterPlayer", "LuaCharacterController"},
    Ignored{"StarterPlayer", "PlayerModuleStatus"},
    Ignored{"StarterGui", "ClipsDescendantsSupportsRotation"},
    Ignored{"StarterGui", "RtlTextSupport"},
    Ignored{"StarterGui", "VirtualCursorMode"},
    Ignored{"StarterGui", "StudioDefaultStyleSheet"},
    Ignored{"StarterGui", "StudioInsertWidgetLayerCollectorAutoLinkStyleSheet"},
    Ignored{"TextChatService", "ChatTranslationFTUXShown"},
    Ignored{"TextChatService", "ChatTranslationToggleEnabled"},
    Ignored{"TextChatService", "EnableProtectedChat"},
    Ignored{"TextChatService", "HasSeenDeprecationDialog"},
    Ignored{"TextChatService", "IsLegacyChatDisabled"},
    Ignored{"Chat", "IsAutoMigrated"},
    Ignored{"VoiceChatService", "*"},
    // Avatar internals Roblox recomputes.
    Ignored{"FaceControls", "InternalOverrideFACSData"},
    Ignored{"WrapTarget", "HSRData"},
    Ignored{"WrapTarget", "HSRMeshIdData"},
    Ignored{"WrapTarget", "TemporaryCageMeshId"},
    // Serialized camera of a ViewportFrame; the CurrentCamera child is what counts.
    Ignored{"ViewportFrame", "CameraCFrame"},
    Ignored{"ViewportFrame", "CameraFieldOfView"},
    Ignored{"GuiObject", "SizeFromContents"},
    Ignored{"AnimationRigData", "*"},
    Ignored{"VRService", "*"},
};

bool isIgnored(const Instance& instance, std::string_view serializedName)
{
    return std::ranges::any_of(ignored, [&](const Ignored& entry) {
        return (entry.property == "*" || entry.property == serializedName)
               && (entry.className.empty() || instance.isA(entry.className));
    });
}

std::string_view blob(const Value& value)
{
    if (auto* s = std::get_if<std::string>(&value))
        return *s;
    throw Error(std::format("binary data expected, got {}", typeName(value)));
}

// Serialized properties that aren't properties at runtime, but fill in some
// other state. They only exist for the loaders.
// A BasePart's CollisionGroup on the network [live]: the group's id as one
// byte, then its name, which the server leaves out once it has sent it (the game'
// crouch blockers arrive as "\x09" alone). The id alone is kept as it came,
// and Workspace::collisionGroupsCollide reads it as that id.
void setCollisionGroupReplicate(Instance& instance, const Value& value)
{
    const std::string_view text = blob(value);
    if (text.empty())
        return;
    auto& part = static_cast<BasePart&>(instance);
    if (static_cast<unsigned char>(text.front()) >= 0x20)
        part.setCollisionGroup(std::string(text)); // a name alone
    else
        part.setCollisionGroup(std::string(text.size() > 1 ? text.substr(1) : text));
}

void setAttributesSerialize(Instance& instance, const Value& value)
{
    auto attributes = decodeAttributes(blob(value));
    if (!attributes)
        throw Error(attributes.error());
    std::string firstError;
    for (const auto& [name, attribute] : *attributes) {
        try {
            instance.setAttribute(name, attribute);
        } catch (const Error& e) {
            if (firstError.empty())
                firstError = e.what();
        }
    }
    if (!firstError.empty())
        throw Error(firstError);
}

void setTags(Instance& instance, const Value& value)
{
    for (const std::string& tag : decodeTags(blob(value)))
        instance.addTag(tag);
}

// The tags as files save them: each one's name, a zero byte between them.
Value getTags(const Instance& instance)
{
    std::string bytes;
    for (const std::string& tag : instance.tags()) {
        if (!bytes.empty())
            bytes.push_back('\0');
        bytes += tag;
    }
    return Value(std::move(bytes));
}

template <auto Decode>
void setCollisionGroups(Instance& instance, const Value& value)
{
    auto groups = Decode(blob(value));
    if (!groups)
        throw Error(groups.error());
    if (!groups->empty())
        static_cast<Workspace&>(instance).setCollisionGroups(std::move(*groups));
}

void setSmoothGrid(Instance& instance, const Value& value)
{
    auto chunks = decodeSmoothGrid(blob(value));
    if (!chunks)
        throw Error(chunks.error());
    static_cast<Terrain&>(instance).setChunks(std::move(*chunks));
}

void setMaterialColors(Instance& instance, const Value& value)
{
    auto colors = decodeMaterialColors(blob(value));
    if (!colors)
        throw Error(colors.error());
    auto& terrain = static_cast<Terrain&>(instance);
    for (std::size_t i = 2; i < colors->size(); ++i) // Air and Water have no color
        terrain.setMaterialColor(Terrain::materials[i], (*colors)[i]);
}

// Collision blobs: shared when they come from a SharedString, copied once
// otherwise (PhysicsData is a plain string column in binary files).
void setPhysicalConfigDataShared(Instance& instance, SharedBytes data)
{
    static_cast<TriangleMeshPart&>(instance).setPhysicalConfigData(std::move(data));
}

void setPhysicsDataShared(Instance& instance, SharedBytes data)
{
    static_cast<TriangleMeshPart&>(instance).setPhysicsData(std::move(data));
}

template <auto SetShared>
void setBlob(Instance& instance, const Value& value)
{
    SetShared(instance, std::make_shared<const std::string>(blob(value)));
}

struct SharedOnly {
    std::string_view className;
    std::string_view name;
    Builder::SharedSetter set;
};

constexpr std::array sharedOnly{
    SharedOnly{"TriangleMeshPart", "PhysicalConfigData", &setPhysicalConfigDataShared},
    SharedOnly{"TriangleMeshPart", "PhysicsData", &setPhysicsDataShared},
};

struct SerializedOnly {
    std::string_view className; // applies to this class and its subclasses
    Property property;
};

constexpr std::array serializedOnly{
    SerializedOnly{"Instance", {"AttributesSerialize", "string", nullptr, &setAttributesSerialize}},
    // The same attributes as an instance arrives on the network [live].
    SerializedOnly{"Instance", {"AttributesReplicate", "string", nullptr, &setAttributesSerialize}},
    SerializedOnly{"BasePart", {"CollisionGroupReplicate", "string", nullptr, &setCollisionGroupReplicate}},
    SerializedOnly{"Instance", {"Tags", "string", &getTags, &setTags}},
    SerializedOnly{"Workspace", {"CollisionGroups", "string", nullptr, &setCollisionGroups<&decodeCollisionGroups>}},
    SerializedOnly{"Workspace",
                   {"CollisionGroupData", "string", nullptr, &setCollisionGroups<&decodeCollisionGroupData>}},
    SerializedOnly{"Terrain", {"SmoothGrid", "string", nullptr, &setSmoothGrid}},
    SerializedOnly{"Terrain", {"MaterialColors", "string", nullptr, &setMaterialColors}},
    SerializedOnly{"TriangleMeshPart",
                   {"PhysicalConfigData", "string", nullptr, &setBlob<&setPhysicalConfigDataShared>}},
    SerializedOnly{"TriangleMeshPart", {"PhysicsData", "string", nullptr, &setBlob<&setPhysicsDataShared>}},
};

} // namespace

void Builder::warnOnce(std::string message)
{
    if (warned_.insert(message).second)
        warnings.push_back(std::move(message));
}

InstancePtr Builder::create(std::string_view className)
{
    const ClassInfo* info = ClassRegistry::find(className);
    if (info && info->factory)
        return info->factory();
    // Studio saves some of its own objects (FilteredSelection) as plain
    // "Instance"; there's nothing more to make of them.
    if (className == "Instance")
        return std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
    warnOnce(std::format("class {} isn't implemented yet, loaded as a placeholder", className));
    return std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
}

const Property* Builder::settableProperty(const Instance& instance, std::string_view serializedName)
{
    for (const auto& [className, special] : serializedOnly)
        if (special.name == serializedName && instance.isA(className))
            return &special;

    const Property* property = instance.classInfo().findProperty(serializedName);
    for (const Alias& alias : aliases)
        if (!property && alias.serialized == serializedName)
            property = instance.classInfo().findProperty(alias.property);

    if (property && !property->readOnly())
        return property;
    // Placeholders were already reported as a whole.
    if (!dynamic_cast<const UnknownInstance*>(&instance) && !isIgnored(instance, serializedName))
        warnOnce(std::format("{}.{} isn't supported yet", instance.className(), serializedName));
    return nullptr;
}

Builder::SharedSetter Builder::sharedSetter(const Instance& instance, std::string_view serializedName)
{
    for (const SharedOnly& entry : sharedOnly)
        if (entry.name == serializedName && instance.isA(entry.className))
            return entry.set;
    return nullptr;
}

void Builder::assign(Instance& instance, const Property& property, const Value& value)
{
    try {
        property.set(instance, value);
    } catch (const Error& e) {
        warnOnce(std::format("{}.{}: {}", instance.className(), property.name, e.what()));
    }
}

} // namespace lunar::detail

namespace lunar {

const Property* findSerializedProperty(const Instance& instance, std::string_view serializedName)
{
    for (const auto& [className, special] : detail::serializedOnly)
        if (special.name == serializedName && instance.isA(className))
            return &special;
    const Property* property = instance.classInfo().findProperty(serializedName);
    for (const detail::Alias& alias : detail::aliases)
        if (!property && alias.serialized == serializedName)
            property = instance.classInfo().findProperty(alias.property);
    return property && !property->readOnly() ? property : nullptr;
}

} // namespace lunar
