# V2M Playback Plugin
#
# This plugin plays V2 Synthesizer Music files (v2m) using jgilje's v2m-player.
# V2M is the format used by Farbrausch's V2 synthesizer in demoscene productions.

cmake_minimum_required(VERSION 3.16)
project(playback_v2m C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

# Download v2m-player (jgilje fork, BZRPlayer-tested)
download_library(
    NAME v2m
    URL "https://github.com/jgilje/v2m-player/archive/385ad6956202d09a2912ef91df9c44e13f9e4a84.tar.gz"
    FILENAME "v2m-player-385ad69.tar.gz"
    HASH "b98a6707445e81434ecc051a5be91155fda667eafff2933da2e4495677a4b13e"
    EXTRACT_DIR "v2m-player-385ad6956202d09a2912ef91df9c44e13f9e4a84"
    PATCHES
        patches/v2m-sound-fixes.patch
        patches/scope-capture.patch
)

# Build v2m as a static library
add_library(v2m_static STATIC
    ${v2m_SOURCE_DIR}/src/ronan.cpp
    ${v2m_SOURCE_DIR}/src/v2mplayer.cpp
    ${v2m_SOURCE_DIR}/src/v2mconv.cpp
    ${v2m_SOURCE_DIR}/src/synth_core.cpp
    ${v2m_SOURCE_DIR}/src/sounddef.cpp
)

target_include_directories(v2m_static PUBLIC
    ${v2m_SOURCE_DIR}/src
)

suppress_external_warnings(v2m_static)
set_target_properties(v2m_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 11
)

# Build plugin shared library
add_library(v2m_playback SHARED
    v2m_plugin.cpp
)

target_include_directories(v2m_playback PRIVATE
    ${v2m_SOURCE_DIR}/src
)

target_link_libraries(v2m_playback PRIVATE v2m_static)

if(NOT WIN32)
    target_link_libraries(v2m_playback PRIVATE stdc++ m)
endif()

target_compile_options(v2m_playback PRIVATE
    -Wno-unused-parameter
)

set_target_properties(v2m_playback PROPERTIES
    CXX_STANDARD 11
)

rv_set_plugin_properties(v2m_playback)
