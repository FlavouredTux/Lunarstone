find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

# GLSL -> SPIR-V at build time; Shaders.cpp #embeds the results.
set(LUNAR_SHADERS mesh.vert mesh.frag shadow.vert shadow.frag sky.vert sky.frag map.vert map.frag)
# Beams, trails and other blended effects (EffectPass.cpp #embeds these itself).
list(APPEND LUNAR_SHADERS effect.vert effect.frag)
# Bloom, blur, sun rays (traced too), depth of field (PostPass.cpp #embeds these itself).
list(APPEND LUNAR_SHADERS post.vert post.frag shafts.frag)
# Highlights: their mask, drawn with mesh.vert, which ids each tile of it has, and their fill and
# outline (HighlightPass.cpp).
list(APPEND LUNAR_SHADERS highlight_mask.frag highlight.frag highlight_tiles.comp)
# Ray traced local lights' shared shadow ray, denoised (the renderer's denoise pass) and composited.
list(APPEND LUNAR_SHADERS denoise.comp denoise_composite.frag)
# Decals on solid parts, into the layer the opaque pass reads (drawn with mesh.vert).
list(APPEND LUNAR_SHADERS decal_layer.frag)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(spirv_files)
foreach(shader IN LISTS LUNAR_SHADERS)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
    set(output ${shader_dir}/${shader}.spv)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -Werror -O -MD -MF ${output}.d -o ${output} ${source}
        DEPENDS ${source}
        DEPFILE ${output}.d
        COMMENT "Compiling shader ${shader}"
        VERBATIM
    )
    list(APPEND spirv_files ${output})
endforeach()
# mesh.frag again with ray queries, for contexts that have them (it won't
# load on devices without the capability): as the see-through passes draw
# with it, and as the opaque pass and the decals do, which also hand the
# denoiser what the local lights' shared shadow ray found (DENOISE).
foreach(variant IN ITEMS rt rt-denoise)
    set(defines -DRAYTRACING)
    if(variant STREQUAL "rt-denoise")
        list(APPEND defines -DDENOISE)
    endif()
    set(output ${shader_dir}/mesh.${variant}.frag.spv)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -Werror -O ${defines} -MD -MF ${output}.d -o ${output}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag
        DEPFILE ${output}.d
        COMMENT "Compiling shader mesh.frag with ray queries (${variant})"
        VERBATIM
    )
    list(APPEND spirv_files ${output})
endforeach()

add_library(lunarstone_render
    src/Allocator.cpp
    src/BuiltinMaterials.cpp
    src/Camera.cpp
    src/Context.cpp
    src/EffectPass.cpp
    src/Image.cpp
    src/Lighting.cpp
    src/LightGrid.cpp
    src/LightClusters.cpp
    src/LocalLights.cpp
    src/Graphics.cpp
    src/Lod.cpp
    src/MapMesh.cpp
    src/MapRenderer.cpp
    src/Material.cpp
    src/Offscreen.cpp
    src/Particles.cpp
    src/PostPass.cpp
    src/HighlightPass.cpp
    src/Presenter.cpp
    src/Primitives.cpp
    src/RayScene.cpp
    src/RayTracing.cpp
    src/Ribbons.cpp
    src/Adornments.cpp
    src/Renderer.cpp
    src/Scene.cpp
    src/Shaders.cpp
    src/Textures.cpp
)
add_library(lunarstone::render ALIAS lunarstone_render)

# A target of its own so the SPIR-V exists before module scanning preprocesses Shaders.cpp.
add_custom_target(lunarstone_shaders DEPENDS ${spirv_files})
add_dependencies(lunarstone_render lunarstone_shaders)
set_source_files_properties(src/Shaders.cpp src/EffectPass.cpp PROPERTIES OBJECT_DEPENDS "${spirv_files}")
target_include_directories(lunarstone_render PUBLIC include)
target_compile_options(lunarstone_render PRIVATE --embed-dir=${shader_dir})
target_link_libraries(lunarstone_render PUBLIC lunarstone::engine Vulkan::Vulkan PRIVATE lunarstone_warnings)
