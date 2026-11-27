#pragma once

// The console's globals, split over the files that make them. Console.cpp
// opens them into the one thread's globals; each of these fills in a part of
// what that thread can reach. See Console.cpp for what the whole is and why
// no game script sees any of it.

#include <lua.h>

#include <string>

struct Proto;

namespace lunar {
class Instance;
}

namespace lunar::lua {

// A prototype of the VM's of its own, with from's code (and room for
// `sizecode` instructions at least, the ones past from's NOPs) and its own
// arrays of everything else, as the collector frees each proto's; the strings
// and protos in them are shared. ConsoleDebug.cpp's, for the hooks too.
Proto* copyProto(lua_State* L, const Proto* from, int sizecode = 0);

// cache.invalidate/iscached/replace, cloneref, compareinstances and the
// closure helpers (clonefunction, newcclosure, iscclosure, islclosure,
// isexecutorclosure, getscriptclosure), getscriptbytecode, with the lz4 pair.
void openConsoleCaches(lua_State* thread);
// The crypt table: base64, AES, generatebytes/generatekey and hash; and
// getscripthash and getfunctionhash, digests of a script's bytecode and of a
// function's code.
void openConsoleCrypt(lua_State* thread);
// getfunctionhash's SHA-384 of a function's constants and code, as hex.
std::string functionHash(const Proto* proto);
// The console's own debug table, over the library one: getconstant(s),
// getinfo, getproto(s), getstack, getupvalue(s) and their setters. This is
// the only file of the console's that reaches into the VM's own structures
// (lobject.h's Closure and Proto).
void openConsoleDebug(lua_State* thread);
// Drawing.new and Drawing.Fonts, isrenderobj, getrenderproperty,
// setrenderproperty and cleardrawcache.
void openConsoleDrawing(lua_State* thread);
// openfiledialog, openfilesdialog, openfolderdialog and savefiledialog: the
// desktop's own dialogs, and the file and folder objects they give.
void openConsoleDialogs(lua_State* thread);
// The file functions, sandboxed to the workspace folder, and getcustomasset.
void openConsoleFiles(lua_State* thread);
// gethiddenproperty/sethiddenproperty, isscriptable/setscriptable,
// getcallbackvalue, gethui and getnamecallmethod.
void openConsoleProperties(lua_State* thread);
// gethui()'s Folder in CoreGui, made the first time it's asked for.
Instance& consoleGui(lua_State* L);
// hookfunction (replaceclosure), hookmetamethod, restorefunction,
// isfunctionhooked and getgc, with getrunningscripts and getscriptfromthread
// (which walk the VM's threads as getgc walks its objects). With the debug
// file, the only ones that change the VM's own structures.
void openConsoleHooks(lua_State* thread);
// What the application outside the VM can answer: isrbxactive, messagebox,
// setfpscap/getfpscap and the clipboard.
void openConsoleHost(lua_State* thread);
// request (http_request, http.request); game:HttpGet and game:HttpPost are
// there too, as consoleMethods (InstanceApi.hpp).
void openConsoleHttp(lua_State* thread);
// WebSocket.connect and the WebSocket it answers with.
void openConsoleWebSocket(lua_State* thread);

} // namespace lunar::lua