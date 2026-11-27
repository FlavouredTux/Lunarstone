#pragma once

// Models, the Workspace and what lives directly in it, and the DataModel.
// Included through <lunar/Classes.hpp>.

#include <lunar/classes/Parts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar {

class Model : public InstanceImpl<Model, PVInstance> {
public:
    LUNAR_REF(BasePart, primaryPart, PrimaryPart)
    LUNAR_FIELD(Enum::ModelLevelOfDetail, levelOfDetail, LevelOfDetail, Enum::ModelLevelOfDetail::Automatic)
    LUNAR_FIELD(Enum::ModelStreamingMode, modelStreamingMode, ModelStreamingMode, Enum::ModelStreamingMode::Default)
    // Model:GetScale(); serialized as ScaleFactor.
    LUNAR_FIELD(float, scaleFactor, ScaleFactor, 1.0f)
    // The pivot used when there's no PrimaryPart; nil until one is set.
    LUNAR_FIELD(std::optional<CFrame>, worldPivotData, WorldPivotData)

    // The model's pivot: PrimaryPart's pivot, else WorldPivotData, else the
    // center of its parts' bounding box.
    LUNAR_PROPERTY() CFrame worldPivot() const { return getPivot(); }
    void setWorldPivot(CFrame pivot) { setWorldPivotData(pivot); }

    CFrame getPivot() const;
    // Moves every descendant part so the pivot lands on `pivot`.
    void pivotTo(const CFrame& pivot);
    // Model:ScaleTo: everything in it grows by scale / ScaleFactor about the
    // pivot, and ScaleFactor becomes `scale` (positive).
    void scaleTo(float scale);
    // Model:GetBoundingBox(): oriented like the pivot, enclosing every
    // descendant part. Identity and zero size without parts.
    std::pair<CFrame, Vector3> getBoundingBox() const;
    Vector3 getExtentsSize() const { return getBoundingBox().second; }
};

// Runs its scripts in parallel in Roblox.
class Actor final : public InstanceImpl<Actor, Model> {};

// The parts of a ViewportFrame, in Roblox a world of their own for raycasts
// and physics; here they only draw.
class WorldModel final : public InstanceImpl<WorldModel, Model> {};

class WorldRoot : public InstanceImpl<WorldRoot, Model, ClassKind::Abstract> {};

class Camera final : public InstanceImpl<Camera, PVInstance> {
public:
    LUNAR_FIELD(CFrame, cframe, CFrame, CFrame(0.0f, 20.0f, 20.0f))
    LUNAR_FIELD(CFrame, focus, Focus)
    LUNAR_FIELD(float, fieldOfView, FieldOfView, 70.0f)
    LUNAR_FIELD(Enum::FieldOfViewMode, fieldOfViewMode, FieldOfViewMode, Enum::FieldOfViewMode::Vertical)
    LUNAR_FIELD(Enum::CameraType, cameraType, CameraType, Enum::CameraType::Fixed)
    LUNAR_REF(Instance, cameraSubject, CameraSubject)
    LUNAR_FIELD(bool, headLocked, HeadLocked, true)
    LUNAR_FIELD(float, headScale, HeadScale, 1.0f)
    LUNAR_FIELD(bool, vrTiltAndRollEnabled, VRTiltAndRollEnabled, false)

    // The window size in pixels, kept up to date by the renderer; read-only for scripts.
    LUNAR_PROPERTY() Vector2 viewportSize() const { return viewportSize_; }
    void updateViewportSize(Vector2 size) { setProperty(viewportSize_, size, "ViewportSize"); }
    LUNAR_PROPERTY() float nearPlaneZ() const { return -0.1f; }

private:
    Vector2 viewportSize_{800.0f, 600.0f};
};

// One terrain cell, a 4x4x4 stud cube.
struct TerrainVoxel {
    Enum::Material material = Enum::Material::Air;
    std::uint8_t occupancy = 0; // how full the cell is, as (occupancy + 1) / 256; Air is empty
    std::uint8_t water = 0;     // water filling the rest of a partly solid cell, same scale; 0 for none

    float occupancyFraction() const
    {
        return material == Enum::Material::Air ? 0.0f : static_cast<float>(occupancy + 1) / 256.0f;
    }

    friend bool operator==(const TerrainVoxel&, const TerrainVoxel&) = default;
};

// A 32x32x32 cell block of terrain, run-length encoded the way places store it.
struct TerrainChunk {
    static constexpr int size = 32;

    // Packed: 6 bytes, as big maps have tens of millions of runs.
    struct Run {
        std::uint16_t count = 0;
        std::uint8_t material = 0; // index into Terrain::materials
        std::uint8_t occupancy = 0;
        std::uint8_t water = 0;

        TerrainVoxel voxel() const;
        friend bool operator==(const Run&, const Run&) = default;
    };

    // In chunks: this one covers cells position * 32 up to (position + 1) * 32.
    std::array<int, 3> position{};
    // All size^3 cells, X fastest, then Z, then Y.
    std::vector<Run> runs;

    // Local cell coordinates, 0 to 31.
    TerrainVoxel voxel(int x, int y, int z) const;
};

// Voxel terrain. Cell (0, 0, 0) spans studs 0 to 4 on each axis.
class Terrain final : public InstanceImpl<Terrain, BasePart, ClassKind::NotCreatable> {
public:
    // The materials terrain can use, in the order place files number them.
    static constexpr std::array<Enum::Material, 23> materials{
        Enum::Material::Air,      Enum::Material::Water,       Enum::Material::Grass,    Enum::Material::Slate,
        Enum::Material::Concrete, Enum::Material::Brick,       Enum::Material::Sand,     Enum::Material::WoodPlanks,
        Enum::Material::Rock,     Enum::Material::Glacier,     Enum::Material::Snow,     Enum::Material::Sandstone,
        Enum::Material::Mud,      Enum::Material::Basalt,      Enum::Material::Ground,   Enum::Material::CrackedLava,
        Enum::Material::Asphalt,  Enum::Material::Cobblestone, Enum::Material::Ice,      Enum::Material::LeafyGrass,
        Enum::Material::Salt,     Enum::Material::Limestone,   Enum::Material::Pavement,
    };

    LUNAR_FIELD(Color3, waterColor, WaterColor, Color3::fromRGB(12, 84, 92))
    LUNAR_FIELD(float, waterReflectance, WaterReflectance, 1.0f)
    LUNAR_FIELD(float, waterTransparency, WaterTransparency, 0.3f)
    LUNAR_FIELD(float, waterWaveSize, WaterWaveSize, 0.15f)
    LUNAR_FIELD(float, waterWaveSpeed, WaterWaveSpeed, 10.0f)
    LUNAR_FIELD(bool, decoration, Decoration, false)
    LUNAR_FIELD(float, grassLength, GrassLength, 0.7f)

    // Chunks holding anything but air; cells outside them are air.
    const std::vector<TerrainChunk>& chunks() const { return chunks_; }
    void setChunks(std::vector<TerrainChunk> chunks);

    // A cube of cells written over what's there: `side` cells a side from
    // cell `origin`, in runs (X fastest, then Z, then Y), none for all air.
    // A server streams terrain this way, 16 cells a side.
    struct Block {
        std::array<int, 3> origin{};
        int side = 0;
        std::vector<TerrainChunk::Run> runs;
    };
    // False (and nothing written) when the runs don't cover the cube.
    bool write(const Block& block);
    // While something reads the chunks on other threads (a renderer's
    // meshers), writes wait in a queue until it applies them.
    void setDeferWrites(bool defer) { deferWrites_ = defer; }
    bool hasQueuedWrites() const { return !queued_.empty(); }
    void applyQueuedWrites();
    // Goes up with every change to the cells.
    std::uint64_t revision() const { return revision_; }
    TerrainVoxel voxelAt(int x, int y, int z) const;
    // The chunk at these chunk coordinates, or null where it's all air.
    const TerrainChunk* findChunk(int x, int y, int z) const;

    // Terrain:GetMaterialColor / SetMaterialColor. Air and Water have none (black).
    Color3 getMaterialColor(Enum::Material material) const;
    void setMaterialColor(Enum::Material material, Color3 color);

private:
    void writeNow(std::span<const Block> blocks);

    std::vector<TerrainChunk> chunks_;
    std::unordered_map<std::uint64_t, std::size_t> chunkIndex_;
    bool deferWrites_ = false;
    std::vector<Block> queued_;
    std::uint64_t revision_ = 0;
    std::array<Color3, materials.size()> materialColors_{
        Color3{},
        Color3{},
        Color3::fromRGB(106, 127, 63),
        Color3::fromRGB(63, 127, 107),
        Color3::fromRGB(127, 102, 63),
        Color3::fromRGB(138, 86, 62),
        Color3::fromRGB(143, 126, 95),
        Color3::fromRGB(139, 109, 79),
        Color3::fromRGB(102, 108, 111),
        Color3::fromRGB(101, 176, 234),
        Color3::fromRGB(195, 199, 218),
        Color3::fromRGB(137, 90, 71),
        Color3::fromRGB(58, 46, 36),
        Color3::fromRGB(30, 30, 37),
        Color3::fromRGB(102, 92, 59),
        Color3::fromRGB(232, 156, 74),
        Color3::fromRGB(115, 123, 107),
        Color3::fromRGB(132, 123, 90),
        Color3::fromRGB(129, 194, 224),
        Color3::fromRGB(115, 132, 74),
        Color3::fromRGB(198, 189, 181),
        Color3::fromRGB(206, 173, 148),
        Color3::fromRGB(148, 148, 140),
    };
};

// A physics collision group. Bit n of mask says whether it collides with group n.
struct CollisionGroup {
    std::string name;
    int id = 0;
    std::uint32_t mask = 0xFFFFFFFF;

    friend bool operator==(const CollisionGroup&, const CollisionGroup&) = default;
};

class Workspace final : public InstanceImpl<Workspace, WorldRoot, ClassKind::Service> {
public:
    // CurrentCamera, first making a Camera child for it if there's none, as
    // Roblox clients do, and putting it in the Workspace if it's nowhere.
    Camera* ensureCurrentCamera();

    // The registered collision groups ("Default" is always id 0).
    const std::vector<CollisionGroup>& collisionGroups() const { return collisionGroups_; }
    void setCollisionGroups(std::vector<CollisionGroup> groups) { collisionGroups_ = std::move(groups); }
    // Unknown names are treated as Default, which collides with everything. A
    // name that is one byte below 0x20 is a group's id, as the network sends a
    // group whose name it already sent.
    bool collisionGroupsCollide(std::string_view a, std::string_view b) const;

    LUNAR_REF(Camera, currentCamera, CurrentCamera)
    LUNAR_FIELD(float, gravity, Gravity, 196.2f)
    LUNAR_FIELD(float, fallenPartsDestroyHeight, FallenPartsDestroyHeight, -500.0f)
    LUNAR_FIELD(bool, fallHeightEnabled, FallHeightEnabled, true)
    LUNAR_FIELD(Vector3, globalWind, GlobalWind)
    LUNAR_FIELD(float, airDensity, AirDensity, 0.0012f)
    LUNAR_FIELD(float, airTurbulenceIntensity, AirTurbulenceIntensity, 0.0f)
    LUNAR_FIELD(double, distributedGameTime, DistributedGameTime, 0.0)
    LUNAR_FIELD(bool, streamingEnabled, StreamingEnabled, false)
    LUNAR_FIELD(int, streamingMinRadius, StreamingMinRadius, 64)
    LUNAR_FIELD(int, streamingTargetRadius, StreamingTargetRadius, 1024)
    LUNAR_FIELD(bool, allowThirdPartySales, AllowThirdPartySales, false)
    LUNAR_FIELD(bool, filteringEnabled, FilteringEnabled, true)
    LUNAR_FIELD(bool, touchesUseCollisionGroups, TouchesUseCollisionGroups, false)
    // Whether events run their handlers right away or at the next resumption point.
    LUNAR_FIELD(Enum::SignalBehavior, signalBehavior, SignalBehavior, Enum::SignalBehavior::Default)

    // The workspace's Terrain child, if there is one.
    LUNAR_PROPERTY() Terrain* terrain() const { return static_cast<Terrain*>(findFirstChildOfClass("Terrain")); }

private:
    std::vector<CollisionGroup> collisionGroups_{{"Default", 0, 0xFFFFFFFF}};
};

class DataModel final : public InstanceImpl<DataModel, ServiceProvider, ClassKind::NotCreatable> {
public:
    // A fresh game with its always-present services.
    static std::shared_ptr<DataModel> create();

    Workspace* workspace() { return getService<Workspace>(); }

    // The server's job id and the place, as the network join knows them;
    // read-only to scripts, like Roblox's (no set<name> member on purpose).
    LUNAR_PROPERTY() std::string jobId() const { return jobId_; }
    LUNAR_PROPERTY() double placeId() const { return placeId_; }
    void assignJob(std::string jobId, double placeId)
    {
        jobId_ = std::move(jobId);
        placeId_ = placeId;
    }
    // game.GameId: the universe the place belongs to (0 offline, like Studio's unpublished places).
    LUNAR_PROPERTY() double gameId() const { return gameId_; }
    void assignUniverse(double universeId) { gameId_ = universeId; }

private:
    std::string jobId_;
    double placeId_ = 0;
    double gameId_ = 0;
};

} // namespace lunar
