#pragma once

// BasePart and the part classes. Included through <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/Core.hpp>

#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace lunar {

class BasePart : public InstanceImpl<BasePart, PVInstance, ClassKind::Abstract> {
public:
    // While one lives, the CFrames set on this thread are a script's
    // (part.CFrame = ..., PivotTo, BulkMoveTo): physics then moves the part's
    // whole assembly there, as Roblox does. The engine's own placements
    // (animation, replication, seats) move only the part.
    class CarryAssembly {
    public:
        CarryAssembly() { ++carrying_; }
        ~CarryAssembly() { --carrying_; }
        CarryAssembly(const CarryAssembly&) = delete;
        CarryAssembly& operator=(const CarryAssembly&) = delete;
    };
    static bool carryingAssembly() { return carrying_ > 0; }

    LUNAR_PROPERTY() Vector3 size() const { return size_; }
    void setSize(Vector3 size) { setProperty(size_, size, "Size"); }

    // Always a rotation, as the client keeps it (the API reference for
    // CFrame:Orthonormalize: BasePart.CFrame orthonormalizes what it's set
    // to): a skewed or scaled one is squared up first. Model:PivotTo moves
    // the parts by the target times the pivot's inverse(), which for a pivot
    // scaled by s scales them by s squared: a script pivoting a model every
    // frame (the game moving another player's character) doubled the rounding of
    // the last product each time, and in under 40 frames the character's
    // parts were billions of studs across, then NaN.
    LUNAR_PROPERTY("CFrame") CFrame cframe() const { return cframe_; }
    // Made a rotation as it's set, as Studio does (CFrame::orthonormalized,
    // which leaves one that already is exactly as it is).
    void setCFrame(CFrame cframe)
    {
        cframe = cframe.orthonormalized();
        bool moved = cframe.position != cframe_.position;
        bool turned = cframe.rotation() != cframe_.rotation();
        setProperty(cframe_, cframe, "CFrame");
        if (moved)
            changed.fire("Position");
        if (turned)
            changed.fire("Orientation");
    }

    // Shortcut into CFrame, keeps the rotation.
    LUNAR_PROPERTY() Vector3 position() const { return cframe_.position; }
    void setPosition(Vector3 position) { setCFrame(CFrame(position) * cframe_.rotation()); }

    // CFrame's rotation as YXZ angles in degrees; keeps the position.
    LUNAR_PROPERTY() Vector3 orientation() const
    {
        return cframe_.toOrientation() * (180.0f / std::numbers::pi_v<float>);
    }
    void setOrientation(Vector3 degrees)
    {
        Vector3 r = degrees * (std::numbers::pi_v<float> / 180.0f);
        setCFrame(CFrame(cframe_.position) * CFrame::fromOrientation(r.x, r.y, r.z));
    }

    LUNAR_PROPERTY() Color3 color() const { return color_; }
    void setColor(Color3 color)
    {
        if (color == color_)
            return;
        setProperty(color_, color, "Color");
        changed.fire("BrickColor");
    }

    // The palette color closest to Color; setting it sets Color, as in Roblox.
    LUNAR_PROPERTY() BrickColor brickColor() const { return BrickColor::nearest(color_); }
    void setBrickColor(BrickColor brickColor) { setColor(brickColor.color()); }

    LUNAR_PROPERTY() float transparency() const { return transparency_; }
    void setTransparency(float transparency) { setProperty(transparency_, transparency, "Transparency"); }

    LUNAR_PROPERTY() bool anchored() const { return anchored_; }
    void setAnchored(bool anchored) { setProperty(anchored_, anchored, "Anchored"); }

    LUNAR_PROPERTY() bool canCollide() const { return canCollide_; }
    void setCanCollide(bool canCollide) { setProperty(canCollide_, canCollide, "CanCollide"); }

    LUNAR_FIELD(Enum::Material, material, Material, Enum::Material::Plastic)
    LUNAR_FIELD(std::string, materialVariant, MaterialVariant)
    LUNAR_FIELD(float, reflectance, Reflectance, 0.0f)
    LUNAR_FIELD(bool, canTouch, CanTouch, true)
    LUNAR_FIELD(bool, canQuery, CanQuery, true)
    LUNAR_FIELD(bool, castShadow, CastShadow, true)
    LUNAR_FIELD(bool, locked, Locked, false)
    LUNAR_FIELD(bool, massless, Massless, false)
    LUNAR_FIELD(bool, enableFluidForces, EnableFluidForces, true)
    LUNAR_FIELD(bool, audioCanCollide, AudioCanCollide, true)
    LUNAR_FIELD(int, rootPriority, RootPriority, 0)
    LUNAR_FIELD(std::string, collisionGroup, CollisionGroup, "Default")
    // nil means the material's own physics.
    LUNAR_FIELD(std::optional<PhysicalProperties>, customPhysicalProperties, CustomPhysicalProperties)
    LUNAR_FIELD(Vector3, assemblyLinearVelocity, AssemblyLinearVelocity)
    LUNAR_FIELD(Vector3, assemblyAngularVelocity, AssemblyAngularVelocity)
    // The deprecated names, which scripts still read (the Animate script's
    // strafing blend uses HumanoidRootPart.Velocity).
    LUNAR_PROPERTY("Velocity") Vector3 velocity() const { return assemblyLinearVelocity(); }
    void setVelocity(Vector3 velocity) { setAssemblyLinearVelocity(velocity); }
    LUNAR_PROPERTY("RotVelocity") Vector3 rotVelocity() const { return assemblyAngularVelocity(); }
    void setRotVelocity(Vector3 velocity) { setAssemblyAngularVelocity(velocity); }
    // Where the pivot sits relative to CFrame.
    LUNAR_FIELD(CFrame, pivotOffset, PivotOffset)
    // Extra transparency the local client applies (first-person bodies), never saved.
    LUNAR_FIELD(float, localTransparencyModifier, LocalTransparencyModifier, 0.0f)

    LUNAR_FIELD(Enum::SurfaceType, topSurface, TopSurface, Enum::SurfaceType::Smooth)
    LUNAR_FIELD(Enum::SurfaceType, bottomSurface, BottomSurface, Enum::SurfaceType::Smooth)
    LUNAR_FIELD(Enum::SurfaceType, frontSurface, FrontSurface, Enum::SurfaceType::Smooth)
    LUNAR_FIELD(Enum::SurfaceType, backSurface, BackSurface, Enum::SurfaceType::Smooth)
    LUNAR_FIELD(Enum::SurfaceType, leftSurface, LeftSurface, Enum::SurfaceType::Smooth)
    LUNAR_FIELD(Enum::SurfaceType, rightSurface, RightSurface, Enum::SurfaceType::Smooth)

    // BasePart:GetPivot() and :PivotTo().
    CFrame getPivot() const { return cframe_ * pivotOffset(); }
    void pivotTo(const CFrame& pivot) { setCFrame(pivot * pivotOffset().inverse()); }

private:
    static inline thread_local int carrying_ = 0;
    Vector3 size_{4.0f, 1.0f, 2.0f};
    CFrame cframe_;
    Color3 color_ = Color3::fromRGB(163, 162, 165); // Medium stone grey
    float transparency_ = 0.0f;
    bool anchored_ = false;
    bool canCollide_ = true;
};

// Parts whose size used to snap to a form factor. A new one is a Brick
// [sUNC: tostring(part.formFactorRaw) is "Enum.FormFactor.Brick"].
class FormFactorPart : public InstanceImpl<FormFactorPart, BasePart, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::FormFactor, formFactor, FormFactor, Enum::FormFactor::Brick)
};

class Part : public InstanceImpl<Part, FormFactorPart> {
public:
    LUNAR_FIELD(Enum::PartType, shape, Shape, Enum::PartType::Block)
};

class WedgePart final : public InstanceImpl<WedgePart, FormFactorPart> {};

class CornerWedgePart final : public InstanceImpl<CornerWedgePart, BasePart> {};

class TrussPart final : public InstanceImpl<TrussPart, BasePart> {};

class SpawnLocation final : public InstanceImpl<SpawnLocation, Part> {
public:
    LUNAR_FIELD(bool, allowTeamChangeOnTouch, AllowTeamChangeOnTouch, false)
    LUNAR_FIELD(int, duration, Duration, 10)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, neutral, Neutral, true)
    LUNAR_FIELD(BrickColor, teamColor, TeamColor, BrickColor{194})
};

// Parts whose shape comes from a mesh.
class TriangleMeshPart : public InstanceImpl<TriangleMeshPart, BasePart, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::CollisionFidelity, collisionFidelity, CollisionFidelity, Enum::CollisionFidelity::Default)
    // The mesh's own size; Size scales it.
    LUNAR_FIELD(Vector3, initialSize, InitialSize, Vector3{1.0f, 1.0f, 1.0f})

    // Roblox's precomputed collision geometry, a "CSGPHS" blob (see
    // physics/CollisionData.hpp): PhysicalConfigData, and PhysicsData, the
    // older property some files still carry. Only the loaders set them; they
    // aren't scriptable properties, and null means none was saved.
    const SharedBytes& physicalConfigData() const { return physicalConfigData_; }
    void setPhysicalConfigData(SharedBytes data)
    {
        physicalConfigData_ = std::move(data);
        changed.fire("PhysicalConfigData");
    }
    const SharedBytes& physicsData() const { return physicsData_; }
    void setPhysicsData(SharedBytes data)
    {
        physicsData_ = std::move(data);
        changed.fire("PhysicsData");
    }

private:
    SharedBytes physicalConfigData_;
    SharedBytes physicsData_;
};

class MeshPart final : public InstanceImpl<MeshPart, TriangleMeshPart> {
public:
    LUNAR_FIELD(std::string, meshId, MeshId)
    LUNAR_FIELD(std::string, textureId, TextureID)
    LUNAR_FIELD(Enum::RenderFidelity, renderFidelity, RenderFidelity, Enum::RenderFidelity::Automatic)
    LUNAR_FIELD(bool, doubleSided, DoubleSided, false)

    // Read-only in Roblox: the mesh's size before scaling.
    LUNAR_PROPERTY() Vector3 meshSize() const { return initialSize(); }
};

// Solid modeling results; the geometry itself lives in the serialized mesh data.
class PartOperation : public InstanceImpl<PartOperation, TriangleMeshPart, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::RenderFidelity, renderFidelity, RenderFidelity, Enum::RenderFidelity::Automatic)
    LUNAR_FIELD(bool, usePartColor, UsePartColor, false)
    // The PartOperationAsset with the geometry (the network calls it Content).
    LUNAR_FIELD(std::string, assetId, AssetId)
    LUNAR_FIELD(float, smoothingAngle, SmoothingAngle, 0.0f)
};

class UnionOperation final : public InstanceImpl<UnionOperation, PartOperation> {};
class NegateOperation final : public InstanceImpl<NegateOperation, PartOperation> {};
class IntersectOperation final : public InstanceImpl<IntersectOperation, PartOperation> {};

} // namespace lunar
