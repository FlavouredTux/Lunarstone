add_library(lunarstone_terrain
    src/TerrainMaterials.cpp
    src/TerrainLod.cpp
    src/SmoothMesher.cpp
    src/TerrainMesher.cpp
)
add_library(lunarstone::terrain ALIAS lunarstone_terrain)

target_include_directories(lunarstone_terrain PUBLIC include)
target_link_libraries(lunarstone_terrain PUBLIC lunarstone::engine PRIVATE lunarstone_warnings)
