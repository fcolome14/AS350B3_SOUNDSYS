add_executable(soundsys_render render_start.cpp)
target_link_libraries(soundsys_render PRIVATE soundsys)
target_include_directories(soundsys_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(SOUNDSYS_USE_MINIAUDIO)
    add_executable(soundsys_play play_start.cpp)
    target_link_libraries(soundsys_play PRIVATE soundsys)
    target_include_directories(soundsys_play PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # The audio process of the simulator: listens to the VEMD over simlink.
    add_executable(soundsys_host soundsys_host.cpp)
    target_link_libraries(soundsys_host PRIVATE soundsys)
endif()
