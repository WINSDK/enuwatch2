cmake_minimum_required(VERSION 3.17)
project(enu LANGUAGES CXX)

# Disable inlining
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -fno-inline -fno-inline-functions")

add_executable(enu)
target_compile_features(enu PRIVATE cxx_std_23)

target_link_libraries(enu PRIVATE TBB::tbb)

target_sources(enu PRIVATE main.cpp log.cpp argparser.cpp spawn.cpp)

if(APPLE)
    target_sources(enu PRIVATE fsevent.cpp)
    target_link_libraries(enu PRIVATE "-framework CoreServices")
    
    # Weirdness with clang not linking when using std::println
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++")
elseif(LINUX)
    target_sources(enu PRIVATE inotify.cpp)
else()
    message(FATAL_ERROR "Unsupported OS: only Linux and macOS are supported.")
endif()

# Clang requires additional flags for jthread
if (CLANG)
    target_compile_options(enu PRIVATE -fexperimental-library)
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
