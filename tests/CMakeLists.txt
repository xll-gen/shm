cmake_minimum_required(VERSION 3.10)
project(DirectShutdownTest)

set(CMAKE_CXX_STANDARD 17)

include_directories(../include ../src)

add_executable(DirectShutdownTest DirectShutdownTest.cpp)
target_link_libraries(DirectShutdownTest pthread rt)
