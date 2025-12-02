cmake_minimum_required(VERSION 3.10)
project(shm_benchmark)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_PROFILING "Enable gprof profiling" OFF)
if(ENABLE_PROFILING)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pg")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -pg")
endif()

# Include directories
# We need to reach the root src/include from benchmarks/build
# benchmarks/main.cpp includes "../../src/IPC.h" which is relative to source file
# But we should add include paths properly.

include_directories(${CMAKE_SOURCE_DIR}/../include) # shm/include
include_directories(${CMAKE_SOURCE_DIR}/../src)     # shm/src
include_directories(${CMAKE_SOURCE_DIR}/include)    # benchmarks/include (generated headers)

# Flatbuffers usually expects <flatbuffers/flatbuffers.h>
# So we include the parent of 'flatbuffers' directory.
include_directories(${CMAKE_SOURCE_DIR}/external)

# Add source files
set(SOURCES
    main.cpp
    ${CMAKE_SOURCE_DIR}/../src/IPCHost.cpp
)

# Executable
add_executable(shm_benchmark ${SOURCES})

# Libraries
if(WIN32)
    target_link_libraries(shm_benchmark PRIVATE ws2_32)
else()
    target_link_libraries(shm_benchmark PRIVATE pthread rt)
endif()
