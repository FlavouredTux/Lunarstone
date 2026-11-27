find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

# GLSL -> SPIR-V at build time; UiRenderer.cpp #embeds the results.
set(LUNAR_UI_SHADERS ui.vert ui.frag)
set(ui_shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(ui_spirv_files)
foreach(shader IN LISTS LUNAR_UI_SHADERS)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
    set(output ${ui_shader_dir}/${shader}.spv)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ui_shader_dir}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -Werror -O -MD -MF ${output}.d -o ${output} ${source}
        DEPENDS ${source}
        DEPFILE ${output}.d
        COMMENT "Compiling shader ${shader}"
        VERBATIM
    )
    list(APPEND ui_spirv_files ${output})
endforeach()

add_library(lunarstone_ui
    src/BackpackGui.cpp
    src/ClassicChat.cpp
    src/ModernChat.cpp
    src/Notifications.cpp
    src/CoreGui.cpp
    src/Topbar.cpp
    src/DevConsole.cpp
    src/DrawList.cpp
    src/Font.cpp
    src/FontLibrary.cpp
    src/GameMenu.cpp
    src/GameUi.cpp
    src/GameInvitePrompt.cpp
    src/Gui.cpp
    src/HealthBars.cpp
    src/Json.cpp
    src/LoadingScreen.cpp
    src/LuauHighlight.cpp
    src/TextEditor.cpp
    src/TextLayout.cpp
    src/UiRenderer.cpp
    src/ViewportFrames.cpp
    src/WorldMap.cpp
)
add_library(lunarstone::ui ALIAS lunarstone_ui)

add_custom_target(lunarstone_ui_shaders DEPENDS ${ui_spirv_files})
add_dependencies(lunarstone_ui lunarstone_ui_shaders)
set_source_files_properties(src/UiRenderer.cpp PROPERTIES OBJECT_DEPENDS "${ui_spirv_files}")
target_include_directories(lunarstone_ui PUBLIC include)
target_compile_options(lunarstone_ui PRIVATE --embed-dir=${ui_shader_dir})
target_link_libraries(lunarstone_ui PUBLIC lunarstone::engine lunarstone::render lunarstone::platform PRIVATE lunarstone_warnings)
