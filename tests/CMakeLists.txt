add_executable(test_anchor_table test_anchor_table.cpp)
target_link_libraries(test_anchor_table PRIVATE soundsys)
target_include_directories(test_anchor_table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anchor_table COMMAND test_anchor_table)

add_executable(test_sync test_sync.cpp)
target_link_libraries(test_sync PRIVATE soundsys)
target_include_directories(test_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sync COMMAND test_sync)

add_executable(test_simlink test_simlink.cpp)
target_link_libraries(test_simlink PRIVATE soundsys)
target_include_directories(test_simlink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simlink COMMAND test_simlink)

add_executable(test_loopbank test_loopbank.cpp)
target_link_libraries(test_loopbank PRIVATE soundsys)
target_include_directories(test_loopbank PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME loop_bank COMMAND test_loopbank)
