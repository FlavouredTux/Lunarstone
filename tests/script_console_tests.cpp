// The Escape menu's Luau tab: code run from there has every capability and
// the globals an executor offers, while the game's own scripts keep having
// none of them and none of those globals.

#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/script/PlaySession.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace lunar;

namespace {

// What every console run prints, one line each.
std::vector<std::string> printed(const Harness& h)
{
    std::vector<std::string> lines;
    for (const std::string& line : h.lines)
        if (!line.starts_with("warn: ") && !line.starts_with("error: "))
            lines.push_back(line);
    return lines;
}

} // namespace

// The capabilities the API dump says a member needs: what a game script cannot
// reach, and console code can.
TEST(script_console_reaches_what_game_scripts_cannot)
{
    Harness h;
    CHECK(h.context.setPlatform("WiiU"));
    h.run(R"lua(
        local function failure(f)
            local ok, e = pcall(f)
            assert(not ok)
            return e
        end
        assert(failure(function() return game:GetService("UserInputService"):GetPlatform() end) ==
            "The current thread cannot call 'GetPlatform' (lacking capability RobloxScript)")
        assert(failure(function() return workspace.RobloxLocked end) ==
            "The current thread cannot read 'RobloxLocked' (lacking capability Plugin)")
        assert(failure(settings) ==
            "The current thread cannot call 'settings' (lacking capability Plugin)")
    )lua");
    h.console(R"lua(
        assert(game:GetService("UserInputService"):GetPlatform() == Enum.Platform.WiiU)
        -- RobloxLocked is a member the engine doesn't model: a false, but read.
        assert(workspace.RobloxLocked == false)
        assert(settings().ClassName == "GlobalSettings")
        assert(getidentity() == 8)
    )lua");
}

TEST(script_game_scripts_cannot_see_the_consoles_globals)
{
    Harness h;
    h.console("assert(getgenv and getrenv and getidentity and getinstances and checkcaller())");
    // Nothing here is a global of the game's scripts, which run as 2.
    h.run(R"lua(
        for _, name in ipairs({"getgenv", "getrenv", "getreg", "getidentity", "setidentity", "getinstances",
                               "getnilinstances", "getscripts", "getloadedmodules", "getsenv", "getcallingscript",
                               "getconnections", "fireproximityprompt", "fireclickdetector", "firetouchinterest",
                               "checkcaller", "identifyexecutor", "loadstring", "getrawmetatable", "setrawmetatable",
                               "setreadonly", "isreadonly"}) do
            assert(_G[name] == nil and getfenv().name == nil, name)
        end
        assert(getidentity == nil and checkcaller == nil)
    )lua");
}

TEST(script_getgenv_keeps_what_an_earlier_run_wrote)
{
    Harness h;
    h.console("getgenv().runs = (getgenv().runs or 0) + 1");
    h.console("getgenv().runs = getgenv().runs + 1 print(getgenv().runs, getgenv() == getgenv())");
    CHECK_EQ(printed(h), (std::vector<std::string>{"2 true"}));
    // The game's own globals are a different table, and untouched by that.
    h.run(R"lua(
        assert(getfenv().runs == nil)
        assert(_G.runs == nil)
    )lua");
}

TEST(script_getrenv_is_the_environments_games_share)
{
    Harness h;
    h.console(R"lua(
        assert(getrenv().game == game)
        assert(getrenv().print == print and getrenv().task == task)
        assert(getgenv() ~= getrenv() and getrenv() ~= getfenv())
        -- What's written there the console reads too.
        local warnWas = warn
        getrenv().warn = "Hello!"
        assert(type(warn) == "string", "the console's warn is not the game's")
        getrenv().warn = warnWas
        getrenv().game = nil
        assert(game == nil, "the console's game is not the game's")
        getrenv().game = getgenv().cloneref(getrenv().workspace.Parent)
    )lua");
}

// getgenv() is every console run's; a run's own globals are its own.
TEST(script_getgenv_is_shared_and_a_runs_globals_are_its_own)
{
    Harness h;
    h.console(R"lua(
        getgenv().dummy_val = "value"
        getfenv().dummy_val_2 = 1
        assert(dummy_val == "value" and getgenv().dummy_val_2 == nil)
        getgenv().dummy_val = "value2"
        dummy_val = nil
        assert(dummy_val == "value2", "the run's nil hides getgenv()'s")
        mine = true
    )lua");
    h.console(R"lua(
        assert(dummy_val == "value2", "another run doesn't share getgenv()")
        assert(mine == nil, "another run's global leaked into this one")
        assert(getgenv().getgenv == getgenv and getgenv()._G == _G)
    )lua");
}

TEST(script_setidentity_takes_the_capabilities_away_again)
{
    Harness h;
    CHECK(h.context.setPlatform("WiiU"));
    h.console(R"lua(
        assert(getidentity() == 8)
        setidentity(2)
        assert(getidentity() == 2)
        local ok, e = pcall(function() return game:GetService("UserInputService"):GetPlatform() end)
        assert(not ok and e == "The current thread cannot call 'GetPlatform' (lacking capability RobloxScript)")
        -- What the thread goes on to spawn runs as the thread does now.
        task.spawn(function()
            assert(getidentity() == 2)
            assert(not pcall(settings))
        end)
        -- The levels between have some: a plugin's has Plugin but not
        -- RobloxScript, and only Full has them all.
        setidentity(5)
        assert(settings().ClassName == "GlobalSettings")
        assert(not pcall(function() return game:GetService("UserInputService"):GetPlatform() end))
        setidentity(0)
        assert(not pcall(settings))
        setidentity(8)
        assert(settings().ClassName == "GlobalSettings")
        -- A number that isn't a level is taken, with none of them.
        setidentity(math.huge)
        assert(not pcall(settings))
        setidentity(9)
        assert(getidentity() == 9 and not pcall(settings))
        setidentity(8)
        -- Only the highest makes a Player [sUNC].
        local player = Instance.new("Player")
        assert(typeof(player) == "Instance" and player.ClassName == "Player", "8 made no Player")
        setidentity(6)
        assert(not pcall(Instance.new, "Player"), "6 made a Player")
        setidentity(8)
    )lua");
    // Nor does a game script.
    CHECK_EQ(h.error("Instance.new('Player')"), "Unable to create an Instance of type \"Player\"");
}

TEST(script_console_threads_keep_its_access)
{
    // What a console run spawns runs as the run did: task.spawn, a coroutine,
    // a render step binding and the thread a connection's handler runs in.
    Harness h;
    h.console(R"lua(
        getgenv().seen = {}
        local seen = getgenv().seen
        task.spawn(function() table.insert(seen, "task.spawn " .. getidentity()) end)
        local co = coroutine.create(function() table.insert(seen, "coroutine " .. getidentity()) end)
        coroutine.resume(co)
        game:GetService("RunService"):BindToRenderStep("Console", 100, function()
            table.insert(seen, "render step " .. getidentity())
        end)
        game:GetService("RunService").Heartbeat:Connect(function()
            table.insert(seen, "connection " .. getidentity())
        end)
        table.insert(seen, "console " .. getidentity())
    )lua");
    h.steps(1);
    h.console("print(table.concat(getgenv().seen, ', '))");
    // task.spawn runs at once, before the line that spawned it.
    CHECK_EQ(printed(h), (std::vector<std::string>{
                              "task.spawn 8, coroutine 8, console 8, render step 8, connection 8"}));
}

TEST(script_console_threads_keep_its_globals)
{
    // The thread a handler runs in has the globals of the code that set it
    // up, as the engine makes it from the connecting thread: getfenv(0) in a
    // console run's handler reaches the executor's globals (Myriad looks its
    // aliases up that way from its Run button's click), a game script's its
    // own.
    Harness h;
    h.run(R"lua(
        scriptMarker = "game"
        local event = Instance.new("BindableEvent", workspace)
        event.Name = "GameEvent"
        event.Event:Connect(function()
            local env = getfenv(0)
            workspace:SetAttribute("GameSaw", tostring(env.scriptMarker) .. " " .. tostring(env.getidentity == nil))
        end)
    )lua");
    h.console(R"lua(
        getgenv().seen = {}
        local seen = getgenv().seen
        local function note(what)
            local env = getfenv(0)
            table.insert(seen, what .. " " .. tostring(env.getidentity ~= nil and env ~= getrenv()))
        end
        local event = Instance.new("BindableEvent")
        event.Event:Once(function() note("once") end)
        event.Event:Connect(function() note("connection") end)
        event:Fire()
        game:GetService("RunService"):BindToRenderStep("Globals", 100, function() note("render step") end)
        local invoked = Instance.new("BindableFunction")
        invoked.OnInvoke = function() note("callback") end
        invoked:Invoke()
        workspace.GameEvent:Fire()
    )lua");
    h.steps(1);
    h.console("print(table.concat(getgenv().seen, ', '), workspace:GetAttribute('GameSaw'))");
    CHECK_EQ(printed(h), (std::vector<std::string>{
                              "once true, connection true, callback true, render step true game true"}));
}

TEST(script_console_lists_the_render_step_bindings)
{
    Harness h;
    h.console(R"lua(
        local RunService = game:GetService("RunService")
        local function late() end
        RunService:BindToRenderStep("Late", 300, late)
        RunService:BindToRenderStep("Early", 100, function() end)
        local list = getrendersteppedlist()
        assert(#list == 2 and list[1].Name == "Early" and list[2].Name == "Late", "not in the order they run")
        assert(list[2].Priority == 300 and list[2].Function == late, "not the binding")
        assert(list[2].Thread == coroutine.running(), "not the thread that bound it")
        RunService:UnbindFromRenderStep("Late")
        list = getrendersteppedlist()
        assert(#list == 1 and list[1].Name == "Early", "the unbound one is still there")
    )lua");
}

TEST(script_console_knows_what_signals_pass)
{
    Harness h;
    h.console(R"lua(
        local part = Instance.new("Part")
        local args = getsignalarguments(part.Touched)
        assert(#args == 1 and args[1] == "BasePart", table.concat(args, ","))
        local info = getsignalargumentsinfo(part.Touched)
        assert(info[1].Name == "otherPart" and info[1].Type == "BasePart", "not Touched's parameter")
        -- An inherited event, one with several, and one that passes nothing.
        assert(getsignalarguments(part.ChildAdded)[1] == "Instance", "not ChildAdded's")
        local changed = getsignalargumentsinfo(game:GetService("UserInputService").InputBegan)
        assert(#changed == 2 and changed[2].Name == "gameProcessedEvent" and changed[2].Type == "bool", "not InputBegan's")
        assert(#getsignalarguments(part:GetPropertyChangedSignal("Size")) == 0, "a property's signal passes nothing")
        assert(not pcall(getsignalarguments, part), "a part is no signal")
    )lua");
}

TEST(script_console_signals_of_its_own_fire_now)
{
    Harness h;
    h.console(R"lua(
        local sig = Signal.new()
        local got
        local c = sig:Connect(function(...) got = table.pack(...) end)
        sig:Fire("a", 1, nil)
        assert(got and got.n == 3 and got[1] == "a" and got[2] == 1, "not fired now")
        c:Disconnect()
        got = nil
        sig:Fire()
        assert(got == nil, "fired after Disconnect")
        local once = 0
        sig:Once(function() once += 1 end)
        sig:Fire() sig:Fire()
        assert(once == 1, "Once ran " .. once)
        task.spawn(function() getgenv().waited = table.pack(sig:Wait()) end)
        sig:Fire("w", 2)
        -- The engine's own signals have no Fire.
        assert(not pcall(function() return Instance.new("Part").Touched.Fire end))
        -- firesignal wakes what waits on an engine signal too.
        local event = Instance.new("BindableEvent")
        task.spawn(function() getgenv().bindable = table.pack(event.Event:Wait()) end)
        firesignal(event.Event, "x")
    )lua");
    h.steps(1);
    h.console(R"lua(
        local w, b = getgenv().waited, getgenv().bindable
        assert(w and w[1] == "w" and w[2] == 2, "Wait didn't get the fire")
        assert(b and b.n == 1 and b[1] == "x", "firesignal didn't wake the Wait")
    )lua");
}

TEST(script_console_callbacks_run_as_whoever_set_them)
{
    // A callback (OnInvoke, OnClientInvoke...) runs as the thread that set it,
    // as a connection's handler runs as its connector: sUNC answers the
    // server from an OnClientInvoke of its own. One a game script set stays
    // the game's when the console invokes it.
    Harness h;
    h.run(R"lua(
        local game_set = Instance.new("BindableFunction", workspace)
        game_set.Name = "GameSet"
        game_set.OnInvoke = function() return getfenv(0).checkcaller == nil, #tostring(game_set) > 0 end
    )lua");
    h.console(R"lua(
        local console_set = Instance.new("BindableFunction")
        console_set.OnInvoke = function() return checkcaller(), getidentity() end
        local console, identity = console_set:Invoke()
        assert(console == true and identity == 8, "console callback ran as " .. tostring(identity))
        local blind = workspace.GameSet:Invoke()
        assert(blind == true, "the game's callback saw the console's globals")
    )lua");
}

TEST(script_getnilinstances_finds_what_nothing_parents)
{
    Harness h;
    h.console("getgenv().leak = Instance.new('Part') getgenv().leak.Name = 'Leak'");;
    h.console(R"lua(
        local names = {}
        for _, instance in ipairs(getnilinstances()) do
            table.insert(names, instance.Name)
        end
        table.sort(names)
        print(table.concat(names, ","))
    )lua");
    // The game itself has no parent either.
    CHECK_EQ(printed(h), (std::vector<std::string>{"DataModel,Leak"}));
    // Parented, it is no longer loose.
    h.console("getgenv().leak.Parent = workspace");
    h.lines.clear();
    h.console(R"lua(
        local names = {}
        for _, instance in ipairs(getnilinstances()) do table.insert(names, instance.Name) end
        print(table.concat(names, ","))
    )lua");
    CHECK_EQ(printed(h), (std::vector<std::string>{"DataModel"}));
}

TEST(script_console_lists_the_games_scripts_and_modules)
{
    Harness h;
    auto folder = std::make_shared<Folder>();
    folder->setName("ReplicatedStorageScripts");
    folder->setParent(h.game->getService("ReplicatedStorage"));
    auto module = std::make_shared<ModuleScript>();
    module->setName("Util");
    module->setSource("return 7");
    module->setParent(folder.get());
    h.console("print(getgenv().value == nil) require(game.ReplicatedStorage.ReplicatedStorageScripts.Util)");
    h.console(R"lua(
        local names = {}
        for _, script in ipairs(getscripts()) do
            table.insert(names, script.ClassName .. ":" .. script.Name)
        end
        table.sort(names)
        print(table.concat(names, " "))
        local loaded = {}
        for _, script in ipairs(getloadedmodules()) do
            table.insert(loaded, script.Name)
        end
        print(#loaded, loaded[1])
    )lua");
    CHECK(printed(h).size() >= 2);
    const std::vector<std::string> lines = printed(h);
    CHECK_EQ(lines[0], "true");
    CHECK(lines[1].find("ModuleScript:Util") != std::string::npos);
    CHECK_EQ(lines[2], "1 Util");
    // One a script made and never parented counts, a core script doesn't [sUNC].
    auto core = std::make_shared<LocalScript>();
    core->setName("Core");
    core->setParent(h.game->getService("CoreGui"));
    h.console(R"lua(
        local loose = Instance.new("LocalScript")
        local client = Instance.new("Script")
        client.RunContext = Enum.RunContext.Client
        local server = Instance.new("Script")
        local found = {}
        for _, script in getscripts() do found[script] = true end
        assert(found[loose], "the loose LocalScript is missing")
        assert(found[client], "the client Script is missing")
        assert(not found[server], "a server Script is listed")
        assert(not found[game.CoreGui.Core], "a core script is listed")
    )lua");
}

TEST(script_the_client_idles_and_listens_for_it_itself)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setUserId(42);
    player->setParent(game->getService<Players>());
    ClientSession client(game);
    std::vector<std::string> lines;
    client.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(client.start(42));
    // The client's own connection on Idled, the idle kick's: a C connection,
    // as sUNC's getconnections looks for.
    const std::string check = R"lua(
        local connections = getconnections(game.Players.LocalPlayer.Idled)
        local c = connections[#connections]
        assert(c, "no connection on Idled")
        assert(c.LuaConnection == false and c.Function == nil and c.Thread == nil, "not a C connection")
        assert(c.ForeignState == true and c.Enabled == true, "not the engine's")
        game.Players.LocalPlayer.Idled:Connect(function(time) print("idled", math.floor(time)) end)
        assert(getconnections(game.Players.LocalPlayer.Idled)[1].LuaConnection, "the script's isn't first")
    )lua";
    const auto ran = client.client().runConsole(check);
    CHECK_EQ(ran ? std::string() : ran.error().message, std::string());
    // Two minutes without input, then each half minute.
    for (int i = 0; i < 151; ++i)
        client.step(1.0);
    std::vector<std::string> idled;
    std::ranges::copy_if(lines, std::back_inserter(idled), [](const std::string& l) { return l.starts_with("idled"); });
    CHECK_EQ(idled, (std::vector<std::string>{"idled 120", "idled 150"}));
    client.input().keyDown(Enum::KeyCode::W);
    client.step(1.0);
    client.input().keyUp(Enum::KeyCode::W);
    for (int i = 0; i < 119; ++i)
        client.step(1.0);
    CHECK_EQ(std::ranges::count_if(lines, [](const std::string& l) { return l.starts_with("idled"); }), 2);
    // At twenty minutes the client's own handler kicks.
    for (int i = 0; i < 20 * 60; ++i)
        client.step(1.0);
    CHECK(std::ranges::any_of(lines, [](const std::string& l) { return l.contains("idle 20 minutes"); }));
}

TEST(script_a_client_takes_a_server_callback_only_out_of_its_game)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setUserId(42);
    player->setParent(game->getService<Players>());
    ClientSession client(game);
    CHECK(client.start(42));
    // The client refuses OnServerInvoke on a remote in its game, and takes it
    // on one in no game at all, as the client's setter checks.
    const auto ran = client.client().runConsole(R"lua(
        local loose = Instance.new("RemoteFunction")
        loose.OnServerInvoke = function(player) return player end
        assert(getcallbackvalue(loose, "OnServerInvoke")(1) == 1, "the loose remote didn't keep it")
        local placed = Instance.new("RemoteFunction")
        placed.Parent = game:GetService("ReplicatedStorage")
        local ok, message = pcall(function() placed.OnServerInvoke = function() end end)
        assert(not ok and message:find("OnServerInvoke can only be implemented on the server"), message)
        assert(getcallbackvalue(placed, "OnServerInvoke") == nil, "the refused one was kept")
        -- Moving one with a callback in doesn't drop it: only setting it is checked.
        loose.Parent = game:GetService("ReplicatedStorage")
        assert(getcallbackvalue(loose, "OnServerInvoke") ~= nil, "moving it in dropped it")
    )lua");
    CHECK_EQ(ran ? std::string() : ran.error().message, std::string());
}

TEST(script_the_clients_backpack_listens_on_the_backpack)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setUserId(42);
    player->setParent(game->getService<Players>());
    ClientSession client(game);
    CHECK(client.start(42));
    // The server's Backpack comes after the client started, as it does live.
    auto backpack = std::make_shared<Backpack>();
    backpack->setParent(player);
    const auto ran = client.client().runConsole(R"lua(
        local backpack = game.Players.LocalPlayer.Backpack
        for _, event in {"ChildAdded", "ChildRemoved"} do
            local c = getconnections(backpack[event])[1]
            assert(c, "nothing on " .. event)
            assert(c.ForeignState == true and c.LuaConnection == false and c.Function == nil, event .. " isn't foreign")
        end
    )lua");
    CHECK_EQ(ran ? std::string() : ran.error().message, std::string());
}

TEST(script_connecting_touched_puts_a_touch_interest_in)
{
    Harness h;
    auto gui = std::make_shared<SurfaceGui>();
    gui->setParent(h.game->workspace());
    auto pad = std::make_shared<Part>();
    pad->setName("Pad");
    pad->setParent(gui);
    CHECK_EQ(h.output(R"lua(
        local pad = workspace.SurfaceGui.Pad
        print(pad:FindFirstChildOfClass("TouchTransmitter"))
        pad.Touched:Connect(function() end)
        local interest = pad:FindFirstChildOfClass("TouchTransmitter")
        print(interest and interest.Name)
        pad.TouchEnded:Connect(function() end)
        print(#pad:GetChildren())
    )lua"),
             (std::vector<std::string>{"nil", "TouchInterest", "1"}));
}

TEST(script_getconnections_hands_back_what_the_game_connected)
{
    Harness h;
    auto pad = std::make_shared<Part>();
    pad->setName("Pad");
    pad->setParent(h.game->workspace());
    h.run(R"lua(
        _G.hits = 0
        workspace.Pad.ChildAdded:Connect(function() _G.hits = _G.hits + 10 end)
        workspace.Pad.ChildAdded:Connect(function(child) _G.hits = _G.hits + 1 end)
    )lua");
    h.console(R"lua(
        local _G = getrenv()._G -- the game's, not the console's own
        -- Newest first: [1] is the one that adds one.
        local connections = getconnections(workspace.Pad.ChildAdded)
        print(#connections, connections[1].Enabled, type(connections[1].Function))
        -- Fire runs its own handler only, with the child as its argument,
        -- called with a colon as the docs call it or as a plain field.
        local child = Instance.new("Part")
        connections[1]:Fire(child)
        print("fired", _G.hits)
        -- Disabled, it stays connected and its handler stops.
        connections[1]:Disable()
        print("enabled", connections[1].Enabled)
        connections[1].Fire(child)
        print("disabled", _G.hits)
        connections[1]:Enable()
        connections[1]:Disconnect()
        connections[1]:Fire(child)
        print("gone", _G.hits, #getconnections(workspace.Pad.ChildAdded))
        -- firesignal runs every connection left: the first adds ten.
        firesignal(workspace.Pad.ChildAdded, child)
        print("signal", _G.hits)
    )lua");
    CHECK_EQ(printed(h), (std::vector<std::string>{"2 true function", "fired 1", "enabled false", "disabled 1",
                                                  "gone 1 1", "signal 11"}));
}

// A thread in :Wait() holds the engine's own connection on the signal until
// the fire it waits for: a Luau one with that thread and no function, which
// the console can turn off.
TEST(script_getconnections_lists_a_threads_wait)
{
    Harness h;
    h.console(R"lua(
        local folder = Instance.new("Folder")
        local got
        local waiter = task.spawn(function() got = folder.ChildAdded:Wait() end)
        local c = getconnections(folder.ChildAdded)[1]
        print(c.ForeignState, c.LuaConnection, c.Function, c.Thread == waiter, c.Enabled)
        c:Disable()
        Instance.new("Part", folder)
        print("disabled", got)
        c:Enable()
        local part = Instance.new("Part", folder)
        print("woken", got == part, #getconnections(folder.ChildAdded))
    )lua");
    CHECK_EQ(printed(h),
             (std::vector<std::string>{"false true nil true true", "disabled nil", "woken true 0"}));
}

TEST(script_console_fires_events_the_engine_would)
{
    Harness h;
    h.console(R"lua(
        local seen = {}
        local pad, other = Instance.new("Part"), Instance.new("Part")
        pad.Name, other.Name = "Pad", "Other"
        pad.Touched:Connect(function(hit) table.insert(seen, "touched " .. hit.Name) end)
        other.Touched:Connect(function() table.insert(seen, "touched too") end)
        firetouchinterest(pad, other, 0)
        firetouchinterest(pad, other, 1) -- TouchEnded, which nothing listens to here
        -- A boolean is the number it stands for: false touches, true ends.
        pad.TouchEnded:Connect(function() table.insert(seen, "ended") end)
        firetouchinterest(pad, other, false)
        firetouchinterest(pad, other, true)
        local detector = Instance.new("ClickDetector")
        detector.Parent = other
        detector.MouseClick:Connect(function() table.insert(seen, "clicked") end)
        fireclickdetector(detector)
        local prompt = Instance.new("ProximityPrompt")
        prompt.Parent = other
        prompt.Triggered:Connect(function(who) table.insert(seen, "prompted " .. tostring(who)) end)
        fireproximityprompt(prompt)
        print(table.concat(seen, ", "))
        print(getcallingscript(), checkcaller(), identifyexecutor())
    )lua");
    const std::vector<std::string> lines = printed(h);
    CHECK_EQ(lines[0], "touched Other, touched too, touched Other, touched too, ended, clicked, prompted nil");
    CHECK_EQ(lines[1], "nil true Lunarstone 0.1.0");
}

TEST(script_loadstring_and_the_table_helpers)
{
    Harness h;
    h.console(R"lua(
        local f = loadstring("return 1 + 1")
        assert(f() == 2 and type(f) == "function")
        -- Like Lua's own: nil and the compiler's message, not an error.
        local none, message = loadstring("local = 5")
        assert(none == nil and type(message) == "string")
        -- The chunk name is Lua's: "=name" reads as name, anything else as
        -- [string "..."]; the compiler's messages start with it as well.
        assert(debug.info(loadstring("return 1", "=MYRIAD"), "s") == "MYRIAD")
        assert(debug.info(loadstring("return 1", "Plain"), "s") == '[string "Plain"]')
        assert(debug.info(loadstring("return 1"), "s") == "Console")
        local _, incomplete = loadstring("MYRIAD", "=MYRIAD")
        assert(incomplete == "MYRIAD:1: Incomplete statement: expected assignment or a function call", incomplete)
        local t = setmetatable({}, {__index = function() return "raw" end})
        assert(getrawmetatable(t).__index())
        setrawmetatable(t, nil)
        assert(getrawmetatable(t) == nil)
        assert(not isreadonly({}), "fresh table is readonly")
        local locked = {}
        setreadonly(locked, true)
        assert(isreadonly(locked))
        local made = {}
        makereadonly(made)
        assert(isreadonly(made) and not pcall(function() made.x = 1 end), "makereadonly")
        makewriteable(made)
        assert(not isreadonly(made), "makewriteable")
        assert(type(getreg()) == "table")
    )lua");
}

TEST(script_console_runs_on_the_server_where_there_is_one)
{
    // Play Solo: the menu's Luau tab runs on the client, or on the server where
    // a first line of --!server asks for it.
    auto game = DataModel::create();
    PlaySession session(game);
    std::vector<std::string> lines;
    session.setOutput([&lines](ScriptRole role, MessageType type, std::string_view text) {
        if (type != MessageType::Error)
            lines.push_back(std::format("{}: {}", role == ScriptRole::Server ? "server" : "client", text));
    });
    session.start();
    const std::string where = R"(print('where', game:GetService("RunService"):IsServer()))";
    CHECK(session.client().runConsole(where).has_value());
    CHECK(session.client().runConsole("--!server\n" + where, &session.server()).has_value());
    // Without a server to run on, the console says so.
    CHECK_EQ(session.server().runConsole("--!server\nprint('nowhere')").error().message,
             "There is no server here to run on.");
    CHECK_EQ(session.server().runConsole(where).has_value(), true);
    CHECK(std::ranges::contains(lines, std::string("client: where false")));
    CHECK(std::ranges::contains(lines, std::string("server: where true")));
}

// BasePart:GetRootPart() is the assembly's root, as the physics picks it:
// the camera's Popper keeps the subject's own parts from occluding by it.
TEST(script_get_root_part_is_the_assemblys_root)
{
    Harness h;
    h.console(R"lua(
        local model = Instance.new("Model", workspace)
        local root = Instance.new("Part", model)
        root.Name = "HumanoidRootPart"
        root.Size = Vector3.new(2, 2, 1)
        local torso = Instance.new("Part", model)
        torso.Size = Vector3.new(4, 4, 4)
        local hat = Instance.new("Part", model)
        local weld = Instance.new("Weld", torso)
        weld.Part0, weld.Part1 = root, torso
        local glue = Instance.new("WeldConstraint", hat)
        glue.Part0, glue.Part1 = torso, hat
        assert(hat:GetRootPart() == root, "not the HumanoidRootPart")
        assert(torso:GetRootPart() == root, "not the HumanoidRootPart")
        local loose = Instance.new("Part", workspace)
        assert(loose:GetRootPart() == loose, "a loose part is its own root")
        hat.Anchored = true
        assert(root:GetRootPart() == hat, "an anchored part wins")
        weld.Enabled = false
        assert(root:GetRootPart() == root, "a disabled weld holds nothing")
    )lua");
}
