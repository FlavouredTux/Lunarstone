find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(LIBCRYPTO REQUIRED IMPORTED_TARGET libcrypto)

add_library(lunarstone_net
    src/BitStream.cpp
    src/CharacterSync.cpp
    src/Capture.cpp
    src/CaptureDecoder.cpp
    src/Crypto.cpp
    src/Datagram.cpp
    src/Describe.cpp
    src/Join.cpp
    src/JoinData.cpp
    src/Messages.cpp
    src/Motion.cpp
    src/NetworkStream.cpp
    src/Offline.cpp
    src/Peer.cpp
    src/Physics.cpp
    src/Reliability.cpp
    src/Remotes.cpp
    src/Replication.cpp
    src/Rupp.cpp
    src/Session.cpp
    src/Socket.cpp
    src/Streaming.cpp
    src/StreamingClient.cpp
    src/ToolReplication.cpp
    src/Values.cpp
)
add_library(lunarstone::net ALIAS lunarstone_net)

target_include_directories(lunarstone_net PUBLIC include)
target_link_libraries(lunarstone_net PUBLIC lunarstone::engine lunarstone::assets PRIVATE lunarstone::avatar PkgConfig::SODIUM PkgConfig::ZSTD PkgConfig::LIBCRYPTO lunarstone_warnings)
