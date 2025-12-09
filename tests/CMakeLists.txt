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

# Build guest_call_test but do not add it to CTest (requires external Go process)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/guest_call_test.cpp")
    add_executable(guest_call_test guest_call_test.cpp)
    target_link_libraries(guest_call_test PRIVATE shm)
endif()
