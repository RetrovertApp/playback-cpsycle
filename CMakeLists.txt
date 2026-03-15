# cpsycle Playback Plugin
#
# Psycle tracker (.psy) playback using the cpsycle audio engine.
# Only built-in machines produce audio; PSY files using third-party
# VST/LADSPA plugins will have those channels replaced with silent Dummy machines.

cmake_minimum_required(VERSION 3.16)
project(playback_cpsycle C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

add_subdirectory(external/zlib)

# Make vendored zlib visible to cpsycle-lib's find_package(ZLIB)
set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY zlib_static CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES zlib_static)

# Download pre-patched cpsycle library (cross-platform fixes already applied)
download_library(
    NAME cpsycle
    URL "https://github.com/emoon/cpsycle-lib/archive/9187443880a6f315885675102e97ffec6e7ebb04.tar.gz"
    FILENAME "cpsycle-lib-9187443880a6f315885675102e97ffec6e7ebb04.tar.gz"
    HASH "f08752029bcebf5dd695cab20dc3a408482596cb671e456e2d36901cccb44ef5"
    EXTRACT_DIR "cpsycle-lib-9187443880a6f315885675102e97ffec6e7ebb04"
)

# Build cpsycle static libraries via its own CMakeLists.txt
add_subdirectory(${cpsycle_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/cpsycle-build EXCLUDE_FROM_ALL)

# Suppress warnings and add Clang-specific flags for cpsycle's callback-heavy C code
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CPSYCLE_EXTRA_C_FLAGS
        -Wno-incompatible-function-pointer-types
        -Wno-int-conversion
        -Wno-implicit-function-declaration
    )
endif()

foreach(tgt cpsycle_container cpsycle_file cpsycle_thread cpsycle_dsp cpsycle_audio)
    suppress_external_warnings(${tgt})
    # Force C11 to avoid GCC 15+ C23 'bool' keyword conflict with typedef
    set_target_properties(${tgt} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(${tgt} PRIVATE ${CPSYCLE_EXTRA_C_FLAGS})
endforeach()

# =============================================================================
# cpsycle_playback shared library (the final plugin)
# =============================================================================

add_library(cpsycle_playback SHARED
    cpsycle_plugin.c
    cpsycle_stubs.c
)

suppress_external_warnings(cpsycle_playback)

target_link_libraries(cpsycle_playback PRIVATE
    cpsycle_audio
    cpsycle_dsp
    cpsycle_file
    cpsycle_thread
    cpsycle_container
    ZLIB::ZLIB
)

if(UNIX)
    target_link_libraries(cpsycle_playback PRIVATE m pthread)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(cpsycle_playback PRIVATE dl)
endif()

rv_set_plugin_properties(cpsycle_playback)
