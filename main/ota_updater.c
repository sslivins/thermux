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

/* Async check state */
typedef enum {
    OTA_CHECK_IDLE,
    OTA_CHECK_IN_PROGRESS,
    OTA_CHECK_COMPLETE,
    OTA_CHECK_FAILED
} ota_check_state_t;

static volatile ota_check_state_t s_check_state = OTA_CHECK_IDLE;
static TaskHandle_t s_check_task_handle = NULL;

/* OTA download progress tracking */
typedef enum {
    OTA_UPDATE_IDLE,
    OTA_UPDATE_DOWNLOADING,
    OTA_UPDATE_COMPLETE,
    OTA_UPDATE_FAILED
} ota_update_state_t;

static volatile ota_update_state_t s_update_state = OTA_UPDATE_IDLE;
static volatile int s_download_progress = 0;  /* 0-100 */
static volatile int s_download_total = 0;
static volatile int s_download_received = 0;

/**
 * @brief HTTP event handler for GitHub API request
 * 
 * Note: GitHub API uses chunked transfer encoding, so we must handle
 * both chunked and non-chunked responses.
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer = NULL;
    static int output_len = 0;
    
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        /* Handle both chunked and non-chunked responses */
        if (output_buffer == NULL) {
            output_buffer = (char *)malloc(GITHUB_API_BUFFER_SIZE);
            output_len = 0;
            if (output_buffer == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                return ESP_FAIL;
            }
        }
        /* Append data if there's room */
        if (output_len + evt->data_len < GITHUB_API_BUFFER_SIZE) {
            memcpy(output_buffer + output_len, evt->data, evt->data_len);
            output_len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "Response buffer full, truncating");
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
    case HTTP_EVENT_ERROR:
        /* Clean up on disconnect or error to prevent memory leaks */
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

/* Retry configuration */
#define OTA_CHECK_MAX_RETRIES   3
#define OTA_CHECK_RETRY_DELAY_MS 2000

/**
 * @brief Internal function to perform single OTA check attempt
 */
static esp_err_t ota_check_for_update_internal(void)
{
    /* Build GitHub API URL */
    char url[256];
    snprintf(url, sizeof(url), GITHUB_API_URL, CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
    ESP_LOGD(TAG, "API URL: %s", url);
    
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
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Updater");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    ESP_LOGI(TAG, "Sending request to GitHub API...");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int content_len = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP response: status=%d, content_length=%d", status, content_len);
        
        if (status == 200 && response_buffer != NULL) {
            /* Parse JSON response */
            cJSON *root = cJSON_Parse(response_buffer);
            if (root != NULL) {
                cJSON *tag_name = cJSON_GetObjectItem(root, "tag_name");
                cJSON *assets = cJSON_GetObjectItem(root, "assets");
                
                if (cJSON_IsString(tag_name)) {
                    strncpy(s_latest_version, tag_name->valuestring, sizeof(s_latest_version) - 1);
                    s_latest_version[sizeof(s_latest_version) - 1] = '\0';  /* Ensure null termination */
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
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON response");
            }
        } else if (status == 200 && response_buffer == NULL) {
            ESP_LOGE(TAG, "HTTP 200 but no response data received");
            err = ESP_FAIL;
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

/**
 * @brief Check for firmware updates with retry
 */
esp_err_t ota_check_for_update(void)
{
    ESP_LOGI(TAG, "Checking for updates...");
    
    s_update_available = false;
    
    esp_err_t err = ESP_FAIL;
    int retry_delay_ms = OTA_CHECK_RETRY_DELAY_MS;
    
    for (int attempt = 1; attempt <= OTA_CHECK_MAX_RETRIES; attempt++) {
        err = ota_check_for_update_internal();
        
        if (err == ESP_OK) {
            break;  /* Success */
        }
        
        if (attempt < OTA_CHECK_MAX_RETRIES) {
            ESP_LOGW(TAG, "OTA check attempt %d/%d failed, retrying in %d ms...", 
                     attempt, OTA_CHECK_MAX_RETRIES, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            retry_delay_ms *= 2;  /* Exponential backoff */
        } else {
            ESP_LOGE(TAG, "OTA check failed after %d attempts", OTA_CHECK_MAX_RETRIES);
        }
    }
    
    ESP_LOGI(TAG, "OTA check complete");
    return err;
}

/**
 * @brief Task for async OTA check (runs with larger stack)
 */
static void ota_check_task(void *pvParameters)
{
    esp_err_t err = ota_check_for_update();
    
    if (err == ESP_OK) {
        s_check_state = OTA_CHECK_COMPLETE;
    } else {
        s_check_state = OTA_CHECK_FAILED;
    }
    
    s_check_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t ota_check_for_update_async(void)
{
    if (s_check_state == OTA_CHECK_IN_PROGRESS) {
        ESP_LOGW(TAG, "OTA check already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Reset state for new check */
    s_check_state = OTA_CHECK_IN_PROGRESS;
    s_update_available = false;
    s_latest_version[0] = '\0';
    
    /* Create task with 8KB stack - enough for HTTPS + TLS */
    BaseType_t ret = xTaskCreate(ota_check_task, "ota_check", 8192, NULL, 5, &s_check_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA check task");
        s_check_state = OTA_CHECK_FAILED;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

bool ota_check_in_progress(void)
{
    return s_check_state == OTA_CHECK_IN_PROGRESS;
}

int ota_get_check_result(void)
{
    switch (s_check_state) {
        case OTA_CHECK_COMPLETE:
            return 1;
        case OTA_CHECK_FAILED:
            return -1;
        case OTA_CHECK_IN_PROGRESS:
        default:
            return 0;
    }
}

bool ota_is_update_available(void)
{
    return s_update_available;
}

esp_err_t ota_get_latest_version(char *version, size_t max_len)
{
    if (s_latest_version[0] == '\0') {
        strncpy(version, "unknown", max_len - 1);
    } else {
        strncpy(version, s_latest_version, max_len - 1);
    }
    version[max_len - 1] = '\0';
    return ESP_OK;
}

/**
 * @brief OTA update task with progress tracking
 */
static void ota_update_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting OTA update from: %s", s_download_url);
    
    s_update_state = OTA_UPDATE_DOWNLOADING;
    s_download_progress = 0;
    s_download_total = 0;
    s_download_received = 0;
    
    esp_http_client_config_t config = {
        .url = s_download_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,      /* Larger buffer for faster download */
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .partial_http_download = true,
        .max_http_request_size = 8 * 1024,  /* Smaller chunks for more progress updates */
    };
    
    esp_https_ota_handle_t ota_handle = NULL;
    ESP_LOGI(TAG, "Connecting to GitHub...");
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    
    /* Get total image size - may be 0 if server doesn't provide Content-Length */
    int real_total = esp_https_ota_get_image_size(ota_handle);
    ESP_LOGI(TAG, "Image size from server: %d bytes (0 means unknown)", real_total);
    
    /* If image size unknown, estimate ~1.1MB based on typical firmware size */
    s_download_total = (real_total > 0) ? real_total : (1100 * 1024);
    
    /* Download and flash in chunks */
    int last_logged_pct = -1;
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        /* Update progress */
        s_download_received = esp_https_ota_get_image_len_read(ota_handle);
        s_download_progress = (s_download_received * 100) / s_download_total;
        if (s_download_progress > 99) s_download_progress = 99;  /* Cap at 99 until complete */
        
        /* Log every 5% change to avoid log spam */
        if (s_download_progress / 5 != last_logged_pct / 5) {
            ESP_LOGI(TAG, "Download: %d KB / %d KB (%d%%)", 
                     s_download_received / 1024, s_download_total / 1024, s_download_progress);
            last_logged_pct = s_download_progress;
        }
        
        /* Yield to allow HTTP server to respond to status requests */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    
    /* Verify and finish */
    if (esp_https_ota_is_complete_data_received(ota_handle) != true) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(ota_handle);
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    
    s_download_progress = 100;
    err = esp_https_ota_finish(ota_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful, restarting...");
        s_update_state = OTA_UPDATE_COMPLETE;
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        s_update_state = OTA_UPDATE_FAILED;
    }
    
    vTaskDelete(NULL);
}

esp_err_t ota_start_update(void)
{
    if (!s_update_available || strlen(s_download_url) == 0) {
        ESP_LOGE(TAG, "No update available or download URL not set");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Reset progress state */
    s_update_state = OTA_UPDATE_IDLE;
    s_download_progress = 0;
    s_download_total = 0;
    s_download_received = 0;
    
    /* Create OTA task with high priority */
    xTaskCreate(ota_update_task, "ota_update", 8192, NULL, 10, NULL);
    
    return ESP_OK;
}

const char* ota_get_current_version(void)
{
    return APP_VERSION;
}

bool ota_update_in_progress(void)
{
    return s_update_state == OTA_UPDATE_DOWNLOADING;
}

int ota_get_update_state(void)
{
    /* Return state: 0=idle, 1=downloading, 2=complete, -1=failed */
    switch (s_update_state) {
        case OTA_UPDATE_IDLE: return 0;
        case OTA_UPDATE_DOWNLOADING: return 1;
        case OTA_UPDATE_COMPLETE: return 2;
        case OTA_UPDATE_FAILED: return -1;
        default: return 0;
    }
}

int ota_get_download_progress(void)
{
    return s_download_progress;
}

void ota_get_download_stats(int *received, int *total)
{
    if (received) *received = s_download_received;
    if (total) *total = s_download_total;
}
