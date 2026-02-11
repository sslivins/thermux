# Gzip HTML files at build time
set(HTML_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/html")
set(HTML_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/html_gz")

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

idf_component_register(
    SRCS 
        "main.c"
        "wifi_manager.c"
        "ethernet_manager.c"
        "onewire_temp.c"
        "mqtt_client_ha.c"
        "web_server.c"
        "ota_updater.c"
        "nvs_storage.c"
        "sensor_manager.c"
        "log_buffer.c"
        "version_utils.c"
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
    EMBED_TXTFILES
        "certs/github_root_ca.pem"
    EMBED_FILES
        "${HTML_BUILD_DIR}/index.html.gz"
        "${HTML_BUILD_DIR}/config.html.gz"
)
