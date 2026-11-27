#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/ui/CoreGui.hpp>
#include <lunar/ui/Notifications.hpp>

using namespace lunar;

TEST(core_gui_setcore_reads_and_writes_native_settings)
{
    Harness h;
    h.run(R"(
        local gui = game:GetService("StarterGui")
        print(gui:GetCore("TopbarEnabled"), gui:GetCore("ChatActive"))
        gui:SetCore("TopbarEnabled", false)
        gui:SetCore("ChatActive", false)
        gui:SetCore("ChatBarDisabled", true)
        gui:SetCore("ChatWindowPosition", UDim2.fromOffset(25, 50))
        gui:SetCore("ChatWindowSize", UDim2.fromOffset(320, 240))
        print(gui:GetCore("TopbarEnabled"), gui:GetCore("ChatActive"), gui:GetCore("ChatBarDisabled"))
        print(gui:GetCore("ChatWindowPosition").X.Offset, gui:GetCore("ChatWindowSize").Y.Offset)
        gui:SetCore("ChatMakeSystemMessage", {Text="hello", Color=Color3.fromRGB(12, 34, 56)})
        local reset = Instance.new("BindableEvent")
        gui:SetCore("ResetButtonCallback", reset)
        print(gui:GetCore("ResetButtonCallback") == reset)
        gui:SetCore("ResetButtonCallback", false)
        print(gui:GetCore("ResetButtonCallback"))
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"true true", "false false true", "25 240", "true", "false"}));
    auto* starter = h.game->getService<StarterGui>();
    const auto messages = starter->takeSystemChat();
    CHECK_EQ(messages.size(), 1u);
    CHECK_EQ(messages.front().text, "hello");
    CHECK(messages.front().color == Color3::fromRGB(12, 34, 56));
    CHECK(starter->takeSystemChat().empty());
}

TEST(core_gui_registered_callbacks_take_precedence_and_preserve_tables)
{
    Harness h;
    h.context.setCoreScriptAccess();
    h.run(R"(
        local gui = game:GetService("StarterGui")
        local saved
        gui:RegisterSetCore("Example", function(value) saved = value end)
        gui:RegisterGetCore("Example", function() return saved end)
        gui:SetCore("Example", {enabled=true, count=7})
        local value = gui:GetCore("Example")
        print(value.enabled, value.count)
        print(pcall(function() gui:SetCore("MissingSetting", true) end))
    )");
    CHECK_EQ(h.lines.front(), "true 7");
    CHECK(h.lines.back().starts_with("false"));
}

TEST(core_gui_enable_and_menu_events_reach_game_scripts)
{
    Harness h;
    h.run(R"(
        local starter = game:GetService("StarterGui")
        starter.CoreGuiChangedSignal:Connect(function(kind, enabled)
            print(kind.Name, enabled)
        end)
        local gui = game:GetService("GuiService")
        gui.MenuOpened:Connect(function() print("opened", gui.MenuIsOpen) end)
        gui.MenuClosed:Connect(function() print("closed", gui.MenuIsOpen) end)
        starter:SetCoreGuiEnabled(Enum.CoreGuiType.PlayerList, false)
        starter:SetCoreGuiEnabled(Enum.CoreGuiType.PlayerList, false)
    )");
    auto* gui = h.game->getService<GuiService>();
    gui->assignMenuIsOpen(true);
    gui->assignMenuIsOpen(true);
    gui->assignMenuIsOpen(false);
    h.context.step(0.0);
    CHECK_EQ(h.lines.size(), 3u);
    CHECK_EQ(h.lines.front(), "PlayerList false");
    CHECK(h.lines[1].starts_with("opened"));
    CHECK(h.lines[2].starts_with("closed"));
}

TEST(core_gui_desktop_preferred_input_and_disabled_reset)
{
    Harness h;
    h.run(R"(
        local input = game:GetService("UserInputService")
        print(input.PreferredInput == Enum.PreferredInput.KeyboardAndMouse, input.TouchEnabled)
        game:GetService("StarterGui"):SetCore("ResetButtonCallback", false)
    )");
    CHECK_EQ(h.lines.back(), "true false");
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::CoreGui core(fonts, atlas);
    core.setGame(h.game.get());
    CHECK(!core.resetEnabled());
    core.menu().openDialog(ui::GameMenu::Dialog::Reset);
    CHECK(core.menu().dialog() == ui::GameMenu::Dialog::None);
}

TEST(core_gui_console_visibility_round_trips_scripts_and_native_input)
{
    Harness h;
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::CoreGui core(fonts, atlas);
    core.setGame(h.game.get());
    h.run(R"(game:GetService("StarterGui"):SetCore("DevConsoleVisible", true))");
    core.update(0, {1280, 720}, 58, 1);
    CHECK(core.menuOpen());
    CHECK(core.menu().tab() == ui::GameMenu::Tab::Luau);
    core.handle(platform::KeyEvent{.key = platform::Key::Escape, .pressed = true});
    h.run(R"(print(game:GetService("StarterGui"):GetCore("DevConsoleVisible")))");
    CHECK_EQ(h.lines.back(), "false");
    core.handle(platform::KeyEvent{.key = platform::Key::F9, .pressed = true});
    CHECK(core.menuOpen());
    CHECK(core.menu().tab() == ui::GameMenu::Tab::Luau);
    h.run(R"(print(game:GetService("StarterGui"):GetCore("DevConsoleVisible")))");
    CHECK_EQ(h.lines.back(), "true");
    h.run(R"(game:GetService("StarterGui"):SetCore("DevConsoleVisible", false))");
    core.update(1, {1280, 720}, 58, 1);
    CHECK(!core.menuOpen());
}

TEST(core_gui_modern_topbar_controls_and_safe_area)
{
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::Topbar bar(fonts, atlas);
    bar.update({2560, 1440}, 2.0f);
    int menu = 0, chat = 0, players = 0;
    bar.onMenu = [&] { ++menu; };
    bar.onChat = [&] { ++chat; };
    bar.onPlayerList = [&] { ++players; };
    auto click = [&](ui::Topbar::Button button) {
        const auto rect = bar.buttonRect(button);
        const double x = static_cast<double>((rect.x + rect.w / 2) / 2.0f);
        const double y = static_cast<double>((rect.y + rect.h / 2) / 2.0f);
        CHECK(bar.handle(
            platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y}));
        CHECK(bar.handle(
            platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y}));
    };
    CHECK_EQ(bar.safeArea().height(), ui::Topbar::height);
    CHECK(bar.safeArea().min.x > bar.buttonRect(ui::Topbar::Button::Chat).right() / 2.0f);
    click(ui::Topbar::Button::Roblox);
    click(ui::Topbar::Button::Chat);
    click(ui::Topbar::Button::Controls);
    CHECK(bar.controlsOpen());
    click(ui::Topbar::Button::PlayerList);
    CHECK(!bar.controlsOpen());
    CHECK(menu == 1 && chat == 1 && players == 1);
    bar.enabled = false;
    bar.update({2560, 1440}, 2.0f);
    CHECK_EQ(bar.safeArea().height(), 0.0f);
    CHECK(bar.buttonRect(ui::Topbar::Button::Roblox).w == 0.0f);
}

TEST(core_gui_screen_insets_and_ignore_flag_are_the_same_native_state)
{
    auto gui = std::make_shared<ScreenGui>();
    CHECK(!gui->ignoreGuiInset());
    gui->setIgnoreGuiInset(true);
    CHECK(gui->screenInsets() == Enum::ScreenInsets::DeviceSafeInsets);
    gui->setScreenInsets(Enum::ScreenInsets::TopbarSafeInsets);
    CHECK(gui->ignoreGuiInset());
    gui->setIgnoreGuiInset(false);
    CHECK(gui->screenInsets() == Enum::ScreenInsets::CoreUISafeInsets);
    gui->setScreenInsets(Enum::ScreenInsets::None);
    CHECK(gui->ignoreGuiInset());
}

TEST(core_gui_live_client_has_the_non_archivable_default_player_module_facade)
{
    auto game = DataModel::create();
    auto* starter = game->getService<StarterPlayer>();
    auto scripts = std::make_shared<StarterPlayerScripts>();
    scripts->setParent(starter);
    auto module = std::make_shared<ModuleScript>();
    module->setName("PlayerModule");
    module->setSource("error('the native camera must not run a duplicate controller')");
    module->setArchivable(false);
    module->setParent(scripts);
    for (const char* name : {"CameraModule", "ControlModule"}) {
        auto child = std::make_shared<ModuleScript>();
        child->setName(name);
        child->setParent(module);
    }
    auto lock = std::make_shared<ModuleScript>();
    lock->setName("MouseLockController");
    lock->setSource("return {nested = true}");
    lock->setArchivable(false);
    lock->setParent(module->findFirstChild("CameraModule"));
    auto keys = std::make_shared<StringValue>();
    keys->setName("BoundKeys");
    keys->setValue("LeftShift");
    keys->setParent(lock);
    auto player = std::make_shared<Player>();
    player->setUserId(5);
    player->setParent(game->getService<Players>());
    ClientSession client(game);
    CHECK(client.start(*player));
    CHECK(client.client()
              .run(R"(
        local scripts = game.Players.LocalPlayer.PlayerScripts
        local playerModule = require(scripts:WaitForChild("PlayerModule"))
        assert(type(playerModule.GetControls) == "function")
        assert(type(playerModule:GetControls().Enable) == "function")
        assert(playerModule.controls == playerModule:GetControls())
        assert(playerModule.cameras == playerModule:GetCameras())
        local controller = playerModule.cameras.activeMouseLockController
        local toggled = 0
        controller:GetBindableToggleEvent():Connect(function() toggled += 1 end)
        controller:OnMouseLockToggled()
        assert(controller:GetIsMouseLocked())
        assert(game:GetService("UserInputService").MouseBehavior == Enum.MouseBehavior.LockCenter)
        controller:OnMouseLockToggled()
        assert(not controller:GetIsMouseLocked() and toggled == 2)
        local lock = scripts.PlayerModule.CameraModule:WaitForChild("MouseLockController")
        assert(lock.BoundKeys.Value == "LeftShift")
        assert(require(lock).nested)
    )",
                   "PlayerModuleCompat")
              .has_value());
    CHECK(!module->archivable());
    CHECK(!lock->archivable());
}

TEST(core_gui_is_there_by_name_once_a_client_starts)
{
    // The server never sends CoreGui; the client has it before any script
    // runs, so game.CoreGui works with nothing having asked for it (sUNC's
    // check indexes it so), live and in Play Solo alike.
    const char* byName = R"(assert(game.CoreGui == game:GetService("CoreGui"), "no CoreGui by name"))";
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setUserId(5);
    player->setParent(game->getService<Players>());
    CHECK(game->findService("CoreGui") == nullptr);
    ClientSession client(game);
    CHECK(client.start(*player));
    CHECK(client.client().run(byName, "CoreGuiByName").has_value());

    auto solo = DataModel::create();
    PlaySession session(solo);
    session.start();
    CHECK(session.client().run(byName, "CoreGuiByName").has_value());
}

TEST(core_gui_client_stats_has_the_data_ping)
{
    // Stats.Network.ServerStatsItem["Data Ping"], where the console reads its
    // ping (sUNC's run ended without it): the connection's round trip in
    // milliseconds, 0 with no server to ask. GetValue is a plugin's to call,
    // so a game script can't.
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setUserId(5);
    player->setParent(game->getService<Players>());
    ClientSession client(game);
    CHECK(client.start(*player));
    const auto console = [&](const char* source) {
        return client.client().run(source, "Console", nullptr, RunAccess::Full).has_value();
    };
    CHECK(console(R"(
        local ping = game:GetService("Stats").Network.ServerStatsItem["Data Ping"]
        assert(ping:IsA("StatsItem"), "not a StatsItem: " .. ping.ClassName)
        assert(ping:GetValue() == 0, "a ping with no server: " .. ping:GetValue())
    )"));
    client.client().setNetworkPing([] { return 0.0625; });
    CHECK(console(R"(
        local ping = game.Stats.Network.ServerStatsItem["Data Ping"]
        assert(ping:GetValue() == 62.5, "not the round trip: " .. ping:GetValue())
        assert(ping:GetValueString() == "62.50 ms", "not its text: " .. ping:GetValueString())
    )"));
    auto script = client.client().run(R"(game.Stats.Network.ServerStatsItem["Data Ping"]:GetValue())", "Script");
    CHECK(!script && script.error().message.contains("lacking capability Plugin"));
}

TEST(core_gui_legacy_user_id_and_sound_pitch_use_real_properties)
{
    Harness h;
    auto player = std::make_shared<Player>();
    player->setName("Player");
    player->setUserId(424242);
    player->setParent(h.game->getService<Players>());
    h.run(R"(
        print(game.Players.Player.userId, game.Players.Player.UserId)
        local sound = Instance.new("Sound")
        sound.Pitch = 1.25
        print(sound.Pitch, sound.PlaybackSpeed)
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"424242 424242", "1.25 1.25"}));
}

TEST(core_gui_position_query_is_sorted_filtered_and_refreshes_after_script_changes)
{
    Harness h;
    auto player = std::make_shared<Player>();
    player->setName("Player");
    player->setParent(h.game->getService<Players>());
    auto playerGui = std::make_shared<PlayerGui>();
    playerGui->setParent(player);
    auto screen = std::make_shared<ScreenGui>();
    screen->setParent(playerGui);
    for (const auto& [name, z] : {std::pair{"Lower", 1}, std::pair{"Upper", 5}}) {
        auto frame = std::make_shared<Frame>();
        frame->setName(name);
        frame->setSize(UDim2::fromOffset(100, 100));
        frame->setZIndex(z);
        frame->setParent(screen);
    }
    auto other = std::make_shared<ScreenGui>();
    other->setDisplayOrder(100);
    other->setParent(h.game->getService<CoreGui>());
    auto coreFrame = std::make_shared<Frame>();
    coreFrame->setName("CoreOnly");
    coreFrame->setSize(UDim2::fromOffset(100, 100));
    coreFrame->setParent(other);
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.settings().topInset = 0;
    h.game->getService<GuiService>()->assignTopbarInset({{0, 0}, {0, 0}});
    gui.setRoots({playerGui.get(), h.game->getService<CoreGui>()});
    h.context.setGuiHitTest([&](float x, float y) {
        gui.update({800, 600}, 0);
        return gui.objectsAt(x, y);
    });
    h.run(R"(
        local root = game.Players.Player.PlayerGui
        local found = root:GetGuiObjectsAtPosition(20, 20)
        print(#found, found[1].Name, found[2].Name)
        found[1].Position = UDim2.fromOffset(300, 300)
        found = root:GetGuiObjectsAtPosition(20, 20)
        print(#found, found[1].Name)
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"2 Upper Lower", "1 Lower"}));
}

TEST(core_gui_inset_area_matches_the_renderer_regions)
{
    Harness h;
    h.game->workspace()->ensureCurrentCamera()->updateViewportSize({1280, 720});
    auto* service = h.game->getService<GuiService>();
    service->assignTopbarInset({{172, 0}, {1264, 58}});
    service->assignGlobalGuiInset({10, 20, 30, 40});
    h.run(R"(
        local service = game:GetService("GuiService")
        local core = service:GetInsetArea(Enum.ScreenInsets.CoreUISafeInsets)
        local topbar = service:GetInsetArea(Enum.ScreenInsets.TopbarSafeInsets)
        local device = service:GetInsetArea(Enum.ScreenInsets.DeviceSafeInsets)
        print(core.Min.X, core.Min.Y, core.Max.X, core.Max.Y)
        print(topbar.Min.X, topbar.Height, device.Width)
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"10 78 1250 680", "172 58 1280"}));
}

TEST(core_gui_queries_and_absolute_positions_share_the_inset_coordinate_system)
{
    Harness h;
    auto* starter = h.game->getService<StarterGui>();
    auto screen = std::make_shared<ScreenGui>();
    screen->setName("Screen");
    screen->setParent(starter);
    auto frame = std::make_shared<Frame>();
    frame->setName("Button");
    frame->setSize(UDim2::fromOffset(100, 100));
    frame->setParent(screen);
    auto* service = h.game->getService<GuiService>();
    service->assignTopbarInset({{172, 0}, {784, 58}});
    service->assignGlobalGuiInset({10, 20, 0, 0});
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.settings().topInset = 58;
    gui.setRoots({starter});
    h.context.setGuiHitTest([&](float x, float y) {
        gui.update({800, 600}, 0);
        return gui.objectsAt(x, y);
    });
    h.run(R"(
        local root = game:GetService("StarterGui")
        local objects = root:GetGuiObjectsAtPosition(20, 20)
        print(#objects, objects[1].AbsolutePosition.X, objects[1].AbsolutePosition.Y)
        root.Screen.IgnoreGuiInset = true
        objects = root:GetGuiObjectsAtPosition(10, -68)
        print(#objects, objects[1].AbsolutePosition.X, objects[1].AbsolutePosition.Y)
    )");
    CHECK_EQ(h.lines, (std::vector<std::string>{"1 0 0", "1 -10 -78"}));
}

TEST(core_gui_notifications_validate_queue_and_invoke_button_callback)
{
    Harness h;
    h.run(R"(
        local callback = Instance.new("BindableFunction")
        callback.OnInvoke = function(button) print("chosen", button) end
        game:GetService("StarterGui"):SetCore("SendNotification", {
            Title = "Inventory", Text = "Trade request", Duration = 8,
            Button1 = "Accept", Button2 = "Decline", Callback = callback,
        })
    )");
    auto pending = h.game->getService<StarterGui>()->takeNotifications();
    CHECK_EQ(pending.size(), 1u);
    CHECK_EQ(pending[0].title, "Inventory");
    CHECK_EQ(pending[0].buttons, (std::vector<std::string>{"Accept", "Decline"}));
    CHECK_EQ(pending[0].duration, 8.0);
    CHECK(h.game->getService<StarterGui>()->takeNotifications().empty());
    pending[0].choose("Decline");
    h.context.step(0);
    CHECK_EQ(h.lines.back(), "chosen Decline");
}

TEST(core_gui_notifications_buttons_and_expiry)
{
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::Notifications notifications(fonts, atlas);
    std::string chosen;
    StarterGui::Notification notification;
    notification.title = "Inventory";
    notification.text = "A player wants to trade.";
    notification.buttons = {"Accept", "Decline"};
    notification.choose = [&](std::string value) { chosen = std::move(value); };
    notifications.post(std::move(notification));
    notifications.update(1, {2560, 1440}, 2);
    const ui::RectF button = notifications.buttonRect(0, 1);
    const double x = (button.x + button.w / 2) / 2, y = (button.y + button.h / 2) / 2;
    CHECK(notifications.handle(
        platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y}));
    CHECK(notifications.handle(
        platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y}));
    CHECK_EQ(chosen, "Decline");
    CHECK_EQ(notifications.count(), 0u);
    notifications.post({.title = "Expiring", .text = "Message", .duration = 2});
    notifications.update(10, {1280, 720}, 1);
    CHECK_EQ(notifications.count(), 1u);
    notifications.update(12, {1280, 720}, 1);
    CHECK_EQ(notifications.count(), 0u);
}

TEST(core_gui_startup_selector_can_enable_its_loading_script)
{
    auto game = DataModel::create();
    auto player = std::make_shared<Player>();
    player->setParent(game->getService<Players>());
    auto playerGui = std::make_shared<PlayerGui>();
    playerGui->setParent(player);
    auto selector = std::make_shared<LocalScript>();
    selector->setName("UISelector");
    selector->setSource("script.Loading.Enabled = true");
    auto loading = std::make_shared<LocalScript>();
    loading->setName("Loading");
    loading->setEnabled(false);
    loading->setSource(R"(
        local gui = game.Players.LocalPlayer:WaitForChild("PlayerGui")
        local screen = Instance.new("ScreenGui", gui)
        screen.Name = "MainGUI"
        local inventory = Instance.new("TextButton", screen)
        inventory.Name = "Inventory"
    )");
    loading->setParent(selector);
    selector->setParent(game->getService<ReplicatedFirst>());
    ClientSession client(game);
    CHECK(client.start(*player));
    client.step(0);
    auto* gui = player->findFirstChildOfClass("PlayerGui");
    CHECK(gui != nullptr);
    CHECK(gui->findFirstChild("Inventory", true) != nullptr);
}

TEST(core_gui_textbox_capture_and_release_focus_reach_input_service)
{
    Harness h;
    h.run(R"(
        local box = Instance.new("TextBox", game:GetService("StarterGui"))
        local input = game:GetService("UserInputService")
        input.TextBoxFocused:Connect(function(value) assert(value == box) end)
        box.FocusLost:Connect(function(entered) print("released", entered) end)
        box:CaptureFocus()
        assert(box:IsFocused() and input:GetFocusedTextBox() == box)
        box:ReleaseFocus(true)
        assert(not box:IsFocused() and input:GetFocusedTextBox() == nil)
    )");
    h.context.step(0);
    CHECK_EQ(h.lines, std::vector<std::string>{"released true"});
}

TEST(core_gui_controller_icon_lookup_has_non_printable_key_names)
{
    Harness h;
    CHECK_EQ(h.print(R"(print(game:GetService("UserInputService"):GetStringForKeyCode(Enum.KeyCode.ButtonA)))"),
             "ButtonA");
}

TEST(core_gui_hidden_geometry_queries_measure_current_constraints_without_drawing)
{
    Harness h;
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.settings().topInset = 0;
    gui.setRoots({h.game->getService<StarterGui>()});
    int measured = 0;
    h.context.setGuiLayoutQuery([&](Instance*) {
        if (gui.geometryQueryNeedsUpdate()) {
            ++measured;
            gui.ensureGeometry({800, 600}, 0);
        }
    });
    h.run(R"(
        local screen = Instance.new("ScreenGui", game:GetService("StarterGui"))
        local hidden = Instance.new("Frame", screen)
        hidden.Visible = false
        hidden.Size = UDim2.fromScale(0.5, 0.5)
        local child = Instance.new("TextButton", hidden)
        child.Size = UDim2.new(0.5, 0, 0, 100)
        local ratio = Instance.new("UIAspectRatioConstraint", child)
        ratio.AspectRatio = 2
        ratio.AspectType = Enum.AspectType.ScaleWithParentSize
        ratio.DominantAxis = Enum.DominantAxis.Width
        assert(child.AbsoluteSize == Vector2.new(200, 100))
        assert(child.AbsoluteSize == Vector2.new(200, 100))
        _G.hidden, _G.child, _G.screen = hidden, child, screen
    )");
    CHECK_EQ(measured, 1);
    CHECK(gui.objectsAt(10, 10).empty());
    h.run(R"(
        _G.hidden.Size = UDim2.fromScale(0.25, 0.5)
        assert(_G.child.AbsoluteSize == Vector2.new(100, 50))
    )");
    CHECK_EQ(measured, 2);
    CHECK(gui.objectsAt(10, 10).empty());
}

TEST(core_gui_disabled_screens_and_script_templates_have_geometry)
{
    Harness h;
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.settings().topInset = 0;
    gui.setRoots({h.game->getService<StarterGui>()});
    h.context.setGuiLayoutQuery([&](Instance*) { gui.ensureGeometry({800, 600}, 0); });
    h.run(R"(
        local screen = Instance.new("ScreenGui", game:GetService("StarterGui"))
        screen.Enabled = false
        local holder = Instance.new("LocalScript", screen)
        local prototype = Instance.new("TextButton", holder)
        prototype.Size = UDim2.fromScale(0.25, 0.25)
        assert(prototype.AbsoluteSize == Vector2.new(200, 150))
        assert(screen.AbsoluteSize == Vector2.new(800, 600))
    )");
    CHECK(gui.objectsAt(10, 10).empty());
}
