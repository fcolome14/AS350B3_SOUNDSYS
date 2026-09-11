cmake_minimum_required(VERSION 3.20)
project(as350b3_soundsys VERSION 0.1.0 LANGUAGES CXX)

option(SOUNDSYS_USE_LIBSAMPLERATE "Use libsamplerate for variable-ratio playback" ON)
option(SOUNDSYS_USE_MINIAUDIO     "Build the miniaudio playback backend"          ON)
option(SOUNDSYS_BUILD_APPS        "Build the offline renderer and live demo"      ON)
option(SOUNDSYS_BUILD_TESTS       "Build the tests"                               ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

include(cmake/Dependencies.cmake)

# The wire contract with the VEMD. Its own tiny project so other modules can
# pull in just that directory.
add_subdirectory(simlink)

add_library(soundsys STATIC
    src/link/SimLinkAdapter.cpp
    src/AudioEngine.cpp
    src/AudioBuffer.cpp
    src/AnchorTable.cpp
    src/PlaybackHead.cpp
    src/VarispeedReader.cpp
    src/TimeStretchReader.cpp
    src/voices/TurbineStartVoice.cpp
    src/voices/LoopVoice.cpp
    src/voices/NgLoopBankVoice.cpp
    src/backend/MiniaudioDevice.cpp
)
add_library(soundsys::soundsys ALIAS soundsys)

target_include_directories(soundsys PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(soundsys PRIVATE soundsys_dr_wav)
target_link_libraries(soundsys PUBLIC simlink::simlink)

if(SOUNDSYS_USE_LIBSAMPLERATE)
    target_link_libraries(soundsys PRIVATE SampleRate::samplerate)
    target_compile_definitions(soundsys PRIVATE SOUNDSYS_WITH_LIBSAMPLERATE)
endif()

if(SOUNDSYS_USE_MINIAUDIO)
    target_link_libraries(soundsys PRIVATE soundsys_miniaudio)
    target_compile_definitions(soundsys PRIVATE SOUNDSYS_WITH_MINIAUDIO)
endif()

if(MSVC)
    target_compile_options(soundsys PRIVATE /W4 /permissive-)
else()
    target_compile_options(soundsys PRIVATE -Wall -Wextra)
endif()

if(SOUNDSYS_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(SOUNDSYS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
