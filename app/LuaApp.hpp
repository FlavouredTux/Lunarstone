#pragma once

#include <span>
#include <string_view>

// --lua-app <client resources> [--seconds N] [--headless | --render out.png]:
// Roblox's own app (the home screen and the rest of the universal app,
// written in Luau) run by Lunarstone. The client's resources folder (RobloxPlayer.app/Contents/
// Resources, or the Windows client's folder) has the app's empty place,
// ExtraContent/places/Mobile.rbxl, and its code, ExtraContent/models/
// UniversalApp/UniversalApp.rbxm: a patch whose DataModelInstances go into
// the place's services (CoreGui, CorePackages) and whose CoreScripts start
// it (LuaAppStarterScript). The scripts keep their Source compiled, as
// Luau bytecode with the client's scrambled opcodes. In a window of its
// own; --headless runs it for N seconds without one, and --render draws
// what it made then to a picture.
// Returns the exit code, -1 for bad arguments.
int runLuaApp(std::span<const std::string_view> args);
