#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Ribbons.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

using lunar::Attachment;
using lunar::Beam;
using lunar::CFrame;
using lunar::Color3;
using lunar::ColorSequence;
using lunar::DataModel;
using lunar::NumberSequence;
using lunar::Part;
using lunar::Trail;
using lunar::Vector3;
using lunar::render::EffectGeometry;
using lunar::render::EffectVertex;
using lunar::render::RibbonView;
using lunar::render::TrailHistory;

namespace {

bool close(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

bool close(Vector3 a, Vector3 b, float tolerance = 1e-3f)
{
    return (a - b).magnitude() <= tolerance;
}

// Looking at the origin from far up +Z: nothing thins the segments.
RibbonView frontView()
{
    return {{0.0f, 0.0f, 50.0f}, {0.0f, 0.0f, -1.0f}};
}

float u(const EffectVertex& v)
{
    return v.uvq[0] / v.uvq[2];
}

float v(const EffectVertex& vertex)
{
    return vertex.uvq[1] / vertex.uvq[2];
}

} // namespace

TEST(ribbon_beam_straight_spans_attachments_with_its_widths)
{
    Beam beam;
    beam.setWidth0(2.0f);
    beam.setWidth1(4.0f);
    beam.setSegments(4);
    EffectGeometry out;
    // Along X, the attachments unturned: the width goes along their Y axes.
    CHECK(lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.0f,
                                    nullptr, out));
    CHECK_EQ(out.batches.size(), 1u);
    CHECK_EQ(out.vertices.size(), 10u); // five sections, two sides each
    CHECK_EQ(out.batches[0].indexCount, 24u);
    // First section: Attachment0's end, 2 wide; last: 4 wide.
    CHECK(close(out.vertices[0].position, {0.0f, 1.0f, 0.0f}));
    CHECK(close(out.vertices[1].position, {0.0f, -1.0f, 0.0f}));
    CHECK(close(out.vertices[8].position, {10.0f, 2.0f, 0.0f}));
    CHECK(close(out.vertices[9].position, {10.0f, -2.0f, 0.0f}));
    // Halfway, halfway between the widths.
    CHECK(close(out.vertices[4].position, {5.0f, 1.5f, 0.0f}));
    // Stretch at TextureLength 1: the texture once along it; v across.
    CHECK(close(u(out.vertices[0]), 0.0f));
    CHECK(close(u(out.vertices[9]), 1.0f));
    CHECK(close(v(out.vertices[0]), 0.0f));
    CHECK(close(v(out.vertices[1]), 1.0f));
    // q is each section's half width, for perspective-correct trapezoids.
    CHECK(close(out.vertices[0].uvq[2], 1.0f));
    CHECK(close(out.vertices[9].uvq[2], 2.0f));
    // The default Transparency is 0.5.
    CHECK(close(out.vertices[0].color[3], 0.5f));
    CHECK(close(out.batches[0].center, {5.0f, 0.0f, 0.0f}));
}

TEST(ribbon_beam_curves_along_the_attachments_x_axes)
{
    // From the origin up +Z; CurveSize0 pulls the start towards Attachment0's
    // +X, CurveSize1 the end towards Attachment1's -X: an S.
    Beam beam;
    beam.setSegments(10);
    beam.setCurveSize0(6.0f);
    EffectGeometry out;
    CHECK(lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(0.0f, 0.0f, 10.0f), frontView(), 0.0f,
                                    nullptr, out));
    const auto middle = [&](std::size_t section) {
        return (out.vertices[2 * section].position + out.vertices[2 * section + 1].position) * 0.5f;
    };
    CHECK(middle(3).x > 1.0f);
    CHECK(close(middle(0), {0.0f, 0.0f, 0.0f}));
    CHECK(close(middle(10), {0.0f, 0.0f, 10.0f}));

    beam.setCurveSize0(0.0f);
    beam.setCurveSize1(6.0f);
    out.clear();
    CHECK(lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(0.0f, 0.0f, 10.0f), frontView(), 0.0f,
                                    nullptr, out));
    CHECK(middle(7).x < -1.0f);
}

TEST(ribbon_beam_turns_its_width_from_one_attachment_to_the_other)
{
    Beam beam;
    beam.setSegments(2);
    beam.setWidth0(2.0f);
    beam.setWidth1(2.0f);
    // Attachment1 turned a quarter about X: its Y axis is +Z.
    const CFrame a1 = CFrame(10.0f, 0.0f, 0.0f) * CFrame::angles(std::numbers::pi_v<float> / 2.0f, 0.0f, 0.0f);
    EffectGeometry out;
    CHECK(lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), a1, frontView(), 0.0f, nullptr, out));
    CHECK(close(out.vertices[0].position, {0.0f, 1.0f, 0.0f}));
    CHECK(close(out.vertices[4].position, {10.0f, 0.0f, 1.0f}));
    // Halfway, half way round.
    const float h = std::sqrt(0.5f);
    CHECK(close(out.vertices[2].position, {5.0f, h, h}));
}

TEST(ribbon_beam_face_camera_turns_it_square_to_the_view)
{
    Beam beam;
    beam.setSegments(1);
    beam.setFaceCamera(true);
    // Attachments turned so their Y axes point at the camera: FaceCamera
    // ignores them.
    const CFrame turned = CFrame::angles(std::numbers::pi_v<float> / 2.0f, 0.0f, 0.0f);
    EffectGeometry out;
    CHECK(lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f) * turned, CFrame(10.0f, 0.0f, 0.0f) * turned,
                                    {{5.0f, 0.0f, 50.0f}, {0.0f, 0.0f, -1.0f}}, 0.0f, nullptr, out));
    const Vector3 across = out.vertices[0].position - out.vertices[1].position;
    CHECK(close(std::abs(across.y), 1.0f));
    CHECK(close(across.z, 0.0f));
}

TEST(ribbon_beam_texture_modes_and_scrolling)
{
    Beam beam;
    beam.setSegments(1);
    beam.setTextureLength(2.0f);
    beam.setTextureMode(lunar::Enum::TextureMode::Wrap);
    EffectGeometry out;
    // Wrap: a repeat every TextureLength studs.
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.0f, nullptr,
                              out);
    CHECK(close(u(out.vertices[3]), 5.0f));
    // Stretch: TextureLength repeats over the whole beam.
    beam.setTextureMode(lunar::Enum::TextureMode::Stretch);
    out.clear();
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.0f, nullptr,
                              out);
    CHECK(close(u(out.vertices[3]), 2.0f));
    // Scrolling moves the texture towards Attachment1: lower u at a point.
    out.clear();
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.25f, nullptr,
                              out);
    CHECK(close(u(out.vertices[0]), -0.25f));
}

TEST(ribbon_beam_colour_and_transparency_along_it)
{
    Beam beam;
    beam.setSegments(2);
    beam.setColor(ColorSequence({{0.0f, Color3(1.0f, 0.0f, 0.0f)}, {1.0f, Color3(0.0f, 0.0f, 1.0f)}}));
    beam.setTransparency(NumberSequence({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}));
    beam.setLocalTransparencyModifier(0.5f);
    beam.setLightEmission(1.0f);
    beam.setLightInfluence(0.25f);
    beam.setBrightness(3.0f);
    EffectGeometry out;
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.0f, nullptr,
                              out);
    CHECK(close(out.vertices[0].color[0], 1.0f));
    CHECK(close(out.vertices[2].color[0], 0.5f));
    CHECK(close(out.vertices[2].color[2], 0.5f));
    CHECK(close(out.vertices[4].color[2], 1.0f));
    // Opacity: one less the sequence, times one less LocalTransparencyModifier.
    CHECK(close(out.vertices[0].color[3], 0.5f));
    CHECK(close(out.vertices[2].color[3], 0.25f));
    CHECK(close(out.vertices[4].color[3], 0.0f));
    CHECK(close(out.batches[0].lightEmission, 1.0f));
    CHECK(close(out.batches[0].lightInfluence, 0.25f));
    CHECK(close(out.batches[0].brightness, 3.0f));

    // Fully transparent everywhere: nothing to draw.
    beam.setTransparency(NumberSequence(1.0f));
    out.clear();
    CHECK(!lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.0f,
                                     nullptr, out));
    CHECK(out.vertices.empty());
}

TEST(ribbon_beam_thins_its_segments_with_distance)
{
    Beam beam;
    beam.setSegments(20);
    EffectGeometry out;
    // The client's BeamNode: all of them within 200 studs, a tenth past 920.
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f),
                              {{0.0f, 0.0f, 100.0f}, {0.0f, 0.0f, -1.0f}}, 0.0f, nullptr, out);
    CHECK_EQ(out.vertices.size(), 42u);
    out.clear();
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f),
                              {{0.0f, 0.0f, 2000.0f}, {0.0f, 0.0f, -1.0f}}, 0.0f, nullptr, out);
    CHECK_EQ(out.vertices.size(), 6u);
}

TEST(ribbon_beam_zoffset_moves_it_towards_the_camera)
{
    Beam beam;
    beam.setSegments(1);
    beam.setZOffset(2.0f);
    EffectGeometry out;
    lunar::render::appendBeam(beam, CFrame(0.0f, 0.0f, 0.0f), CFrame(10.0f, 0.0f, 0.0f), frontView(), 0.0f, nullptr,
                              out);
    CHECK(close(out.vertices[0].position.z, 2.0f));
}

TEST(trail_history_lays_segments_every_min_length)
{
    TrailHistory history;
    const TrailHistory::Settings settings{.lifetime = 2.0f, .minLength = 1.0f};
    history.update({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.0, settings);
    CHECK_EQ(history.points().size(), 1u);
    // Under MinLength: the newest point just follows the attachments.
    history.update({0.5f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, 0.1, settings);
    CHECK_EQ(history.points().size(), 2u);
    CHECK(history.following());
    history.update({0.8f, 0.0f, 0.0f}, {0.8f, 1.0f, 0.0f}, 0.2, settings);
    CHECK_EQ(history.points().size(), 2u);
    CHECK(close(history.points().back().p0, {0.8f, 0.0f, 0.0f}));
    // Far enough: laid.
    history.update({1.5f, 0.0f, 0.0f}, {1.5f, 1.0f, 0.0f}, 0.3, settings);
    CHECK_EQ(history.points().size(), 2u);
    CHECK(!history.following());
    history.update({1.6f, 0.0f, 0.0f}, {1.6f, 1.0f, 0.0f}, 0.4, settings);
    CHECK_EQ(history.points().size(), 3u);
    CHECK(close(history.points()[2].traveled, 1.6f));
}

TEST(trail_history_expires_by_lifetime_shrinking_the_tail)
{
    TrailHistory history;
    const TrailHistory::Settings settings{.lifetime = 1.0f, .minLength = 0.0f};
    for (int i = 0; i <= 10; ++i)
        history.update({static_cast<float>(i), 0.0f, 0.0f}, {static_cast<float>(i), 1.0f, 0.0f}, i * 0.1, settings);
    CHECK_EQ(history.points().size(), 11u);
    // Half a step past the oldest's lifetime: it's pulled halfway to the next.
    history.update({11.0f, 0.0f, 0.0f}, {11.0f, 1.0f, 0.0f}, 1.05, settings);
    CHECK_EQ(history.points().size(), 12u);
    CHECK(close(history.points().front().p0, {0.5f, 0.0f, 0.0f}));
    CHECK(close(static_cast<float>(history.points().front().born), 0.05f));
    // Disabled: nothing new, and the rest runs out.
    TrailHistory::Settings off = settings;
    off.enabled = false;
    history.update({12.0f, 0.0f, 0.0f}, {12.0f, 1.0f, 0.0f}, 1.5, off);
    CHECK(close(history.points().back().p0, {11.0f, 0.0f, 0.0f}));
    history.update({12.0f, 0.0f, 0.0f}, {12.0f, 1.0f, 0.0f}, 3.0, off);
    CHECK(history.points().empty());
}

TEST(trail_history_max_length_cuts_the_oldest)
{
    TrailHistory history;
    const TrailHistory::Settings settings{.lifetime = 10.0f, .minLength = 0.0f, .maxLength = 3.5f};
    for (int i = 0; i <= 10; ++i)
        history.update({static_cast<float>(i), 0.0f, 0.0f}, {static_cast<float>(i), 1.0f, 0.0f}, i * 0.1, settings);
    const auto points = history.points();
    CHECK_EQ(points.size(), 5u);
    CHECK(close(points.front().p0, {6.5f, 0.0f, 0.0f}));
    CHECK(close(points.back().p0, {10.0f, 0.0f, 0.0f}));
}

TEST(trail_history_reenabled_starts_a_new_run)
{
    TrailHistory history;
    TrailHistory::Settings settings{.lifetime = 10.0f, .minLength = 0.0f};
    history.update({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.0, settings);
    history.update({1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, 0.1, settings);
    settings.enabled = false;
    history.update({2.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, 0.2, settings);
    settings.enabled = true;
    history.update({5.0f, 0.0f, 0.0f}, {5.0f, 1.0f, 0.0f}, 0.3, settings);
    const auto points = history.points();
    CHECK_EQ(points.size(), 3u);
    CHECK(points[2].start); // no segment across the gap
    CHECK(!points[1].start);
    history.clear();
    CHECK(history.points().empty());
}

TEST(trail_geometry_follows_age)
{
    Trail trail;
    trail.setLifetime(1.0f);
    trail.setMinLength(0.0f);
    trail.setTransparency(NumberSequence({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}));
    trail.setWidthScale(NumberSequence({{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}));
    TrailHistory history;
    const TrailHistory::Settings settings{.lifetime = 1.0f, .minLength = 0.0f};
    // The attachments 2 studs apart in Y, moving along X.
    for (int i = 0; i <= 4; ++i)
        history.update({static_cast<float>(i), -1.0f, 0.0f}, {static_cast<float>(i), 1.0f, 0.0f}, i * 0.25, settings);
    EffectGeometry out;
    CHECK(lunar::render::appendTrail(trail, history, 1.0, frontView(), nullptr, out));
    // Newest last: full width (Attachment1's side first) and opaque; the
    // oldest (age 1) a point, clear.
    const EffectVertex& newest = out.vertices[out.vertices.size() - 2];
    CHECK(close(newest.position, {4.0f, 1.0f, 0.0f}));
    CHECK(close(newest.color[3], 1.0f));
    CHECK(close(out.vertices[0].position, {0.0f, 0.0f, 0.0f}));
    CHECK(close(out.vertices[0].color[3], 0.0f));
    // Halfway through its life: half as wide, half transparent.
    CHECK(close(out.vertices[4].position, {2.0f, 0.5f, 0.0f}));
    CHECK(close(out.vertices[4].color[3], 0.5f));
    // Stretch at TextureLength 1: u runs from the head (0) to the tail (1).
    CHECK(close(u(out.vertices[0]), 1.0f));
    CHECK(close(u(newest), 0.0f));
}

TEST(ribbons_collects_the_workspaces_beams_and_trails_over_frames)
{
    auto game = DataModel::create();
    auto part = std::make_shared<Part>();
    part->setCFrame(CFrame(0.0f, 0.0f, 0.0f));
    part->setParent(game->workspace());
    auto a0 = std::make_shared<Attachment>();
    a0->setCFrame(CFrame(-5.0f, 0.0f, 0.0f));
    a0->setParent(part.get());
    auto a1 = std::make_shared<Attachment>();
    a1->setCFrame(CFrame(5.0f, 0.0f, 0.0f));
    a1->setParent(part.get());
    auto beam = std::make_shared<Beam>();
    beam->setAttachment0(a0.get());
    beam->setAttachment1(a1.get());
    beam->setParent(part.get());

    lunar::render::Ribbons ribbons(*game->workspace(), {});
    CHECK_EQ(ribbons.beamCount(), 1u);
    lunar::render::EffectView view;
    view.camera.cframe = CFrame(0.0f, 0.0f, 30.0f);
    view.viewProjection = view.camera.viewProjection(16.0f / 9.0f);
    EffectGeometry out;
    ribbons.collect(view, out);
    CHECK_EQ(out.batches.size(), 1u);

    // Disabled, behind the camera or an attachment out of the Workspace: none.
    beam->setEnabled(false);
    out.clear();
    ribbons.collect(view, out);
    CHECK(out.batches.empty());
    beam->setEnabled(true);
    lunar::render::EffectView behind = view;
    behind.camera.cframe = CFrame(0.0f, 0.0f, 30.0f) * CFrame::angles(0.0f, std::numbers::pi_v<float>, 0.0f);
    behind.viewProjection = behind.camera.viewProjection(16.0f / 9.0f);
    out.clear();
    ribbons.collect(behind, out);
    CHECK(out.batches.empty());
    auto elsewhere = std::make_shared<Part>();
    a1->setParent(elsewhere.get());
    out.clear();
    ribbons.collect(view, out);
    CHECK(out.batches.empty());
    a1->setParent(part.get());

    // A trail on the part, moving: it grows from frame to frame, and
    // Trail:Clear (or new attachments) wipes it.
    auto trail = std::make_shared<Trail>();
    trail->setAttachment0(a0.get());
    trail->setAttachment1(a1.get());
    trail->setParent(part.get());
    CHECK_EQ(ribbons.trailCount(), 1u);
    beam->setParent(nullptr);
    CHECK_EQ(ribbons.beamCount(), 0u);
    for (int frame = 0; frame < 10; ++frame) {
        part->setCFrame(CFrame(0.0f, static_cast<float>(frame), 0.0f));
        view.time = frame / 60.0;
        out.clear();
        ribbons.collect(view, out);
    }
    CHECK_EQ(out.batches.size(), 1u);
    CHECK_EQ(ribbons.trailsDrawn(), 1u);
    CHECK_EQ(out.vertices.size(), 20u);
    trail->clear();
    view.time = 11 / 60.0;
    out.clear();
    ribbons.collect(view, out);
    CHECK(out.batches.empty());
}

TEST(ribbon_script_methods_reach_the_renderer)
{
    Harness h;
    auto trail = std::make_shared<Trail>();
    trail->setName("T");
    trail->setParent(h.game->workspace());
    auto beam = std::make_shared<Beam>();
    beam->setName("B");
    beam->setParent(h.game->workspace());
    h.run("workspace.T:Clear() workspace.T:Clear() workspace.B:SetTextureOffset(0.25)");
    CHECK_EQ(trail->clears(), 2u);
    CHECK_EQ(beam->textureOffsetSets(), 1u);
    CHECK(close(beam->textureOffset(), 0.25f));
    h.run("workspace.B:SetTextureOffset()");
    CHECK(close(beam->textureOffset(), 0.0f));
}
