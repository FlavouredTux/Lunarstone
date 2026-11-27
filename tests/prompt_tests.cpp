// ProximityPrompts on a client (PromptSystem): which ones show, their key,
// holds, and the events each side hears.

#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Prompts.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace lunar;

namespace {

template <class T>
std::shared_ptr<T> make(const std::string& name, Instance* parent)
{
    auto instance = std::make_shared<T>();
    instance->setName(name);
    instance->setParent(parent);
    return instance;
}

// A player whose character stands at the origin, looking down -Z, and what
// the prompts fired: "<prompt>.<event>" on the prompts, "service.<event>"
// on ProximityPromptService, "server <prompt>.<event>" for the server.
struct World {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Player> player;
    std::shared_ptr<Model> character;
    PromptSystem prompts{*game};
    std::vector<std::string> heard;
    std::vector<Connection> connections;
    PromptSystem::View view{.reference = {0, 3, 0}, .camera = CFrame(0, 5, 8)};

    World()
    {
        auto* players = static_cast<Players*>(game->getService("Players"));
        player = make<Player>("Player1", players);
        players->assignLocalPlayer(player.get());
        character = make<Model>("Player1", game->workspace());
        auto root = make<Part>("HumanoidRootPart", character.get());
        root->setCFrame(CFrame(0, 3, 0));
        player->setCharacter(character.get());
        Instance* service = game->getService("ProximityPromptService");
        connections.push_back(service->eventFired.connect(
            [this](std::string_view event, std::span<const Value>) { heard.push_back(std::format("service.{}", event)); }));
        prompts.replicate = [this](ProximityPrompt& prompt, std::string_view event, Player& who) {
            CHECK(&who == player.get());
            heard.push_back(std::format("server {}.{}", prompt.parent()->name(), event));
        };
    }

    // A part at `at` with a prompt in it.
    std::shared_ptr<ProximityPrompt> prompt(const std::string& name, Vector3 at)
    {
        auto part = make<Part>(name, game->workspace());
        part->setCFrame(CFrame(at));
        auto prompt = make<ProximityPrompt>("ProximityPrompt", part.get());
        connections.push_back(prompt->eventFired.connect([this, name](std::string_view event, std::span<const Value>) {
            heard.push_back(std::format("{}.{}", name, event));
        }));
        return prompt;
    }

    void step(double dt = 1.0 / 60.0, const PromptSystem::Blocked& blocked = {})
    {
        prompts.step(dt, view, player.get(), blocked);
    }

    std::vector<std::string> take() { return std::exchange(heard, {}); }
};

using Lines = std::vector<std::string>;

} // namespace

TEST(prompt_shows_near_and_triggers_on_its_key)
{
    World w;
    auto door = w.prompt("Door", {0, 3, -5});
    w.step();
    CHECK_EQ(w.prompts.shown().size(), 1u);
    CHECK(w.take() == Lines{"Door.PromptShown", "service.PromptShown"});

    // Another key does nothing; E triggers at once (HoldDuration 0).
    CHECK(!w.prompts.press(Enum::KeyCode::F));
    CHECK(w.prompts.press(Enum::KeyCode::E));
    CHECK(w.take() == Lines{"Door.Triggered", "service.PromptTriggered", "server Door.TriggeredActionReplicated"});
    w.prompts.release(Enum::KeyCode::E);
    CHECK(w.take()
          == Lines{"Door.TriggerEnded", "service.PromptTriggerEnded", "server Door.TriggerEndedActionReplicated"});

    // Walking away hides it.
    w.view.reference = {0, 3, 20};
    w.step();
    CHECK(w.prompts.shown().empty());
    CHECK(w.take() == Lines{"Door.PromptHidden", "service.PromptHidden"});
}

TEST(prompt_holds_for_its_duration)
{
    World w;
    auto closet = w.prompt("Closet", {0, 3, -4});
    closet->setHoldDuration(1.0f);
    w.step();
    w.take();
    CHECK(w.prompts.press(Enum::KeyCode::E));
    CHECK(w.take()
          == Lines{"Closet.PromptButtonHoldBegan", "service.PromptButtonHoldBegan",
                   "server Closet.ButtonHoldBeganActionReplicated"});
    w.step(0.5);
    CHECK(w.take().empty());
    CHECK(std::abs(w.prompts.shown().front().progress - 0.5f) < 1e-4f);
    w.step(0.6);
    CHECK(w.take() == Lines{"Closet.Triggered", "service.PromptTriggered", "server Closet.TriggeredActionReplicated"});
    w.prompts.release(Enum::KeyCode::E);
    CHECK(w.take()
          == Lines{"Closet.PromptButtonHoldEnded", "service.PromptButtonHoldEnded",
                   "server Closet.ButtonHoldEndedActionReplicated", "Closet.TriggerEnded", "service.PromptTriggerEnded",
                   "server Closet.TriggerEndedActionReplicated"});

    // Let go early: the hold ends without a trigger.
    w.prompts.press(Enum::KeyCode::E);
    w.step(0.3);
    w.prompts.release(Enum::KeyCode::E);
    CHECK(w.take()
          == Lines{"Closet.PromptButtonHoldBegan", "service.PromptButtonHoldBegan",
                   "server Closet.ButtonHoldBeganActionReplicated", "Closet.PromptButtonHoldEnded",
                   "service.PromptButtonHoldEnded", "server Closet.ButtonHoldEndedActionReplicated"});
}

TEST(prompt_hidden_mid_hold_lets_go)
{
    World w;
    auto drawer = w.prompt("Drawer", {0, 3, -4});
    drawer->setHoldDuration(2.0f);
    w.step();
    w.prompts.press(Enum::KeyCode::E);
    w.take();
    drawer->setEnabled(false);
    w.step();
    CHECK(w.take()
          == Lines{"Drawer.PromptHidden", "service.PromptHidden", "Drawer.PromptButtonHoldEnded",
                   "service.PromptButtonHoldEnded", "server Drawer.ButtonHoldEndedActionReplicated"});
}

TEST(prompt_needs_to_be_on_screen_and_in_sight)
{
    World w;
    w.prompt("Behind", {0, 3, 9}); // behind the camera at z 8
    w.step();
    CHECK(w.prompts.shown().empty());

    auto ahead = w.prompt("Ahead", {0, 3, -4});
    w.step(1.0 / 60.0, [](Vector3, Vector3, const Instance*) { return true; });
    CHECK(w.prompts.shown().empty());
    ahead->setRequiresLineOfSight(false);
    w.step(1.0 / 60.0, [](Vector3, Vector3, const Instance*) { return true; });
    CHECK_EQ(w.prompts.shown().size(), 1u);
}

TEST(prompt_exclusivity_picks_the_nearest_per_key)
{
    World w;
    auto near = w.prompt("Near", {0, 3, -2});
    auto far = w.prompt("Far", {1, 3, -6});
    w.step();
    CHECK_EQ(w.prompts.shown().size(), 1u);
    CHECK(w.prompts.shown().front().prompt == near);

    // Another key gets its own; AlwaysShow always shows.
    far->setKeyboardKeyCode(Enum::KeyCode::F);
    w.step();
    CHECK_EQ(w.prompts.shown().size(), 2u);
    far->setKeyboardKeyCode(Enum::KeyCode::E);
    far->setExclusivity(Enum::ProximityPromptExclusivity::AlwaysShow);
    w.step();
    CHECK_EQ(w.prompts.shown().size(), 2u);

    // Pressing E holds every shown prompt of E.
    w.take();
    w.prompts.press(Enum::KeyCode::E);
    const Lines heard = w.take();
    CHECK(std::ranges::contains(heard, std::string("Near.Triggered")));
    CHECK(std::ranges::contains(heard, std::string("Far.Triggered")));
}

TEST(prompt_positions_follow_its_parent)
{
    auto game = DataModel::create();
    auto part = make<Part>("Part", game->workspace());
    part->setCFrame(CFrame(1, 2, 3));
    auto prompt = make<ProximityPrompt>("Prompt", part.get());
    CHECK(promptPosition(*prompt).value() == Vector3(1, 2, 3));
    auto attachment = make<Attachment>("Attachment", part.get());
    attachment->setCFrame(CFrame(0, 1, 0));
    prompt->setParent(attachment.get());
    CHECK(promptPosition(*prompt).value() == Vector3(1, 3, 3));
    prompt->setParent(game->workspace());
    CHECK(!promptPosition(*prompt));
}

TEST(prompt_defaults_and_methods_from_scripts)
{
    Harness h;
    CHECK(h.output(R"(
        local p = Instance.new("ProximityPrompt")
        print(p.ActionText, p.HoldDuration, p.KeyboardKeyCode, p.MaxActivationDistance, p.RequiresLineOfSight,
              p.Exclusivity, p.Style)
        p:InputHoldBegin()
        p:InputHoldEnd()
        print("held")
    )") == Lines{"Interact 0 Enum.KeyCode.E 10 true Enum.ProximityPromptExclusivity.OnePerButton "
                 "Enum.ProximityPromptStyle.Default",
                 "held"});
}
