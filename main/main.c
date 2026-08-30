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
#include "esp_ota_ops.h"
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
#include "log_persist.h"
#include "time_sync.h"

static const char *TAG = "main";

/* Event group for network connectivity */
EventGroupHandle_t network_event_group;
const int NETWORK_CONNECTED_BIT = BIT0;

/* Application version - update for each release */
const char *APP_VERSION = "3.3.9-beta";

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
    const char *hostname = CONFIG_MDNS_HOSTNAME;
    
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
 * @brief Confirm the running firmware healthy, or roll back after a bad OTA
 *
 * With bootloader app-rollback enabled, a freshly flashed image boots in the
 * PENDING_VERIFY state. We confirm it (so the bootloader keeps it) once the
 * device proves healthy - here, the network came up. If the health check
 * failed we invalidate the image and reboot into the previous known-good slot.
 * On a normal boot (image already marked valid) this is a no-op, so it is safe
 * to call unconditionally.
 *
 * @param healthy true if the post-boot health check passed (network is up)
 */
static void ota_confirm_or_rollback(bool healthy)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; /* already valid (or factory) - nothing to verify */
    }

    if (healthy) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "New firmware verified healthy; rollback cancelled");
        } else {
            ESP_LOGE(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "New firmware failed health check; rolling back to previous slot");
        esp_ota_mark_app_invalid_rollback_and_reboot(); /* reboots; does not return */
        ESP_LOGE(TAG, "Rollback not possible (no previous slot); staying on current image");
    }
#else
    (void)healthy;
#endif
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
        /* Check at configured interval. pdMS_TO_TICKS() multiplies the
         * millisecond value by configTICK_RATE_HZ before dividing by 1000,
         * so passing a full 24h-in-ms value (86,400,000) overflows 32 bits
         * and wraps to a ~8-minute delay instead - which was silently
         * causing the OTA check (and its "update available" MQTT publish)
         * to re-run every ~8 minutes rather than once a day. Delay in
         * seconds and use pdSECOND_TO_TICKS-equivalent math to stay well
         * clear of the overflow. */
        uint32_t interval_s = (uint32_t)CONFIG_OTA_CHECK_INTERVAL_HOURS * 3600;
        vTaskDelay(pdMS_TO_TICKS(1000) * interval_s);
    }
}

#if CONFIG_OTA_ENABLED
/**
 * @brief Mirror OTA state to the Home Assistant "update" entity
 *
 * Publishes installed/latest version and live download progress to MQTT
 * whenever it changes, so HA's update card shows availability and a progress
 * bar. Polls fast while an install is running, slow otherwise.
 */
static void ota_status_publish_task(void *pvParameters)
{
    char last_latest[32] = {0};
    int last_pct = -2;            /* sentinel: force the first publish */
    bool last_connected = false;

    while (1) {
        bool connected = mqtt_ha_is_connected();

        if (connected) {
            char latest[32] = {0};
            if (ota_is_update_available()) {
                ota_get_latest_version(latest, sizeof(latest));
            } else {
                strncpy(latest, APP_VERSION, sizeof(latest) - 1);
            }

            int st = ota_get_update_state();   /* 0 idle, 1 downloading, 2 complete, -1 failed */
            int pct = (st == 1) ? ota_get_download_progress() : -1;

            bool reconnected = connected && !last_connected;
            if (reconnected || pct != last_pct ||
                strncmp(latest, last_latest, sizeof(latest)) != 0) {

                const char *release_url = NULL;
                char url_buf[128];
                if (ota_is_update_available()) {
                    snprintf(url_buf, sizeof(url_buf),
                             "https://github.com/%s/%s/releases/latest",
                             CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
                    release_url = url_buf;
                }

                mqtt_ha_publish_update_state(APP_VERSION, latest, release_url, pct);

                strncpy(last_latest, latest, sizeof(last_latest) - 1);
                last_latest[sizeof(last_latest) - 1] = '\0';
                last_pct = pct;
            }
        }
        last_connected = connected;

        vTaskDelay(pdMS_TO_TICKS(ota_update_in_progress() ? 1000 : 5000));
    }
}
#endif

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

    /* Recover log history from the previous boot (persisted to flash) and
     * seed it into the fresh RAM ring buffer, then start persisting again */
    {
        static char recovered[LOG_BUFFER_SIZE];
        size_t recovered_len = 0;
        if (log_persist_init(recovered, sizeof(recovered), &recovered_len) == ESP_OK &&
            recovered_len > 0) {
            static const char header[] = "\n===== Recovered log from previous boot =====\n";
            static const char footer[] = "\n===== End of previous boot log =====\n\n";
            log_buffer_seed(header, sizeof(header) - 1);
            log_buffer_seed(recovered, recovered_len);
            log_buffer_seed(footer, sizeof(footer) - 1);
        }
        esp_register_shutdown_handler(log_persist_flush);
        log_persist_start_periodic_flush(300); /* every 5 minutes */
    }

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

    /* Wait for network connection. Bounded so a freshly-OTA'd image that
       breaks networking triggers a rollback instead of hanging forever in
       the PENDING_VERIFY state (where it would never be marked good). */
    ESP_LOGD(TAG, "Waiting for network connection...");
    EventBits_t net_bits = xEventGroupWaitBits(
        network_event_group, NETWORK_CONNECTED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(CONFIG_OTA_HEALTHCHECK_TIMEOUT_S * 1000));

    if (net_bits & NETWORK_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Network connected!");
        /* Health check passed: keep this firmware. */
        ota_confirm_or_rollback(true);
    } else {
        ESP_LOGE(TAG, "No network after %d s", CONFIG_OTA_HEALTHCHECK_TIMEOUT_S);
        /* If this is a pending OTA image, a missing network means the update
           is bad: roll back to the previous slot (does not return). On a
           normal boot this is a no-op and we keep waiting below. */
        ota_confirm_or_rollback(false);
        ESP_LOGW(TAG, "Continuing to wait for network...");
        xEventGroupWaitBits(network_event_group, NETWORK_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Network connected!");
    }

    /* Start SNTP so we can report a wall-clock "Last Boot" time to HA */
    time_sync_init();

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
    xTaskCreate(ota_status_publish_task, "ota_status_task", 4096, NULL, 2, NULL);
#endif

    ESP_LOGI(TAG, "Application started successfully!");
}
