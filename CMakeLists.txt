cmake_minimum_required(VERSION 3.17)
project(enu LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
   CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
    set(CLANG TRUE)
endif()

if(APPLE)
    # Weirdness with clang not linking when using std::println
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++")
endif()

option(ENU_TEST "Build tests" OFF)

set(BUILD_SHARED_LIBS OFF)
set(TBB_TEST OFF CACHE INTERNAL "")
set(TBB_STRICT OFF CACHE INTERNAL "")
add_subdirectory(third-party/tbb EXCLUDE_FROM_ALL)
target_compile_definitions(tbb PRIVATE __TBB_DYNAMIC_LOAD_ENABLED=0)

add_subdirectory("enu/")

if (ENU_TEST)
    set(ENU_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})

    enable_testing()
    add_subdirectory("test/")

    file(GLOB TEST_SRC test/*.cpp)
    foreach(test ${TEST_SRC})
        get_filename_component(testname ${test} NAME_WE)

        set(command $<TARGET_FILE:${testname}>)

        add_test(NAME ${testname}
            COMMAND ${command}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test")
    endforeach(test ${TEST_SRC})
endif()
