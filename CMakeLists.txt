cmake_minimum_required(VERSION 3.13)

# Read version from VERSION file
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SHM_VERSION_STRING)
string(STRIP "${SHM_VERSION_STRING}" SHM_VERSION_STRING)

project(shm VERSION ${SHM_VERSION_STRING} LANGUAGES CXX)

# --- Options ---
option(SHM_BUILD_TESTS "Build unit tests" OFF)
option(SHM_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(SHM_DEBUG "Enable debug logging" OFF)

# --- Main Interface Library ---
add_library(shm INTERFACE)

# Define C++17 standard requirement for consumers
target_compile_features(shm INTERFACE cxx_std_17)

# Include directories
# BUILD_INTERFACE: Used when building within this project
# INSTALL_INTERFACE: Used when installed and found via find_package
target_include_directories(shm INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(SHM_DEBUG)
    target_compile_definitions(shm INTERFACE SHM_DEBUG)
endif()

if(MSVC)
    # Ensure consumers don't get min/max macros from windows.h
    target_compile_definitions(shm INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
    # Set warning level 4 for this project's build
    add_compile_options(/W4 /permissive-)
endif()

# Dependencies
find_package(Threads REQUIRED)
target_link_libraries(shm INTERFACE Threads::Threads)

# Windows-only: link ws2_32 (Winsock) for compatibility.
target_link_libraries(shm INTERFACE ws2_32)

# --- Installation ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install headers
install(DIRECTORY include/shm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install library target
install(TARGETS shm
    EXPORT shmTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Generate and install cmake config files
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shmConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/shmConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shm
)

# SameMinorVersion, not SameMajorVersion: on the 0.x line CMake reads major = 0,
# so SameMajorVersion made every 0.y release satisfy every other one. That is
# exactly wrong here — AGENTS.md §Versioning states that on 0.x the MINOR slot
# plays the major role (0.8.x -> 0.9.0 carried the SHM_VERSION 0x00070000 ->
# 0x00080000 wire break), and SPEC §2.1 requires mixed-version traffic be
# impossible by construction rather than by convention. Under SameMajorVersion a
# consumer that pinned `find_package(shm 0.8.21 REQUIRED)` to the 0x00070000 wire
# was silently handed 0.9.0's headers, and the mismatch only surfaced as a total
# attach failure at runtime.
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/shmConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/shmConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/shmConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shm
)

install(EXPORT shmTargets
    FILE shmTargets.cmake
    NAMESPACE shm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shm
)

# --- Subdirectories ---
if(SHM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SHM_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
