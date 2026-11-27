-- Native stock GUI art and content insets, using the installed client resources.
local starter = game:GetService("StarterGui")
starter:SetCoreGuiEnabled(Enum.CoreGuiType.Chat, false)
starter:SetCoreGuiEnabled(Enum.CoreGuiType.PlayerList, false)
starter:SetCoreGuiEnabled(Enum.CoreGuiType.Backpack, false)
local ui = game.Players.LocalPlayer.PlayerGui
for _, child in ui:GetChildren() do child:Destroy() end
local screen = Instance.new("ScreenGui", ui)
screen.Name = "LegacyStyleCheck"
screen.ResetOnSpawn = false

local function caption(value, x, y, w, h)
    local label = Instance.new("TextLabel", screen)
    label.Position = UDim2.fromOffset(x, y)
    label.Size = UDim2.fromOffset(w, h)
    label.BackgroundTransparency = 1
    label.Text = value
    label.TextSize = 18
    label.TextColor3 = Color3.new(0, 0, 0)
    return label
end
caption("Roblox stock frame and button styles", 20, 0, 1240, 30)
local frames = {
    Enum.FrameStyle.ChatBlue, Enum.FrameStyle.ChatGreen, Enum.FrameStyle.ChatRed,
    Enum.FrameStyle.RobloxSquare, Enum.FrameStyle.RobloxRound, Enum.FrameStyle.DropShadow,
}
for i, style in frames do
    local col, row = (i - 1) % 2, math.floor((i - 1) / 2)
    local x, y = 20 + col * 210, 66 + row * 190
    caption(style.Name, x, y - 28, 190, 26)
    local panel = Instance.new("Frame", screen)
    panel.Position = UDim2.fromOffset(x, y)
    panel.Size = UDim2.fromOffset(190, 144)
    panel.Style = style
    local content = Instance.new("Frame", panel)
    content.Size = UDim2.fromScale(1, 1)
    content.BorderSizePixel = 1
    content.BorderColor3 = Color3.new(1, 1, 1)
    content.BackgroundTransparency = 0.9
    content.BackgroundColor3 = Color3.new(1, 1, 1)
end
local buttons = {}
for i, style in {
    Enum.ButtonStyle.RobloxButtonDefault, Enum.ButtonStyle.RobloxButton,
    Enum.ButtonStyle.RobloxRoundButton, Enum.ButtonStyle.RobloxRoundDefaultButton,
    Enum.ButtonStyle.RobloxRoundDropdownButton,
} do
    local y = 50 + (i - 1) * 112
    caption(style.Name, 500, y, 700, 28)
    local button = Instance.new("TextButton", screen)
    button.Position = UDim2.fromOffset(670, y + 34)
    button.Size = UDim2.fromOffset(340, 52)
    button.Style = style
    button.Text = "Invite / continue"
    button.TextSize = 20
    button.TextColor3 = if style == Enum.ButtonStyle.RobloxRoundDropdownButton
        then Color3.new(0.1, 0.1, 0.1) else Color3.new(1, 1, 1)
    buttons[i] = button
end
-- IsLoaded asks the same provider used by the native skin renderer.
for _, file in {
    "textures/ui/dialog_blue.png", "textures/ui/dialog_green.png", "textures/ui/dialog_red.png",
    "textures/blackBkg_square.png", "textures/blackBkg_round.png", "textures/ui/newBkg_square.png",
    "textures/ui/btn_greyTransp.png", "textures/ui/btn_grey.png", "textures/ui/btn_white.png",
    "textures/ui/btn_red.png", "textures/ui/btn_redGlow.png",
    "textures/ui/btn_newGrey.png", "textures/ui/btn_newGreyGlow.png",
    "textures/ui/btn_newBlue.png", "textures/ui/btn_newBlueGlow.png",
    "textures/ui/btn_newWhite.png", "textures/ui/btn_newWhiteGlow.png",
} do
    local preload = Instance.new("ImageLabel", screen)
    preload.Visible = false
    preload.Image = "rbxasset://" .. file
    playtest.waitFor(function() return preload.IsLoaded end, 5, file .. " loads")
    preload:Destroy()
end
playtest.frames(2)
playtest.screenshot("stock-styles")
local clicked = false
buttons[4].Activated:Connect(function() clicked = true end)
playtest.clickGui(buttons[4])
playtest.expect(clicked, "a styled Invite button activates")
buttons[4].Selected = true
playtest.screenshot("stock-styles-selected")
