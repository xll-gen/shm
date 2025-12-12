# shm_benchmark

# Note: This file is intended to be included from the root CMakeLists.txt

option(ENABLE_PROFILING "Enable gprof profiling" OFF)

set(SOURCES main.cpp)

add_executable(shm_benchmark ${SOURCES})

# Optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(shm_benchmark PRIVATE -O3 -march=native -flto)
        target_link_options(shm_benchmark PRIVATE -flto)
    elseif(MSVC)
        target_compile_options(shm_benchmark PRIVATE /O2 /Oi /Ot /GL /permissive-)
        target_link_options(shm_benchmark PRIVATE /LTCG)
    endif()
endif()

if(ENABLE_PROFILING)
    target_compile_options(shm_benchmark PRIVATE -pg)
    target_link_options(shm_benchmark PRIVATE -pg)
endif()

# Include local headers (generated flatbuffers) and external
target_include_directories(shm_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external
)

# Link against the main library
target_link_libraries(shm_benchmark PRIVATE shm)

if(MSVC)
    target_compile_definitions(shm_benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
