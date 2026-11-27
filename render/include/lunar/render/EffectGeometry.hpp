#pragma once

#include <lunar/Math.hpp>
#include <lunar/render/Camera.hpp>
#include <lunar/render/Mat4.hpp>
#include <lunar/render/Texture.hpp>

#include <cstdint>
#include <vector>

namespace lunar::render {

// See-through effect geometry (beams, trails, particles): triangles already
// in world space, blended over the scene after the opaque items and sorted
// with the transparent ones, batch by batch, farthest first. Depth tested
// against the scene, never written.
//
// They're shaded the way the client's trail shaders do it (TrailFS and
// TrailFSLightUnified in its shader pack): texture times vertex colour,
// times Brightness where unlit, lit by the ambient, the sky and the sun
// where LightInfluence says so, no normals, faded out (not tinted) by fog,
// and blended premultiplied with the destination kept by alpha times one
// less LightEmission, so 0 blends normally and 1 adds.
struct EffectVertex {
    Vector3 position;
    // u and v times q, and q: divided back per pixel, so a quad that's wider
    // at one end maps its texture as if it were a rectangle seen at an angle
    // instead of folding it along the diagonal (the client's TrailVSQuad).
    // q is 1 for plain UVs.
    float uvq[3]{0.0f, 0.0f, 1.0f};
    float color[4]{1.0f, 1.0f, 1.0f, 1.0f}; // sRGB colour, opacity
    // A second place in the texture faded towards by frameBlend: a flipbook
    // frame blending into the next (ParticleEmitter.FlipbookBlendFrames).
    // Not divided by q; ignored at 0.
    float uv2[2]{0.0f, 0.0f};
    float frameBlend = 0.0f;
};

// A run of triangles with one look, sorted as a whole.
struct EffectBatch {
    std::uint32_t firstIndex = 0; // into EffectGeometry::indices
    std::uint32_t indexCount = 0;
    ImagePtr texture; // null draws white; one not on the GPU yet waits for it
    float lightEmission = 0.0f;
    float lightInfluence = 0.0f;
    float brightness = 1.0f;
    // Sorted by where this is, and its textures sized for a sphere this big.
    Vector3 center;
    float radius = 0.0f;
    // Studs nearer the camera it's depth tested at, its size on screen
    // unchanged (ParticleEmitter.ZOffset, as the client's particle shader
    // does it).
    float depthOffset = 0.0f;
    // The texture clamped at its edges rather than repeated (particles).
    bool clampUv = false;
    // Through the scene's tone curve; off, exposed and clamped as the
    // client's particle shader (ParticleLightingFS) does.
    bool toneMapped = true;
    // Seen through everything (a HandleAdornment's AlwaysOnTop): drawn after
    // all the see-through items and other batches, depth tested as if at the
    // near plane.
    bool onTop = false;
};

struct EffectGeometry {
    std::vector<EffectVertex> vertices;
    std::vector<std::uint32_t> indices; // triangle lists, into vertices
    std::vector<EffectBatch> batches;

    void clear()
    {
        vertices.clear();
        indices.clear();
        batches.clear();
    }
};

// What a source sees the frame from.
struct EffectView {
    Camera camera;
    Mat4 viewProjection;            // the frame's, for culling
    double time = 0.0;              // Lighting::time: seconds of game time
    float drawDistance = 100000.0f; // MeshRenderer::drawDistance
    float pixelsPerStud = 0.0f;     // the frame's pixels across a stud one stud away
};

// Something that adds effect geometry to every frame a MeshRenderer records
// (MeshRenderer::addEffects).
class EffectSource {
public:
    virtual ~EffectSource() = default;
    // Appends this frame's batches; called once per frame, before anything
    // is drawn.
    virtual void collect(const EffectView& view, EffectGeometry& out) = 0;
};

} // namespace lunar::render
