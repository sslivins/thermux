/**
 * @file ota_updater.c
 * @brief OTA firmware updates from GitHub Releases
 * 
 * This module checks GitHub releases for new firmware versions and
 * downloads/installs updates using ESP-IDF's HTTPS OTA functionality.
 */

#include "ota_updater.h"
#include "version_utils.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_updater";

/* Forward declaration */
extern const char *APP_VERSION;

/* GitHub API URL for releases */
#define GITHUB_API_URL "https://api.github.com/repos/%s/%s/releases/latest"
#define GITHUB_API_BUFFER_SIZE 4096

static char s_latest_version[32] = {0};
static char s_download_url[512] = {0};
static bool s_update_available = false;

/**
 * @brief HTTP event handler for GitHub API request
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer = NULL;
    static int output_len = 0;
    
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            if (output_buffer == NULL) {
                output_buffer = (char *)malloc(GITHUB_API_BUFFER_SIZE);
                output_len = 0;
                if (output_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                    return ESP_FAIL;
                }
            }
            int copy_len = evt->data_len;
            if (output_len + copy_len < GITHUB_API_BUFFER_SIZE) {
                memcpy(output_buffer + output_len, evt->data, copy_len);
                output_len += copy_len;
            }
        }
        break;
        
    case HTTP_EVENT_ON_FINISH:
        if (output_buffer != NULL) {
            output_buffer[output_len] = '\0';
            /* Store the buffer pointer in user_data for later use */
            if (evt->user_data) {
                *((char **)evt->user_data) = output_buffer;
            }
            output_buffer = NULL;
            output_len = 0;
        }
        break;
        
    case HTTP_EVENT_DISCONNECTED:
        if (output_buffer != NULL) {
            free(output_buffer);
            output_buffer = NULL;
            output_len = 0;
        }
        break;
        
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t ota_updater_init(void)
{
    ESP_LOGI(TAG, "OTA updater initialized");
    ESP_LOGI(TAG, "Current version: %s", APP_VERSION);
    ESP_LOGI(TAG, "GitHub repo: %s/%s", CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
    
    s_update_available = false;
    s_latest_version[0] = '\0';
    s_download_url[0] = '\0';
    
    return ESP_OK;
}

esp_err_t ota_check_for_update(void)
{
    ESP_LOGI(TAG, "Checking for updates...");
    
    s_update_available = false;
    
    /* Build GitHub API URL */
    char url[256];
    snprintf(url, sizeof(url), GITHUB_API_URL, CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
    
    char *response_buffer = NULL;
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response_buffer,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    /* Add User-Agent header (required by GitHub API) */
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Updater");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        
        if (status == 200 && response_buffer != NULL) {
            /* Parse JSON response */
            cJSON *root = cJSON_Parse(response_buffer);
            if (root != NULL) {
                cJSON *tag_name = cJSON_GetObjectItem(root, "tag_name");
                cJSON *assets = cJSON_GetObjectItem(root, "assets");
                
                if (cJSON_IsString(tag_name)) {
                    strncpy(s_latest_version, tag_name->valuestring, sizeof(s_latest_version) - 1);
                    ESP_LOGI(TAG, "Latest version: %s", s_latest_version);
                    
                    /* Compare versions */
                    if (version_compare(s_latest_version, APP_VERSION) > 0) {
                        s_update_available = true;
                        ESP_LOGI(TAG, "Update available: %s -> %s", APP_VERSION, s_latest_version);
                        
                        /* Find firmware binary in assets */
                        if (cJSON_IsArray(assets)) {
                            int asset_count = cJSON_GetArraySize(assets);
                            for (int i = 0; i < asset_count; i++) {
                                cJSON *asset = cJSON_GetArrayItem(assets, i);
                                cJSON *name = cJSON_GetObjectItem(asset, "name");
                                cJSON *browser_url = cJSON_GetObjectItem(asset, "browser_download_url");
                                
                                if (cJSON_IsString(name) && cJSON_IsString(browser_url)) {
                                    /* Look for .bin file */
                                    if (strstr(name->valuestring, ".bin") != NULL) {
                                        strncpy(s_download_url, browser_url->valuestring, 
                                               sizeof(s_download_url) - 1);
                                        ESP_LOGI(TAG, "Firmware URL: %s", s_download_url);
                                        break;
                                    }
                                }
                            }
                        }
                    } else {
                        ESP_LOGI(TAG, "Already up to date");
                    }
                }
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGE(TAG, "GitHub API returned status %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    if (response_buffer) {
        free(response_buffer);
    }
    
    esp_http_client_cleanup(client);
    return err;
}

bool ota_is_update_available(void)
{
    return s_update_available;
}

esp_err_t ota_get_latest_version(char *version, size_t max_len)
{
    if (strlen(s_latest_version) == 0) {
        strncpy(version, "unknown", max_len - 1);
    } else {
        strncpy(version, s_latest_version, max_len - 1);
    }
    version[max_len - 1] = '\0';
    return ESP_OK;
}

/**
 * @brief OTA update task
 */
static void ota_update_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting OTA update from: %s", s_download_url);
    
    esp_http_client_config_t config = {
        .url = s_download_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .partial_http_download = true,
        .max_http_request_size = 64 * 1024,
    };
    
    esp_err_t err = esp_https_ota(&ota_config);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful, restarting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
    }
    
    vTaskDelete(NULL);
}

esp_err_t ota_start_update(void)
{
    if (!s_update_available || strlen(s_download_url) == 0) {
        ESP_LOGE(TAG, "No update available or download URL not set");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Create OTA task with high priority */
    xTaskCreate(ota_update_task, "ota_update", 8192, NULL, 10, NULL);
    
    return ESP_OK;
}

const char* ota_get_current_version(void)
{
    return APP_VERSION;
}
