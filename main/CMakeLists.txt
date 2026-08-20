# Gzip HTML files at build time
set(HTML_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/html")
set(HTML_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/html_gz")

# Only run gzip during actual build (not during requirements discovery phase)
if(EXISTS ${HTML_SRC_DIR})
    # Create gzipped output directory
    file(MAKE_DIRECTORY ${HTML_BUILD_DIR})

    # Find Python
    find_package(Python3 REQUIRED)

    # Run HTML gzip compression during configuration
    # Note: If you edit HTML files, run: python scripts/gzip_html.py main/html main/html_gz
    # Or do a full clean build: idf.py fullclean && idf.py build
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/gzip_html.py" 
                "${HTML_SRC_DIR}" "${HTML_BUILD_DIR}"
        RESULT_VARIABLE GZIP_RESULT
        OUTPUT_VARIABLE GZIP_OUTPUT
        ERROR_VARIABLE GZIP_ERROR
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(NOT GZIP_RESULT EQUAL 0)
        message(WARNING "HTML gzip compression failed: ${GZIP_ERROR}")
        message(FATAL_ERROR "Cannot build without gzipped HTML files")
    else()
        message(STATUS "HTML gzip compression:")
        message(STATUS "${GZIP_OUTPUT}")
    endif()
endif()

# EXPERIMENTAL (issue #48 / experiment/partition-migration-test): compile the
# Stage A / Stage B partition table CSVs into .bin blobs so the migration
# module can embed and flash them. Not used unless partition_migration.c is
# invoked, which currently only happens on the experimental branch's manual
# debug endpoint - never shipped to the production OTA channel.
set(MIGRATION_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/migration_tables")
file(MAKE_DIRECTORY ${MIGRATION_BUILD_DIR})
set(GEN_ESP32PART "$ENV{IDF_PATH}/components/partition_table/gen_esp32part.py")

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${GEN_ESP32PART}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../partitions_stageA.csv"
            "${MIGRATION_BUILD_DIR}/partitions_stageA.bin"
    RESULT_VARIABLE STAGEA_RESULT
    OUTPUT_VARIABLE STAGEA_OUTPUT
    ERROR_VARIABLE STAGEA_ERROR
)
if(NOT STAGEA_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate Stage A partition table: ${STAGEA_ERROR}")
endif()

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${GEN_ESP32PART}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../partitions_stageB.csv"
            "${MIGRATION_BUILD_DIR}/partitions_stageB.bin"
    RESULT_VARIABLE STAGEB_RESULT
    OUTPUT_VARIABLE STAGEB_OUTPUT
    ERROR_VARIABLE STAGEB_ERROR
)
if(NOT STAGEB_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate Stage B partition table: ${STAGEB_ERROR}")
endif()

idf_component_register(
    SRCS 
        "main.c"
        "wifi_manager.c"
        "ethernet_manager.c"
        "bus_stats.c"
        "onewire_temp.c"
        "mqtt_client_ha.c"
        "web_server.c"
        "ota_updater.c"
        "nvs_storage.c"
        "sensor_manager.c"
        "log_buffer.c"
        "version_utils.c"
        "time_sync.c"
        "partition_migration.c"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash
        esp_wifi
        esp_eth
        esp_http_server
        esp_https_ota
        app_update
        mqtt
        json
        mdns
        esp_netif
        esp_event
        driver
        spi_flash
    EMBED_FILES
        "${HTML_BUILD_DIR}/index.html.gz"
        "${HTML_BUILD_DIR}/config.html.gz"
        "${MIGRATION_BUILD_DIR}/partitions_stageA.bin"
        "${MIGRATION_BUILD_DIR}/partitions_stageB.bin"
)
