cmake_minimum_required(VERSION 3.17)
project(enu LANGUAGES CXX)

add_executable(enu)
target_compile_features(enu PRIVATE cxx_std_23)

target_sources(enu PRIVATE main.cpp log.cpp argparser.cpp)

if(APPLE)
    target_sources(enu PRIVATE fsevent.cpp)
    target_link_libraries(enu PRIVATE "-framework CoreServices")
elseif(LINUX)
else()
    message(FATAL_ERROR "Unsupported OS: only Linux and macOS are supported.")
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(enu PRIVATE -fsanitize=address -fsanitize=undefined)
  target_link_options(enu PRIVATE -fsanitize=address -fsanitize=undefined)
endif()

target_compile_options(enu PRIVATE
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
