cmake_minimum_required(VERSION 3.12)
project(pingpong_aeron)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Aeron REQUIRED)
find_package(Threads REQUIRED)

add_executable(pingpong_aeron main.cpp)
target_link_libraries(pingpong_aeron PRIVATE Aeron::Aeron Threads::Threads)
