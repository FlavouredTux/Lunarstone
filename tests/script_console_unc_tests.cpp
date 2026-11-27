// What the Escape menu's Luau tab can reach beyond the game: the instance and
// function caches, the closure helpers, crypt, the console's own debug table,
// the workspace files, the hidden members of instances and the answers only
// the application has. None of it is a game's to see, which the last test
// checks.

#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/script/PlaySession.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace lunar;

namespace {

// What a console run printed, one line each, without the warnings.
std::vector<std::string> printed(const Harness& h)
{
    std::vector<std::string> lines;
    for (const std::string& line : h.lines)
        if (!line.starts_with("warn: ") && !line.starts_with("error: "))
            lines.push_back(line);
    return lines;
}

// A throwaway data directory for the file functions, which are sandboxed to
// the workspace under LUNAR_DATA_DIR (see ConsoleFiles.cpp). Set for as long
// as it lives, and put back whatever was there before.
struct DataDir {
    std::filesystem::path root;
    std::string previous;

    explicit DataDir(std::string_view name)
    {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        if (const char* given = std::getenv("LUNAR_DATA_DIR"))
            previous = given;
        ::setenv("LUNAR_DATA_DIR", root.c_str(), 1);
    }
    ~DataDir()
    {
        if (previous.empty())
            ::unsetenv("LUNAR_DATA_DIR");
        else
            ::setenv("LUNAR_DATA_DIR", previous.c_str(), 1);
        std::filesystem::remove_all(root);
    }
    DataDir(const DataDir&) = delete;
    DataDir& operator=(const DataDir&) = delete;

    std::filesystem::path workspace() const { return root / "workspace"; }
};

} // namespace

TEST(script_console_reaches_the_instance_cache)
{
    Harness h;
    h.console(R"lua(
        local container = Instance.new("Folder")
        local part = Instance.new("Part", container)
        assert(cache.iscached(part), "a fresh instance is cached")
        -- Invalidating forgets the userdata the VM had, so the next read of
        -- the child is a different value.
        cache.invalidate(part)
        assert(not cache.iscached(part), "still cached after invalidate")
        assert(part ~= container:FindFirstChild("Part"), "invalidate did nothing")
    )lua");
}

TEST(script_console_cache_replace_hands_out_the_other_from_then_on)
{
    Harness h;
    // Where the game hands out the old instance from here on, a script gets the
    // new one; a reference it already had is still the old one [sUNC].
    h.console(R"lua(
        local frame = Instance.new("Frame")
        frame.Parent = workspace
        frame.Name = "FF"
        local part = Instance.new("Part")
        part.Name = "FF2"
        part.Transparency = 1
        cache.replace(frame, part)
        assert(workspace.FF == part, "the cache does not hand out the part")
        assert(workspace.FF.Transparency == 1, "not the part's property")
        assert(frame ~= part and frame.Name == "FF" and frame.ClassName == "Frame", "the old reference moved")
        assert(cache.iscached(frame), "the entry is gone")
        frame:Destroy()
        part:Destroy()
    )lua");
}

TEST(script_console_cloneref_is_another_value_for_one_instance)
{
    Harness h;
    h.console(R"lua(
        local part = Instance.new("Part")
        local reference = cloneref(part)
        assert(part ~= reference, "a cloneref is the same value")
        assert(compareinstances(part, reference), "it is not the same instance")
        assert(not compareinstances(part, workspace), "a different instance")
        assert(not pcall(compareinstances, {}, {}), "tables are not instances")
        reference.Name = "Through the reference"
        assert(part.Name == "Through the reference", "the write did not go through")
        assert(part:IsA("BasePart"), "members do not resolve")
    )lua");
}

TEST(script_console_clonefunction_and_the_closure_kinds)
{
    Harness h;
    h.console(R"lua(
        local function original()
            return "success"
        end
        local copy = clonefunction(original)
        assert(original() == copy(), "the clone returns something else")
        assert(original ~= copy, "the clone is the same value")
        -- The clone shares the prototype, so it is the same kind of function.
        assert(islclosure(copy) and not iscclosure(copy))

        assert(iscclosure(print) and not islclosure(print), "print is a C closure")
        assert(islclosure(original) and not iscclosure(original))

        local asC = newcclosure(original)
        assert(iscclosure(asC), "newcclosure did not make a C closure")
        assert(asC() == "success", "the C closure does not call through")
        assert(asC ~= original)
        -- Arguments and results pass through unchanged.
        local passthrough = newcclosure(function(...) return select("#", ...), ... end)
        assert(passthrough("a", "b") == 2)
        -- A name given is the one debug.info reports.
        local named = newcclosure(original, "MYR1AD")
        assert(debug.info(named, "n") == "MYR1AD" and debug.info(named, "l") == -1, "the name was not kept")
        assert(named() == "success")

        -- A clone keeps the environment the original has, set or not.
        local env = {marker = 1}
        local function reads() return marker end
        setfenv(reads, env)
        local cloned = clonefunction(reads)
        assert(getfenv(cloned) == env and cloned() == 1, "the clone changed the environment")
    )lua");
}

TEST(script_console_knows_its_own_closures)
{
    // Every closure the console makes belongs to the console; none of the
    // client's libraries' does.
    Harness h;
    h.console(R"lua(
        assert(isexecutorclosure(isexecutorclosure), "a console global is not ours")
        assert(isexecutorclosure(newcclosure(print)), "newcclosure's is not ours")
        assert(isexecutorclosure(function() end), "a Luau closure of ours is not ours")
        assert(isexecutorclosure(loadstring("return 1")), "loadstring's is not ours")
        assert(not isexecutorclosure(print), "print is ours")
        assert(not isexecutorclosure(getrenv().require))
        -- The aliases answer the same way, each its own function.
        assert(checkclosure(print) == false and isourclosure(print) == false, "an alias differs")
        assert(checkclosure(checkclosure) == true and isourclosure(checkclosure) == true)
    )lua");
    // A closure the game's own code made is not the console's, even read
    // from there: it carries the environment of the thread that made it.
    auto module = std::make_shared<ModuleScript>();
    module->setName("Theirs");
    module->setSource("return function() return 'the game' end");
    module->setParent(h.game->getService("ReplicatedStorage"));
    h.run("require(game.ReplicatedStorage.Theirs)");
    h.console(R"lua(
        local theirs = getrenv().require(game.ReplicatedStorage.Theirs)
        assert(type(theirs) == "function", "the module did not load")
        assert(not isexecutorclosure(theirs), "the game's own function is ours")
    )lua");
}

TEST(script_getscriptclosure_runs_the_code_again)
{
    Harness h;
    auto folder = std::make_shared<Folder>();
    folder->setName("UncModules");
    folder->setParent(h.game->getService("ReplicatedStorage"));
    auto module = std::make_shared<ModuleScript>();
    module->setName("Constants");
    module->setSource("return {answer = 42, name = 'the module'}");
    module->setParent(folder.get());
    h.console(R"lua(
        local module = game.ReplicatedStorage.UncModules.Constants
        local first = require(module)
        -- getscriptclosure gives a function over the module's own code
        -- without the script having run it: calling it builds a fresh table.
        local again = getscriptclosure(module)()
        assert(first ~= again, "it is the same table")
        assert(first.answer == again.answer and first.name == again.name)
        assert(getscriptfunction(module) ~= nil, "getscriptfunction differs")
        -- An instance that is not a script is not one to ask.
        assert(not pcall(getscriptclosure, game), "a non-script was accepted")
    )lua");
}

TEST(script_console_getrunningscripts_and_bytecode)
{
    Harness h;
    // Running: waiting in a loop, out of the game or destroyed too, or
    // listening for an event once its code is done, in an Actor it moved to
    // as well. Done: its code ran to its end. Not this VM's: one that started
    // in an Actor.
    auto script = std::make_shared<LocalScript>();
    script->setName("UncRunning");
    script->setSource("while task.wait() do end");
    script->setParent(h.game->workspace());
    CHECK(h.context.runScript(*script).has_value());
    auto loose = std::make_shared<LocalScript>();
    loose->setName("UncLoose");
    loose->setSource("script.Parent = nil while task.wait() do end");
    loose->setParent(h.game->workspace());
    CHECK(h.context.runScript(*loose).has_value());
    auto done = std::make_shared<LocalScript>();
    done->setName("UncDone");
    done->setSource("return 7");
    done->setParent(h.game->workspace());
    CHECK(h.context.runScript(*done).has_value());
    auto listening = std::make_shared<LocalScript>();
    listening->setName("UncListening");
    listening->setSource("workspace.ChildAdded:Connect(function() end) return 7");
    listening->setParent(h.game->workspace());
    CHECK(h.context.runScript(*listening).has_value());
    // Destroyed, it stops as the play session stops it, but a connection it
    // makes after that still runs it [sUNC's seebau].
    Connection stopping = h.game->descendantRemoving.connect([&h](Instance* instance) {
        if (instance->destroyed())
            h.context.stopScript(*instance);
    });
    auto destroyed = std::make_shared<LocalScript>();
    destroyed->setName("UncDestroyed");
    destroyed->setSource("script:Destroy() script = nil workspace.ChildAdded:Connect(function() end)");
    destroyed->setParent(h.game->workspace());
    CHECK(h.context.runScript(*destroyed).has_value());
    stopping.disconnect();
    auto moved = std::make_shared<LocalScript>();
    moved->setName("UncMoved");
    moved->setSource("script.Parent = Instance.new('Actor', workspace) workspace.ChildAdded:Connect(function() end)");
    moved->setParent(h.game->workspace());
    CHECK(h.context.runScript(*moved).has_value());
    auto actor = ClassRegistry::create("Actor");
    actor->setParent(h.game->workspace());
    auto inActor = std::make_shared<LocalScript>();
    inActor->setName("UncInActor");
    inActor->setSource("game:GetService('LogService').MessageOut:Connect(function() end)");
    inActor->setParent(actor);
    CHECK(h.context.runScript(*inActor).has_value());
    h.steps(2);
    h.console(R"lua(
        local names = {}
        for _, script in ipairs(getrunningscripts()) do
            assert(typeof(script) == "Instance", "not an Instance")
            assert(script:IsA("LuaSourceContainer"), "not a script")
            table.insert(names, script.Name)
        end
        table.sort(names)
        print(table.concat(names, ","))

        -- The bytecode the script is run from: the Luau its Source compiles
        -- to, which is stable for the same source and changes with it.
        local bytecode = getscriptbytecode(workspace.UncRunning)
        assert(type(bytecode) == "string" and #bytecode > 0, "no bytecode")
        assert(getscriptbytecode(workspace.UncRunning) == bytecode, "the bytecode is not stable")
        workspace.UncRunning.Source = "return 8"
        assert(getscriptbytecode(workspace.UncRunning) ~= bytecode, "the bytecode did not change")
        assert(#dumpstring(workspace.UncRunning) == #getscriptbytecode(workspace.UncRunning), "dumpstring differs")
        -- A script with no code has no bytecode, and no hash either.
        local empty = Instance.new("LocalScript")
        assert(getscriptbytecode(empty) == nil and getscripthash(empty) == nil, "an empty script has bytecode")
        assert(type(getscripthash(workspace.UncRunning)) == "string", "no hash")
        print("ok")
    )lua");
    const std::vector<std::string> lines = printed(h);
    CHECK_EQ(lines[0], "UncDestroyed,UncListening,UncLoose,UncMoved,UncRunning");
    CHECK_EQ(lines[1], "ok");
}

// getscriptbytecode hands out the format from before version 11, which
// decompilers know [sUNC checks the version byte is from 3 to 10]: written
// back as the bytecode a server sent, it walks, loads and runs as it did.
TEST(script_console_getscriptbytecode_is_the_older_format)
{
    Harness h;
    auto older = std::make_shared<LocalScript>();
    older->setName("Older");
    older->setSource(R"lua(
        local function add(a, b) return a + b end
        local t = {x = 1, y = 2}
        local ok, err = pcall(function() error("boom", 0) end)
        local sum = 0
        for i = 1, 10 do sum = add(sum, i) end
        print("older", sum, t.x + t.y, ok, err, vector.create(1, 2, 3).y, 7 // 2)
    )lua");
    older->setParent(h.game->workspace());
    h.console(R"lua(
        local bytecode = getscriptbytecode(workspace.Older)
        print(bytecode:byte(1))
        print((bytecode:gsub(".", function(c) return string.format("%02x", string.byte(c)) end)))
    )lua");
    std::vector<std::string> lines = printed(h);
    CHECK_EQ(lines.size(), std::size_t{2});
    if (lines.size() != 2)
        return;
    const int version = std::stoi(lines[0]);
    CHECK(version >= 3 && version <= 10);
    std::string blob;
    for (std::size_t i = 0; i + 1 < lines[1].size(); i += 2)
        blob.push_back(static_cast<char>(std::stoi(lines[1].substr(i, 2), nullptr, 16)));
    h.context.setNetworkBytecode([blob](std::string_view source) -> std::optional<std::string> {
        if (source == "sent")
            return blob;
        return std::nullopt;
    });
    auto sent = std::make_shared<LocalScript>();
    sent->setName("Sent");
    sent->setSource("sent");
    sent->setParent(h.game->workspace());
    h.lines.clear();
    CHECK(h.context.runScript(*sent).has_value());
    CHECK_EQ(printed(h), std::vector<std::string>{"older 55 3 false boom 2 3"});
}

// The script a thread runs for is the one whose code made it, whatever its
// globals say `script` is now; the console's threads run for none. A hook
// the console put on what a game script calls sees that script calling.
TEST(script_console_threads_know_their_script)
{
    Harness h;
    auto script = std::make_shared<LocalScript>();
    script->setName("Caller");
    script->setSource(R"lua(
        local thread = coroutine.running()
        script = Instance.new("Script")
        _G.callerThread = thread
        while task.wait() do
            game:GetService("ReplicatedStorage"):FindFirstChild("Nothing")
        end
    )lua");
    script->setParent(h.game->workspace());
    CHECK(h.context.runScript(*script).has_value());
    h.steps(2);
    h.console(R"lua(
        assert(getscriptfromthread(getrenv()._G.callerThread) == workspace.Caller, "not the script that made it")
        assert(getscriptfromthread(coroutine.running()) == nil, "the console's thread has a script")
        assert(getcallingscript() == nil, "the console has a calling script")
        local old
        old = hookmetamethod(game, "__namecall", function(self, ...)
            if not checkcaller() and getnamecallmethod() == "FindFirstChild" then
                _G.calling = getcallingscript()
            end
            return old(self, ...)
        end)
    )lua");
    h.steps(2);
    h.console(R"lua(
        assert(_G.calling == workspace.Caller, "the hook didn't see the caller")
    )lua");
}

TEST(script_console_lz4_round_trips)
{
    Harness h;
    // The raw block format the place files use, without a frame header.
    h.console(R"lua(
        local function roundTrip(raw)
            local block = lz4compress(raw)
            assert(type(block) == "string", "not a string")
            assert(lz4decompress(block, #raw) == raw, "did not decode back")
        end
        roundTrip("Hello, world!")          -- no repeat in it
        roundTrip(string.rep("Lunarstone", 500))  -- long runs
        roundTrip("")
        for i = 1, 64 do roundTrip(("part %d: the quick brown fox"):format(i)) end
        -- A block that decodes to a size it hasn't got is refused, not guessed.
        assert(not pcall(lz4decompress, lz4compress("Hello"), 99))
    )lua");
}

TEST(script_console_crypt_base64)
{
    Harness h;
    h.console(R"lua(
        assert(crypt.base64encode("test") == "dGVzdA==", "base64 encoding failed")
        assert(crypt.base64decode("dGVzdA==") == "test", "base64 decoding failed")
        -- Every length, including the two the padding differs for.
        for _, text in ipairs({"", "a", "ab", "abc", "abcd"}) do
            assert(crypt.base64decode(crypt.base64encode(text)) == text, "round trip")
        end
        assert(crypt.base64encode("a") == "YQ==")
        assert(crypt.base64encode("ab") == "YWI=")
        -- The spellings the executors give the same two functions.
        assert(crypt.base64.encode("test") == "dGVzdA==")
        assert(crypt.base64_decode("dGVzdA==") == "test")
        assert(base64.encode("test") == "dGVzdA==")
        assert(base64_decode("dGVzdA==") == "test")
        assert(not pcall(crypt.base64decode, "not base64!"))
    )lua");
}

TEST(script_console_crypt_encrypts_and_hashes)
{
    Harness h;
    h.console(R"lua(
        local key = crypt.generatekey()
        assert(#crypt.base64decode(key) == 32, "the key is not 32 bytes")
        local encrypted, iv = crypt.encrypt("test", key, nil, "CBC")
        assert(iv, "no IV came back")
        assert(encrypted ~= "test", "it is not encrypted")
        assert(crypt.decrypt(encrypted, key, iv, "CBC") == "test", "CBC did not round trip")

        -- An IV of the caller's own, and the other modes the table takes.
        local other, iv2 = crypt.generatekey(), crypt.generatekey()
        local text = crypt.encrypt("a longer message than one block", other, iv2, "CBC")
        assert(crypt.decrypt(text, other, iv2, "CBC") == "a longer message than one block")
        local ecb = crypt.encrypt("plain", key, nil, "ECB")
        assert(crypt.decrypt(ecb, key, nil, "ECB") == "plain", "ECB did not round trip")
        -- The same key encrypts the same bytes the same way without an IV.
        assert(crypt.encrypt("plain", key, iv, "ECB") == ecb, "ECB is not deterministic")

        local size = math.random(10, 100)
        assert(#crypt.base64decode(crypt.generatebytes(size)) == size, "wrong byte count")
        assert(crypt.generatekey() ~= crypt.generatekey(), "the same key twice")
    )lua");
}

TEST(script_console_crypt_hash)
{
    Harness h;
    // The hex digests of "test", which OpenSSL's implementations give.
    h.console(R"lua(
        local digests = {
            md5 = "098f6bcd4621d373cade4e832627b4f6",
            sha1 = "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3",
            sha256 = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
            sha384 = "768412320f7b0aa5812fce428dc4706b3cae50e02a64caa16a782249bfe8efc4"
                  .. "b7ef1ccb126255d196047dfedf17a0a9",
            ["sha3-224"] = "3797bf0afbbfca4a7bbba7602a2b552746876517a7f9b7ce2db0ae7b",
            ["sha3-256"] = "36f028580bb02cc8272a9a020f4200e346e276ae664e45ee80745574e2f5ab80",
        }
        for algorithm, digest in pairs(digests) do
            assert(crypt.hash("test", algorithm) == digest, algorithm .. " is wrong")
        end
        assert(#crypt.hash("test", "sha512") == 128, "sha512 is not 64 bytes")
        assert(#crypt.hash("test", "sha3-512") == 128, "sha3-512 is not 64 bytes")
        assert(crypt.hash("test", "sha224") == "90a3ed9e32b2aaf4c61c410eb925426119e1a9dc53d4286ade99a809", "sha224")
        assert(crypt.hash("test", "sha3-384") == "e516dabb23b6e30026863543282780a3ae0dccf05551cf0295178d7ff0f1b41e"
            .. "ecb9db3ff219007c4e097260d58621bd", "sha3-384")
        assert(not pcall(crypt.hash, "test", "crc32"))
        -- crypt.hmac(key, data, algorithm), in base64.
        assert(crypt.hmac("secret_key", "MYRIAD", "sha256") == "FaDPiTSLlbxRP5rKvomt0dzwl7zhszc46/QFPsZRGWY=", "hmac")
        -- crypt.random(size): that many printable characters, up to 1024.
        assert(#crypt.random(64) == 64 and #crypt.random(1024) == 1024 and crypt.random(0) == "", "random")
        assert(not crypt.random(64):find("[%c%s]"), "random has unprintable characters")
        assert(not pcall(crypt.random, -1) and not pcall(crypt.random, 1025), "random's size")
    )lua");
}

TEST(script_console_debug_reads_a_functions_code)
{
    Harness h;
    h.console(R"lua(
        local function constants()
            print("Hello, world!")
        end
        -- The constants are the function's own, in the order they are indexed.
        assert(debug.getconstant(constants, 1) == "print", "the first is not print")
        assert(debug.getconstant(constants, 3) == "Hello, world!", "the third is not the message")
        assert(not pcall(debug.getconstant, constants, 9), "an index past the end was accepted")
        assert(not pcall(debug.getconstant, constants, 0), "index 0 was accepted")
        assert(debug.setconstant(constants, 9, "x") == nil, "an index past the end was changed")
        -- A C closure has no code to read.
        assert(not pcall(debug.getconstant, print))

        local all = debug.getconstants(constants)
        assert(#all == 3, "there are not three constants")
        assert(all[1] == "print" and all[3] == "Hello, world!")
        -- Nothing that isn't a constant can become one.
        assert(not pcall(debug.setconstant, constants, 1, {}))
    )lua");
}

TEST(script_console_debug_changes_a_functions_constants)
{
    Harness h;
    h.console(R"lua(
        local function test()
            return "fail"
        end
        assert(test() == "fail")
        debug.setconstant(test, 1, "success")
        assert(test() == "success", "the constant was not changed")
        -- A nil takes a constant's place like any other value.
        local function nothing()
            return "fail"
        end
        debug.setconstant(nothing, 1, nil)
        assert(nothing() == nil, "the constant is not nil")
    )lua");
}

TEST(script_console_debug_reaches_the_inner_functions)
{
    Harness h;
    h.console(R"lua(
        local function outer()
            local function first()
                return true
            end
            local function second()
                return "two"
            end
            return first
        end
        local protos = debug.getprotos(outer)
        assert(#protos == 2, "there are not two inner functions")
        assert(type(protos[1]) == "function" and islclosure(protos[1]), "not a Luau function")
        assert(debug.info(protos[1], "n") == "first" and debug.info(protos[2], "n") == "second")
        assert(debug.getconstants(protos[2])[1] == "two", "not the second's constants")
        -- They are to read, not to run: no closure the code made.
        assert(not pcall(protos[1]) and not pcall(protos[2]), "an inactive one ran")
        -- The same one by index, a closure of its own...
        assert(debug.info(debug.getproto(outer, 2), "n") == "second")
        assert(not pcall(debug.getproto(outer, 1)), "an inactive one ran")
        assert(debug.getproto(outer, 1) ~= debug.getproto(outer, 1), "not a new closure")
        -- ...or the closures the running code made of it (the loader makes
        -- one of a function with no upvalues for outer to hand out).
        local made = outer()
        local live = debug.getproto(outer, 1, true)
        assert(#live == 1 and live[1] == made, "not the closure outer made")
        assert(live[1]() == true)
        assert(not pcall(debug.getproto, outer, 3), "an index past the end was accepted")
        assert(not pcall(debug.getproto, outer, 0), "index 0 was accepted")
        assert(not pcall(debug.getprotos, print), "a C closure has no inner functions")
    )lua");
}

TEST(script_console_debug_inner_functions_have_their_upvalues)
{
    Harness h;
    h.console(R"lua(
        local function outer()
            local a, b = {}, {}
            local function reads()
                return a, b
            end
        end
        -- outer never ran, so what they close over is nothing yet, and what
        -- is written there stays.
        local reads = debug.getproto(outer, 1)
        assert(#debug.getupvalues(reads) == 0 and debug.getupvalue(reads, 1) == nil, "the upvalues are not nil")
        debug.setupvalue(reads, 2, 10)
        assert(debug.getupvalue(reads, 2) == 10, "the upvalue does not keep its value")
        assert(not pcall(debug.setupvalue, reads, 3, 0), "there is no third upvalue")
    )lua");
}

TEST(script_console_debug_reads_and_writes_upvalues)
{
    Harness h;
    h.console(R"lua(
        local upvalue = function() end
        local function uses()
            return upvalue
        end
        assert(debug.getupvalue(uses, 1) == upvalue, "not the same value")
        assert(debug.getupvalues(uses)[1] == upvalue, "not the same value in the table")
        assert(not pcall(debug.getupvalue, uses, 2), "there is no second upvalue")
        assert(not pcall(debug.getupvalue, uses, 0), "index 0 was accepted")
        assert(not pcall(debug.getupvalue, print, 1) and not pcall(debug.getupvalues, print), "a C closure's")

        local function speech()
            return "fail"
        end
        local function shout()
            return speech()
        end
        assert(shout() == "fail")
        debug.setupvalue(shout, 1, function() return "success" end)
        assert(shout() == "success", "the upvalue was not changed")
        -- An index it has no upvalue at changes nothing, and says so.
        assert(not pcall(debug.setupvalue, shout, 0, 0), "index 0 was accepted")
        assert(not pcall(debug.setupvalue, shout, 2, 0), "an index past the end was accepted")
        assert(not pcall(debug.setupvalue, shout, 2147483648, 0), "a huge index was accepted")
    )lua");
}

TEST(script_console_debug_getinfo)
{
    Harness h;
    h.console(R"lua(
        local function inspect(a, b, ...)
            return a
        end
        local info = debug.getinfo(inspect)
        for _, field in ipairs({"source", "short_src", "func"}) do
            assert(type(info[field]) == "string" or type(info[field]) == "function", field)
        end
        for _, field in ipairs({"what", "name"}) do
            assert(type(info[field]) == "string", field)
            assert(#info[field] > 0, field .. " is empty")
        end
        for _, field in ipairs({"currentline", "nups", "numparams", "is_vararg"}) do
            assert(type(info[field]) == "number", field .. " is not a number")
        end
        assert(info.func == inspect, "the function is not itself")
        assert(info.what == "Lua", "not a Luau function")
        assert(info.name == "inspect", "not the function's name")
        assert(info.nups == 0, "it closes over nothing")
        assert(info.numparams == 2, "two parameters")
        assert(info.is_vararg == 1, "it is variadic")

        -- A C closure answers as one.
        local c = debug.getinfo(print)
        assert(c.what == "C", "not a C function")
        assert(c.name == "print", "not named print")

        -- A level gives the same fields for a running frame.
        local level = debug.getinfo(1)
        assert(type(level.what) == "string" and type(level.func) == "function", "level")
        -- A level with no call under it answers with nothing.
        assert(select("#", debug.getinfo(99)) == 0, "a level past the call answered")
    )lua");
}

TEST(script_console_debug_reads_and_writes_the_stack)
{
    Harness h;
    // A frame's registers: what the running code keeps its values in. This is
    // what makes a hooked function's return value changeable.
    h.console(R"lua(
        local function registers(a, b)
            assert(debug.getstack(1, 1) == "first", "the first register")
            assert(debug.getstack(1, 2) == "second", "the second register")
            local whole = debug.getstack(1)
            assert(whole[1] == "first" and whole[2] == "second", "the whole frame")
            assert(not pcall(debug.getstack, 1, 0), "index 0 was accepted")
            assert(not pcall(debug.getstack, 99, 1), "a level past the call was accepted")
        end
        registers("first", "second")

        -- Writing the register a return value goes out through.
        local function ret()
            return "fail", debug.setstack(1, 1, "success")
        end
        assert(ret() == "success", "the register was not changed")
        assert(not pcall(debug.setstack, 1, 1), "a missing value was accepted")
    )lua");
    // sUNC's: the frame above, its locals in the order they were made.
    h.console(R"lua(
        local function dummy_function()
            return "Hello"
        end
        local var = 5
        var += 1
        ;(function()
            assert(debug.getstack(2)[1]() == "Hello", "not the caller's first local")
            assert(debug.getstack(2)[2] == 6, "not the caller's second local")
        end)()
    )lua");
    h.console(R"lua(
        local outer_value = 10
        local function inner_function()
            outer_value += 9
            debug.setstack(2, 1, 100)
        end
        inner_function()
        assert(outer_value == 100, "the caller's local was not changed")
    )lua");
}

TEST(script_console_debug_keeps_the_vms_own_library)
{
    // The console's debug table is the VM's with the console's extras on it;
    // the game's scripts keep the library as it was.
    Harness h;
    h.console(R"lua(
        assert(debug.traceback, "the VM's traceback is missing")
        assert(type(debug.info) == "function", "the VM's info is missing")
        assert(type(debug.getupvalue) == "function", "the VM's getupvalue is missing")
        assert(type(debug.getconstant) == "function", "getconstant is missing")
    )lua");
    h.run(R"lua(
        assert(debug.traceback, "the VM's traceback is missing")
        assert(debug.getconstant == nil and debug.getstack == nil and debug.getinfo == nil)
    )lua");
}

TEST(script_console_reads_and_writes_the_workspace)
{
    DataDir data{"lunarstone-console-files"};
    Harness h;
    h.console(R"lua(
        makefolder("notes")
        writefile("notes/hello.txt", "success")
        assert(readfile("notes/hello.txt") == "success", "the file did not read back")
        -- Writing replaces what was there; appending adds to it.
        writefile("notes/hello.txt", "bye")
        assert(readfile("notes/hello.txt") == "bye", "the file was not replaced")
        appendfile("notes/hello.txt", " now")
        assert(readfile("notes/hello.txt") == "bye now", "the text was not appended")
        -- A file in a folder that isn't there yet is written all the same.
        writefile("notes/deep/down/here.txt", "made")
        assert(isfolder("notes/deep/down"), "the folder was not made")
        assert(readfile("notes/deep/down/here.txt") == "made", "it did not read back")
        -- Binary data survives the round trip.
        writefile("notes/bytes.bin", "\0\1\2end")
        assert(readfile("notes/bytes.bin") == "\0\1\2end", "the bytes did not survive")

        assert(isfolder("notes") and not isfile("notes"), "the folder is not a folder")
        assert(isfile("notes/hello.txt") and not isfolder("notes/hello.txt"), "the file is not a file")
        assert(not isfile("notes/nothing.txt"), "a file that is not there")
        assert(not isfolder("notes/nothing"), "a folder that is not there")
    )lua");
}

TEST(script_console_lists_and_deletes)
{
    DataDir data{"lunarstone-console-list"};
    Harness h;
    h.console(R"lua(
        makefolder("listfiles")
        writefile("listfiles/test_1.txt", "success")
        writefile("listfiles/test_2.txt", "success")
        local files = listfiles("listfiles")
        assert(#files == 2, "two files, not " .. #files)
        -- The paths are the workspace's own, so they read as they are.
        table.sort(files)
        assert(files[1] == "listfiles/test_1.txt", "not the path given: " .. files[1])
        assert(isfile(files[1]), "the path is not a file")
        assert(readfile(files[1]) == "success", "it did not read back")

        makefolder("listfiles_2/test_1")
        makefolder("listfiles_2/test_2")
        local folders = listfiles("listfiles_2")
        assert(#folders == 2, "two folders, not " .. #folders)
        assert(isfolder(folders[1]), "the path is not a folder")
        -- A folder that is not there has nothing in it.
        assert(#listfiles("listfiles/nothing") == 0, "a folder that is not there listed something")
        assert(not pcall(listfiles, "listfiles/test_1.txt"), "a file listed as a folder")
        -- An empty path is the workspace itself, but it can't be deleted.
        assert(isfolder(""), "the workspace is not a folder")
        assert(table.find(listfiles(""), "listfiles"), "the workspace does not list its folder")
        assert(not pcall(delfolder, ""), "the workspace was deleted")
        assert(isfolder("listfiles"), "the workspace lost its folder")

        -- delfile takes one file, delfolder takes what is in it.
        delfile("listfiles/test_1.txt")
        assert(not isfile("listfiles/test_1.txt"), "the file is still there")
        assert(isfile("listfiles/test_2.txt"), "the other file went too")
        delfolder("listfiles")
        assert(not isfolder("listfiles"), "the folder is still there")
        assert(not pcall(delfolder, "listfiles"), "deleting it twice was allowed")
    )lua");
}

TEST(script_console_file_functions_stay_in_the_workspace)
{
    DataDir data{"lunarstone-console-sandbox"};
    // A folder outside the workspace, and a link out of it: what a script
    // behind them would be reading.
    const std::filesystem::path outside = std::filesystem::temp_directory_path() / "lunarstone-console-outside";
    std::error_code ec;
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::ofstream secret(outside / "secret.txt");
    secret << "not yours";
    secret.close();
    // The link the console code reads through.
    std::filesystem::create_directories(data.workspace());
    std::filesystem::create_directory_symlink(outside, data.workspace() / "link", ec);

    Harness h;
    h.console(R"lua(
        -- Absolute paths are refused whatever they say.
        assert(not pcall(readfile, "/etc/passwd"))
        assert(not pcall(readfile, "/etc/hostname"))
        assert(not pcall(writefile, "/tmp/lunarstone-console-escape.txt", "no"))
        -- A ".." that climbs out is not in the workspace either.
        assert(not pcall(readfile, "../secret.txt"))
        assert(not pcall(writefile, "../../lunarstone-console-escape.txt", "no"))
        assert(not pcall(listfiles, ".."))
        assert(not pcall(makefolder, "../elsewhere"))
        -- A link out of the workspace leads nowhere the sandbox follows.
        assert(not pcall(readfile, "link/secret.txt"))
        assert(not pcall(writefile, "link/escape.txt", "no"))
        -- An empty path is the workspace itself, which is no file.
        assert(not pcall(readfile, ""))
        -- Nor a Windows path, which an executor there refuses.
        assert(not pcall(writefile, "C:/escape.txt", "no") and not pcall(makefolder, "C:/escape"))
        assert(not pcall(writefile, "\\\\server\\escape.txt", "no"))
        -- Inside it, everything works as before.
        writefile("inside.txt", "yes")
        assert(readfile("inside.txt") == "yes")
    )lua");
    CHECK(!std::filesystem::exists(outside / "escape.txt", ec));
    CHECK(!std::filesystem::exists(data.workspace() / "C:", ec));
    std::filesystem::remove_all(outside);

    // A sibling whose name starts as the workspace's does is outside it too.
    const std::filesystem::path sibling = data.workspace().parent_path() / (data.workspace().filename().string() + "2");
    std::filesystem::create_directories(sibling);
    std::ofstream(sibling / "secret.txt") << "not yours";
    h.console(std::format(R"lua(
        assert(not pcall(readfile, "../{0}/secret.txt"), "the sibling's file was read")
        assert(not pcall(writefile, "../{0}/escape.txt", "no"), "the sibling was written to")
    )lua", sibling.filename().string()));
    CHECK(!std::filesystem::exists(sibling / "escape.txt", ec));
    std::filesystem::remove_all(sibling);
}

TEST(script_console_loadfile_and_dofile)
{
    DataDir data{"lunarstone-console-load"};
    Harness h;
    h.console(R"lua(
        writefile("chunk.luau", "return ... + 1")
        local f = loadfile("chunk.luau")
        assert(type(f) == "function", "no function")
        assert(f(1) == 2, "it does not run")
        -- A chunk that doesn't compile gives nil and the compiler's message,
        -- as loadstring does.
        writefile("bad.luau", "local = 5")
        local callback, message = loadfile("bad.luau")
        assert(callback == nil and type(message) == "string", "a compiler error was raised")
        assert(not pcall(loadfile, "nothing.luau"), "a file that is not there was loaded")
        -- The chunk runs in the console's globals, like loadstring's.
        writefile("sets.luau", "globalFromAFile = getidentity()")
        loadfile("sets.luau")()
        assert(globalFromAFile == 8, "it did not run in the console")

        -- dofile runs it and answers with its results.
        writefile("answers.luau", "return 6, 7")
        local six, seven = dofile("answers.luau")
        assert(six == 6 and seven == 7, "the results did not come back")
        writefile("writes.luau", "fileRan = true")
        dofile("writes.luau")
        assert(fileRan, "the file did not run")
    )lua");
}

TEST(script_console_getcustomasset_gives_an_id_the_client_loads)
{
    DataDir data{"lunarstone-console-asset"};
    Harness h;
    std::string id;
    h.console(R"lua(
        writefile("custom.txt", "the file's own bytes")
        id = getcustomasset("custom.txt")
        print(id)
        assert(type(id) == "string" and #id > 0, "no id")
        -- The same file answers with the same id, another file with another.
        assert(getcustomasset("custom.txt") == id, "the id is not stable")
        writefile("other.txt", "different bytes")
        assert(getcustomasset("other.txt") ~= id, "two files share an id")
        assert(not pcall(getcustomasset, "../elsewhere.txt"), "outside the workspace")
    )lua");
    for (const std::string& line : printed(h))
        if (line.starts_with("rbx"))
            id = line;
    // The id is one the asset loader resolves from its own cache, with
    // nothing fetched from the network.
    CHECK(id.starts_with("rbxasset://customassets/"));
    lunar::assets::AssetCache cache{[] {
        lunar::assets::AssetConfig config = lunar::assets::AssetConfig::fromEnvironment();
        config.contentDir.clear();
        config.download = false;
        return config;
    }()};
    auto bytes = cache.fetch(id);
    CHECK(bytes.has_value());
    CHECK_EQ(**bytes, "the file's own bytes");
    // The file it put there is the console's to clean up after.
    std::error_code ec;
    std::filesystem::remove(
        lunar::assets::AssetConfig::fromEnvironment().cacheDir / id.substr(std::string_view("rbxasset://").size()), ec);
}

TEST(script_console_reaches_the_hidden_members)
{
    Harness h;
    h.console(R"lua(
        local fire = Instance.new("Fire")
        -- size_xml is one of the members Roblox keeps from scripts: its own
        -- field behind the XML, which the engine has no value for.
        local value, hidden = gethiddenproperty(fire, "size_xml")
        assert(value == 5, "size_xml is " .. tostring(value) .. ", not 5")
        assert(hidden == true, "it is not reported as hidden")
        -- A public member of the same class is not hidden. (The engine has no
        -- value for it either -- a Fire's Heat reads as 0 -- which is a gap of
        -- its own and not what these are for.)
        local heat, isHidden = gethiddenproperty(fire, "Heat")
        assert(type(heat) == "number", "Heat is not a number")
        assert(isHidden == false, "Heat is hidden")
        -- An engine property reads through as it is.
        local part = Instance.new("Part")
        assert(gethiddenproperty(part, "Name") == "Part", "Name did not read")
        assert(not select(2, gethiddenproperty(part, "Name")), "Name is hidden")
        assert(not pcall(gethiddenproperty, part, "Nonsense"))

        -- Setting one answers whether it was hidden, and reads back.
        assert(sethiddenproperty(fire, "size_xml", 10) == true, "not reported as hidden")
        assert(gethiddenproperty(fire, "size_xml") == 10, "the value did not stick")
        assert(sethiddenproperty(fire, "Size", 7) == false, "Size is hidden")
        assert(gethiddenproperty(fire, "Size") == 7, "the value did not stick")
        assert(fire.Size == 7, "a script reads what the console set")
        -- A member of an instance the engine does model is set like a script's.
        part.Name = "first"
        sethiddenproperty(part, "Name", "second")
        assert(part.Name == "second", "the property did not change")
    )lua");
}

TEST(script_console_changes_what_is_scriptable)
{
    Harness h;
    h.console(R"lua(
        local fire = Instance.new("Fire")
        -- size_xml is one Roblox keeps from scripts, Fire.Size is not.
        assert(isscriptable(fire, "size_xml") == false, "a hidden member is scriptable")
        assert(isscriptable(fire, "Size") == true, "a public member is not scriptable")
        -- setscriptable answers what it was, and the change is the console's.
        assert(setscriptable(fire, "size_xml", true) == false, "it was already scriptable")
        assert(isscriptable(fire, "size_xml") == true, "it is still not scriptable")
        assert(setscriptable(fire, "size_xml", false) == true, "the change did not take")
        assert(isscriptable(fire, "size_xml") == false, "it is still scriptable")
        -- Another instance of the same class is untouched.
        assert(isscriptable(Instance.new("Fire"), "size_xml") == false, "the change carried over")
        assert(not pcall(isscriptable, fire, "Nonsense"))

        -- Indexing reaches a hidden member only while it is set scriptable, and
        -- then reads the property it is saved for [sUNC's formFactorRaw].
        local part = Instance.new("Part")
        assert(not pcall(function() return part.formFactorRaw end), "a hidden member was indexed")
        assert(setscriptable(part, "formFactorRaw", true) == false)
        assert(part.formFactorRaw == part.FormFactor, "not the part's FormFactor")
        setscriptable(part, "formFactorRaw", false)
        assert(not pcall(function() return part.formFactorRaw end), "still scriptable")
        -- One with nothing to read stays out of reach [sUNC: siz].
        setscriptable(part, "siz", true)
        assert(not pcall(function() return part.siz end), "siz was read")
        -- gethiddenproperty still reads it, as the Size it loads [Myriad].
        local siz, sizHidden = gethiddenproperty(part, "siz")
        assert(siz == part.Size and sizHidden == true, "siz is not the part's Size")
        -- The lower-case spelling of a hidden member is the capitalised one.
        part.Size = Vector3.new(1, 2, 3)
        assert(part.size == part.Size, "part.size is not Size")
        -- gethiddenproperty reads the property a saved name stands for.
        local humanoid = Instance.new("Humanoid")
        humanoid.Health = 42
        local health, hidden = gethiddenproperty(humanoid, "Health_XML")
        assert(health == 42 and hidden == true, "Health_XML is not the humanoid's Health")
        -- A Fire's saved size is its Size [sUNC].
        local fire = Instance.new("Fire")
        assert(sethiddenproperty(fire, "size_xml", 1) == true)
        assert(gethiddenproperty(fire, "size_xml") == 1 and fire.Size == 1, "size_xml is not the fire's Size")
        fire.Size = 7
        assert(gethiddenproperty(fire, "size_xml") == 7, "Size is not the fire's size_xml")
        -- A new part is a Brick, the way sUNC reads it.
        assert(tostring(Instance.new("Part").FormFactor) == "Enum.FormFactor.Brick", "a new part is not a Brick")
    )lua");
    // A game script can't reach one, whatever the console did.
    h.run(R"lua(
        local fire = Instance.new("Fire")
        assert(not pcall(function() return fire.size_xml end), "a game script read a hidden member")
    )lua");
}

TEST(script_console_reads_what_the_server_sent_for_an_unmodelled_property)
{
    Harness h;
    // The engine doesn't model a MeshPart's AeroMeshData, but keeps what came
    // for it [sUNC encodes it and compares].
    auto mesh = std::make_shared<MeshPart>();
    mesh->setName("Mesh");
    mesh->setUnmodelled("AeroMeshData", Value(std::string("\x01\0\0\0mesh", 8)));
    mesh->setParent(h.game->workspace());
    h.console(R"lua(
        local data, hidden = gethiddenproperty(workspace.Mesh, "AeroMeshData")
        assert(data == "\1\0\0\0mesh" and hidden == true, "not what was sent")
        -- A copy keeps it.
        assert(gethiddenproperty(workspace.Mesh:Clone(), "AeroMeshData") == data, "the copy lost it")
        -- The tags read as a file saves them, a zero byte between each.
        local part = Instance.new("Part")
        part:AddTag("one")
        part:AddTag("two")
        assert(gethiddenproperty(part, "Tags") == "one\0two", "not the tags")
    )lua");
}

TEST(script_console_reads_a_callback_members_function)
{
    Harness h;
    h.console(R"lua(
        local bindable = Instance.new("BindableFunction")
        assert(getcallbackvalue(bindable, "OnInvoke") == nil, "nothing was set")
        local function handler(a)
            return a * 2
        end
        bindable.OnInvoke = handler
        assert(getcallbackvalue(bindable, "OnInvoke") == handler, "not the function that was set")
        -- It is the one the instance calls.
        assert(bindable:Invoke(21) == 42, "the callback does not answer")
        -- A member that is not a callback has none.
        assert(getcallbackvalue(bindable, "Name") == nil)
    )lua");
    // A game script may set one but not read it back, as the client has it.
    h.run(R"lua(
        local bindable = Instance.new("BindableFunction")
        bindable.OnInvoke = function() return "the game's" end
        assert(bindable:Invoke() == "the game's")
        local ok, message = pcall(function() return bindable.OnInvoke end)
        assert(not ok, "a game script could read it back")
    )lua");
}

TEST(script_console_gethui_is_a_gui_the_client_draws)
{
    Harness h;
    h.console(R"lua(
        local hui = gethui()
        assert(typeof(hui) == "Instance", "not an Instance")
        -- A container for ScreenGuis, as UNC's example uses it and Infinite
        -- Yield does: a ScreenGui in a ScreenGui wouldn't show, one in a
        -- Folder does (ui_screen_guis_in_folders_show).
        assert(hui:IsA("Folder"), "not a Folder")
        -- CoreGui is the service the client's own GUI lives in, which the GUI
        -- engine draws over everything else.
        assert(hui.Parent == game:GetService("CoreGui"), "not inside CoreGui")
        assert(gethui() == hui, "a second call gave a different one")
        local gui = Instance.new("ScreenGui")
        gui.Name = "From the console"
        gui.Parent = hui
        assert(gui.Parent == hui)
        print("hui", hui:GetFullName(), gui:GetFullName())
    )lua");
    CHECK_EQ(printed(h).at(0), "hui CoreGui.ConsoleGui CoreGui.ConsoleGui.From the console");
}

TEST(script_console_connection_tables_have_the_executors_fields)
{
    Harness h;
    h.run(R"lua(
        local bindable = Instance.new("BindableEvent")
        bindable.Name = "uncBindable"
        bindable.Parent = workspace
        bindable.Event:Connect(function() end)
    )lua");
    h.console(R"lua(
        local connection = getconnections(game.Workspace.uncBindable.Event)[1]
        local types = {
            Enabled = "boolean",
            ForeignState = "boolean",
            LuaConnection = "boolean",
            Function = "function",
            Thread = "thread",
            Fire = "function",
            Defer = "function",
            Disconnect = "function",
            Disable = "function",
            Enable = "function",
        }
        for field, kind in pairs(types) do
            assert(type(connection[field]) == kind, field .. " is a " .. type(connection[field]))
        end
        -- The game connected it, so it runs as the game's and not as this one.
        assert(connection.ForeignState == true, "the game's connection is not foreign")
        assert(connection.LuaConnection == true, "the callback is not Luau")
        -- One thread per connection, not one per question.
        local thread = connection.Thread
        assert(getconnections(game.Workspace.uncBindable.Event)[1].Thread == thread, "a second thread")
        -- Defer is Fire from the next resumption point, so nothing has run yet.
        local seen = 0
        connection.Function = function() seen = seen + 1 end
        connection.Defer()
        assert(seen == 0, "the handler ran at once")
        task.wait()
        assert(seen == 1, "the handler did not run when it was due")
    )lua");
}

TEST(script_console_a_connections_thread_is_the_one_that_connected_it)
{
    Harness h;
    // sUNC's getconnections connects from a game script's thread it caught
    // in a __namecall hook, and looks for that thread.
    h.run(R"lua(
        local bindable = Instance.new("BindableEvent")
        bindable.Name = "byThread"
        bindable.Parent = workspace
        _G.connector = coroutine.create(function() bindable.Event:Connect(function() end) end)
        coroutine.resume(_G.connector)
    )lua");
    h.console(R"lua(
        local connection = getconnections(workspace.byThread.Event)[1]
        assert(connection.Thread == getrenv()._G.connector, "not the game's thread that connected it")
        local event = Instance.new("BindableEvent")
        local ran
        event.Event:Connect(function() ran = coroutine.running() end)
        local mine = getconnections(event.Event)[1]
        assert(mine.Thread == coroutine.running(), "not the console's thread")
        -- Newest first, as Roblox keeps them.
        local co = coroutine.create(function() event.Event:Connect(function() end) end)
        coroutine.resume(co)
        assert(getconnections(event.Event)[1].Thread == co, "the newest isn't first")
        assert(getconnections(event.Event)[2].Thread == coroutine.running(), "the oldest isn't last")
        event:Fire()
        assert(ran and ran ~= mine.Thread, "the handler didn't run in a thread of its own")
    )lua");
}

TEST(script_console_getnamecallmethod_outside_a_call)
{
    Harness h;
    // It reads what the VM's NAMECALL left on the thread, as the executors'
    // does: nothing on a thread that has made no method call, and the last
    // one's name after one (script_console_hook_tests has it inside a hook).
    h.console(R"lua(
        assert(getnamecallmethod() == nil, "not nil before any method call")
        game:GetService("Lighting")
        assert(getnamecallmethod() == "GetService", "not the last method call's name")
        -- A method call on any of Roblox's types counts, one that fails too
        -- [Myriad], and it stays the thread's across a wait; a new thread
        -- starts with none, and a plain call of the method sets none.
        local color = Color3.new()
        pcall(function() color:MYRIAD1() end)
        local spawned = "unset"
        task.spawn(function()
            spawned = getnamecallmethod()
            pcall(function() color.MYRIAD1(color) end)
            spawned = spawned or getnamecallmethod()
        end)
        task.wait()
        assert(getnamecallmethod() == "MYRIAD1", tostring(getnamecallmethod()))
        assert(spawned == nil, "it leaked into a new thread: " .. tostring(spawned))
        UDim2.new():Lerp(UDim2.new(), 1)
        assert(getnamecallmethod() == "Lerp", "a UDim2's method call")
        -- Its methods still read as themselves when they fail.
        local _, message = pcall(function() return color:Lerp("x", 1) end)
        assert(message:find("to 'Lerp'"), message)
        print("ok")
    )lua");
    h.steps(2);
    CHECK_EQ(printed(h).back(), "ok");
}

TEST(script_console_reaches_the_window_around_it)
{
    Harness h;
    // Without a window there is nothing focused, no clipboard and no frame loop
    // to cap, which each of these says rather than failing.
    h.console(R"lua(
        assert(type(isrbxactive()) == "boolean", "not a boolean")
        assert(isrbxactive() == false, "focused without a window")
        assert(type(isgameactive) == "function", "the alias is missing")

        assert(getfpscap() == 0, "the cap is not 0 to begin with")
        setfpscap(60)
        assert(getfpscap() == 60, "the cap did not take")
        setfpscap(-5)
        assert(getfpscap() == 0, "a negative cap was taken")

        -- A clipboard needs a compositor, which this context has not.
        assert(pcall(setclipboard, "text"))
        assert(pcall(toclipboard, "text"))
        assert(pcall(setrbxclipboard, "text"))

        assert(messagebox("Hello from the console", "Title") == 1, "not 1")
    )lua");
    CHECK_EQ(printed(h).at(0), "Title: Hello from the console");
}

TEST(script_console_reaches_the_window_through_the_hooks)
{
    // What an application installs is what the console gets (see
    // ScriptContext::setActive and its neighbours).
    Harness h;
    bool focused = false;
    std::string clipboard;
    int cap = 0;
    h.context.setActive([&focused] { return focused; });
    h.context.setClipboard([&clipboard](std::string_view text) { clipboard = text; });
    h.context.setFrameCap([&cap](int fps) { cap = fps; });

    focused = true;
    h.console(R"lua(
        assert(isrbxactive() == true, "the window is focused and it does not say so")
        setclipboard("from the console")
        assert(type(toclipboard) == "function" and type(setrbxclipboard) == "function", "an alias is missing")
        setfpscap(144)
        assert(getfpscap() == 144, "not kept")
    )lua");
    CHECK_EQ(clipboard, "from the console");
    CHECK_EQ(cap, 144);

    // And the game's scripts none of it.
    focused = false;
    h.run(R"lua(
        assert(isrbxactive == nil and setfpscap == nil and messagebox == nil)
    )lua");
}

TEST(script_console_rconsole_is_the_terminal)
{
    // The external console is the terminal the process runs in: standard
    // output for what's printed, standard input for rconsoleinput. Both are
    // files here, which is no terminal, so nothing is coloured.
    std::FILE* in = std::tmpfile();
    std::FILE* out = std::tmpfile();
    std::fputs("typed here\r\nsecond line\n", in);
    std::rewind(in);
    std::fflush(stdout);
    const int savedIn = ::dup(STDIN_FILENO);
    const int savedOut = ::dup(STDOUT_FILENO);
    ::dup2(::fileno(in), STDIN_FILENO);
    ::dup2(::fileno(out), STDOUT_FILENO);

    Harness h;
    h.console(R"lua(
        rconsoleprint("plain\27]0;sneaky\7 ") -- no escape sequence gets through
        rconsoleinfo("info") rconsolewarn("warn") rconsoleerr(42)
        rconsoleclear() rconsolename("title") rconsoleshow() rconsolehide()
        assert(rconsolesettitle == rconsolename)
        task.spawn(function() getgenv().typed = rconsoleinput() end)
    )lua");
    // The line is read on a thread of its own, and handed over on a frame.
    for (int i = 0; i < 500; ++i) {
        h.steps(1);
        h.console("getgenv().done = getgenv().typed ~= nil");
        h.lines.clear();
        h.console("print(getgenv().done)");
        if (printed(h) == std::vector<std::string>{"true"})
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    h.lines.clear();
    h.console("print(getgenv().typed)");

    std::fflush(stdout);
    ::dup2(savedIn, STDIN_FILENO);
    ::dup2(savedOut, STDOUT_FILENO);
    ::close(savedIn);
    ::close(savedOut);
    std::rewind(out);
    std::string written;
    for (int c = 0; (c = std::fgetc(out)) != EOF;)
        written.push_back(static_cast<char>(c));
    std::fclose(in);
    std::fclose(out);

    CHECK_EQ(printed(h), (std::vector<std::string>{"typed here"}));
    CHECK_EQ(written, std::string("plain]0;sneaky [INFO] info\n[WARNING] warn\n[ERROR] 42\n"));
}

TEST(script_console_file_dialogs_give_what_the_user_picked)
{
    // The dialog is a program of its own; this one answers each call with the
    // next of its answer files (and exit status: 1 is cancelled) and keeps the
    // arguments it was given.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / std::format("lunarstone-dialogs-{}", ::getpid());
    fs::remove_all(dir);
    fs::create_directories(dir / "picks");
    auto put = [](const fs::path& path, std::string_view text) { std::ofstream(path) << text; };
    const std::string picks = (dir / "picks").string();
    put(dir / "picks" / "dialog",
        std::format("#!/bin/sh\nn=$(cat {0}/count 2>/dev/null || echo 0)\necho $((n + 1)) > {0}/count\n"
                    "printf '%s\\n' \"$@\" >> {0}/args\nprintf '%s' \"$(cat {0}/answer.$n)\"\n"
                    "exit $(cat {0}/status.$n 2>/dev/null || echo 0)\n",
                    picks));
    fs::permissions(dir / "picks" / "dialog", fs::perms::owner_all);
    const fs::path folder = dir / "home";
    fs::create_directories(folder);
    put(folder / "notes.txt", "hello");
    put(folder / "pic.png", "png");
    const std::vector<std::string> answers{
        (folder / "notes.txt").string(),
        (folder / "pic.png").string(),
        "",
        folder.string(),
        (folder / "out").string(),
        (folder / "notes.txt").string() + "\n" + (folder / "pic.png").string(),
    };
    for (std::size_t i = 0; i < answers.size(); ++i)
        put(dir / "picks" / std::format("answer.{}", i), answers[i]);
    put(dir / "picks" / "status.2", "1");
    ::setenv("LUNARSTONE_FILE_DIALOG", (dir / "picks" / "dialog").c_str(), 1);

    Harness h;
    h.console(R"lua(
        task.spawn(function()
            local seen = {}
            local f = openfiledialog({ title = "Pick", extensionFilter = { "txt" } })
            assert(f:GetName() == "notes.txt" and f:IsFile() and not f:IsFolder(), "not the file")
            assert(f:Read() == "hello", "not its content")
            f:Write("a") f:Append("b")
            assert(f:Read() == "ab", "the writes didn't land")
            assert(not pcall(function() return f.List end), "a file has no List")
            local ok, err = pcall(openfiledialog, { extensionFilter = { "txt" } })
            assert(not ok and err:find("pic.png is not one of the file types"), err)
            assert(openfolderdialog() == nil, "cancelling isn't nil")
            local home = openfolderdialog({ defaultPath = "C:/Users/Public/Desktop" })
            assert(home:IsFolder() and home:GetName() == "home", "not the folder")
            local sub = home:MakeFolder("sub")
            assert(sub:IsFolder() and sub:GetName() == "sub", "not the new folder")
            assert(not pcall(home.MakeFolder, home, ".."), "made a folder outside")
            for _, item in home:List() do seen[item:GetName()] = item:IsFolder() end
            assert(seen["notes.txt"] == false and seen.sub == true, "List missed some")
            sub:Delete()
            local saved = savefiledialog("data", { suggestedName = "cfg", defaultExtension = "json" })
            assert(saved:GetName() == "out.json" and saved:Read() == "data", "not saved")
            saved:Delete()
            assert(not pcall(saved.Read, saved), "deleted, and still read")
            local files = openfilesdialog()
            assert(#files == 2 and files[2]:GetName() == "pic.png", "not both files")
            getgenv().done = "yes"
        end)
    )lua");
    std::string state;
    for (int i = 0; i < 2000 && state != "yes"; ++i) {
        h.steps(1);
        h.lines.clear();
        h.console("print(getgenv().done)");
        state = printed(h).empty() ? "" : printed(h).front();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ::unsetenv("LUNARSTONE_FILE_DIALOG");
    std::ifstream argsFile(dir / "picks" / "args");
    const std::string args{std::istreambuf_iterator<char>(argsFile), {}};
    fs::remove_all(dir);

    CHECK_EQ(state, std::string("yes"));
    CHECK(args.contains("--file-selection\n--title=Pick\n"));
    CHECK(args.contains("--file-filter=Files | *.txt\n"));
    CHECK(args.contains("--directory\n"));
    CHECK(args.contains("--save\n"));
    CHECK(args.contains("cfg\n"));
    CHECK(args.contains("--multiple\n"));
}

TEST(script_console_runs_code_on_actors)
{
    Harness h;
    auto actor = ClassRegistry::create("Actor");
    actor->setName("Act");
    actor->setParent(h.game->workspace());
    auto inActor = std::make_shared<LocalScript>();
    inActor->setSource(R"lua(
        task.desynchronize()
        workspace:SetAttribute("ScriptParallel", true)
        task.synchronize()
    )lua");
    inActor->setParent(actor);
    CHECK(h.context.runScript(*inActor).has_value());
    // Outside an actor the phase doesn't change.
    h.run("task.desynchronize() workspace:SetAttribute('GameParallel', true)");
    h.console(R"lua(
        local actors = getactors()
        assert(#actors == 1 and actors[1] == workspace.Act, "not the running actor")
        assert(not isparallel(), "the console's thread isn't parallel")
        task.desynchronize()
        assert(not isparallel(), "a thread outside an actor stays serial")

        local sent = { inner = { 1 } }
        getgenv().sent = sent
        run_on_actor(workspace.Act, [[
            local data, n = ...
            assert(select("#", ...) == 3, "not every argument")
            workspace:SetAttribute("Copied", data.inner ~= getgenv().sent.inner and data.inner[1] == 1 and data.game == game)
            workspace:SetAttribute("Second", n)
            task.desynchronize()
            local parallel = isparallel()
            task.spawn(function() workspace:SetAttribute("ChildParallel", isparallel()) end)
            task.synchronize()
            workspace:SetAttribute("Phases", parallel and not isparallel())
            get_comm_channel(getgenv().id):Fire("hi", game, nil)
        ]], { inner = sent.inner, game = game }, 2, nil)
        assert(not pcall(run_on_actor, workspace, "") , "a non-actor")
        assert(not pcall(run_on_actor, workspace.Act, "this is not luau"), "code that doesn't compile")

        local id, channel = create_comm_channel()
        assert(type(id) == "number" and math.floor(id) == id, "not a whole number")
        assert(get_comm_channel(id) == channel and channel:IsA("BindableEvent"), "not the same channel")
        assert(get_comm_channel(id + 0.5) == nil and get_comm_channel(1e9) == nil, "a channel that isn't")
        getgenv().id = id
        channel.Event:Once(function(...) getgenv().heard = table.pack(...) end)
        run_on_actor(workspace.Act, "get_comm_channel(getgenv().id):Fire('hi', game, nil)")
    )lua");
    h.steps(2);
    h.console(R"lua(
        local heard = getgenv().heard
        assert(heard and heard[1] == "hi" and heard[2] == game and heard.n == 3, "the channel didn't carry it")
        print(workspace:GetAttribute("ScriptParallel"), workspace:GetAttribute("GameParallel"))
        print(workspace:GetAttribute("Second"), workspace:GetAttribute("ChildParallel"), workspace:GetAttribute("Phases"))
    )lua");
    CHECK_EQ(printed(h), (std::vector<std::string>{"true true", "2 true true"}));
    h.lines.clear();
    // The first run's data table was a copy: it didn't get the console's.
    h.console("print(workspace:GetAttribute('Copied'))");
    CHECK_EQ(printed(h), (std::vector<std::string>{"true"}));
}

TEST(script_console_presses_keys_and_moves_the_mouse_through_the_window)
{
    // keypress and its neighbours reach the window as input of its own, by
    // the hook the application installs; without one they do nothing.
    Harness h;
    h.console("keypress(Enum.KeyCode.W) mouse1click() mousemoverel(3, 4)");

    using Input = lunar::ScriptContext::HostInput;
    std::vector<Input> sent;
    h.context.setInput([&sent](const Input& input) { sent.push_back(input); });
    h.console(R"lua(
        keypress(Enum.KeyCode.W)  -- an item
        keyrelease("W")           -- its name
        keypress(0x31)            -- the executors' virtual-key code: 1
        keypress(0xA0)            -- left shift
        mouse1press() mouse1release()
        mouse2click()
        mousemoverel(12, -5)
        mousemoveabs(640, 360)
        keyclick(Enum.KeyCode.E)  -- down and back up
        mousescroll(-240)         -- two notches down
        assert(keytap == keyclick, "keytap is keyclick")
        assert(not pcall(keypress, 0x01), "a mouse button's code is not a key")
        assert(not pcall(keypress, "NoSuchKey"))
        assert(not pcall(keypress, Enum.UserInputType.Keyboard), "an item of another enum")
        assert(not pcall(mousemoverel, 0/0, 0), "a NaN motion")
        assert(not pcall(mousescroll, math.huge), "an endless turn")
    )lua");
    CHECK_EQ(sent.size(), std::size_t{13});
    if (sent.size() != 13)
        return;
    CHECK(sent[0].kind == Input::Kind::Key && sent[0].key == lunar::Enum::KeyCode::W && sent[0].pressed);
    CHECK(sent[1].kind == Input::Kind::Key && sent[1].key == lunar::Enum::KeyCode::W && !sent[1].pressed);
    CHECK(sent[2].key == lunar::Enum::KeyCode::One && sent[2].pressed);
    CHECK(sent[3].key == lunar::Enum::KeyCode::LeftShift);
    CHECK(sent[4].kind == Input::Kind::Button && sent[4].button == 1 && sent[4].pressed);
    CHECK(sent[5].kind == Input::Kind::Button && sent[5].button == 1 && !sent[5].pressed);
    CHECK(sent[6].button == 2 && sent[6].pressed && sent[7].button == 2 && !sent[7].pressed);
    CHECK(sent[8].kind == Input::Kind::MoveBy && sent[8].x == 12.0f && sent[8].y == -5.0f);
    CHECK(sent[9].kind == Input::Kind::MoveTo && sent[9].x == 640.0f && sent[9].y == 360.0f);
    CHECK(sent[10].key == lunar::Enum::KeyCode::E && sent[10].pressed);
    CHECK(sent[11].key == lunar::Enum::KeyCode::E && !sent[11].pressed);
    CHECK(sent[12].kind == Input::Kind::Scroll && sent[12].y == -2.0f);

    // None of it is the game's.
    h.run("assert(keypress == nil and mouse1click == nil and mousemoverel == nil and mousescroll == nil)");
}

TEST(script_queue_on_teleport_runs_after_the_next_teleport)
{
    Harness h;
    h.console(R"lua(
        assert(type(queueonteleport) == "function", "the alias is missing")
        queue_on_teleport("getgenv().teleported = (getgenv().teleported or 0) + 1")
        queueonteleport("getgenv().teleported = 100")  -- the last one queued is the one that runs
    )lua");
    // Nothing has happened yet.
    h.console("assert(getgenv().teleported == nil, 'it ran before the teleport')");
    // A teleport takes it, and forgets it whatever the run did.
    h.context.runTeleportCode();
    h.console("assert(getgenv().teleported == 100, 'the queued code did not run')");
    h.context.runTeleportCode();
    h.console("assert(getgenv().teleported == 100, 'it ran twice')");
    // Code that errors is still gone.
    h.console("queue_on_teleport('this is not = = luau')");
    h.context.runTeleportCode();
    h.context.runTeleportCode();
    h.lines.clear();
    h.console("print('after a failed one')");
    CHECK_EQ(printed(h), (std::vector<std::string>{"after a failed one"}));
    // clearqueueonteleport takes it back before the teleport.
    h.console(R"lua(
        assert(clearteleportqueue == clearqueueonteleport and clear_teleport_queue == clearqueueonteleport)
        queue_on_teleport("getgenv().teleported = 200")
        clearqueueonteleport()
    )lua");
    h.context.runTeleportCode();
    h.console("assert(getgenv().teleported == 100, 'the cleared code ran')");
}

TEST(script_console_answers_to_the_executors_other_names)
{
    // The other names executors give what the console has, as Myriad asks for
    // them: each the very function (or table) it stands for.
    Harness h;
    h.console(R"lua(
        local aliases = {
            hookfunc = hookfunction, restorefunc = restorefunction, get_hidden_gui = gethui,
            getcallbackmember = getcallbackvalue, clonereference = cloneref, iswindowactive = isrbxactive,
            websocket = WebSocket, getconstant = debug.getconstant, getconstants = debug.getconstants,
            setconstant = debug.setconstant, getproto = debug.getproto, getprotos = debug.getprotos,
            getupvalue = debug.getupvalue, getupvalues = debug.getupvalues, setupvalue = debug.setupvalue,
            getstack = debug.getstack, setstack = debug.setstack, keytap = keyclick,
        }
        for alias, original in aliases do
            assert(original ~= nil and getfenv(0)[alias] == original, alias)
        end
        assert(crypt.lz4compress == lz4compress and crypt.lz4decompress == lz4decompress)
        assert(lz4decompress(crypt.lz4compress("hello hello hello"), 17) == "hello hello hello")
    )lua");
    // Still the console's alone.
    h.run("assert(hookfunc == nil and getupvalue == nil and websocket == nil and keyclick == nil)");
}

TEST(script_game_scripts_cannot_see_the_new_console_globals)
{
    // Everything the Luau tab gained is still the console's alone: a
    // LocalScript and a Script find none of it in their own globals.
    Harness h;
    h.console("assert(cache and crypt and cloneref and clonefunction)");
    h.run(R"lua(
        for _, name in ipairs({"cache", "crypt", "cloneref", "compareinstances", "clonefunction",
                               "iscclosure", "islclosure", "isexecutorclosure", "checkclosure", "isourclosure",
                               "newcclosure", "getscriptclosure", "getscriptfunction", "getrunningscripts",
                               "getscriptbytecode", "dumpstring", "getscripthash", "lz4compress", "lz4decompress",
                               "readfile", "writefile", "appendfile", "listfiles", "makefolder", "isfile", "isfolder",
                               "delfile", "delfolder", "loadfile", "dofile", "getcustomasset",
                               "gethiddenproperty", "sethiddenproperty", "isscriptable", "setscriptable",
                               "getcallbackvalue", "getnamecallmethod", "isrbxactive", "messagebox",
                               "setfpscap", "getfpscap", "setclipboard", "setrbxclipboard", "toclipboard",
                               "queue_on_teleport", "queueonteleport", "keypress", "keyrelease", "mouse1press",
                               "mouse1release", "mouse1click", "mouse2press", "mouse2release", "mouse2click",
                               "mousemoverel", "mousemoveabs", "base64", "base64_encode", "base64_decode",
                               "getthreadcontext", "setthreadcontext", "getexecutorname", "isgameactive"}) do
            assert(_G[name] == nil, name)
            assert(getfenv()[name] == nil, name)
        end
        -- The debug library is the VM's own, with none of the console's
        -- extras on it.
        for _, name in ipairs({"getconstant", "getconstants", "getproto", "getprotos", "getstack", "getinfo",
                               "setconstant", "setstack", "setupvalue"}) do
            assert(debug[name] == nil, name)
        end
        assert(debug.traceback and debug.info, "the VM's debug library is gone")
    )lua");
}
// getfunctionhash and filtergc, as the sUNC docs' examples use them.
TEST(script_console_filtergc_finds_by_name_hash_constants_and_tables)
{
    Harness h;
    h.console(R"lua(
        local function is_sha384_hex(hash) return #hash == 96 and hash:match("^[0-9a-fA-F]+$") ~= nil end
        local f0 = function() end
        local f1 = function(...) end
        local f2 = function() end
        local f3 = function() return "Constant" end
        local f4 = function() return "Constant2" end
        assert(is_sha384_hex(getfunctionhash(f0)), "not SHA-384 hex")
        assert(getfunctionhash(f0) ~= getfunctionhash(f1), "vararg hashes the same")
        assert(getfunctionhash(f0) == getfunctionhash(f2), "the same code hashes apart")
        assert(getfunctionhash(f3) ~= getfunctionhash(f4), "other constants hash the same")
        assert(not pcall(getfunctionhash, print), "a C function hashed")

        local function dummy_function() return "Hello" end
        local all = filtergc("function", {Name = "dummy_function", IgnoreExecutor = false})
        assert(typeof(all) == "table" and all[1] == dummy_function, "not found by name")
        assert(filtergc("function", {Name = "dummy_function"}, true) == nil, "the console's own counted")
        assert(filtergc("function", {Hash = getfunctionhash(dummy_function), IgnoreExecutor = false}, true) == dummy_function, "not found by hash")

        local upvalue = 5
        local function uses()
            upvalue += 1
            print(game.Players.LocalPlayer)
        end
        assert(filtergc("function", {Constants = {"print", "game", "Players", "LocalPlayer", 1}, Upvalues = {5},
            IgnoreExecutor = false}, true) == uses, "not found by constants and upvalues")

        local nan = 0/0
        local function holds_nan() return nan + 0.1 end
        assert(filtergc("function", {Constants = {nan}, IgnoreExecutor = false}, true) ~= nil, "a NaN constant not found")

        local by_key = {dummy_key = ""}
        assert(filtergc("table", {Keys = {"dummy_key"}}, true) == by_key, "not found by key")
        local by_pair = {pair_key = "pair_value"}
        assert(filtergc("table", {KeyValuePairs = {pair_key = "pair_value"}}, true) == by_pair, "not found by pair")
        local by_value = {"only_here_value"}
        assert(filtergc("table", {Values = {"only_here_value"}}, true) == by_value, "not found by value")
        local by_meta = setmetatable({}, {__index = getgenv()})
        assert(filtergc("table", {Metatable = getmetatable(by_meta)}, true) == by_meta, "not found by metatable")
        print("ok")
    )lua");
    CHECK_EQ(printed(h).back(), "ok");
}

// replicatesignal sends the event when the arguments are what the server's
// schema has it take, and raises otherwise.
TEST(script_console_replicatesignal_holds_to_the_events_parameters)
{
    Harness h;
    std::vector<std::string> sent;
    h.context.setRemoteSink([&](Instance& owner, std::string_view event, std::vector<RemoteValue> arguments) {
        sent.push_back(std::format("{}.{} {}", owner.className(), event, arguments.size()));
    });
    h.context.setEventShape(
        [](const Instance& owner, std::string_view event) -> std::optional<std::vector<std::string>> {
            if (owner.className() == "Frame" && event == "MouseWheelForward")
                return std::vector<std::string>{"number", "number"};
            if (owner.className() == "ClickDetector" && event == "MouseActionReplicated")
                return std::vector<std::string>{"Instance", "number"};
            return std::nullopt;
        });
    h.console(R"lua(
        local frame = Instance.new("Frame")
        print(pcall(replicatesignal, frame.MouseWheelForward))
        print(pcall(replicatesignal, frame.MouseWheelForward, 121))
        print(pcall(replicatesignal, frame.MouseWheelForward, 121, "x"))
        print(pcall(replicatesignal, frame.MouseWheelForward, 1, 2, 3))
        print(pcall(replicatesignal, frame.MouseWheelForward, 121, 214))
        print(pcall(replicatesignal, Instance.new("ClickDetector").MouseActionReplicated, workspace, 0))
        print(pcall(replicatesignal, frame.MouseEnter, 1, 2))
        print(pcall(replicatesignal, frame:GetPropertyChangedSignal("Size")))
    )lua");
    const std::vector<std::string> lines = printed(h);
    CHECK_EQ(lines.size(), std::size_t{8});
    if (lines.size() != 8)
        return;
    CHECK(lines[0].starts_with("false") && lines[0].find("missing argument #1") != std::string::npos);
    CHECK(lines[1].starts_with("false") && lines[1].find("missing argument #2") != std::string::npos);
    CHECK(lines[2].starts_with("false") && lines[2].find("number expected, got string") != std::string::npos);
    CHECK(lines[3].starts_with("false") && lines[3].find("takes 2 arguments") != std::string::npos);
    CHECK_EQ(lines[4], "true");
    CHECK_EQ(lines[5], "true");
    CHECK(lines[6].starts_with("false") && lines[6].find("does not replicate") != std::string::npos);
    CHECK(lines[7].starts_with("false"));
    CHECK_EQ(sent, (std::vector<std::string>{"Frame.MouseWheelForward 2", "ClickDetector.MouseActionReplicated 2"}));
}

// decompile hands back a script's own Source, or a listing of a function.
TEST(script_console_decompile_answers_with_text)
{
    Harness h;
    auto script = std::make_shared<LocalScript>();
    script->setName("Readable");
    script->setSource("print('readable')");
    script->setParent(h.game->workspace());
    h.console(R"lua(
        assert(decompile(workspace.Readable) == "print('readable')", "not its source")
        local function greet(name, ...) local greeting = "hi " .. name return greeting end
        local listing = decompile(greet)
        assert(listing:find("function greet(name, ...)", 1, true), listing)
        assert(listing:find("\"hi \"", 1, true), listing)
        assert(not pcall(decompile, print), "a C function listed")
        print("ok")
    )lua");
    CHECK_EQ(printed(h).back(), "ok");
}

// A script the server sent as bytecode decompiles to Luau that loads and runs
// as it did [sUNC decompiles a ModuleScript and runs what comes back].
TEST(script_console_decompile_rebuilds_sent_bytecode)
{
    Harness h;
    auto plain = std::make_shared<ModuleScript>();
    plain->setName("Plain");
    plain->setSource(R"lua(
        local Binding = {}
        Binding.__index = Binding
        function Binding.new(name, parts)
            local self = setmetatable({_parsedManifest = {name = name}, _connections = {}}, Binding)
            for i, part in ipairs(parts) do
                if part ~= "" then
                    self._connections[#self._connections + 1] = {Disconnect = function() return i end}
                end
            end
            return self
        end
        function Binding:destroy()
            local total = 0
            for _, connection in self._connections do total += connection:Disconnect() end
            return total
        end
        return Binding
    )lua");
    plain->setParent(h.game->workspace());
    h.console(R"lua(
        print((getscriptbytecode(workspace.Plain):gsub(".", function(c) return string.format("%02x", string.byte(c)) end)))
    )lua");
    const std::vector<std::string> hex = printed(h);
    CHECK_EQ(hex.size(), std::size_t{1});
    if (hex.size() != 1)
        return;
    std::string blob;
    for (std::size_t i = 0; i + 1 < hex[0].size(); i += 2)
        blob.push_back(static_cast<char>(std::stoi(hex[0].substr(i, 2), nullptr, 16)));
    h.context.setNetworkBytecode([blob](std::string_view source) -> std::optional<std::string> {
        if (source == "sent")
            return blob;
        return std::nullopt;
    });
    auto sent = std::make_shared<ModuleScript>();
    sent->setName("Sent");
    sent->setSource("sent");
    sent->setParent(h.game->workspace());
    h.lines.clear();
    h.console(R"lua(
        local text = decompile(workspace.Sent)
        assert(text:find("_parsedManifest", 1, true) and text:find("Disconnect()", 1, true), text)
        local Binding = loadstring(text)()
        local binding = Binding.new("sounds", {"a", "", "c"})
        print(binding._parsedManifest.name, #binding._connections, binding:destroy())
    )lua");
    CHECK_EQ(printed(h), std::vector<std::string>{"sounds 2 4"});
}

// The Drawing library as the sUNC docs' examples use it.
TEST(script_console_drawings_keep_their_properties)
{
    Harness h;
    h.console(R"lua(
        local circle = Drawing.new("Circle")
        circle.Radius = 50
        circle.Color = Color3.fromRGB(255, 0, 0)
        circle.Filled = true
        circle.NumSides = 150
        circle.Position = Vector2.new(10, 20)
        circle.Transparency = 0
        circle.Visible = true
        assert(circle.Radius == 50 and circle.Filled and circle.Position == Vector2.new(10, 20), "properties lost")
        assert(circle.__OBJECT_EXISTS == true, "doesn't exist")
        assert(typeof(circle) == "userdata", typeof(circle))
        -- What shows it, as sUNC finds it: a Frame rounded by a UICorner of
        -- (1, 0), the radius twice across, centred on the position.
        local found = {}
        for _, d in gethui():GetDescendants() do
            if d:IsA("UICorner") and tostring(d.CornerRadius) == "1, 0" and d.Parent.ClassName == "Frame" then
                table.insert(found, d.Parent)
            end
        end
        assert(#found == 1, "frames: " .. #found)
        local frame = found[1]
        assert(frame.Size.X.Offset == 100 and frame.Size.Y.Offset == 100, tostring(frame.Size))
        assert(frame.Position == UDim2.fromOffset(10, 20) and frame.AnchorPoint == Vector2.new(0.5, 0.5), tostring(frame.Position))
        assert(frame.Visible and frame.BackgroundColor3 == Color3.fromRGB(255, 0, 0), "not shown in its colour")
        circle:Destroy()
        assert(circle.__OBJECT_EXISTS == false, "still exists")
        assert(frame.Parent == nil, "its frame outlived it")

        local square = Drawing.new("Square")
        assert(isrenderobj(square) and not isrenderobj(workspace) and not isrenderobj("not a draw"), "isrenderobj")
        setrenderproperty(square, "Size", Vector2.new(5, 6))
        assert(getrenderproperty(square, "Size") == Vector2.new(5, 6) and square.Size == Vector2.new(5, 6), "render property")
        assert(not pcall(function() square.Radius = 1 end), "a circle's property on a square")
        assert(not pcall(function() square.Size = 1 end), "a number for a Vector2")

        local text = Drawing.new("Text")
        text.Text = "abcd"
        text.Size = 10
        assert(text.TextBounds == Vector2.new(20, 10), tostring(text.TextBounds))
        assert(not pcall(function() text.TextBounds = Vector2.zero end), "TextBounds written")
        assert(Drawing.Fonts.UI == 0 and Drawing.Fonts.Monospace == 3, "fonts")
        for _, kind in {"Line", "Text", "Image", "Circle", "Square", "Quad", "Triangle"} do
            assert(Drawing.new(kind).Visible == false, kind)
        end
        assert(not pcall(Drawing.new, "Blob"), "a type that isn't")

        local line = Drawing.new("Line")
        task.defer(cleardrawcache)
        assert(line.__OBJECT_EXISTS, "cleared early")
        task.wait()
        assert(not line.__OBJECT_EXISTS, "not cleared")
        assert(gethui().Drawing:FindFirstChildWhichIsA("Frame") == nil, "a frame outlived the cache")
        print("ok")
    )lua");
    h.steps(3);
    CHECK_EQ(printed(h).back(), "ok");
}

TEST(script_console_draw_cache_holds_what_nothing_refers_to)
{
    // A drawing stays on screen until it's destroyed, whether or not anything
    // refers to it, and cleardrawcache still finds it after collections.
    Harness h;
    h.console("do Drawing.new('Square') end");
    for (int i = 0; i < 20; ++i) {
        h.console("for i = 1, 20000 do local _ = {i} end");
        h.steps(1);
    }
    h.console(R"lua(
        assert(gethui().Drawing:FindFirstChildWhichIsA("Frame") ~= nil, "a drawing nothing held went away")
        cleardrawcache()
        assert(gethui().Drawing:FindFirstChildWhichIsA("Frame") == nil, "a drawing nothing held outlived the cache")
        print("ok")
    )lua");
    CHECK_EQ(printed(h), (std::vector<std::string>{"ok"}));
}

TEST(script_console_a_hooked_metamethod_looks_the_same_to_a_handler)
{
    Harness h;
    // What a game script sees of __namecall from an xpcall handler, and from
    // calling it bare, is the same with a hook on it [sUNC's hookfunction:
    // "generic namecall hook detected"].
    h.console(R"lua(
        local function seen()
            local out
            xpcall(function() return game:_() end, function()
                local a, b, c, d, e, f = debug.info(2, "slnaf")
                local _, message = pcall(f)
                out = table.concat({tostring(a), tostring(b), tostring(c), tostring(d), tostring(e), tostring(f),
                                    tostring(message)}, "|")
            end)
            return out
        end
        local before = seen()
        local mt = getrawmetatable(game)
        local old
        old = hookfunction(mt.__namecall, function(...) return old(...) end)
        assert(seen() == before, "the hook shows: " .. seen() .. " against " .. before)
        hookfunction(mt.__namecall, old)
        assert(seen() == before, "the restore shows")
    )lua");
}
