# Find all test source files
file(GLOB TEST_SOURCES "test_*.cpp")

# Create an executable and test for each source file
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE shm)

    # Add to CTest
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# --- test_stream_integration: needs the Go toolchain -------------------------
# It shells out to `go build` for tests/go_integration and then runs the result.
# Two things used to make it a permanent FAILURE rather than a real signal:
#   1. it used paths relative to the SOURCE tree, but ctest runs it from the
#      BUILD tree, so the `cd` failed before Go was ever invoked;
#   2. `go` is resolved from the environment, which ctest does not necessarily
#      carry, and a missing toolchain is not a defect in this repository.
# So: hand it absolute paths at configure time, and make "no Go toolchain" a
# SKIP. A hard FAIL here is worse than useless -- it is indistinguishable from
# the protocol regression this test exists to catch.
if(TARGET test_stream_integration)
    find_program(SHM_GO_EXECUTABLE NAMES go)
    if(SHM_GO_EXECUTABLE)
        message(STATUS "test_stream_integration: using Go at ${SHM_GO_EXECUTABLE}")
    else()
        message(STATUS "test_stream_integration: no Go toolchain found - the test will report SKIP")
    endif()
    target_compile_definitions(test_stream_integration PRIVATE
        SHM_GO_EXECUTABLE="${SHM_GO_EXECUTABLE}"
        SHM_GO_INTEGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/go_integration"
        # Build output goes in the BUILD tree; the source tree stays clean.
        SHM_GO_INTEGRATION_BIN="${CMAKE_CURRENT_BINARY_DIR}/go_integration_test.exe")
    # 77 is the GNU/autotools convention for "skipped", and what CTest's
    # SKIP_RETURN_CODE expects.
    set_tests_properties(test_stream_integration PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Build guest_call_test but do not add it to CTest (requires external Go process)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/guest_call_test.cpp")
    add_executable(guest_call_test guest_call_test.cpp)
    target_link_libraries(guest_call_test PRIVATE shm)
endif()
