#pragma once

#include "PlaySession.hpp"

#include <lunar/render/Graphics.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// --playtest: Play Solo without a window, driven by a test written in Luau.
//
//     lunarstone --playtest <place> <test.luau> [--out <folder>] [--size <w> <h>]
//                [--timeout <seconds>] [--players <n>] [--seed <n>] [--allow-errors]
//                [--graphics ...]
//
// The place's scripts start as in --play, a second goes by for them to
// settle, and then the test runs in the console (full access, so it sees
// the server's side too: Play Solo has one DataModel) with a `playtest`
// table of helpers:
//
//     playtest.wait(s)               simulated seconds (task.wait)
//     playtest.frames(n)             n frames
//     playtest.press(key)            a tap; keys as keypress takes them: "E", Enum.KeyCode.E
//     playtest.hold(key, s)          held for s seconds; hold({"W", "D"}, s) for several
//     playtest.move(x, y)            the pointer to a point (pixels of the picture)
//     playtest.click([x, y][, 2])    the left button (2: the right) where the pointer is or at x, y
//     playtest.clickGui(object)      the middle of a GuiObject
//     playtest.look(dx, dy)          turn the camera, as dragging with the right button
//     playtest.character(), .humanoid(), .root()
//     playtest.teleport(at)          the character to a Vector3 or CFrame
//     playtest.waitFor(fn[, s][, message])  until fn() is truthy (it's returned), else fail
//     playtest.check(ok, message)    a failure that lets the test go on
//     playtest.expect(ok, message)   a failure that stops it
//     playtest.screenshot(name)      <out>/<name>.png of this frame
//
// The test passes when it returns with no check failed. Errors from the
// game's own scripts fail it too unless --allow-errors. Everything said goes
// to stdout and <out>/log.txt, each line with its simulated time and side.
// The game's clocks (tick, os.clock, os.time, DateTime.now) follow the
// simulation, which runs as fast as it can, starting on 2026-01-01 UTC.
// The game's random numbers differ run to run unless --seed fixes them;
// a failing run says its seed so it can be played again. What a seed can't
// fix: the order pairs() visits a table keyed by tables or Instances in
// (Luau hashes those by address, which differs run to run, as on Roblox).
// Exit code: 0 passed, 1 failed, 2 couldn't run.
struct PlaytestOptions {
    std::filesystem::path test;
    std::filesystem::path out; // empty: playtest/<test's name>
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    double timeout = 120.0; // simulated seconds for the whole test
    int players = 1;
    bool allowErrors = false;
    std::optional<std::uint64_t> seed; // --seed: the scripts' random numbers (seedScripts)
    lunar::render::GraphicsSettings graphics;
    LocalUser user;
};

int playtestPlace(const std::filesystem::path& path, const PlaytestOptions& options);

// The console code a playtest runs: the helpers above, then `test` (loaded as
// a chunk named `chunkName`, so errors point at its own lines) under a pcall
// that tells playtest.finish how it went.
std::string playtestSource(std::string_view test, std::string_view chunkName);

// One of --playtest's own flags at args[i] (moving i past its values);
// false for anything else.
bool parsePlaytestFlag(std::span<const std::string_view> args, std::size_t& i, PlaytestOptions& options);
