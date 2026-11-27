-- End-to-end coverage for game-owned menus, InputActions, coordinates and focus.
local ui = game.Players.LocalPlayer.PlayerGui
for _, child in ui:GetChildren() do child:Destroy() end
local screen = Instance.new("ScreenGui", ui)
screen.Name = "CompatibilityCheck"
screen.ResetOnSpawn = false

local function text(parent, class, name, value, position, size)
    local object = Instance.new(class, parent)
    object.Name = name
    object.Text = value
    object.Position = position
    object.Size = size
    object.BorderSizePixel = 0
    object.BackgroundColor3 = Color3.fromRGB(28, 30, 35)
    object.TextColor3 = Color3.new(1, 1, 1)
    object.TextSize = 18
    local corner = Instance.new("UICorner", object)
    corner.CornerRadius = UDim.new(0, 8)
    return object
end
local title = text(screen, "TextLabel", "Title", "Game GUI interaction check", UDim2.fromOffset(430, 24), UDim2.fromOffset(400, 44))
local wrap = Instance.new("Folder", screen)
wrap.Name = "MenuProvider"
local buttons = {}
for i, name in {"Inventory", "Shop", "Spectate"} do
    buttons[name] = text(wrap, "TextButton", name, name, UDim2.fromOffset(18, 300 + (i - 1) * 54), UDim2.fromOffset(150, 44))
end
local panel = Instance.new("Frame", screen)
panel.Name = "InventoryPanel"
panel.Position = UDim2.fromOffset(450, 140)
panel.Size = UDim2.fromOffset(380, 280)
panel.BackgroundColor3 = Color3.fromRGB(37, 40, 48)
panel.BorderSizePixel = 0
panel.Visible = false
Instance.new("UICorner", panel).CornerRadius = UDim.new(0, 12)
text(panel, "TextLabel", "Heading", "Inventory", UDim2.fromOffset(16, 12), UDim2.fromOffset(348, 40))
local filter = text(panel, "TextBox", "Filter", "", UDim2.fromOffset(16, 64), UDim2.fromOffset(348, 38))
filter.PlaceholderText = "Search inventory"
filter.ClearTextOnFocus = false
local context = Instance.new("InputContext", ui)
local action = Instance.new("InputAction", context)
action.Name = "Inventory"
local binding = Instance.new("InputBinding", action)
binding.KeyCode = Enum.KeyCode.B
binding.UIButton = buttons.Inventory
local label = Instance.new("InputActionLabel", buttons.Inventory)
label.InputAction = action
label.Position = UDim2.new(1, -32, 0, 8)
label.Size = UDim2.fromOffset(26, 26)
label.TextColor3 = Color3.new(1, 1, 1)
label.BackgroundTransparency = 1
action.Pressed:Connect(function() panel.Visible = not panel.Visible end)
local selected
for name, button in buttons do
    button.Activated:Connect(function() selected = name end)
end
local slider = Instance.new("Frame", screen)
slider.Name = "VolumeSlider"
slider.Position = UDim2.fromOffset(450, 450)
slider.Size = UDim2.fromOffset(280, 24)
slider.BackgroundColor3 = Color3.fromRGB(55, 59, 70)
local fill = Instance.new("Frame", slider)
fill.Size = UDim2.fromScale(0.2, 1)
fill.BackgroundColor3 = Color3.fromRGB(96, 190, 130)
local held, motion, released, value
slider.InputBegan:Connect(function(input)
    if input.UserInputType == Enum.UserInputType.MouseButton1 then held = input end
end)
slider.InputChanged:Connect(function(input)
    if input.UserInputType == Enum.UserInputType.MouseMovement then motion = input end
end)
slider.InputEnded:Connect(function(input)
    if input == held then held = nil; released = true end
end)
game:GetService("UserInputService").InputChanged:Connect(function(input)
    if held and input == motion then
        value = math.clamp((input.Position.X - slider.AbsolutePosition.X) / slider.AbsoluteSize.X, 0, 1)
        fill.Size = UDim2.fromScale(value, 1)
    end
end)

playtest.frames(3)
playtest.expect(label.ResolvedText == "B", "InputActionLabel resolves the desktop key")
playtest.expect(game:GetService("GuiService"):GetGuiInset().Y == 58, "modern bar reserves 58 pixels")
playtest.press("B")
playtest.expect(panel.Visible, "key binding opens the inventory")
filter:CaptureFocus()
playtest.frames(1)
playtest.expect(game:GetService("UserInputService"):GetFocusedTextBox() == filter, "focused TextBox reaches UserInputService")
playtest.press("B")
playtest.expect(panel.Visible, "typing in a TextBox does not toggle the inventory")
playtest.clickGui(buttons.Shop)
playtest.expect(selected == "Shop", "one click leaves the TextBox and activates the shop")
playtest.expect(game:GetService("UserInputService"):GetFocusedTextBox() == nil, "clicking a button releases text focus")
playtest.clickGui(buttons.Inventory)
playtest.expect(not panel.Visible, "the bound GUI button closes inventory")
playtest.press("B")
playtest.expect(panel.Visible, "keyboard remains usable after GUI activation")
local at = buttons.Shop.AbsolutePosition + buttons.Shop.AbsoluteSize / 2
local found = ui:GetGuiObjectsAtPosition(at.X, at.Y)
playtest.expect(table.find(found, buttons.Shop) ~= nil, "GUI query and AbsolutePosition share coordinates")
local inset = game:GetService("GuiService"):GetGuiInset()
local start = slider.AbsolutePosition + Vector2.new(10, 12) + inset
playtest.move(start.X, start.Y)
mouse1press()
playtest.frames(1)
playtest.move(start.X + 30, start.Y)
playtest.move(start.X + 400, start.Y + 80)
mouse1release()
playtest.frames(1)
playtest.expect(value == 1 and released and held == nil, "slider drags outside its rectangle and receives release")
local callback = Instance.new("BindableFunction")
callback.OnInvoke = function(choice) selected = choice end
game:GetService("StarterGui"):SetCore("SendNotification", {
    Title = "Trade request", Text = "Notification buttons and callbacks", Duration = 30,
    Button1 = "Accept", Button2 = "Decline", Callback = callback,
})
playtest.frames(2)
playtest.screenshot("inventory-and-notification")
playtest.click(1280 - 340 + 12 + 75, 720 - 8 - 44 + 16)
playtest.expect(selected == "Accept", "notification calls its BindableFunction")
playtest.screenshot("inventory-after-choice")
