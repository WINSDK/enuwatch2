cmake_minimum_required(VERSION 3.17)
project(enu LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
   CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
    set(CLANG TRUE)
endif()

option(ENU_TEST "Build tests" OFF)

add_subdirectory("src/")

if (ENU_TEST)
    set(ENU_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}) # Not sure what this does

    enable_testing()
    add_subdirectory("test/")

    file(GLOB TEST_SRC test/*.cpp)
    foreach(test ${TEST_SRC})
        get_filename_component(testname ${test} NAME_WE)

        set(command $<TARGET_FILE:${testname}>)

        add_test(NAME ${testname}
            COMMAND ${command}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test")
    endforeach(test ${TEST_SOURCES})
endif()
