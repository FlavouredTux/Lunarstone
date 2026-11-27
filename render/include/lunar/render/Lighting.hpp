#pragma once

#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>
#include <lunar/render/Texture.hpp>

#include <array>
#include <string>

namespace lunar {
class DataModel;
class Instance;
class Lighting;
} // namespace lunar

namespace lunar::render {

// Lighting's Sky: the six skybox faces and the celestial bodies. Faces are
// in the order the sky shader indexes them (an OpenGL cube map's +X, -X, +Y,
// -Y, +Z, -Z): SkyboxRt, Lf, Up, Dn, Bk, Ft. Missing faces draw the flat sky
// colour.
struct SkyLook {
    std::array<ImagePtr, 6> faces;
    ImagePtr sun;
    ImagePtr moon;
    float sunAngularSize = 21.0f; // degrees across the sprite
    float moonAngularSize = 11.0f;
    int starCount = 3000;
    bool celestialBodiesShown = true; // sun, moon and stars
    Vector3 orientation;              // Sky.SkyboxOrientation, degrees
};

// Terrain's Clouds child.
struct CloudLook {
    bool enabled = false;
    float cover = 0.5f;
    float density = 0.7f;
    Color3 color{1.0f, 1.0f, 1.0f};
};

// Lighting's Atmosphere child.
struct AtmosphereLook {
    bool enabled = false;
    Color3 color = Color3::fromRGB(199, 170, 107);
    Color3 decay = Color3::fromRGB(92, 60, 13);
    float density = 0.395f;
    float offset = 0.0f;
    float glare = 0.0f;
    float haze = 0.0f;

    // Of the light from things this far away, the fraction lost to haze.
    // A guess at the client's falloff: about a third gone at 1000 studs for
    // the default Density.
    float densityPerStud() const { return enabled ? density / 1000.0f : 0.0f; }
};

// The post effects under Lighting and the current camera (BloomEffect,
// BlurEffect, SunRaysEffect, DepthOfFieldEffect), drawn over the 3D image
// before the GUI. Where several of a kind are on, the strongest counts
// (GUESS: the client's choice isn't known).
struct PostLook {
    bool bloom = false;
    float bloomIntensity = 1.0f;
    float bloomSize = 24.0f;     // pixels the glow reaches, about
    float bloomThreshold = 2.0f; // light (before tone mapping) above which things glow
    float blur = 0.0f;           // BlurEffect.Size, pixels; 0 without one
    bool sunRays = false;
    float sunRaysIntensity = 0.25f;
    float sunRaysSpread = 1.0f;
    bool depthOfField = false;
    float nearIntensity = 0.75f;
    float farIntensity = 0.75f;
    float focusDistance = 0.05f; // studs
    float inFocusRadius = 10.0f; // studs either side of the focus

    bool any() const { return bloom || blur > 0.0f || sunRays || depthOfField; }
};

// What lights a frame, the way the client feeds its shaders (the Globals
// constant buffer of shaders_glsl3.pack's DefaultUnified* shaders). Sky and
// fog are sRGB colours, like Roblox colours; the light colours are linear
// intensities, made the client's way by squaring sRGB values. Lighting
// happens in linear space.
struct Lighting {
    Color3 sky = Color3::fromRGB(150, 196, 236);
    // Towards the light: the sun, or the moon once the sun is well down.
    Vector3 sunDirection = Vector3{0.35f, 0.85f, 0.4f}.unit();
    // The light from sunDirection (the client's Lamp0Color), and the one
    // from the opposite side that lights faces turned away from it
    // (Lamp1Color, ColorShift_Bottom's).
    Color3 sunColor{0.75f, 0.72f, 0.66f};
    Color3 backColor{0.0f, 0.0f, 0.0f};
    // Light from everywhere (AmbientColor: Ambient squared), and what the
    // open sky adds outdoors (SkyAmbient: OutdoorAmbient squared, less
    // Ambient). Everything counts as outdoors here: there's no light grid.
    Color3 ambient{0.42f, 0.44f, 0.48f};
    Color3 skyAmbient{0.0f, 0.0f, 0.0f};
    // The skybox's light (EnvironmentDiffuseScale) and reflections
    // (EnvironmentSpecularScale), both tinted by the time of day's sky
    // gradient: top for light from above, bottom for light from below.
    float environmentDiffuse = 0.0f;
    float environmentSpecular = 0.0f;
    Color3 skyGradientTop{1.0f, 1.0f, 1.0f};
    Color3 skyGradientBottom{1.0f, 1.0f, 1.0f};
    // Scales all light before tone mapping (2^ExposureCompensation).
    float exposure = 1.0f;
    // Which of the client's two tone curves the frame goes through
    // (tone.glsl): Retro, its old look, for Technology Compatibility;
    // Default otherwise.
    bool retroTone = false;

    // Linear fog between fogStart and fogEnd studs from the camera...
    Color3 fogColor = Color3::fromRGB(192, 192, 192);
    float fogStart = 0.0f;
    float fogEnd = 100000.0f;
    AtmosphereLook atmosphere;
    CloudLook clouds;

    // The sky behind everything. Without faces it's the flat sky colour.
    SkyLook skybox;
    Color3 skyTint{1.0f, 1.0f, 1.0f};                         // the time of day's tint over the skybox
    float starBrightness = 0.0f;                              // 0 by day
    Vector3 sunPosition = Vector3{0.35f, 0.85f, 0.4f}.unit(); // where the sun's sprite goes
    Vector3 moonDirection = Vector3{-0.35f, -0.85f, -0.4f}.unit();

    // Sun shadows on parts near the camera (GlobalShadows), blurred more
    // with ShadowSoftness.
    bool shadows = true;
    float shadowSoftness = 0.2f;

    // ColorCorrection effects under Lighting, combined into one affine map
    // of the final colour (sRGB): rows of a 3x4 matrix, identity without any.
    std::array<std::array<float, 4>, 3> colorCorrection{{
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    }};
    PostLook post;

    // Seconds of game time, for animated water.
    double time = 0.0;

    // Nothing behind the scene: no sky, and alpha 0 where nothing is drawn
    // (a ViewportFrame's picture, which goes over the GUI under it). What's
    // see-through then leaves premultiplied colour, as nothing is blended in.
    bool transparentBackground = false;

    // Lighting.Technology: how local lights (PointLight, SpotLight,
    // SurfaceLight) shine. Future (and Unified) light each pixel near the
    // camera; the others only through the light grid.
    Enum::Technology technology = Enum::Technology::ShadowMap;
};

// Where the sun and moon are for a ClockTime (hours) and GeographicLatitude
// (degrees), as the client's sky model puts them: the sun rises towards +X
// at 6, peaks at noon and sets towards -X at 18, its path leaning towards
// +Z by the latitude; the moon is opposite it in the day's turn.
Vector3 sunDirection(double clockTime, float latitude);
Vector3 moonDirection(double clockTime, float latitude);

// What the client lights with for a Lighting service: the sun's colour and
// sky gradient by ClockTime, Brightness, Ambient, OutdoorAmbient,
// ColorShift_Top and _Bottom, the Environment scales, Fog*, an Atmosphere
// child as haze, the Sky child (or the client's default sky without one)
// with its textures from the provider, and the post effects among its
// children.
Lighting lightingFrom(const lunar::Lighting& service, const TextureProvider& textures = {});
// Adds the post effects (ColorCorrection too) among `parent`'s children:
// Lighting's, or the current camera's.
void addPostEffects(Lighting& lighting, const Instance& parent);
// The game's Lighting service, or the defaults when it has none, with the
// post effects under Workspace.CurrentCamera too.
Lighting lightingFrom(const DataModel& game, const TextureProvider& textures = {});

// The content ids of a Sky's faces in SkyLook order, and its sun and moon.
// Without a Sky, the client's defaults.
struct SkyTextureIds {
    std::array<std::string, 6> faces;
    std::string sun;
    std::string moon;
};
SkyTextureIds skyTextureIds(const lunar::Lighting& service);

} // namespace lunar::render
