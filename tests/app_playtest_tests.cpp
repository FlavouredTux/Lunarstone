// --playtest's side of the console: the playtest table only a playtest has,
// the helpers its tests are written with (app/Playtest.cpp), and the seed
// that makes a run repeatable.

#include "framework.hpp"
#include "script_harness.hpp"

#include "../app/Playtest.hpp"

#include <lunar/script/ScriptContext.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lunar;

namespace {

// A test run in a console with a playtest host, stepped until it finishes
// (or ten simulated seconds go by).
struct Playtest {
    Harness h;
    std::vector<std::string> shots;
    std::optional<std::pair<bool, std::string>> result;

    Playtest()
    {
        h.context.setPlaytest({
            .screenshot = [this](std::string_view name) { shots.emplace_back(name); },
            .finish = [this](bool passed, std::string_view message) { result.emplace(passed, message); },
        });
    }

    void run(std::string_view test)
    {
        if (auto ran = h.context.runConsole(playtestSource(test, "t.luau")); !ran)
            result.emplace(false, ran.error().message);
        for (int frame = 0; frame < 600 && !result; ++frame)
            h.context.step(1.0 / 60.0);
    }
};

// Whether it finished, and failed.
bool failed(const Playtest& t)
{
    return t.result && !t.result->first;
}

} // namespace

TEST(playtest_passes_a_test_that_returns_with_no_check_failed)
{
    Playtest t;
    t.run(R"lua(
        local ready = false
        task.delay(0.5, function() ready = true end)
        playtest.check(playtest.waitFor(function() return ready end, 2), "waitFor gives the value back")
        playtest.screenshot("done")
    )lua");
    CHECK(t.result.has_value());
    if (!t.result)
        return;
    CHECK(t.result->first);
    CHECK_EQ(t.result->second, std::string());
    CHECK_EQ(t.shots, std::vector<std::string>{"done"});
}

TEST(playtest_fails_on_checks_errors_and_waits_that_time_out)
{
    Playtest checks;
    checks.run(R"lua(
        playtest.check(false, "first")
        playtest.check(true, "fine")
        playtest.check(1 + 1 == 3, "second")
    )lua");
    CHECK(failed(checks));
    CHECK_EQ(checks.result ? checks.result->second : "", std::string("2 check(s) failed: first; second"));

    // An error stops it, with the test's own line.
    Playtest error;
    error.run("playtest.wait(0.1)\nlocal t = nil\nprint(t.field)");
    CHECK(failed(error));
    CHECK(error.result && error.result->second.starts_with("t.luau:3:"));

    Playtest expect;
    expect.run("playtest.expect(false, 'not there')\nplaytest.check(false, 'never reached')");
    CHECK(expect.result && expect.result->second.ends_with("not there"));

    Playtest waited;
    waited.run("playtest.waitFor(function() return false end, 0.5, 'never')");
    CHECK(waited.result && waited.result->second.ends_with("never (waited 0.5 s)"));

    // A test that doesn't compile says why.
    Playtest syntax;
    syntax.run("local = 1");
    CHECK(failed(syntax));
    CHECK(syntax.result && syntax.result->second.starts_with("t.luau:1:"));
}

TEST(playtest_takes_any_test_source)
{
    // Long brackets of every level, quotes and a last ] can't end the source early.
    Playtest t;
    t.run("local s = [[a]] .. [==[b]=]c]==] .. \"]]\"\nplaytest.check(s == 'ab]=]c]]', s)\n-- ]");
    CHECK(t.result && t.result->first);
}

TEST(playtest_screenshot_names_are_file_names)
{
    Playtest t;
    t.run(R"lua(
        assert(not pcall(playtest.screenshot, "../up"))
        assert(not pcall(playtest.screenshot, "a/b"))
        assert(not pcall(playtest.screenshot, ""))
        playtest.screenshot("fine")
    )lua");
    CHECK(t.result && t.result->first);
    CHECK_EQ(t.shots, std::vector<std::string>{"fine"});
}

TEST(playtest_table_is_only_there_in_a_playtest)
{
    // Not in a console without a playtest host, and never for the game.
    Harness plain;
    plain.console("assert(playtest == nil)");
    Playtest t;
    t.h.run("assert(playtest == nil)");
}

TEST(playtest_seed_repeats_the_scripts_random_numbers)
{
    constexpr std::string_view draw = R"lua(
        print(math.random(1, 1e9), Random.new():NextInteger(1, 1e9), BrickColor.random().Number,
              game:GetService("HttpService"):GenerateGUID())
    )lua";
    auto run = [&](std::uint64_t seed) {
        seedScripts(seed);
        Harness h;
        h.run(draw);
        return h.lines;
    };
    const std::vector<std::string> first = run(7);
    CHECK_EQ(first.size(), std::size_t{1});
    CHECK_EQ(run(7), first);
    CHECK(run(8) != first);
    seedScripts(std::nullopt); // the other tests' scripts get the machine's entropy again
}

TEST(playtest_clocks_follow_simulated_time)
{
    // Under simulated clocks a second of steps is a second to every clock
    // the game reads, however long it took; off again, they're the real ones.
    simulateClocks(1'767'225'600.0);
    Harness h;
    h.run(R"lua(
        local start = { tick(), os.clock(), os.time(), DateTime.now().UnixTimestampMillis,
                        workspace:GetServerTimeNow() }
        assert(start[1] == 1767225600 and start[2] == 0 and start[3] == 1767225600, table.concat(start, " "))
        assert(start[4] == 1767225600000 and start[5] == 1767225600)
        assert(os.time({ year = 2000, month = 1, day = 1, hour = 0 }) ~= nil, "a date still goes to os.time")
        task.wait(1)
        print(tick() - start[1], os.clock() - start[2], os.time() - start[3])
    )lua");
    for (int i = 0; i < 70; ++i)
        h.context.step(1.0 / 60.0);
    simulateClocks(std::nullopt);
    CHECK_EQ(h.lines.size(), std::size_t{1});
    if (!h.lines.empty())
        CHECK_EQ(h.lines[0], std::string("1 1 1"));

    Harness real;
    real.run("assert(tick() > 1.7e9 and os.clock() ~= nil)");
}
