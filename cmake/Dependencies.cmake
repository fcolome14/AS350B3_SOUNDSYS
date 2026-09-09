# Third-party dependencies, all fetched at configure time and pinned.
#
# The module stays deliberately thin: one resampler, one device backend, one WAV
# decoder. Everything else (mixing, event routing, the NG mapping) is ours.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- dr_wav: single-header WAV decode/encode ---------------------------------
FetchContent_Declare(
    dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG        dfe8377631000664666519fdb83da193fd8037f4  # master, 2026-09
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(dr_libs)
add_library(soundsys_dr_wav INTERFACE)
target_include_directories(soundsys_dr_wav INTERFACE ${dr_libs_SOURCE_DIR})

# --- miniaudio: playback device backend --------------------------------------
if(SOUNDSYS_USE_MINIAUDIO)
    FetchContent_Declare(
        miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG        0.11.25
        GIT_SHALLOW    TRUE
        # miniaudio ships a CMakeLists of its own that builds tests and examples;
        # point at a directory that does not exist so we only get the header.
        SOURCE_SUBDIR  no_cmake
    )
    FetchContent_MakeAvailable(miniaudio)
    add_library(soundsys_miniaudio INTERFACE)
    target_include_directories(soundsys_miniaudio INTERFACE ${miniaudio_SOURCE_DIR})
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(soundsys_miniaudio INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
endif()

# --- libsamplerate: variable-ratio resampling --------------------------------
if(SOUNDSYS_USE_LIBSAMPLERATE)
    set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libsamplerate
        GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
        # Past the 0.2.2 tag: that release still declares cmake_minimum_required
        # below 3.5, which CMake 4 refuses to configure.
        GIT_TAG        0844c208f683527c08ea8a80acc13b398aa9c8bf
        GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(libsamplerate)
endif()
