// What the console can ask of the application around the VM (Console.cpp opens
// it): whether the window has the focus, the frames a second it draws at, its
// clipboard, a message box, the code a teleport should run, and the keyboard
// and mouse input it should take as its own.
//
// None of these is anything the game itself can ask -- they are the window and
// the process around it -- so each goes through a hook the application installs
// (see ScriptContext::setActive and its neighbours). What the client cannot
// answer for itself, it says so by answering nothing: there is no system dialog
// to put a message box on, and no window to hold the focus. The one exception
// is the external console (rconsole*), which is the terminal the process runs
// in whatever the application is.

#include "Console.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

namespace lunar::lua {

namespace {

// isrbxactive() (isgameactive): whether the game window has the keyboard focus,
// which is what the executors read it for. False where there is no window.
int isRbxActive(lua_State* L)
{
    const std::function<bool()>& active = Runtime::of(L).shared().active;
    lua_pushboolean(L, active && active());
    return 1;
}

// setfpscap(frames): how many frames a second the client draws at, 0 for as many
// as it can. A windowed session takes it; where there is no frame loop to cap
// the value is kept, and getfpscap reads it back.
// GUESS: how the executors' setfpscap presents itself to the game. Nothing
// lunarstone knows says what a frame cap looks like from a script.
int setFpsCap(lua_State* L)
{
    Shared& shared = Runtime::of(L).shared();
    const lua_Integer cap = luaL_checkinteger(L, 1);
    shared.fpsCap = static_cast<int>(std::max<lua_Integer>(0, cap));
    if (shared.frameCap)
        shared.frameCap(shared.fpsCap);
    return 0;
}

// getfpscap(): the cap as it was last set, which is 0 (no cap) to begin with.
int getFpsCap(lua_State* L)
{
    lua_pushinteger(L, Runtime::of(L).shared().fpsCap);
    return 1;
}

// setclipboard(text) (toclipboard, setrbxclipboard): the window's clipboard.
// Needs a compositor connection, which the platform layer owns; without one the
// text has nowhere to go and true is not the answer. Roblox's own clipboard
// takes the text with setrbxclipboard, which the two aliases are for.
int setClipboard(lua_State* L)
{
    const std::function<void(std::string_view)>& clipboard = Runtime::of(L).shared().clipboard;
    if (!clipboard)
        return 0;
    clipboard(checkString(L, 1));
    return 0;
}

// messagebox(text[, title][, button]): what the executors put on the screen as
// a system dialog. This client has none -- it draws its own, and a popup needs
// a connection to the compositor's seat that only the platform layer has -- so
// the message goes to the console's own log, where the code that asked for it
// can read it, and 1 answers for the button that was pressed.
int messageBox(lua_State* L)
{
    Runtime& rt = Runtime::of(L);
    const std::string_view text = checkString(L, 1);
    std::string message(text);
    if (lua_type(L, 2) == LUA_TSTRING)
        message = std::format("{}: {}", checkString(L, 2), text);
    rt.output(MessageType::Output, message);
    lua_pushinteger(L, 1);
    return 1;
}

// queue_on_teleport(code) (queueonteleport): the code runs in the console once
// the next teleport has happened, as the executors' does after their own. It is
// kept here until an application takes it with ScriptContext::runTeleportCode,
// which forgets it whether the run worked or not. Queueing again replaces it,
// which is what a second call does in the clients too.
int queueOnTeleport(lua_State* L)
{
    Runtime::of(L).shared().teleportCode = std::string(checkString(L, 1));
    return 0;
}

// clearqueueonteleport() (clearteleportqueue): forgets what queue_on_teleport
// stored, so the next teleport runs nothing.
int clearQueueOnTeleport(lua_State* L)
{
    Runtime::of(L).shared().teleportCode.clear();
    return 0;
}

// The key keypress and keyrelease were given: an Enum.KeyCode, its name, or
// a Windows virtual-key code, which is what the executors take (keypress(0x41)
// for A). Codes with no KeyCode of their own (the mouse buttons, the media
// keys) are an error, as an unknown name is.
Enum::KeyCode checkHostKey(lua_State* L, int idx)
{
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const int vk = static_cast<int>(lua_tointeger(L, idx));
        static constexpr std::array<std::pair<int, std::string_view>, 34> named{{
            {0x08, "Backspace"},   {0x09, "Tab"},          {0x0D, "Return"},       {0x10, "LeftShift"},
            {0x11, "LeftControl"}, {0x12, "LeftAlt"},      {0x14, "CapsLock"},     {0x1B, "Escape"},
            {0x20, "Space"},       {0x25, "Left"},         {0x26, "Up"},           {0x27, "Right"},
            {0x28, "Down"},        {0x2E, "Delete"},       {0xA0, "LeftShift"},    {0xA1, "RightShift"},
            {0xA2, "LeftControl"}, {0xA3, "RightControl"}, {0xA4, "LeftAlt"},      {0xA5, "RightAlt"},
            {0xBA, "Semicolon"},   {0xBB, "Equals"},       {0xBC, "Comma"},        {0xBD, "Minus"},
            {0xBE, "Period"},      {0xBF, "Slash"},        {0xC0, "Backquote"},    {0xDB, "LeftBracket"},
            {0xDC, "BackSlash"},   {0xDD, "RightBracket"}, {0xDE, "Quote"},        {0x2D, "Insert"},
            {0x24, "Home"},        {0x23, "End"},
        }};
        static constexpr std::array<std::string_view, 10> digits{"Zero", "One", "Two",   "Three", "Four",
                                                                 "Five", "Six", "Seven", "Eight", "Nine"};
        std::string name;
        if (vk >= 0x30 && vk <= 0x39)
            name = digits[static_cast<std::size_t>(vk - 0x30)];
        else if (vk >= 0x41 && vk <= 0x5A)
            name = std::string(1, static_cast<char>(vk));
        else if (vk >= 0x70 && vk <= 0x7E)
            name = std::format("F{}", vk - 0x6F);
        else if (auto it = std::ranges::find(named, vk, &std::pair<int, std::string_view>::first); it != named.end())
            name = it->second;
        const EnumItem item = name.empty() ? EnumItem{} : enumItemNamed("KeyCode", name);
        if (!item.enumType)
            luaL_error(L, "no key for virtual-key code 0x%02X", vk);
        return static_cast<Enum::KeyCode>(item.value);
    }
    const std::optional<EnumItem> item = toEnumItem(L, idx, "KeyCode");
    if (!item)
        luaL_error(L, "expected an Enum.KeyCode, its name or a virtual-key code, got %s", luaL_typename(L, idx));
    return static_cast<Enum::KeyCode>(item->value);
}

void sendInput(lua_State* L, const ScriptContext::HostInput& input)
{
    const std::function<void(const ScriptContext::HostInput&)>& hook = Runtime::of(L).shared().input;
    if (hook)
        hook(input);
}

// keypress(key) and keyrelease(key): the key goes down or up in the window,
// for the game as if the player had pressed it. A key pressed and never let go
// stays down, as it does with a real keyboard.
template <bool Pressed>
int hostKey(lua_State* L)
{
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Key, .key = checkHostKey(L, 1), .pressed = Pressed});
    return 0;
}

// keyclick(key) (keytap): down and straight back up.
int hostKeyClick(lua_State* L)
{
    const Enum::KeyCode key = checkHostKey(L, 1);
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Key, .key = key, .pressed = true});
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Key, .key = key, .pressed = false});
    return 0;
}

// mouse1press, mouse1release, mouse2press, mouse2release (and mouse1click,
// mouse2click: down and straight back up): the window's buttons, wherever the
// pointer is.
template <int Button, bool Pressed>
int hostButton(lua_State* L)
{
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Button, .button = Button, .pressed = Pressed});
    return 0;
}

template <int Button>
int hostClick(lua_State* L)
{
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Button, .button = Button, .pressed = true});
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Button, .button = Button, .pressed = false});
    return 0;
}

// mousemoverel(dx, dy): the mouse moved by that many pixels, what turns the
// camera while the pointer is locked. mousemoveabs(x, y): the pointer to a
// point in the window.
template <ScriptContext::HostInput::Kind Kind>
int hostMove(lua_State* L)
{
    const double x = luaL_checknumber(L, 1);
    const double y = luaL_checknumber(L, 2);
    // A mouse moves by real amounts: NaN would turn the camera to nowhere.
    if (!std::isfinite(x) || !std::isfinite(y))
        luaL_error(L, "the mouse can't move by a number that isn't finite");
    sendInput(L, {.kind = Kind, .x = static_cast<float>(x), .y = static_cast<float>(y)});
    return 0;
}

// mousescroll(amount): the wheel turned by that much, in the units Windows
// gives a wheel (120 to a notch), forward (up the page) when positive.
int hostScroll(lua_State* L)
{
    const double amount = luaL_checknumber(L, 1);
    if (!std::isfinite(amount))
        luaL_error(L, "the wheel can't turn by a number that isn't finite");
    sendInput(L, {.kind = ScriptContext::HostInput::Kind::Scroll, .y = static_cast<float>(amount / 120.0)});
    return 0;
}

// --- The external console ------------------------------------------------------
//
// rconsole*: the executors' second window for text, beside the game's.
// Lunarstone's is the terminal it was started from: what is printed goes to
// its standard output (in colour when that is a terminal), a line typed there
// is what rconsoleinput reads, and its title is the terminal's. It is always
// there, so showing or hiding it changes nothing. What a script prints has its
// control characters taken out, so it can't send the terminal escape
// sequences of its own.

bool toTerminal()
{
    return ::isatty(STDOUT_FILENO) != 0;
}

void writeOut(std::string_view text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// The value at idx as tostring gives it, without control characters but the
// line breaks and tabs.
std::string printable(lua_State* L, int idx)
{
    std::size_t length = 0;
    const char* text = luaL_tolstring(L, idx, &length);
    std::string out;
    out.reserve(length);
    for (const char c : std::string_view(text, length)) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 0x20 && byte != 0x7f) || c == '\n' || c == '\t')
            out.push_back(c);
    }
    lua_pop(L, 1);
    return out;
}

// rconsoleprint(text): the text as it is, with no line break added.
int rconsolePrint(lua_State* L)
{
    writeOut(printable(L, 1));
    return 0;
}

// rconsoleinfo, rconsolewarn and rconsoleerr: a line of its own, marked as
// what it is.
template <int Kind>
int rconsoleLine(lua_State* L)
{
    static constexpr std::array<std::string_view, 3> marks{"[INFO]", "[WARNING]", "[ERROR]"};
    static constexpr std::array<std::string_view, 3> colours{"\x1b[36m", "\x1b[33m", "\x1b[31m"};
    const std::string text = printable(L, 1);
    if (toTerminal())
        writeOut(std::format("{}{}\x1b[0m {}\n", colours[Kind], marks[Kind], text));
    else
        writeOut(std::format("{} {}\n", marks[Kind], text));
    return 0;
}

// rconsoleclear(): an empty terminal.
int rconsoleClear(lua_State*)
{
    if (toTerminal())
        writeOut("\x1b[2J\x1b[H");
    return 0;
}

// rconsolename(title) (rconsolesettitle): the terminal's title.
int rconsoleName(lua_State* L)
{
    const std::string title = printable(L, 1);
    if (toTerminal())
        writeOut(std::format("\x1b]0;{}\x07", title));
    return 0;
}

// rconsoleshow() and rconsolehide(): the terminal is always there.
int rconsoleWindow(lua_State*)
{
    return 0;
}

// A line rconsoleinput is waiting for, read off the terminal on a thread of
// its own so the game keeps running meanwhile. One read at a time: two would
// split a line between them.
struct LineRead {
    std::atomic<bool> done{false};
    std::optional<std::string> line; // none once there is nothing more to read
};

void readLine(LineRead& read)
{
    static std::mutex reading;
    const std::scoped_lock lock(reading);
    std::string line;
    char c = 0;
    bool any = false;
    while (::read(STDIN_FILENO, &c, 1) == 1) {
        any = true;
        if (c == '\n')
            break;
        line.push_back(c);
    }
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (any)
        read.line = std::move(line);
    read.done.store(true, std::memory_order_release);
}

void whenRead(Runtime& rt, lua_State* L, std::uint64_t token, std::shared_ptr<LineRead> read)
{
    rt.after(0.0, [&rt, L, token, read = std::move(read)] mutable {
        if (!read->done.load(std::memory_order_acquire)) {
            whenRead(rt, L, token, std::move(read));
            return;
        }
        if (!rt.isSuspended(L, token))
            return;
        rt.wake(
            L, token,
            [&](lua_State* t) {
                if (read->line)
                    pushString(t, *read->line);
                else
                    lua_pushnil(t);
                return 1;
            },
            "rconsoleinput");
    });
}

// rconsoleinput(): waits for a line typed in the terminal and gives it without
// its line break; nil when there is nothing to read from (no terminal, or it
// was closed).
int rconsoleInput(lua_State* L)
{
    checkYieldable(L);
    Runtime& rt = Runtime::of(L);
    auto read = std::make_shared<LineRead>();
    std::thread([read] { readLine(*read); }).detach();
    const std::uint64_t token = rt.suspend(L);
    whenRead(rt, L, token, std::move(read));
    return lua_yield(L, 0);
}

// playtest.screenshot(name): a picture of the frame the test is on, saved by
// the playtest as <name>.png. Only a playtest (lunarstone --playtest) has it.
int playtestScreenshot(lua_State* L)
{
    const std::string_view name = checkString(L, 1);
    if (name.empty() || name.find_first_of("/\\") != std::string_view::npos || name.starts_with('.'))
        luaL_error(L, "a screenshot's name is a file name without a folder");
    Runtime::of(L).shared().playtest.screenshot(name);
    return 0;
}

// playtest.finish(passed[, message]): the test is over, passed or not.
int playtestFinish(lua_State* L)
{
    const bool passed = lua_toboolean(L, 1) != 0;
    const std::string_view message = lua_type(L, 2) == LUA_TSTRING ? checkString(L, 2) : std::string_view{};
    Runtime::of(L).shared().playtest.finish(passed, message);
    return 0;
}

} // namespace

void openConsoleHost(lua_State* thread)
{
    const auto set = [thread](const char* name, lua_CFunction function) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    };
    set("isrbxactive", isRbxActive);
    set("isgameactive", isRbxActive);
    set("setfpscap", setFpsCap);
    set("getfpscap", getFpsCap);
    set("setclipboard", setClipboard);
    set("toclipboard", setClipboard);
    set("setrbxclipboard", setClipboard);
    set("messagebox", messageBox);
    set("queue_on_teleport", queueOnTeleport);
    set("queueonteleport", queueOnTeleport);
    set("clearqueueonteleport", clearQueueOnTeleport);
    set("keypress", hostKey<true>);
    set("keyrelease", hostKey<false>);
    set("keyclick", hostKeyClick);
    set("mouse1press", hostButton<1, true>);
    set("mouse1release", hostButton<1, false>);
    set("mouse1click", hostClick<1>);
    set("mouse2press", hostButton<2, true>);
    set("mouse2release", hostButton<2, false>);
    set("mouse2click", hostClick<2>);
    set("mousemoverel", hostMove<ScriptContext::HostInput::Kind::MoveBy>);
    set("mousemoveabs", hostMove<ScriptContext::HostInput::Kind::MoveTo>);
    set("mousescroll", hostScroll);
    set("rconsoleprint", rconsolePrint);
    set("rconsoleinfo", rconsoleLine<0>);
    set("rconsolewarn", rconsoleLine<1>);
    set("rconsoleerr", rconsoleLine<2>);
    set("rconsoleclear", rconsoleClear);
    set("rconsolename", rconsoleName);
    set("rconsoleshow", rconsoleWindow);
    set("rconsolehide", rconsoleWindow);
    set("rconsoleinput", rconsoleInput);
    // A playtest's own table, which the test's helpers (app/Playtest.cpp) fill out.
    if (const ScriptContext::PlaytestHost& playtest = Runtime::of(thread).shared().playtest;
        playtest.screenshot && playtest.finish) {
        lua_newtable(thread);
        lua_pushcfunction(thread, playtestScreenshot, "screenshot");
        lua_setfield(thread, -2, "screenshot");
        lua_pushcfunction(thread, playtestFinish, "finish");
        lua_setfield(thread, -2, "finish");
        lua_setglobal(thread, "playtest");
    }
}

} // namespace lunar::lua