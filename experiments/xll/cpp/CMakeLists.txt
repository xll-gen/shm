cmake_minimum_required(VERSION 3.10)
project(MyXLL_ZeroCopy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include directories
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/external/EXCELSDK/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/external" # For Flatbuffers headers
    "${CMAKE_CURRENT_SOURCE_DIR}/generated" # For generated Flatbuffers C++ headers
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include" # For SHM library headers (Platform.h, IPCUtils.h)
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include/shm" # For SHM library headers (DirectHost.h)
    "${CMAKE_CURRENT_SOURCE_DIR}" # For MyAddin.h, logger.h etc.
)

# Source files
set(SOURCES
    MyAddin.cpp
    PascalString.cpp
    logger.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/external/EXCELSDK/XLCALL.CPP"
)

# Define the XLL (DLL) target
add_library(MyXLL_ZeroCopy SHARED ${SOURCES})

# Set the output file name and extension to .xll
set_target_properties(MyXLL_ZeroCopy PROPERTIES
    PREFIX ""
    SUFFIX ".xll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build"
)

# Specify the .def file for exporting functions
set_property(TARGET MyXLL_ZeroCopy PROPERTY LINK_DEF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/MyAddin.def")

# Link against required libraries
target_link_libraries(MyXLL_ZeroCopy PUBLIC ws2_32)
