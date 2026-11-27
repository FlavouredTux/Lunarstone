add_executable(lunarstone_tests
    main.cpp
    assets_cache_tests.cpp
    assets_web_tests.cpp
    assets_websocket_tests.cpp
    assets_social_tests.cpp
    assets_image_tests.cpp
    assets_mesh_tests.cpp
    assets_simplify_tests.cpp
    assets_texture_tests.cpp
    audio_tests.cpp
    avatar_animate_tests.cpp
    avatar_animator_tests.cpp
    avatar_character_tests.cpp
    avatar_clip_tests.cpp
    avatar_easing_tests.cpp
    avatar_math_tests.cpp
    avatar_replicated_tests.cpp
    avatar_user_tests.cpp
    ui_font_tests.cpp
    ui_health_bars_tests.cpp
    ui_layout_tests.cpp
    ui_invite_tests.cpp
    chat_tabs_tests.cpp
    core_gui_compat_tests.cpp
    modern_chat_tests.cpp
    input_actions_tests.cpp
    emotes_tests.cpp
    ui_loading_screen_tests.cpp
    ui_menu_tests.cpp
    ui_text_tests.cpp
    ui_render_tests.cpp
    instance_tests.cpp
    blob_tests.cpp
    class_tests.cpp
    datatype_tests.cpp
    lz4_tests.cpp
    net_tests.cpp
    net_join_tests.cpp
    net_motion_tests.cpp
    net_physics_tests.cpp
    net_character_tests.cpp
    net_joindata_tests.cpp
    net_remotes_tests.cpp
    net_text_chat_tests.cpp
    app_console_api_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/ConsoleApi.cpp
    app_playtest_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/Playtest.cpp
    app_live_chat_tests.cpp
    app_rtc_chat_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/RtcChat.cpp
    ${CMAKE_SOURCE_DIR}/app/LiveChat.cpp
    app_place_scene_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/PlaceScene.cpp
    app_settings_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/Settings.cpp
    app_game_details_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/GameDetails.cpp
    bench_graphics_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/BenchGraphics.cpp
    bench_stats_tests.cpp
    ${CMAKE_SOURCE_DIR}/app/BenchStats.cpp
    net_stream_tests.cpp
    net_streaming_client_tests.cpp
    net_tools_tests.cpp
    math_tests.cpp
    physics_bench_tests.cpp
    physics_camera_tests.cpp
    physics_constraint_tests.cpp
    physics_geometry_tests.cpp
    physics_humanoid_tests.cpp
    physics_mesh_tests.cpp
    physics_seat_tests.cpp
    physics_thread_tests.cpp
    physics_world_tests.cpp
    platform_tests.cpp
    property_tests.cpp
    render_camera_tests.cpp
    render_effects_tests.cpp
    render_material_tests.cpp
    render_ray_tracing_tests.cpp
    render_lightgrid_tests.cpp
    render_lights_tests.cpp
    render_lod_tests.cpp
    render_map_tests.cpp
    render_math_tests.cpp
    render_offscreen_tests.cpp
    render_post_tests.cpp
    render_particles_tests.cpp
    render_primitive_tests.cpp
    render_ribbons_tests.cpp
    render_scene_tests.cpp
    render_sky_tests.cpp
    render_texture_tests.cpp
    render_vulkan_tests.cpp
    rbxl_tests.cpp
    tool_tests.cpp
    prompt_tests.cpp
    rbxlx_tests.cpp
    script_animation_tests.cpp
    script_console_tests.cpp
    script_console_unc_tests.cpp
    script_console_hook_tests.cpp
    script_console_http_tests.cpp
    render_adornments_tests.cpp
    script_datatype_tests.cpp
    script_players_web_tests.cpp
    script_social_tests.cpp
    script_runtime_tests.cpp
    lua_app_tests.cpp
    style_engine_tests.cpp
    script_tests.cpp
    signal_tests.cpp
    terrain_materials_tests.cpp
    terrain_lod_tests.cpp
    terrain_mesher_tests.cpp
    terrain_smooth_mesher_tests.cpp
    window_tests.cpp
    xml_tests.cpp
)
target_link_libraries(lunarstone_tests PRIVATE lunarstone::engine lunarstone::platform lunarstone::render lunarstone::script lunarstone::terrain lunarstone::assets lunarstone::physics lunarstone::avatar lunarstone::ui lunarstone::net lunarstone::audio lunarstone_warnings)
# The client's chat helpers (app/LiveChat) are tested where they live.
target_include_directories(lunarstone_tests PRIVATE ${CMAKE_SOURCE_DIR}/app)
# The console API's /v1/status reports the version (app/CMakeLists.txt sets it there),
# and the console's request() sends it in its User-Agent.
set_source_files_properties(${CMAKE_SOURCE_DIR}/app/ConsoleApi.cpp script_console_http_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "LUNAR_VERSION=\"${PROJECT_VERSION}\"")
# Test data files committed with the tests.
target_compile_definitions(lunarstone_tests PRIVATE LUNAR_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
# For keysym constants in the platform tests.
target_link_libraries(lunarstone_tests PRIVATE PkgConfig::XKBCOMMON)

# Optional reference data for comparison tests.
set(LUNAR_RESEARCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/reference-data" CACHE PATH "Directory for optional test reference data")
target_compile_definitions(lunarstone_tests PRIVATE LUNAR_RESEARCH_DIR="${LUNAR_RESEARCH_DIR}")

add_test(NAME lunarstone_tests COMMAND lunarstone_tests)

# The tools' own tests: tools/bench_compare.py and tools/png_compare.py need no
# client build, no recording and no GPU -- png_compare.py even reads the pngs
# itself, out of the standard library alone -- so they run wherever there is a
# python3. They find their tools from their own path, so the working directory
# does not matter.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME bench_compare_tests
             COMMAND ${Python3_EXECUTABLE} -m unittest discover
                     -s ${CMAKE_CURRENT_SOURCE_DIR} -p bench_compare_tests.py)
    add_test(NAME png_compare_tests
             COMMAND ${Python3_EXECUTABLE} -m unittest discover
                     -s ${CMAKE_CURRENT_SOURCE_DIR} -p png_compare_tests.py)
    add_test(NAME gen_script_api_tests
             COMMAND ${Python3_EXECUTABLE} -m unittest discover
                     -s ${CMAKE_CURRENT_SOURCE_DIR} -p gen_script_api_tests.py)
endif()
