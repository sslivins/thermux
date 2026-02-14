/**
 * @file main.c
 * @brief Thermux - Multi-Sensor Temperature Monitoring System
 * 
 * This application reads multiple DS18B20 1-Wire temperature sensors,
 * publishes readings to Home Assistant via MQTT, and provides a web
 * interface for sensor management.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mdns.h"

#include "nvs_storage.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "onewire_temp.h"
#include "sensor_manager.h"
#include "mqtt_client_ha.h"
#include "web_server.h"
#include "ota_updater.h"
#include "log_buffer.h"

static const char *TAG = "main";

/* Event group for network connectivity */
EventGroupHandle_t network_event_group;
const int NETWORK_CONNECTED_BIT = BIT0;

/* Application version - update for each release */
const char *APP_VERSION = "2.5.0";

/* Runtime sensor settings (can be changed via web UI) */
static uint32_t s_read_interval_ms = CONFIG_SENSOR_READ_INTERVAL_MS;
static uint32_t s_publish_interval_ms = CONFIG_SENSOR_PUBLISH_INTERVAL_MS;

/* Accessor functions for sensor settings */
uint32_t get_sensor_read_interval(void) { return s_read_interval_ms; }
uint32_t get_sensor_publish_interval(void) { return s_publish_interval_ms; }

void set_sensor_read_interval(uint32_t ms) { 
    s_read_interval_ms = ms; 
    ESP_LOGD(TAG, "Read interval set to %lu ms", ms);
}

void set_sensor_publish_interval(uint32_t ms) { 
    s_publish_interval_ms = ms; 
    ESP_LOGD(TAG, "Publish interval set to %lu ms", ms);
}

/**
 * @brief Initialize mDNS service for device discovery
 * 
 * Uses simple hostname with automatic collision handling (thermux.local, 
 * thermux-2.local, etc.). Registers discoverable services for network scanning.
 */
static esp_err_t init_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Use simple hostname - mDNS handles collisions automatically */
    const char *hostname = "thermux";
    
    mdns_hostname_set(hostname);
    mdns_instance_name_set("Thermux Temperature Monitor");
    
    /* TXT records for service discovery */
    mdns_txt_item_t http_txt[] = {
        {"version", APP_VERSION},
        {"type", "temperature"},
    };
    
    /* Add HTTP service for web interface discovery */
    mdns_service_add("Thermux", "_http", "_tcp", CONFIG_WEB_SERVER_PORT, http_txt, 2);
    
    /* Add custom service type for easy discovery of all Thermux devices */
    mdns_service_add("Thermux", "_thermux", "_tcp", CONFIG_WEB_SERVER_PORT, http_txt, 2);
    
    ESP_LOGD(TAG, "mDNS hostname: %s.local", hostname);
    ESP_LOGD(TAG, "mDNS services: _http._tcp, _thermux._tcp");
    return ESP_OK;
}

/**
 * @brief Network event handler
 */
static void network_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(network_event_group, NETWORK_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(network_event_group, NETWORK_CONNECTED_BIT);
    }
}

/**
 * @brief Temperature reading task
 */
static void temperature_task(void *pvParameters)
{
    ESP_LOGD(TAG, "Temperature task started");
    
    while (1) {
        /* Read all connected sensors */
        sensor_manager_read_all();
        
        vTaskDelay(pdMS_TO_TICKS(s_read_interval_ms));
    }
}

/**
 * @brief MQTT publishing task
 */
static void mqtt_publish_task(void *pvParameters)
{
    ESP_LOGD(TAG, "MQTT publish task started");
    
    /* Wait for MQTT to connect */
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    while (1) {
        if (mqtt_ha_is_connected()) {
            sensor_manager_publish_all();
        }
        
        vTaskDelay(pdMS_TO_TICKS(s_publish_interval_ms));
    }
}

/**
 * @brief OTA check task
 */
static void ota_check_task(void *pvParameters)
{
    ESP_LOGD(TAG, "OTA check task started");
    
    /* Initial delay before first check */
    vTaskDelay(pdMS_TO_TICKS(60000));
    
    while (1) {
#if CONFIG_OTA_ENABLED
        ota_check_for_update();
#endif
        /* Check at configured interval */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_OTA_CHECK_INTERVAL_HOURS * 3600 * 1000));
    }
}

/**
 * @brief Watchdog task to monitor system health
 */
static void watchdog_task(void *pvParameters)
{
    while (1) {
        /* Log heap status periodically (debug level - not shown by default) */
        ESP_LOGD(TAG, "Free heap: %lu bytes, minimum: %lu bytes",
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size());
        
        vTaskDelay(pdMS_TO_TICKS(60000)); /* Every minute */
    }
}

void app_main(void)
{
    /* Initialize log buffer first to capture all logs */
    log_buffer_init(LOG_BUFFER_SIZE);
    
    /* Set default runtime log level to INFO (compile-time is DEBUG to allow switching) */
    esp_log_level_set("*", ESP_LOG_INFO);
    
    /* Quiet down noisy ESP-IDF components - set to WARN level
       This reduces startup spam while keeping important messages */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_WARN);
    esp_log_level_set("esp_https_ota", ESP_LOG_WARN);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    /* HTTP server internals - extremely verbose, rarely useful */
    esp_log_level_set("httpd_parse", ESP_LOG_WARN);
    esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
    esp_log_level_set("httpd_uri", ESP_LOG_WARN);
    esp_log_level_set("httpd_sess", ESP_LOG_WARN);
    esp_log_level_set("httpd", ESP_LOG_WARN);
    /* Ethernet/network spam */
    esp_log_level_set("esp.emac", ESP_LOG_WARN);
    esp_log_level_set("event", ESP_LOG_WARN);
    
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "Thermux - Temperature Monitor");
    ESP_LOGI(TAG, "Version: %s", APP_VERSION);
    ESP_LOGI(TAG, "=================================");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /* Create network event group */
    network_event_group = xEventGroupCreate();
    
    /* Register IP event handler */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, 
                                                &network_event_handler, NULL));

    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize NVS storage for our app data */
    ESP_ERROR_CHECK(nvs_storage_init());

    /* Load sensor settings from NVS (or use defaults) */
    {
        uint32_t read_ms, publish_ms;
        uint8_t resolution;
        if (nvs_storage_load_sensor_settings(&read_ms, &publish_ms, &resolution) == ESP_OK) {
            s_read_interval_ms = read_ms;
            s_publish_interval_ms = publish_ms;
            ESP_LOGD(TAG, "Loaded sensor settings: read=%lums, publish=%lums, resolution=%d",
                     read_ms, publish_ms, resolution);
            /* Resolution will be applied after onewire_temp_init */
        } else {
            ESP_LOGD(TAG, "Using default sensor settings");
        }
    }

    /* Initialize 1-Wire bus and discover sensors */
    ESP_ERROR_CHECK(onewire_temp_init(CONFIG_ONEWIRE_GPIO));

    /* Apply saved resolution setting */
    {
        uint32_t read_ms, publish_ms;
        uint8_t resolution;
        if (nvs_storage_load_sensor_settings(&read_ms, &publish_ms, &resolution) == ESP_OK) {
            if (resolution >= 9 && resolution <= 12) {
                onewire_temp_set_resolution(resolution);
            }
        }
    }
    
    /* Initialize sensor manager */
    ESP_ERROR_CHECK(sensor_manager_init());

#if CONFIG_USE_ETHERNET
    /* Initialize Ethernet (primary connection for POE) */
    ESP_ERROR_CHECK(ethernet_manager_init());
    ethernet_manager_start();
#endif

#if CONFIG_USE_WIFI_FALLBACK
    /* Initialize WiFi as fallback */
    ESP_ERROR_CHECK(wifi_manager_init());
    
    #if !CONFIG_USE_ETHERNET
    /* If Ethernet is disabled, start WiFi immediately */
    wifi_manager_start();
    #endif
#endif

    /* Wait for network connection */
    ESP_LOGD(TAG, "Waiting for network connection...");
    xEventGroupWaitBits(network_event_group, NETWORK_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Network connected!");

    /* Initialize mDNS */
    init_mdns();

    /* Initialize MQTT client */
    ESP_ERROR_CHECK(mqtt_ha_init());

    /* Start web server */
    ESP_ERROR_CHECK(web_server_start());
    ESP_LOGD(TAG, "Web server started on port %d", CONFIG_WEB_SERVER_PORT);

#if CONFIG_OTA_ENABLED
    /* Initialize OTA updater */
    ESP_ERROR_CHECK(ota_updater_init());
#endif

    /* Create application tasks */
    xTaskCreate(temperature_task, "temp_task", 4096, NULL, 5, NULL);
    xTaskCreate(mqtt_publish_task, "mqtt_pub_task", 4096, NULL, 4, NULL);
    xTaskCreate(watchdog_task, "watchdog_task", 2048, NULL, 1, NULL);
    
#if CONFIG_OTA_ENABLED
    xTaskCreate(ota_check_task, "ota_task", 8192, NULL, 2, NULL);
#endif

    ESP_LOGI(TAG, "Application started successfully!");
}
