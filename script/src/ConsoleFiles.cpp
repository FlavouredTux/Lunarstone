// The console's files (Console.cpp opens them): the workspace folder's own
// readfile/writefile/listfiles family, the chunk loaders that go with it, and
// getcustomasset.
//
// Where the executors' file functions can touch anything the user can, these
// are sandboxed to one folder of ours -- the workspace under the client's data
// directory -- and every path is resolved before it is used, so nothing reaches
// out of it: not an absolute path, not a "..", not a symlink pointing away.
// That is the one place the client's own reach stops; everything else the
// console does goes through the engine like a script's would.

#include "Console.hpp"
#include "Runtime.hpp"

#include <lunar/assets/AssetCache.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lunar::lua {

namespace {

// The sandbox root: the workspace folder under the client's data directory
// (LUNAR_DATA_DIR, else $XDG_DATA_HOME/lunarstone, else ~/.local/share/lunarstone).
std::filesystem::path workspaceRoot()
{
    if (const char* given = std::getenv("LUNAR_DATA_DIR"); given && *given)
        return std::filesystem::path(given) / "workspace";
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "lunarstone/workspace";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local/share/lunarstone/workspace";
    return std::filesystem::temp_directory_path() / "lunarstone-workspace";
}

// What a path resolved to: the workspace it is in and the file itself.
struct Sandboxed {
    std::filesystem::path root;
    std::filesystem::path path;
};

// The root, made and canonicalised. weakly_canonical resolves what is there,
// which is what the prefix check below compares against. It is worked out
// again each call rather than kept, so a changed LUNAR_DATA_HOME is followed.
std::filesystem::path sandboxRoot()
{
    std::error_code ec;
    const std::filesystem::path made = workspaceRoot();
    std::filesystem::create_directories(made, ec);
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(made, ec);
    return ec ? made.lexically_normal() : canonical;
}

// The path at `idx`, inside the sandbox, or a raised error. An empty one is
// the workspace itself, as "." is [sUNC lists it so]. An absolute path is
// refused outright, a Windows one too ("C:/...", what an executor there
// refuses [Myriad writes there to check]); everything else is joined to the
// root and resolved, so a ".." that climbs out and a symlink that points away
// both land outside the root and fail the prefix check.
Sandboxed sandbox(lua_State* L, int idx)
{
    const std::string_view text = checkString(L, idx);
    const std::filesystem::path given = text;
    const bool drive = text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':';
    if (given.is_absolute() || drive || text.starts_with('\\'))
        luaL_argerror(L, idx, "is not inside the workspace");
    const std::filesystem::path root = sandboxRoot();
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(root / given, ec);
    if (ec)
        luaL_argerror(L, idx, "is not a path of this machine");
    // Inside is the root's every part, then more: a sibling whose name starts
    // the same way ("workspace2") is not.
    if (std::ranges::mismatch(root, resolved).in1 != root.end())
        luaL_argerror(L, idx, "is outside the workspace");
    return {root, resolved};
}

// A resolved path as the workspace-relative text listfiles hands back, so
// reading one of them finds the same file.
std::string relativeToRoot(const Sandboxed& box)
{
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(box.path, box.root, ec);
    return ec ? box.path.filename().string() : relative.generic_string();
}

std::string readWholeFile(lua_State* L, const std::filesystem::path& path)
{
    // A folder opens as a file would and reads as nothing.
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        raise(L, std::format("{}: is a folder", path.string()));
    std::ifstream file(path, std::ios::binary);
    if (!file)
        raise(L, std::format("{}: can't read", path.string()));
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// readfile(path): the file's bytes as text, binary data and all.
int readFile(lua_State* L)
{
    const std::string text = readWholeFile(L, sandbox(L, 1).path);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// The extensions writefile and appendfile refuse, sUNC's list (docs.sunc.su,
// Filesystem): what would run, or install, when opened. Any case.
void refuseRunnable(lua_State* L, const std::filesystem::path& path)
{
    static constexpr std::string_view refused[]
        = {".exe", ".com", ".scr", ".pif", ".cpl", ".msc", ".bat", ".cmd", ".ps1", ".psd1", ".vbs",
           ".vbe", ".js",  ".jse", ".wsf", ".wsh", ".hta", ".scf", ".lnk", ".zip", ".rar", ".7z",
           ".cab", ".iso", ".img", ".xml", ".msi", ".msp", ".reg", ".inf", ".url"};
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
    if (std::ranges::find(refused, extension) != std::end(refused))
        raise(L, std::format("{}: a {} file can't be written", path.filename().string(), extension));
}

// writefile(path, text): the file's own contents, with any folder above it made
// -- the executors' do this too, so a path in a folder that isn't there yet
// still works.
int writeFile(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    const std::string_view text = checkString(L, 2);
    refuseRunnable(L, path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file)
        raise(L, std::format("{}: can't write", path.string()));
    return 0;
}

// appendfile(path, text): the text after what the file has.
int appendFile(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    const std::string_view text = checkString(L, 2);
    refuseRunnable(L, path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file)
        raise(L, std::format("{}: can't write", path.string()));
    return 0;
}

// listfiles(folder): what is in it, files and folders alike, as paths relative
// to the workspace so that isfile() and readfile() take them as they are. A
// folder that is not there has nothing in it, which is what the executors'
// answer rather than an error; a path that is a file is not a folder.
int listFiles(lua_State* L)
{
    const Sandboxed box = sandbox(L, 1);
    const std::filesystem::path& path = box.path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        lua_newtable(L);
        return 1;
    }
    if (!std::filesystem::is_directory(path, ec))
        raise(L, std::format("{}: not a folder", path.string()));
    std::vector<std::filesystem::path> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path, ec))
        names.push_back(entry.path());
    if (ec)
        raise(L, std::format("{}: can't be listed", path.string()));
    // The order the filesystem gives is not one anything can rely on.
    std::sort(names.begin(), names.end());
    lua_createtable(L, static_cast<int>(names.size()), 0);
    int i = 0;
    for (const std::filesystem::path& name : names) {
        const std::string text = relativeToRoot({box.root, name});
        lua_pushlstring(L, text.data(), text.size());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// makefolder(path): the folder and any above it that aren't there.
int makeFolder(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
        raise(L, std::format("{}: can't make the folder ({})", path.string(), ec.message()));
    return 0;
}

// isfile(path) / isfolder(path): false for what isn't there, which is what the
// callers ask; a path that leaves the workspace is an error, not a "no".
int isFile(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    std::error_code ec;
    lua_pushboolean(L, std::filesystem::is_regular_file(path, ec));
    return 1;
}

int isFolder(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    std::error_code ec;
    lua_pushboolean(L, std::filesystem::is_directory(path, ec));
    return 1;
}

// delfile(path): the file, and only the file; delfolder(path) takes what is in
// it too, as the executors' do.
int delFile(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        raise(L, std::format("{}: not a file", path.string()));
    std::filesystem::remove(path, ec);
    if (ec)
        raise(L, std::format("{}: can't be deleted ({})", path.string(), ec.message()));
    return 0;
}

int delFolder(lua_State* L)
{
    const Sandboxed box = sandbox(L, 1);
    const std::filesystem::path& path = box.path;
    if (path == box.root)
        raise(L, "delfolder: the workspace itself can't be deleted");
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec))
        raise(L, std::format("{}: not a folder", path.string()));
    std::filesystem::remove_all(path, ec);
    if (ec)
        raise(L, std::format("{}: can't be deleted ({})", path.string(), ec.message()));
    return 0;
}

// loadfile(path[, chunkname]): the file as a function of the console's own
// globals, like loadstring gives. Nil and the compiler's message when the
// source doesn't compile, which is what loadstring and loadstring's own callers
// expect of it.
int loadFile(lua_State* L)
{
    const std::string name{checkString(L, 1)};
    const std::filesystem::path path = sandbox(L, 1).path;
    const std::string chunkName = lua_type(L, 2) == LUA_TSTRING ? std::string(checkString(L, 2)) : name;
    const std::string source = readWholeFile(L, path);
    auto bytecode = ScriptContext::compile(source, chunkName, 2);
    if (!bytecode) {
        lua_pushnil(L);
        lua_pushlstring(L, bytecode.error().data(), bytecode.error().size());
        return 2;
    }
    const std::string chunk = std::format("={}", chunkName);
    if (luau_load(L, chunk.c_str(), bytecode->data(), bytecode->size(), 0) != 0) {
        const char* message = lua_tostring(L, -1);
        lua_pushnil(L);
        lua_pushstring(L, message ? message : "the file did not load");
        lua_remove(L, -2);
        return 2;
    }
    return 1;
}

// dofile(path): runs the file in the console's globals, as loadfile would if
// it were called: the function's own results are the answer.
int doFile(lua_State* L)
{
    const std::string name{checkString(L, 1)};
    const std::filesystem::path path = sandbox(L, 1).path;
    const std::string source = readWholeFile(L, path);
    auto bytecode = ScriptContext::compile(source, name, 2);
    if (!bytecode)
        raise(L, bytecode.error());
    const std::string chunk = std::format("={}", name);
    // Whatever the chunk answers with sits above the arguments it was called
    // with, so the results are counted from here.
    const int below = lua_gettop(L);
    if (luau_load(L, chunk.c_str(), bytecode->data(), bytecode->size(), 0) != 0) {
        const char* message = lua_tostring(L, -1);
        raise(L, message ? message : "the file did not load");
    }
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - below;
}

// getcustomasset(path): the file as a content id the client can load. The
// executors copy it into the client's content folder and answer with the
// rbxasset:// path it got there; this client's content folder is the real
// client's, so the copy goes to the asset cache's customassets folder instead,
// which AssetCache reads rbxasset://customassets/ from. The name is decided by
// the bytes (and keeps the file's extension), so two files never answer for
// each other.
int getCustomAsset(lua_State* L)
{
    const std::filesystem::path path = sandbox(L, 1).path;
    const std::string source = readWholeFile(L, path);
    const std::string name = std::format("{:016x}{}", std::hash<std::string>{}(source), path.extension().string());
    const std::filesystem::path folder = assets::AssetConfig::fromEnvironment().cacheDir / assets::customAssetFolder;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
    file.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!file)
        raise(L, std::format("{}: can't put the file in the asset cache", (folder / name).string()));
    const std::string id = std::format("rbxasset://{}/{}", assets::customAssetFolder, name);
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

} // namespace

void openConsoleFiles(lua_State* thread)
{
    const auto set = [thread](const char* name, lua_CFunction function) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    };
    set("readfile", readFile);
    set("writefile", writeFile);
    set("appendfile", appendFile);
    set("listfiles", listFiles);
    set("makefolder", makeFolder);
    set("isfile", isFile);
    set("isfolder", isFolder);
    set("delfile", delFile);
    set("delfolder", delFolder);
    set("loadfile", loadFile);
    set("dofile", doFile);
    set("getcustomasset", getCustomAsset);
}

} // namespace lunar::lua