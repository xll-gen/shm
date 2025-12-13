cmake_minimum_required(VERSION 3.20)
project(pingpong_coro)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pingpong_coro main.cpp)
target_link_libraries(pingpong_coro pthread rt)
