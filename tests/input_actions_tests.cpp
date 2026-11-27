#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/ui/Gui.hpp>

using namespace lunar;

TEST(input_actions_scriptable_states_validate_types_and_release_when_disabled)
{
    Harness h;
    h.run(R"(
        local ctx = Instance.new("InputContext", workspace)
        local action = Instance.new("InputAction", ctx)
        local binding = Instance.new("InputBinding", action)
        binding.Type = Enum.InputBindingType.Scriptable
        action.Pressed:Connect(function() print("pressed") end)
        action.Released:Connect(function() print("released") end)
        binding:Fire(true)
        binding:Fire(true)
        assert(action:GetState())
        ctx.Enabled = false
        assert(not action:GetState())
        binding:Fire(true)
        assert(not action:GetState())
        assert(not pcall(function() action:Fire(5) end))
    )");
    h.context.step(0);
    CHECK_EQ(h.lines, (std::vector<std::string>{"pressed", "released"}));
}

TEST(input_actions_keyboard_context_priority_and_processed_input)
{
    Harness h;
    h.run(R"(
        for i, priority in {1, 2, 2} do
            local ctx = Instance.new("InputContext", workspace)
            ctx.Name = "Context"..i
            ctx.Priority = priority
            ctx.Sink = priority == 2
            local action = Instance.new("InputAction", ctx)
            action.Name = "Use"
            local binding = Instance.new("InputBinding", action)
            binding.KeyCode = Enum.KeyCode.E
        end
    )");
    ClientInput input(h.context);
    input.keyDown(Enum::KeyCode::E);
    input.process();
    auto state = [&](int i) {
        return static_cast<InputAction*>(
                   h.game->workspace()->findFirstChild(std::format("Context{}", i))->findFirstChild("Use"))
            ->boolState();
    };
    CHECK(!state(1));
    CHECK(state(2));
    CHECK(state(3));
    input.keyUp(Enum::KeyCode::E, true);
    input.process();
    CHECK(!state(1));
    CHECK(!state(2));
    CHECK(!state(3));
    input.keyDown(Enum::KeyCode::E, true);
    input.process();
    CHECK(!state(1));
    CHECK(!state(2));
    CHECK(!state(3));
}

TEST(input_actions_modifiers_composite_axes_and_multiple_bindings)
{
    Harness h;
    h.run(R"(
        local action = Instance.new("InputAction", workspace)
        action.Name = "Move"
        action.Type = Enum.InputActionType.Direction2D
        local binding = Instance.new("InputBinding", action)
        binding.Up = Enum.KeyCode.W
        binding.Right = Enum.KeyCode.D
        binding.PrimaryModifier = Enum.KeyCode.LeftShift
        local use = Instance.new("InputAction", workspace)
        use.Name = "Use"
        for _, key in {Enum.KeyCode.E, Enum.KeyCode.Q} do
            local binding = Instance.new("InputBinding", use)
            binding.KeyCode = key
        end
    )");
    auto* move = static_cast<InputAction*>(h.game->workspace()->findFirstChild("Move"));
    auto* use = static_cast<InputAction*>(h.game->workspace()->findFirstChild("Use"));
    ClientInput input(h.context);
    input.keyDown(Enum::KeyCode::W);
    input.process();
    CHECK(move->direction2DState() == Vector2{});
    input.keyDown(Enum::KeyCode::LeftShift);
    input.keyDown(Enum::KeyCode::D);
    input.keyDown(Enum::KeyCode::E);
    input.keyDown(Enum::KeyCode::Q);
    input.process();
    CHECK(std::abs(move->direction2DState().magnitude() - 1.0f) < 0.001f);
    CHECK(use->boolState());
    input.keyUp(Enum::KeyCode::E);
    input.process();
    CHECK(use->boolState());
    input.keyUp(Enum::KeyCode::LeftShift);
    input.keyUp(Enum::KeyCode::Q);
    input.process();
    CHECK(move->direction2DState() == Vector2{});
    CHECK(!use->boolState());
}

TEST(input_actions_templates_do_not_receive_hardware_and_gui_buttons_do)
{
    Harness h;
    h.run(R"(
        local action = Instance.new("InputAction", game:GetService("ReplicatedStorage"))
        action.Name = "Template"
        local binding = Instance.new("InputBinding", action)
        binding.KeyCode = Enum.KeyCode.E
        local ctx = Instance.new("InputContext", workspace)
        local use = action:Clone()
        use.Name = "Use"
        use.Parent = ctx
        local screen = Instance.new("ScreenGui", game:GetService("StarterGui"))
        local button = Instance.new("TextButton", screen)
        button.Name = "UseButton"
        use.InputBinding.UIButton = button
    )");
    ClientInput input(h.context);
    input.process();
    auto* use = static_cast<InputAction*>(h.game->workspace()->findFirstChild("InputContext")->findFirstChild("Use"));
    auto* button = static_cast<TextButton*>(h.game->getService<StarterGui>()->findFirstChild("UseButton", true));
    button->firePress(10, 20);
    CHECK(use->boolState());
    button->fireRelease(10, 20, false);
    CHECK(!use->boolState());
    input.keyDown(Enum::KeyCode::E);
    input.process();
    CHECK(use->boolState());
    CHECK(!static_cast<InputAction*>(h.game->getService("ReplicatedStorage")->findFirstChild("Template"))->boolState());
}

TEST(input_actions_labels_use_keyboard_binding_and_render_text)
{
    const auto contentDir = assets::AssetConfig::fromEnvironment().contentDir;
    if (contentDir.empty())
        SKIP("set LUNAR_CONTENT_DIR to run font-dependent tests");
    auto game = DataModel::create();
    auto screen = std::make_shared<ScreenGui>();
    screen->setParent(game->getService<StarterGui>());
    auto action = std::make_shared<InputAction>();
    action->setParent(screen);
    auto gamepad = std::make_shared<InputBinding>();
    gamepad->setKeyCode(Enum::KeyCode::ButtonA);
    gamepad->setParent(action);
    auto keyboard = std::make_shared<InputBinding>();
    keyboard->setKeyCode(Enum::KeyCode::E);
    keyboard->setPrimaryModifier(Enum::KeyCode::LeftShift);
    keyboard->setParent(action);
    auto label = std::make_shared<InputActionLabel>();
    label->setInputAction(action.get());
    label->setSize(UDim2::fromOffset(140, 40));
    label->setParent(screen);
    CHECK(action->preferredBinding() == keyboard.get());
    CHECK_EQ(label->resolvedText(), "Shift + E");
    ui::FontLibrary fonts(contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.setRoots({game->getService<StarterGui>()});
    gui.update({640, 480}, 0);
    ui::DrawList list;
    gui.draw(list);
    CHECK(!list.quads.empty());
    CHECK(list.quads.size() > 5);
    keyboard->setDisplayName("Open inventory");
    CHECK_EQ(label->resolvedText(), "Open inventory");
}

TEST(input_events_use_gui_origin_and_keep_raw_mouse_location)
{
    Harness h;
    h.game->getService<GuiService>()->assignTopbarInset({{170, 0}, {1280, 58}});
    h.run(R"(
        local input = game:GetService("UserInputService")
        input.InputBegan:Connect(function(obj, processed)
            print(obj.Position.X, obj.Position.Y, processed)
        end)
        input.LastInputTypeChanged:Connect(function(kind) print(kind.Name) end)
    )");
    ClientInput input(h.context);
    input.mouseMove({100, 200}, {5, 8});
    input.mouseButton(Enum::UserInputType::MouseButton1, true, true);
    input.process();
    h.run(R"(print(game:GetService("UserInputService"):GetMouseLocation()))");
    CHECK_EQ(h.lines, (std::vector<std::string>{"MouseMovement", "MouseButton1", "100 142 true", "100, 200"}));
}

TEST(input_events_gui_buttons_report_logical_coordinates_at_high_dpi)
{
    auto game = DataModel::create();
    auto screen = std::make_shared<ScreenGui>();
    screen->setParent(game->getService<StarterGui>());
    auto button = std::make_shared<TextButton>();
    button->setPosition(UDim2::fromOffset(10, 20));
    button->setSize(UDim2::fromOffset(100, 50));
    button->setParent(screen);
    Vector2 at{-1, -1};
    button->eventFired.connect([&](std::string_view event, std::span<const Value> arguments) {
        if (event == "MouseButton1Down")
            at = {static_cast<float>(std::get<double>(arguments[0])),
                  static_cast<float>(std::get<double>(arguments[1]))};
    });
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.setRoots({game->getService<StarterGui>()});
    gui.settings().pixelScale = 2;
    gui.settings().topInset = 58;
    gui.update({2560, 1440}, 0);
    CHECK(gui.handle(
        platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = 60, .y = 103}));
    CHECK(at == (Vector2{60, 45}));
    CHECK(button->absolutePosition() == (Vector2{10, 20}));
}

TEST(input_events_gui_drag_and_user_input_share_objects_and_release_outside)
{
    Harness h;
    h.run(R"(
        local screen = Instance.new("ScreenGui", game:GetService("StarterGui"))
        screen.IgnoreGuiInset = true
        local frame = Instance.new("Frame", screen)
        frame.Name = "Slider"
        frame.Size = UDim2.fromOffset(100, 50)
        assert(not frame.Active)
        local input = game:GetService("UserInputService")
        local press, motion
        frame.InputBegan:Connect(function(obj)
            assert(obj.UserInputState == Enum.UserInputState.Begin)
            press = obj
            print("gui began")
        end)
        frame.InputChanged:Connect(function(obj)
            if obj.UserInputType == Enum.UserInputType.MouseMovement then
                motion = obj
            else
                assert(obj.Position.Z == 1)
                print("wheel up")
            end
        end)
        frame.InputEnded:Connect(function(obj)
            assert(obj == press)
            assert(obj.Position == Vector3.new(200, 150, 0))
            print("gui ended")
        end)
        input.InputBegan:Connect(function(obj) assert(obj == press) end)
        input.InputChanged:Connect(function(obj)
            if obj.UserInputType == Enum.UserInputType.MouseMovement and motion then
                assert(obj == motion)
                print("drag", obj.Position.X)
            end
        end)
    )");
    ui::FontLibrary fonts(assets::AssetConfig::fromEnvironment().contentDir);
    ui::GlyphAtlas atlas;
    ui::GuiEngine gui(fonts, atlas);
    gui.setRoots({h.game->getService<StarterGui>()});
    gui.settings().topInset = 0;
    gui.setDeferredInput(true);
    gui.update({800, 600}, 0);
    ClientInput input(h.context);
    gui.handle(platform::MouseButtonEvent{.pressed = true, .x = 10, .y = 10});
    input.mouseMove({10, 10}, {});
    input.mouseButton(Enum::UserInputType::MouseButton1, true, false, gui.takeInputDispatch());
    input.process();
    gui.handle(platform::MouseMoveEvent{20, 10});
    input.mouseMove({20, 10}, {10, 0}, false, gui.takeInputDispatch());
    input.process();
    gui.handle(platform::ScrollEvent{.dy = 1});
    input.mouseWheel(1, false, gui.takeInputDispatch());
    input.process();
    gui.handle(platform::MouseMoveEvent{200, 150});
    input.mouseMove({200, 150}, {180, 140}, false, gui.takeInputDispatch());
    input.process();
    // A native overlay may consume the release. It must still end the drag.
    gui.clearInputDispatch();
    gui.finishInputEvent(platform::MouseButtonEvent{.pressed = false, .x = 200, .y = 150});
    input.mouseButton(Enum::UserInputType::MouseButton1, false, true, gui.takeInputDispatch());
    input.process();
    CHECK_EQ(h.lines, (std::vector<std::string>{"gui began", "drag 20", "wheel up", "drag 200", "gui ended"}));
}
