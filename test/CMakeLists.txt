cmake_minimum_required(VERSION 3.17)
project(enuwatch_tests LANGUAGES CXX)

file(GLOB TEST_SRC *.cpp)
foreach(test ${TEST_SOURCES})
    get_filename_component(testname ${test} NAME_WE)
    add_executable(${testname} ${test})

    set_target_properties(${testname} PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF LINKER_LANGUAGE CXX)
    target_link_libraries(${testname} PRIVATE enuwatch)
    target_include_directories(${testname} PRIVATE ${ENU_INCLUDE_DIRS})
endforeach(test ${TEST_SOURCES})
