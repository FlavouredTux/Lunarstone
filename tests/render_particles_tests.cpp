#include "render_gpu.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Particles.hpp>
#include <lunar/render/Primitives.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::render;

namespace {

constexpr float frame = 1.0f / 60.0f;

std::shared_ptr<Part> addPart(Instance* parent, Vector3 position, Vector3 size = {2.0f, 2.0f, 2.0f})
{
    auto part = std::make_shared<Part>();
    part->setPosition(position);
    part->setSize(size);
    part->setParent(parent);
    return part;
}

// An emitter that's easy to reason about: no spread, one speed, long lives.
std::shared_ptr<ParticleEmitter> addEmitter(Instance* parent, float rate = 0.0f)
{
    auto emitter = std::make_shared<ParticleEmitter>();
    emitter->setTexture("");
    emitter->setRate(rate);
    emitter->setEnabled(rate > 0.0f);
    emitter->setLifetime({100.0f, 100.0f});
    emitter->setSpeed({5.0f, 5.0f});
    emitter->setParent(parent);
    return emitter;
}

void run(ParticleSystem& particles, float seconds, Vector3 eye = {})
{
    for (float t = 0.0f; t < seconds - 1e-4f; t += frame)
        particles.step(frame, eye);
}

void emit(ParticleEmitter& emitter, double count)
{
    const Value arguments[1] = {count};
    emitter.fireEvent("OnEmitRequested", arguments);
}

bool near(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

bool near(Vector3 a, Vector3 b, float tolerance = 1e-3f)
{
    return (a - b).magnitude() <= tolerance;
}

} // namespace

TEST(particles_emit_at_rate)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get(), 30.0f);
    ParticleSystem particles(*game->workspace(), {});
    run(particles, 1.0f);
    const std::size_t count = particles.particlesOf(*emitter).size();
    CHECK(count >= 29 && count <= 30);
    run(particles, 1.0f);
    CHECK(particles.particlesOf(*emitter).size() >= 59);
}

TEST(particles_disabled_emitters_cost_nothing)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    std::vector<std::shared_ptr<ParticleEmitter>> emitters;
    for (int i = 0; i < 1000; ++i)
        emitters.push_back(addEmitter(part.get()));
    ParticleSystem particles(*game->workspace(), {});
    CHECK_EQ(particles.stats().emitters, 1000u);
    CHECK_EQ(particles.stats().active, 0u);
    run(particles, 0.5f);
    CHECK_EQ(particles.stats().particles, 0u);

    // Enabling one wakes it, and only it.
    emitters[7]->setRate(10.0f);
    emitters[7]->setEnabled(true);
    run(particles, 1.0f);
    CHECK_EQ(particles.stats().active, 1u);
    CHECK(particles.stats().particles > 0);
}

TEST(particles_emit_and_clear_arrive_as_events)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get()); // disabled: Emit works anyway
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 25);
    particles.step(frame, {});
    CHECK_EQ(particles.particlesOf(*emitter).size(), 25u);
    emitter->fireEvent("OnEmitRequested"); // no count: 16
    particles.step(frame, {});
    CHECK_EQ(particles.particlesOf(*emitter).size(), 41u);
    emitter->fireEvent("OnClearRequested");
    particles.step(frame, {});
    CHECK_EQ(particles.particlesOf(*emitter).size(), 0u);
    CHECK_EQ(particles.stats().particles, 0u);
}

TEST(particles_die_at_the_end_of_their_lifetime)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setLifetime({0.5f, 0.5f});
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 10);
    run(particles, 0.4f);
    CHECK_EQ(particles.particlesOf(*emitter).size(), 10u);
    run(particles, 0.2f);
    CHECK_EQ(particles.particlesOf(*emitter).size(), 0u);
    // Done: the emitter drops out of the frame's work.
    CHECK_EQ(particles.stats().active, 0u);
}

TEST(particles_leave_along_the_emission_direction_of_the_part)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {0.0f, 10.0f, 0.0f});
    // Turned a quarter around Z: the part's top faces world -X.
    part->setCFrame(CFrame(0.0f, 10.0f, 0.0f) * CFrame::angles(0.0f, 0.0f, std::numbers::pi_v<float> / 2.0f));
    auto emitter = addEmitter(part.get());
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 5);
    particles.step(frame, {});
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter))
        CHECK(near(p.velocity, part->cframe().upVector() * 5.0f));

    emitter->setEmissionDirection(Enum::NormalId::Front);
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 5);
    particles.step(frame, {});
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter))
        CHECK(near(p.velocity, part->cframe().lookVector() * 5.0f));
}

TEST(particles_fall_by_acceleration_and_slow_by_drag)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setAcceleration({0.0f, -10.0f, 0.0f});
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    run(particles, 1.0f);
    const ParticleSystem::State p = particles.particlesOf(*emitter).at(0);
    // Up at 5, less 10 a second for a second (Emit's are born at the end of the step).
    CHECK(near(p.velocity.y, 5.0f - 10.0f, 1e-2f));

    // Drag 1 halves the speed every second.
    emitter->setAcceleration({});
    emitter->setDrag(1.0f);
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 1);
    particles.step(frame, {});
    const float start = particles.particlesOf(*emitter).at(0).velocity.magnitude();
    run(particles, 1.0f);
    CHECK(near(particles.particlesOf(*emitter).at(0).velocity.magnitude(), start * 0.5f, 1e-2f));
}

TEST(particles_drag_towards_the_wind)
{
    auto game = DataModel::create();
    game->workspace()->setGlobalWind({10.0f, 0.0f, 0.0f});
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setDrag(5.0f);
    emitter->setWindAffectsDrag(true);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    run(particles, 3.0f);
    CHECK(near(particles.particlesOf(*emitter).at(0).velocity, {10.0f, 0.0f, 0.0f}, 1e-2f));
}

TEST(particles_locked_to_the_part_move_with_it)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setLockedToPart(true);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 3);
    particles.step(frame, {});
    const std::vector<ParticleSystem::State> before = particles.particlesOf(*emitter);
    part->setPosition({20.0f, 0.0f, 0.0f});
    particles.step(frame, {});
    const std::vector<ParticleSystem::State> after = particles.particlesOf(*emitter);
    CHECK_EQ(after.size(), 3u);
    for (std::size_t i = 0; i < after.size(); ++i)
        CHECK(near(after[i].position, before[i].position + Vector3{20.0f, 0.0f, 0.0f}));

    // Unlocked, they stay behind.
    emitter->setLockedToPart(false);
    particles.step(frame, {});
    part->setPosition({});
    particles.step(frame, {});
    for (std::size_t i = 0; i < after.size(); ++i)
        CHECK(near(particles.particlesOf(*emitter)[i].position, after[i].position));
}

TEST(particles_inherit_the_parts_velocity)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    part->setAssemblyLinearVelocity({0.0f, 0.0f, 8.0f});
    auto emitter = addEmitter(part.get());
    emitter->setVelocityInheritance(0.5f);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    CHECK(near(particles.particlesOf(*emitter).at(0).velocity, {0.0f, 5.0f, 4.0f}));
}

TEST(particles_come_from_the_box_face_or_through_it)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {0.0f, 0.0f, 0.0f}, {4.0f, 2.0f, 6.0f});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 200);
    particles.step(frame, {});
    bool spread = false;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        CHECK(near(p.position.y, 1.0f)); // the top face
        CHECK(std::abs(p.position.x) <= 2.0f && std::abs(p.position.z) <= 3.0f);
        spread |= std::abs(p.position.z) > 2.0f;
    }
    CHECK(spread);

    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Volume);
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 200);
    particles.step(frame, {});
    float lowest = 1.0f;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        CHECK(std::abs(p.position.y) <= 1.0f && std::abs(p.position.x) <= 2.0f && std::abs(p.position.z) <= 3.0f);
        lowest = std::min(lowest, p.position.y);
    }
    CHECK(lowest < 0.0f);
}

TEST(particles_from_a_sphere_leave_outwards_or_inwards)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {0.0f, 5.0f, 0.0f}, {4.0f, 4.0f, 4.0f});
    auto emitter = addEmitter(part.get());
    emitter->setShape(Enum::ParticleEmitterShape::Sphere);
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 100);
    particles.step(frame, {});
    bool below = false;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        const Vector3 offset = p.position - Vector3{0.0f, 5.0f, 0.0f};
        CHECK(near(offset.magnitude(), 2.0f, 1e-2f));
        CHECK(near(p.velocity, offset.unit() * 5.0f, 1e-2f));
        below |= offset.y < -1.0f;
    }
    CHECK(below);

    // Half a sphere: the cap above the equator (EmissionDirection Top), inwards.
    emitter->setShapePartial(0.5f);
    emitter->setShapeInOut(Enum::ParticleEmitterShapeInOut::Inward);
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 100);
    particles.step(frame, {});
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        const Vector3 offset = p.position - Vector3{0.0f, 5.0f, 0.0f};
        CHECK(offset.y >= -1e-3f);
        CHECK(near(p.velocity, offset.unit() * -5.0f, 1e-2f));
    }
}

TEST(particles_from_a_disc_keep_out_of_its_hole)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {}, {10.0f, 1.0f, 10.0f});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setShape(Enum::ParticleEmitterShape::Disc);
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    emitter->setShapePartial(0.4f); // the inner 60% is a hole
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 200);
    particles.step(frame, {});
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        const float r = std::hypot(p.position.x, p.position.z);
        CHECK(r >= 3.0f - 1e-3f && r <= 5.0f + 1e-3f);
        CHECK(near(p.position.y, 0.5f));
    }
}

TEST(particles_from_a_cylinder_leave_away_from_its_axis)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {}, {2.0f, 6.0f, 2.0f});
    auto emitter = addEmitter(part.get());
    emitter->setShape(Enum::ParticleEmitterShape::Cylinder);
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 100);
    particles.step(frame, {});
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        const Vector3 start = p.position;
        CHECK(near(std::hypot(start.x, start.z), 1.0f, 1e-2f));
        CHECK(std::abs(start.y) <= 3.0f + 1e-3f);
        CHECK(near(p.velocity.y, 0.0f));
        CHECK(near(p.velocity.magnitude(), 5.0f));
    }
}

TEST(particles_spread_within_the_spread_angle)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSpreadAngle({30.0f, 30.0f});
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 300);
    particles.step(frame, {});
    float widest = 0.0f;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        const float angle
            = std::acos(std::clamp(p.velocity.unit().y, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
        CHECK(angle <= 43.0f); // 30 either way, both ways at once
        widest = std::max(widest, angle);
    }
    CHECK(widest > 20.0f);

    // A full turn both ways: every way, down too.
    emitter->setSpreadAngle({180.0f, 180.0f});
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 300);
    particles.step(frame, {});
    bool down = false;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter))
        down |= p.velocity.y < -4.0f;
    CHECK(down);
}

TEST(particles_take_size_transparency_and_color_over_their_life)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setLifetime({2.0f, 2.0f});
    emitter->setSize(NumberSequence({{0.0f, 1.0f, 0.0f}, {1.0f, 3.0f, 0.0f}}));
    emitter->setTransparency(NumberSequence({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}));
    emitter->setColor(ColorSequence({{0.0f, {1.0f, 0.0f, 0.0f}}, {1.0f, {0.0f, 0.0f, 1.0f}}}));
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    run(particles, 1.0f);
    const ParticleSystem::State p = particles.particlesOf(*emitter).at(0);
    CHECK(near(p.age, 1.0f));
    CHECK(near(p.size, 2.0f, 1e-2f));
    CHECK(near(p.transparency, 0.5f, 1e-2f));
    CHECK(near(p.color.r, 0.5f, 1e-2f) && near(p.color.b, 0.5f, 1e-2f));
}

TEST(particles_sit_within_the_envelopes)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSize(NumberSequence({{0.0f, 2.0f, 1.0f}, {1.0f, 2.0f, 1.0f}}));
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 200);
    particles.step(frame, {});
    float smallest = 10.0f, largest = 0.0f;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter)) {
        smallest = std::min(smallest, p.size);
        largest = std::max(largest, p.size);
    }
    CHECK(smallest >= 1.0f && largest <= 3.0f);
    CHECK(smallest < 1.2f && largest > 2.8f);
}

TEST(particles_turn_by_rotation_and_rotspeed)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setRotation({30.0f, 30.0f});
    emitter->setRotSpeed({90.0f, 90.0f});
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    run(particles, 1.0f);
    CHECK(near(particles.particlesOf(*emitter).at(0).rotation, 120.0f, 0.1f));
}

TEST(particles_follow_time_scale)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get(), 60.0f);
    emitter->setTimeScale(0.0f);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    run(particles, 1.0f);
    // Frozen: nothing from Rate, and Emit's particle never moved or aged.
    const std::vector<ParticleSystem::State> frozen = particles.particlesOf(*emitter);
    CHECK_EQ(frozen.size(), 1u);
    CHECK_EQ(frozen[0].age, 0.0f);

    emitter->setTimeScale(0.5f);
    run(particles, 1.0f);
    const std::size_t count = particles.particlesOf(*emitter).size();
    CHECK(count >= 30 && count <= 31);
}

TEST(particles_come_from_an_attachment)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {0.0f, 10.0f, 0.0f});
    auto attachment = std::make_shared<Attachment>();
    attachment->setCFrame(CFrame(3.0f, 0.0f, 0.0f) * CFrame::angles(std::numbers::pi_v<float> / 2.0f, 0.0f, 0.0f));
    attachment->setParent(part);
    auto emitter = addEmitter(attachment.get());
    emitter->setSpeed({0.0f, 0.0f});
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 4);
    particles.step(frame, {});
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter))
        CHECK(near(p.position, {3.0f, 10.0f, 0.0f}));
    // Its Top is the attachment's: turned a quarter around X, that's +Z.
    emitter->setSpeed({1.0f, 1.0f});
    emit(*emitter, 1);
    particles.step(frame, {});
    bool along = false;
    for (const ParticleSystem::State& p : particles.particlesOf(*emitter))
        along |= near(p.velocity, {0.0f, 0.0f, 1.0f});
    CHECK(along);
}

TEST(particles_need_a_part_or_attachment)
{
    auto game = DataModel::create();
    auto folder = std::make_shared<Folder>();
    folder->setParent(game->workspace());
    auto emitter = addEmitter(folder.get(), 50.0f);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 5);
    run(particles, 1.0f);
    CHECK_EQ(particles.stats().particles, 0u);
}

TEST(particles_stop_at_the_budget)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    ParticleSystem particles(*game->workspace(), {});
    particles.setSettings({.maxParticles = 50});
    emit(*emitter, 80);
    particles.step(frame, {});
    CHECK_EQ(particles.stats().particles, 50u);

    // Density thins out what's emitted.
    particles.setSettings({.density = 0.5f});
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 80);
    particles.step(frame, {});
    CHECK_EQ(particles.stats().particles, 40u);
}

TEST(particles_far_away_do_not_emit)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {5000.0f, 0.0f, 0.0f});
    auto emitter = addEmitter(part.get(), 60.0f);
    ParticleSystem particles(*game->workspace(), {});
    run(particles, 0.5f, {});
    CHECK_EQ(particles.stats().particles, 0u);
    run(particles, 0.5f, {4990.0f, 0.0f, 0.0f});
    CHECK(particles.stats().particles > 0);
}

TEST(particles_repeat_with_the_same_seed)
{
    auto make = [] {
        auto game = DataModel::create();
        auto part = addPart(game->workspace(), {});
        auto emitter = addEmitter(part.get(), 40.0f);
        emitter->setSpreadAngle({45.0f, 20.0f});
        emitter->setShape(Enum::ParticleEmitterShape::Sphere);
        ParticleSystem particles(*game->workspace(), {}, 7);
        run(particles, 1.0f);
        std::vector<Vector3> positions;
        for (const ParticleSystem::State& p : particles.particlesOf(*emitter))
            positions.push_back(p.position);
        return positions;
    };
    const std::vector<Vector3> a = make();
    CHECK(!a.empty());
    CHECK(a == make());
}

TEST(particles_forget_emitters_that_leave)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get(), 60.0f);
    ParticleSystem particles(*game->workspace(), {});
    run(particles, 0.5f);
    CHECK(particles.stats().particles > 0);
    emitter->destroy();
    particles.step(frame, {});
    CHECK_EQ(particles.stats().emitters, 0u);
    CHECK_EQ(particles.stats().particles, 0u);
    CHECK_EQ(particles.stats().active, 0u);

    // New ones are picked up.
    addEmitter(part.get(), 60.0f);
    run(particles, 0.5f);
    CHECK_EQ(particles.stats().emitters, 1u);
    CHECK(particles.stats().particles > 0);
}

namespace {

// A camera 10 studs back from the origin looking down -Z.
render::Camera frontCamera()
{
    render::Camera camera;
    camera.cframe = CFrame(0.0f, 0.0f, 10.0f);
    return camera;
}

EffectView viewOf(const render::Camera& camera, float aspect = 1.0f)
{
    return {.camera = camera, .viewProjection = camera.viewProjection(aspect)};
}

} // namespace

TEST(particles_face_the_camera_twice_their_size_across)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    emitter->setSize(NumberSequence(1.5f));
    emitter->setZOffset(2.0f);
    emitter->setLightEmission(0.25f);
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    const Vector3 at = particles.particlesOf(*emitter).at(0).position;

    EffectGeometry geometry;
    particles.collect(viewOf(frontCamera()), geometry);
    CHECK_EQ(geometry.batches.size(), 1u);
    CHECK_EQ(geometry.vertices.size(), 4u);
    CHECK_EQ(geometry.indices.size(), 6u);
    const EffectBatch& batch = geometry.batches[0];
    CHECK_EQ(batch.depthOffset, 2.0f);
    CHECK_EQ(batch.lightEmission, 0.25f);
    CHECK(batch.clampUv && !batch.toneMapped);
    // Corners (0, 0) to (1, 1) along the view's right and up, v down the texture.
    const std::vector<EffectVertex>& v = geometry.vertices;
    CHECK(near(v[0].position, at + Vector3{-1.5f, -1.5f, 0.0f}));
    CHECK(near(v[3].position, at + Vector3{1.5f, 1.5f, 0.0f}));
    CHECK(near(v[0].uvq[0], 0.0f) && near(v[0].uvq[1], 1.0f));
    CHECK(near(v[3].uvq[0], 1.0f) && near(v[3].uvq[1], 0.0f));
}

TEST(particles_squash_and_turn)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    emitter->setSize(NumberSequence(1.0f));
    emitter->setSquash(NumberSequence(1.0f)); // half as wide, twice as tall
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    const Vector3 at = particles.particlesOf(*emitter).at(0).position;
    EffectGeometry geometry;
    particles.collect(viewOf(frontCamera()), geometry);
    CHECK(near(geometry.vertices[3].position, at + Vector3{0.5f, 2.0f, 0.0f}));

    // A quarter turn clockwise: the top right corner goes to the bottom right.
    emitter->setSquash(NumberSequence(0.0f));
    emitter->setRotation({90.0f, 90.0f});
    emitter->fireEvent("OnClearRequested");
    emit(*emitter, 1);
    particles.step(frame, {});
    geometry.clear();
    particles.collect(viewOf(frontCamera()), geometry);
    const Vector3 turned = particles.particlesOf(*emitter).at(0).position;
    CHECK(near(geometry.vertices[3].position, turned + Vector3{1.0f, -1.0f, 0.0f}));
}

TEST(particles_play_flipbook_frames)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setFlipbookLayout(Enum::ParticleFlipbookLayout::Grid4x4);
    emitter->setFlipbookFramerate({10.0f, 10.0f});
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 1);
    particles.step(frame, {});
    run(particles, 0.65f); // 6.5 frames in: frame 6, half way to 7
    EffectGeometry geometry;
    particles.collect(viewOf(frontCamera()), geometry);
    const EffectVertex& corner = geometry.vertices.at(0); // (0, 0): u at the cell's left, v at its bottom
    CHECK(near(corner.uvq[0], 0.5f) && near(corner.uvq[1], 0.5f));
    CHECK(near(corner.uv2[0], 0.75f) && near(corner.uv2[1], 0.5f));
    CHECK(near(corner.frameBlend, 0.5f, 0.02f));

    // OneShot plays the 16 frames once over the life (here 100 s).
    emitter->setFlipbookMode(Enum::ParticleFlipbookMode::OneShot);
    emitter->setFlipbookBlendFrames(false);
    geometry.clear();
    particles.collect(viewOf(frontCamera()), geometry);
    CHECK(near(geometry.vertices.at(0).uvq[0], 0.0f) && near(geometry.vertices.at(0).uvq[1], 0.25f));
    CHECK_EQ(geometry.vertices.at(0).frameBlend, 0.0f);
}

TEST(particles_out_of_view_are_not_drawn)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {0.0f, 0.0f, 50.0f}); // behind the camera
    auto emitter = addEmitter(part.get());
    ParticleSystem particles(*game->workspace(), {});
    emit(*emitter, 10);
    particles.step(frame, {});
    EffectGeometry geometry;
    particles.collect(viewOf(frontCamera()), geometry);
    CHECK(geometry.batches.empty());
    CHECK_EQ(particles.stats().drawn, 0u);
}

TEST(particles_wait_for_their_texture)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    auto emitter = addEmitter(part.get());
    emitter->setTexture("rbxassetid://1");
    bool loaded = false;
    auto image = std::make_shared<lunar::Image>();
    image->width = image->height = 1;
    image->rgba = {255, 255, 255, 255};
    ParticleSystem particles(*game->workspace(),
                             [&](std::string_view) -> ImagePtr { return loaded ? image : nullptr; });
    emit(*emitter, 3);
    particles.step(frame, {});
    EffectGeometry geometry;
    particles.collect(viewOf(frontCamera()), geometry);
    CHECK(geometry.batches.empty());
    loaded = true;
    particles.collect(viewOf(frontCamera()), geometry);
    CHECK_EQ(geometry.batches.size(), 1u);
    CHECK(geometry.batches[0].texture == image);
}

namespace {

std::unique_ptr<OffscreenRenderer> makeOffscreen(Context& context)
{
    auto offscreen = OffscreenRenderer::create(context, 64, 48);
    if (!offscreen)
        throw lt::Failure{offscreen.error()};
    return std::move(*offscreen);
}

Rgba centre(const PixelImage& image)
{
    return image.at(image.width / 2, image.height / 2);
}

} // namespace

GPU_TEST(particles_draw_over_the_scene)
{
    auto game = DataModel::create();
    auto part = addPart(game->workspace(), {});
    part->setTransparency(1.0f);
    auto emitter = addEmitter(part.get());
    emitter->setSpeed({0.0f, 0.0f});
    emitter->setShapeStyle(Enum::ParticleEmitterShapeStyle::Surface);
    emitter->setEmissionDirection(Enum::NormalId::Back); // the face towards the camera, at z = 1
    emitter->setSize(NumberSequence(2.0f));
    emitter->setColor(ColorSequence(Color3{1.0f, 0.0f, 0.0f}));
    ParticleSystem particles(*game->workspace(), {});
    auto offscreen = makeOffscreen(context);
    offscreen->renderer().addEffects(&particles);
    const Rgba sky = centre(offscreen->render(frontCamera(), {}));

    emit(*emitter, 1);
    particles.step(frame, {});
    const Rgba red = centre(offscreen->render(frontCamera(), {}));
    CHECK(red.r > 200 && red.g < 40 && red.b < 40);

    // Additive: the sky shows through, redder.
    emitter->setLightEmission(1.0f);
    emitter->setColor(ColorSequence(Color3{0.25f, 0.0f, 0.0f}));
    const Rgba added = centre(offscreen->render(frontCamera(), {}));
    CHECK(added.r > sky.r + 20 && std::abs(added.g - sky.g) <= 2 && std::abs(added.b - sky.b) <= 2);

    // A solid part in front hides it, unless ZOffset brings it nearer.
    emitter->setLightEmission(0.0f);
    emitter->setColor(ColorSequence(Color3{1.0f, 0.0f, 0.0f}));
    const DrawItem wall{partMatrix(CFrame(0.0f, 0.0f, 2.0f), {20.0f, 20.0f, 0.5f}), {0.0f, 0.0f, 1.0f}};
    const Rgba hidden = centre(offscreen->render(frontCamera(), {&wall, 1}));
    CHECK(hidden.b > hidden.r);
    emitter->setZOffset(3.0f);
    const Rgba shown = centre(offscreen->render(frontCamera(), {&wall, 1}));
    CHECK(shown.r > 200 && shown.b < 40);
    offscreen->renderer().removeEffects(&particles);
}
