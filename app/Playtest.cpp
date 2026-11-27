#include "Playtest.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace {

// The helpers, in Luau over the console's own keypress/mouse1press family
// and the playtest table's screenshot and finish (ConsoleHost.cpp).
constexpr std::string_view helpers = R"luau(
local playtest = playtest
getgenv().playtest = playtest
local Players = game:GetService("Players")
local GuiService = game:GetService("GuiService")
local failures = {}

function playtest.wait(seconds)
    return task.wait(seconds)
end

function playtest.frames(count)
    for _ = 1, count or 1 do
        task.wait()
    end
end

local function keys(key)
    return if type(key) == "table" then key else { key }
end

function playtest.hold(key, seconds)
    for _, k in keys(key) do
        keypress(k)
    end
    task.wait(seconds)
    for _, k in keys(key) do
        keyrelease(k)
    end
    task.wait()
end

function playtest.press(key)
    playtest.hold(key, nil)
end

function playtest.move(x, y)
    mousemoveabs(x, y)
    task.wait()
end

function playtest.click(x, y, button)
    if type(x) == "number" and type(y) == "number" then
        playtest.move(x, y)
    elseif x == 2 then
        button = 2
    end
    if button == 2 then
        mouse2press()
        task.wait()
        mouse2release()
    else
        mouse1press()
        task.wait()
        mouse1release()
    end
    task.wait()
end

function playtest.clickGui(object)
    assert(typeof(object) == "Instance" and object:IsA("GuiObject"), "clickGui wants a GuiObject")
    local inset = GuiService:GetGuiInset()
    local at = object.AbsolutePosition + object.AbsoluteSize / 2 + inset
    playtest.click(at.X, at.Y)
end

function playtest.look(dx, dy)
    mouse2press()
    task.wait()
    task.wait() -- the camera locks the pointer a frame after the button goes down
    mousemoverel(dx, dy or 0)
    task.wait()
    mouse2release()
    task.wait()
end

function playtest.character()
    local player = Players.LocalPlayer
    return player and player.Character
end

function playtest.humanoid()
    local character = playtest.character()
    return character and character:FindFirstChildOfClass("Humanoid")
end

function playtest.root()
    local character = playtest.character()
    return character and character:FindFirstChild("HumanoidRootPart")
end

function playtest.teleport(at)
    local character = playtest.character()
    assert(character, "no character to teleport")
    character:PivotTo(if typeof(at) == "Vector3" then CFrame.new(at) else at)
    task.wait()
end

function playtest.waitFor(condition, timeout, message)
    timeout = timeout or 10
    local start = time()
    while true do
        local value = condition()
        if value then
            return value
        end
        if time() - start >= timeout then
            error(string.format("%s (waited %g s)", message or "waitFor timed out", timeout), 2)
        end
        task.wait()
    end
end

function playtest.check(ok, message)
    if not ok then
        message = message or "check failed"
        table.insert(failures, message)
        warn("FAIL: " .. message)
    end
    return ok
end

function playtest.expect(ok, message)
    if not ok then
        error(message or "expectation failed", 2)
    end
    return ok
end

-- "=name": messages read name:line, as the file is called.
local test, problem = loadstring(source, "=" .. chunk)
if not test then
    playtest.finish(false, problem)
    return
end
local ran, failure = pcall(test)
if not ran then
    playtest.finish(false, tostring(failure))
elseif #failures > 0 then
    playtest.finish(false, string.format("%d check(s) failed: %s", #failures, table.concat(failures, "; ")))
else
    playtest.finish(true)
end
)luau";

// A long string that `text` can't end early: [=[ ]=] with more = than any
// closing bracket in it.
std::string longString(std::string_view text)
{
    std::string level;
    while (text.find("]" + level + "]") != std::string_view::npos)
        level += '=';
    // A newline right after the opening bracket is dropped, so one goes in
    // ahead of the text's own first line; one before the closing bracket
    // keeps a last ] of the text's from closing it early.
    return std::format("[{}[\n{}\n]{}]", level, text, level);
}

// A Luau string literal of `text`.
std::string quoted(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out + '"';
}

} // namespace

std::string playtestSource(std::string_view test, std::string_view chunkName)
{
    return std::format("local source = {}\nlocal chunk = {}\n{}", longString(test), quoted(chunkName), helpers);
}

bool parsePlaytestFlag(std::span<const std::string_view> args, std::size_t& i, PlaytestOptions& options)
{
    const bool hasValue = i + 1 < args.size();
    if (args[i] == "--out" && hasValue)
        options.out = args[++i];
    else if (args[i] == "--size" && i + 2 < args.size()) {
        options.width = static_cast<std::uint32_t>(std::stoul(std::string(args[++i])));
        options.height = static_cast<std::uint32_t>(std::stoul(std::string(args[++i])));
    } else if (args[i] == "--timeout" && hasValue)
        options.timeout = std::stod(std::string(args[++i]));
    else if (args[i] == "--players" && hasValue)
        options.players = std::max(1, std::stoi(std::string(args[++i])));
    else if (args[i] == "--seed" && hasValue)
        options.seed = std::stoull(std::string(args[++i]));
    else if (args[i] == "--allow-errors")
        options.allowErrors = true;
    else
        return false;
    return true;
}
