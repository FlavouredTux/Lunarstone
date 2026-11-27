find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_library(lunarstone_engine
    src/Base64.cpp
    src/BinaryFormat.cpp
    src/BrickColor.cpp
    src/Builder.cpp
    src/ClassRegistry.cpp
    src/Classes.cpp
    src/Enums.cpp
    src/Emotes.cpp
    src/Instance.cpp
    src/Input.cpp
    src/Lz4.cpp
    src/PlaceFile.cpp
    src/Rbxl.cpp
    src/Rbxlx.cpp
    src/Seats.cpp
    src/StyleEngine.cpp
    src/Selector.cpp
    src/Prompts.cpp
    src/Tools.cpp
    src/Value.cpp
    src/Xml.cpp
)
add_library(lunarstone::engine ALIAS lunarstone_engine)

# The class table's slices (see src/ClassRegistrySlice.hpp), compiled in parallel.
set(LUNAR_CLASS_SLICES 8)
math(EXPR lastSlice "${LUNAR_CLASS_SLICES} - 1")
foreach(LUNAR_SLICE RANGE ${lastSlice})
    set(slice "${CMAKE_CURRENT_BINARY_DIR}/ClassRegistrySlice${LUNAR_SLICE}.cpp")
    configure_file(src/ClassRegistrySlice.cpp.in "${slice}" @ONLY)
    target_sources(lunarstone_engine PRIVATE "${slice}")
endforeach()
target_include_directories(lunarstone_engine PRIVATE src)

target_include_directories(lunarstone_engine PUBLIC include)
# InstanceImpl uses reflection in a header, so everyone including it needs the flag.
target_compile_options(lunarstone_engine PUBLIC -freflection)
# ZSTD: the binary format's chunks are LZ4 or ZSTD compressed.
target_link_libraries(lunarstone_engine PRIVATE PkgConfig::ZSTD lunarstone_warnings)
