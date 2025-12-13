cmake_minimum_required(VERSION 3.12)
project(pingpong_aeron)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point to the local Aeron build
set(AERON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmarks/external/aeron")
set(AERON_BUILD "${AERON_ROOT}/cppbuild")

# Include paths
include_directories(${AERON_ROOT}/aeron-client/src/main/cpp)
include_directories(${AERON_ROOT}/aeron-client/src/main/c)

# Link paths
link_directories(${AERON_BUILD}/lib)

find_package(Threads REQUIRED)

add_executable(pingpong_aeron main.cpp)

# Link against aeron_client (static or shared, static preferred for simplicity here)
# Note: 'aeron_client' is the C++ client lib name from Aeron's build
target_link_libraries(pingpong_aeron PRIVATE aeron_client aeron_driver_static Threads::Threads)
