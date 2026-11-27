#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/RobloxVersion.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;

namespace {

std::shared_ptr<Part> makePart(const std::string& name, Instance* parent)
{
    auto part = std::make_shared<Part>();
    part->setName(name);
    part->setParent(parent);
    return part;
}

} // namespace

// --- Basics ------------------------------------------------------------------

TEST(script_print_goes_to_the_output_sink)
{
    Harness h;
    h.run("print('hello', 1, true, nil) warn('careful')");
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[0], "hello 1 true nil");
    CHECK_EQ(h.lines[1], "warn: careful");
}

TEST(script_errors_carry_the_chunk_name_and_line)
{
    Harness h;
    CHECK_EQ(h.error("local x = 1\nerror('boom')"), "Test:2: boom");
    CHECK_EQ(h.error("local x = nil\nprint(x.y)"), "Test:2: attempt to index nil with 'y'");
}

TEST(script_syntax_errors_come_back_from_run)
{
    Harness h;
    std::string message = h.error("local = 5");
    CHECK(message.starts_with("Test:1: "));
    auto compiled = ScriptContext::compile("return 1", "Chunk");
    CHECK(compiled.has_value());
}

TEST(script_globals_point_at_the_game)
{
    Harness h;
    h.run(R"(
        assert(game.ClassName == "DataModel")
        assert(workspace == game.Workspace)
        assert(workspace == game:GetService("Workspace"))
        assert(game:FindService("Workspace") == workspace)
        assert(script == nil)
        assert(typeof(game) == "Instance")
    )");
}

TEST(script_version_matches_the_network_client)
{
    Harness h;
    h.console(R"lua(
        print(version(), game:GetService("RunService"):GetRobloxVersion(),
              game:GetService("HttpService"):GetUserAgent())
    )lua");
    CHECK_EQ(h.lines.size(), 1u);
    CHECK_EQ(h.lines[0],
             std::format("{} {} {}", lunar::robloxVersion, lunar::robloxVersion, lunar::robloxMacUserAgent));
}

TEST(script_current_api_members_and_enums_resolve)
{
    Harness h;
    h.run(R"lua(
        local binding = Instance.new("InputBinding")
        assert(typeof(binding.Deadzone) == "NumberRange")
        binding.Deadzone = NumberRange.new(0.1, 0.25)
        assert(binding.Deadzone == NumberRange.new(0.1, 0.25))
        assert(type(game:GetService("CollectionService").GetCollections) == "function")
        assert(type(game:GetService("RunService").BindToSimulationParallel) == "function")
        assert(typeof(Enum.AudioSortType:GetEnumItems()[1]) == "EnumItem")
        assert(typeof(Enum.PlayerDeviceTrustLevel:GetEnumItems()[1]) == "EnumItem")
        assert(Instance.new("PhysicsActivitySensor"):IsA("Instance"))
        assert(game:GetService("InputReplayService").ClassName == "InputReplayService")
        assert(not pcall(function() return Enum.ProjectServiceOperationResult end))
    )lua");
}

TEST(script_platform_is_what_the_client_says)
{
    // GetPlatform is RobloxScriptSecurity: game scripts can't ask.
    Harness h;
    CHECK(h.context.setPlatform("WiiU"));
    CHECK(!h.context.setPlatform("GameBoy"));
    CHECK_EQ(h.error(R"(game:GetService("UserInputService"):GetPlatform())"),
             "The current thread cannot call 'GetPlatform' (lacking capability RobloxScript)");
}

TEST(script_members_need_the_clients_capabilities)
{
    // The API dump's security: what game scripts can't read, write or call.
    Harness h;
    h.run(R"lua(
        local function failure(f)
            local ok, e = pcall(f)
            assert(not ok)
            return e
        end
        local value = Instance.new("StringValue")
        assert(failure(function() value.RobloxLocked = true end) ==
            "The current thread cannot write 'RobloxLocked' (lacking capability Plugin)")
        assert(failure(function() return value.RobloxLocked end) ==
            "The current thread cannot read 'RobloxLocked' (lacking capability Plugin)")
        assert(failure(function() return workspace:GetDebugId() end) ==
            "The current thread cannot call 'GetDebugId' (lacking capability Plugin)")
        assert(failure(function() return workspace.GetDebugId(workspace) end) ==
            "The current thread cannot call 'GetDebugId' (lacking capability Plugin)")
        -- The capability comes before the class check, as in the client's thunks.
        assert(failure(function() return workspace.GetDebugId(5) end) ==
            "The current thread cannot call 'GetDebugId' (lacking capability Plugin)")
        -- Indexing a method is fine; calling it is not.
        assert(type(game:GetService("UserInputService").GetPlatform) == "function")
        value.Value = "fine"
        assert(value.Value == "fine")
    )lua");
}

TEST(script_settings_roots_have_the_clients_names)
{
    // The server's 0x9b challenge adds up the bytes of tostring(UserSettings()).
    Harness h;
    h.run(R"(
        assert(UserSettings() == UserSettings())
        assert(UserSettings().ClassName == "UserSettings")
        assert(tostring(UserSettings()) == "GlobalBasicSettings")
        assert(UserSettings().GameSettings.ClassName == "UserGameSettings")
    )");
}

// Actor messages: SendMessage reaches the topic's bindings, the parallel
// ones too (run serially, which parallel Luau allows).
TEST(script_actor_messages_reach_their_bindings)
{
    Harness h;
    h.run(R"(
        local actor = Instance.new("Actor")
        actor.Parent = workspace
        _G.got = {}
        actor:BindToMessage("hit", function(a, b) table.insert(_G.got, "serial " .. a .. b) end)
        actor:BindToMessageParallel("hit", function(a) table.insert(_G.got, "parallel " .. a) end)
        actor:BindToMessage("other", function() table.insert(_G.got, "other") end)
        actor:SendMessage("hit", 1, 2)
    )");
    CHECK(h.output(R"(table.sort(_G.got) for _, v in _G.got do print(v) end)")
          == (std::vector<std::string>{"parallel 1", "serial 12"}));
}

// SharedTable and SharedTableRegistry, as the game's Parallel
// module uses them for its crates: tables assigned in become SharedTables,
// increment returns the value from before, the registry hands the same table
// back under a name, and frozen clones refuse writes.
TEST(script_shared_tables_and_their_registry)
{
    Harness h;
    CHECK(h.output(R"(
        local st = SharedTable.new({a = 1, nested = {x = 2}})
        print(typeof(st), typeof(st.nested), st.nested.x, SharedTable.size(st))
        st.list = {10, 20}
        print(typeof(st.list), #st.list, st.list[2])
        print(SharedTable.increment(st, "count", 3), SharedTable.increment(st, "count", -1), st.count)
        SharedTable.update(st, "a", function(old) return old + 41 end)
        local keys = {}
        for k in st do table.insert(keys, k) end
        table.sort(keys)
        print(st.a, table.concat(keys, ","))
        local registry = game:GetService("SharedTableRegistry")
        registry:SetSharedTable("jobs", st)
        print(registry:GetSharedTable("jobs") == st, typeof(registry:GetSharedTable("fresh")))
        local frozen = SharedTable.cloneAndFreeze(st, true)
        print(SharedTable.isFrozen(frozen), frozen.nested.x, (pcall(function() frozen.a = 0 end)))
        print((pcall(function() st.f = print end)))
        SharedTable.clear(st)
        print(SharedTable.size(st), SharedTable.size(frozen))
    )") == (std::vector<std::string>{"SharedTable SharedTable 2 2", "SharedTable 2 20", "0 3 2",
                                      "42 a,count,list,nested", "true SharedTable", "true 2 false", "false",
                                      "0 4"}));
}

// require by string, as the game's effects library uses it:
// "./x" a sibling, "../x" one further up, "@self/x" the module's own child,
// resolved from the module whose code calls, whichever thread runs it.
TEST(script_require_by_string_path)
{
    Harness h;
    CHECK(h.output(R"(
        local function module(name, parent, source)
            local m = Instance.new("ModuleScript")
            m.Name = name
            m.Source = source
            m.Parent = parent
            return m
        end
        local folder = Instance.new("Folder")
        folder.Parent = workspace
        local main = module("main", folder, [[
            return require("./sibling") .. require("@self/child") .. require("@self/child/deep")
        ]])
        module("sibling", folder, [[return "S"]])
        local child = module("child", main, [[return "C"]])
        module("deep", child, [[return require("../../sibling") .. "D"]])
        print(require(main))
        print(pcall(require, "nowhere"))
    )") == (std::vector<std::string>{"SCSD", "false require path must start with a valid prefix: ./, ../, or @"}));
}

// PreloadAsync tells its callback of every content id in the instances it's
// given (descendants too) and the strings, once each: a client whose
// callback never hears anything looks like one hooking it.
TEST(script_preload_async_reports_the_content)
{
    Harness h;
    CHECK(h.output(R"(
        local gui = Instance.new("ScreenGui")
        local a = Instance.new("ImageLabel")
        a.Image = "rbxassetid://1"
        a.Parent = gui
        local b = Instance.new("ImageButton")
        b.Image = "rbxassetid://1"
        b.HoverImage = "rbxassetid://2"
        b.Parent = a
        local seen = {}
        game:GetService("ContentProvider"):PreloadAsync({gui, "rbxassetid://3"}, function(id, status)
            table.insert(seen, id .. " " .. status.Name)
        end)
        table.sort(seen)
        print(table.concat(seen, ", "))
        print(game:GetService("ContentProvider"):GetAssetFetchStatus("rbxassetid://1"))
    )") == (std::vector<std::string>{"rbxassetid://1 Success, rbxassetid://2 Success, rbxassetid://3 Success",
                                     "Enum.AssetFetchStatus.Success"}));
}

TEST(script_path2d_control_point)
{
    Harness h;
    CHECK(h.output(R"(
        local p = Path2DControlPoint.new(UDim2.fromOffset(1, 2), UDim2.fromScale(0.5, 0))
        print(typeof(p), p.Position.X.Offset, p.LeftTangent.X.Scale, p.RightTangent == UDim2.new())
        print(Path2DControlPoint.new() == Path2DControlPoint.new())
    )") == (std::vector<std::string>{"Path2DControlPoint 1 0.5 true", "true"}));
}

TEST(script_the_rarer_datatypes)
{
    Harness h;
    CHECK_EQ(h.output(R"lua(
        local v = Vector2int16.new(3, -4)
        print(typeof(v), v.X, v.Y, v + Vector2int16.new(1, 1), v * 2, -v, v == Vector2int16.new(3, -4))
        local w = PathWaypoint.new(Vector3.new(1, 2, 3), Enum.PathWaypointAction.Jump, "Door")
        print(typeof(w), w.Position, w.Action, w.Label, PathWaypoint.new().Action)
        local key = RotationCurveKey.new(0.5, CFrame.new(), Enum.KeyInterpolationMode.Cubic)
        print(typeof(key), key.Time, key.Interpolation, key.LeftTangent)
        local uri, none = Content.fromUri("rbxassetid://7"), Content.none
        print(typeof(uri), uri.SourceType, uri.Uri, none.SourceType, none.Uri, uri == Content.fromAssetId(7))
        print(Content.fromObject(workspace).Object == workspace)
        local drag = PluginDrag.new("me", "text/plain", "hi")
        print(typeof(drag), drag.Sender, drag.MimeType, drag.Data, drag.HotSpot)
        print(type(CellId), stats() == game:GetService("Stats"), Stats == stats)
        print(loadstring("return 1"))
        print(pcall(PluginManager))
    )lua"),
             (std::vector<std::string>{
                 "Vector2int16 3 -4 4, -3 6, -8 -3, 4 true",
                 "PathWaypoint 1, 2, 3 Enum.PathWaypointAction.Jump Door Enum.PathWaypointAction.Walk",
                 "RotationCurveKey 0.5 Enum.KeyInterpolationMode.Cubic nil",
                 "Content Enum.ContentSourceType.Uri rbxassetid://7 Enum.ContentSourceType.None nil true",
                 "true",
                 "PluginDrag me text/plain hi 0, 0",
                 "table true false",
                 "nil loadstring() is not available",
                 "false The current thread cannot call 'PluginManager' (lacking capability Plugin)",
             }));
}

TEST(script_members_the_reflection_dump_missed_resolve)
{
    // In the 0.742 player but not in spelunk's reflection dump.
    Harness h;
    h.run(R"lua(
        local market = game:GetService("MarketplaceService")
        assert(typeof(market.PromptBulkPurchaseFinished) == "RBXScriptSignal")
        market.PromptBulkPurchaseFinished:Connect(function() end)
    )lua");
}

TEST(script_gui_inset_is_the_top_bar_we_lay_out_under)
{
    Harness h;
    CHECK_EQ(h.print(R"(print(game:GetService("GuiService"):GetGuiInset()))"), "0, 36 0, 0");
}

TEST(script_log_history_has_what_was_printed)
{
    Harness h;
    h.run(R"lua(
        local logs = game:GetService("LogService")
        local seen = {}
        logs.MessageOut:Connect(function(message, kind) table.insert(seen, kind) end)
        print("hello")
        warn("careful")
        local _ = game:GetService("Lighting").ShadowColor -- unimplemented: a notice scripts never see
        local history = logs:GetLogHistory()
        local first, last = history[#history - 1], history[#history]
        assert(first.message == "hello" and first.messageType == Enum.MessageType.MessageOutput)
        assert(last.message == "careful" and last.messageType == Enum.MessageType.MessageWarning)
        assert(type(last.timestamp) == "number" and math.abs(last.timestamp - os.time()) <= 2)
        assert(#seen == 2 and seen[2] == Enum.MessageType.MessageWarning)
        -- Two calls give equal entries (Adonis compares them).
        local again = logs:GetLogHistory()
        assert(#again == #history and again[#again].messageType == last.messageType)
    )lua");
}

TEST(script_translators_for_players_and_locales)
{
    auto game = DataModel::create();
    auto* players = dynamic_cast<Players*>(game->getService("Players"));
    auto me = std::make_shared<Player>();
    me->setName("Me");
    me->setParent(players);
    players->assignLocalPlayer(me.get());
    ScriptContext client(game, ScriptRole::Client);
    CHECK(client
              .run(R"lua(
        local localization = game:GetService("LocalizationService")
        local translator = localization:GetTranslatorForPlayerAsync(game:GetService("Players").LocalPlayer)
        assert(translator.ClassName == "Translator" and translator.LocaleId == "en-us")
        local changed = translator:GetPropertyChangedSignal("LocaleId")
        assert(typeof(changed) == "RBXScriptSignal")
        assert(localization:GetTranslatorForLocaleAsync("de-de").LocaleId == "de-de")
        -- The older name, TopbarPlus's (the game's character select)
        assert(localization:GetTranslatorForPlayer(game:GetService("Players").LocalPlayer):Translate(game, "Hi") == "Hi")
    )lua",
                   "Test")
              .has_value());
}

// Player.DevEnableMouseLock: the PlayerModule's MouseLockController allows
// shift lock only while it's true (the game had none without it).
TEST(script_player_dev_enable_mouse_lock)
{
    auto game = DataModel::create();
    auto* players = dynamic_cast<Players*>(game->getService("Players"));
    auto me = std::make_shared<Player>();
    me->setParent(players);
    players->assignLocalPlayer(me.get());
    ScriptContext client(game, ScriptRole::Client);
    CHECK(client
              .run(R"lua(
        local me = game:GetService("Players").LocalPlayer
        assert(me.DevEnableMouseLock == true)
        local changed = false
        me:GetPropertyChangedSignal("DevEnableMouseLock"):Connect(function() changed = true end)
        me.DevEnableMouseLock = false
        task.wait()
        assert(changed and me.DevEnableMouseLock == false)
    )lua",
                   "Test")
              .has_value());
}

TEST(script_collectgarbage_only_counts)
{
    Harness h;
    h.run(R"lua(
        assert(gcinfo() == collectgarbage("count"))
        assert(type(collectgarbage("count")) == "number")
        local ok, e = pcall(collectgarbage, "collect")
        assert(not ok and e == "collectgarbage must be called with 'count'; use gcinfo() instead", e)
    )lua");
}

TEST(script_settings_needs_a_capability_game_scripts_lack)
{
    Harness h;
    CHECK_EQ(h.error("settings()"), "The current thread cannot call 'settings' (lacking capability Plugin)");
    h.run(R"lua(
        local ok, message = pcall(settings)
        assert(not ok and message == "The current thread cannot call 'settings' (lacking capability Plugin)")
    )lua");
}

// --- Properties --------------------------------------------------------------

TEST(script_reads_engine_properties)
{
    Harness h;
    auto part = makePart("Brick", h.game->workspace());
    part->setTransparency(0.25f);
    part->setPosition({1.0f, 2.0f, 3.0f});
    h.run(R"(
        local p = workspace.Brick
        print(p.Name, p.ClassName, p.Transparency, p.Anchored, p.Position, p.Parent)
    )");
    CHECK_EQ(h.lines.at(0), "Brick Part 0.25 false 1, 2, 3 Workspace");
}

TEST(script_writes_engine_properties)
{
    Harness h;
    auto part = makePart("Brick", h.game->workspace());
    h.run(R"(
        local p = workspace.Brick
        p.Name = "Renamed"
        p.Transparency = 0.5
        p.Anchored = true
        p.Size = Vector3.new(1, 2, 3)
        p.Color = Color3.fromRGB(255, 0, 0)
        p.CFrame = CFrame.new(4, 5, 6)
    )");
    CHECK_EQ(part->name(), "Renamed");
    CHECK_EQ(part->transparency(), 0.5f);
    CHECK(part->anchored());
    CHECK(part->size() == (Vector3{1.0f, 2.0f, 3.0f}));
    CHECK(part->color() == (Color3{1.0f, 0.0f, 0.0f}));
    CHECK(part->position() == (Vector3{4.0f, 5.0f, 6.0f}));
}

// GuiObject.Transparency, deprecated: BackgroundTransparency under another
// name, which on text objects sets TextTransparency too.
TEST(script_gui_transparency_is_the_background_transparency)
{
    Harness h;
    h.run(R"(
        local frame = Instance.new("Frame")
        frame.Transparency = 0.25
        local label = Instance.new("TextLabel")
        label.TextTransparency = 0.125
        label.Transparency = 0.5
        print(frame.BackgroundTransparency, frame.Transparency, label.BackgroundTransparency, label.TextTransparency)
        frame.BackgroundTransparency = 0.75
        print(frame.Transparency)
    )");
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[0], "0.25 0.25 0.5 0.5");
    CHECK_EQ(h.lines[1], "0.75");
}

// The 3D adornments as Infinite Yield makes them: a SelectionBox's Color3,
// ESP's BoxHandleAdornments colored through the deprecated BrickColor Color.
TEST(script_adornments_have_their_properties)
{
    Harness h;
    h.run(R"(
        local box = Instance.new("SelectionBox")
        print(box.Color3 == Color3.fromRGB(13, 105, 172), box.LineThickness, box.SurfaceTransparency, box.Visible)
        box.Color3 = Color3.new(0, 1, 0)
        box.Adornee = workspace
        print(box.Color, box.Adornee == workspace, box:IsA("InstanceAdornment"), box:IsA("GuiBase3d"), box:IsA("GuiBase"))
        local esp = Instance.new("BoxHandleAdornment")
        esp.AlwaysOnTop, esp.ZIndex, esp.Size = true, 10, Vector3.new(2, 1, 1)
        esp.Color = BrickColor.new("Bright red")
        print(esp.Color3 == BrickColor.new("Bright red").Color, esp.ZIndex, esp.Size, esp:IsA("PVAdornment"))
        print(Instance.new("SphereHandleAdornment").Radius, Instance.new("CylinderHandleAdornment").Angle,
              Instance.new("Handles").Style, Instance.new("SurfaceSelection").TargetSurface)
        print(pcall(Instance.new, "GuiBase3d"))
    )");
    CHECK_EQ(h.lines.size(), 5u);
    CHECK_EQ(h.lines.at(0), "true 0.15000000596046448 1 true"); // a float, as Roblox reads it back
    CHECK_EQ(h.lines.at(1), "Lime green true true true true");
    CHECK_EQ(h.lines.at(2), "true 10 2, 1, 1 true");
    CHECK_EQ(h.lines.at(3), "1 360 Enum.HandlesStyle.Resize Enum.NormalId.Right");
    CHECK(h.lines.at(4).starts_with("false "));
}

// The deprecated FontSize is TextSize in steps.
TEST(script_font_size_is_text_size_in_steps)
{
    Harness h;
    h.run(R"(
        local label = Instance.new("TextLabel")
        print(label.FontSize, label.TextSize)
        label.FontSize = Enum.FontSize.Size24
        print(label.TextSize)
        label.FontSize = Enum.FontSize.Size28
        print(label.TextSize, label.FontSize)
        label.TextSize = 25
        print(label.FontSize)
        local button = Instance.new("TextButton")
        button.FontSize = "Size18"
        print(button.TextSize, Instance.new("TextBox").FontSize)
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"Enum.FontSize.Size14 14", "24", "28 Enum.FontSize.Size28",
                                                "Enum.FontSize.Size24", "18 Enum.FontSize.Size14"}));
}

TEST(script_property_errors_use_roblox_wording)
{
    Harness h;
    makePart("Part", h.game->workspace());
    CHECK_EQ(h.error("print(workspace.Part.Foo)"), "Foo is not a valid member of Part \"Workspace.Part\"");
    CHECK_EQ(h.error("workspace.Part.Foo = 1"), "Foo is not a valid member of Part \"Workspace.Part\"");
    CHECK_EQ(h.error("workspace.Part.Size = 'big'"), "Unable to assign property Size. Vector3 expected, got string");
    CHECK_EQ(h.error("workspace.Part.Size = {}"), "Unable to assign property Size. Vector3 expected, got table");
    CHECK_EQ(h.error("workspace.Part.ClassName = 'Model'"),
             "Unable to assign property ClassName. Property is read only");
    CHECK_EQ(h.error("workspace.Part:Destroy() workspace.Part.Parent = workspace"),
             "Part is not a valid member of Workspace \"Workspace\"");
}

// --- Instances ---------------------------------------------------------------

TEST(script_instance_new_and_parenting)
{
    Harness h;
    h.run(R"(
        local model = Instance.new("Model", workspace)
        model.Name = "Tower"
        local part = Instance.new("Part")
        part.Parent = model
        assert(part:IsDescendantOf(workspace))
        assert(workspace:IsAncestorOf(part))
        assert(part:GetFullName() == "Workspace.Tower.Part")
    )");
    Instance* tower = h.game->workspace()->findFirstChild("Tower");
    CHECK(tower != nullptr);
    CHECK(tower->findFirstChild("Part") != nullptr);
    CHECK_EQ(h.error("Instance.new('Nope')"), "Unable to create an Instance of type \"Nope\"");
    CHECK_EQ(h.error("workspace.Parent = nil"),
             "The Parent property of Workspace is locked, current parent: DataModel, new parent NULL");
}

TEST(script_instances_keep_their_identity)
{
    Harness h;
    makePart("Part", h.game->workspace());
    h.run(R"(
        local a, b = workspace.Part, workspace:FindFirstChild("Part")
        assert(rawequal(a, b))
        local seen = {[a] = true}
        assert(seen[workspace:GetChildren()[1]])
        assert(rawequal(a.Parent, workspace))
    )");
}

TEST(script_instance_methods)
{
    Harness h;
    h.run(R"(
        local folder = Instance.new("Folder", workspace)
        local model = Instance.new("Model", folder)
        local part = Instance.new("Part", model)
        part.Name = "Deep"

        assert(workspace:FindFirstChild("Deep") == nil)
        assert(workspace:FindFirstChild("Deep", true) == part)
        assert(model:FindFirstChildOfClass("Part") == part)
        assert(folder:FindFirstChildWhichIsA("BasePart", true) == part)
        assert(part:FindFirstAncestorOfClass("Folder") == folder)
        assert(part:FindFirstAncestorWhichIsA("PVInstance") == model)
        assert(part:FindFirstAncestor("Folder") == folder)
        assert(part:IsA("BasePart") and not part:IsA("Model"))
        assert(#folder:GetChildren() == 1 and #folder:GetDescendants() == 2)

        local copy = model:Clone()
        assert(copy.Parent == nil and copy.Deep ~= part and copy.Deep.Name == "Deep")

        part:Destroy()
        assert(part.Parent == nil and #model:GetChildren() == 0)
        local ok, err = pcall(function() part.Parent = workspace end)
        assert(not ok and err:find("locked"))

        Instance.new("Part", folder)
        folder:ClearAllChildren()
        assert(#folder:GetChildren() == 0)
    )");
}

TEST(script_method_call_errors)
{
    Harness h;
    CHECK_EQ(h.error("workspace.FindFirstChild('x')"), "Expected ':' not '.' calling member function FindFirstChild");
    CHECK_EQ(h.error("game:GetService('Nope')"), "'Nope' is not a valid Service name");
    CHECK_EQ(h.error("workspace:GetService('Workspace')"),
             "GetService is not a valid member of Workspace \"Workspace\"");
}

TEST(script_engine_errors_carry_no_position)
{
    // The client's errors come from C++ exceptions, raised with the message
    // alone; Luau's own errors still say where they happened.
    Harness h;
    h.run(R"lua(
        local ok, e = pcall(function() return workspace.Nope end)
        assert(e == 'Nope is not a valid member of Workspace "Workspace"', e)
        ok, e = pcall(function() workspace:Kick() end)
        assert(e == 'Kick is not a valid member of Workspace "Workspace"', e)
        ok, e = pcall(function() local t = nil; return t.x end)
        assert(e:match("^Test:%d+: attempt to index nil"), e)
    )lua");
}

TEST(script_metamethods_are_nameless_c_functions)
{
    // Scripts reach a metamethod through the frame that raised (debug.info(2)
    // in an xpcall handler); the client's have no name, like this.
    Harness h;
    h.run(R"lua(
        local function raiser(f)
            local found
            xpcall(f, function() found = debug.info(2, "f") end)
            return found
        end
        local function check(what, f)
            local m = raiser(f)
            assert(type(m) == "function", what)
            assert(debug.info(m, "s") == "[C]", what)
            assert(debug.info(m, "l") == -1, what)
            assert(debug.info(m, "n") == "", what .. ": " .. debug.info(m, "n"))
            assert(debug.info(m, "a") == 0, what)
            return m
        end
        local index = check("Instance.__index", function() local _ = game.____ end)
        local newindex = check("Instance.__newindex", function() game.____ = 5 end)
        local namecall = check("Instance.__namecall", function() game:____() end)
        check("Enum.__index", function() local _ = Enum.HumanoidStateType.____ end)
        check("Enum namecall", function() Enum.HumanoidStateType:____() end)
        check("Vector3.__index", function() local _ = Vector3.one.____ end)
        check("CFrame.__index", function() local _ = CFrame.new().____ end)
        check("UDim2.__index", function() local _ = UDim2.new().____ end)
        check("RBXScriptSignal.__index", function() local _ = workspace.ChildAdded.____ end)
        assert(index ~= newindex and index ~= namecall)

        -- Called with nothing to work on, they fail, and the same way each time.
        local probe = newproxy(true)
        getmetatable(probe).__index = function() error("touched") end
        getmetatable(probe).__tostring = function() error("touched") end
        for _, m in {index, newindex, namecall} do
            local a, e1 = pcall(m)
            local b, e2 = pcall(m)
            assert(not a and not b and e1 == e2)
            assert(not pcall(m, probe, "Name"))
            assert(not pcall(m, game, probe))
        end
    )lua");
}

TEST(script_namecall_finds_methods_only)
{
    Harness h;
    h.run(R"lua(
        local child = Instance.new("Folder")
        child.Name = "Child"
        child.Parent = workspace
        -- The capitalised name, by call and by index (after children).
        assert(#workspace:getDescendants() == 1)
        assert(workspace.getDescendants == workspace.GetDescendants)
        local ok, e = pcall(function() workspace:Child() end)
        assert(e == 'Child is not a valid member of Workspace "Workspace"', e)
        ok, e = pcall(function() workspace:Name() end)
        assert(e == 'Name is not a valid member of Workspace "Workspace"', e)
        ok, e = pcall(function() workspace:KicK() end)
        assert(e == 'KicK is not a valid member of Workspace "Workspace"', e)
        ok, e = pcall(function() workspace:fireServer() end)
        assert(e == 'fireServer is not a valid member of Workspace "Workspace"', e)
        -- Yielding methods resume through it.
        task.delay(0, function() Instance.new("Folder", workspace).Name = "Later" end)
        assert(workspace:WaitForChild("Later").Name == "Later")
    )lua");
}

TEST(script_methods_are_the_same_function_each_time)
{
    Harness h;
    h.run(R"lua(
        assert(game.GetService == game.GetService)
        assert(workspace.FindFirstChild == game.FindFirstChild)
        local market = game:GetService("MarketplaceService")
        assert(market.PromptPurchase == market.PromptPurchase)
        assert(rawequal(market.PromptPurchase, market.PromptPurchase))
    )lua");
}

TEST(script_methods_check_their_class)
{
    Harness h;
    h.run(R"lua(
        local function failure(f, ...)
            local ok, e = pcall(f, ...)
            assert(not ok)
            return e
        end
        local remote = Instance.new("RemoteEvent")
        assert(failure(remote.FireServer, workspace) == "Expected ':' not '.' calling member function FireServer")
        assert(failure(workspace.GetRealPhysicsFPS, game) ==
            "Expected ':' not '.' calling member function GetRealPhysicsFPS")
        local logs = game:GetService("LogService")
        assert(failure(logs.GetLogHistory, workspace) == "Expected ':' not '.' calling member function GetLogHistory")
        -- Functions Lunarstone doesn't implement check too.
        local market = game:GetService("MarketplaceService")
        assert(failure(market.PromptPurchase, workspace) ==
            "Expected ':' not '.' calling member function PromptPurchase")
        -- Inherited methods take any instance of the class that has them.
        assert(workspace.FindFirstChild(game, "Workspace") == workspace)
    )lua");
    // Classes the engine only has a placeholder for still know their bases.
    auto replicator = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("ClientReplicator"));
    replicator->setParent(h.game->workspace());
    h.run(R"lua(
        local replicator = workspace.ClientReplicator
        assert(replicator:IsA("NetworkReplicator") and not replicator:IsA("Part"))
    )lua");
}

TEST(script_client_kicks_only_its_own_player)
{
    auto game = DataModel::create();
    auto* players = dynamic_cast<Players*>(game->getService("Players"));
    CHECK(players != nullptr);
    auto me = std::make_shared<Player>();
    me->setName("Me");
    me->setParent(players);
    players->assignLocalPlayer(me.get());
    auto other = std::make_shared<Player>();
    other->setName("Other");
    other->setParent(players);
    ScriptContext client(game, ScriptRole::Client);
    std::vector<std::string> lines;
    client.setOutput([&](MessageType type, std::string_view message) {
        if (type != MessageType::Info)
            lines.emplace_back(message);
    });
    CHECK(client
              .run(R"lua(
        local players = game:GetService("Players")
        local me, other = players.Me, players.Other
        local ok, e = pcall(me.Kick, other, "x")
        assert(not ok and e == "Cannot kick a non-local Player from a LocalScript", e)
        ok, e = pcall(function() other:Kick("x") end)
        assert(not ok and e == "Cannot kick a non-local Player from a LocalScript", e)
        ok, e = pcall(me.Kick, workspace, "x")
        assert(not ok and e == "Expected ':' not '.' calling member function Kick", e)
        me:Kick("bye")
    )lua",
                   "Test")
              .has_value());
    CHECK(std::ranges::find(lines, std::string("Me was kicked: bye")) != lines.end());
}

// --- Events ------------------------------------------------------------------

TEST(script_events_connect_and_disconnect)
{
    Harness h;
    h.run(R"(
        local added = {}
        local connection = workspace.ChildAdded:Connect(function(child)
            table.insert(added, child.Name)
        end)
        assert(connection.Connected)
        Instance.new("Folder", workspace).Name = "ignored-name-change"
        local second = Instance.new("Part")
        second.Name = "Second"
        second.Parent = workspace
        connection:Disconnect()
        assert(not connection.Connected)
        Instance.new("Part", workspace)
        print(table.concat(added, ","))
    )");
    CHECK_EQ(h.lines.at(0), "Folder,Second");
}

TEST(script_events_fire_from_engine_changes)
{
    Harness h;
    auto part = makePart("Part", h.game->workspace());
    h.run(R"(
        local p = workspace.Part
        p.Changed:Connect(function(property) print("changed", property) end)
        p:GetPropertyChangedSignal("Anchored"):Connect(function() print("anchored", p.Anchored) end)
        p.Destroying:Connect(function() print("destroying") end)
    )");
    part->setAnchored(true);
    part->setName("Renamed");
    part->destroy();
    CHECK_EQ(h.lines.size(), 5u);
    CHECK_EQ(h.lines[0], "changed Anchored");
    CHECK_EQ(h.lines[1], "anchored true");
    CHECK_EQ(h.lines[2], "changed Name");
    CHECK_EQ(h.lines[3], "destroying");
    CHECK_EQ(h.lines[4], "changed Parent");
    CHECK_EQ(h.error("workspace:GetPropertyChangedSignal('Nope')"), "Nope is not a valid property name.");
}

TEST(script_once_fires_a_single_time)
{
    Harness h;
    h.run(R"(
        local count = 0
        local connection = workspace.ChildAdded:Once(function() count += 1 end)
        Instance.new("Part", workspace)
        Instance.new("Part", workspace)
        print(count, connection.Connected)
    )");
    CHECK_EQ(h.lines.at(0), "1 false");
}

TEST(script_signal_wait_yields_until_the_next_fire)
{
    Harness h;
    h.run(R"(
        task.spawn(function()
            local child = workspace.ChildAdded:Wait()
            print("got", child.Name)
        end)
        print("waiting")
    )");
    CHECK_EQ(h.lines.size(), 1u);
    makePart("Arrival", h.game->workspace());
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[1], "got Arrival");
    makePart("Another", h.game->workspace());
    CHECK_EQ(h.lines.size(), 2u);
}

TEST(script_callback_errors_go_to_the_output)
{
    Harness h;
    h.run(R"(
        workspace.ChildAdded:Connect(function() error("bad handler") end)
        workspace.ChildAdded:Connect(function() print("still runs") end)
        Instance.new("Part", workspace)
        print("after")
    )");
    CHECK_EQ(h.lines.size(), 3u);
    CHECK_EQ(h.lines[0], "error: Test:2: bad handler");
    CHECK_EQ(h.lines[1], "still runs");
    CHECK_EQ(h.lines[2], "after");
}

TEST(script_runaway_event_recursion_is_stopped)
{
    Harness h;
    h.run(R"(
        local p = Instance.new("Part", workspace)
        p.Changed:Connect(function() p.Transparency += 0.001 end)
        p.Transparency = 0.5
    )");
    bool limited = false;
    for (const std::string& line : h.lines)
        limited = limited || line.contains("re-entrancy") || line.contains("stack overflow");
    CHECK(limited);
}

TEST(script_connections_outliving_the_context_are_harmless)
{
    auto game = DataModel::create();
    auto part = makePart("Part", game->workspace());
    {
        ScriptContext context(game);
        context.setOutput([](MessageType, std::string_view) {});
        CHECK(context.run("workspace.Part.Changed:Connect(function() print('x') end)").has_value());
        CHECK(context.run("task.spawn(function() workspace.Part.Changed:Wait() end)").has_value());
        CHECK(context.run("task.spawn(function() workspace:WaitForChild('Never') end)").has_value());
    }
    part->setName("AfterContext");
    makePart("Never", game->workspace());
    CHECK_EQ(part->changed.connectionCount(), 0u);
}

TEST(script_destroy_drops_script_connections)
{
    Harness h;
    auto part = makePart("Part", h.game->workspace());
    h.run(R"(
        local p = workspace.Part
        connection = p.Changed:Connect(function() end)
        p:Destroy()
        print(connection.Connected)
    )");
    CHECK_EQ(h.lines.at(0), "false");
    CHECK_EQ(part->changed.connectionCount(), 0u);
}

// --- Scheduler ---------------------------------------------------------------

TEST(script_task_wait_resumes_from_step)
{
    Harness h;
    h.run(R"(
        print("start")
        local elapsed = task.wait(0.5)
        print("resumed", elapsed >= 0.5, time())
    )");
    CHECK_EQ(h.lines.size(), 1u);
    h.steps(29);
    CHECK_EQ(h.lines.size(), 1u);
    h.steps(1);
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[1], "resumed true 0.5");
    CHECK(h.context.idle());
}

TEST(script_task_spawn_defer_delay_order)
{
    Harness h;
    h.run(R"(
        task.delay(0.1, function(x) print("delay", x) end, "arg")
        task.defer(function() print("defer") end)
        task.spawn(function(x) print("spawn", x) end, 1)
        print("main")
    )");
    CHECK_EQ(h.lines.size(), 3u);
    CHECK_EQ(h.lines[0], "spawn 1");
    CHECK_EQ(h.lines[1], "main");
    CHECK_EQ(h.lines[2], "defer");
    h.steps(6);
    CHECK_EQ(h.lines.size(), 4u);
    CHECK_EQ(h.lines[3], "delay arg");
}

TEST(script_task_wait_in_a_loop_waits_a_step_each_time)
{
    Harness h;
    h.run(R"(
        task.spawn(function()
            for i = 1, 3 do
                task.wait()
                print(i)
            end
        end)
    )");
    for (std::size_t i = 1; i <= 3; ++i) {
        h.steps(1);
        CHECK_EQ(h.lines.size(), i);
    }
}

TEST(script_task_cancel_stops_a_waiting_thread)
{
    Harness h;
    h.run(R"(
        local thread = task.delay(1, function() print("never") end)
        local waiting = task.spawn(function() task.wait(1) print("never either") end)
        task.cancel(thread)
        task.cancel(waiting)
        print(coroutine.status(waiting))
    )");
    h.steps(120);
    CHECK_EQ(h.lines.size(), 1u);
    CHECK_EQ(h.lines[0], "dead");
}

TEST(script_wait_for_child_yields_or_times_out)
{
    Harness h;
    makePart("Here", h.game->workspace());
    h.run(R"(
        assert(workspace:WaitForChild("Here").Name == "Here")
        task.spawn(function() print("found", workspace:WaitForChild("Later"):GetFullName()) end)
        task.spawn(function() print("timeout", workspace:WaitForChild("Missing", 0.5)) end)
    )");
    CHECK(h.lines.empty());
    makePart("Later", h.game->workspace());
    CHECK_EQ(h.lines.size(), 1u);
    CHECK_EQ(h.lines[0], "found Workspace.Later");
    h.steps(30);
    CHECK_EQ(h.lines.size(), 2u);
    CHECK_EQ(h.lines[1], "timeout nil");
}

TEST(script_wait_for_child_warns_about_infinite_yields)
{
    Harness h;
    h.run("workspace:WaitForChild('Ghost')");
    h.steps(301);
    CHECK_EQ(h.lines.size(), 1u);
    CHECK_EQ(h.lines[0], "warn: Infinite yield possible on 'Workspace:WaitForChild(\"Ghost\")'");
}

TEST(script_yielding_where_it_cannot_is_an_error)
{
    Harness h;
    // task.wait's own words in the client (0x1014cf855), not Luau's.
    CHECK(h.error("local _ = setmetatable({}, {__index = function() task.wait() end}).x")
              .contains("thread is not yieldable"));
}

TEST(script_runaway_loops_time_out)
{
    Harness h;
    h.context.setTimeout(0.05);
    CHECK_EQ(h.error("while true do end"), "Test:1: Script timeout: exhausted allowed execution time");
    h.run("print('still alive')");
    CHECK_EQ(h.lines.at(0), "still alive");
}

// --- Sandboxing --------------------------------------------------------------

TEST(script_globals_are_per_script)
{
    Harness h;
    h.run("x = 1 game = 5 print(x, game)");
    h.run("print(x, game)");
    CHECK_EQ(h.lines.at(0), "1 5");
    CHECK_EQ(h.lines.at(1), "nil DataModel");
}

TEST(script_shared_libraries_are_read_only)
{
    Harness h;
    CHECK_EQ(h.error("string.upper = nil"), "Test:1: attempt to modify a readonly table");
    CHECK_EQ(h.error("Vector3.new = nil"), "Test:1: attempt to modify a readonly table");
    CHECK_EQ(h.error("getmetatable(workspace).__index = nil"), "Test:1: attempt to index string with '__index'");
    h.run("assert(getmetatable(Vector3.zero) == 'The metatable is locked')");
}

TEST(script_g_and_shared_are_shared)
{
    Harness h;
    h.run("_G.answer = 42 shared.name = 'x'");
    h.run("print(_G.answer, shared.name)");
    CHECK_EQ(h.lines.at(0), "42 x");
}

// --- Datatypes ---------------------------------------------------------------

TEST(script_vector3)
{
    Harness h;
    h.run(R"(
        local v = Vector3.new(3, 4, 0)
        assert(v.X == 3 and v.Y == 4 and v.Z == 0 and v.x == 3)
        assert(v.Magnitude == 5)
        assert(v.Unit == Vector3.new(0.6, 0.8, 0))
        assert(v:Dot(Vector3.xAxis) == 3)
        assert(Vector3.xAxis:Cross(Vector3.yAxis) == Vector3.zAxis)
        assert(v + Vector3.one == Vector3.new(4, 5, 1))
        assert(v - v == Vector3.zero and v * 2 == Vector3.new(6, 8, 0) and v / 2 == Vector3.new(1.5, 2, 0))
        assert(-v == Vector3.new(-3, -4, 0))
        assert(Vector3.zero:Lerp(Vector3.one, 0.5) == Vector3.one / 2)
        assert(Vector3.new(1, 5, 3):Max(Vector3.new(4, 2, 6)) == Vector3.new(4, 5, 6))
        assert(Vector3.new(-1.5, 2.5, 0):Floor() == Vector3.new(-2, 2, 0))
        assert(typeof(v) == "Vector3")
        assert(tostring(Vector3.new(1, 2.5, -3)) == "1, 2.5, -3")
        assert(Vector3.new() == Vector3.zero)
        assert(math.abs(Vector3.xAxis:Angle(Vector3.yAxis) - math.pi / 2) < 1e-6)
    )");
    CHECK_EQ(h.error("print(Vector3.one.Nope)"), "Nope is not a valid member of Vector3");
}

TEST(script_color3)
{
    Harness h;
    h.run(R"(
        local c = Color3.fromRGB(255, 0, 0)
        assert(c.R == 1 and c.G == 0 and c.B == 0)
        assert(c == Color3.new(1, 0, 0))
        assert(typeof(c) == "Color3")
        local hue, sat, val = Color3.fromHSV(1/3, 1, 1):ToHSV()
        assert(math.abs(hue - 1/3) < 1e-6 and sat == 1 and val == 1)
        assert(Color3.fromHSV(1/3, 1, 1) == Color3.new(0, 1, 0))
        assert(Color3.new(0, 0, 0):Lerp(Color3.new(1, 1, 1), 0.5) == Color3.new(0.5, 0.5, 0.5))
        assert(Color3.fromHex("#ff8000"):ToHex() == "ff8000")
        assert(Color3.fromHex("0f0") == Color3.new(0, 1, 0))
        assert(tostring(Color3.new(1, 0.5, 0)) == "1, 0.5, 0")
    )");
}

TEST(script_cframe)
{
    Harness h;
    h.run(R"(
        local function near(a, b) return (a - b).Magnitude < 1e-5 end
        local cf = CFrame.new(1, 2, 3)
        assert(cf.Position == Vector3.new(1, 2, 3) and cf.X == 1 and cf.Y == 2 and cf.Z == 3)
        assert(cf.LookVector == -Vector3.zAxis and cf.RightVector == Vector3.xAxis and cf.UpVector == Vector3.yAxis)
        assert(typeof(cf) == "CFrame")

        local turned = cf * CFrame.Angles(0, math.pi / 2, 0)
        assert(near(turned.LookVector, -Vector3.xAxis))
        assert(near(turned * Vector3.new(0, 0, -1), Vector3.new(0, 2, 3)))
        assert(near((turned:Inverse() * turned).Position, Vector3.zero))
        assert(near(turned:PointToObjectSpace(turned:PointToWorldSpace(Vector3.one)), Vector3.one))
        assert(near(turned:ToObjectSpace(turned:ToWorldSpace(cf)).Position, cf.Position))
        assert(near(turned:VectorToWorldSpace(Vector3.zAxis), Vector3.xAxis))

        assert((cf + Vector3.one).Position == Vector3.new(2, 3, 4))
        assert((cf - Vector3.one).Position == Vector3.new(0, 1, 2))
        assert(CFrame.new() == CFrame.identity)
        assert(CFrame.new(Vector3.one) == CFrame.new(1, 1, 1))

        local look = CFrame.lookAt(Vector3.zero, Vector3.new(10, 0, 0))
        assert(near(look.LookVector, Vector3.xAxis))
        assert(near(CFrame.new(Vector3.zero, Vector3.new(0, 0, 5)).LookVector, Vector3.zAxis))

        local x, y, z = CFrame.Angles(0.1, 0.2, 0.3):ToEulerAnglesXYZ()
        assert(math.abs(x - 0.1) < 1e-5 and math.abs(y - 0.2) < 1e-5 and math.abs(z - 0.3) < 1e-5)
        local rx, ry, rz = CFrame.fromOrientation(0.1, 0.2, 0.3):ToOrientation()
        assert(math.abs(rx - 0.1) < 1e-5 and math.abs(ry - 0.2) < 1e-5 and math.abs(rz - 0.3) < 1e-5)

        local components = {CFrame.new(1, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1):GetComponents()}
        assert(#components == 12 and components[1] == 1 and components[4] == 1 and components[5] == 0)

        local half = CFrame.new():Lerp(CFrame.new(2, 0, 0) * CFrame.Angles(0, math.pi / 2, 0), 0.5)
        assert(near(half.Position, Vector3.xAxis))
        assert(near(half.LookVector, CFrame.Angles(0, math.pi / 4, 0).LookVector))

        local axis, angle = CFrame.fromAxisAngle(Vector3.yAxis, 1):ToAxisAngle()
        assert(near(axis, Vector3.yAxis) and math.abs(angle - 1) < 1e-5)
    )");
    CHECK_EQ(h.error("CFrame.new(1, 2)"), "Test:1: invalid argument #1 to 'new' (Vector3 expected, got number)");
    CHECK(h.error("local _ = Vector3.one * CFrame.new()").contains("attempt to perform arithmetic"));
}

TEST(script_typeof_names)
{
    Harness h;
    h.run(R"(
        print(typeof(nil), typeof(1), typeof("s"), typeof({}), typeof(print), typeof(workspace),
              typeof(workspace.ChildAdded), typeof(workspace.ChildAdded:Connect(function() end)),
              typeof(Vector3.one), typeof(CFrame.new()), typeof(Color3.new()), typeof(coroutine.running()))
    )");
    std::string_view expected
        = "nil number string table function Instance RBXScriptSignal RBXScriptConnection Vector3 CFrame Color3 thread";
    CHECK_EQ(h.lines.at(0), expected);
}

// --- Scripts in a place ------------------------------------------------------

TEST(script_server_scripts_are_found_in_workspace_and_server_script_service)
{
    auto game = DataModel::create();
    auto make = [](std::string_view className, Instance* parent) {
        auto instance = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
        instance->setParent(parent);
        return instance;
    };
    auto service = make("ServerScriptService", game.get());
    auto inWorkspace = make("Script", game->workspace());
    auto inService = make("Script", service.get());
    make("LocalScript", game->workspace());
    make("ModuleScript", service.get());
    auto storage = make("ReplicatedStorage", game.get());
    make("Script", storage.get());

    auto scripts = serverScripts(*game);
    CHECK_EQ(scripts.size(), 2u);
    CHECK(scripts[0] == inWorkspace);
    CHECK(scripts[1] == inService);

    // Placeholder scripts have no Source to read yet.
    ScriptContext context(game);
    auto result = context.runScript(*inWorkspace);
    CHECK(!result.has_value());
    CHECK(result.error().message.contains("Source"));
}

TEST(script_global_is_the_running_script)
{
    Harness h;
    auto folder = ClassRegistry::create("Folder");
    folder->setName("Holder");
    folder->setParent(h.game->workspace());
    CHECK(h.context.run("print(script:GetFullName())", "Holder", folder.get()).has_value());
    CHECK_EQ(h.lines.at(0), "Workspace.Holder");
}

TEST(script_game_has_empty_server_containers_and_its_universe)
{
    // A client has ServerStorage and ServerScriptService, empty: Ball Tower
    // Defense's loader parents a folder to game.ServerStorage on either side.
    Harness h;
    h.game->assignUniverse(123456005);
    h.run(R"(
        local folder = Instance.new("Folder")
        folder.Parent = game.ServerStorage
        print(folder.Parent.ClassName, game.ServerScriptService.ClassName, #game.ServerStorage:GetChildren())
        print(string.format("%d", game.GameId))
    )");
    CHECK_EQ(h.lines.size(), std::size_t{2});
    CHECK_EQ(h.lines[0], "ServerStorage ServerScriptService 1");
    CHECK_EQ(h.lines[1], "123456005");
}
