// The console's file dialogs (Console.cpp opens them): openfiledialog,
// openfilesdialog, openfolderdialog and savefiledialog, and the file and folder
// objects they answer with, as the Myriad spec gives them
// (docs.myriad.tools/spec/File Dialog).
//
// The dialog is the desktop's own: zenity, or kdialog without it, run as a
// program of its own (no shell between) that talks to the compositor itself.
// LUNARSTONE_FILE_DIALOG names a program to run in zenity's place, with
// zenity's arguments. It runs on a thread of its own, and the calling thread
// yields until it is dismissed, as the spec has it block; the game keeps
// running meanwhile. Without a desktop, or either program, the call raises.
//
// What the dialogs give is outside the workspace sandbox (ConsoleFiles.cpp):
// it is what the user picked, and a folder's objects reach only what is in
// it. A file's object reads, writes, appends and deletes that one file.

#include "Console.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace lunar::lua {

namespace {

namespace fs = std::filesystem;

// Past the tags Runtime.hpp names (and the Drawing's and the WebSocket's).
constexpr int DialogItemTag = 66;

enum class Dialog : std::uint8_t { File, Files, Folder, Save };

struct Options {
    std::string title;
    std::string defaultPath;
    std::vector<std::string> extensions; // without the dot
    std::string suggestedName;
    std::string defaultExtension;
};

// What the dialog's thread and the script's side share.
struct Pick {
    std::atomic<bool> done{false};
    std::optional<std::vector<std::string>> paths; // none when cancelled
    std::string error;                              // why there was no dialog
};

struct Item {
    fs::path path;
    bool folder = false;
};

std::optional<std::string> findProgram(std::string_view name)
{
    const char* path = std::getenv("PATH");
    if (!path)
        return std::nullopt;
    for (auto part : std::string_view(path) | std::views::split(':')) {
        const std::string_view dir(part.begin(), part.end());
        if (dir.empty())
            continue;
        std::string candidate = std::format("{}/{}", dir, name);
        if (::access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return std::nullopt;
}

// Where the dialog opens: the folder asked for when there is one, else home.
std::string startFolder(const Options& options)
{
    std::error_code ec;
    if (!options.defaultPath.empty() && fs::is_directory(options.defaultPath, ec))
        return options.defaultPath;
    const char* home = std::getenv("HOME");
    return home ? home : ".";
}

std::vector<std::string> zenityArguments(Dialog dialog, const Options& options)
{
    std::vector<std::string> args{"--file-selection"};
    if (dialog == Dialog::Files)
        args.insert(args.end(), {"--multiple", "--separator=\n"});
    if (dialog == Dialog::Folder)
        args.emplace_back("--directory");
    if (dialog == Dialog::Save)
        args.emplace_back("--save");
    if (!options.title.empty())
        args.push_back("--title=" + options.title);
    std::string start = startFolder(options) + "/";
    if (dialog == Dialog::Save)
        start += options.suggestedName;
    args.push_back("--filename=" + start);
    if (!options.extensions.empty()) {
        std::string filter = "Files |";
        for (const std::string& extension : options.extensions)
            filter += " *." + extension;
        args.push_back("--file-filter=" + filter);
    }
    return args;
}

std::vector<std::string> kdialogArguments(Dialog dialog, const Options& options)
{
    std::vector<std::string> args;
    if (!options.title.empty())
        args.insert(args.end(), {"--title", options.title});
    switch (dialog) {
    case Dialog::File: args.emplace_back("--getopenfilename"); break;
    case Dialog::Files: args.insert(args.end(), {"--getopenfilename", "--multiple", "--separate-output"}); break;
    case Dialog::Folder: args.emplace_back("--getexistingdirectory"); break;
    case Dialog::Save: args.emplace_back("--getsavefilename"); break;
    }
    std::string start = startFolder(options);
    if (dialog == Dialog::Save && !options.suggestedName.empty())
        start += "/" + options.suggestedName;
    args.push_back(std::move(start));
    if (dialog != Dialog::Folder && !options.extensions.empty()) {
        std::string filter;
        for (const std::string& extension : options.extensions)
            filter += std::format("{}*.{}", filter.empty() ? "" : " ", extension);
        args.push_back(std::move(filter));
    }
    return args;
}

// Runs the program with nothing it doesn't need: stdin and stderr are
// /dev/null and no descriptor of ours past them goes with it. Its exit status
// and what it wrote, or an error when it never ran.
std::optional<std::pair<int, std::string>> run(const std::string& program, const std::vector<std::string>& args,
                                               std::string& error)
{
    int out[2];
    if (::pipe2(out, O_CLOEXEC) != 0) {
        error = "no pipe for the dialog";
        return std::nullopt;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    if (spawned != 0) {
        ::close(out[0]);
        error = std::format("{} did not run: {}", program, std::generic_category().message(spawned));
        return std::nullopt;
    }
    std::string output;
    char buffer[4096];
    for (ssize_t n; (n = ::read(out[0], buffer, sizeof buffer)) != 0;) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        output.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(out[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return std::pair{WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(output)};
}

void show(Pick& pick, Dialog dialog, const Options& options)
{
    std::optional<std::pair<int, std::string>> result;
    if (const char* override = std::getenv("LUNARSTONE_FILE_DIALOG"); override && *override) {
        result = run(override, zenityArguments(dialog, options), pick.error);
    } else if (!std::getenv("WAYLAND_DISPLAY") && !std::getenv("DISPLAY")) {
        pick.error = "there is no desktop to open a dialog on";
    } else if (auto zenity = findProgram("zenity")) {
        result = run(*zenity, zenityArguments(dialog, options), pick.error);
    } else if (auto kdialog = findProgram("kdialog")) {
        result = run(*kdialog, kdialogArguments(dialog, options), pick.error);
    } else {
        pick.error = "a file dialog needs zenity or kdialog installed";
    }
    if (result) {
        // 0 is a pick, 1 is cancelled; anything else went wrong.
        if (result->first == 0) {
            std::vector<std::string> paths;
            for (auto line : result->second | std::views::split('\n'))
                if (!std::ranges::empty(line))
                    paths.emplace_back(line.begin(), line.end());
            pick.paths = std::move(paths);
        } else if (result->first != 1) {
            pick.error = std::format("the file dialog failed (exit {})", result->first);
        }
    }
    pick.done.store(true, std::memory_order_release);
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// --- The objects ------------------------------------------------------------

Item& checkItem(lua_State* L, int idx)
{
    if (auto* item = static_cast<Item*>(lua_touserdatatagged(L, idx, DialogItemTag)))
        return *item;
    luaL_typeerror(L, idx, "file or folder");
}

Item& checkFile(lua_State* L)
{
    Item& item = checkItem(L, 1);
    if (item.folder)
        luaL_typeerror(L, 1, "file");
    return item;
}

Item& checkFolder(lua_State* L)
{
    Item& item = checkItem(L, 1);
    if (!item.folder)
        luaL_typeerror(L, 1, "folder");
    return item;
}

void pushItem(lua_State* L, fs::path path, bool folder)
{
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(Item), DialogItemTag);
    new (memory) Item{std::move(path), folder};
}

int getName(lua_State* L)
{
    pushString(L, checkItem(L, 1).path.filename().string());
    return 1;
}

int isFile(lua_State* L)
{
    lua_pushboolean(L, !checkItem(L, 1).folder);
    return 1;
}

int isFolder(lua_State* L)
{
    lua_pushboolean(L, checkItem(L, 1).folder);
    return 1;
}

// Delete(): the file, or the folder and all in it.
int remove(lua_State* L)
{
    const Item& item = checkItem(L, 1);
    std::error_code ec;
    if (item.folder)
        fs::remove_all(item.path, ec);
    else
        fs::remove(item.path, ec);
    if (ec)
        raise(L, std::format("could not delete {}: {}", item.path.filename().string(), ec.message()));
    return 0;
}

int read(lua_State* L)
{
    const Item& item = checkFile(L);
    std::ifstream in(item.path, std::ios::binary);
    if (!in)
        raise(L, std::format("could not read {}", item.path.filename().string()));
    const std::string content{std::istreambuf_iterator<char>(in), {}};
    pushString(L, content);
    return 1;
}

void put(lua_State* L, std::ios::openmode mode)
{
    const Item& item = checkFile(L);
    const std::string_view content = checkString(L, 2);
    std::ofstream out(item.path, std::ios::binary | mode);
    if (!out || !out.write(content.data(), static_cast<std::streamsize>(content.size())))
        raise(L, std::format("could not write {}", item.path.filename().string()));
}

int write(lua_State* L)
{
    put(L, std::ios::trunc);
    return 0;
}

int append(lua_State* L)
{
    put(L, std::ios::app);
    return 0;
}

// List([recursive]): what is in the folder, files and folders both; recursive
// goes into the folders too, but not through links to folders elsewhere.
int list(lua_State* L)
{
    const Item& item = checkFolder(L);
    const bool recursive = lua_toboolean(L, 2) != 0;
    std::vector<std::pair<fs::path, bool>> found;
    std::error_code ec;
    auto add = [&](const fs::directory_entry& entry) {
        std::error_code kind;
        found.emplace_back(entry.path(), entry.is_directory(kind));
    };
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(item.path, ec); !ec && it != fs::end(it); it.increment(ec))
            add(*it);
    } else {
        for (auto it = fs::directory_iterator(item.path, ec); !ec && it != fs::end(it); it.increment(ec))
            add(*it);
    }
    if (ec)
        raise(L, std::format("could not list {}: {}", item.path.filename().string(), ec.message()));
    lua_createtable(L, static_cast<int>(found.size()), 0);
    int i = 0;
    for (auto& [path, folder] : found) {
        pushItem(L, std::move(path), folder);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// MakeFolder(name): a folder of that name in this one (the one there already,
// when there is), never anywhere else.
int makeFolder(lua_State* L)
{
    const Item& item = checkFolder(L);
    const std::string_view name = checkString(L, 2);
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos)
        luaL_argerror(L, 2, "is not a folder name");
    fs::path path = item.path / name;
    std::error_code ec;
    fs::create_directory(path, ec);
    if (ec || !fs::is_directory(path, ec))
        raise(L, std::format("could not make {}: {}", std::string(name), ec ? ec.message() : "a file is there"));
    pushItem(L, std::move(path), true);
    return 1;
}

int itemIndex(lua_State* L)
{
    const Item& item = checkItem(L, 1);
    const std::string_view key = checkString(L, 2);
    struct Method {
        std::string_view name;
        lua_CFunction fn;
        bool file, folder;
    };
    static constexpr Method methods[] = {
        {"GetName", getName, true, true},    {"IsFile", isFile, true, true},     {"IsFolder", isFolder, true, true},
        {"Delete", remove, true, true},      {"Read", read, true, false},        {"Write", write, true, false},
        {"Append", append, true, false},     {"List", list, false, true},        {"MakeFolder", makeFolder, false, true},
    };
    for (const Method& method : methods) {
        if (method.name == key && (item.folder ? method.folder : method.file)) {
            lua_pushcfunction(L, method.fn, method.name.data()); // the names are literals
            return 1;
        }
    }
    raise(L, std::format("{} is not a valid member of {}", key, item.folder ? "folder" : "file"));
}

int itemToString(lua_State* L)
{
    const Item& item = checkItem(L, 1);
    pushString(L, std::format("{}: {}", item.folder ? "folder" : "file", item.path.filename().string()));
    return 1;
}

// --- The dialogs ------------------------------------------------------------

std::string optionString(lua_State* L, int idx, const char* field)
{
    lua_getfield(L, idx, field);
    std::string value;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
            raise(L, std::format("options.{} must be a string", field));
        value = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

Options checkOptions(lua_State* L, int idx, Dialog dialog)
{
    Options options;
    if (lua_isnoneornil(L, idx))
        return options;
    luaL_checktype(L, idx, LUA_TTABLE);
    options.title = optionString(L, idx, "title");
    options.defaultPath = optionString(L, idx, "defaultPath");
    if (dialog != Dialog::Folder) {
        lua_getfield(L, idx, "extensionFilter");
        if (lua_istable(L, -1)) {
            for (int i = 1;; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                if (lua_type(L, -1) != LUA_TSTRING)
                    raise(L, "options.extensionFilter must be a list of strings");
                std::string_view extension = lua_tostring(L, -1);
                if (extension.starts_with('.'))
                    extension.remove_prefix(1);
                if (!extension.empty() && extension.find_first_of("/\\ *") == std::string_view::npos)
                    options.extensions.push_back(lower(extension));
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1)) {
            raise(L, "options.extensionFilter must be a list of strings");
        }
        lua_pop(L, 1);
    }
    if (dialog == Dialog::Save) {
        options.suggestedName = optionString(L, idx, "suggestedName");
        if (options.suggestedName.find_first_of("/\\") != std::string::npos)
            raise(L, "options.suggestedName is a file name without a folder");
        options.defaultExtension = optionString(L, idx, "defaultExtension");
        if (options.defaultExtension.starts_with('.'))
            options.defaultExtension.erase(0, 1);
    }
    return options;
}

// What the dialog gave, checked (and for a save, written), before the thread
// wakes with it: the paths of the objects, or why it fails instead.
std::expected<std::vector<fs::path>, std::string> settle(Dialog dialog, const Options& options,
                                                         std::vector<std::string> picked, const std::string& content)
{
    auto allowed = [&](const fs::path& path) {
        const std::string extension = path.extension().string();
        return options.extensions.empty() ||
               std::ranges::contains(options.extensions, lower(std::string_view(extension).substr(extension.empty() ? 0 : 1)));
    };
    std::vector<fs::path> paths(picked.begin(), picked.end());
    if (dialog != Dialog::Files && paths.size() > 1)
        paths.resize(1);
    for (fs::path& path : paths) {
        std::error_code ec;
        switch (dialog) {
        case Dialog::Folder:
            if (!fs::is_directory(path, ec))
                return std::unexpected("the dialog did not give a folder");
            break;
        case Dialog::Save: {
            if (!options.defaultExtension.empty() && !path.has_extension())
                path += "." + options.defaultExtension;
            if (!allowed(path))
                return std::unexpected(std::format("{} is not one of the file types asked for", path.filename().string()));
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(content.data(), static_cast<std::streamsize>(content.size())))
                return std::unexpected(std::format("could not write {}", path.filename().string()));
            break;
        }
        case Dialog::File:
        case Dialog::Files:
            if (!fs::is_regular_file(path, ec))
                return std::unexpected("the dialog did not give a file");
            if (!allowed(path))
                return std::unexpected(std::format("{} is not one of the file types asked for", path.filename().string()));
            break;
        }
    }
    return paths;
}

void whenPicked(Runtime& rt, lua_State* L, std::uint64_t token, std::shared_ptr<Pick> pick, Dialog dialog,
                Options options, std::string content, const char* name)
{
    rt.after(0.0, [&rt, L, token, pick = std::move(pick), dialog, options = std::move(options),
                   content = std::move(content), name] mutable {
        if (!pick->done.load(std::memory_order_acquire)) {
            whenPicked(rt, L, token, std::move(pick), dialog, std::move(options), std::move(content), name);
            return;
        }
        if (!rt.isSuspended(L, token))
            return;
        if (!pick->error.empty()) {
            rt.fail(L, token, std::format("{}: {}", name, pick->error), name);
            return;
        }
        if (!pick->paths || pick->paths->empty()) {
            rt.wake(L, token, [](lua_State* t) { lua_pushnil(t); return 1; }, name);
            return;
        }
        auto paths = settle(dialog, options, std::move(*pick->paths), content);
        if (!paths) {
            rt.fail(L, token, std::format("{}: {}", name, paths.error()), name);
            return;
        }
        rt.wake(
            L, token,
            [&](lua_State* t) {
                const bool folder = dialog == Dialog::Folder;
                if (dialog != Dialog::Files) {
                    pushItem(t, paths->front(), folder);
                    return 1;
                }
                lua_createtable(t, static_cast<int>(paths->size()), 0);
                int i = 0;
                for (const fs::path& path : *paths) {
                    pushItem(t, path, false);
                    lua_rawseti(t, -2, ++i);
                }
                return 1;
            },
            name);
    });
}

int open(lua_State* L, Dialog dialog, Options options, std::string content, const char* name)
{
    checkYieldable(L);
    Runtime& rt = Runtime::of(L);
    auto pick = std::make_shared<Pick>();
    std::thread([pick, dialog, options] { show(*pick, dialog, options); }).detach();
    const std::uint64_t token = rt.suspend(L);
    whenPicked(rt, L, token, std::move(pick), dialog, std::move(options), std::move(content), name);
    return lua_yield(L, 0);
}

// openfiledialog([options]): a file, or nil when the dialog was cancelled.
int openFileDialog(lua_State* L)
{
    return open(L, Dialog::File, checkOptions(L, 1, Dialog::File), {}, "openfiledialog");
}

// openfilesdialog([options]): a list of files, or nil when cancelled.
int openFilesDialog(lua_State* L)
{
    return open(L, Dialog::Files, checkOptions(L, 1, Dialog::Files), {}, "openfilesdialog");
}

// openfolderdialog([options]): a folder, or nil when cancelled.
int openFolderDialog(lua_State* L)
{
    return open(L, Dialog::Folder, checkOptions(L, 1, Dialog::Folder), {}, "openfolderdialog");
}

// savefiledialog(content[, options]): writes the content where the user chose
// and gives that file, or nil when cancelled. defaultExtension goes on a name
// given without one.
int saveFileDialog(lua_State* L)
{
    std::string content(checkString(L, 1));
    return open(L, Dialog::Save, checkOptions(L, 2, Dialog::Save), std::move(content), "savefiledialog");
}

} // namespace

void openConsoleDialogs(lua_State* thread)
{
    // The metatable is the VM's, made the first time a console's globals are.
    lua_getuserdatametatable(thread, DialogItemTag);
    const bool made = !lua_isnil(thread, -1);
    lua_pop(thread, 1);
    if (!made) {
        lua_newtable(thread);
        lua_pushcfunction(thread, itemIndex, "__index");
        lua_setfield(thread, -2, "__index");
        lua_pushcfunction(thread, itemToString, "__tostring");
        lua_setfield(thread, -2, "__tostring");
        registerType(thread, DialogItemTag, "DialogItem",
                     [](lua_State*, void* memory) { static_cast<Item*>(memory)->~Item(); });
    }
    const luaL_Reg functions[] = {
        {"openfiledialog", openFileDialog},
        {"openfilesdialog", openFilesDialog},
        {"openfolderdialog", openFolderDialog},
        {"savefiledialog", saveFileDialog},
    };
    for (const luaL_Reg& fn : functions) {
        lua_pushcfunction(thread, fn.func, fn.name);
        lua_setglobal(thread, fn.name);
    }
}

} // namespace lunar::lua
