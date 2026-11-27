#include "framework.hpp"

#include "PlaceScene.hpp"

#include <lunar/Classes.hpp>

#include <cmath>
#include <memory>

using namespace lunar;

namespace {

// Each part item where its part is (plus `offset` for those under
// `moving`), faded by its part's LocalTransparencyModifier.
bool itemsFollowParts(const PlaceScene& scene, const Instance* moving, Vector3 offset)
{
    bool ok = true;
    for (std::size_t i = 0; i < scene.partItemCount(); ++i) {
        const auto* part = dynamic_cast<const BasePart*>(scene.owners()[i]);
        if (!part)
            continue;
        Vector3 want = part->cframe().position;
        if (moving && part->isDescendantOf(moving))
            want = want + offset;
        const render::Mat4& model = scene.items()[i].model;
        const Vector3 at{model.m[3][0], model.m[3][1], model.m[3][2]};
        const float fade = 1.0f - (1.0f - part->transparency()) * (1.0f - part->localTransparencyModifier());
        if ((at - want).magnitude() > 1e-4f || std::abs(scene.items()[i].transparency - fade) > 1e-5f)
            ok = false;
    }
    return ok;
}

} // namespace

// Parts extracted again (a look changed) or new go to the end of the items
// while the rest keep theirs; followParts places only what moved or is new,
// and keeps following the moving model's parts, old and new.
TEST(app_place_scene_follows_moves_and_reextracted_parts)
{
    auto game = DataModel::create();
    Workspace* workspace = game->workspace();
    auto add = [&](Instance* parent, Vector3 at) {
        auto part = std::make_shared<Part>();
        part->setCFrame(CFrame(at));
        part->setParent(parent);
        return part;
    };
    auto a = add(workspace, {0, 0, 0});
    auto b = add(workspace, {10, 0, 0});
    auto character = std::make_shared<Model>();
    character->setParent(workspace);
    auto limb = add(character.get(), {20, 0, 0});
    PlaceScene scene(*game);
    const Connection added = workspace->descendantAdded.connect([&](Instance* instance) { scene.touch(*instance); });
    scene.refresh();
    const Vector3 offset{0, 1, 0};
    scene.followParts(character.get(), offset);
    CHECK_EQ(scene.partItemCount(), std::size_t{3});
    CHECK(itemsFollowParts(scene, character.get(), offset));

    // a moves; b changes colour and is extracted again, after the others;
    // new parts come, one faded and one in the character.
    a->setCFrame(CFrame(Vector3{0, 5, 0}));
    b->setColor({1, 0, 0});
    auto faded = add(workspace, {30, 0, 0});
    faded->setLocalTransparencyModifier(0.5f);
    auto hand = add(character.get(), {40, 0, 0});
    CHECK(scene.refresh());
    // Moves between the extraction and the frame's followParts count too.
    limb->setCFrame(CFrame(Vector3{20, 2, 0}));
    b->setCFrame(CFrame(Vector3{10, 3, 0}));
    scene.followParts(character.get(), offset);
    CHECK_EQ(scene.partItemCount(), std::size_t{5});
    CHECK_EQ(scene.owners()[0], static_cast<const Instance*>(a.get())); // kept its place
    CHECK(itemsFollowParts(scene, character.get(), offset));

    // Following stops: the character's parts go back where they are.
    scene.followParts(nullptr, {});
    CHECK(itemsFollowParts(scene, nullptr, {}));
    // And another extraction while nothing is followed.
    a->setColor({0, 1, 0});
    hand->setCFrame(CFrame(Vector3{40, 4, 0}));
    CHECK(scene.refresh());
    scene.followParts(nullptr, {});
    CHECK(itemsFollowParts(scene, nullptr, {}));
}

// Highlights cover their Adornee's (else their parent's) parts, a deeper
// one over a shallower; the renderer tells them apart by their look's
// address. Frame after frame, and as they change, each item carries the
// look of the deepest enabled Highlight over it, one object per Highlight.
TEST(app_place_scene_highlights_follow_their_parts)
{
    auto game = DataModel::create();
    Workspace* workspace = game->workspace();
    auto add = [&](Instance* parent, Vector3 at) {
        auto part = std::make_shared<Part>();
        part->setCFrame(CFrame(at));
        part->setParent(parent);
        return part;
    };
    auto model = std::make_shared<Model>();
    model->setParent(workspace);
    auto inner = std::make_shared<Model>();
    inner->setParent(model.get());
    auto body = add(model.get(), {0, 0, 0});
    auto arm = add(inner.get(), {2, 0, 0});
    auto loose = add(workspace, {10, 0, 0});
    auto outer = std::make_shared<Highlight>();
    outer->setFillColor({0, 1, 0});
    outer->setParent(model.get());
    auto deep = std::make_shared<Highlight>();
    deep->setFillColor({0, 0, 1});
    deep->setParent(inner.get());
    PlaceScene scene(*game);
    const Connection added = workspace->descendantAdded.connect([&](Instance* instance) { scene.touch(*instance); });
    scene.refresh();
    auto lookOf = [&](const std::shared_ptr<Part>& part) -> const render::HighlightStyle* {
        for (std::size_t i = 0; i < scene.partItemCount(); ++i)
            if (scene.owners()[i] == part.get())
                return scene.items()[i].highlight.get();
        return nullptr;
    };
    for (int frame = 0; frame < 3; ++frame) {
        scene.followParts();
        CHECK(lookOf(body) && lookOf(body)->fillColor.g == 1.0f);
        CHECK(lookOf(arm) && lookOf(arm)->fillColor.b == 1.0f); // the deeper one
        CHECK(lookOf(loose) == nullptr);
        CHECK(lookOf(body) != lookOf(arm));
    }
    const render::HighlightStyle* kept = lookOf(body);
    // A look that changes is a new one; the other stays as it was.
    deep->setFillColor({1, 1, 0});
    scene.followParts();
    CHECK(lookOf(arm) && lookOf(arm)->fillColor.r == 1.0f && lookOf(arm)->fillColor.g == 1.0f);
    CHECK(lookOf(body) == kept);
    // Disabled: the arm falls back to the outer one.
    deep->setEnabled(false);
    scene.followParts();
    CHECK(lookOf(arm) == lookOf(body));
    // The arm leaves the model (extracted again), a new part joins it.
    arm->setParent(workspace);
    auto leg = add(model.get(), {-2, 0, 0});
    scene.refresh();
    scene.followParts();
    CHECK(lookOf(arm) == nullptr);
    CHECK(lookOf(leg) == kept && lookOf(body) == kept);
    // The outer one goes: nothing is covered.
    outer->setParent(nullptr);
    scene.followParts();
    CHECK(lookOf(body) == nullptr && lookOf(leg) == nullptr && lookOf(arm) == nullptr);
    // Adornee: a loose part covered from elsewhere.
    auto aimed = std::make_shared<Highlight>();
    aimed->setAdornee(loose.get());
    aimed->setParent(workspace);
    scene.followParts();
    CHECK(lookOf(loose) != nullptr && lookOf(body) == nullptr);
}
