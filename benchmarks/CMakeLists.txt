cmake_minimum_required(VERSION 3.10)
project(shm_benchmark)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Enable aggressive optimizations for Release builds
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto")
    # Link Time Optimization requires linker support
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
endif()

option(ENABLE_PROFILING "Enable gprof profiling" OFF)
if(ENABLE_PROFILING)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pg")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -pg")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/../include) # shm/include
include_directories(${CMAKE_SOURCE_DIR}/include)    # benchmarks/include (generated headers)
include_directories(${CMAKE_SOURCE_DIR}/external)

# Add source files
# IPCHost.cpp is removed, now header-only (templated)
set(SOURCES
    main.cpp
)

# Executable
add_executable(shm_benchmark ${SOURCES})
add_executable(guest_call_test ../tests/guest_call_test.cpp)

# Libraries
if(WIN32)
    target_link_libraries(shm_benchmark PRIVATE ws2_32)
    target_link_libraries(guest_call_test PRIVATE ws2_32)
else()
    target_link_libraries(shm_benchmark PRIVATE pthread rt)
    target_link_libraries(guest_call_test PRIVATE pthread rt)
endif()
