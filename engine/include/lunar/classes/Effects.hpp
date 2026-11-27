#pragma once

// Things that change how parts and the world look or sound: decals, meshes,
// lights, sounds, particles, the sky and post effects. Included through
// <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/Joints.hpp>

#include <cstdint>
#include <string>

namespace lunar {

// --- On a part's faces ----------------------------------------------------------

class FaceInstance : public InstanceImpl<FaceInstance, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::NormalId, face, Face, Enum::NormalId::Front)
};

class Decal : public InstanceImpl<Decal, FaceInstance> {
public:
    LUNAR_FIELD(std::string, texture, Texture)
    LUNAR_FIELD(Color3, color3, Color3, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, transparency, Transparency, 0.0f)
    LUNAR_FIELD(int, zIndex, ZIndex, 1)
    LUNAR_FIELD(float, rotation, Rotation, 0.0f)
    LUNAR_FIELD(Vector2, uvOffset, UVOffset)
    LUNAR_FIELD(Vector2, uvScale, UVScale, Vector2(1.0f, 1.0f))
    LUNAR_FIELD(std::string, metalnessMap, MetalnessMap)
    LUNAR_FIELD(std::string, normalMap, NormalMap)
    LUNAR_FIELD(std::string, roughnessMap, RoughnessMap)
    LUNAR_FIELD(std::string, texturePack, TexturePack)
};

// A decal repeated across the face.
class Texture final : public InstanceImpl<Texture, Decal> {
public:
    LUNAR_FIELD(float, offsetStudsU, OffsetStudsU, 0.0f)
    LUNAR_FIELD(float, offsetStudsV, OffsetStudsV, 0.0f)
    LUNAR_FIELD(float, studsPerTileU, StudsPerTileU, 2.0f)
    LUNAR_FIELD(float, studsPerTileV, StudsPerTileV, 2.0f)
};

// An outline and a fill over its Adornee (its parent without one). Not drawn yet.
class Highlight final : public InstanceImpl<Highlight, Instance> {
public:
    LUNAR_REF(Instance, adornee, Adornee)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(Color3, fillColor, FillColor, Color3(1.0f, 0.0f, 0.0f))
    LUNAR_FIELD(float, fillTransparency, FillTransparency, 0.5f)
    LUNAR_FIELD(Color3, outlineColor, OutlineColor, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, outlineTransparency, OutlineTransparency, 0.0f)
    LUNAR_FIELD(Enum::HighlightDepthMode, depthMode, DepthMode, Enum::HighlightDepthMode::AlwaysOnTop)
};

// PBR textures for a MeshPart.
class SurfaceAppearance final : public InstanceImpl<SurfaceAppearance, Instance> {
public:
    LUNAR_FIELD(Enum::AlphaMode, alphaMode, AlphaMode, Enum::AlphaMode::Overlay)
    LUNAR_FIELD(Color3, color, Color, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(std::string, colorMap, ColorMap)
    LUNAR_FIELD(std::string, metalnessMap, MetalnessMap)
    LUNAR_FIELD(std::string, normalMap, NormalMap)
    LUNAR_FIELD(std::string, roughnessMap, RoughnessMap)
    LUNAR_FIELD(std::string, texturePack, TexturePack)
    LUNAR_FIELD(std::string, emissiveMaskContent, EmissiveMaskContent)
    LUNAR_FIELD(float, emissiveStrength, EmissiveStrength, 1.0f)
    LUNAR_FIELD(Color3, emissiveTint, EmissiveTint, Color3(1.0f, 1.0f, 1.0f))
};

// --- Meshes that reshape their parent part -----------------------------------------

class DataModelMesh : public InstanceImpl<DataModelMesh, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Vector3, offset, Offset)
    LUNAR_FIELD(Vector3, scale, Scale, Vector3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(Vector3, vertexColor, VertexColor, Vector3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(Enum::LevelOfDetailSetting, lodX, LODX, Enum::LevelOfDetailSetting::High)
    LUNAR_FIELD(Enum::LevelOfDetailSetting, lodY, LODY, Enum::LevelOfDetailSetting::High)
};

class FileMesh : public InstanceImpl<FileMesh, DataModelMesh> {
public:
    LUNAR_FIELD(std::string, meshId, MeshId)
    LUNAR_FIELD(std::string, textureId, TextureId)
};

class SpecialMesh final : public InstanceImpl<SpecialMesh, FileMesh> {
public:
    LUNAR_FIELD(Enum::MeshType, meshType, MeshType, Enum::MeshType::Head)
};

class BevelMesh : public InstanceImpl<BevelMesh, DataModelMesh, ClassKind::Abstract> {
public:
    LUNAR_FIELD(float, bevel, Bevel, 0.0f)
    LUNAR_FIELD(float, bulge, Bulge, 0.0f)

    // The serialized name has a space in it.
    LUNAR_PROPERTY("Bevel Roundness") float bevelRoundness() const { return bevelRoundness_; }
    void setBevelRoundness(float roundness) { setProperty(bevelRoundness_, roundness, "Bevel Roundness"); }

private:
    float bevelRoundness_ = 0.0f;
};

class BlockMesh final : public InstanceImpl<BlockMesh, BevelMesh> {};
class CylinderMesh final : public InstanceImpl<CylinderMesh, BevelMesh> {};

// --- Lights ---------------------------------------------------------------------------

class Light : public InstanceImpl<Light, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(float, brightness, Brightness, 1.0f)
    LUNAR_FIELD(Color3, color, Color, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, shadows, Shadows, false)
};

class PointLight final : public InstanceImpl<PointLight, Light> {
public:
    LUNAR_FIELD(float, range, Range, 8.0f)
};

class SpotLight final : public InstanceImpl<SpotLight, Light> {
public:
    LUNAR_FIELD(float, angle, Angle, 90.0f)
    LUNAR_FIELD(Enum::NormalId, face, Face, Enum::NormalId::Front)
    LUNAR_FIELD(float, range, Range, 16.0f)
};

class SurfaceLight final : public InstanceImpl<SurfaceLight, Light> {
public:
    LUNAR_FIELD(float, angle, Angle, 90.0f)
    LUNAR_FIELD(Enum::NormalId, face, Face, Enum::NormalId::Front)
    LUNAR_FIELD(float, range, Range, 16.0f)
};

// --- Sound ----------------------------------------------------------------------------

class Sound final : public InstanceImpl<Sound, Instance> {
public:
    LUNAR_FIELD(std::string, soundId, SoundId)
    LUNAR_FIELD(bool, looped, Looped, false)
    LUNAR_FIELD(bool, playing, Playing, false)
    LUNAR_FIELD(bool, playOnRemove, PlayOnRemove, false)
    LUNAR_FIELD(float, playbackSpeed, PlaybackSpeed, 1.0f)
    LUNAR_FIELD(float, volume, Volume, 0.5f)
    LUNAR_FIELD(double, timePosition, TimePosition, 0.0)
    LUNAR_FIELD(Enum::RollOffMode, rollOffMode, RollOffMode, Enum::RollOffMode::Inverse)
    LUNAR_FIELD(float, rollOffMinDistance, RollOffMinDistance, 10.0f)
    LUNAR_FIELD(float, rollOffMaxDistance, RollOffMaxDistance, 10000.0f)
    LUNAR_REF(Instance, soundGroup, SoundGroup)

    // What the sound system (lunar::audio::SoundSystem) found: whether its
    // audio has loaded, how long it is, how loud it plays right now.
    LUNAR_PROPERTY() bool isLoaded() const { return isLoaded_; }
    LUNAR_PROPERTY() double timeLength() const { return timeLength_; }
    LUNAR_PROPERTY() bool isPlaying() const { return playing(); }
    LUNAR_PROPERTY() bool isPaused() const { return !playing(); }
    LUNAR_PROPERTY() float playbackLoudness() const { return playbackLoudness_; }
    void assignLoaded(bool loaded, double length)
    {
        assign(timeLength_, length, "TimeLength");
        assign(isLoaded_, loaded, "IsLoaded");
    }
    void assignLoudness(float loudness) { assign(playbackLoudness_, loudness, "PlaybackLoudness"); }

private:
    template <class T>
    void assign(T& field, const T& value, std::string_view property)
    {
        if (field == value)
            return;
        field = value;
        changed.fire(property);
    }
    bool isLoaded_ = false;
    double timeLength_ = 0.0;
    float playbackLoudness_ = 0.0f;
};

class SoundGroup final : public InstanceImpl<SoundGroup, Instance> {
public:
    LUNAR_FIELD(float, volume, Volume, 0.5f)
};

class SoundEffect : public InstanceImpl<SoundEffect, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(int, priority, Priority, 0)
};

class PitchShiftSoundEffect final : public InstanceImpl<PitchShiftSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, octave, Octave, 1.25f)
};

// The rest of the SoundEffects, in a Sound or a SoundGroup (all its
// sounds then). GUESS: some defaults, Roblox's documentation doesn't give them all.
class ChorusSoundEffect final : public InstanceImpl<ChorusSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, depth, Depth, 0.15f)
    LUNAR_FIELD(float, mix, Mix, 0.5f)
    LUNAR_FIELD(float, rate, Rate, 0.5f)
};

class CompressorSoundEffect final : public InstanceImpl<CompressorSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, attack, Attack, 0.1f)
    LUNAR_FIELD(float, gainMakeup, GainMakeup, 0.0f)
    LUNAR_FIELD(float, ratio, Ratio, 40.0f)
    LUNAR_FIELD(float, release, Release, 0.1f)
    LUNAR_REF(Instance, sideChain, SideChain)
    LUNAR_FIELD(float, threshold, Threshold, -40.0f)
};

class DistortionSoundEffect final : public InstanceImpl<DistortionSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, level, Level, 0.75f)
};

class EchoSoundEffect final : public InstanceImpl<EchoSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, delay, Delay, 1.0f)
    LUNAR_FIELD(float, dryLevel, DryLevel, 0.0f)
    LUNAR_FIELD(float, feedback, Feedback, 0.5f)
    LUNAR_FIELD(float, wetLevel, WetLevel, 0.0f)
};

// Gains in dB for below 400 Hz, between, and above 4 kHz.
class EqualizerSoundEffect final : public InstanceImpl<EqualizerSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, highGain, HighGain, 0.0f)
    LUNAR_FIELD(float, lowGain, LowGain, -20.0f)
    LUNAR_FIELD(float, midGain, MidGain, -10.0f)
};

class FlangeSoundEffect final : public InstanceImpl<FlangeSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, depth, Depth, 0.45f)
    LUNAR_FIELD(float, mix, Mix, 0.85f)
    LUNAR_FIELD(float, rate, Rate, 5.0f)
};

class ReverbSoundEffect final : public InstanceImpl<ReverbSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, decayTime, DecayTime, 1.5f)
    LUNAR_FIELD(float, density, Density, 1.0f)
    LUNAR_FIELD(float, diffusion, Diffusion, 1.0f)
    LUNAR_FIELD(float, dryLevel, DryLevel, -6.0f)
    LUNAR_FIELD(float, wetLevel, WetLevel, 0.0f)
};

class TremoloSoundEffect final : public InstanceImpl<TremoloSoundEffect, SoundEffect> {
public:
    LUNAR_FIELD(float, depth, Depth, 1.0f)
    LUNAR_FIELD(float, duty, Duty, 0.5f)
    LUNAR_FIELD(float, frequency, Frequency, 5.0f)
};

// --- Particles, beams and trails ---------------------------------------------------------

class ParticleEmitter final : public InstanceImpl<ParticleEmitter, Instance> {
public:
    LUNAR_FIELD(std::string, texture, Texture, "rbxasset://textures/particles/sparkles_main.dds")
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(float, rate, Rate, 20.0f)
    LUNAR_FIELD(NumberRange, lifetime, Lifetime, NumberRange(5.0f, 10.0f))
    LUNAR_FIELD(NumberRange, speed, Speed, NumberRange(5.0f, 5.0f))
    LUNAR_FIELD(Vector2, spreadAngle, SpreadAngle)
    LUNAR_FIELD(Vector3, acceleration, Acceleration)
    LUNAR_FIELD(float, drag, Drag, 0.0f)
    LUNAR_FIELD(Enum::NormalId, emissionDirection, EmissionDirection, Enum::NormalId::Top)
    LUNAR_FIELD(ColorSequence, color, Color)
    LUNAR_FIELD(NumberSequence, size, Size, NumberSequence(1.0f))
    LUNAR_FIELD(NumberSequence, transparency, Transparency)
    LUNAR_FIELD(NumberSequence, squash, Squash)
    LUNAR_FIELD(NumberRange, rotation, Rotation)
    LUNAR_FIELD(NumberRange, rotSpeed, RotSpeed)
    LUNAR_FIELD(float, brightness, Brightness, 1.0f)
    LUNAR_FIELD(float, lightEmission, LightEmission, 0.0f)
    LUNAR_FIELD(float, lightInfluence, LightInfluence, 0.0f)
    LUNAR_FIELD(bool, lockedToPart, LockedToPart, false)
    LUNAR_FIELD(Enum::ParticleOrientation, orientation, Orientation, Enum::ParticleOrientation::FacingCamera)
    LUNAR_FIELD(Enum::ParticleEmitterShape, shape, Shape, Enum::ParticleEmitterShape::Box)
    LUNAR_FIELD(Enum::ParticleEmitterShapeInOut, shapeInOut, ShapeInOut, Enum::ParticleEmitterShapeInOut::Outward)
    LUNAR_FIELD(Enum::ParticleEmitterShapeStyle, shapeStyle, ShapeStyle, Enum::ParticleEmitterShapeStyle::Volume)
    LUNAR_FIELD(float, shapePartial, ShapePartial, 1.0f)
    LUNAR_FIELD(float, timeScale, TimeScale, 1.0f)
    LUNAR_FIELD(float, velocityInheritance, VelocityInheritance, 0.0f)
    LUNAR_FIELD(float, zOffset, ZOffset, 0.0f)
    LUNAR_FIELD(bool, windAffectsDrag, WindAffectsDrag, false)
    // Sprite sheets: the texture as a grid of frames, played over each
    // particle's life. GUESS: the defaults of FlipbookFramerate (1),
    // FlipbookBlendFrames (on) and FlipbookSizeX/Y (8, Custom's grid).
    LUNAR_FIELD(Enum::ParticleFlipbookLayout, flipbookLayout, FlipbookLayout, Enum::ParticleFlipbookLayout::None)
    LUNAR_FIELD(Enum::ParticleFlipbookMode, flipbookMode, FlipbookMode, Enum::ParticleFlipbookMode::Loop)
    LUNAR_FIELD(NumberRange, flipbookFramerate, FlipbookFramerate, NumberRange(1.0f, 1.0f))
    LUNAR_FIELD(bool, flipbookStartRandom, FlipbookStartRandom, false)
    LUNAR_FIELD(bool, flipbookBlendFrames, FlipbookBlendFrames, true)
    LUNAR_FIELD(int, flipbookSizeX, FlipbookSizeX, 8)
    LUNAR_FIELD(int, flipbookSizeY, FlipbookSizeY, 8)
    // Fades the particles further (first person hides the character's).
    LUNAR_FIELD(float, localTransparencyModifier, LocalTransparencyModifier, 0.0f)
};

class Beam final : public InstanceImpl<Beam, Instance> {
public:
    LUNAR_REF(Attachment, attachment0, Attachment0)
    LUNAR_REF(Attachment, attachment1, Attachment1)
    LUNAR_FIELD(float, brightness, Brightness, 1.0f)
    LUNAR_FIELD(ColorSequence, color, Color)
    LUNAR_FIELD(float, curveSize0, CurveSize0, 0.0f)
    LUNAR_FIELD(float, curveSize1, CurveSize1, 0.0f)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, faceCamera, FaceCamera, false)
    LUNAR_FIELD(float, lightEmission, LightEmission, 0.0f)
    LUNAR_FIELD(float, lightInfluence, LightInfluence, 0.0f)
    LUNAR_FIELD(int, segments, Segments, 10)
    LUNAR_FIELD(std::string, texture, Texture)
    LUNAR_FIELD(float, textureLength, TextureLength, 1.0f)
    LUNAR_FIELD(Enum::TextureMode, textureMode, TextureMode, Enum::TextureMode::Stretch)
    LUNAR_FIELD(float, textureSpeed, TextureSpeed, 1.0f)
    LUNAR_FIELD(NumberSequence, transparency, Transparency, NumberSequence(0.5f))
    LUNAR_FIELD(float, width0, Width0, 1.0f)
    LUNAR_FIELD(float, width1, Width1, 1.0f)
    LUNAR_FIELD(float, zOffset, ZOffset, 0.0f)
    LUNAR_FIELD(float, localTransparencyModifier, LocalTransparencyModifier, 0.0f)

    // Beam:SetTextureOffset: where the texture's scrolling restarts from,
    // and a count of the calls, which tells the renderer to take it.
    void setTextureOffset(float offset)
    {
        textureOffset_ = offset;
        ++textureOffsetSets_;
    }
    float textureOffset() const { return textureOffset_; }
    std::uint64_t textureOffsetSets() const { return textureOffsetSets_; }

private:
    float textureOffset_ = 0.0f;
    std::uint64_t textureOffsetSets_ = 0;
};

class Trail final : public InstanceImpl<Trail, Instance> {
public:
    LUNAR_REF(Attachment, attachment0, Attachment0)
    LUNAR_REF(Attachment, attachment1, Attachment1)
    LUNAR_FIELD(float, brightness, Brightness, 1.0f)
    LUNAR_FIELD(ColorSequence, color, Color)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(bool, faceCamera, FaceCamera, false)
    LUNAR_FIELD(float, lifetime, Lifetime, 2.0f)
    LUNAR_FIELD(float, lightEmission, LightEmission, 0.0f)
    LUNAR_FIELD(float, lightInfluence, LightInfluence, 0.0f)
    LUNAR_FIELD(float, maxLength, MaxLength, 0.0f)
    LUNAR_FIELD(float, minLength, MinLength, 0.1f)
    LUNAR_FIELD(std::string, texture, Texture)
    LUNAR_FIELD(float, textureLength, TextureLength, 1.0f)
    LUNAR_FIELD(Enum::TextureMode, textureMode, TextureMode, Enum::TextureMode::Stretch)
    LUNAR_FIELD(NumberSequence, transparency, Transparency, NumberSequence(0.5f))
    LUNAR_FIELD(NumberSequence, widthScale, WidthScale, NumberSequence(1.0f))
    LUNAR_FIELD(float, localTransparencyModifier, LocalTransparencyModifier, 0.0f)

    // Trail:Clear: a count of the calls; the renderer drops the segments
    // drawn so far whenever it moves.
    void clear() { ++clears_; }
    std::uint64_t clears() const { return clears_; }

private:
    std::uint64_t clears_ = 0;
};

// --- Sky, atmosphere and post effects (children of Lighting or the camera) ---------------

class Sky final : public InstanceImpl<Sky, Instance> {
public:
    LUNAR_FIELD(bool, celestialBodiesShown, CelestialBodiesShown, true)
    LUNAR_FIELD(float, moonAngularSize, MoonAngularSize, 11.0f)
    LUNAR_FIELD(std::string, moonTextureId, MoonTextureId, "rbxasset://sky/moon.jpg")
    LUNAR_FIELD(std::string, skyboxBk, SkyboxBk, "rbxasset://textures/sky/sky512_bk.tex")
    LUNAR_FIELD(std::string, skyboxDn, SkyboxDn, "rbxasset://textures/sky/sky512_dn.tex")
    LUNAR_FIELD(std::string, skyboxFt, SkyboxFt, "rbxasset://textures/sky/sky512_ft.tex")
    LUNAR_FIELD(std::string, skyboxLf, SkyboxLf, "rbxasset://textures/sky/sky512_lf.tex")
    LUNAR_FIELD(std::string, skyboxRt, SkyboxRt, "rbxasset://textures/sky/sky512_rt.tex")
    LUNAR_FIELD(std::string, skyboxUp, SkyboxUp, "rbxasset://textures/sky/sky512_up.tex")
    LUNAR_FIELD(Vector3, skyboxOrientation, SkyboxOrientation)
    LUNAR_FIELD(int, starCount, StarCount, 3000)
    LUNAR_FIELD(float, sunAngularSize, SunAngularSize, 21.0f)
    LUNAR_FIELD(std::string, sunTextureId, SunTextureId, "rbxasset://sky/sun.jpg")
};

class Atmosphere final : public InstanceImpl<Atmosphere, Instance> {
public:
    LUNAR_FIELD(Color3, color, Color, Color3::fromRGB(199, 170, 107))
    LUNAR_FIELD(Color3, decay, Decay, Color3::fromRGB(92, 60, 13))
    LUNAR_FIELD(float, density, Density, 0.395f)
    LUNAR_FIELD(float, glare, Glare, 0.0f)
    LUNAR_FIELD(float, haze, Haze, 0.0f)
    LUNAR_FIELD(float, offset, Offset, 0.0f)
};

class Clouds final : public InstanceImpl<Clouds, Instance> {
public:
    LUNAR_FIELD(Color3, color, Color, Color3(1.0f, 1.0f, 1.0f))
    LUNAR_FIELD(float, cover, Cover, 0.5f)
    LUNAR_FIELD(float, density, Density, 0.7f)
    LUNAR_FIELD(bool, enabled, Enabled, true)
};

class PostEffect : public InstanceImpl<PostEffect, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
};

class BloomEffect final : public InstanceImpl<BloomEffect, PostEffect> {
public:
    LUNAR_FIELD(float, intensity, Intensity, 1.0f)
    LUNAR_FIELD(float, size, Size, 24.0f)
    LUNAR_FIELD(float, threshold, Threshold, 2.0f)
};

class BlurEffect final : public InstanceImpl<BlurEffect, PostEffect> {
public:
    LUNAR_FIELD(float, size, Size, 24.0f)
};

class ColorCorrectionEffect final : public InstanceImpl<ColorCorrectionEffect, PostEffect> {
public:
    LUNAR_FIELD(float, brightness, Brightness, 0.0f)
    LUNAR_FIELD(float, contrast, Contrast, 0.0f)
    LUNAR_FIELD(float, saturation, Saturation, 0.0f)
    LUNAR_FIELD(Color3, tintColor, TintColor, Color3(1.0f, 1.0f, 1.0f))
};

class SunRaysEffect final : public InstanceImpl<SunRaysEffect, PostEffect> {
public:
    LUNAR_FIELD(float, intensity, Intensity, 0.25f)
    LUNAR_FIELD(float, spread, Spread, 1.0f)
};

class DepthOfFieldEffect final : public InstanceImpl<DepthOfFieldEffect, PostEffect> {
public:
    LUNAR_FIELD(float, farIntensity, FarIntensity, 0.75f)
    LUNAR_FIELD(float, focusDistance, FocusDistance, 0.05f)
    LUNAR_FIELD(float, inFocusRadius, InFocusRadius, 10.0f)
    LUNAR_FIELD(float, nearIntensity, NearIntensity, 0.75f)
};

} // namespace lunar
