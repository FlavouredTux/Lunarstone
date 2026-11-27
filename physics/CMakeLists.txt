add_library(lunarstone_physics
    src/Bvh.cpp
    src/Character.cpp
    src/Collide.cpp
    src/Constraints.cpp
    src/CollisionData.cpp
    src/ConvexHull.cpp
    src/Geometry.cpp
    src/HumanoidController.cpp
    src/Materials.cpp
    src/PlayerCamera.cpp
    src/ThreadPool.cpp
    src/World.cpp
)
add_library(lunarstone::physics ALIAS lunarstone_physics)

target_include_directories(lunarstone_physics PUBLIC include)
target_link_libraries(lunarstone_physics PUBLIC lunarstone::engine PRIVATE lunarstone::terrain lunarstone::assets lunarstone_warnings)
# Version 8 collision data is a zstd frame; the decoder is the assets library's own.
target_include_directories(lunarstone_physics PRIVATE ${PROJECT_SOURCE_DIR}/assets/src)
