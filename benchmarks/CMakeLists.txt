cmake_minimum_required(VERSION 3.10)
project(shm_benchmark)

set(CMAKE_CXX_STANDARD 17)

# Include directories
include_directories(../include)
include_directories(../src)
include_directories(../../phase3/xll/external)

# Add source files
set(SOURCES
    main.cpp
    ../src/IPC.cpp
)

# Executable
add_executable(shm_benchmark ${SOURCES})

# Libraries (Windows)
target_link_libraries(shm_benchmark PRIVATE ws2_32)
