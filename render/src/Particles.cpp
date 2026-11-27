#include <lunar/render/Particles.hpp>

#include <lunar/Classes.hpp>
#include <lunar/render/Frustum.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

// How the client simulates and draws particles, from its particle shaders
// (ParticleLightingVS and ParticleLightingFS in shaders_glsl3.pack) and its
// emitter code (RBX::Graphics::Emitter, the EmitterShapeSampler classes):
//
// - Size, Transparency and Squash are sampled into 32-entry tables of their
//   low and high envelopes (value less and plus envelope) at even steps of
//   the particle's life, read with linear interpolation; each particle has
//   one random number that says where between the two it sits, the same
//   for all three. Color is a table too.
// - A quad's corners are its position plus the view's right and up (or the
//   orientation's axes) times its corner in [-1, 1] times the size, so a
//   particle is twice Size across; squash s widens it by 1 - s and flattens
//   it by as much when negative, narrows and stretches it by 1 + s when
//   positive; the rotation turns the squashed quad.
// - The rotation is the particle's Rotation plus RotSpeed times its age.
// - Box samples on the EmissionDirection face (Surface) or through the box
//   (Volume) and emits along EmissionDirection; Disc likewise, from an
//   ellipse whose hole is 1 - ShapePartial of it; Cylinder along
//   EmissionDirection, narrowing to ShapePartial of its radius at the far
//   end, emits outwards from its axis; Sphere emits outwards from its centre
//   over the cap ShapePartial of the way around from the pole, Volume pulling
//   points in by 1 - u^3. Inward turns the direction round, InAndOut picks
//   either at random.
// - SpreadAngle: with X (nearly) 0, a fan of Y either way in one plane;
//   otherwise X either way around the direction and up to Y off it.

namespace lunar::render {

namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr float degrees = pi / 180.0f;
constexpr std::size_t tableSize = 32;
constexpr float lastStep = 0.99999994f; // the client's cap on life fractions read from the tables

// A PCG32 stream: the same seed gives the same particles.
struct Random {
    std::uint64_t state = 0;

    explicit Random(std::uint64_t seed = 0) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) { next(); }

    std::uint32_t next()
    {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto shifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rotation = static_cast<std::uint32_t>(old >> 59u);
        return std::rotr(shifted, static_cast<int>(rotation));
    }
    // [0, 1)
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * unit(); }
    float range(const NumberRange& r) { return range(r.min, r.max); }
};

// The client's flipbook Random mode: a hash of the particle and the frame step.
std::uint32_t hashFrame(std::uint32_t seed, std::uint32_t step)
{
    std::uint32_t x = (seed << 16) + step + 0x55555555u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

struct Curve {
    std::array<float, tableSize> low{};
    std::array<float, tableSize> high{};

    // At life fraction t, `between` of the way from the low envelope to the high one.
    float at(float t, float between) const
    {
        const float x = std::clamp(t, 0.0f, lastStep) * static_cast<float>(tableSize - 1);
        const auto i = static_cast<std::size_t>(x);
        const float f = x - static_cast<float>(i);
        const float lo = low[i] + (low[i + 1] - low[i]) * f;
        const float hi = high[i] + (high[i + 1] - high[i]) * f;
        return lo + (hi - lo) * between;
    }
};

// The sequence's value and envelope at time x.
std::pair<float, float> evaluate(const NumberSequence& sequence, float x)
{
    const auto& points = sequence.keypoints;
    if (points.empty())
        return {0.0f, 0.0f};
    if (x <= points.front().time)
        return {points.front().value, points.front().envelope};
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].time) {
            const NumberSequenceKeypoint& a = points[i - 1];
            const NumberSequenceKeypoint& b = points[i];
            const float span = b.time - a.time;
            const float f = span > 0.0f ? (x - a.time) / span : 1.0f;
            return {a.value + (b.value - a.value) * f, a.envelope + (b.envelope - a.envelope) * f};
        }
    }
    return {points.back().value, points.back().envelope};
}

Color3 evaluate(const ColorSequence& sequence, float x)
{
    const auto& points = sequence.keypoints;
    if (points.empty())
        return {1.0f, 1.0f, 1.0f};
    if (x <= points.front().time)
        return points.front().value;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].time) {
            const ColorSequenceKeypoint& a = points[i - 1];
            const ColorSequenceKeypoint& b = points[i];
            const float span = b.time - a.time;
            const float f = span > 0.0f ? (x - a.time) / span : 1.0f;
            return {a.value.r + (b.value.r - a.value.r) * f, a.value.g + (b.value.g - a.value.g) * f,
                    a.value.b + (b.value.b - a.value.b) * f};
        }
    }
    return points.back().value;
}

Curve sample(const NumberSequence& sequence)
{
    Curve curve;
    for (std::size_t i = 0; i < tableSize; ++i) {
        const auto [value, envelope] = evaluate(sequence, static_cast<float>(i) / static_cast<float>(tableSize - 1));
        curve.low[i] = value - envelope;
        curve.high[i] = value + envelope;
    }
    return curve;
}

Vector3 axis(Enum::NormalId face)
{
    switch (face) {
    case Enum::NormalId::Right:
        return {1.0f, 0.0f, 0.0f};
    case Enum::NormalId::Top:
        return {0.0f, 1.0f, 0.0f};
    case Enum::NormalId::Back:
        return {0.0f, 0.0f, 1.0f};
    case Enum::NormalId::Left:
        return {-1.0f, 0.0f, 0.0f};
    case Enum::NormalId::Bottom:
        return {0.0f, -1.0f, 0.0f};
    case Enum::NormalId::Front:
        return {0.0f, 0.0f, -1.0f};
    }
    return {0.0f, 1.0f, 0.0f};
}

Vector3 unit(Vector3 v, Vector3 fallback = {0.0f, 1.0f, 0.0f})
{
    const float length = v.magnitude();
    return length > 1e-6f ? v / length : fallback;
}

// Two unit vectors at right angles to d and each other.
std::pair<Vector3, Vector3> perpendiculars(Vector3 d)
{
    const Vector3 helper = std::abs(d.y) < 0.9f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
    const Vector3 p = unit(d.cross(helper), {1.0f, 0.0f, 0.0f});
    return {p, d.cross(p)};
}

// Where an emitter emits from this step.
struct Source {
    CFrame cframe;
    Vector3 half; // the part's half size; zero for an attachment's point
    Vector3 velocity;
    bool point = false;
};

std::optional<Source> sourceOf(const ParticleEmitter& emitter)
{
    Instance* parent = emitter.parent();
    if (const auto* part = instanceCast<const BasePart>(parent))
        return Source{part->cframe(), part->size() * 0.5f, part->velocity(), false};
    if (const auto* attachment = instanceCast<const Attachment>(parent)) {
        Source source{attachment->worldCFrame(), {}, {}, true};
        if (const auto* part = instanceCast<const BasePart>(attachment->parent()))
            source.velocity = part->velocity();
        return source;
    }
    return std::nullopt;
}

// A point (in the source's space) and the direction a particle leaves it in,
// as EmitterShapeSampler does.
std::pair<Vector3, Vector3> sampleShape(const ParticleEmitter& emitter, const Source& source, Random& random)
{
    const Vector3 n = axis(emitter.emissionDirection());
    const bool surface = emitter.shapeStyle() == Enum::ParticleEmitterShapeStyle::Surface;
    const float partial = std::clamp(emitter.shapePartial(), 0.0f, 1.0f);
    // The part's other two axes, and its half sizes along each.
    const Vector3 a = std::abs(n.x) > 0.5f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
    const Vector3 b = n.cross(a).dot(n.cross(a)) > 0.5f ? unit(n.cross(a)) : Vector3{0.0f, 0.0f, 1.0f};
    auto along = [&](Vector3 v) {
        return std::abs(v.x) * source.half.x + std::abs(v.y) * source.half.y + std::abs(v.z) * source.half.z;
    };
    const float hn = along(n), ha = along(a), hb = along(b);
    auto turn = [&](Vector3 outward) {
        switch (emitter.shapeInOut()) {
        case Enum::ParticleEmitterShapeInOut::Outward:
            return outward;
        case Enum::ParticleEmitterShapeInOut::Inward:
            return -outward;
        case Enum::ParticleEmitterShapeInOut::InAndOut:
            return (random.next() & 1u) != 0 ? -outward : outward;
        }
        return outward;
    };
    // A point in the unit disc's ring from inner out, area-uniform.
    auto ring = [&](float inner) {
        const float angle = random.unit() * 2.0f * pi;
        const float r = std::sqrt(inner * inner + (1.0f - inner * inner) * random.unit());
        return std::pair{std::cos(angle) * r, std::sin(angle) * r};
    };
    if (source.point)
        return {{}, turn(n)};

    switch (emitter.shape()) {
    case Enum::ParticleEmitterShape::Box: {
        const float x = surface ? hn : random.range(-hn, hn);
        return {n * x + a * random.range(-ha, ha) + b * random.range(-hb, hb), turn(n)};
    }
    case Enum::ParticleEmitterShape::Disc: {
        const float x = surface ? hn : random.range(-hn, hn);
        const auto [u, v] = ring(1.0f - partial);
        return {n * x + a * (u * ha) + b * (v * hb), turn(n)};
    }
    case Enum::ParticleEmitterShape::Cylinder: {
        // How far up the axis: the client's stand-in for the inverse of the
        // cone's distribution, through the volume or over the side.
        const float u = random.unit();
        const float k = surface ? u * u : u * u * u;
        const float h = partial * (u - k) + k;
        const float radius = 1.0f + (partial - 1.0f) * h;
        const auto [x, y] = ring(surface ? 1.0f : 0.0f);
        const Vector3 radial = unit(a * x + b * y, a);
        return {n * ((2.0f * h - 1.0f) * hn) + a * (x * radius * ha) + b * (y * radius * hb), turn(radial)};
    }
    case Enum::ParticleEmitterShape::Sphere: {
        // Area-uniform over the cap around the pole at n. GUESS: the client
        // samples a mesh of the cap, so on long ellipsoids this is a little
        // denser at the ends.
        const float lowest = std::cos(pi * partial);
        const float c = random.range(lowest, 1.0f);
        const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
        const float angle = random.unit() * 2.0f * pi;
        Vector3 p = n * (c * hn) + a * (s * std::cos(angle) * ha) + b * (s * std::sin(angle) * hb);
        const Vector3 outward = unit(p, n);
        if (!surface) {
            const float u = random.unit();
            p = p * (1.0f - u * u * u);
        }
        return {p, turn(outward)};
    }
    }
    return {{}, turn(n)};
}

// The direction d spread by SpreadAngle (radians).
Vector3 spread(Vector3 d, Vector2 angles, Random& random)
{
    const float x = std::abs(angles.x);
    const float y = std::abs(angles.y);
    if (x < 0.001f && y < 0.001f)
        return d;
    const auto [p, q] = perpendiculars(d);
    if (x < 0.001f) {
        const float angle = random.range(-y, y);
        return d * std::cos(angle) + q * std::sin(angle);
    }
    const float around = random.range(-x, x);
    // Up to y off the plane, area-uniform within a quarter turn.
    const float lift = y <= pi * 0.5f ? std::asin(random.range(-std::sin(y), std::sin(y))) : random.range(-y, y);
    const Vector3 flat = d * std::cos(around) + p * std::sin(around);
    return flat * std::cos(lift) + q * std::sin(lift);
}

std::pair<int, int> flipbookGrid(const ParticleEmitter& emitter)
{
    switch (emitter.flipbookLayout()) {
    case Enum::ParticleFlipbookLayout::None:
        return {1, 1};
    case Enum::ParticleFlipbookLayout::Grid2x2:
        return {2, 2};
    case Enum::ParticleFlipbookLayout::Grid4x4:
        return {4, 4};
    case Enum::ParticleFlipbookLayout::Grid8x8:
        return {8, 8};
    case Enum::ParticleFlipbookLayout::Custom:
        return {std::clamp(emitter.flipbookSizeX(), 1, 64), std::clamp(emitter.flipbookSizeY(), 1, 64)};
    }
    return {1, 1};
}

} // namespace

struct ParticleSystem::Particle {
    Vector3 position; // in the world, or in the parent's space when locked
    float age = 0.0f;
    Vector3 velocity;
    float lifetime = 1.0f;
    float rotation = 0.0f; // radians, clockwise
    float rotSpeed = 0.0f;
    float between = 0.0f; // where it sits within the envelopes
    float framerate = 0.0f;
    float startFrame = 0.0f;
    std::uint32_t seed = 0;
};

struct ParticleSystem::Emitter {
    ParticleEmitter* instance = nullptr;
    Connection changed;
    Connection events;
    std::vector<Particle> particles;
    double owed = 0.0;   // fractions of a particle Rate has built up
    double burst = 0.0;  // Emit()s waiting for the next step
    bool clear = false;  // Clear() waiting for the next step
    bool listed = false; // in active_
    bool local = false;  // the particles are in the parent's space (LockedToPart)
    bool tablesStale = true;
    Curve size;
    Curve squash;
    Curve alpha;
    std::array<Color3, tableSize> color{};
    float reach = 0.0f; // the farthest a corner gets from its particle
    bool textureStale = true;
    ImagePtr texture;
    Random random;
    std::uint32_t emitted = 0;

    bool wanted() const
    {
        return !particles.empty() || burst >= 1.0 || clear
               || (instance->enabled() && instance->rate() > 0.0f && instance->parent() != nullptr);
    }

    void rebuildTables()
    {
        size = sample(instance->size());
        squash = sample(instance->squash());
        // Opacity: one less transparency, its envelopes swapped.
        const Curve transparency = sample(instance->transparency());
        for (std::size_t i = 0; i < tableSize; ++i) {
            alpha.low[i] = std::clamp(1.0f - transparency.high[i], 0.0f, 1.0f);
            alpha.high[i] = std::clamp(1.0f - transparency.low[i], 0.0f, 1.0f);
            color[i] = evaluate(instance->color(), static_cast<float>(i) / static_cast<float>(tableSize - 1));
        }
        float largest = 0.0f;
        float stretch = 1.0f;
        for (std::size_t i = 0; i < tableSize; ++i) {
            largest = std::max({largest, std::abs(size.low[i]), std::abs(size.high[i])});
            stretch = std::max({stretch, 1.0f + std::abs(squash.low[i]), 1.0f + std::abs(squash.high[i])});
        }
        reach = largest * stretch * std::numbers::sqrt2_v<float>;
        tablesStale = false;
    }
};

ParticleSystem::ParticleSystem(Instance& root, TextureProvider textures, std::uint64_t seed)
    : root_(&root), textures_(std::move(textures)), random_(seed)
{
    for (const InstancePtr& instance : root.descendants())
        watch(*instance);
    added_ = root.descendantAdded.connect([this](Instance* instance) { watch(*instance); });
    removing_ = root.descendantRemoving.connect([this](Instance* instance) { forget(*instance); });
}

ParticleSystem::~ParticleSystem()
{
    added_.disconnect();
    removing_.disconnect();
    for (auto& [instance, emitter] : emitters_) {
        emitter->changed.disconnect();
        emitter->events.disconnect();
    }
}

void ParticleSystem::watch(Instance& instance)
{
    auto* particleEmitter = instanceCast<ParticleEmitter>(&instance);
    if (!particleEmitter)
        return;
    auto [it, added] = emitters_.try_emplace(&instance);
    if (!added)
        return;
    it->second = std::make_unique<Emitter>();
    Emitter& emitter = *it->second;
    emitter.instance = particleEmitter;
    emitter.random = Random(random_ + emitters_.size() * 0x9E3779B97F4A7C15ULL);
    emitter.changed = instance.changed.connect([this, &emitter](std::string_view property) {
        if (property == "Size" || property == "Squash" || property == "Transparency" || property == "Color")
            emitter.tablesStale = true;
        else if (property == "Texture")
            emitter.textureStale = true;
        else if (property == "Enabled" || property == "Rate" || property == "Parent")
            activate(emitter);
    });
    emitter.events
        = instance.eventFired.connect([this, &emitter](std::string_view event, std::span<const Value> arguments) {
              if (event == "OnEmitRequested") {
                  double count = 16.0;
                  if (!arguments.empty())
                      if (const double* number = std::get_if<double>(&arguments[0]))
                          count = *number;
                  emitter.burst += std::max(0.0, std::floor(count));
                  activate(emitter);
              } else if (event == "OnClearRequested") {
                  emitter.clear = true;
                  activate(emitter);
              }
          });
    activate(emitter);
}

void ParticleSystem::forget(Instance& instance)
{
    auto it = emitters_.find(&instance);
    if (it == emitters_.end())
        return;
    Emitter* emitter = it->second.get();
    emitter->changed.disconnect();
    emitter->events.disconnect();
    particles_ -= emitter->particles.size();
    if (emitter->listed)
        std::erase(active_, emitter);
    emitters_.erase(it);
}

void ParticleSystem::activate(Emitter& emitter)
{
    if (!emitter.listed && emitter.wanted()) {
        emitter.listed = true;
        active_.push_back(&emitter);
    }
}

void ParticleSystem::step(float dt, Vector3 eye)
{
    const auto started = std::chrono::steady_clock::now();
    std::erase_if(active_, [&](Emitter* emitter) {
        if (stepEmitter(*emitter, dt, eye))
            return false;
        emitter->listed = false;
        return true;
    });
    stepMilliseconds_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void ParticleSystem::advance(float seconds, Vector3 eye)
{
    constexpr float frame = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += frame)
        step(std::min(frame, seconds - t), eye);
}

bool ParticleSystem::stepEmitter(Emitter& e, float dt, Vector3 eye)
{
    const ParticleEmitter& emitter = *e.instance;
    if (e.clear) {
        particles_ -= e.particles.size();
        e.particles.clear();
        e.clear = false;
    }
    if (e.tablesStale)
        e.rebuildTables();
    const float t = std::max(dt, 0.0f) * std::clamp(emitter.timeScale(), 0.0f, 1.0f);
    const std::optional<Source> source = sourceOf(emitter);

    // Locked particles live in the parent's space; a change of mind (or of
    // parent) moves them over with the part where it is now.
    // Without a parent to follow they stay where they are.
    const bool locked = emitter.lockedToPart();
    if (source && locked != e.local) {
        for (Particle& p : e.particles) {
            p.position
                = locked ? source->cframe.pointToObjectSpace(p.position) : source->cframe.pointToWorldSpace(p.position);
            p.velocity = locked ? source->cframe.vectorToObjectSpace(p.velocity)
                                : source->cframe.vectorToWorldSpace(p.velocity);
        }
        e.local = locked;
    }
    Vector3 acceleration = emitter.acceleration();
    Vector3 wind;
    if (emitter.windAffectsDrag())
        if (const auto* workspace = instanceCast<const Workspace>(root_))
            wind = workspace->globalWind();
    if (e.local && source) {
        acceleration = source->cframe.vectorToObjectSpace(acceleration);
        wind = source->cframe.vectorToObjectSpace(wind);
    }
    // Drag halves the speed every 1 / Drag seconds (GUESS: the client's
    // decay; with WindAffectsDrag, the speed relative to the wind).
    const float drag = emitter.drag();
    const float decay = drag != 0.0f ? std::exp2(-drag * t) : 1.0f;
    auto move = [&](Particle& p, float seconds, float factor) {
        p.velocity = p.velocity + acceleration * seconds;
        if (factor != 1.0f)
            p.velocity = wind + (p.velocity - wind) * factor;
        p.position = p.position + p.velocity * seconds;
    };
    if (t > 0.0f) {
        for (std::size_t i = 0; i < e.particles.size();) {
            Particle& p = e.particles[i];
            p.age += t;
            if (p.age >= p.lifetime) {
                p = e.particles.back();
                e.particles.pop_back();
                --particles_;
                continue;
            }
            move(p, t, decay);
            ++i;
        }
    }

    // New particles: Rate's share of this step spread over it (each moved on
    // by the part of the step since it was born) and Emit()'s all at once.
    int count = 0;
    std::vector<float> ages;
    const bool near
        = source && (source->cframe.position - eye).magnitude() <= settings_.emitDistance + source->half.magnitude();
    if (source && near) {
        const float density = std::max(settings_.density, 0.0f);
        if (emitter.enabled() && emitter.rate() > 0.0f && t > 0.0f) {
            e.owed += static_cast<double>(emitter.rate() * density * t);
            const int fromRate = static_cast<int>(std::floor(e.owed));
            e.owed -= fromRate;
            for (int k = 0; k < fromRate; ++k)
                ages.push_back(t * (static_cast<float>(k) + 0.5f) / static_cast<float>(fromRate));
            count += fromRate;
        }
        const int fromEmit = static_cast<int>(std::floor(e.burst * static_cast<double>(density) + 0.5));
        for (int k = 0; k < fromEmit; ++k)
            ages.push_back(0.0f);
        count += fromEmit;
    } else if (!emitter.enabled()) {
        e.owed = 0.0;
    }
    e.burst = 0.0;
    const std::size_t room = settings_.maxParticles > particles_ ? settings_.maxParticles - particles_ : 0;
    count = std::min<int>(count, static_cast<int>(std::min<std::size_t>(room, 1u << 20)));
    if (count > 0) {
        const auto [columns, rows] = flipbookGrid(emitter);
        const int frames = columns * rows;
        e.particles.reserve(e.particles.size() + static_cast<std::size_t>(count));
        for (int k = 0; k < count; ++k) {
            Particle p;
            p.lifetime = e.random.range(emitter.lifetime());
            if (!(p.lifetime > 0.0f))
                continue;
            auto [position, direction] = sampleShape(emitter, *source, e.random);
            direction = spread(direction, Vector2{emitter.spreadAngle().x * degrees, emitter.spreadAngle().y * degrees},
                               e.random);
            const Vector3 velocity = direction * e.random.range(emitter.speed());
            const Vector3 inherited = source->velocity * emitter.velocityInheritance();
            if (e.local) {
                p.position = position;
                p.velocity = velocity + source->cframe.vectorToObjectSpace(inherited);
            } else {
                p.position = source->cframe.pointToWorldSpace(position);
                p.velocity = source->cframe.vectorToWorldSpace(velocity) + inherited;
            }
            p.rotation = e.random.range(emitter.rotation()) * degrees;
            p.rotSpeed = e.random.range(emitter.rotSpeed()) * degrees;
            p.between = e.random.unit();
            p.framerate = e.random.range(emitter.flipbookFramerate());
            p.startFrame = emitter.flipbookStartRandom() && frames > 1
                               ? std::floor(e.random.unit() * static_cast<float>(frames))
                               : 0.0f;
            p.seed = e.emitted++;
            const float age = ages[static_cast<std::size_t>(k)];
            if (age >= p.lifetime)
                continue;
            if (age > 0.0f) {
                p.age = age;
                move(p, age, drag != 0.0f ? std::exp2(-drag * age) : 1.0f);
            }
            e.particles.push_back(p);
            ++particles_;
        }
    }
    return e.wanted();
}

void ParticleSystem::collect(const EffectView& view, EffectGeometry& out)
{
    const auto started = std::chrono::steady_clock::now();
    drawn_ = 0;
    batches_ = 0;
    const Camera& camera = view.camera;
    const Frustum frustum(view.viewProjection);
    const Vector3 eye = camera.cframe.position;
    const Vector3 right = camera.cframe.rightVector();
    const Vector3 up = camera.cframe.upVector();
    const Vector3 forward = camera.cframe.lookVector();
    struct Placed {
        Vector3 position;
        Vector3 velocity;
        float depth;
        std::uint32_t index;
    };
    std::vector<Placed> placed;
    for (Emitter* e : active_) {
        if (e->particles.empty())
            continue;
        const ParticleEmitter& emitter = *e->instance;
        if (e->tablesStale)
            e->rebuildTables();
        const std::optional<Source> source = e->local ? sourceOf(emitter) : std::nullopt;
        if (e->local && !source)
            continue;

        // Where they are, and whether any of them can be seen.
        placed.clear();
        Vector3 low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
        for (std::size_t i = 0; i < e->particles.size(); ++i) {
            const Particle& p = e->particles[i];
            Placed at{p.position, p.velocity, 0.0f, static_cast<std::uint32_t>(i)};
            if (source) {
                at.position = source->cframe.pointToWorldSpace(p.position);
                at.velocity = source->cframe.vectorToWorldSpace(p.velocity);
            }
            at.depth = (at.position - eye).dot(forward);
            low = {std::min(low.x, at.position.x), std::min(low.y, at.position.y), std::min(low.z, at.position.z)};
            high = {std::max(high.x, at.position.x), std::max(high.y, at.position.y), std::max(high.z, at.position.z)};
            placed.push_back(at);
        }
        const Vector3 center = (low + high) * 0.5f;
        const float radius = (high - low).magnitude() * 0.5f + e->reach;
        if (!frustum.intersects(center, radius) || (center - eye).magnitude() - radius > view.drawDistance)
            continue;

        // Its texture, once it's there (a white square in the meantime
        // would look wrong); no texture draws plain quads.
        if (e->textureStale || (!e->texture && !emitter.texture().empty())) {
            e->texture = emitter.texture().empty() ? nullptr : textures_(emitter.texture());
            e->textureStale = false;
        }
        if (!e->texture && !emitter.texture().empty())
            continue;

        // Back to front within the emitter, as the client sorts them.
        std::ranges::sort(placed, std::greater{}, &Placed::depth);

        const auto [columns, rows] = flipbookGrid(emitter);
        const int frames = columns * rows;
        const bool flipbook = frames > 1;
        const Enum::ParticleFlipbookMode mode = emitter.flipbookMode();
        const bool blendFrames = emitter.flipbookBlendFrames();
        const float fade = 1.0f - std::clamp(emitter.localTransparencyModifier(), 0.0f, 1.0f);
        const Enum::ParticleOrientation orientation = emitter.orientation();
        Vector3 partX{1.0f, 0.0f, 0.0f}, partY{0.0f, 1.0f, 0.0f};
        if (orientation == Enum::ParticleOrientation::VelocityPerpendicular)
            if (const std::optional<Source> where = sourceOf(emitter)) {
                partX = where->cframe.rightVector();
                partY = where->cframe.upVector();
            }

        const auto firstIndex = static_cast<std::uint32_t>(out.indices.size());
        std::uint32_t quads = 0;
        for (const Placed& at : placed) {
            const Particle& p = e->particles[at.index];
            const float life = std::clamp(p.age / p.lifetime, 0.0f, lastStep);
            const float size = e->size.at(life, p.between);
            const float opacity = e->alpha.at(life, p.between) * fade;
            if (opacity <= 0.0f || size == 0.0f)
                continue;
            const float squash = e->squash.at(life, p.between);
            const float x = life * static_cast<float>(tableSize - 1);
            const auto i = static_cast<std::size_t>(x);
            const float f = x - static_cast<float>(i);
            const Color3 color{e->color[i].r + (e->color[i + 1].r - e->color[i].r) * f,
                               e->color[i].g + (e->color[i + 1].g - e->color[i].g) * f,
                               e->color[i].b + (e->color[i + 1].b - e->color[i].b) * f};

            // The quad's axes (ParticleLightingVS).
            Vector3 axisX = right, axisY = up;
            if (orientation == Enum::ParticleOrientation::FacingCameraWorldUp) {
                axisY = {0.0f, 1.0f, 0.0f};
            } else if (orientation == Enum::ParticleOrientation::VelocityParallel
                       || orientation == Enum::ParticleOrientation::VelocityPerpendicular) {
                const float speed = at.velocity.magnitude();
                if (speed <= 1e-4f)
                    continue; // the client's quad collapses to its centre
                const Vector3 v = at.velocity / speed;
                if (orientation == Enum::ParticleOrientation::VelocityParallel) {
                    axisX = v;
                    axisY = unit(v.cross(forward), up);
                } else {
                    Vector3 side = partX.cross(v);
                    if (side.dot(side) > 0.02f) {
                        side = side / std::sqrt(side.dot(side));
                    } else {
                        const Vector3 flat = partY - v * partY.dot(v);
                        side = flat.dot(flat) > 1e-12f ? unit(flat) : Vector3{0.0f, 1.0f, 0.0f};
                    }
                    axisX = v.cross(side);
                    axisY = side;
                }
            }
            // Squash, then the turn (clockwise for positive Rotation and
            // RotSpeed; GUESS: the client's sign).
            const float wide = squash <= 0.0f ? 1.0f - squash : 1.0f / (1.0f + squash);
            const float tall = squash <= 0.0f ? 1.0f / (1.0f - squash) : 1.0f + squash;
            const float angle = -(p.rotation + p.rotSpeed * p.age);
            const float c = std::cos(angle), s = std::sin(angle);
            const float row0[2] = {c * size * wide, -s * size * tall};
            const float row1[2] = {s * size * wide, c * size * tall};

            // Which flipbook frames show, and how far to the next.
            float frame = 0.0f, next = 0.0f, blend = 0.0f;
            if (flipbook) {
                const auto count = static_cast<float>(frames);
                const float played = p.startFrame + p.age * p.framerate;
                const float step = std::floor(played);
                blend = played - step;
                switch (mode) {
                case Enum::ParticleFlipbookMode::Loop:
                    frame = std::fmod(step, count);
                    next = std::fmod(step + 1.0f, count);
                    break;
                case Enum::ParticleFlipbookMode::OneShot: {
                    const float through = life * count;
                    frame = std::min(std::floor(through), count - 1.0f);
                    next = std::min(frame + 1.0f, count - 1.0f);
                    blend = through - std::floor(through);
                    break;
                }
                case Enum::ParticleFlipbookMode::PingPong: {
                    const float period = std::max(2.0f * count - 2.0f, 1.0f);
                    auto bounce = [&](float k) {
                        const float m = std::fmod(k, period);
                        return m < count ? m : period - m;
                    };
                    frame = bounce(step);
                    next = bounce(step + 1.0f);
                    break;
                }
                case Enum::ParticleFlipbookMode::Random: {
                    const auto cycle = static_cast<std::uint32_t>(std::max(step, 0.0f));
                    frame = static_cast<float>(hashFrame(p.seed, cycle) % static_cast<std::uint32_t>(frames));
                    next = static_cast<float>(hashFrame(p.seed, cycle + 1) % static_cast<std::uint32_t>(frames));
                    break;
                }
                }
                if (!blendFrames)
                    blend = 0.0f;
            }
            const float cellU = 1.0f / static_cast<float>(columns), cellV = 1.0f / static_cast<float>(rows);
            const float frameU = std::fmod(frame, static_cast<float>(columns)) * cellU;
            const float frameV = std::floor(frame / static_cast<float>(columns)) * cellV;
            const float nextU = std::fmod(next, static_cast<float>(columns)) * cellU;
            const float nextV = std::floor(next / static_cast<float>(columns)) * cellV;

            // Corners (0, 0), (1, 0), (0, 1), (1, 1), as -1 to 1 along the
            // axes; v runs down the texture.
            const auto base = static_cast<std::uint32_t>(out.vertices.size());
            for (int corner = 0; corner < 4; ++corner) {
                const float cx = static_cast<float>(corner & 1);
                const float cy = static_cast<float>(corner >> 1);
                const float dx = 2.0f * cx - 1.0f, dy = 2.0f * cy - 1.0f;
                EffectVertex& v = out.vertices.emplace_back();
                v.position
                    = at.position + axisX * (dx * row0[0] + dy * row0[1]) + axisY * (dx * row1[0] + dy * row1[1]);
                v.uvq[0] = frameU + cx * cellU;
                v.uvq[1] = frameV + (1.0f - cy) * cellV;
                v.uv2[0] = nextU + cx * cellU;
                v.uv2[1] = nextV + (1.0f - cy) * cellV;
                v.frameBlend = blend;
                v.color[0] = color.r;
                v.color[1] = color.g;
                v.color[2] = color.b;
                v.color[3] = opacity;
            }
            for (std::uint32_t k : {0u, 1u, 2u, 2u, 1u, 3u})
                out.indices.push_back(base + k);
            ++quads;
        }
        if (quads == 0)
            continue;
        out.batches.push_back({
            .firstIndex = firstIndex,
            .indexCount = quads * 6,
            .texture = e->texture,
            .lightEmission = emitter.lightEmission(),
            .lightInfluence = emitter.lightInfluence(),
            .brightness = emitter.brightness(),
            .center = center,
            .radius = radius,
            .depthOffset = emitter.zOffset(),
            .clampUv = true,
            .toneMapped = false,
        });
        drawn_ += quads;
        ++batches_;
    }
    collectMilliseconds_
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

std::vector<ParticleSystem::State> ParticleSystem::particlesOf(const Instance& instance) const
{
    std::vector<State> out;
    auto it = emitters_.find(&instance);
    if (it == emitters_.end())
        return out;
    Emitter& e = *it->second;
    if (e.tablesStale)
        e.rebuildTables();
    const std::optional<Source> source = e.local ? sourceOf(*e.instance) : std::nullopt;
    for (const Particle& p : e.particles) {
        const float life = std::clamp(p.age / p.lifetime, 0.0f, lastStep);
        const float x = life * static_cast<float>(tableSize - 1);
        const auto i = static_cast<std::size_t>(x);
        const float f = x - static_cast<float>(i);
        State state;
        state.position = source ? source->cframe.pointToWorldSpace(p.position) : p.position;
        state.velocity = source ? source->cframe.vectorToWorldSpace(p.velocity) : p.velocity;
        state.age = p.age;
        state.lifetime = p.lifetime;
        state.size = e.size.at(life, p.between);
        state.transparency = 1.0f - e.alpha.at(life, p.between);
        state.squash = e.squash.at(life, p.between);
        state.color = {e.color[i].r + (e.color[i + 1].r - e.color[i].r) * f,
                       e.color[i].g + (e.color[i + 1].g - e.color[i].g) * f,
                       e.color[i].b + (e.color[i + 1].b - e.color[i].b) * f};
        state.rotation = (p.rotation + p.rotSpeed * p.age) / degrees;
        out.push_back(state);
    }
    return out;
}

ParticleSystem::Stats ParticleSystem::stats() const
{
    return {emitters_.size(), active_.size(), particles_, drawn_, batches_, stepMilliseconds_, collectMilliseconds_};
}

} // namespace lunar::render
