find_package(PkgConfig REQUIRED)
# The console's crypt table (ConsoleCrypt.cpp) hashes and encrypts with
# libcrypto, the one the network side links for RakNet's AES.
pkg_check_modules(LIBCRYPTO REQUIRED IMPORTED_TARGET libcrypto)

include(FetchContent)

# Luau, pinned to a release. Only the VM, compiler and AST get built (they're
# the only targets linked); none of the project's warning flags apply to it.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 0.744
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(luau)
set_target_properties(Luau.Common Luau.Ast Luau.Bytecode Luau.Compiler Luau.VM PROPERTIES CXX_STANDARD 17)

add_library(lunarstone_script
    src/Animation.cpp
    src/BuiltinTrace.cpp
    src/Utf8Roblox.cpp
    src/Console.cpp
    src/ConsoleCache.cpp
    src/ConsoleCrypt.cpp
    src/ConsoleDebug.cpp
    src/ConsoleDialogs.cpp
    src/ConsoleDrawing.cpp
    src/ConsoleFiles.cpp
    src/ConsoleHooks.cpp
    src/ConsoleProperties.cpp
    src/ConsoleHost.cpp
    src/ConsoleHttp.cpp
    src/ConsoleWebSocket.cpp
    src/Datatypes.cpp
    src/Decompiler.cpp
    src/EnumApi.cpp
    src/Events.cpp
    src/HumanoidApi.cpp
    src/InstanceApi.cpp
    src/RobloxApi.cpp
    src/RobloxApiData.cpp
    src/RobloxEventData.cpp
    src/Packing.cpp
    src/PlayerModule.cpp
    src/Profiler.cpp
    src/ScriptContext.cpp
    src/Scripts.cpp
    src/Services.cpp
    src/SharedTable.cpp
    src/Simulation.cpp
    src/TextChat.cpp
    src/AppApi.cpp
    src/AppHttp.cpp
    src/Localization.cpp
    src/StylesApi.cpp
    src/Prompts.cpp
    src/Tools.cpp
    src/FrameDriver.cpp
    src/Emotes.cpp
    src/Social.cpp
    src/InputActions.cpp
    src/ScriptTypes.cpp
    src/ValueConversion.cpp
    src/ValueTypes.cpp
)
add_library(lunarstone::script ALIAS lunarstone_script)

target_include_directories(lunarstone_script PUBLIC include)
# identifyexecutor's version is the project's (and request's User-Agent);
# only those two files need it, so a version bump rebuilds just them.
set_source_files_properties(src/Console.cpp src/ConsoleHttp.cpp PROPERTIES
    COMPILE_DEFINITIONS "LUNAR_VERSION=\"${PROJECT_VERSION}\"")
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(ICU REQUIRED IMPORTED_TARGET icu-uc)
# The console's debug, cache and hook files reach into the VM's own structures
# (lobject.h's Closure and Proto, lstate.h's lua_State), which Luau.VM keeps
# to itself. Luau is fetched SYSTEM, so nothing it declares warns here.
target_include_directories(lunarstone_script SYSTEM PRIVATE ${luau_SOURCE_DIR}/VM/src)
target_link_libraries(lunarstone_script
    PUBLIC lunarstone::engine
    PRIVATE lunarstone::avatar lunarstone::assets Luau.VM Luau.Compiler PkgConfig::ZSTD PkgConfig::ICU PkgConfig::LIBCRYPTO
            lunarstone_warnings
)
