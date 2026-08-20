# V2M Playback Plugin
#
# This plugin plays V2 Synthesizer Music files (v2m) using v2redux, a clean-room
# C++17 port of the V2 engine that renders every v2m format version (0..6) with
# its own era's behaviour. V2M is the format used by Farbrausch's V2 synthesizer
# in demoscene productions.

cmake_minimum_required(VERSION 3.16)
project(playback_v2m C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

download_library(
    NAME v2redux
    URL "https://github.com/spheenik/v2redux/archive/5d3157b7c312fbfe2f82430dd3e819594cf3205b.tar.gz"
    FILENAME "v2redux-5d3157b.tar.gz"
    HASH "ce1c709bcfadbb754a102c35d9c0da190abed7a55233d7e1cda83e7de618a85a"
    EXTRACT_DIR "v2redux-5d3157b7c312fbfe2f82430dd3e819594cf3205b"
    PATCHES
        patches/v2redux-scope-capture.patch
)

# Build v2redux as a static library
add_library(v2redux_static STATIC
    ${v2redux_SOURCE_DIR}/src/v2player.cpp
    ${v2redux_SOURCE_DIR}/src/v2load.cpp
    ${v2redux_SOURCE_DIR}/src/v2core.cpp
    ${v2redux_SOURCE_DIR}/src/v2seq.cpp
    ${v2redux_SOURCE_DIR}/src/ronan.cpp
)

target_include_directories(v2redux_static PUBLIC
    ${v2redux_SOURCE_DIR}/src
)

# v2core.cpp never includes v2redux.h, so the V2_RONAN=1 default from that
# header does not reach it: it compiles the no-op speech stubs, which
# collide with ronan.cpp's real definitions under lld-link and silently
# disable speech under ELF archive resolution. Define it for every TU.
target_compile_definitions(v2redux_static PRIVATE V2_RONAN=1)

# v2redux's determinism contract: no FMA contraction, no fast-math, SSE2 rather
# than x87. Without these the engine still plays, but not bit-identically.
if(MSVC)
    target_compile_options(v2redux_static PRIVATE /fp:precise)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_options(v2redux_static PUBLIC /arch:SSE2)
    endif()
else()
    target_compile_options(v2redux_static PRIVATE -ffp-contract=off)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "i[0-9]86|x86")
        target_compile_options(v2redux_static PUBLIC -msse2 -mfpmath=sse)
    endif()
endif()

suppress_external_warnings(v2redux_static)
set_target_properties(v2redux_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
)

# Build plugin shared library
add_library(v2m_playback SHARED
    v2m_plugin.cpp
)

target_link_libraries(v2m_playback PRIVATE v2redux_static)

if(NOT WIN32)
    target_link_libraries(v2m_playback PRIVATE m)
endif()

target_compile_options(v2m_playback PRIVATE
    -Wno-unused-parameter
)

set_target_properties(v2m_playback PROPERTIES
    CXX_STANDARD 17
)

rv_set_plugin_properties(v2m_playback)

# Self-check for the pattern-grid extractor, run against the v2redux corpus.
option(V2M_BUILD_TESTS "Build the pattern extraction self-check" OFF)
if(V2M_BUILD_TESTS)
    enable_testing()
    add_executable(v2m_test_pattern test_pattern.cpp)
    target_link_libraries(v2m_test_pattern PRIVATE v2redux_static)
    target_include_directories(v2m_test_pattern PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(v2m_test_pattern PROPERTIES CXX_STANDARD 17)
    target_compile_options(v2m_test_pattern PRIVATE -UNDEBUG) # the asserts are the test
    file(GLOB V2M_CORPUS
        ${v2redux_SOURCE_DIR}/corpus/*.v2m
        ${v2redux_SOURCE_DIR}/corpus/embedded/*.v2m)
    add_test(NAME v2m_pattern COMMAND v2m_test_pattern ${V2M_CORPUS})
endif()
