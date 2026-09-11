# simlink - the wire contract between simulator modules.
#
# Deliberately its own CMake project with no dependency on the audio engine:
# the VEMD (or any other module) pulls in just this directory, either from a
# sibling checkout (add_subdirectory) or from git (FetchContent with
# SOURCE_SUBDIR simlink), and gets one small static library.
cmake_minimum_required(VERSION 3.20)

if(TARGET simlink::simlink)
    return()
endif()

project(simlink VERSION 1.0.0 LANGUAGES CXX)

add_library(simlink STATIC src/Udp.cpp)
add_library(simlink::simlink ALIAS simlink)

target_include_directories(simlink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(simlink PUBLIC cxx_std_17)

if(WIN32)
    target_link_libraries(simlink PUBLIC ws2_32)
endif()
