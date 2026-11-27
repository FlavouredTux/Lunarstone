add_executable(lunarstone main.cpp AvatarRender.cpp BenchGraphics.cpp BenchReplay.cpp BenchStats.cpp ConsoleApi.cpp Control.cpp GameDetails.cpp GameWindow.cpp LiveAnimation.cpp LiveCharacter.cpp LiveChat.cpp LiveClient.cpp LivePlay.cpp LuaApp.cpp MapScene.cpp MusicVisualizer.cpp NetJoin.cpp NetTools.cpp OffscreenUi.cpp Online.cpp PlaceScene.cpp PlaySession.cpp Playtest.cpp Settings.cpp TerrainExport.cpp ToolActions.cpp UiTest.cpp UnionShapes.cpp Viewer.cpp)
target_link_libraries(lunarstone PRIVATE lunarstone::engine lunarstone::platform lunarstone::render lunarstone::script lunarstone::terrain lunarstone::assets lunarstone::physics lunarstone::avatar lunarstone::ui lunarstone::net lunarstone::audio lunarstone_warnings)
target_sources(lunarstone PRIVATE RtcChat.cpp)
# /v1/status reports the project's version; only ConsoleApi.cpp needs it, so
# a version bump rebuilds that one file (as script/src/Console.cpp does).
set_source_files_properties(ConsoleApi.cpp PROPERTIES COMPILE_DEFINITIONS "LUNAR_VERSION=\"${PROJECT_VERSION}\"")
