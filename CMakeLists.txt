cmake_minimum_required(VERSION 3.13)

# Read version from VERSION file
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SHM_VERSION_STRING)
string(STRIP "${SHM_VERSION_STRING}" SHM_VERSION_STRING)

project(shm VERSION ${SHM_VERSION_STRING} LANGUAGES CXX)

# --- Options ---
option(SHM_BUILD_TESTS "Build unit tests" OFF)
option(SHM_BUILD_BENCHMARKS "Build benchmarks" OFF)

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

# Dependencies
find_package(Threads REQUIRED)
target_link_libraries(shm INTERFACE Threads::Threads)

if(UNIX)
    # On Linux, shm_open requires -lrt
    target_link_libraries(shm INTERFACE rt)
elseif(WIN32)
    # On Windows, link ws2_32 (Winsock) just in case, ensuring compatibility
    target_link_libraries(shm INTERFACE ws2_32)
endif()

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

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/shmConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
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
