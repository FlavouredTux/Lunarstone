# Sound: Roblox's audio (Ogg Vorbis, MP3, WAV) decoded and played through
# PulseAudio, which PipeWire serves too. Only system libraries, each one
# optional: without a decoder those files stay silent, without libpulse
# there's no output.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VORBISFILE QUIET IMPORTED_TARGET vorbisfile)
pkg_check_modules(MPG123 QUIET IMPORTED_TARGET libmpg123)
pkg_check_modules(PULSE_SIMPLE QUIET IMPORTED_TARGET libpulse-simple)

add_library(lunarstone_audio
    src/Decode.cpp
    src/Effects.cpp
    src/Mixer.cpp
    src/Music.cpp
    src/SoundSystem.cpp
)
add_library(lunarstone::audio ALIAS lunarstone_audio)

target_include_directories(lunarstone_audio PUBLIC include)
target_link_libraries(lunarstone_audio PUBLIC lunarstone::engine PRIVATE lunarstone::assets lunarstone_warnings)
foreach(lib VORBISFILE MPG123 PULSE_SIMPLE)
    if(${lib}_FOUND)
        target_link_libraries(lunarstone_audio PRIVATE PkgConfig::${lib})
    endif()
endforeach()
target_compile_definitions(lunarstone_audio PRIVATE
    LUNAR_WITH_VORBIS=$<BOOL:${VORBISFILE_FOUND}>
    LUNAR_WITH_MPG123=$<BOOL:${MPG123_FOUND}>
    LUNAR_WITH_PULSE=$<BOOL:${PULSE_SIMPLE_FOUND}>)
