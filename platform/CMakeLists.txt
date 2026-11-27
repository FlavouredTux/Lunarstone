# Raw Wayland windowing and input. Only system libraries: wayland-client,
# libxkbcommon, and protocol glue generated from wayland-protocols.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET GLOBAL xkbcommon) # GLOBAL: tests use its keysyms
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER_PKG wayland-scanner wayland_scanner)
find_program(WAYLAND_SCANNER NAMES wayland-scanner HINTS "${WAYLAND_SCANNER_PKG}" REQUIRED)

# The generated marshalling code is C.
enable_language(C)

set(protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${protocol_dir}")
set(protocol_sources "")

# Generates <name>-client-protocol.h and <name>-protocol.c from a protocol XML.
function(lunar_wayland_protocol xml)
    get_filename_component(name "${xml}" NAME_WE)
    set(header "${protocol_dir}/${name}-client-protocol.h")
    set(source "${protocol_dir}/${name}-protocol.c")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${header}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(
        OUTPUT "${source}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${source}"
        DEPENDS "${xml}" VERBATIM)
    set(protocol_sources ${protocol_sources} "${header}" "${source}" PARENT_SCOPE)
endfunction()

lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/stable/tablet/tablet-v2.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/staging/cursor-shape/cursor-shape-v1.xml")
lunar_wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")

# Kept apart from the project's warning flags: it's machine-written C.
add_library(lunarstone_wayland_protocols OBJECT ${protocol_sources})
target_link_libraries(lunarstone_wayland_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)
# SYSTEM so the generated inline wrappers don't trip C++ warnings or clang-tidy.
target_include_directories(lunarstone_wayland_protocols SYSTEM PUBLIC "${protocol_dir}")

add_library(lunarstone_platform
    src/Display.cpp
    src/Geometry.cpp
    src/InputState.cpp
    src/Key.cpp
    src/KeyRepeat.cpp
    src/Keyboard.cpp
    src/Window.cpp
)
add_library(lunarstone::platform ALIAS lunarstone_platform)

target_include_directories(lunarstone_platform PUBLIC include)
# Only for enum names in Key.cpp; nothing public needs it.
target_compile_options(lunarstone_platform PRIVATE -freflection)
target_link_libraries(lunarstone_platform
    PRIVATE lunarstone_wayland_protocols PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON lunarstone_warnings)
