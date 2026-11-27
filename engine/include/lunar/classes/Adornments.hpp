#pragma once

// 3D adornments: the GuiBase3d family that draws over parts in the world
// (SelectionBox, the HandleAdornments, Handles). Their properties, as the
// 0.742 client's API dump lays them out; nothing draws them yet. Included
// through <lunar/Classes.hpp>.
//
// GUESS: the defaults. The dumps hold no values; these are what Studio shows
// for a new one, Color3 being BrickColor Bright blue (13, 105, 172) all over.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/Core.hpp>
#include <lunar/classes/Gui.hpp>
#include <lunar/classes/Parts.hpp>

#include <string>

namespace lunar {

class GuiBase3d : public InstanceImpl<GuiBase3d, GuiBase, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Color3, color3, Color3, Color3::fromRGB(13, 105, 172))
    LUNAR_FIELD(float, transparency, Transparency, 0.0f)
    LUNAR_FIELD(bool, visible, Visible, true)

    // The deprecated BrickColor spelling of Color3; setting it sets Color3.
    LUNAR_PROPERTY() BrickColor color() const { return BrickColor::nearest(color3()); }
    void setColor(BrickColor brickColor) { setColor3(brickColor.color()); }
};

// Adorns any instance (a part or a model).
class InstanceAdornment : public InstanceImpl<InstanceAdornment, GuiBase3d, ClassKind::Abstract> {
public:
    LUNAR_REF(Instance, adornee, Adornee)
};

class SelectionBox final : public InstanceImpl<SelectionBox, InstanceAdornment> {
public:
    LUNAR_FIELD(float, lineThickness, LineThickness, 0.15f)
    LUNAR_FIELD(Color3, surfaceColor3, SurfaceColor3, Color3::fromRGB(13, 105, 172))
    LUNAR_FIELD(float, surfaceTransparency, SurfaceTransparency, 1.0f)
    LUNAR_FIELD(bool, studioSelectionBox, StudioSelectionBox, false)

    LUNAR_PROPERTY() BrickColor surfaceColor() const { return BrickColor::nearest(surfaceColor3()); }
    void setSurfaceColor(BrickColor brickColor) { setSurfaceColor3(brickColor.color()); }
};

// Adorns a part or a model, placed by its pivot.
class PVAdornment : public InstanceImpl<PVAdornment, GuiBase3d, ClassKind::Abstract> {
public:
    LUNAR_REF(PVInstance, adornee, Adornee)
};

class SelectionSphere final : public InstanceImpl<SelectionSphere, PVAdornment> {
public:
    LUNAR_FIELD(Color3, surfaceColor3, SurfaceColor3, Color3::fromRGB(13, 105, 172))
    LUNAR_FIELD(float, surfaceTransparency, SurfaceTransparency, 1.0f)

    LUNAR_PROPERTY() BrickColor surfaceColor() const { return BrickColor::nearest(surfaceColor3()); }
    void setSurfaceColor(BrickColor brickColor) { setSurfaceColor3(brickColor.color()); }
};

// A shape drawn at CFrame relative to its Adornee: what ESP scripts box
// players' parts with.
class HandleAdornment : public InstanceImpl<HandleAdornment, PVAdornment, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::AdornCullingMode, adornCullingMode, AdornCullingMode, Enum::AdornCullingMode::Automatic)
    LUNAR_FIELD(bool, alwaysOnTop, AlwaysOnTop, false)
    LUNAR_FIELD(CFrame, cframe, CFrame)
    LUNAR_FIELD(Vector3, sizeRelativeOffset, SizeRelativeOffset)
    LUNAR_FIELD(int, zIndex, ZIndex, -1)
};

class BoxHandleAdornment final : public InstanceImpl<BoxHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(Vector3, size, Size, Vector3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(Enum::AdornShading, shading, Shading, Enum::AdornShading::Default)
};

class SphereHandleAdornment final : public InstanceImpl<SphereHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(float, radius, Radius, 1.0f)
    LUNAR_FIELD(Enum::AdornShading, shading, Shading, Enum::AdornShading::Default)
};

class ConeHandleAdornment final : public InstanceImpl<ConeHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(float, height, Height, 2.0f)
    LUNAR_FIELD(float, radius, Radius, 0.5f)
    LUNAR_FIELD(bool, hollow, Hollow, false)
    LUNAR_FIELD(Enum::AdornShading, shading, Shading, Enum::AdornShading::Default)
};

class CylinderHandleAdornment final : public InstanceImpl<CylinderHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(float, angle, Angle, 360.0f)
    LUNAR_FIELD(float, height, Height, 1.0f)
    LUNAR_FIELD(float, innerRadius, InnerRadius, 0.0f)
    LUNAR_FIELD(float, radius, Radius, 1.0f)
    LUNAR_FIELD(Enum::AdornShading, shading, Shading, Enum::AdornShading::Default)
};

class LineHandleAdornment final : public InstanceImpl<LineHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(float, length, Length, 5.0f)
    LUNAR_FIELD(float, thickness, Thickness, 1.0f)
};

class ImageHandleAdornment final : public InstanceImpl<ImageHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(std::string, image, Image, "rbxasset://textures/SurfacesDefault.png")
    LUNAR_FIELD(Vector2, size, Size, Vector2(1.0f, 1.0f))
};

class WireframeHandleAdornment final : public InstanceImpl<WireframeHandleAdornment, HandleAdornment> {
public:
    LUNAR_FIELD(Vector3, scale, Scale, Vector3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, thickness, Thickness, 1.0f)
};

// Adorns one part.
class PartAdornment : public InstanceImpl<PartAdornment, GuiBase3d, ClassKind::Abstract> {
public:
    LUNAR_REF(BasePart, adornee, Adornee)
};

class SurfaceSelection final : public InstanceImpl<SurfaceSelection, PartAdornment> {
public:
    LUNAR_FIELD(Enum::NormalId, targetSurface, TargetSurface, Enum::NormalId::Right)
};

class HandlesBase : public InstanceImpl<HandlesBase, PartAdornment, ClassKind::Abstract> {};

class Handles final : public InstanceImpl<Handles, HandlesBase> {
public:
    LUNAR_FIELD(Faces, faces, Faces, Faces{true, true, true, true, true, true})
    LUNAR_FIELD(Enum::HandlesStyle, style, Style, Enum::HandlesStyle::Resize)
};

class ArcHandles final : public InstanceImpl<ArcHandles, HandlesBase> {
public:
    LUNAR_FIELD(Axes, axes, Axes, Axes{true, true, true})
};

} // namespace lunar
