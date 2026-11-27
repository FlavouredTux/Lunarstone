// require, the frame order, events by name, signal behaviour, bindables and
// the services. Messages are the 0.742 client's own strings.

#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/physics/World.hpp>
#include <lunar/script/PlaySession.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace lunar;

namespace {

std::shared_ptr<ModuleScript> makeModule(const std::string& name, const std::string& source, Instance* parent)
{
    auto module = std::make_shared<ModuleScript>();
    module->setName(name);
    module->setSource(source);
    module->setParent(parent);
    return module;
}

template <class T>
std::shared_ptr<T> make(const std::string& name, Instance* parent)
{
    auto instance = std::make_shared<T>();
    instance->setName(name);
    instance->setParent(parent);
    return instance;
}

} // namespace

// --- require ---------------------------------------------------------------------

TEST(script_require_caches_module_results)
{
    Harness h;
    auto storage = h.game->getService("ReplicatedStorage");
    makeModule("Counter", "print('loading') return {count = 0}", storage);
    h.run(R"(
        local a = require(game.ReplicatedStorage.Counter)
        local b = require(game.ReplicatedStorage.Counter)
        a.count += 1
        print(rawequal(a, b), b.count)
    )");
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[0], "loading");
    CHECK_EQ(h.lines[1], "true 1");
}

TEST(script_require_sets_script_and_errors_like_roblox)
{
    Harness h;
    auto storage = h.game->getService("ReplicatedStorage");
    makeModule("Named", "return script:GetFullName()", storage);
    makeModule("Two", "return 1, 2", storage);
    makeModule("Broken", "error('boom')", storage);
    makeModule("A", "return require(script.Parent.B)", storage);
    makeModule("B", "return require(script.Parent.A)", storage);
    CHECK_EQ(h.print("print(require(game.ReplicatedStorage.Named))"), "ReplicatedStorage.Named");
    CHECK_EQ(h.error("require(game.ReplicatedStorage.Two)"), "Module code did not return exactly one value");
    CHECK_EQ(h.error("require(game.ReplicatedStorage.Broken)"), "Requested module experienced an error while loading");
    // The module's own error went to the output first, and the failure sticks.
    CHECK(h.lines.back() == "error: ReplicatedStorage.Broken:1: boom");
    CHECK_EQ(h.error("require(game.ReplicatedStorage.Broken)"), "Requested module experienced an error while loading");
    CHECK(
        h.error("require(game.ReplicatedStorage.A)").ends_with("Requested module experienced an error while loading"));
    bool recursive = false;
    for (const std::string& line : h.lines)
        recursive = recursive || line.ends_with("Requested module was required recursively");
    CHECK(recursive);
    CHECK_EQ(h.error("require(workspace)"), "Attempted to call require with invalid argument(s).");
    // A string is a path now (require by string); 0.742 called any string invalid.
    CHECK_EQ(h.error("require('x')"), "require path must start with a valid prefix: ./, ../, or @");
}

TEST(script_require_waits_for_a_yielding_module)
{
    Harness h;
    auto storage = h.game->getService("ReplicatedStorage");
    makeModule("Slow", "task.wait(0.1) return 'ready'", storage);
    h.run(R"(
        task.spawn(function() print("first", require(game.ReplicatedStorage.Slow)) end)
        task.spawn(function() print("second", require(game.ReplicatedStorage.Slow)) end)
    )");
    CHECK(h.lines.empty());
    h.steps(10);
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[0], "first ready");
    CHECK_EQ(h.lines[1], "second ready");
}

// --- The frame ---------------------------------------------------------------------

TEST(script_frame_runs_run_service_steps_in_order)
{
    Harness h;
    h.run(R"(
        local RunService = game:GetService("RunService")
        local log = {}
        _G.log = log
        RunService:BindToRenderStep("late", 300, function() table.insert(log, "bind300") end)
        RunService:BindToRenderStep("early", 100, function() table.insert(log, "bind100") end)
        RunService.RenderStepped:Connect(function() table.insert(log, "RenderStepped") end)
        RunService.PreRender:Connect(function() table.insert(log, "PreRender") end)
        RunService.PreAnimation:Connect(function() table.insert(log, "PreAnimation") end)
        RunService.Stepped:Connect(function(t, dt) table.insert(log, "Stepped") end)
        RunService.PreSimulation:Connect(function() table.insert(log, "PreSimulation") end)
        RunService.PostSimulation:Connect(function() table.insert(log, "PostSimulation") end)
        RunService.Heartbeat:Connect(function(dt) table.insert(log, "Heartbeat") end)
        task.spawn(function() task.wait() table.insert(log, "task.wait") end)
        task.spawn(function() wait() table.insert(log, "wait") end)
    )");
    h.steps(2);
    h.run("print(table.concat(_G.log, ' ', 1, 11))");
    CHECK_EQ(h.lines.back(), concat("bind100 bind300 RenderStepped PreRender PreAnimation Stepped ",
                                    "PreSimulation PostSimulation Heartbeat task.wait bind100"));
}

TEST(script_legacy_wait_resumes_before_the_simulation)
{
    Harness h;
    h.run(R"(
        local RunService = game:GetService("RunService")
        _G.log = {}
        RunService.PreAnimation:Connect(function() table.insert(_G.log, "PreAnimation") end)
        task.spawn(function() local elapsed, now = wait() table.insert(_G.log, "wait " .. tostring(elapsed >= 0.03)) end)
    )");
    h.steps(2);
    h.run("print(table.concat(_G.log, ' '))");
    CHECK_EQ(h.lines.back(), "PreAnimation wait true PreAnimation");
}

// --- Events --------------------------------------------------------------------------

TEST(script_class_events_fire_from_cpp_by_name)
{
    Harness h;
    auto part = make<Part>("Pad", h.game->workspace());
    auto other = make<Part>("Foot", h.game->workspace());
    h.run(R"(
        workspace.Pad.Touched:Connect(function(hit) print("touched by", hit.Name) end)
        print(typeof(workspace.Pad.Touched), tostring(workspace.Pad.Touched))
    )");
    CHECK_EQ(h.lines.at(0), "RBXScriptSignal Signal Touched");
    const Value arguments[] = {Value(InstancePtr(other))};
    part->fireEvent("Touched", arguments);
    CHECK_EQ(h.lines.at(1), "touched by Foot");
}

TEST(script_deferred_signal_behavior_runs_handlers_after_the_firing_code)
{
    Harness h;
    h.game->workspace()->setSignalBehavior(Enum::SignalBehavior::Deferred);
    h.run(R"(
        local event = Instance.new("BindableEvent")
        event.Event:Connect(function(x) print("handler", x) end)
        event:Fire(1)
        print("after fire")
    )");
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[0], "after fire");
    CHECK_EQ(h.lines[1], "handler 1");

    // Immediate (and Default) run the handler inside Fire.
    h.lines.clear();
    h.game->workspace()->setSignalBehavior(Enum::SignalBehavior::Default);
    h.run(R"(
        local event = Instance.new("BindableEvent")
        event.Event:Connect(function(x) print("handler", x) end)
        event:Fire(1)
        print("after fire")
    )");
    CHECK_EQ(h.lines[0], "handler 1");
    CHECK_EQ(h.lines[1], "after fire");
}

TEST(script_data_update_waiters_observe_the_completed_character_description)
{
    bool updating = false;
    Harness h;
    h.context.setDataUpdateInProgress([&] { return updating; });
    auto character = std::make_shared<Model>();
    character->setName("Rig");
    character->setParent(h.game->workspace());
    h.run(R"(
        local humanoid = workspace.Rig:WaitForChild("Humanoid")
        local description = humanoid:GetAppliedDescription()
        print(description:GetEmotes().Hello[1], description:GetEquippedEmotes()[1].Name)
    )");
    updating = true;
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setParent(character);
    CHECK(h.lines.empty());
    auto description = std::make_shared<HumanoidDescription>();
    description->addEmote("Hello", 55);
    description->setEquippedEmotes({{1, "Hello"}});
    description->setArchivable(false);
    description->setParent(humanoid);
    CHECK(h.lines.empty());
    updating = false;
    h.context.step(0);
    CHECK_EQ(h.lines, (std::vector<std::string>{"55 Hello"}));
}

TEST(script_data_update_callbacks_wait_until_all_related_values_are_applied)
{
    bool updating = false;
    Harness h;
    h.context.setDataUpdateInProgress([&] { return updating; });
    h.run(R"(workspace.ChildAdded:Connect(function(child) print(child.Name, child.Value) end))");
    updating = true;
    auto value = std::make_shared<IntValue>();
    value->setName("Complete");
    value->setParent(h.game->workspace());
    CHECK(h.lines.empty());
    // An incidental resumption point must not flush the pending handlers
    // while the host is still applying the rest of the update.
    h.run("return");
    CHECK(h.lines.empty());
    value->setValue(42);
    updating = false;
    h.context.step(0);
    CHECK_EQ(h.lines, (std::vector<std::string>{"Complete 42"}));
    h.run(R"(
        local value = Instance.new("IntValue")
        value.Name = "Immediate"
        value.Parent = workspace
        print("after parent")
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"Complete 42", "Immediate 0", "after parent"}));
}

TEST(script_data_update_keeps_streamed_script_startup_after_its_children)
{
    bool updating = false;
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setUserId(42);
    player->setParent(game->getService<Players>());
    auto gui = std::make_shared<PlayerGui>();
    gui->setParent(player);
    ClientSession client(game);
    client.client().setDataUpdateInProgress([&] { return updating; });
    std::vector<std::string> lines;
    client.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(client.start(42));
    updating = true;
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(print("started", script.Config:GetAttribute("Ready")))");
    script->setParent(gui);
    CHECK(client.client().run("return").has_value());
    CHECK(lines.empty());
    auto configuration = std::make_shared<Folder>();
    configuration->setName("Config");
    configuration->setAttribute("Ready", true);
    configuration->setParent(script);
    updating = false;
    client.step(0);
    CHECK_EQ(lines, (std::vector<std::string>{"started true"}));
}

TEST(script_bindables_copy_tables_and_return_values)
{
    Harness h;
    h.run(R"(
        local event = Instance.new("BindableEvent")
        local sent = setmetatable({1, 2, nested = {x = 3}}, {})
        event.Event:Connect(function(t)
            print(rawequal(t, sent), getmetatable(t) == nil, t[2], t.nested.x)
        end)
        event:Fire(sent)
        local f = Instance.new("BindableFunction")
        f.OnInvoke = function(a, b) return a + b, "sum" end
        print(f:Invoke(2, 3))
    )");
    CHECK_EQ(h.lines.at(0), "false true 2 3");
    CHECK_EQ(h.lines.at(1), "5 sum");
    CHECK_EQ(h.error("local f = Instance.new('BindableFunction') f.OnInvoke = print print(f.OnInvoke)"),
             concat("OnInvoke is a callback member of BindableFunction; ",
                    "you can only set the callback value, get is not available"));
}

TEST(script_bindable_invoke_waits_for_a_callback)
{
    Harness h;
    h.run(R"(
        local f = Instance.new("BindableFunction")
        task.spawn(function() print("answered", f:Invoke(2, 3)) end)
        print("waiting")
        f.OnInvoke = function(a, b) return a + b end
    )");
    h.steps(2);
    CHECK_EQ(h.lines.at(0), "waiting");
    CHECK_EQ(h.lines.at(1), "answered 5");
}

TEST(script_signal_members_and_lowercase_aliases)
{
    Harness h;
    h.run(R"(
        local event = Instance.new("BindableEvent")
        local count = 0
        local c = event.Event:connect(function() count += 1 end)
        event:Fire()
        c:disconnect()
        event:Fire()
        print(count, c.Connected)
    )");
    CHECK_EQ(h.lines.at(0), "1 false");
    CHECK_EQ(h.error("Instance.new('BindableEvent').Event:Connect(5)"),
             "Attempt to connect failed: Passed value is not a function");
    CHECK_EQ(h.error("local _ = Instance.new('BindableEvent').Event.Nope"),
             "Nope is not a valid member of RBXScriptSignal");
}

TEST(script_attributes)
{
    Harness h;
    h.run(R"(
        local part = Instance.new("Part")
        part:GetAttributeChangedSignal("Speed"):Connect(function() print("speed now", part:GetAttribute("Speed")) end)
        part.AttributeChanged:Connect(function(name) print("changed", name) end)
        part:SetAttribute("Speed", 10)
        part:SetAttribute("Color", Color3.new(1, 0, 0))
        local all = part:GetAttributes()
        print(all.Speed, all.Color, part:GetAttribute("Missing"))
    )");
    CHECK_EQ(h.lines.at(0), "speed now 10");
    CHECK_EQ(h.lines.at(1), "changed Speed");
    CHECK_EQ(h.lines.at(2), "changed Color");
    CHECK_EQ(h.lines.at(3), "10 1, 0, 0 nil");
}

TEST(script_members_the_engine_lacks_resolve)
{
    Harness h;
    h.run(R"(
        local sound = Instance.new("Sound")
        print(typeof(workspace.PersistentLoaded))
        local light = Instance.new("SurfaceLight")
        print(light.ClassName)
    )");
    CHECK_EQ(h.lines.at(0), "RBXScriptSignal");
    CHECK_EQ(h.error("game:GetService('MarketplaceService'):PromptBundlePurchase()"),
             "MarketplaceService:PromptBundlePurchase is not implemented in Lunarstone");
    CHECK_EQ(h.error("game:GetService('NoSuchService')"), "'NoSuchService' is not a valid Service name");
    CHECK(h.print("print(game:GetService('ScriptContext').ClassName)") == "ScriptContext");
}

// --- Services ----------------------------------------------------------------------

TEST(script_http_service_json_and_guids)
{
    Harness h;
    h.run(R"(
        local Http = game:GetService("HttpService")
        print(Http:JSONEncode({1, 2, "three", true}))
        print(Http:JSONEncode({}))
        print(Http:JSONEncode({name = "x"}))
        local decoded = Http:JSONDecode('{"a": [1, 2, {"b": null}], "c": "\\u00e9\\n", "d": 1.5e2}')
        print(#decoded.a, decoded.a[3].b, decoded.c == "\u{e9}\n", decoded.d)
        local guid = Http:GenerateGUID()
        print(#guid, guid:sub(1, 1), guid:sub(-1), #Http:GenerateGUID(false))
        print(Http:UrlEncode("a b&c"))
    )");
    CHECK_EQ(h.lines.at(0), "[1,2,\"three\",true]");
    CHECK_EQ(h.lines.at(1), "[]");
    CHECK_EQ(h.lines.at(2), "{\"name\":\"x\"}");
    CHECK_EQ(h.lines.at(3), "3 nil true 150");
    CHECK_EQ(h.lines.at(4), "38 { } 36");
    CHECK_EQ(h.lines.at(5), "a%20b%26c");
    CHECK_EQ(h.error("game:GetService('HttpService'):JSONDecode('{')"), "Can't parse JSON");
}

TEST(script_collection_service_tags_and_signals)
{
    Harness h;
    h.run(R"(
        local CollectionService = game:GetService("CollectionService")
        CollectionService:GetInstanceAddedSignal("Lava"):Connect(function(p) print("added", p.Name) end)
        CollectionService:GetInstanceRemovedSignal("Lava"):Connect(function(p) print("removed", p.Name) end)
        local part = Instance.new("Part")
        part.Name = "Floor"
        CollectionService:AddTag(part, "Lava")
        part.Parent = workspace
        print(#CollectionService:GetTagged("Lava"), CollectionService:HasTag(part, "Lava"), part:HasTag("Lava"))
        part:RemoveTag("Lava")
        print(#CollectionService:GetTagged("Lava"))
    )");
    CHECK_EQ(h.lines.at(0), "added Floor");
    CHECK_EQ(h.lines.at(1), "1 true true");
    CHECK_EQ(h.lines.at(2), "removed Floor");
    CHECK_EQ(h.lines.at(3), "0");
}

TEST(script_collection_service_index_follows_the_tree)
{
    Harness h;
    h.run(R"(
        local CS = game:GetService("CollectionService")
        local a = Instance.new("Part")
        a:AddTag("Loot")
        a.Parent = workspace
        print(#CS:GetTagged("Loot")) -- builds the index
        local model = Instance.new("Model")
        local b = Instance.new("Part")
        b:AddTag("Loot")
        b.Parent = model
        print(#CS:GetTagged("Loot")) -- not in the game yet
        model.Parent = workspace
        print(#CS:GetTagged("Loot"))
        model.Parent = game:GetService("ReplicatedStorage") -- moving inside the game
        print(#CS:GetTagged("Loot"))
        local copy = model:Clone()
        copy.Parent = workspace
        print(#CS:GetTagged("Loot"))
        model:Destroy()
        a.Parent = nil
        print(#CS:GetTagged("Loot"), CS:GetTagged("Loot")[1] == copy.Part)
        CS:RemoveTag(copy.Part, "Loot")
        print(#CS:GetTagged("Loot"), #CS:GetTagged("Nothing"))
    )");
    CHECK_EQ(h.lines.at(0), "1");
    CHECK_EQ(h.lines.at(1), "1");
    CHECK_EQ(h.lines.at(2), "2");
    CHECK_EQ(h.lines.at(3), "2");
    CHECK_EQ(h.lines.at(4), "3");
    CHECK_EQ(h.lines.at(5), "1 true");
    CHECK_EQ(h.lines.at(6), "0 0");
}

TEST(script_tween_service_easing_values)
{
    Harness h;
    h.run(R"(
        local T = game:GetService("TweenService")
        local function v(a, s, d) return string.format("%.4f", T:GetValue(a, Enum.EasingStyle[s], Enum.EasingDirection[d])) end
        print(v(0.5, "Linear", "In"), v(0.5, "Quad", "In"), v(0.5, "Quad", "Out"), v(0.25, "Quad", "InOut"))
        print(v(0.5, "Sine", "In"), v(0.5, "Cubic", "Out"), v(0.5, "Back", "In"), v(0.5, "Bounce", "Out"))
        print(v(0, "Elastic", "Out"), v(1, "Elastic", "In"), v(0.5, "Exponential", "In"), v(0.5, "Circular", "Out"))
    )");
    CHECK_EQ(h.lines.at(0), "0.5000 0.2500 0.7500 0.1250");
    CHECK_EQ(h.lines.at(1), "0.2929 0.8750 -0.0877 0.7656");
    CHECK_EQ(h.lines.at(2), "0.0000 1.0000 0.0312 0.8660");
}

TEST(script_tweens_move_properties_and_complete)
{
    Harness h;
    h.run(R"(
        local part = Instance.new("Part", workspace)
        part.Transparency = 0
        local tween = game:GetService("TweenService"):Create(part, TweenInfo.new(1, Enum.EasingStyle.Linear),
            {Transparency = 1, Position = Vector3.new(10, 0, 0)})
        _G.part, _G.tween = part, tween
        tween.Completed:Connect(function(state) print("completed", state) end)
        print(tween.PlaybackState, tween.Instance == part)
        tween:Play()
        print(tween.PlaybackState)
    )");
    CHECK_EQ(h.lines.at(0), "Enum.PlaybackState.Begin true");
    CHECK_EQ(h.lines.at(1), "Enum.PlaybackState.Playing");
    h.steps(30);
    h.run("print(string.format('%.2f %.2f', _G.part.Transparency, _G.part.Position.X))");
    CHECK_EQ(h.lines.back(), "0.50 5.00");
    h.steps(31);
    CHECK_EQ(h.lines.back(), "completed Enum.PlaybackState.Completed");
    h.run("print(_G.part.Transparency, _G.part.Position.X, _G.tween.PlaybackState)");
    CHECK_EQ(h.lines.back(), "1 10 Enum.PlaybackState.Completed");
}

// A tweened CFrame carries the part's assembly, as a script's write does:
// The game's drawers tween an anchored Main the rest of the drawer is welded to.
TEST(script_tweening_a_cframe_carries_the_welded_parts)
{
    Harness h;
    h.run(R"(
        local main = Instance.new("Part")
        main.Anchored = true
        main.CFrame = CFrame.new(0, 10, 0)
        main.Parent = workspace
        local front = Instance.new("Part")
        front.Anchored = false
        front.CFrame = CFrame.new(0, 10, 2)
        front.Parent = workspace
        local weld = Instance.new("WeldConstraint")
        weld.Part0, weld.Part1 = main, front
        weld.Parent = main
        _G.main, _G.front = main, front
    )");
    lunar::physics::World world(*h.game->workspace());
    world.step();
    h.run(R"(
        game:GetService("TweenService"):Create(_G.main, TweenInfo.new(0.5, Enum.EasingStyle.Linear),
            {CFrame = CFrame.new(0, 10, 3)}):Play()
    )");
    h.steps(40);
    h.run("print(string.format('%.2f %.2f', _G.main.Position.Z, _G.front.Position.Z))");
    CHECK_EQ(h.lines.back(), "3.00 5.00");
}

// The same for a rig a script clones in after the world was made, held
// together by a Weld, in a world that simulates nothing of ours (no
// character yet) and is only kept up to date: Midnight Racing's map menu
// tweens its camera rig's anchored Root and the camera follows the Camera
// part welded to it.
TEST(script_tweening_a_cloned_rig_carries_its_welded_camera)
{
    Harness h;
    lunar::physics::World world(*h.game->workspace());
    world.setSimulationFilter([](const BasePart&) { return false; });
    h.run(R"(
        local rig = Instance.new("Model")
        local root = Instance.new("Part")
        root.Name, root.Anchored, root.CFrame = "Root", true, CFrame.new(0, 200, 0)
        root.Parent = rig
        local camera = Instance.new("Part")
        camera.Name, camera.Anchored, camera.CFrame = "Camera", false, CFrame.new(0, 200, 0)
        camera.Parent = rig
        local weld = Instance.new("Weld")
        weld.Part0, weld.Part1 = root, camera
        weld.Parent = root
        local setup = rig:Clone()
        setup.Parent = workspace
        _G.setup = setup
    )");
    world.refresh();
    h.run(R"(
        game:GetService("TweenService"):Create(_G.setup.Root, TweenInfo.new(0.5),
            {CFrame = CFrame.new(10, 3, 30) * CFrame.Angles(0, 1, 0)}):Play()
    )");
    for (int i = 0; i < 40; ++i) {
        h.steps(1);
        world.refresh();
    }
    h.run(R"(
        local camera = _G.setup.Camera.CFrame
        print(string.format('%.2f %.2f %.2f %.2f', camera.X, camera.Y, camera.Z, camera.LookVector.X))
    )");
    CHECK_EQ(h.lines.back(), "10.00 3.00 30.00 -0.84");
}

// Model:ScaleTo grows everything about the pivot: sizes, places, joint
// offsets (the game scales its effect models every frame).
TEST(script_model_scale_to)
{
    Harness h;
    h.run(R"(
        local m = Instance.new("Model")
        local a = Instance.new("Part")
        a.Size = Vector3.new(2, 2, 2)
        a.CFrame = CFrame.new(0, 0, 0)
        a.Parent = m
        local b = Instance.new("Part")
        b.Size = Vector3.new(1, 1, 1)
        b.CFrame = CFrame.new(3, 0, 0)
        b.Parent = m
        local weld = Instance.new("Weld")
        weld.Part0, weld.Part1 = a, b
        weld.C0 = CFrame.new(3, 0, 0)
        weld.Parent = a
        m.PrimaryPart = a
        m.Parent = workspace
        m:ScaleTo(2)
        print(m:GetScale(), a.Size.X, b.Size.X, b.Position.X, weld.C0.X)
        m:ScaleTo(1)
        print(m:GetScale(), b.Size.X, b.Position.X)
        print(pcall(function() m:ScaleTo(0) end))
    )");
    CHECK_EQ(h.lines.at(0), "2 4 2 6 6");
    CHECK_EQ(h.lines.at(1), "1 1 3");
    CHECK(h.lines.at(2).starts_with("false"));
}

TEST(script_raycast_goes_through_the_hook)
{
    Harness h;
    auto wall = make<Part>("Wall", h.game->workspace());
    RaycastQuery seen;
    h.context.setRaycastHook([&](const DataModel&, const RaycastQuery& query) -> std::optional<RaycastHit> {
        seen = query;
        if (query.direction.z > -5.0f)
            return std::nullopt;
        return RaycastHit{wall, {0, 0, -5}, {0, 0, 1}, Enum::Material::Brick, 5.0f};
    });
    h.run(R"(
        local params = RaycastParams.new()
        params.FilterType = Enum.RaycastFilterType.Include
        params.FilterDescendantsInstances = {workspace.Wall}
        local result = workspace:Raycast(Vector3.zero, Vector3.new(0, 0, -10), params)
        print(result.Instance.Name, result.Position, result.Normal, result.Material, result.Distance)
        print(workspace:Raycast(Vector3.zero, Vector3.new(0, 0, -1)))
    )");
    CHECK_EQ(h.lines.at(0), "Wall 0, 0, -5 0, 0, 1 Enum.Material.Brick 5");
    CHECK_EQ(h.lines.at(1), "nil");
    CHECK(!seen.filterIsInclude);
}

TEST(script_move_to_rises_out_of_obstructions)
{
    // Model:MoveTo onto ground the model would sink into ends on top of it,
    // as spawning does: here the ground is everything below y = 10.
    Harness h;
    auto ground = make<Part>("Ground", h.game->workspace());
    h.context.setRaycastHook([&](const DataModel&, const RaycastQuery& query) -> std::optional<RaycastHit> {
        const float end = query.origin.y + query.direction.y;
        if (!query.respectCanCollide || query.origin.y < 10.0f || end > 10.0f)
            return std::nullopt;
        const Vector3 at{query.origin.x, 10.0f, query.origin.z};
        return RaycastHit{ground, at, {0, 1, 0}, Enum::Material::Grass, query.origin.y - 10.0f};
    });
    h.run(R"(
        local model = Instance.new("Model")
        local part = Instance.new("Part")
        part.Size = Vector3.new(2, 4, 2)
        part.Parent = model
        model.Parent = workspace
        model:MoveTo(Vector3.new(5, 9, 0))
        print(part.Position)
        model:MoveTo(Vector3.new(5, 30, 0))
        print(part.Position)
    )");
    CHECK_EQ(h.lines.at(0), "5, 12, 0");
    CHECK_EQ(h.lines.at(1), "5, 30, 0");
}

TEST(script_particle_emitter_emit)
{
    // GUESS: nothing is drawn, but the Weapons Kit's muzzle flashes don't error.
    Harness h;
    h.run(R"(
        local emitter = Instance.new("ParticleEmitter")
        emitter:Emit(20)
        emitter:Emit()
        emitter:Clear()
        print(pcall(emitter.Emit, emitter, "lots"))
    )");
    CHECK_EQ(h.lines.size(), std::size_t{1});
    CHECK(h.lines.at(0).starts_with("false"));
}

// --- Play Solo ---------------------------------------------------------------------

namespace {

struct Session {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::unique_ptr<PlaySession> session;
    std::vector<std::string> lines;

    void open()
    {
        session = std::make_unique<PlaySession>(game);
        session->setOutput([this](ScriptRole role, MessageType type, std::string_view text) {
            if (type == MessageType::Info)
                return;
            lines.push_back(std::format("{}{}: {}", type == MessageType::Error ? "error " : "",
                                        role == ScriptRole::Server ? "server" : "client", text));
        });
    }

    std::shared_ptr<Script> script(const std::string& source, Instance* parent, bool local = false)
    {
        std::shared_ptr<Script> s = local ? std::make_shared<LocalScript>() : std::make_shared<Script>();
        s->setSource(source);
        s->setParent(parent);
        return s;
    }

    void steps(int count) const
    {
        for (int i = 0; i < count; ++i)
            session->step(1.0 / 60.0);
    }

    bool has(std::string_view line) const { return std::ranges::find(lines, line) != lines.end(); }
};

} // namespace

TEST(script_play_solo_runs_both_sides)
{
    Session s;
    auto sss = s.game->getService("ServerScriptService");
    auto starterPlayer = s.game->getService("StarterPlayer");
    auto playerScripts = std::make_shared<StarterPlayerScripts>();
    playerScripts->setName("StarterPlayerScripts");
    playerScripts->setParent(starterPlayer);
    s.script(R"(
        local RunService = game:GetService("RunService")
        print(RunService:IsServer(), RunService:IsClient(), game:GetService("Players").LocalPlayer)
        game:GetService("Players").PlayerAdded:Connect(function(p)
            print("joined", p.Name)
            p.CharacterAdded:Connect(function(c) print("character", c.Name, c.Parent.Name) end)
        end)
    )",
             sss);
    s.script(R"(
        local RunService = game:GetService("RunService")
        print(RunService:IsServer(), RunService:IsClient(), game:GetService("Players").LocalPlayer.Name)
        print(script.Parent.ClassName)
    )",
             playerScripts.get(), true);
    // Never runs: LocalScripts don't run in ServerScriptService, Scripts not in ReplicatedStorage.
    s.script("print('wrong place')", sss, true);
    s.script("print('wrong place')", s.game->getService("ReplicatedStorage"));
    s.open();
    s.session->start();
    CHECK(s.has("server: true false nil"));
    CHECK(s.has("server: joined Player1"));
    CHECK(s.has("client: false true Player1"));
    CHECK(s.has("client: PlayerScripts"));
    CHECK(s.has("server: character Player1 Workspace"));
    CHECK(!s.has("server: wrong place"));
    CHECK(!s.has("client: wrong place"));
}

TEST(script_play_solo_remotes_loop_back)
{
    Session s;
    auto storage = s.game->getService("ReplicatedStorage");
    auto remote = make<RemoteEvent>("Ping", storage);
    auto function = make<RemoteFunction>("Add", storage);
    auto sss = s.game->getService("ServerScriptService");
    s.script(R"(
        local storage = game:GetService("ReplicatedStorage")
        storage.Ping.OnServerEvent:Connect(function(player, x)
            print("server got", player.Name, x)
            storage.Ping:FireClient(player, x * 2)
        end)
        storage.Add.OnServerInvoke = function(player, a, b) return a + b end
    )",
             sss);
    auto starterGui = s.game->getService("StarterGui");
    auto gui = make<ScreenGui>("Gui", starterGui);
    s.script(R"(
        local storage = game:GetService("ReplicatedStorage")
        storage.Ping.OnClientEvent:Connect(function(x) print("client got", x) end)
        storage.Ping:FireServer(21)
        print("sum", storage.Add:InvokeServer(2, 3))
    )",
             gui.get(), true);
    s.open();
    s.session->start();
    s.steps(4);
    CHECK(s.has("server: server got Player1 21"));
    CHECK(s.has("client: client got 42"));
    CHECK(s.has("client: sum 5"));
}

TEST(script_play_solo_profile_splits_frames_by_script)
{
    Session s;
    auto sss = s.game->getService("ServerScriptService");
    auto busy = s.script(R"(
        game:GetService("RunService").Heartbeat:Connect(function()
            local x = 0
            for i = 1, 20000 do x += i end
        end)
        while true do task.wait() end
    )",
                         sss);
    busy->setName("Busy");
    s.open();
    s.session->setProfiling(true);
    s.session->start();
    const SimulationProfile startup = s.session->takeProfile();
    CHECK(std::ranges::any_of(startup.activity, [](const ScriptActivity& a) {
        return a.script == "ServerScriptService.Busy" && a.what == "start";
    }));
    s.steps(10);
    const SimulationProfile profile = s.session->takeProfile();
    CHECK_EQ(profile.frames, 10);
    CHECK(profile.phases[SimulationProfile::Heartbeat] >= profile.phaseLuau[SimulationProfile::Heartbeat]);
    CHECK(profile.phaseLuau[SimulationProfile::Heartbeat] > 0.0);
    CHECK(profile.luau[static_cast<std::size_t>(ScriptRole::Server)] > 0.0);
    // The Heartbeat handler, ten times; the loop's task.wait resumptions too.
    auto find = [&](std::string_view what) {
        return std::ranges::find_if(profile.activity, [&](const ScriptActivity& a) {
            return a.script == "ServerScriptService.Busy" && a.what == what;
        });
    };
    CHECK(find("RunService.Heartbeat") != profile.activity.end());
    CHECK_EQ(find("RunService.Heartbeat")->resumes, 10u);
    CHECK(find("task.wait") != profile.activity.end());
    CHECK_EQ(profile.activity.front().what, "RunService.Heartbeat");
    // Taking starts over.
    CHECK_EQ(s.session->takeProfile().frames, 0);
}

TEST(script_play_solo_inserts_the_default_player_scripts)
{
    Session s;
    auto starterPlayer = s.game->getService("StarterPlayer");
    auto starterScripts = std::make_shared<StarterPlayerScripts>();
    starterScripts->setName("StarterPlayerScripts");
    starterScripts->setParent(starterPlayer);
    s.script(R"(
        local player = game:GetService("Players").LocalPlayer
        local scripts = player:WaitForChild("PlayerScripts")
        local module = scripts:WaitForChild("PlayerModule")
        print(module.ClassName, scripts:FindFirstChild("PlayerScriptsLoader").ClassName,
              scripts:FindFirstChild("RbxCharacterSounds").ClassName)
        local controls = require(module):GetControls()
        print(rawequal(controls, require(module:WaitForChild("ControlModule"))), require(module):GetCameras() ~= nil)
        controls:Disable()
        print("move", controls:GetMoveVector())
    )",
             starterScripts.get(), true);
    s.script(R"(
        local market = game:GetService("MarketplaceService")
        print(market:PlayerOwnsAsset(game:GetService("Players"):GetPlayers()[1], 1), market:UserOwnsGamePassAsync(1, 2))
    )",
             s.game->getService("ServerScriptService"));
    s.open();
    s.session->start();
    s.steps(2);
    CHECK(s.has("client: ModuleScript LocalScript LocalScript"));
    CHECK(s.has("client: true true"));
    CHECK(s.has("client: move 0, 0, 0"));
    CHECK(!s.session->controlsEnabled());
    CHECK(s.has("server: false false"));
    // In the starter container too, as the client puts them there.
    CHECK(starterScripts->findFirstChild("PlayerModule") != nullptr);
    CHECK(std::ranges::none_of(s.lines, [](const std::string& line) { return line.starts_with("error"); }));
    // The CameraModule reads the camera every frame as itself, where an
    // __index hook sees it [sUNC's getcallingscript].
    CHECK(s.session->client().run(R"lua(
        local old
        old = hookmetamethod(game, "__index", function(self, key)
            if not checkcaller() and tostring(getcallingscript()) == "CameraModule" then
                getgenv().cameraRead = key
            end
            return old(self, key)
        end)
    )lua", "Console", nullptr, lunar::RunAccess::Full).has_value());
    s.steps(2);
    CHECK(s.session->client().run("print('camera', getgenv().cameraRead)", "Console", nullptr, lunar::RunAccess::Full)
              .has_value());
    s.steps(1);
    CHECK(s.has("client: camera CFrame") || s.has("client: camera CurrentCamera"));
}

TEST(script_play_solo_keeps_a_place_s_own_player_module)
{
    Session s;
    auto starterPlayer = s.game->getService("StarterPlayer");
    auto starterScripts = std::make_shared<StarterPlayerScripts>();
    starterScripts->setName("StarterPlayerScripts");
    starterScripts->setParent(starterPlayer);
    makeModule("PlayerModule", "return {own = true}", starterScripts.get());
    s.script(R"(
        local module = game:GetService("Players").LocalPlayer.PlayerScripts:WaitForChild("PlayerModule")
        print(require(module).own, module.Parent:FindFirstChild("PlayerScriptsLoader"))
    )",
             starterScripts.get(), true);
    s.open();
    s.session->start();
    s.steps(2);
    CHECK(s.has("client: true nil"));

    // CreateDefaultPlayerModule off: no PlayerModule, the character sounds still.
    Session t;
    auto player = static_cast<StarterPlayer*>(t.game->getService("StarterPlayer"));
    player->setCreateDefaultPlayerModule(false);
    t.open();
    t.session->start();
    Instance* scripts = player->findFirstChildOfClass("StarterPlayerScripts");
    CHECK(scripts != nullptr);
    CHECK(scripts->findFirstChild("PlayerModule") == nullptr);
    CHECK(scripts->findFirstChild("RbxCharacterSounds") != nullptr);
}

TEST(script_play_solo_input_reaches_user_input_service_and_actions)
{
    Session s;
    auto starterPlayer = s.game->getService("StarterPlayer");
    auto playerScripts = std::make_shared<StarterPlayerScripts>();
    playerScripts->setName("StarterPlayerScripts");
    playerScripts->setParent(starterPlayer);
    s.script(R"(
        local UIS = game:GetService("UserInputService")
        local CAS = game:GetService("ContextActionService")
        UIS.InputBegan:Connect(function(input, processed)
            print("began", input.KeyCode, input.UserInputType, UIS:IsKeyDown(Enum.KeyCode.W))
        end)
        UIS.InputEnded:Connect(function(input) print("ended", input.KeyCode) end)
        CAS:BindAction("Jump", function(name, state, input)
            print("action", name, state)
            return Enum.ContextActionResult.Pass
        end, false, Enum.KeyCode.Space)
    )",
             playerScripts.get(), true);
    s.open();
    s.session->start();
    s.session->keyDown(Enum::KeyCode::W);
    s.session->keyDown(Enum::KeyCode::Space);
    s.steps(1);
    s.session->keyUp(Enum::KeyCode::W);
    s.steps(1);
    CHECK(s.has("client: began Enum.KeyCode.W Enum.UserInputType.Keyboard true"));
    CHECK(s.has("client: action Jump Enum.UserInputState.Begin"));
    CHECK(s.has("client: ended Enum.KeyCode.W"));
}

// A client of a real server has a camera of its own (the server's isn't
// replicated), as big as the screen it pretends to have.
TEST(script_client_session_has_its_own_camera)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setName("Player1");
    player->setUserId(5);
    player->setParent(game->getService("Players"));
    ClientSession client(game);
    CHECK(game->workspace()->currentCamera() == nullptr);
    CHECK(client.start(5));
    Camera* camera = game->workspace()->currentCamera();
    CHECK(camera != nullptr);
    CHECK(camera->viewportSize() == (Vector2{1280.0f, 720.0f}));
    client.setViewportSize({800.0f, 600.0f});
    client.step(1.0 / 60.0);
    CHECK(camera->viewportSize() == (Vector2{800.0f, 600.0f}));
}

// A server can still list a Player of ours from an earlier session, with
// the same UserId: the owner says which one is the LocalPlayer.
TEST(script_client_session_starts_as_the_chosen_player)
{
    auto game = DataModel::create();
    std::vector<std::shared_ptr<Player>> players;
    for (int i = 0; i < 2; ++i) {
        auto player = std::make_shared<Player>();
        player->setName("Player1");
        player->setUserId(5);
        player->setParent(game->getService("Players"));
        players.push_back(player);
    }
    ClientSession client(game);
    CHECK(client.start(*players[0]));
    CHECK(client.localPlayer() == players[0].get());
    CHECK(players[0]->findFirstChildOfClass("PlayerScripts") != nullptr);
    CHECK(players[1]->findFirstChildOfClass("PlayerScripts") == nullptr);
}

// A windowed client of a real server (--net-play) hands the user's keys and
// mouse to its LocalScripts the way Play Solo does: UserInputService,
// ContextActionService and the legacy Mouse at the start of the next step.
TEST(script_client_session_takes_input)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setName("Player1");
    player->setUserId(5);
    player->setParent(game->getService("Players"));
    auto starterScripts = std::make_shared<StarterPlayerScripts>();
    starterScripts->setName("StarterPlayerScripts");
    starterScripts->setParent(game->getService("StarterPlayer"));
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local UIS = game:GetService("UserInputService")
        local CAS = game:GetService("ContextActionService")
        UIS.InputBegan:Connect(function(input)
            print("began", input.KeyCode, input.UserInputType, UIS:IsKeyDown(Enum.KeyCode.W))
        end)
        UIS.InputChanged:Connect(function(input)
            if input.UserInputType == Enum.UserInputType.MouseWheel then
                print("wheel", input.Position.Z)
            end
        end)
        UIS.InputEnded:Connect(function(input) print("ended", input.KeyCode) end)
        CAS:BindAction("Jump", function(name, state) print("action", name, state) end, false, Enum.KeyCode.Space)
    )");
    script->setParent(starterScripts);
    ClientSession client(game);
    std::vector<std::string> lines;
    client.client().setOutput([&lines](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(client.start(5));
    client.step(1.0 / 60.0);
    client.input().keyDown(Enum::KeyCode::W);
    client.input().keyDown(Enum::KeyCode::Space);
    client.input().mouseButton(Enum::UserInputType::MouseButton2, true);
    client.step(1.0 / 60.0);
    client.input().keyUp(Enum::KeyCode::W);
    client.input().mouseWheel(-1.0f);
    client.step(1.0 / 60.0);
    auto has = [&](std::string_view line) { return std::ranges::find(lines, line) != lines.end(); };
    CHECK(has("began Enum.KeyCode.W Enum.UserInputType.Keyboard true"));
    CHECK(has("action Jump Enum.UserInputState.Begin"));
    CHECK(has("began Enum.KeyCode.None Enum.UserInputType.MouseButton2 true"));
    CHECK(has("ended Enum.KeyCode.W"));
    CHECK(has("wheel -1"));
}

// The live client draws between the render step and the rest of the frame,
// as the genuine client does: a step ends after the render step, and the
// last frame's physics and Heartbeat open the next. GetMouseDelta is the
// frame's motion, every move in it, and nothing without any.
TEST(script_client_session_draws_before_the_simulation)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setName("Player1");
    player->setUserId(5);
    player->setParent(game->getService("Players"));
    auto starterScripts = std::make_shared<StarterPlayerScripts>();
    starterScripts->setName("StarterPlayerScripts");
    starterScripts->setParent(game->getService("StarterPlayer"));
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local RunService = game:GetService("RunService")
        local UIS = game:GetService("UserInputService")
        local frame = 0
        RunService:BindToRenderStep("probe", 100, function()
            frame += 1
            print("render", frame, UIS:GetMouseDelta().X)
        end)
        RunService.Heartbeat:Connect(function() print("heartbeat", frame) end)
    )");
    script->setParent(starterScripts);
    ClientSession client(game);
    client.setDrawBeforeSimulation(true);
    std::vector<std::string> lines;
    client.client().setOutput([&lines](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    FrameHooks hooks;
    hooks.physics = [&lines](double) { lines.emplace_back("physics"); };
    client.setHooks(hooks);
    CHECK(client.start(5));
    client.step(1.0 / 60.0);
    client.step(1.0 / 60.0); // the script is running
    auto frame = [&](auto&& before) {
        lines.clear();
        before();
        client.step(1.0 / 60.0);
        return lines;
    };
    auto at = [](const std::vector<std::string>& got, std::string_view prefix) {
        return std::ranges::find_if(got, [&](const std::string& l) { return l.starts_with(prefix); }) - got.begin();
    };
    const auto moved = frame([&] {
        client.input().mouseMove({10.0f, 10.0f}, {3.0f, 0.0f});
        client.input().mouseMove({10.0f, 10.0f}, {4.0f, 0.0f});
    });
    // The last frame's physics and Heartbeat, then this frame up to its picture.
    CHECK(!moved.empty() && moved.back().starts_with("render "));
    CHECK(at(moved, "physics") < at(moved, "heartbeat"));
    CHECK(at(moved, "heartbeat") < at(moved, "render"));
    CHECK(moved.back().ends_with(" 7")); // both moves
    const auto still = frame([] {});
    CHECK(!still.empty() && still.back().ends_with(" 0")); // none this frame
}

// Replication sets Player.Character (the server spawns ours after the
// join); the client's scripts hear CharacterAdded and CharacterRemoving.
TEST(script_client_session_fires_character_events)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setName("Player1");
    player->setUserId(5);
    player->setParent(game->getService("Players"));
    auto starterScripts = std::make_shared<StarterPlayerScripts>();
    starterScripts->setName("StarterPlayerScripts");
    starterScripts->setParent(game->getService("StarterPlayer"));
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local player = game:GetService("Players").LocalPlayer
        player.CharacterRemoving:Connect(function(c) print("removing", c.Name) end)
        print("added", player.Character or player.CharacterAdded:Wait())
    )");
    script->setParent(starterScripts);
    ClientSession client(game);
    std::vector<std::string> lines;
    client.client().setOutput([&lines](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(client.start(5));
    client.step(1.0 / 60.0);
    auto character = [&](const char* name) {
        auto model = std::make_shared<Model>();
        model->setName(name);
        model->setParent(game->workspace());
        player->setCharacter(model.get());
        client.step(1.0 / 60.0);
        return model;
    };
    const auto first = character("First");
    const auto second = character("Second");
    CHECK(std::ranges::find(lines, std::string_view("added First")) != lines.end());
    CHECK(std::ranges::find(lines, std::string_view("removing First")) != lines.end());
}

// Hotkeys may be strings: one character is that key; longer ones (mobile
// shift-lock scripts pass "On") bind nothing and aren't an error.
TEST(script_bind_action_takes_string_hotkeys)
{
    Session s;
    auto starterPlayer = s.game->getService("StarterPlayer");
    auto playerScripts = std::make_shared<StarterPlayerScripts>();
    playerScripts->setName("StarterPlayerScripts");
    playerScripts->setParent(starterPlayer);
    s.script(R"(
        local CAS = game:GetService("ContextActionService")
        CAS:BindAction("Wave", function(name, state) print("action", name, state) end, false, "q", "On")
        print("bound")
    )",
             playerScripts.get(), true);
    s.open();
    s.session->start();
    s.session->keyDown(Enum::KeyCode::Q);
    s.steps(1);
    CHECK(s.has("client: bound"));
    CHECK(s.has("client: action Wave Enum.UserInputState.Begin"));
}

TEST(script_errors_and_output_reach_script_context_and_log_service)
{
    Harness h;
    h.run(R"(
        game:GetService("ScriptContext").Error:Connect(function(message) _G.lastError = message end)
        game:GetService("LogService").MessageOut:Connect(function(message, kind)
            if kind == Enum.MessageType.MessageWarning then _G.lastWarning = message end
        end)
        task.spawn(function() error("oops") end)
        warn("careful")
        print(_G.lastError, _G.lastWarning)
    )");
    CHECK_EQ(h.lines.back(), "Test:6: oops careful");
}

TEST(script_camera_projects_points)
{
    Harness h;
    h.run(R"(
        local camera = workspace.CurrentCamera or Instance.new("Camera", workspace)
        camera.CFrame = CFrame.new(0, 0, 10)
        local point, visible = camera:WorldToViewportPoint(Vector3.new(0, 0, 0))
        print(point.X, point.Y, point.Z, visible)
        local ray = camera:ViewportPointToRay(camera.ViewportSize.X / 2, camera.ViewportSize.Y / 2)
        print(ray.Origin, ray.Direction)
        print(select(2, camera:WorldToViewportPoint(Vector3.new(0, 0, 20))))
    )");
    CHECK_EQ(h.lines.at(0), "400 300 10 true");
    CHECK_EQ(h.lines.at(1), "0, 0, 10 0, 0, -1");
    CHECK_EQ(h.lines.at(2), "false");
}

TEST(script_terrain_material_colors)
{
    Harness h;
    std::make_shared<Terrain>()->setParent(h.game->workspace());
    h.run(R"(
        local t = workspace:FindFirstChildOfClass("Terrain")
        t:SetMaterialColor(Enum.Material.Grass, Color3.new(1, 0, 0))
        print(t:GetMaterialColor(Enum.Material.Grass) == Color3.new(1, 0, 0))
        print(t:GetMaterialColor(Enum.Material.Water) == Color3.new(0, 0, 0))
    )");
    CHECK_EQ(h.lines.at(0), "true");
    CHECK_EQ(h.lines.at(1), "true");
}

TEST(script_network_ownership_in_play_solo)
{
    Harness h;
    h.run(R"(
        local part = Instance.new("Part")
        part.Parent = workspace
        part:SetNetworkOwner(nil)
        part:SetNetworkOwnershipAuto()
        print(part:GetNetworkOwner(), part:GetNetworkOwnershipAuto(), (part:CanSetNetworkOwnership()))
    )");
    CHECK_EQ(h.lines.at(0), "nil true true");
}

TEST(script_live_client_stands_its_character)
{
    // A live server's game as the client gets it: our Player and its
    // character are the server's, its Animate-like LocalScript ours. With
    // nothing simulating the character, the client puts its Humanoid into
    // Running at speed 0 once, as a real client's does on landing.
    auto game = DataModel::create();
    auto* players = game->getService<Players>();
    auto player = std::make_shared<Player>();
    player->setName("Me");
    player->setUserId(42);
    player->setParent(players);
    auto character = std::make_shared<Model>();
    character->setName("Me");
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setParent(character.get());
    auto animate = std::make_shared<LocalScript>();
    animate->setName("Animate");
    animate->setSource(R"(
        local humanoid = script.Parent:WaitForChild("Humanoid")
        print("before", humanoid:GetState().Name)
        humanoid.StateChanged:Connect(function(old, new) print("state", old.Name, new.Name) end)
        humanoid.Running:Connect(function(speed) print("running", speed) end)
    )");
    animate->setParent(character.get());
    character->setParent(game->workspace());
    player->setCharacter(character.get());

    ClientSession session(game);
    std::vector<std::string> lines;
    session.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(session.start(42));
    for (int i = 0; i < 10; ++i)
        session.step(1.0 / 60.0);
    const std::vector<std::string> expected{"before Running", "state None Running", "running 0"};
    CHECK(lines == expected);
    CHECK(humanoid->getState() == Enum::HumanoidStateType::Running);
}

// --- Lobby script compatibility ------------------------------------------------------

TEST(script_some_services_have_names_apart_from_their_class)
{
    Harness h;
    CHECK_EQ(h.print(R"(
        local early = game["Run Service"] -- there before anything asks for it
        local rs = game:GetService("RunService")
        print(rs.Name, early == rs, game:FindFirstChild("RunService") == nil,
              game:GetService("TeleportService").Name, game:GetService("Players").Name)
    )"),
             "Run Service true true Teleport Service Players");
}

TEST(script_debris_is_there_without_get_service)
{
    Harness h;
    // The game indexes game.Debris directly; the genuine client's core scripts have made it by then.
    CHECK_EQ(h.print("print(game.Debris == game:GetService('Debris'), game.TweenService.ClassName)"),
             "true TweenService");
}

TEST(script_bulk_move_to_moves_each_part)
{
    Harness h;
    CHECK_EQ(h.print(R"(
        local a, b = Instance.new("Part", workspace), Instance.new("Part", workspace)
        workspace:BulkMoveTo({a, b}, {CFrame.new(1, 2, 3), CFrame.new(4, 5, 6)}, Enum.BulkMoveMode.FireCFrameChanged)
        print(a.Position, b.Position, (pcall(workspace.BulkMoveTo, workspace, {a}, {})))
    )"),
             "1, 2, 3 4, 5, 6 false");
}

TEST(script_color3_has_the_lower_case_lerp)
{
    Harness h;
    CHECK_EQ(h.print("print(Color3.new(0, 0, 0):lerp(Color3.new(1, 1, 1), 0.5).R)"), "0.5");
}

TEST(script_gui_objects_tween_their_size_and_position)
{
    Harness h;
    h.run(R"(
        local f = Instance.new("Frame")
        _G.f = f
        local ok = f:TweenSize(UDim2.new(0, 100, 0, 50), Enum.EasingDirection.Out, Enum.EasingStyle.Linear, 0.1,
                               false, function(status) print("done", status.Name) end)
        print(ok, f:TweenSize(UDim2.new(0, 1, 0, 1), nil, nil, 1, false))
    )");
    h.steps(20);
    h.run(R"(
        print(_G.f.Size)
        print(_G.f:TweenPosition(UDim2.new(0.5, 0, 0, 0)))
    )");
    const std::vector<std::string> expected{"true false", "done Completed", "{0, 100}, {0, 50}", "true"};
    CHECK_EQ(h.lines, expected);
}

TEST(script_query_descendants_takes_selectors)
{
    Harness h;
    const auto lines = h.output(R"lua(
        local root = Instance.new("Folder")
        local a = Instance.new("Frame") a.Name = "A" a.Parent = root a:AddTag("Redacted")
        local b = Instance.new("ImageLabel") b.Name = "B" b.Parent = a b:SetAttribute("Category", "x")
        local c = Instance.new("TextLabel") c.Name = "C" c.Parent = root c.Visible = false
        local function names(selector)
            local out = {}
            for _, v in root:QueryDescendants(selector) do table.insert(out, v.Name) end
            print(table.concat(out, " "))
        end
        names("GuiObject")
        names(".Redacted")
        names("[$Category]")
        names("[$Category = x]")
        names("> GuiObject")
        names("Frame > ImageLabel, #C")
        names("GuiObject:not(.Redacted)")
        names("Frame:has(ImageLabel)")
        names("[Visible = false]")
        names("Folder ImageLabel")
        print((pcall(root.QueryDescendants, root, "[")))
    )lua");
    const std::vector<std::string> expected{"A B C", "A", "B", "B", "A C", "B C", "B C", "A", "C", "", "false"};
    CHECK(lines == expected);
}

TEST(script_installs_every_enum_for_the_decoders)
{
    // The engine models a few enums; the decoders see all of the client's
    // once a script context exists (remote arguments of Enum.PlaybackState).
    Harness h;
    CHECK(lunar::enums::find("PlaybackState") == nullptr);
    const lunar::EnumInfo* state = lunar::enums::findAny("PlaybackState");
    CHECK(state != nullptr);
    const lunar::EnumItem* playing = state ? state->find(2) : nullptr;
    CHECK(playing && playing->name == "Playing");
    CHECK(lunar::enums::findAny("Material") == lunar::enums::find("Material"));
    // EasingStyle is the engine's now (UIPageLayout's), numbered as the client's.
    const lunar::EnumInfo* style = lunar::enums::findAny("EasingStyle");
    CHECK(style == lunar::enums::find("EasingStyle"));
    const lunar::EnumItem* sine = style ? style->find(1) : nullptr;
    CHECK(sine && sine->name == "Sine");
}

TEST(script_boolean_properties_take_any_value_by_its_truth)
{
    // Roblox's ControlModule sets Humanoid.Jump to `false or nil` every frame.
    Harness h;
    CHECK_EQ(h.print(R"(
        local p = Instance.new("Part")
        p.Anchored = true
        p.Anchored = nil
        local first = p.Anchored
        p.Anchored = 1
        print(first, p.Anchored)
    )"),
             "false true");
}

// New instances get the client's default Names, which the server leaves out
// of what it sends: the game's effects find part.Mesh, a SpecialMesh, and
// Midnight Racing's menu Lighting.Blur and Lighting.DepthOfField.
TEST(script_default_instance_names)
{
    Harness h;
    CHECK_EQ(h.print(R"(print(Instance.new("SpecialMesh").Name, Instance.new("BodyColors").Name,
        Instance.new("WedgePart").Name, Instance.new("Part").Name))"),
             "Mesh Body Colors Wedge Part");
    CHECK_EQ(h.print(R"(print(Instance.new("BlurEffect").Name, Instance.new("DepthOfFieldEffect").Name,
        Instance.new("BloomEffect").Name, Instance.new("ColorCorrectionEffect").Name, Instance.new("SunRaysEffect").Name))"),
             "Blur DepthOfField Bloom ColorCorrection SunRays");
    // A class the engine doesn't model gets its default name too.
    const UnknownInstance grading(ClassRegistry::placeholder("ColorGradingEffect"));
    CHECK_EQ(grading.name(), std::string("ColorGrading"));
}

// Midnight Racing's menu asks whether the gamepad rumbles (no), and its
// LocalizationTables for translators (that pass text through).
TEST(script_haptics_and_table_translators)
{
    Harness h;
    CHECK_EQ(h.print(R"(
        local haptics = game:GetService("HapticService")
        haptics:SetMotor(Enum.UserInputType.Gamepad1, Enum.VibrationMotor.Large, 1)
        local translator = Instance.new("LocalizationTable"):GetTranslator("en-us")
        print(haptics:IsVibrationSupported(Enum.UserInputType.Gamepad1),
              haptics:IsMotorSupported(Enum.UserInputType.Gamepad1, Enum.VibrationMotor.Large),
              haptics:GetMotor(Enum.UserInputType.Gamepad1, Enum.VibrationMotor.Large), translator.ClassName,
              translator:Translate(game, "Join Server"))
    )"),
             "false false 0 Translator Join Server");
}

// UserGameSettings.RotationType is a real setting the engine reads: the game's
// own Shift Lock sets it to CameraRelative and the character faces the camera.
TEST(script_user_game_settings_rotation_type)
{
    Harness h;
    CHECK(!h.context.cameraRelativeRotation());
    h.run(R"(
        local settings = UserSettings():GetService("UserGameSettings")
        print(settings.RotationType, settings.ClassName, settings == UserSettings().GameSettings)
        settings.RotationType = Enum.RotationType.CameraRelative
    )");
    CHECK_EQ(h.lines.at(0), "Enum.RotationType.MovementRelative UserGameSettings true");
    CHECK(h.context.cameraRelativeRotation());
}

// EncodingService on buffers: Zstd both ways (JJS' effects data), Base64.
TEST(script_encoding_service_buffers)
{
    Harness h;
    h.run(R"(
        local encoding = game:GetService("EncodingService")
        local zstd = Enum.CompressionAlgorithm.Zstd
        local packed = encoding:CompressBuffer(buffer.fromstring(string.rep("jjs ", 100)), zstd)
        print(typeof(packed), buffer.len(packed) < 400, encoding:GetDecompressedBufferSize(packed, zstd))
        print(buffer.tostring(encoding:DecompressBuffer(packed, zstd)) == string.rep("jjs ", 100))
        print(buffer.tostring(encoding:Base64Encode(buffer.fromstring("hi!?"))))
        print(buffer.tostring(encoding:Base64Decode(buffer.fromstring("aGkhPw=="))))
        print(pcall(function() return encoding:DecompressBuffer("not a buffer", zstd) end))
    )");
    CHECK_EQ(h.lines.at(0), "buffer true 400");
    CHECK_EQ(h.lines.at(1), "true");
    CHECK_EQ(h.lines.at(2), "aGkhPw==");
    CHECK_EQ(h.lines.at(3), "hi!?");
    CHECK(h.lines.at(4).starts_with("false"));
}

// A buffer stays a buffer through the engine's values (a remote's arguments).
TEST(script_buffer_values_stay_buffers)
{
    Harness h;
    h.run(R"(
        local event = Instance.new("BindableEvent")
        event.Event:Connect(function(b) print(typeof(b), buffer.len(b), buffer.readu8(b, 1)) end)
        local b = buffer.create(3)
        buffer.writeu8(b, 1, 42)
        event:Fire(b)
    )");
    CHECK_EQ(h.lines.at(0), "buffer 3 42");
}

// A property only the API knows tweens too, kept beside the instance as a
// script's write is: JJS tweens AudioEqualizer.MidGain.
TEST(script_tweens_move_properties_only_the_api_knows)
{
    Harness h;
    h.run(R"(
        local eq = Instance.new("AudioEqualizer", workspace)
        eq.MidGain = -10
        eq:GetPropertyChangedSignal("MidGain"):Connect(function() _G.changes = (_G.changes or 0) + 1 end)
        _G.eq = eq
        game:GetService("TweenService"):Create(eq, TweenInfo.new(1, Enum.EasingStyle.Linear), {MidGain = 10, HighGain = 4}):Play()
    )");
    h.steps(30);
    h.run("print(string.format('%.1f %.1f %s', _G.eq.MidGain, _G.eq.HighGain, tostring(_G.changes > 0)))");
    CHECK_EQ(h.lines.back(), "0.0 2.0 true");
    h.steps(31);
    h.run("print(_G.eq.MidGain, _G.eq.HighGain)");
    CHECK_EQ(h.lines.back(), "10 4");
    h.run(
        R"(print(pcall(function() game:GetService("TweenService"):Create(_G.eq, TweenInfo.new(1), {Nope = 1}) end)))");
    CHECK(h.lines.back().starts_with("false"));
}

// Motors turn towards their DesiredAngle, MaxVelocity radians a 60th of a
// second, set by Motor:SetDesiredAngle or by the property: the R6 Animate
// script swings the limbs that way when the character falls down or gets up
// (and stopped altogether when the method raised).
// A GUI object outside any GUI still has its offsets for a size, and its
// children lay out against it (what Studio prints for these): the game sizes its
// hotbar slots from a template kept under a LocalScript.
TEST(script_gui_outside_a_gui_has_its_offsets)
{
    Harness h;
    h.run(R"(
        local holder = Instance.new("LocalScript")
        local button = Instance.new("TextButton")
        button.Size = UDim2.new(0, 60, 0, 60)
        button.Parent = holder
        local child = Instance.new("Frame")
        child.Size = UDim2.new(0.5, 10, 1, 5)
        child.Parent = button
        local lone = Instance.new("Frame")
        lone.Size = UDim2.new(0.5, 30, 0, 40)
        print(tostring(button.AbsoluteSize), tostring(child.AbsoluteSize), tostring(lone.AbsoluteSize))
    )");
    CHECK_EQ(h.lines.back(), "60, 60 40, 65 30, 40");
}

// TopbarPlus reads GuiService.TopbarInset.Height, loading screens count
// ContentProvider.RequestQueueSize down: both are numbers, not nil.
TEST(script_topbar_inset_and_request_queue)
{
    Harness h;
    h.run(R"(
        print(game:GetService("GuiService").TopbarInset.Height, game:GetService("ContentProvider").RequestQueueSize)
    )");
    CHECK_EQ(h.lines.back(), "36 0");
}

// UIPageLayout:JumpToIndex, Next and Previous move the current page:
// PageLeave and PageEnter at once, Stopped once the slide is over; Next
// stops at the last page unless Circular. Midnight Racing's intro credits
// jump between pages this way, and its loading screen never finished
// while JumpToIndex raised.
TEST(script_page_layout_moves_between_pages)
{
    Harness h;
    h.run(R"(
        local holder = Instance.new("Frame")
        local layout = Instance.new("UIPageLayout")
        layout.TweenTime = 0.2
        layout.Parent = holder
        for i = 1, 3 do
            local page = Instance.new("Frame")
            page.Name = "Page" .. i
            page.LayoutOrder = i
            page.Parent = holder
        end
        _G.layout, _G.holder = layout, holder -- the pages live in the holder
        layout.PageLeave:Connect(function(page) print("leave", page.Name) end)
        layout.PageEnter:Connect(function(page) print("enter", page.Name) end)
        layout.Stopped:Connect(function(page) print("stopped", page.Name) end)
        print("current", layout.CurrentPage.Name)
        layout:JumpToIndex(2)
        print("current", layout.CurrentPage.Name)
    )");
    auto heard = [&](std::string_view line) { return std::ranges::find(h.lines, line) != h.lines.end(); };
    h.steps(1);
    CHECK(heard("current Page1"));
    CHECK(heard("current Page3"));
    CHECK(heard("leave Page1"));
    CHECK(heard("enter Page3"));
    CHECK(!heard("stopped Page3"));
    h.steps(18);
    CHECK(heard("stopped Page3"));
    h.run(R"(
        _G.layout:Next()
        print("past the end", _G.layout.CurrentPage.Name)
        _G.layout.Circular = true
        _G.layout:Next()
        print("round", _G.layout.CurrentPage.Name)
        _G.layout:Previous()
        print("back", _G.layout.CurrentPage.Name)
    )");
    CHECK(heard("past the end Page3"));
    CHECK(heard("round Page1"));
    CHECK(heard("back Page3"));
}

// What The Mimic's scripts asked for and died on: RaycastFilterType's old
// item names (its monster targets with Blacklist), Players' thumbnails,
// MarketplaceService's product info, the arriving teleport GUI and
// Camera.CoordinateFrame.
TEST(script_mimic_apis)
{
    Harness h;
    h.run(R"(
        print(Enum.RaycastFilterType.Blacklist == Enum.RaycastFilterType.Exclude,
              Enum.RaycastFilterType.Whitelist == Enum.RaycastFilterType.Include)
        local content, ready = game:GetService("Players"):GetUserThumbnailAsync(42,
            Enum.ThumbnailType.HeadShot, Enum.ThumbnailSize.Size150x150)
        print(content, ready)
        local info = game:GetService("MarketplaceService"):GetProductInfo(123)
        print(info.Name == "", info.IsForSale, info.AssetId, info.Creator.CreatorType)
        print(game:GetService("TeleportService"):GetArrivingTeleportGui())
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(1, 2, 3)
        print(camera.CoordinateFrame == camera.CFrame)
    )");
    const std::vector<std::string> expected{"true true", "rbxthumb://type=AvatarHeadShot&id=42&w=150&h=150 true",
                                            "true false 123 User", "nil", "true"};
    CHECK(h.lines == expected);
}

// ReplicatedFirst:RemoveDefaultLoadingScreen() returns at once, so a
// loader that calls it first goes on to take its own screen down, and the
// app hears it (its loading screen comes down).
TEST(script_remove_default_loading_screen_returns)
{
    Harness h;
    CHECK(!h.context.defaultLoadingScreenRemoved());
    h.run(R"(
        game:GetService("ReplicatedFirst"):RemoveDefaultLoadingScreen()
        print("past it")
    )");
    const std::vector<std::string> expected{"past it"};
    CHECK(h.lines == expected);
    CHECK(h.context.defaultLoadingScreenRemoved());
}

// TweenService:Create(...):Play() keeps no reference to the Tween: the
// engine keeps it playing all the same, through any garbage collection (the game
// grows its hotbar slots that way, which stayed -7 pixels wide).
TEST(script_a_tween_nobody_holds_plays_to_the_end)
{
    Harness h;
    h.run(R"(
        local frame = Instance.new("Frame")
        frame.Size = UDim2.new(0, -7, 0, 60)
        _G.frame = frame
        game:GetService("TweenService"):Create(frame, TweenInfo.new(0.5), {Size = UDim2.new(0, 60, 0, 60)}):Play()
    )");
    for (int i = 0; i < 40; ++i) {
        h.run("local junk = {} for i = 1, 20000 do junk[i] = {i} end");
        h.steps(1);
    }
    h.run("print(tostring(_G.frame.Size))");
    CHECK_EQ(h.lines.back(), "{0, 60}, {0, 60}");
}

// Vector3.fromAxis takes what any enum argument takes, and a face's name
// for its axis (what Studio prints for these; the game's effects pass names).
TEST(script_vector_from_axis_casts_like_studio)
{
    Harness h;
    h.run(R"(
        local out = {}
        for _, a in {"Y", 2, Enum.Axis.Z, "Top", "Left", "Back", 1.7} do
            table.insert(out, tostring(Vector3.fromAxis(a)))
        end
        for _, a in {"x", 3, Enum.NormalId.Top} do
            table.insert(out, tostring((pcall(Vector3.fromAxis, a))))
        end
        table.insert(out, tostring(Vector3.FromNormalId("Top")) .. "/" .. tostring(Vector3.FromNormalId(4)))
        print(table.concat(out, " "))
    )");
    CHECK_EQ(h.lines.back(),
             "0, 1, 0 0, 0, 1 0, 0, 1 0, 1, 0 1, 0, 0 0, 0, 1 0, 1, 0 false false false 0, 1, 0/0, -1, 0");
}

TEST(script_motors_turn_towards_their_desired_angle)
{
    Harness h;
    h.run(R"(
        local motor = Instance.new("Motor6D")
        motor.MaxVelocity = 0.1
        motor:SetDesiredAngle(0.35)
        _G.motor = motor
        local other = Instance.new("Motor")
        other.MaxVelocity = 0.05
        other.DesiredAngle = -0.1
        _G.other = other
    )");
    h.steps(2);
    h.run("print(string.format('%.2f %.2f', _G.motor.CurrentAngle, _G.other.CurrentAngle))");
    CHECK_EQ(h.lines.back(), "0.20 -0.10");
    h.steps(5);
    h.run("print(string.format('%.2f', _G.motor.CurrentAngle))");
    CHECK_EQ(h.lines.back(), "0.35");
}
