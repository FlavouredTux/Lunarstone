// The 3D adornments as effect geometry: the faces they draw, where they sit
// on their Adornee, and which of the game's adornments show (Adornments).

#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/Adornments.hpp>
#include <lunar/render/Camera.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using lunar::BoxHandleAdornment;
using lunar::CFrame;
using lunar::Color3;
using lunar::CoreGui;
using lunar::CylinderHandleAdornment;
using lunar::DataModel;
using lunar::Folder;
using lunar::LineHandleAdornment;
using lunar::Part;
using lunar::ReplicatedStorage;
using lunar::SelectionBox;
using lunar::StarterGui;
using lunar::Vector3;
using lunar::render::Adornments;
using lunar::render::AdornView;
using lunar::render::EffectGeometry;
using lunar::render::EffectView;

namespace {

bool close(Vector3 a, Vector3 b, float tolerance = 1e-3f)
{
    return (a - b).magnitude() <= tolerance;
}

AdornView viewFrom(Vector3 eye, float pixelsPerStud = 0.0f)
{
    return {lunar::render::Camera::lookAt(eye, {0.0f, 0.0f, 0.0f}), pixelsPerStud};
}

EffectView effectViewFrom(Vector3 eye)
{
    lunar::render::Camera camera;
    camera.cframe = lunar::render::Camera::lookAt(eye, {0.0f, 0.0f, 0.0f});
    return {camera, camera.viewProjection(4.0f / 3.0f)};
}

// A part at the origin in a fresh game's Workspace.
struct Game {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Part> part = std::make_shared<Part>();

    Game()
    {
        part->setSize({2.0f, 2.0f, 2.0f});
        part->setParent(game->workspace());
    }
};

} // namespace

// Only the faces turned to the eye: one looking straight at a face sees
// that one, one at a corner three.
TEST(render_adornment_box_draws_the_faces_the_eye_sees)
{
    Game g;
    BoxHandleAdornment box;
    box.setAdornee(g.part.get());
    box.setSize({2.0f, 2.0f, 2.0f});
    box.setColor3({1.0f, 0.0f, 0.0f});
    box.setTransparency(0.25f);
    box.setAlwaysOnTop(true);
    EffectGeometry front;
    CHECK(appendAdornment(box, viewFrom({0.0f, 0.0f, 10.0f}), {}, nullptr, front));
    CHECK_EQ(front.batches.size(), std::size_t{1});
    CHECK_EQ(front.indices.size(), std::size_t{6});
    CHECK(front.batches[0].onTop);
    CHECK(!front.batches[0].toneMapped);
    for (const auto& v : front.vertices) {
        CHECK_EQ(v.position.z, 1.0f);
        CHECK_EQ(v.color[0], 1.0f);
        CHECK_EQ(v.color[3], 0.75f);
    }
    EffectGeometry corner;
    CHECK(appendAdornment(box, viewFrom({10.0f, 10.0f, 10.0f}), {}, nullptr, corner));
    CHECK_EQ(corner.indices.size(), std::size_t{18});
    box.setAlwaysOnTop(false);
    EffectGeometry behind;
    CHECK(appendAdornment(box, viewFrom({0.0f, 0.0f, 10.0f}), {}, nullptr, behind));
    CHECK(!behind.batches[0].onTop);
}

TEST(render_adornment_shows_only_with_an_adornee_while_visible)
{
    Game g;
    BoxHandleAdornment box;
    EffectGeometry out;
    CHECK(!appendAdornment(box, viewFrom({0.0f, 0.0f, 10.0f}), {}, nullptr, out)); // no Adornee
    box.setAdornee(g.part.get());
    box.setVisible(false);
    CHECK(!appendAdornment(box, viewFrom({0.0f, 0.0f, 10.0f}), {}, nullptr, out));
    box.setVisible(true);
    box.setTransparency(1.0f);
    CHECK(!appendAdornment(box, viewFrom({0.0f, 0.0f, 10.0f}), {}, nullptr, out));
    CHECK(out.batches.empty() && out.vertices.empty());
}

// CFrame is from the Adornee's pivot, after SizeRelativeOffset's halves of
// its size; and `offset` (a character drawn ahead of physics) moves it all.
TEST(render_adornment_handles_sit_on_their_adornee)
{
    Game g;
    g.part->setCFrame(CFrame(5.0f, 0.0f, 0.0f));
    g.part->setSize({4.0f, 2.0f, 2.0f});
    BoxHandleAdornment box;
    box.setAdornee(g.part.get());
    box.setCFrame(CFrame(0.0f, 2.0f, 0.0f));
    box.setSizeRelativeOffset({1.0f, 0.0f, 0.0f});
    EffectGeometry out;
    CHECK(appendAdornment(box, viewFrom({0.0f, 0.0f, 30.0f}), {}, nullptr, out));
    CHECK(close(out.batches[0].center, {7.0f, 2.0f, 0.0f}));
    EffectGeometry moved;
    CHECK(appendAdornment(box, viewFrom({0.0f, 0.0f, 30.0f}), {0.0f, 0.0f, -1.0f}, nullptr, moved));
    CHECK(close(moved.batches[0].center, {7.0f, 2.0f, -1.0f}));
}

// Twelve bars along the edges, three faces of each seen from a corner, and
// the surface only once SurfaceTransparency lets it show.
TEST(render_adornment_selection_box_outlines_the_part)
{
    Game g;
    SelectionBox box;
    box.setAdornee(g.part.get());
    EffectGeometry lines;
    CHECK(appendAdornment(box, viewFrom({10.0f, 12.0f, 14.0f}), {}, nullptr, lines));
    CHECK_EQ(lines.indices.size(), std::size_t{12 * 3 * 6});
    // Out to the box's corners and half a line past them.
    for (const auto& v : lines.vertices)
        for (const float c : {v.position.x, v.position.y, v.position.z})
            CHECK(std::abs(c) <= 1.0f + 0.075f + 1e-4f);
    box.setSurfaceTransparency(0.5f);
    box.setSurfaceColor3({0.0f, 1.0f, 0.0f});
    EffectGeometry both;
    CHECK(appendAdornment(box, viewFrom({10.0f, 12.0f, 14.0f}), {}, nullptr, both));
    CHECK_EQ(both.indices.size(), std::size_t{12 * 3 * 6 + 3 * 6});
}

TEST(render_adornment_cylinder_keeps_to_its_sizes)
{
    Game g;
    CylinderHandleAdornment cylinder;
    cylinder.setAdornee(g.part.get());
    cylinder.setRadius(1.5f);
    cylinder.setHeight(4.0f);
    cylinder.setInnerRadius(0.5f);
    cylinder.setAngle(180.0f);
    EffectGeometry out;
    CHECK(appendAdornment(cylinder, viewFrom({3.0f, 4.0f, 10.0f}), {}, nullptr, out));
    for (const auto& v : out.vertices) {
        const float r = std::hypot(v.position.x, v.position.y);
        CHECK(r <= 1.5f + 1e-4f && r >= 0.5f - 1e-4f);
        CHECK(std::abs(v.position.z) <= 2.0f + 1e-4f);
        CHECK(v.position.y >= -1e-4f); // half a turn, from +X through +Y
    }
}

// A line's Thickness is in pixels: as wide on screen however far it is.
TEST(render_adornment_line_is_thickness_pixels_wide)
{
    Game g;
    LineHandleAdornment line;
    line.setAdornee(g.part.get());
    line.setCFrame(CFrame::angles(0.0f, -std::numbers::pi_v<float> / 2.0f, 0.0f)); // LookVector +X
    line.setLength(4.0f);
    line.setThickness(10.0f);
    EffectGeometry out;
    // 100 pixels a stud at a stud's distance: 10 pixels is a stud at 10 studs.
    CHECK(appendAdornment(line, viewFrom({0.0f, 0.0f, 10.0f}, 100.0f), {}, nullptr, out));
    CHECK_EQ(out.vertices.size(), std::size_t{4});
    const float near = (out.vertices[0].position - out.vertices[1].position).magnitude();
    CHECK(std::abs(near - 1.0f) < 1e-3f);
    CHECK(close(out.vertices[2].position + out.vertices[3].position, {8.0f, 0.0f, 0.0f}, 1e-3f));
}

// Adornments shows the game's adornments that are in the Workspace, a
// PlayerGui or CoreGui and adorn something in the Workspace; not those
// kept in StarterGui or storage, nor ones adorning a part that isn't out.
TEST(render_adornments_shows_what_the_client_draws)
{
    Game g;
    Adornments adornments(*g.game, {});
    auto make = [&](lunar::Instance* parent, lunar::PVInstance* adornee) {
        auto box = std::make_shared<BoxHandleAdornment>();
        box->setAdornee(adornee);
        box->setParent(parent);
        return box;
    };
    auto stored = std::make_shared<Part>();
    stored->setParent(g.game->getService<ReplicatedStorage>());
    auto folder = std::make_shared<Folder>();
    folder->setParent(g.game->getService<CoreGui>());
    const auto inWorkspace = make(g.game->workspace(), g.part.get());
    const auto inCoreGui = make(folder.get(), g.part.get());
    const auto inStarterGui = make(g.game->getService<StarterGui>(), g.part.get());
    const auto inStorage = make(g.game->getService<ReplicatedStorage>(), g.part.get());
    const auto storedAdornee = make(g.game->workspace(), stored.get());
    CHECK_EQ(adornments.count(), std::size_t{5});
    EffectGeometry out;
    adornments.collect(effectViewFrom({0.0f, 0.0f, 10.0f}), out);
    CHECK_EQ(adornments.drawn(), std::size_t{2});
    CHECK_EQ(out.batches.size(), std::size_t{2});
    // Gone once taken out; and one behind the eye isn't drawn.
    inCoreGui->setParent(nullptr);
    CHECK_EQ(adornments.count(), std::size_t{4});
    EffectGeometry fewer;
    adornments.collect(effectViewFrom({0.0f, 0.0f, 10.0f}), fewer);
    CHECK_EQ(adornments.drawn(), std::size_t{1});
    g.part->setCFrame(CFrame(0.0f, 0.0f, 50.0f));
    EffectGeometry behind;
    adornments.collect(effectViewFrom({0.0f, 0.0f, 10.0f}), behind);
    CHECK_EQ(adornments.drawn(), std::size_t{0});
    CHECK(behind.vertices.empty());
}

TEST(render_adornments_follow_a_moving_adornee)
{
    Game g;
    Adornments adornments(*g.game, {});
    auto box = std::make_shared<BoxHandleAdornment>();
    box->setAdornee(g.part.get());
    box->setParent(g.game->workspace());
    adornments.follow(g.part.get(), {1.0f, 0.0f, 0.0f});
    EffectGeometry out;
    adornments.collect(effectViewFrom({0.0f, 0.0f, 10.0f}), out);
    CHECK(close(out.batches.at(0).center, {1.0f, 0.0f, 0.0f}));
}
