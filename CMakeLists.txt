cmake_minimum_required(VERSION 3.30)
project(Lunarstone VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "Lunarstone needs GCC 16 or newer (C++26 static reflection)")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules here: skip the per-file dependency scan (ccache can't cache it either).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(LUNAR_BUILD_TESTS "Build the test suite" ON)
option(LUNAR_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(LUNAR_TSAN "Build with ThreadSanitizer (not with LUNAR_SANITIZE)" OFF)
option(LUNAR_WERROR "Treat warnings as errors" OFF)

# Faster rebuilds when the tools are installed. ccache's base directory is
# the one holding the checkout, so the agents' sibling worktrees share hits.
find_program(LUNAR_CCACHE ccache)
if(LUNAR_CCACHE)
    cmake_path(GET CMAKE_SOURCE_DIR PARENT_PATH ccacheBase)
    set(CMAKE_CXX_COMPILER_LAUNCHER
        ${CMAKE_COMMAND} -E env CCACHE_BASEDIR=${ccacheBase} CCACHE_NOHASHDIR=1 ${LUNAR_CCACHE})
endif()
find_program(LUNAR_MOLD mold)
if(LUNAR_MOLD)
    add_link_options(-fuse-ld=mold)
endif()

if(LUNAR_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(LUNAR_TSAN)
    if(LUNAR_SANITIZE)
        message(FATAL_ERROR "LUNAR_TSAN and LUNAR_SANITIZE don't go together")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

add_library(lunarstone_warnings INTERFACE)
target_compile_options(lunarstone_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow
    -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wold-style-cast -Wcast-qual -Wcast-align=strict
    -Wnon-virtual-dtor -Woverloaded-virtual
    -Wnull-dereference -Wimplicit-fallthrough -Wformat=2
    -Wduplicated-cond -Wduplicated-branches -Wlogical-op
    -Wextra-semi -Wredundant-decls -Wmissing-declarations
    # Designated initializers that leave the rest zeroed are how Vulkan structs get filled.
    -Wno-missing-field-initializers
    $<$<BOOL:${LUNAR_WERROR}>:-Werror>
)

add_subdirectory(engine)
add_subdirectory(terrain)
add_subdirectory(physics)
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(script)
add_subdirectory(assets)
add_subdirectory(audio)
add_subdirectory(avatar)
add_subdirectory(ui)
add_subdirectory(net)
add_subdirectory(app)

if(LUNAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
