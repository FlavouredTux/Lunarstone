#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/CoreGui.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/HealthBars.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::ui;

namespace {

bool near(float a, float b, float within = 0.01f)
{
    return std::abs(a - b) <= within;
}

bool sameColor(Color3 a, Color3 b)
{
    return near(a.r, b.r, 1e-4f) && near(a.g, b.g, 1e-4f) && near(a.b, b.b, 1e-4f);
}

// A game whose camera sits at the origin looking down -Z (70 degrees, a
// 1920x1080 screen), with a local player and the players around them, each
// with an R6 character: a Humanoid and a 2x2x1 HumanoidRootPart.
struct Arena {
    static constexpr Vector2 screen{1920, 1080};
    std::shared_ptr<DataModel> game = DataModel::create();
    Players* players = game->getService<Players>();
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    HealthBars bars{fonts, atlas};
    DrawList list;

    Arena()
    {
        Camera* camera = game->workspace()->ensureCurrentCamera();
        camera->setCFrame(CFrame());
        camera->setFieldOfView(70);
    }
    std::shared_ptr<Player> join(std::string_view name)
    {
        auto p = std::make_shared<Player>();
        p->setName(std::string(name));
        p->setParent(players);
        return p;
    }
    std::shared_ptr<Humanoid> spawn(Player& player, Vector3 at, float health = 100.0f)
    {
        auto character = std::make_shared<Model>();
        character->setName(player.name());
        auto humanoid = std::make_shared<Humanoid>();
        humanoid->setHealth(health);
        humanoid->setParent(character.get());
        auto root = std::make_shared<Part>();
        root->setName("HumanoidRootPart");
        root->setSize({2, 2, 1});
        root->setCFrame(CFrame(at));
        root->setParent(character.get());
        character->setParent(game->workspace());
        player.setCharacter(character.get());
        return humanoid;
    }
    // Who gets a bar this frame, by name.
    std::vector<std::string> shown(float scale = 1.0f, Vector2 size = screen)
    {
        bars.update(game.get(), size, scale);
        list.clear();
        bars.draw(list);
        std::vector<std::string> out;
        for (const HealthBars::Bar& bar : bars.bars())
            out.push_back(bar.player->name());
        return out;
    }
    const HealthBars::Bar* bar(std::string_view name) const
    {
        for (const HealthBars::Bar& b : bars.bars())
            if (b.player->name() == name)
                return &b;
        return nullptr;
    }
};

} // namespace

TEST(health_bars_project_a_box_to_its_screen_rectangle)
{
    // 90 degrees on a square screen: one stud at depth d is 500/d pixels.
    // The box's near face (depth 9) is what reaches farthest out.
    const auto r = projectBox(CFrame(), 90, {1000, 1000}, CFrame(Vector3{0, 0, -10}), {-1, -1, -1}, {1, 1, 1});
    CHECK(r.has_value());
    CHECK(near(r->x, 500 - 500.0f / 9));
    CHECK(near(r->y, 500 - 500.0f / 9));
    CHECK(near(r->w, 1000.0f / 9));
    CHECK(near(r->h, 1000.0f / 9));
    // Up is up on the screen: a box above the view's centre is in its top half.
    const auto up = projectBox(CFrame(), 90, {1000, 1000}, CFrame(Vector3{0, 5, -10}), {-1, -1, -1}, {1, 1, 1});
    CHECK(up && up->bottom() < 500);
    // A wider screen keeps the vertical field of view: the same box, as tall.
    const auto wide = projectBox(CFrame(), 90, {2000, 1000}, CFrame(Vector3{0, 0, -10}), {-1, -1, -1}, {1, 1, 1});
    CHECK(wide && near(wide->h, r->h) && near(wide->x + wide->w / 2, 1000));
    // The box's own frame turns it: a box 4 wide turned a quarter turn is 4 deep.
    const auto turned = projectBox(CFrame(), 90, {1000, 1000},
                                   CFrame(Vector3{0, 0, -10}) * CFrame::angles(0, std::numbers::pi_v<float> / 2, 0),
                                   {-2, -1, -0.5f}, {2, 1, 0.5f});
    CHECK(turned && turned->w < 4 * 500.0f / 8);
    // Behind the camera, or through its near plane: nothing.
    CHECK(!projectBox(CFrame(), 90, {1000, 1000}, CFrame(Vector3{0, 0, 10}), {-1, -1, -1}, {1, 1, 1}));
    CHECK(!projectBox(CFrame(), 90, {1000, 1000}, CFrame(Vector3{0, 0, -0.5f}), {-1, -1, -1}, {1, 1, 1}));
    CHECK(!projectBox(CFrame(), 90, {0, 0}, CFrame(Vector3{0, 0, -10}), {-1, -1, -1}, {1, 1, 1}));
}

TEST(health_bars_fill_from_the_bottom_by_fraction_and_colour)
{
    const RectF character{100, 50, 40, 200};
    const HealthBarRects half = healthBarRects(character, 0.5f, 1.0f);
    // Just left of the character, as tall as it, 3 pixels inside a 1 pixel outline.
    CHECK(half.track.right() < character.x);
    CHECK(half.track.right() >= character.x - 4);
    CHECK_EQ(half.track.y, character.y);
    CHECK_EQ(half.track.h, character.h);
    CHECK_EQ(half.track.w, 5.0f);
    CHECK_EQ(half.fill.w, 3.0f);
    // The fill sits on the bottom of the track's inside and is half of it.
    CHECK_EQ(half.fill.bottom(), half.track.bottom() - 1);
    CHECK_EQ(half.fill.h, 99.0f);
    CHECK_EQ(healthBarRects(character, 1.0f, 1.0f).fill.h, 198.0f);
    CHECK_EQ(healthBarRects(character, 0.0f, 1.0f).fill.h, 0.0f);
    CHECK_EQ(healthBarRects(character, 3.0f, 1.0f).fill.h, 198.0f);
    // Twice the pixels at twice the scale.
    CHECK_EQ(healthBarRects(character, 1.0f, 2.0f).track.w, 10.0f);

    CHECK_EQ(healthFraction(100, 100), 1.0f);
    CHECK_EQ(healthFraction(25, 100), 0.25f);
    CHECK_EQ(healthFraction(250, 100), 1.0f);
    CHECK_EQ(healthFraction(-5, 100), 0.0f);
    CHECK_EQ(healthFraction(10, 0), 0.0f);
    CHECK_EQ(healthFraction(10, std::numeric_limits<float>::infinity()), 0.0f);
    CHECK_EQ(healthFraction(std::numeric_limits<float>::quiet_NaN(), 100), 0.0f);
    // Green when healthy, yellow at half, red when nearly gone (the top
    // bar's healthColor); redder from full to half, then less green.
    CHECK(sameColor(healthColor(1.0f), ui::CoreGui::healthGreen));
    CHECK(sameColor(healthColor(0.5f), ui::CoreGui::healthYellow));
    CHECK(sameColor(healthColor(0.05f), ui::CoreGui::healthRed));
    float red = -1;
    for (float f = 1.0f; f >= 0.5f; f -= 0.05f) {
        CHECK(healthColor(f).r >= red - 1e-4f);
        red = healthColor(f).r;
    }
    float green = 2;
    for (float f = 0.5f; f >= 0.1f; f -= 0.05f) {
        CHECK(healthColor(f).g <= green + 1e-4f);
        green = healthColor(f).g;
    }
}

TEST(health_bars_only_for_living_players_on_the_screen)
{
    Arena a;
    auto me = a.join("me");
    a.players->assignLocalPlayer(me.get());
    a.spawn(*me, {-12, 0, -30}); // our own: never a bar, even in plain view
    auto front = a.join("front");
    auto frontHumanoid = a.spawn(*front, {0, 0, -20}, 60);
    auto dead = a.join("dead");
    a.spawn(*dead, {4, 0, -20}, 0);
    auto behind = a.join("behind");
    a.spawn(*behind, {0, 0, 20});
    auto aside = a.join("aside");
    a.spawn(*aside, {300, 0, -20});
    auto speck = a.join("speck");
    a.spawn(*speck, {0, 0, -5000});
    auto lobby = a.join("lobby"); // no character
    auto mid = a.join("mid");
    a.spawn(*mid, {-10, 0, -60});
    CHECK(a.shown() == std::vector<std::string>{"front", "mid"});

    const HealthBars::Bar* bar = a.bar("front");
    CHECK(bar != nullptr);
    // Its character's rectangle: centred, about 5 studs tall at 20 away.
    const float perStud = 1080 / (2 * std::tan(35 * std::numbers::pi_v<float> / 180));
    CHECK(near(bar->character.x + bar->character.w / 2, 960, 1));
    CHECK(bar->character.h > 5 * perStud / 20.5f && bar->character.h < 5 * perStud / 19.4f);
    CHECK(bar->rects.track.right() < bar->character.x);
    CHECK_EQ(bar->rects.track.h, std::round(bar->character.h));
    CHECK(near(bar->fraction, 0.6f, 1e-5f));
    CHECK(sameColor(bar->color, healthColor(0.6f)));
    // Drawn at the depth of the character's front, nearer than its middle.
    CHECK(bar->origin.z > -20 && bar->origin.z < -17);
    // Up close the HP shows; farther off, the bar alone.
    CHECK(bar->number);
    CHECK(!a.bar("mid")->number);
    // Each bar is a world layer of its own, depth tested.
    CHECK_EQ(a.list.layers.size(), std::size_t{2});
    CHECK(a.list.layers[0].depthTested);
    CHECK(a.list.layers[0].kind == WorldLayer::Kind::Billboard);
    for (const Quad& q : a.list.quads)
        CHECK(q.layer >= 1 && q.layer <= 2);

    // It follows the character.
    instanceCast<Part>(front->character()->findFirstChild("HumanoidRootPart"))->setCFrame(CFrame(Vector3{5, 0, -20}));
    a.shown();
    CHECK(a.bar("front")->character.x + a.bar("front")->character.w / 2 > 1100);

    // Dying takes the bar away; the respawn brings it back.
    frontHumanoid->setHealth(0);
    CHECK(a.shown() == std::vector<std::string>{"mid"});
    a.spawn(*front, {0, 0, -30});
    CHECK(a.shown() == std::vector<std::string>{"front", "mid"});
    CHECK_EQ(a.bar("front")->fraction, 1.0f);
    // The lobby player spawns into view; then leaves.
    a.spawn(*lobby, {-3, 0, -25});
    CHECK(a.shown() == std::vector<std::string>{"front", "lobby", "mid"});
    lobby->setParent(nullptr);
    CHECK(a.shown() == std::vector<std::string>{"front", "mid"});
    // A character that loses its Humanoid has no bar.
    front->character()->findFirstChildOfClass("Humanoid")->setParent(nullptr);
    CHECK(a.shown() == std::vector<std::string>{"mid"});

    // No camera or no game: nothing.
    a.bars.update(nullptr, Arena::screen, 1.0f);
    CHECK(a.bars.bars().empty());
}

TEST(health_bars_lay_the_number_out_only_when_it_changes)
{
    Arena a;
    auto other = a.join("other");
    auto humanoid = a.spawn(*other, {0, 0, -15});
    a.shown();
    const std::uint64_t first = a.bars.layouts();
    CHECK_EQ(first, std::uint64_t{1});
    for (int i = 0; i < 10; ++i)
        a.shown();
    CHECK_EQ(a.bars.layouts(), first);
    humanoid->setHealth(73.4f);
    a.shown();
    CHECK_EQ(a.bars.layouts(), first + 1);
    humanoid->setHealth(73.1f); // still reads 74
    a.shown();
    CHECK_EQ(a.bars.layouts(), first + 1);
    // At twice the scale, once more.
    a.shown(2.0f, {3840, 2160});
    a.shown(2.0f, {3840, 2160});
    CHECK_EQ(a.bars.layouts(), first + 2);
    CHECK_EQ(a.bars.bars()[0].rects.track.w, 10.0f);
}

TEST(health_bars_are_not_hidden_by_our_own_character)
{
    Arena a;
    auto me = a.join("me");
    a.players->assignLocalPlayer(me.get());
    a.spawn(*me, {0, 0, -10}); // the third person camera's view of us
    auto close = a.join("close"); // just behind us, their bar over our back
    a.spawn(*close, {0.5f, 0, -16});
    auto away = a.join("away"); // off to the side, clear of us
    a.spawn(*away, {14, 0, -30});
    CHECK(a.shown() == std::vector<std::string>{"close", "away"});
    // Behind us on the screen: drawn just in front of us (10 studs away, less
    // the 2.5 of our front), where our own body can't hide it.
    const float closeDepth = -a.bar("close")->origin.z;
    CHECK(closeDepth > 7 && closeDepth < 8);
    // Clear of us: at its own character's front, so walls still hide it.
    CHECK(-a.bar("away")->origin.z > 27 && -a.bar("away")->origin.z < 28);
}

TEST(health_bars_follow_the_animated_body_not_the_root)
{
    Arena a;
    auto other = a.join("other");
    a.spawn(*other, {0, 0, -20});
    Model* character = other->character();
    // A move has thrown the body 6 studs right of its root, and an effect
    // the game parented to the character is no part of it.
    auto part = [&](std::string_view name, Vector3 size, Vector3 at) {
        auto p = std::make_shared<Part>();
        p->setName(std::string(name));
        p->setSize(size);
        p->setCFrame(CFrame(at));
        p->setParent(character);
        return p;
    };
    part("Torso", {2, 2, 1}, {6, 0, -20});
    part("Head", {2, 1, 1}, {6, 1.5f, -20});
    part("Left Leg", {1, 2, 1}, {5.5f, -2, -20});
    part("Right Leg", {1, 2, 1}, {6.5f, -2, -20});
    part("Effect", {30, 30, 1}, {-20, 0, -20});
    a.shown();
    const HealthBars::Bar* bar = a.bar("other");
    CHECK(bar != nullptr);
    // Torso, head and legs: from 5 to 7 studs right of the camera, from 3
    // below the root to 2 above, their near faces 19.5 away, far ones 20.5.
    const float f = 1080 / (2 * std::tan(35 * std::numbers::pi_v<float> / 180));
    CHECK(near(bar->character.x, 960 + 5 * f / 20.5f, 0.5f));
    CHECK(near(bar->character.right(), 960 + 7 * f / 19.5f, 0.5f));
    CHECK(near(bar->character.y, 540 - 2 * f / 19.5f, 0.5f));
    CHECK(near(bar->character.bottom(), 540 + 3 * f / 19.5f, 0.5f));
    // Its depth goes by the body too.
    CHECK(bar->origin.x > 5);
}
