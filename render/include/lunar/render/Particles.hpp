#pragma once

#include <lunar/Math.hpp>
#include <lunar/Signal.hpp>
#include <lunar/render/Camera.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lunar {
class Instance;
class ParticleEmitter;
} // namespace lunar

namespace lunar::render {

// How much the particles may cost. The client thins them out at low
// graphics quality (its FRMParticleBudget lerps); density does that here.
struct ParticleSettings {
    float density = 1.0f;               // share of each emitter's Rate (and Emit counts) emitted
    std::uint32_t maxParticles = 20000; // alive at once over every emitter; past it nothing new is emitted. GUESS
    float emitDistance = 1000.0f;       // emitters farther than this from the camera don't emit. GUESS
};

// The ParticleEmitters below a root (the Workspace), simulated on the CPU
// the way the client does and turned into camera-facing quads for the
// renderer's effect pass (MeshRenderer::addEffects).
//
// An emitter emits while it's Enabled with a Rate and parented to a
// BasePart (from its Shape: Box, Sphere, Cylinder or Disc, through its
// volume or over its surface, ShapeInOut and ShapePartial) or an
// Attachment (from a point); Emit(n) and Clear() arrive as its
// OnEmitRequested and OnClearRequested events. Particles take Lifetime,
// Speed, SpreadAngle, Rotation and RotSpeed at random within their ranges,
// fall by Acceleration, slow by Drag (towards Workspace.GlobalWind with
// WindAffectsDrag), inherit VelocityInheritance of the part's velocity,
// stay in the part's space with LockedToPart, and run at TimeScale. Over
// their life they take Size, Transparency and Squash (each particle at its
// own place within the envelopes) and Color, play FlipbookLayout's frames
// and face the camera or their velocity (Orientation).
//
// Only emitters that emit or have particles cost anything each frame: the
// rest wait for their Changed or events.
class ParticleSystem : public EffectSource {
public:
    // seed makes runs repeat exactly.
    ParticleSystem(Instance& root, TextureProvider textures, std::uint64_t seed = 1);
    ~ParticleSystem() override;
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    void setSettings(const ParticleSettings& settings) { settings_ = settings; }
    const ParticleSettings& settings() const { return settings_; }

    // Moves time on by dt seconds for a camera at eye: emitters emit, and
    // particles move, age and die.
    void step(float dt, Vector3 eye);
    // The same over `seconds` in 1/60 s steps: the client's FastForward, and
    // a still picture's head start.
    void advance(float seconds, Vector3 eye);
    // Appends a batch for each emitter with particles in view, its quads
    // back to front (MeshRenderer::addEffects calls it every frame).
    void collect(const EffectView& view, EffectGeometry& out) override;

    // One particle as it is now, in the world.
    struct State {
        Vector3 position;
        Vector3 velocity;
        float age = 0.0f;
        float lifetime = 0.0f;
        float size = 0.0f; // half its width, before Squash
        float transparency = 0.0f;
        float squash = 0.0f;
        Color3 color;
        float rotation = 0.0f; // degrees, clockwise
    };
    // An emitter's particles, in no particular order (tests, tools).
    std::vector<State> particlesOf(const Instance& emitter) const;

    struct Stats {
        std::size_t emitters = 0;         // known, below the root
        std::size_t active = 0;           // emitting or with particles
        std::size_t particles = 0;        // alive
        std::size_t drawn = 0;            // quads the last collect made
        std::size_t batches = 0;          // emitters the last collect drew
        double stepMilliseconds = 0.0;    // CPU time of the last step
        double collectMilliseconds = 0.0; // and of the last collect
    };
    Stats stats() const;

private:
    struct Emitter;
    struct Particle;
    void watch(Instance& instance);
    void forget(Instance& instance);
    void activate(Emitter& emitter);
    void emit(Emitter& emitter, int count, float age);
    bool stepEmitter(Emitter& emitter, float dt, Vector3 eye);

    Instance* root_;
    TextureProvider textures_;
    ParticleSettings settings_;
    std::uint64_t random_;
    Connection added_;
    Connection removing_;
    std::unordered_map<const Instance*, std::unique_ptr<Emitter>> emitters_;
    std::vector<Emitter*> active_; // emitting or with particles
    std::size_t particles_ = 0;
    std::size_t drawn_ = 0;
    std::size_t batches_ = 0;
    double stepMilliseconds_ = 0.0;
    double collectMilliseconds_ = 0.0;
};

} // namespace lunar::render
