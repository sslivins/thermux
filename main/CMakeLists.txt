# Minify HTML files at build time
set(HTML_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/html")
set(HTML_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/html_min")

# Create minified output directory
file(MAKE_DIRECTORY ${HTML_BUILD_DIR})

# Find Python
find_package(Python3 REQUIRED)

# Run HTML minification during configuration
# Note: If you edit HTML files, run: python scripts/html_minifier.py main/html main/html_min
# Or do a full clean build: idf.py fullclean && idf.py build
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/html_minifier.py" 
            "${HTML_SRC_DIR}" "${HTML_BUILD_DIR}"
    RESULT_VARIABLE MINIFY_RESULT
    OUTPUT_VARIABLE MINIFY_OUTPUT
    ERROR_VARIABLE MINIFY_ERROR
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(NOT MINIFY_RESULT EQUAL 0)
    message(WARNING "HTML minification failed: ${MINIFY_ERROR}")
    # Fall back to using unminified files directly
    set(HTML_BUILD_DIR ${HTML_SRC_DIR})
else()
    message(STATUS "HTML minification:")
    message(STATUS "${MINIFY_OUTPUT}")
endif()

idf_component_register(
    SRCS 
        "main.c"
        "wifi_manager.c"
        "ethernet_manager.c"
        "onewire_temp.c"
        "mqtt_client_ha.c"
        "mqtt_utils.c"
        "config_utils.c"
        "nvs_utils.c"
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
        "${HTML_BUILD_DIR}/index.html"
        "${HTML_BUILD_DIR}/config.html"
)
