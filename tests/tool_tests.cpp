// Tools: the RightGrip weld, the equip state machine and its events, touch
// pickup, dropping, Humanoid:EquipTool/UnequipTools, the backpack on spawn and
// the mouse in Play Solo.

#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Tools.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/ui/BackpackGui.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <numbers>
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

std::shared_ptr<Part> makePart(const std::string& name, Vector3 size, const CFrame& cframe, Instance* parent)
{
    auto part = make<Part>(name, parent);
    part->setSize(size);
    part->setCFrame(cframe);
    return part;
}

bool near(const CFrame& a, const CFrame& b, float eps = 1e-4f)
{
    if ((a.position - b.position).magnitude() > eps)
        return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(a.r[i][j] - b.r[i][j]) > eps)
                return false;
    return true;
}

// A player with a Backpack and a character standing at `at`, R15 or R6.
struct World {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Player> player;
    std::shared_ptr<Model> character;
    std::shared_ptr<Humanoid> humanoid;
    BasePart* hand = nullptr;
    std::unique_ptr<ToolSystem> tools;

    explicit World(bool r15 = true, Vector3 at = {0, 3, 0})
    {
        Instance* players = game->getService("Players");
        player = make<Player>("Player1", players);
        make<Backpack>("Backpack", player.get());
        character = make<Model>("Player1", game->workspace());
        humanoid = make<Humanoid>("Humanoid", character.get());
        humanoid->setRigType(r15 ? Enum::HumanoidRigType::R15 : Enum::HumanoidRigType::R6);
        makePart("HumanoidRootPart", {2, 2, 1}, CFrame(at), character.get());
        if (r15) {
            makePart("UpperTorso", {2, 1.6f, 1}, CFrame(at), character.get());
            hand = makePart("RightHand", {0.6f, 0.4f, 0.6f}, CFrame(at + Vector3{1.5f, -1.0f, 0}), character.get())
                       .get();
            auto attachment = make<Attachment>("RightGripAttachment", hand);
            attachment->setCFrame(CFrame(0, -0.15f, 0) * CFrame::angles(-std::numbers::pi_v<float> / 2, 0, 0));
        } else {
            auto torso = makePart("Torso", {2, 2, 1}, CFrame(at), character.get());
            hand = makePart("Right Arm", {1, 2, 1}, CFrame(at + Vector3{1.5f, 0, 0}), character.get()).get();
            auto shoulder = make<Motor6D>("Right Shoulder", torso.get());
            shoulder->setPart0(torso.get());
            shoulder->setPart1(hand);
        }
        player->setCharacter(character.get());
        tools = std::make_unique<ToolSystem>(*game);
    }

    Instance* backpack() const { return player->findFirstChildOfClass("Backpack"); }
};

std::shared_ptr<Tool> makeTool(const std::string& name, Instance* parent, Vector3 at = {20, 1, 0})
{
    auto tool = std::make_shared<Tool>();
    tool->setName(name);
    auto handle = makePart("Handle", {1, 1, 4}, CFrame(at), tool.get());
    makePart("Blade", {0.2f, 0.2f, 3}, CFrame(at + Vector3{0, 0, -3}), tool.get());
    tool->setParent(parent);
    return tool;
}

// Records the tool's events by name.
struct Events {
    std::vector<std::string> fired;
    std::vector<Value> lastArgs;
    Connection connection;

    explicit Events(Tool& tool)
    {
        connection = tool.eventFired.connect([this](std::string_view name, std::span<const Value> args) {
            fired.emplace_back(name);
            lastArgs.assign(args.begin(), args.end());
        });
    }
};

} // namespace

// --- The grip --------------------------------------------------------------------

TEST(tool_equipping_welds_the_handle_to_the_right_hand_r15)
{
    World w;
    auto tool = makeTool("Sword", w.backpack());
    tool->setGrip(CFrame(0, 0, 1.5f));
    CHECK(w.tools->state(*tool) == ToolState::None);
    Events events(*tool);

    tool->setParent(w.character);
    CHECK(w.tools->state(*tool) == ToolState::Equipped);
    CHECK(rightGrip(*w.humanoid) != nullptr);
    auto grip = std::static_pointer_cast<Weld>(rightGrip(*w.humanoid)->shared_from_this());
    CHECK_EQ(grip->name(), "RightGrip");
    CHECK(grip->parent() == w.hand);
    CHECK(grip->part0() == w.hand);
    CHECK(grip->part1() == tool->handle());
    const auto* attachment = static_cast<Attachment*>(w.hand->findFirstChild("RightGripAttachment"));
    CHECK(near(grip->c0(), attachment->cframe()));
    CHECK(near(grip->c1(), tool->grip()));
    // The tool moved into the hand, the blade with it.
    CHECK(near(tool->handle()->cframe(), w.hand->cframe() * grip->c0() * grip->c1().inverse()));
    const auto* blade = static_cast<BasePart*>(tool->findFirstChild("Blade"));
    CHECK(near(blade->cframe(), tool->handle()->cframe() * CFrame(0, 0, -3)));
    CHECK_EQ(events.fired.size(), std::size_t{1});
    CHECK_EQ(events.fired.at(0), "Equipped");

    tool->setParent(w.backpack());
    CHECK(w.tools->state(*tool) == ToolState::None);
    CHECK(rightGrip(*w.humanoid) == nullptr);
    CHECK(grip->parent() == nullptr);
    CHECK_EQ(events.fired.size(), std::size_t{2});
    CHECK_EQ(events.fired.at(1), "Unequipped");
}

TEST(tool_grip_offset_r6_and_r15_without_attachment)
{
    World r6(false);
    const CFrame down = CFrame::angles(-std::numbers::pi_v<float> / 2, 0, 0);
    // CFrame.new(0, -1, 0, 1, 0, -0, 0, 0, 1, 0, -1, -0)
    const CFrame expected = CFrame(0, -1, 0) * down;
    CHECK(near(gripOffset(*r6.humanoid), expected));
    CHECK(std::abs(expected.r[1][2] - 1.0f) < 1e-6f);
    CHECK(std::abs(expected.r[2][1] + 1.0f) < 1e-6f);
    auto tool = makeTool("Sword", r6.character.get());
    CHECK(r6.tools->state(*tool) == ToolState::Equipped);
    CHECK(rightGrip(*r6.humanoid) != nullptr);
    CHECK(rightGrip(*r6.humanoid)->parent() == r6.hand);

    World r15;
    r15.hand->findFirstChild("RightGripAttachment")->destroy();
    CHECK(near(gripOffset(*r15.humanoid), CFrame(0, -0.2f, 0) * down));
}

TEST(tool_needs_a_hand_and_a_handle_when_it_requires_one)
{
    World w(false);
    // No Right Shoulder: the arm can't hold it, so no weld and no Equipped.
    w.character->findFirstChild("Torso")->findFirstChild("Right Shoulder")->destroy();
    auto tool = makeTool("Sword", w.backpack());
    Events events(*tool);
    tool->setParent(w.character);
    CHECK(w.tools->state(*tool) == ToolState::Gripped);
    CHECK(events.fired.empty());

    // Without a Handle a RequiresHandle tool goes nowhere; without RequiresHandle it equips bare.
    World v;
    auto bare = std::make_shared<Tool>();
    Events bareEvents(*bare);
    bare->setParent(v.character);
    CHECK(v.tools->state(*bare) == ToolState::None);
    bare->setParent(v.backpack());
    bare->setRequiresHandle(false);
    bare->setParent(v.character);
    CHECK(v.tools->state(*bare) == ToolState::Equipped);
    CHECK(rightGrip(*v.humanoid) == nullptr);
    CHECK_EQ(bareEvents.fired.size(), std::size_t{1});

    // A Handle arriving later equips the RequiresHandle tool where it is.
    auto late = std::make_shared<Tool>();
    late->setParent(v.character);
    CHECK(v.tools->state(*late) == ToolState::None);
    makePart("Handle", {1, 1, 1}, CFrame(), late.get());
    CHECK(v.tools->state(*late) == ToolState::Equipped);
}

TEST(tool_in_the_workspace_is_loose_and_outside_it_none)
{
    World w;
    auto tool = makeTool("Sword", w.game->workspace());
    CHECK(w.tools->state(*tool) == ToolState::InWorkspace);
    auto model = make<Model>("Shelf", w.game->workspace());
    tool->setParent(model);
    CHECK(w.tools->state(*tool) == ToolState::InWorkspace);
    tool->setParent(w.game->getService("ReplicatedStorage"));
    CHECK(w.tools->state(*tool) == ToolState::None);
}

TEST(tool_moving_between_characters_unequips_then_equips)
{
    World w;
    auto other = make<Model>("Npc", w.game->workspace());
    auto npcHumanoid = make<Humanoid>("Humanoid", other.get());
    npcHumanoid->setRigType(Enum::HumanoidRigType::R15);
    makePart("UpperTorso", {2, 1.6f, 1}, CFrame(10, 3, 0), other.get());
    makePart("RightHand", {1, 1, 1}, CFrame(11, 2, 0), other.get());
    auto tool = makeTool("Sword", w.character.get());
    Events events(*tool);
    tool->setParent(other);
    CHECK(rightGrip(*w.humanoid) == nullptr);
    CHECK(rightGrip(*npcHumanoid) != nullptr);
    CHECK_EQ(events.fired.size(), std::size_t{2});
    CHECK_EQ(events.fired.at(0), "Unequipped");
    CHECK_EQ(events.fired.at(1), "Equipped");
}

TEST(tool_equipped_passes_the_mouse_and_scripts_moving_it_win)
{
    World w;
    auto mouse = std::make_shared<Folder>();
    w.tools->mouse = [&](Player& player) -> InstancePtr { return &player == w.player.get() ? mouse : nullptr; };
    auto tool = makeTool("Sword", w.backpack());
    Events events(*tool);
    tool->setParent(w.character);
    CHECK_EQ(events.lastArgs.size(), std::size_t{1});
    CHECK(std::get<InstancePtr>(events.lastArgs.at(0)) == mouse);

    // An Unequipped handler that equips it again: the nested move decides.
    tool->setParent(w.backpack());
    Connection again = tool->eventFired.connect([&](std::string_view name, std::span<const Value>) {
        if (name == "Unequipped")
            tool->setParent(w.character);
    });
    tool->setParent(w.character);
    tool->setParent(w.backpack());
    again.disconnect();
    CHECK(tool->parent() == w.character.get());
    CHECK(w.tools->state(*tool) == ToolState::Equipped);
    CHECK(rightGrip(*w.humanoid) != nullptr);
}

// --- Activation ----------------------------------------------------------------------

TEST(tool_activation_follows_enabled_and_manual_activation_only)
{
    World w;
    auto tool = makeTool("Sword", w.backpack());
    Events events(*tool);
    CHECK(!w.tools->activate(*tool, true)); // not equipped
    CHECK(!w.tools->deactivate(*tool, true));
    tool->setParent(w.character);
    events.fired.clear();
    CHECK(w.tools->activate(*tool, false));
    CHECK(w.tools->deactivate(*tool, false));
    tool->setManualActivationOnly(true);
    CHECK(w.tools->activate(*tool, false));   // the mouse can't...
    CHECK(w.tools->activate(*tool, true));    // ...scripts can
    CHECK(w.tools->deactivate(*tool, false)); // and letting go still deactivates
    tool->setEnabled(false);
    CHECK(w.tools->activate(*tool, true));
    const std::vector<std::string> expected{"Activated", "Deactivated", "Activated", "Deactivated"};
    CHECK(events.fired == expected);
}

// --- Picking up and dropping ------------------------------------------------------------

TEST(tool_touch_picks_it_up_and_puts_the_held_one_away)
{
    World w;
    auto held = makeTool("Old", w.character.get());
    auto tool = makeTool("Sword", w.game->workspace(), {20, 1, 0});
    w.tools->pickUpTouching();
    CHECK(tool->parent() == w.game->workspace()); // too far away

    // Its handle overlapping the root part: picked up, the old one put away.
    tool->handle()->setCFrame(CFrame(0.5f, 3, 0));
    w.tools->pickUpTouching();
    CHECK(tool->parent() == w.character.get());
    CHECK(held->parent() == w.backpack());
    CHECK(w.tools->state(*tool) == ToolState::Equipped);
    CHECK(rightGrip(*w.humanoid)->part1() == tool->handle());
}

TEST(tool_touch_ignores_the_dead_npcs_and_untouchable_handles)
{
    World w;
    auto tool = makeTool("Sword", w.game->workspace(), {0.5f, 3, 0});
    tool->handle()->setCanTouch(false);
    w.tools->pickUpTouching();
    CHECK(tool->parent() == w.game->workspace());
    tool->handle()->setCanTouch(true);
    w.humanoid->setHealth(0);
    w.tools->pickUpTouching();
    CHECK(tool->parent() == w.game->workspace());
    w.humanoid->setHealth(100);
    // A character no player owns can't pick it up either.
    w.player->setCharacter(nullptr);
    w.tools->touched(*tool, *static_cast<BasePart*>(w.character->findFirstChild("HumanoidRootPart")));
    CHECK(tool->parent() == w.game->workspace());
    w.player->setCharacter(w.character.get());
    w.tools->touched(*tool, *static_cast<BasePart*>(w.character->findFirstChild("HumanoidRootPart")));
    CHECK(tool->parent() == w.character.get());
}

TEST(tool_drop_puts_droppable_tools_in_front_of_the_character)
{
    World w(true, {0, 3, 0});
    auto root = static_cast<BasePart*>(w.character->findFirstChild("HumanoidRootPart"));
    // Facing +X.
    root->setCFrame(CFrame(0, 3, 0) * CFrame::angles(0, -std::numbers::pi_v<float> / 2, 0));
    auto keep = makeTool("Keep", w.character.get());
    keep->setCanBeDropped(false);
    auto tool = makeTool("Sword", w.character.get());
    tool->setGrip(CFrame(0, 0, 1));
    dropTools(*w.game, *w.player);
    CHECK(keep->parent() == w.character.get());
    CHECK(tool->parent() == w.game->workspace());
    CHECK(w.tools->state(*tool) == ToolState::InWorkspace);
    // Its grip point 8 studs ahead and 4 up.
    const CFrame gripPoint = tool->handle()->cframe() * tool->grip();
    CHECK((gripPoint.position - Vector3{8, 7, 0}).magnitude() < 1e-3f);
}

// --- EquipTool / UnequipTools -----------------------------------------------------------------

TEST(tool_equip_tool_unequips_the_others)
{
    World w;
    auto a = makeTool("A", w.backpack());
    auto b = makeTool("B", w.backpack());
    equipTool(*w.game, *w.humanoid, *a);
    CHECK(a->parent() == w.character.get());
    equipTool(*w.game, *w.humanoid, *b);
    CHECK(a->parent() == w.backpack());
    CHECK(b->parent() == w.character.get());
    CHECK(unequipTools(*w.game, *w.humanoid));
    CHECK(b->parent() == w.backpack());
    w.backpack()->destroy();
    equipTool(*w.game, *w.humanoid, *makeTool("C", w.game->workspace()));
    CHECK(!unequipTools(*w.game, *w.humanoid));
}

// --- A client of a live server --------------------------------------------------------

namespace {

// World with the player as the local one and its ToolSystem in client mode.
struct ClientWorld : World {
    ClientWorld()
    {
        static_cast<Players*>(game->findFirstChildOfClass("Players"))->assignLocalPlayer(player.get());
        tools->setMode(ToolSystem::Mode::Client);
    }
};

} // namespace

TEST(tool_client_welds_only_what_comes_from_its_own_backpack)
{
    ClientWorld w;
    // Someone else's character holding a tool: theirs to weld, not ours.
    auto other = make<Model>("Other", w.game->workspace());
    auto otherHumanoid = make<Humanoid>("Humanoid", other.get());
    otherHumanoid->setRigType(Enum::HumanoidRigType::R15);
    makePart("HumanoidRootPart", {2, 2, 1}, CFrame(10, 3, 0), other.get());
    makePart("UpperTorso", {2, 1.6f, 1}, CFrame(10, 3, 0), other.get());
    makePart("RightHand", {0.6f, 0.4f, 0.6f}, CFrame(11.5f, 2, 0), other.get());
    auto theirs = makeTool("Theirs", other.get());
    CHECK(w.tools->state(*theirs) == ToolState::None);
    CHECK(rightGrip(*otherHumanoid) == nullptr);

    // From our Backpack into our character: we weld it.
    auto sword = makeTool("Sword", w.backpack());
    CHECK(w.tools->state(*sword) == ToolState::None);
    sword->setParent(w.character.get());
    CHECK(w.tools->state(*sword) == ToolState::Equipped);
    CHECK(w.tools->weldsLocally(*sword));
    Weld* grip = rightGrip(*w.humanoid);
    CHECK(grip != nullptr && grip->part1() == sword->handle());
    sword->setParent(w.backpack());
    CHECK(w.tools->state(*sword) == ToolState::None);
    CHECK(rightGrip(*w.humanoid) == nullptr);

    // From the Workspace (the server picked it up for us): the server's weld
    // is the one in the hand, and stays when the tool goes.
    auto loose = makeTool("Loose", w.game->workspace());
    CHECK(w.tools->state(*loose) == ToolState::None); // no touch pickup on a client
    const CFrame where = loose->handle()->cframe();
    auto serverGrip = make<Weld>("RightGrip", nullptr);
    serverGrip->setPart0(w.hand);
    serverGrip->setPart1(loose->handle());
    serverGrip->setParent(w.hand);
    loose->setParent(w.character.get());
    CHECK(w.tools->state(*loose) == ToolState::Equipped);
    CHECK(!w.tools->weldsLocally(*loose));
    CHECK(near(loose->handle()->cframe(), where)); // not moved to the hand here
    CHECK(w.hand->children().size() == 2);         // the attachment and the server's weld
    loose->setParent(w.backpack());
    CHECK(serverGrip->parent() == w.hand);

    // Back in client mode's terms: a backend welds everything again.
    w.tools->setMode(ToolSystem::Mode::Backend);
    CHECK(w.tools->state(*theirs) == ToolState::Equipped);
    CHECK(rightGrip(*otherHumanoid) != nullptr);
}

TEST(tool_client_equip_tool_asks_the_server_for_a_loose_tool)
{
    ClientWorld w;
    std::vector<std::string> asked;
    w.humanoid->eventFired.connect([&](std::string_view event, std::span<const Value> args) {
        if (event == "ServerEquipTool" && args.size() == 1)
            if (const auto* tool = std::get_if<InstancePtr>(&args[0]); tool && *tool)
                asked.push_back((*tool)->name());
    });
    auto held = makeTool("Held", w.backpack());
    w.tools->equip(*w.humanoid, *held);
    CHECK(held->parent() == w.character.get());
    auto loose = makeTool("Loose", w.game->workspace());
    w.tools->equip(*w.humanoid, *loose);
    CHECK(loose->parent() == w.game->workspace()); // the server moves it, if it agrees
    CHECK(held->parent() == w.backpack());         // put away first, as the client does
    CHECK(asked == std::vector<std::string>{"Loose"});
}

TEST(tool_client_session_hotbar_mouse_and_drop)
{
    auto game = DataModel::create();
    auto player = make<Player>("Player1", game->getService("Players"));
    player->setUserId(5);
    auto backpack = make<Backpack>("Backpack", player.get());
    auto character = make<Model>("Player1", game->workspace());
    auto humanoid = make<Humanoid>("Humanoid", character.get());
    humanoid->setRigType(Enum::HumanoidRigType::R15);
    makePart("HumanoidRootPart", {2, 2, 1}, CFrame(0, 3, 0), character.get());
    makePart("UpperTorso", {2, 1.6f, 1}, CFrame(0, 3, 0), character.get());
    makePart("RightHand", {0.6f, 0.4f, 0.6f}, CFrame(1.5f, 2, 0), character.get());
    player->setCharacter(character.get());
    auto first = makeTool("First", backpack.get());
    auto sign = makeTool("Sign", backpack.get());
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local tool = script.Parent
        tool.Equipped:Connect(function() print("equipped") end)
        tool.Activated:Connect(function() print("activated") end)
        tool.Deactivated:Connect(function() print("deactivated") end)
        tool.Unequipped:Connect(function() print("unequipped") end)
    )");
    script->setParent(sign.get());
    ClientSession client(game);
    std::vector<std::string> lines;
    client.client().setOutput([&lines](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(client.tools().mode() == ToolSystem::Mode::Client);
    CHECK(client.start(5));
    client.step(1.0 / 60.0);
    CHECK(!client.selectTool(2)); // nothing in slot 3
    CHECK(client.selectTool(1));
    CHECK(sign->parent() == character.get());
    CHECK(client.hotbar().slot(0) == first.get());
    CHECK(rightGrip(*humanoid) != nullptr);
    client.toolButton(true);
    client.toolButton(false);
    client.dropTools(); // a Tool CanBeDropped by default
    CHECK(sign->parent() == game->workspace());
    client.step(1.0 / 60.0);
    const std::vector<std::string> expected{"equipped", "activated", "deactivated", "unequipped"};
    CHECK(lines == expected);
    CHECK(client.selectTool(0));
    client.unequipTools();
    CHECK(first->parent() == backpack.get());
}

TEST(tool_grip_pieces_edit_grip)
{
    Tool tool;
    tool.setGripPos({1, 2, 3});
    CHECK(tool.grip().position == (Vector3{1, 2, 3}));
    CHECK(tool.gripForward() == (Vector3{0, 0, -1}));
    CHECK(tool.gripRight() == (Vector3{1, 0, 0}));
    CHECK(tool.gripUp() == (Vector3{0, 1, 0}));
    tool.setGripForward({0, -1, 0});
    tool.setGripUp({0, 0, 1});
    CHECK(tool.gripForward() == (Vector3{0, -1, 0}));
    CHECK(tool.gripUp() == (Vector3{0, 0, 1}));
    CHECK(tool.grip().r[1][2] == 1.0f);
    CHECK(tool.get("GripPos") == Value(Vector3{1, 2, 3}));
}

// --- From scripts ------------------------------------------------------------------------------

TEST(tool_script_api_events_and_warnings)
{
    Harness h;
    auto player = make<Player>("Player1", h.game->getService("Players"));
    make<Backpack>("Backpack", player.get());
    h.run(R"(
        local player = game:GetService("Players").Player1
        local character = Instance.new("Model")
        character.Name = "Player1"
        local humanoid = Instance.new("Humanoid", character)
        humanoid.RigType = Enum.HumanoidRigType.R6
        local torso = Instance.new("Part", character) torso.Name = "Torso"
        local arm = Instance.new("Part", character) arm.Name = "Right Arm"
        local shoulder = Instance.new("Motor6D", torso) shoulder.Name = "Right Shoulder"
        character.Parent = workspace
        player.Character = character

        local tool = Instance.new("Tool")
        tool.Name = "Sword"
        local handle = Instance.new("Part", tool) handle.Name = "Handle"
        tool.Parent = player.Backpack
        tool:Activate()
        tool.Equipped:Connect(function(mouse)
            print("equipped", mouse == player:GetMouse(), arm:FindFirstChild("RightGrip") ~= nil)
        end)
        tool.Unequipped:Connect(function() print("unequipped") end)
        tool.Activated:Connect(function() print("activated") end)
        tool.Deactivated:Connect(function() print("deactivated") end)
        humanoid:EquipTool(tool)
        print(tool.Parent.Name, arm.RightGrip.Part1 == handle, arm.RightGrip.ClassName)
        tool:Activate()
        tool:Deactivate()
        humanoid:UnequipTools()
        print(tool.Parent.Name)

        local gui = game:GetService("StarterGui")
        print(gui:GetCoreGuiEnabled(Enum.CoreGuiType.Backpack))
        gui:SetCoreGuiEnabled(Enum.CoreGuiType.Backpack, false)
        print(gui:GetCoreGuiEnabled(Enum.CoreGuiType.Backpack), gui:GetCoreGuiEnabled("Health"),
              gui:GetCoreGuiEnabled(Enum.CoreGuiType.All))
        gui:SetCoreGuiEnabled(Enum.CoreGuiType.All, true)
        print(gui:GetCoreGuiEnabled(Enum.CoreGuiType.All))
        print(pcall(gui.SetCoreGuiEnabled, gui, nil, true))
    )");
    const std::vector<std::string> expected{
        "warn: Tool:Activate() called from script when tool is not equipped. Tool will not be activated.",
        "equipped true true",
        "Player1 true Weld",
        "activated",
        "deactivated",
        "unequipped",
        "Backpack",
        "true",
        "false true false",
        "true",
        "false Argument 1 missing or nil",
    };
    CHECK_EQ(h.lines.size(), expected.size());
    for (std::size_t i = 0; i < expected.size() && i < h.lines.size(); ++i)
        CHECK_EQ(h.lines[i], expected[i]);
}

// --- Play Solo --------------------------------------------------------------------------------

namespace {

struct Solo {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::unique_ptr<PlaySession> session;
    std::vector<std::string> lines;

    void open()
    {
        session = std::make_unique<PlaySession>(game);
        session->setOutput([this](ScriptRole role, MessageType type, std::string_view text) {
            if (type != MessageType::Info)
                lines.push_back(std::format("{}: {}", role == ScriptRole::Server ? "server" : "client", text));
        });
    }
    bool has(std::string_view line) const { return std::ranges::find(lines, line) != lines.end(); }
};

} // namespace

TEST(tool_play_solo_fills_a_new_backpack_on_every_spawn)
{
    Solo s;
    makeTool("Pack", s.game->getService("StarterPack"));
    s.open();
    s.session->start();
    Player* player = s.session->localPlayer();
    CHECK(player->findFirstChildOfClass("StarterGear") != nullptr);
    std::weak_ptr<Instance> first = player->findFirstChildOfClass("Backpack")->shared_from_this();
    CHECK(first.lock()->findFirstChild("Pack") != nullptr);
    makeTool("Gear", player->findFirstChildOfClass("StarterGear"));

    auto script = std::make_shared<Script>();
    script->setSource("game:GetService('Players').Player1:LoadCharacter()");
    script->setParent(s.game->getService("ServerScriptService"));
    for (int i = 0; i < 3; ++i)
        s.session->step(1.0 / 60.0);
    Instance* second = player->findFirstChildOfClass("Backpack");
    CHECK(first.expired()); // destroyed and gone
    CHECK_EQ(second->children().size(), std::size_t{2});
    CHECK_EQ(second->children()[0]->name(), "Pack");
    CHECK_EQ(second->children()[1]->name(), "Gear");
}

TEST(tool_play_solo_mouse_activates_and_backspace_drops)
{
    Solo s;
    auto tool = makeTool("Sword", s.game->getService("StarterPack"));
    tool->setRequiresHandle(false); // the stand-in character has a Right Arm but no shoulder joint
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local tool = script.Parent
        local mouse = game:GetService("Players").LocalPlayer:GetMouse()
        tool.Equipped:Connect(function(m) print("equipped", m == mouse) end)
        tool.Activated:Connect(function() print("activated") end)
        tool.Deactivated:Connect(function() print("deactivated") end)
        mouse.Button1Down:Connect(function() print("button1down") end)
        tool.Unequipped:Connect(function() print("unequipped", tool.Parent.Name) end)
    )");
    script->setParent(tool.get());
    s.open();
    s.session->start();
    s.session->step(1.0 / 60.0);
    Player* player = s.session->localPlayer();
    Instance* sword = player->findFirstChildOfClass("Backpack")->findFirstChild("Sword");
    CHECK(sword != nullptr);
    sword->setParent(player->character());
    s.session->mouseButton(Enum::UserInputType::MouseButton1, true);
    s.session->step(1.0 / 60.0);
    s.session->mouseButton(Enum::UserInputType::MouseButton1, false);
    s.session->step(1.0 / 60.0);
    s.session->keyDown(Enum::KeyCode::Backspace);
    s.session->step(1.0 / 60.0);
    CHECK(sword->parent() == s.game->workspace());
    const std::vector<std::string> expected{"client: equipped true", "client: activated", "client: button1down",
                                            "client: deactivated", "client: unequipped Workspace"};
    std::vector<std::string> client;
    for (const std::string& line : s.lines)
        if (line.starts_with("client: "))
            client.push_back(line);
    CHECK(client == expected);
}

// --- The hotbar ----------------------------------------------------------------------------

namespace {

struct HotbarGui {
    World w;
    ui::FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    ui::GlyphAtlas atlas;
    ui::BackpackGui gui{fonts, atlas};

    HotbarGui()
    {
        static_cast<Players*>(w.game->findFirstChildOfClass("Players"))->assignLocalPlayer(w.player.get());
        gui.setGame(w.game.get());
    }
    void layout() { gui.update({1280, 720}, 1.0f); }
    void key(platform::Key key) { gui.handle(platform::KeyEvent{.key = key, .pressed = true}); }
    bool click(ui::RectF r)
    {
        const double x = r.x + r.w / 2;
        const double y = r.y + r.h / 2;
        const bool down = gui.handle(platform::MouseButtonEvent{.pressed = true, .x = x, .y = y});
        const bool up = gui.handle(platform::MouseButtonEvent{.pressed = false, .x = x, .y = y});
        return down && up;
    }
};

} // namespace

TEST(tool_hotbar_fills_the_lowest_slots_and_centres_them)
{
    HotbarGui h;
    auto held = makeTool("Held", h.w.character.get());
    auto a = makeTool("A", h.w.backpack());
    auto b = makeTool("B", h.w.backpack());
    h.layout();
    // The character's tools first, then the Backpack's.
    CHECK(h.gui.slot(0) == held.get());
    CHECK(h.gui.slot(1) == a.get());
    CHECK(h.gui.slot(2) == b.get());
    CHECK(!h.gui.slotRect(3));
    // Three of ten slots, centred: frame 655 wide at x 313 (1280 / 2 - 327,
    // UDim offsets being whole), slots at 297.5 + 65 * (i - 2), cut to whole pixels.
    const ui::RectF first = h.gui.slotRect(0).value_or(ui::RectF{});
    CHECK_EQ(first.w, 60.0f);
    CHECK_EQ(first.y, 720.0f - 70.0f + 5.0f);
    CHECK_EQ(first.x, 313.0f + 232.0f);
    CHECK_EQ(h.gui.slotRect(2).value_or(ui::RectF{}).x, 313.0f + 362.0f);

    // Moving between character and Backpack keeps the slot; leaving both frees it.
    held->setParent(h.w.backpack());
    a->setParent(h.w.game->workspace());
    h.layout();
    CHECK(h.gui.slot(0) == held.get());
    CHECK(h.gui.slot(1) == nullptr);
    auto c = makeTool("C", h.w.backpack());
    h.layout();
    CHECK(h.gui.slot(1) == c.get());
}

TEST(tool_hotbar_keys_and_clicks_equip_and_put_away)
{
    HotbarGui h;
    auto a = makeTool("A", h.w.backpack());
    auto b = makeTool("B", h.w.backpack());
    h.layout();
    h.key(platform::Key::Num1);
    CHECK(a->parent() == h.w.character.get());
    CHECK(h.w.tools->state(*a) == ToolState::Equipped);
    h.key(platform::Key::Num2); // swaps
    CHECK(a->parent() == h.w.backpack());
    CHECK(b->parent() == h.w.character.get());
    h.key(platform::Key::Num2); // puts it away
    CHECK(b->parent() == h.w.backpack());
    h.key(platform::Key::Num9); // empty slot: nothing
    CHECK(rightGrip(*h.w.humanoid) == nullptr);

    h.layout();
    CHECK(h.click(h.gui.slotRect(0).value_or(ui::RectF{})));
    CHECK(a->parent() == h.w.character.get());
    CHECK(!h.click({0, 0, 10, 10})); // not on a slot: the game gets it

    // StarterGui:SetCoreGuiEnabled(Backpack, false): no hotbar, no keys.
    h.w.game->getService<StarterGui>()->setCoreGuiEnabled(Enum::CoreGuiType::Backpack, false);
    h.layout();
    CHECK(!h.gui.slotRect(0));
    h.key(platform::Key::Num1);
    CHECK(a->parent() == h.w.character.get());
    h.w.game->getService<StarterGui>()->setCoreGuiEnabled(Enum::CoreGuiType::All, true);
    h.key(platform::Key::Num1);
    CHECK(a->parent() == h.w.backpack());
}

TEST(tool_hotbar_draws_slots_highlight_and_names)
{
    HotbarGui h;
    if (!std::filesystem::exists(h.fonts.contentDir() / "fonts/families/SourceSansPro.json"))
        SKIP("no Roblox content folder");
    makeTool("Sword", h.w.character.get());
    makeTool("Bow", h.w.backpack());
    h.layout();
    ui::DrawList list;
    h.gui.draw(list);
    // Two backgrounds and the held slot's four blue edges.
    int backgrounds = 0;
    int edges = 0;
    for (const ui::Quad& q : list.quads) {
        if (q.kind != ui::QuadKind::Solid)
            continue;
        if (q.color == ui::Color4::from(ui::BackpackGui::background, ui::BackpackGui::backgroundFade))
            ++backgrounds;
        if (q.color == ui::Color4::from(ui::BackpackGui::equipColor))
            ++edges;
    }
    CHECK_EQ(backgrounds, 2);
    CHECK_EQ(edges, 4);
    CHECK(std::ranges::any_of(list.quads, [](const ui::Quad& q) { return q.kind == ui::QuadKind::Glyph; }));
}
