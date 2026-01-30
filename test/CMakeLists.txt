cmake_minimum_required(VERSION 3.16)
project(temp_monitor_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Enable testing
enable_testing()

# Add Unity test framework
add_subdirectory(unity)

# Test executable
add_executable(test_runner
    test_runner.c
    test_version_compare.c
    test_address_utils.c
    # Modules under test
    ../main/version_utils.c
)

target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/unity/src
)

target_link_libraries(test_runner unity)

# Register test with CTest
add_test(NAME unit_tests COMMAND test_runner)
