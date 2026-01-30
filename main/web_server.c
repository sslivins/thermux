/**
 * @file web_server.c
 * @brief HTTP web server with REST API and embedded web portal
 */

#include "web_server.h"
#include "sensor_manager.h"
#include "ota_updater.h"
#include "nvs_storage.h"
#include "onewire_temp.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

extern const char *APP_VERSION;

/* Forward declarations for reconfiguration */
extern esp_err_t mqtt_ha_stop(void);
extern esp_err_t mqtt_ha_init(void);
extern esp_err_t mqtt_ha_start(void);

/* Embedded HTML files (minified at build time) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t config_html_start[] asm("_binary_config_html_start");
extern const uint8_t config_html_end[] asm("_binary_config_html_end");
extern const uint8_t ota_html_start[] asm("_binary_ota_html_start");
extern const uint8_t ota_html_end[] asm("_binary_ota_html_end");

/**
 * @brief Handler for GET /
 */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

/* Note: HTML content moved to external files in main/html/ directory */

/**
 * @brief Handler for GET /api/status
 */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", APP_VERSION);
    cJSON_AddNumberToObject(root, "sensor_count", sensor_manager_get_count());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_log_timestamp() / 1000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    
    extern bool mqtt_ha_is_connected(void);
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_ha_is_connected());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/sensors
 */
static esp_err_t api_sensors_get_handler(httpd_req_t *req)
{
    int count;
    const managed_sensor_t *sensors = sensor_manager_get_sensors(&count);

    cJSON *root = cJSON_CreateArray();
    
    for (int i = 0; i < count; i++) {
        cJSON *sensor = cJSON_CreateObject();
        cJSON_AddStringToObject(sensor, "address", sensors[i].address_str);
        cJSON_AddNumberToObject(sensor, "temperature", sensors[i].hw_sensor.temperature);
        cJSON_AddBoolToObject(sensor, "valid", sensors[i].hw_sensor.valid);
        
        if (sensors[i].has_friendly_name) {
            cJSON_AddStringToObject(sensor, "friendly_name", sensors[i].friendly_name);
        } else {
            cJSON_AddNullToObject(sensor, "friendly_name");
        }
        
        cJSON_AddItemToArray(root, sensor);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/sensors/rescan
 */
static esp_err_t api_sensors_rescan_handler(httpd_req_t *req)
{
    esp_err_t err = sensor_manager_rescan();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    cJSON_AddNumberToObject(root, "sensor_count", sensor_manager_get_count());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/sensors/:address/name
 */
static esp_err_t api_sensor_name_handler(httpd_req_t *req)
{
    /* Extract address from URI */
    char address[20] = {0};
    const char *uri = req->uri;
    
    /* URI format: /api/sensors/XXXX/name */
    const char *start = strstr(uri, "/api/sensors/");
    if (start) {
        start += strlen("/api/sensors/");
        const char *end = strstr(start, "/name");
        if (end && (end - start) < sizeof(address)) {
            strncpy(address, start, end - start);
        }
    }

    ESP_LOGI("web_server", "Set name request for address: '%s'", address);

    if (strlen(address) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid address");
        return ESP_FAIL;
    }

    /* Read request body */
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI("web_server", "Request body: %s", content);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *name_json = cJSON_GetObjectItem(root, "friendly_name");
    const char *friendly_name = cJSON_IsString(name_json) ? name_json->valuestring : NULL;

    ESP_LOGI("web_server", "Setting name for %s: '%s'", address, friendly_name ? friendly_name : "(null)");

    cJSON_Delete(root);

    /* Update sensor with new name */
    esp_err_t err = sensor_manager_set_friendly_name(address, friendly_name);
    
    if (err != ESP_OK) {
        ESP_LOGE("web_server", "Failed to set friendly name: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /config
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)config_html_start, config_html_end - config_html_start);
    return ESP_OK;
}

/**
 * @brief Handler for GET /ota
 */
static esp_err_t ota_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)ota_html_start, ota_html_end - ota_html_start);
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/check
 */
static esp_err_t api_ota_check_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
#if CONFIG_OTA_ENABLED
    bool update_available = false;
    char latest_version[32] = {0};
    
    esp_err_t err = ota_check_for_update();
    update_available = ota_is_update_available();
    ota_get_latest_version(latest_version, sizeof(latest_version));
    
    cJSON_AddBoolToObject(root, "update_available", update_available);
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
    cJSON_AddStringToObject(root, "latest_version", latest_version);
#else
    cJSON_AddBoolToObject(root, "update_available", false);
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
    cJSON_AddStringToObject(root, "error", "OTA disabled");
#endif

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/update
 */
static esp_err_t api_ota_update_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
#if CONFIG_OTA_ENABLED
    if (ota_is_update_available()) {
        cJSON_AddBoolToObject(root, "started", true);
        cJSON_AddStringToObject(root, "message", "Update starting, device will restart");
        
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
        free(json);
        
        /* Start OTA in background */
        ota_start_update();
    } else {
        cJSON_AddBoolToObject(root, "started", false);
        cJSON_AddStringToObject(root, "message", "No update available");
        
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
        free(json);
    }
#else
    cJSON_AddBoolToObject(root, "started", false);
    cJSON_AddStringToObject(root, "error", "OTA disabled");
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
#endif
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/upload - Manual firmware upload
 * 
 * Expects raw binary firmware data (not multipart form)
 */
static esp_err_t api_ota_upload_handler(httpd_req_t *req)
{
    esp_err_t err;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    char *buf = NULL;
    const int buf_size = 4096;
    int received = 0;
    int remaining = req->content_len;
    bool ota_started = false;
    bool first_chunk = true;
    
    ESP_LOGI(TAG, "Starting manual firmware upload, size: %d bytes", req->content_len);
    
    /* Validate content length */
    if (req->content_len == 0 || req->content_len > 1500000) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid firmware size\"}");
        return ESP_FAIL;
    }
    
    /* Allocate receive buffer */
    buf = malloc(buf_size);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Memory allocation failed\"}");
        return ESP_FAIL;
    }
    
    /* Get update partition */
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No update partition found");
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No update partition available\"}");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx", update_partition->label, update_partition->address);
    
    /* Receive and write firmware data */
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, buf_size));
        if (recv_len < 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Receive error: %d", recv_len);
            goto upload_error;
        }
        
        /* Validate first chunk contains valid ESP32 firmware */
        if (first_chunk) {
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid firmware magic byte: 0x%02x (expected 0xE9)", (uint8_t)buf[0]);
                free(buf);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid firmware file - not an ESP32 binary\"}");
                return ESP_FAIL;
            }
            
            /* Begin OTA now that we've validated the firmware */
            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                free(buf);
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to start OTA\"}");
                return ESP_FAIL;
            }
            ota_started = true;
            first_chunk = false;
        }
        
        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            goto upload_error;
        }
        
        received += recv_len;
        remaining -= recv_len;
        
        if (received % 102400 == 0) {
            ESP_LOGI(TAG, "Upload progress: %d/%d bytes", received, req->content_len);
        }
    }
    
    /* Finish OTA */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Firmware validation failed - file may be corrupted\"}");
        return ESP_FAIL;
    }
    
    /* Set boot partition */
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to set boot partition\"}");
        return ESP_FAIL;
    }
    
    free(buf);
    
    ESP_LOGI(TAG, "Manual firmware upload complete, restarting...");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Firmware uploaded successfully, restarting...\"}");
    
    /* Restart after short delay to allow response to be sent */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;

upload_error:
    if (ota_started) {
        esp_ota_abort(ota_handle);
    }
    free(buf);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Upload failed\"}");
    return ESP_FAIL;
}

/**
 * @brief Handler for GET /api/wifi/scan - Scan for available networks
 */
static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_records[20];
    uint16_t ap_count = 0;
    
    esp_err_t err = wifi_manager_scan(ap_records, 20, &ap_count);
    
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    
    if (err == ESP_OK) {
        for (int i = 0; i < ap_count; i++) {
            /* Skip duplicates and empty SSIDs */
            if (strlen((char *)ap_records[i].ssid) == 0) continue;
            
            /* Check for duplicate SSID already in array */
            bool duplicate = false;
            cJSON *item;
            cJSON_ArrayForEach(item, networks) {
                cJSON *ssid_item = cJSON_GetObjectItem(item, "ssid");
                if (ssid_item && strcmp(ssid_item->valuestring, (char *)ap_records[i].ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            
            cJSON *network = cJSON_CreateObject();
            cJSON_AddStringToObject(network, "ssid", (char *)ap_records[i].ssid);
            cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
            cJSON_AddNumberToObject(network, "channel", ap_records[i].primary);
            cJSON_AddBoolToObject(network, "secure", ap_records[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(networks, network);
        }
        cJSON_AddBoolToObject(root, "success", true);
    } else {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    
    cJSON_AddItemToObject(root, "networks", networks);
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/config/wifi
 */
static esp_err_t api_config_wifi_get_handler(httpd_req_t *req)
{
    char ssid[32] = {0};
    char password[64] = {0};
    
    /* Try NVS first, then menuconfig defaults */
    esp_err_t err = nvs_storage_load_wifi_config(ssid, sizeof(ssid), 
                                                  password, sizeof(password));
    if (err != ESP_OK || strlen(ssid) == 0) {
#ifdef CONFIG_WIFI_SSID
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
#endif
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", ssid);
    /* Don't send password for security */
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/wifi
 */
static esp_err_t api_config_wifi_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(root, "password");
    
    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    
    /* If password not provided, load existing one */
    char password[64] = {0};
    if (cJSON_IsString(password_item) && strlen(password_item->valuestring) > 0) {
        strncpy(password, password_item->valuestring, sizeof(password) - 1);
    } else {
        char existing_ssid[32];
        nvs_storage_load_wifi_config(existing_ssid, sizeof(existing_ssid),
                                      password, sizeof(password));
    }
    
    esp_err_t err = nvs_storage_save_wifi_config(ssid_item->valuestring, password);
    cJSON_Delete(root);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "WiFi config saved. Restart to apply.");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/config/mqtt
 */
static esp_err_t api_config_mqtt_get_handler(httpd_req_t *req)
{
    char uri[128] = {0};
    char username[64] = {0};
    char password[64] = {0};
    
    esp_err_t err = nvs_storage_load_mqtt_config(uri, sizeof(uri),
                                                  username, sizeof(username),
                                                  password, sizeof(password));
    if (err != ESP_OK || strlen(uri) == 0) {
#ifdef CONFIG_MQTT_BROKER_URI
        strncpy(uri, CONFIG_MQTT_BROKER_URI, sizeof(uri) - 1);
#endif
#ifdef CONFIG_MQTT_USERNAME
        strncpy(username, CONFIG_MQTT_USERNAME, sizeof(username) - 1);
#endif
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uri", uri);
    cJSON_AddStringToObject(root, "username", username);
    /* Don't send password for security */
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/mqtt
 */
static esp_err_t api_config_mqtt_post_handler(httpd_req_t *req)
{
    char content[384];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *uri_item = cJSON_GetObjectItem(root, "uri");
    cJSON *username_item = cJSON_GetObjectItem(root, "username");
    cJSON *password_item = cJSON_GetObjectItem(root, "password");
    
    if (!cJSON_IsString(uri_item) || strlen(uri_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing uri");
        return ESP_FAIL;
    }
    
    const char *username = "";
    const char *password = "";
    char existing_password[64] = {0};
    
    if (cJSON_IsString(username_item)) {
        username = username_item->valuestring;
    }
    
    if (cJSON_IsString(password_item) && strlen(password_item->valuestring) > 0) {
        password = password_item->valuestring;
    } else {
        /* Load existing password */
        char existing_uri[128], existing_user[64];
        nvs_storage_load_mqtt_config(existing_uri, sizeof(existing_uri),
                                      existing_user, sizeof(existing_user),
                                      existing_password, sizeof(existing_password));
        password = existing_password;
    }
    
    esp_err_t err = nvs_storage_save_mqtt_config(uri_item->valuestring, username, password);
    cJSON_Delete(root);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "MQTT config saved");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/mqtt/reconnect
 */
static esp_err_t api_mqtt_reconnect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "MQTT reconnect requested");
    
    /* Stop and reinitialize MQTT with new settings */
    mqtt_ha_stop();
    mqtt_ha_init();
    mqtt_ha_start();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "MQTT reconnecting");
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/* External accessor functions from main.c */
extern uint32_t get_sensor_read_interval(void);
extern uint32_t get_sensor_publish_interval(void);
extern void set_sensor_read_interval(uint32_t ms);
extern void set_sensor_publish_interval(uint32_t ms);

/**
 * @brief Handler for GET /api/config/sensor
 */
static esp_err_t api_config_sensor_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "read_interval", get_sensor_read_interval());
    cJSON_AddNumberToObject(root, "publish_interval", get_sensor_publish_interval());
    cJSON_AddNumberToObject(root, "resolution", onewire_temp_get_resolution());
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/sensor
 */
static esp_err_t api_config_sensor_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *read_item = cJSON_GetObjectItem(root, "read_interval");
    cJSON *publish_item = cJSON_GetObjectItem(root, "publish_interval");
    cJSON *resolution_item = cJSON_GetObjectItem(root, "resolution");
    
    uint32_t read_interval = get_sensor_read_interval();
    uint32_t publish_interval = get_sensor_publish_interval();
    uint8_t resolution = onewire_temp_get_resolution();
    
    if (cJSON_IsNumber(read_item)) {
        read_interval = (uint32_t)read_item->valueint;
        if (read_interval < 1000) read_interval = 1000;
        if (read_interval > 300000) read_interval = 300000;
        set_sensor_read_interval(read_interval);
    }
    
    if (cJSON_IsNumber(publish_item)) {
        publish_interval = (uint32_t)publish_item->valueint;
        if (publish_interval < 5000) publish_interval = 5000;
        if (publish_interval > 600000) publish_interval = 600000;
        set_sensor_publish_interval(publish_interval);
    }
    
    if (cJSON_IsNumber(resolution_item)) {
        resolution = (uint8_t)resolution_item->valueint;
        if (resolution >= 9 && resolution <= 12) {
            onewire_temp_set_resolution(resolution);
        }
    }
    
    cJSON_Delete(root);
    
    /* Save to NVS */
    esp_err_t err = nvs_storage_save_sensor_settings(read_interval, publish_interval, resolution);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "Sensor settings saved");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/system/restart
 */
static esp_err_t api_system_restart_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "System restart requested");
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Restarting...");
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    /* Delay restart to allow response to be sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/system/factory-reset
 */
static esp_err_t api_system_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested");
    
    esp_err_t err = nvs_storage_factory_reset();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "Factory reset complete. Restarting...");
    } else {
        cJSON_AddStringToObject(response, "error", "Factory reset failed");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    if (err == ESP_OK) {
        /* Delay restart to allow response to be sent */
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    ESP_LOGI(TAG, "Starting web server on port %d", CONFIG_WEB_SERVER_PORT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;  /* Increased for config endpoints */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return err;
    }

    /* Register URI handlers */
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t sensors_uri = {
        .uri = "/api/sensors",
        .method = HTTP_GET,
        .handler = api_sensors_get_handler,
    };
    httpd_register_uri_handler(s_server, &sensors_uri);

    httpd_uri_t rescan_uri = {
        .uri = "/api/sensors/rescan",
        .method = HTTP_POST,
        .handler = api_sensors_rescan_handler,
    };
    httpd_register_uri_handler(s_server, &rescan_uri);

    httpd_uri_t sensor_name_uri = {
        .uri = "/api/sensors/*",
        .method = HTTP_POST,
        .handler = api_sensor_name_handler,
    };
    httpd_register_uri_handler(s_server, &sensor_name_uri);

    httpd_uri_t ota_check_uri = {
        .uri = "/api/ota/check",
        .method = HTTP_POST,
        .handler = api_ota_check_handler,
    };
    httpd_register_uri_handler(s_server, &ota_check_uri);

    httpd_uri_t ota_update_uri = {
        .uri = "/api/ota/update",
        .method = HTTP_POST,
        .handler = api_ota_update_handler,
    };
    httpd_register_uri_handler(s_server, &ota_update_uri);

    httpd_uri_t ota_upload_uri = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = api_ota_upload_handler,
    };
    httpd_register_uri_handler(s_server, &ota_upload_uri);

    /* Configuration page */
    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_register_uri_handler(s_server, &config_uri);

    /* OTA Update page */
    httpd_uri_t ota_page_uri = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_page_handler,
    };
    httpd_register_uri_handler(s_server, &ota_page_uri);

    /* WiFi scan endpoint */
    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_scan_uri);

    /* WiFi config endpoints */
    httpd_uri_t wifi_config_get_uri = {
        .uri = "/api/config/wifi",
        .method = HTTP_GET,
        .handler = api_config_wifi_get_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_config_get_uri);

    httpd_uri_t wifi_config_post_uri = {
        .uri = "/api/config/wifi",
        .method = HTTP_POST,
        .handler = api_config_wifi_post_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_config_post_uri);

    /* MQTT config endpoints */
    httpd_uri_t mqtt_config_get_uri = {
        .uri = "/api/config/mqtt",
        .method = HTTP_GET,
        .handler = api_config_mqtt_get_handler,
    };
    httpd_register_uri_handler(s_server, &mqtt_config_get_uri);

    httpd_uri_t mqtt_config_post_uri = {
        .uri = "/api/config/mqtt",
        .method = HTTP_POST,
        .handler = api_config_mqtt_post_handler,
    };
    httpd_register_uri_handler(s_server, &mqtt_config_post_uri);

    httpd_uri_t mqtt_reconnect_uri = {
        .uri = "/api/mqtt/reconnect",
        .method = HTTP_POST,
        .handler = api_mqtt_reconnect_handler,
    };
    httpd_register_uri_handler(s_server, &mqtt_reconnect_uri);

    /* Sensor config endpoints */
    httpd_uri_t sensor_config_get_uri = {
        .uri = "/api/config/sensor",
        .method = HTTP_GET,
        .handler = api_config_sensor_get_handler,
    };
    httpd_register_uri_handler(s_server, &sensor_config_get_uri);

    httpd_uri_t sensor_config_post_uri = {
        .uri = "/api/config/sensor",
        .method = HTTP_POST,
        .handler = api_config_sensor_post_handler,
    };
    httpd_register_uri_handler(s_server, &sensor_config_post_uri);

    /* System endpoints */
    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = api_system_restart_handler,
    };
    httpd_register_uri_handler(s_server, &system_restart_uri);

    httpd_uri_t factory_reset_uri = {
        .uri = "/api/system/factory-reset",
        .method = HTTP_POST,
        .handler = api_system_factory_reset_handler,
    };
    httpd_register_uri_handler(s_server, &factory_reset_uri);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
