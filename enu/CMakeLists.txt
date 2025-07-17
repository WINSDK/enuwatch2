cmake_minimum_required(VERSION 3.17)
project(enu LANGUAGES CXX)

# Disable inlining
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -fno-inline -fno-inline-functions")

# Clang requires additional flags for jthread
if (CLANG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexperimental-library")
endif()

# Create a library for the core functionality (excluding main.cpp)
add_library(enuwatch STATIC log.cpp argparser.cpp spawn.cpp)
target_compile_features(enuwatch PUBLIC cxx_std_23)
target_link_libraries(enuwatch PUBLIC TBB::tbb)
target_include_directories(enuwatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Create the main executable
add_executable(enu main.cpp)
target_compile_features(enu PRIVATE cxx_std_23)
target_link_libraries(enu PRIVATE enuwatch)

if(APPLE)
    target_sources(enuwatch PRIVATE fsevent.cpp)
    target_link_libraries(enuwatch PUBLIC "-framework CoreServices")
elseif(LINUX)
    target_sources(enuwatch PRIVATE inotify.cpp)
else()
    message(FATAL_ERROR "Unsupported OS: only Linux and macOS are supported.")
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(enuwatch PUBLIC -fsanitize=address -fsanitize=undefined)
  target_link_options(enuwatch PUBLIC -fsanitize=address -fsanitize=undefined)
endif()

target_compile_options(enuwatch PUBLIC
    -fno-exceptions
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -ffunction-sections
    -fdata-sections
    -fno-rtti # Only ok if you don't use dynamic_cast and typeid
    -Wall
    -Wextra
    -Wno-sign-compare
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -g3)
