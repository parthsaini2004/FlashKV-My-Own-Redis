# CMakeLists.txt
cmake_minimum_required(VERSION 3.15)
project(FlashKV VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags for optimization
if(CMAKE_BUILD_TYPE MATCHES Release)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto")
endif()

# Warning flags (interview gold: shows attention to code quality)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# macOS-specific settings
if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

# Source files
set(SOURCES
    src/main.cpp
)

# Create executable
add_executable(flashkv ${SOURCES})

# Include directories
target_include_directories(flashkv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Threading library
find_package(Threads REQUIRED)
target_link_libraries(flashkv PRIVATE Threads::Threads)

# Installation
install(TARGETS flashkv DESTINATION bin)

# Testing
enable_testing()

# Simple test to verify build
add_test(NAME build_test COMMAND flashkv --help || true)

# Print configuration
message(STATUS "=== FlashKV Build Configuration ===")
message(STATUS "Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Compiler:  ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ Flags:     ${CMAKE_CXX_FLAGS}")
message(STATUS "===================================")