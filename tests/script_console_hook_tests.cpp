// The console's hooks (ConsoleHooks.cpp): hookfunction, hookmetamethod,
// restorefunction and getgc, with what the executors' own environment check
// asks of them first, then what a hook in a running game has to survive --
// game scripts calling through it, yields inside it, hooks stacking, a hook
// taking itself off, and frames already running what got hooked.

#include "framework.hpp"
#include "script_harness.hpp"

#include <string>
#include <vector>

using namespace lunar;

// The executors' environment check (UNC), as it asks it.
TEST(script_console_hookfunction_as_the_unc_check_asks)
{
    Harness h;
    h.console(R"lua(
        local function test()
            return true
        end
        local ref = hookfunction(test, function()
            return false
        end)
        assert(test() == false, "Function should return false")
        assert(ref() == true, "Original function should return true")
        assert(test ~= ref, "Original function should not be same as the reference")
        assert(replaceclosure == hookfunction, "replaceclosure is the other name")
    )lua");
}

TEST(script_console_hookmetamethod_as_the_unc_check_asks)
{
    Harness h;
    h.console(R"lua(
        local object = setmetatable({}, { __index = newcclosure(function() return false end), __metatable = "Locked!" })
        local ref = hookmetamethod(object, "__index", function() return true end)
        assert(object.test == true, "Failed to hook a metamethod and change the return value")
        assert(ref() == false, "Did not return the original function")
    )lua");
}

TEST(script_console_getnamecallmethod_as_the_unc_check_asks)
{
    Harness h;
    h.console(R"lua(
        local method
        local ref
        ref = hookmetamethod(game, "__namecall", function(...)
            if not method then
                method = getnamecallmethod()
            end
            return ref(...)
        end)
        game:GetService("Lighting")
        assert(method == "GetService", "Did not get the correct method (GetService)")
        restorefunction(getrawmetatable(game).__namecall)
    )lua");
}

TEST(script_console_hooks_keep_the_arguments_and_results)
{
    Harness h;
    h.console(R"lua(
        local function add(a, b, ...) return a + b, select("#", ...) end
        local seen
        local old
        old = hookfunction(add, function(a, b, ...)
            seen = {a, b, ...}
            local sum, extra = old(a, b, ...)
            return sum * 10, extra, "more"
        end)
        local sum, extra, more = add(1, 2, nil, nil)
        assert(sum == 30 and extra == 2 and more == "more", "the results changed on the way out")
        assert(#seen == 2 and seen[1] == 1 and seen[2] == 2, "the arguments changed on the way in")
        -- The hook runs wherever the function is called from: it's the function
        -- itself that changed, not this local.
        local holder = { f = add }
        assert(select(1, holder.f(2, 3)) == 50, "a second reference doesn't see the hook")
    )lua");
}

TEST(script_console_hooks_c_functions_where_every_script_calls_them)
{
    Harness h;
    // A game script's function, kept where the console can reach it.
    h.run(R"lua(
        _G.lighting = function() return game:GetService("Lighting") end
    )lua");
    h.console(R"lua(
        local namecall = getrawmetatable(game).__namecall
        assert(iscclosure(namecall), "__namecall should be a C function")
        local calls = 0
        local old
        old = hookmetamethod(game, "__namecall", function(self, ...)
            if getnamecallmethod() == "GetService" then
                calls += 1
            end
            return old(self, ...)
        end)
        assert(isfunctionhooked(namecall), "the metatable's function is the one hooked")
        assert(getrenv()._G.lighting() == game:GetService("Lighting"), "the hook changed what the call answers")
        assert(calls == 2, "the hook didn't see both calls")
        getgenv().calls = function() return calls end
    )lua");
    // A script that starts after the hook goes through it as well.
    h.run("game:GetService('Lighting')");
    h.console(R"lua(
        assert(getgenv().calls() == 3, "a new script's method call skipped the hook")
        restorefunction(getrawmetatable(game).__namecall)
        assert(not isfunctionhooked(getrawmetatable(game).__namecall), "still hooked")
        game:GetService("Lighting")
        assert(getgenv().calls() == 3, "the hook ran after it was taken off")
    )lua");
}

TEST(script_console_checkcaller_tells_the_game_from_the_console_in_a_hook)
{
    Harness h;
    h.console(R"lua(
        local old
        old = hookmetamethod(game, "__namecall", function(self, ...)
            if getnamecallmethod() == "FindFirstChild" then
                table.insert(getgenv().callers, checkcaller())
            end
            return old(self, ...)
        end)
        getgenv().callers = {}
        game:FindFirstChild("Workspace")
    )lua");
    h.run("game:FindFirstChild('Workspace')");
    h.console(R"lua(
        local callers = getgenv().callers
        assert(#callers == 2, "the hook saw " .. #callers .. " calls")
        assert(callers[1] == true, "the console's own call isn't its own")
        assert(callers[2] == false, "the game's call looked like the console's")
        restorefunction(getrawmetatable(game).__namecall)
    )lua");
}

TEST(script_console_a_hook_can_yield)
{
    Harness h;
    // The hook waits a frame before passing the call on, as a hook throttling
    // remotes would; it's wrapped in newcclosure, as most are, so both the
    // hook's own C body and newcclosure's have to let the yield through.
    h.console(R"lua(
        local old
        old = hookmetamethod(game, "__namecall", newcclosure(function(self, ...)
            if getnamecallmethod() == "GetService" and not checkcaller() then
                task.wait()
            end
            return old(self, ...)
        end))
    )lua");
    h.run(R"lua(
        _G.started = true
        _G.service = game:GetService("Lighting")
    )lua");
    h.console("assert(getrenv()._G.started and getrenv()._G.service == nil, 'the hook did not wait')");
    h.steps(3);
    h.console(R"lua(
        assert(getrenv()._G.service == game:GetService("Lighting"), "the call did not finish after the wait")
        restorefunction(getrawmetatable(game).__namecall)
    )lua");
}

TEST(script_console_hooks_stack_and_restore_to_the_first)
{
    Harness h;
    h.console(R"lua(
        local function base() return "base" end
        local first, second
        first = hookfunction(base, function() return "first(" .. first() .. ")" end)
        second = hookfunction(base, function() return "second(" .. second() .. ")" end)
        assert(base() == "second(first(base))", "the hooks didn't both run: " .. base())
        restorefunction(base)
        assert(base() == "base", "restorefunction didn't put the original back: " .. base())
        assert(not isfunctionhooked(base), "still hooked")
        assert(not pcall(restorefunction, base), "restoring what isn't hooked should fail")

        -- The same with a C function, whose hook is looked up per call.
        local c = newcclosure(function() return "c" end)
        local cfirst, csecond
        cfirst = hookfunction(c, function() return "first(" .. cfirst() .. ")" end)
        csecond = hookfunction(c, function() return "second(" .. csecond() .. ")" end)
        assert(c() == "second(first(c))", "the C hooks didn't both run: " .. c())
        restorefunction(c)
        assert(c() == "c", "restorefunction didn't put the C original back: " .. c())
    )lua");
}

TEST(script_console_a_hooked_function_keeps_its_upvalues)
{
    Harness h;
    // Hooked, it is the hook, with the hook's upvalues and nil past them; the
    // collector walks it as it walks any closure, and restoring it brings
    // back the code and the upvalues it uses.
    h.console(R"lua(
        local a, b, c = {}, {}, {}
        local function uses() return a, b, c end
        local mark = {}
        hookfunction(uses, function() return "hooked", mark end)
        assert(uses() == "hooked", "the hook didn't run")
        assert(debug.getupvalue(uses, 1) == mark and debug.getupvalue(uses, 3) == nil, "not the hook's upvalues")
        getgc() -- a full collection first
        restorefunction(uses)
        local x, y, z = uses()
        assert(x == a and y == b and z == c, "restored, it doesn't read its upvalues")
    )lua");
}

TEST(script_console_a_hook_can_take_itself_off)
{
    Harness h;
    // Hooking once: the hook restores the function from inside itself, then
    // passes the call on. __namecall's own C function has no continuation, so
    // the hook's body must not call the one the closure has by the end.
    h.console(R"lua(
        local namecall = getrawmetatable(game).__namecall
        local seen = 0
        local old
        old = hookfunction(namecall, function(self, ...)
            seen += 1
            restorefunction(namecall)
            return old(self, ...)
        end)
        assert(game:GetService("Lighting") == game:GetService("Lighting"), "the calls answered wrong")
        assert(seen == 1, "the hook ran " .. seen .. " times")

        local function f() return 1 end
        local fold
        fold = hookfunction(f, function()
            restorefunction(f)
            return fold() + 1
        end)
        assert(f() == 2 and f() == 1, "the Luau hook didn't take itself off")
    )lua");
}

TEST(script_console_hooking_a_running_function_leaves_its_frame_alone)
{
    Harness h;
    // A game function that is in the middle of a wait when it gets hooked: the
    // frame goes on with the code it started with, its traceback still reads
    // (the debug library looks the frame's line up in the closure's code), and
    // the next call goes to the hook.
    h.run(R"lua(
        local function slow()
            local before = 1
            task.wait()
            _G.trace = debug.traceback()
            return before
        end
        _G.slow = slow
        task.spawn(function() _G.result = slow() end)
    )lua");
    h.console(R"lua(
        hookfunction(getrenv()._G.slow, function() return 2 end)
    )lua");
    h.steps(3);
    h.console(R"lua(
        local G = getrenv()._G
        assert(G.result == 1, "the running frame changed code: " .. tostring(G.result))
        assert(type(G.trace) == "string" and G.trace:find("slow"), "the frame's traceback is wrong: " .. tostring(G.trace))
        assert(G.slow() == 2, "the next call didn't reach the hook")
    )lua");
}

TEST(script_console_a_hooks_caller_is_the_hooked_functions)
{
    Harness h;
    // The hook runs in the hooked function's own frame, so level 2 is what
    // called it: its locals are there to read and change. (So when the hook's
    // upvalues fit in the function's, as sUNC's getstack test has them.)
    h.run(R"lua(
        local calls = 0
        local function target() calls += 1 return "Yo" end
        _G.target = target
        task.spawn(function()
            local marker = table.concat({"the caller's", "own local"}, " ") -- made here, so in a register
            while task.wait() do
                target()
                _G.marker = marker
            end
        end)
    )lua");
    h.console(R"lua(
        local old
        old = hookfunction(getrenv()._G.target, function(...)
            for i, v in debug.getstack(2) do
                if v == "the caller's own local" then debug.setstack(2, i, "changed") end
            end
            return old(...)
        end)
    )lua");
    h.steps(3);
    h.console(R"lua(
        assert(getrenv()._G.marker == "changed", "the caller's local: " .. tostring(getrenv()._G.marker))
    )lua");
}

TEST(script_console_hooking_a_builtin_reaches_fastcalls)
{
    Harness h;
    // typeof is one the compiler calls by FASTCALL, straight to the VM's own
    // fast path, in a script whose globals are a safe environment. A hook on it
    // has to reach a script that was loaded before it and one loaded after.
    h.run(R"lua(
        _G.kind = function(x) return typeof(x) end
    )lua");
    h.console(R"lua(
        local old
        old = hookfunction(getrenv().typeof, function(x)
            if x == "marked" then
                return "hooked"
            end
            return old(x)
        end)
        assert(getrenv()._G.kind("marked") == "hooked", "a script loaded before the hook skipped it")
        assert(getrenv()._G.kind(1) == "number", "the hook broke the answers it passes on")
        assert(typeof("marked") == "hooked", "the console's own call skipped it")
    )lua");
    h.run(R"lua(
        assert(typeof("marked") == "hooked", "a script loaded after the hook skipped it")
        assert(typeof(game) == "Instance", "an Instance isn't one any more")
    )lua");
    h.console(R"lua(
        restorefunction(getrenv().typeof)
        assert(getrenv()._G.kind("marked") == "string", "restored, it still answers the hook's")
    )lua");
}

TEST(script_console_hookfunction_refuses_what_it_cant_hook)
{
    Harness h;
    CHECK(h.consoleError("hookfunction(1, print)").find("function") != std::string::npos);
    CHECK(h.consoleError("hookfunction(print, 1)").find("function") != std::string::npos);
    CHECK(h.consoleError("hookmetamethod({}, '__index', print)").find("no metatable") != std::string::npos);
    CHECK(h.consoleError("hookmetamethod(setmetatable({}, {}), '__index', print)").find("__index")
          != std::string::npos);
}

TEST(script_console_a_function_hooked_with_itself_does_what_it_did)
{
    Harness h;
    // sUNC's restorefunction, step 2: coroutine.resume hooked with itself,
    // with coroutine.yield hooked with it too.
    h.console(R"lua(
        local yield, resume = coroutine.yield, coroutine.resume
        hookfunction(yield, resume)
        restorefunction(yield)
        hookfunction(yield, resume)
        local old = hookfunction(resume, resume)
        assert(isfunctionhooked(resume), "hooked with itself, it isn't hooked")
        local ok, why = pcall(coroutine.yield, 1)
        assert(not ok and why:find("thread"), "yield isn't resume: " .. tostring(why))
        assert(select(2, old(coroutine.create(function() return 115 end))) == 115, "what it handed back")
        restorefunction(yield)
        restorefunction(resume)
        local co = coroutine.create(function(a) return coroutine.yield(a + 1) end)
        assert(select(2, coroutine.resume(co, 114)) == 115, "restored, they don't do what they did")
        assert(select(2, coroutine.resume(co, "BBB")) == "BBB", "restored, yield doesn't hand back")

        local function f(n) return n * 2 end
        hookfunction(f, f)
        assert(f(21) == 42, "a Luau function hooked with itself")
        restorefunction(f)
        assert(f(21) == 42 and not isfunctionhooked(f), "restored")
    )lua");
}

TEST(script_console_clonefunction_copies_c_functions)
{
    Harness h;
    h.console(R"lua(
        local copy = clonefunction(tostring)
        assert(copy ~= tostring, "the copy is the same function")
        assert(iscclosure(copy), "a C function's copy is a C function")
        assert(copy(12) == "12", "the copy doesn't call what the original does")
        local wrapped = newcclosure(function(x) return x * 2 end)
        assert(clonefunction(wrapped)(21) == 42, "a newcclosure's copy lost its upvalue")
    )lua");
}

TEST(script_console_getgc_finds_functions_and_asked_for_tables)
{
    Harness h;
    h.run(R"lua(
        local secret = {}
        _G.keep = function() return secret end
    )lua");
    h.console(R"lua(
        local keep = getrenv()._G.keep
        local secret = keep()
        local function has(list, value)
            for _, v in ipairs(list) do
                if rawequal(v, value) then
                    return true
                end
            end
            return false
        end
        local gc = getgc()
        assert(has(gc, keep), "getgc() is missing a live function")
        assert(has(gc, game), "getgc() is missing an Instance's userdata")
        assert(not has(gc, secret), "getgc() has a table it wasn't asked for")
        assert(has(getgc(true), secret), "getgc(true) is missing a live table")
    )lua");
}

TEST(script_game_scripts_cannot_see_the_hooks)
{
    Harness h;
    h.run(R"lua(
        for _, name in ipairs({"hookfunction", "replaceclosure", "hookmetamethod", "restorefunction",
                               "isfunctionhooked", "getgc"}) do
            assert(_G[name] == nil, name)
            assert(getfenv()[name] == nil, name)
        end
    )lua");
}
