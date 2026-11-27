add_library(lunarstone_avatar
    src/AnimationSystem.cpp
    src/Animate.cpp
    src/Animator.cpp
    src/Character.cpp
    src/Clip.cpp
    src/ClipLoader.cpp
    src/Clothing.cpp
    src/ReplicatedAnimation.cpp
    src/Rig.cpp
    src/UserAvatar.cpp
)
add_library(lunarstone::avatar ALIAS lunarstone_avatar)

target_include_directories(lunarstone_avatar PUBLIC include)
target_link_libraries(lunarstone_avatar PUBLIC lunarstone::engine lunarstone::assets PRIVATE lunarstone_warnings)
