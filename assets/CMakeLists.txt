add_library(lunarstone_assets
    src/AssetCache.cpp
    src/BlockCompression.cpp
    src/BmpTga.cpp
    src/ContentId.cpp
    src/Dds.cpp
    src/DecodedMeshes.cpp
    src/DecodedTextures.cpp
    src/DerivedCache.cpp
    src/Http.cpp
    src/HttpCache.cpp
    src/Json.cpp
    src/ImageDecode.cpp
    src/Inflate.cpp
    src/Jpeg.cpp
    src/Ktx2.cpp
    src/MeshDraco.cpp
    src/MeshFile.cpp
    src/MeshLoader.cpp
    src/MeshSimplify.cpp
    src/TextureLoader.cpp
    src/Social.cpp
    src/Web.cpp
    src/WebSocket.cpp
    src/Png.cpp
    src/Zstd.cpp
)
add_library(lunarstone::assets ALIAS lunarstone_assets)

target_include_directories(lunarstone_assets PUBLIC include)
target_link_libraries(lunarstone_assets PUBLIC lunarstone::engine PRIVATE lunarstone_warnings)
# The decoded-texture cache keys its files by an XXH3 hash of the source (header only).
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
target_include_directories(lunarstone_assets PRIVATE ${XXHASH_INCLUDE_DIRS})

set(LUNAR_CONTENT_DIR ""
    CACHE PATH "RobloxPlayer content folder that rbxasset:// paths resolve in (LUNAR_CONTENT_DIR overrides it at run time)")
target_compile_definitions(lunarstone_assets PRIVATE LUNAR_DEFAULT_CONTENT_DIR="${LUNAR_CONTENT_DIR}")

# Downloads of assets that aren't cached yet.
find_package(CURL QUIET)
option(LUNAR_WITH_CURL "Download assets from Roblox with libcurl" ${CURL_FOUND})
if(LUNAR_WITH_CURL)
    find_package(CURL REQUIRED)
    target_link_libraries(lunarstone_assets PRIVATE CURL::libcurl)
endif()
target_compile_definitions(lunarstone_assets PRIVATE LUNAR_WITH_CURL=$<BOOL:${LUNAR_WITH_CURL}>)

# Version 7 meshes store their geometry as Google Draco bitstreams.
find_package(draco CONFIG QUIET)
option(LUNAR_WITH_DRACO "Decode Draco-compressed (version 7) meshes" ${draco_FOUND})
if(LUNAR_WITH_DRACO)
    find_package(draco CONFIG REQUIRED)
    target_link_libraries(lunarstone_assets PRIVATE draco::draco)
endif()
target_compile_definitions(lunarstone_assets PRIVATE LUNAR_WITH_DRACO=$<BOOL:${LUNAR_WITH_DRACO}>)
