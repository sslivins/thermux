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
    test_mqtt_utils.c
    test_config_utils.c
    test_nvs_utils.c
    test_bus_stats.c
    # Modules under test (test-only utilities are local, version_utils is shared)
    ../main/version_utils.c
    ../main/bus_stats.c
    mqtt_utils.c
    config_utils.c
    nvs_utils.c
)

target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/unity/src
)

target_link_libraries(test_runner unity)

# Register test with CTest
add_test(NAME unit_tests COMMAND test_runner)
