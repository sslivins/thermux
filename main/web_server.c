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
#include "ethernet_manager.h"
#include "log_buffer.h"
#include "mqtt_client_ha.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include "esp_random.h"
#include "esp_timer.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

/* Auth credentials cache (loaded from NVS at startup) */
static bool s_auth_enabled = false;
static char s_auth_username[33] = "";
static char s_auth_password[65] = "";

/* Session management - supports multiple concurrent sessions */
#define MAX_SESSIONS 4
#define SESSION_TIMEOUT_MS (7LL * 24 * 60 * 60 * 1000)  /* 7 days */

typedef struct {
    char token[33];      /* Random hex token */
    int64_t expiry;      /* Expiry time (ms since boot) */
} session_t;

static session_t s_sessions[MAX_SESSIONS] = {0};

/* API key for stateless API access */
static char s_api_key[65] = "";  /* 32 hex chars (128-bit key) */

extern const char *APP_VERSION;

/* Forward declarations for reconfiguration */
extern esp_err_t mqtt_ha_stop(void);
extern esp_err_t mqtt_ha_init(void);
extern esp_err_t mqtt_ha_start(void);

/* Forward declarations */
static void generate_api_key(void);

/* Embedded HTML files (gzipped at build time) */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t config_html_gz_start[] asm("_binary_config_html_gz_start");
extern const uint8_t config_html_gz_end[] asm("_binary_config_html_gz_end");

/**
 * @brief Load auth config from NVS (called at startup)
 */
static void load_auth_config(void)
{
    esp_err_t err = nvs_storage_load_auth_config(&s_auth_enabled, s_auth_username, 
                                                  sizeof(s_auth_username), s_auth_password, 
                                                  sizeof(s_auth_password), s_api_key,
                                                  sizeof(s_api_key));
    if (err != ESP_OK) {
        /* No config saved, use Kconfig defaults if enabled */
#if CONFIG_WEB_AUTH_ENABLED
        s_auth_enabled = true;
        strncpy(s_auth_username, CONFIG_WEB_AUTH_USERNAME, sizeof(s_auth_username) - 1);
        strncpy(s_auth_password, CONFIG_WEB_AUTH_PASSWORD, sizeof(s_auth_password) - 1);
        ESP_LOGI(TAG, "Using default auth credentials from Kconfig");
#else
        s_auth_enabled = false;
        ESP_LOGI(TAG, "Web authentication disabled");
#endif
    } else {
        ESP_LOGI(TAG, "Loaded auth config (enabled=%d)", s_auth_enabled);
    }
    
    /* Generate API key if none exists */
    if (s_auth_enabled && strlen(s_api_key) == 0) {
        generate_api_key();
        ESP_LOGI(TAG, "Generated new API key");
        /* Save the generated key */
        nvs_storage_save_auth_config(s_auth_enabled, s_auth_username, s_auth_password, s_api_key);
    }
}

/**
 * @brief Generate a random session token and store in an available slot
 * @return Pointer to the token string (valid until session expires/replaced)
 */
static const char* generate_session_token(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    int slot = -1;
    int64_t oldest_expiry = INT64_MAX;
    int oldest_slot = 0;
    
    /* Find empty/expired slot, or track oldest for replacement */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0' || now > s_sessions[i].expiry) {
            slot = i;
            break;
        }
        if (s_sessions[i].expiry < oldest_expiry) {
            oldest_expiry = s_sessions[i].expiry;
            oldest_slot = i;
        }
    }
    
    /* Use oldest slot if no empty/expired found */
    if (slot < 0) {
        slot = oldest_slot;
        ESP_LOGD(TAG, "Replacing oldest session in slot %d", slot);
    }
    
    /* Generate random token */
    uint32_t rnd[4];
    for (int i = 0; i < 4; i++) {
        rnd[i] = esp_random();
    }
    snprintf(s_sessions[slot].token, sizeof(s_sessions[slot].token), "%08lx%08lx%08lx%08lx",
             (unsigned long)rnd[0], (unsigned long)rnd[1], 
             (unsigned long)rnd[2], (unsigned long)rnd[3]);
    s_sessions[slot].expiry = now + SESSION_TIMEOUT_MS;
    
    ESP_LOGD(TAG, "Created session in slot %d", slot);
    return s_sessions[slot].token;
}

/**
 * @brief Generate a random API key (256-bit)
 */
static void generate_api_key(void)
{
    uint32_t rnd[8];
    for (int i = 0; i < 8; i++) {
        rnd[i] = esp_random();
    }
    snprintf(s_api_key, sizeof(s_api_key), 
             "%08lx%08lx%08lx%08lx%08lx%08lx%08lx%08lx",
             (unsigned long)rnd[0], (unsigned long)rnd[1], 
             (unsigned long)rnd[2], (unsigned long)rnd[3],
             (unsigned long)rnd[4], (unsigned long)rnd[5],
             (unsigned long)rnd[6], (unsigned long)rnd[7]);
}

/**
 * @brief Check if session token from cookie is valid
 */
static bool is_session_valid(httpd_req_t *req)
{
    /* Get Cookie header */
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len == 0) {
        return false;
    }

    char *cookie = malloc(cookie_len + 1);
    if (!cookie) {
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, cookie_len + 1) != ESP_OK) {
        free(cookie);
        return false;
    }

    /* Look for session=TOKEN in cookie */
    char *session_start = strstr(cookie, "session=");
    if (!session_start) {
        free(cookie);
        return false;
    }

    session_start += 8;  /* Skip "session=" */
    char token[33] = {0};
    int i = 0;
    while (session_start[i] && session_start[i] != ';' && i < 32) {
        token[i] = session_start[i];
        i++;
    }
    token[i] = '\0';
    free(cookie);

    /* Check token against all sessions */
    int64_t now = esp_timer_get_time() / 1000;
    for (int j = 0; j < MAX_SESSIONS; j++) {
        if (s_sessions[j].token[0] != '\0' && strcmp(token, s_sessions[j].token) == 0) {
            if (now > s_sessions[j].expiry) {
                s_sessions[j].token[0] = '\0';  /* Clear expired session */
                return false;
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief Redirect to login page
 */
static void redirect_to_login(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
}

/**
 * @brief Check session auth - redirects to login if unauthorized
 * @return true if authorized (or auth disabled), false if redirect sent
 */
static bool check_session_auth(httpd_req_t *req)
{
    if (!s_auth_enabled) {
        return true;  /* Auth disabled, allow all */
    }

    if (is_session_valid(req)) {
        return true;  /* Valid session */
    }

    redirect_to_login(req);
    return false;
}

/**
 * @brief Check if API key from header is valid
 */
static bool is_api_key_valid(httpd_req_t *req)
{
    if (strlen(s_api_key) == 0) {
        return false;  /* No API key configured */
    }
    
    /* Check X-API-Key header */
    size_t key_len = httpd_req_get_hdr_value_len(req, "X-API-Key");
    if (key_len == 0) {
        return false;
    }
    
    char *key = malloc(key_len + 1);
    if (key == NULL) {
        return false;
    }
    
    if (httpd_req_get_hdr_value_str(req, "X-API-Key", key, key_len + 1) == ESP_OK) {
        bool valid = (strcmp(key, s_api_key) == 0);
        free(key);
        return valid;
    }
    
    free(key);
    return false;
}

/**
 * @brief Check session auth for API calls - returns 401 JSON instead of redirect
 * Checks both session cookie and X-API-Key header
 * @return true if authorized (or auth disabled), false if 401 sent
 */
static bool check_api_auth(httpd_req_t *req)
{
    if (!s_auth_enabled) {
        return true;
    }

    /* Check API key first (stateless auth) */
    if (is_api_key_valid(req)) {
        return true;
    }

    /* Fall back to session cookie */
    if (is_session_valid(req)) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\",\"login_required\":true}");
    return false;
}

/* Macro to check auth at start of handler - returns ESP_OK if unauthorized (response already sent) */
#define CHECK_AUTH(req) do { if (!check_api_auth(req)) return ESP_OK; } while(0)
#define CHECK_PAGE_AUTH(req) do { if (!check_session_auth(req)) return ESP_OK; } while(0)

/**
 * @brief Handler for GET /
 */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    CHECK_PAGE_AUTH(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    return ESP_OK;
}

/* Note: HTML content moved to external files in main/html/ directory */

/**
 * @brief Handler for GET /api/status
 */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", APP_VERSION);
    cJSON_AddNumberToObject(root, "sensor_count", sensor_manager_get_count());
    cJSON_AddNumberToObject(root, "max_sensors", CONFIG_MAX_SENSORS);
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    
    extern bool mqtt_ha_is_connected(void);
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_ha_is_connected());

    /* Richer MQTT status for the UI: distinguish "reconnecting" from a hard
     * "disconnected", and surface the last connection error + its age. */
    {
        mqtt_ha_status_t mqtt_status;
        mqtt_ha_get_status(&mqtt_status);
        cJSON_AddBoolToObject(root, "mqtt_connecting", mqtt_status.connecting);
        cJSON_AddStringToObject(root, "mqtt_last_error", mqtt_status.last_error);
        if (mqtt_status.last_error_uptime_s > 0) {
            int64_t now_s = esp_timer_get_time() / 1000000;
            int64_t age = now_s - mqtt_status.last_error_uptime_s;
            cJSON_AddNumberToObject(root, "mqtt_last_error_age_sec", age < 0 ? 0 : age);
        } else {
            cJSON_AddNullToObject(root, "mqtt_last_error_age_sec");
        }
    }
    
    /* Network connection status */
    bool eth_connected = ethernet_manager_is_connected();
    bool wifi_connected = wifi_manager_is_connected();
    cJSON_AddBoolToObject(root, "ethernet_connected", eth_connected);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_connected);
    cJSON_AddStringToObject(root, "ethernet_ip", eth_connected ? ethernet_manager_get_ip() : "");
    cJSON_AddStringToObject(root, "wifi_ip", wifi_connected ? wifi_manager_get_ip() : "");

    /* Bus error statistics */
    uint32_t total_reads, failed_reads;
    onewire_temp_get_error_stats(&total_reads, &failed_reads);
    onewire_bus_health_t health;
    onewire_temp_get_bus_health(&health);
    cJSON *bus_stats = cJSON_CreateObject();
    cJSON_AddNumberToObject(bus_stats, "total_reads", total_reads);
    cJSON_AddNumberToObject(bus_stats, "failed_reads", failed_reads);
    cJSON_AddNumberToObject(bus_stats, "error_rate", total_reads > 0 ? (double)failed_reads / total_reads * 100.0 : 0.0);
    cJSON_AddNumberToObject(bus_stats, "recent_error_rate",
                            health.recent_total_reads > 0
                                ? (double)health.recent_failed_reads / health.recent_total_reads * 100.0
                                : 0.0);
    cJSON_AddNumberToObject(bus_stats, "consecutive_failed_cycles",
                            health.consecutive_failed_cycles);
    if (health.has_successful_read) {
        cJSON_AddNumberToObject(bus_stats, "seconds_since_last_success",
                                health.seconds_since_last_success);
    } else {
        cJSON_AddNullToObject(bus_stats, "seconds_since_last_success");
    }

    /* Read-cycle duration statistics (helps validate read_interval headroom) */
    onewire_read_duration_stats_t duration_stats;
    onewire_temp_get_read_duration_stats(&duration_stats);
    cJSON_AddNumberToObject(bus_stats, "last_read_duration_ms", duration_stats.last_ms);
    cJSON_AddNumberToObject(bus_stats, "min_read_duration_ms", duration_stats.min_ms);
    cJSON_AddNumberToObject(bus_stats, "max_read_duration_ms", duration_stats.max_ms);
    cJSON_AddNumberToObject(bus_stats, "avg_read_duration_ms", duration_stats.avg_ms);

    cJSON_AddItemToObject(root, "bus_stats", bus_stats);

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
    CHECK_AUTH(req);
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
        
        cJSON_AddNumberToObject(sensor, "total_reads", sensors[i].hw_sensor.total_reads);
        cJSON_AddNumberToObject(sensor, "failed_reads", sensors[i].hw_sensor.failed_reads);
        cJSON_AddBoolToObject(sensor, "genuine", sensors[i].hw_sensor.genuine);
        cJSON_AddBoolToObject(sensor, "genuine_check_ok", sensors[i].hw_sensor.genuine_check_ok);
        cJSON_AddNumberToObject(sensor, "count_remain", sensors[i].hw_sensor.count_remain);
        cJSON_AddNumberToObject(sensor, "count_per_c", sensors[i].hw_sensor.count_per_c);
        cJSON_AddNumberToObject(sensor, "conversion_time_ms", sensors[i].hw_sensor.conversion_time_ms);
        
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
    CHECK_AUTH(req);
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
 * @brief Handler for POST /api/sensors/error-stats/reset
 */
static esp_err_t api_error_stats_reset_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    onewire_temp_reset_error_stats();
    sensor_manager_reset_all_error_stats();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/sensors/:address/error-stats/reset
 */
static esp_err_t api_sensor_error_stats_reset_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    /* Extract address from URI: /api/sensors/XXXX/error-stats/reset */
    char address[20] = {0};
    const char *uri = req->uri;
    const char *start = strstr(uri, "/api/sensors/");
    if (start) {
        start += strlen("/api/sensors/");
        const char *end = strstr(start, "/error-stats/reset");
        if (end && (end - start) < sizeof(address)) {
            strncpy(address, start, end - start);
        }
    }

    if (strlen(address) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid address");
        return ESP_FAIL;
    }

    esp_err_t err = sensor_manager_reset_sensor_error_stats(address);
    cJSON *root = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "success", true);
    } else {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "Sensor not found");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);

    return ESP_OK;
}

/**
 * @brief Handler for POST /api/sensors/:address/name and /api/sensors/:address/error-stats/reset
 */
static esp_err_t api_sensor_name_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    const char *uri = req->uri;

    /* Check if this is a per-sensor error stats reset request */
    if (strstr(uri, "/error-stats/reset")) {
        return api_sensor_error_stats_reset_handler(req);
    }

    /* Otherwise handle as name update */
    /* Extract address from URI */
    char address[20] = {0};
    
    /* URI format: /api/sensors/XXXX/name */
    const char *start = strstr(uri, "/api/sensors/");
    if (start) {
        start += strlen("/api/sensors/");
        const char *end = strstr(start, "/name");
        if (end && (end - start) < sizeof(address)) {
            strncpy(address, start, end - start);
        }
    }

    ESP_LOGD("web_server", "Set name request for address: '%s'", address);

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

    ESP_LOGD("web_server", "Request body: %s", content);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *name_json = cJSON_GetObjectItem(root, "friendly_name");
    
    /* Copy the name before deleting cJSON - the pointer becomes invalid after cJSON_Delete */
    char friendly_name[64] = {0};
    if (cJSON_IsString(name_json) && name_json->valuestring) {
        strncpy(friendly_name, name_json->valuestring, sizeof(friendly_name) - 1);
    }

    ESP_LOGD("web_server", "Setting name for %s: '%s'", address, friendly_name);

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
    CHECK_PAGE_AUTH(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)config_html_gz_start, config_html_gz_end - config_html_gz_start);
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/ota/channel - returns whether the pre-release
 * (beta) channel is enabled for cloud update checks
 */
static esp_err_t api_ota_channel_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "include_prerelease", ota_updater_get_include_prerelease());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/channel - enables/disables the
 * pre-release (beta) channel for cloud update checks
 * Body: {"include_prerelease": true|false}
 */
static esp_err_t api_ota_channel_post_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char content[64];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[received] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *include_json = cJSON_GetObjectItem(root, "include_prerelease");
    if (!include_json || !cJSON_IsBool(include_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing include_prerelease");
        return ESP_FAIL;
    }

    bool enabled = cJSON_IsTrue(include_json);
    cJSON_Delete(root);

    ota_updater_set_include_prerelease(enabled);
    esp_err_t err = nvs_storage_save_ota_prerelease_channel(enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist OTA pre-release channel setting: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA pre-release channel %s", enabled ? "enabled" : "disabled");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/check - starts async check
 */
static esp_err_t api_ota_check_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    ESP_LOGD(TAG, "OTA check requested via web UI");
    cJSON *root = cJSON_CreateObject();
    
#if CONFIG_OTA_ENABLED
    /* Start async check to avoid stack overflow in httpd task */
    esp_err_t err = ota_check_for_update_async();
    
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "checking", true);
        cJSON_AddStringToObject(root, "message", "Check started");
    } else if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddBoolToObject(root, "checking", true);
        cJSON_AddStringToObject(root, "message", "Check already in progress");
    } else {
        cJSON_AddBoolToObject(root, "checking", false);
        cJSON_AddStringToObject(root, "error", "Failed to start check");
    }
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
#else
    cJSON_AddBoolToObject(root, "checking", false);
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
 * @brief Handler for GET /api/ota/status - poll for check result and download progress
 */
static esp_err_t api_ota_status_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    
#if CONFIG_OTA_ENABLED
    int result = ota_get_check_result();
    bool checking = ota_check_in_progress();
    bool update_available = ota_is_update_available();
    char latest_version[32] = {0};
    ota_get_latest_version(latest_version, sizeof(latest_version));
    
    /* Add download progress info */
    int update_state = ota_get_update_state();  /* 0=idle, 1=downloading, 2=complete, -1=failed */
    int download_progress = ota_get_download_progress();
    int received = 0, total = 0;
    ota_get_download_stats(&received, &total);
    
    ESP_LOGD(TAG, "OTA status: checking=%d, result=%d, update=%d, version=%s, update_state=%d, progress=%d%%",
             checking, result, update_available, latest_version, update_state, download_progress);
    
    cJSON_AddBoolToObject(root, "checking", checking);
    cJSON_AddNumberToObject(root, "result", result);  /* 0=in progress, 1=complete, -1=failed */
    cJSON_AddBoolToObject(root, "update_available", update_available);
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
    cJSON_AddStringToObject(root, "latest_version", latest_version);
    cJSON_AddBoolToObject(root, "latest_is_prerelease", ota_is_latest_prerelease());
    
    /* Download progress fields:
       update_state: 0=idle, 1=downloading, 2=complete (rebooting soon), -1=failed */
    cJSON_AddNumberToObject(root, "update_state", update_state);
    cJSON_AddNumberToObject(root, "download_progress", download_progress);
    cJSON_AddNumberToObject(root, "download_received", received);
    cJSON_AddNumberToObject(root, "download_total", total);
#else
    cJSON_AddBoolToObject(root, "checking", false);
    cJSON_AddIntToObject(root, "result", -1);
    cJSON_AddBoolToObject(root, "update_available", false);
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
    cJSON_AddNumberToObject(root, "update_state", 0);
    cJSON_AddNumberToObject(root, "download_progress", 0);
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
    CHECK_AUTH(req);
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
    CHECK_AUTH(req);
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
    
    ESP_LOGD(TAG, "Writing to partition: %s at 0x%lx", update_partition->label, update_partition->address);
    
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
            ESP_LOGD(TAG, "Upload progress: %d/%d bytes", received, req->content_len);
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
    CHECK_AUTH(req);
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
 * @brief Handler for GET /api/logs - returns recent log buffer contents
 */
static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    /* Allocate buffer for logs (same size as ring buffer) */
    char *log_data = malloc(LOG_BUFFER_SIZE);
    if (!log_data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    
    size_t len = log_buffer_get(log_data, LOG_BUFFER_SIZE);
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, log_data, len);
    
    free(log_data);
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/logs/clear - clears log buffer
 */
static esp_err_t api_logs_clear_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    log_buffer_clear();
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/logs/level - returns current log level
 */
static esp_err_t api_logs_level_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    /* Get current log level for "main" tag (representative of app) */
    esp_log_level_t level = esp_log_level_get("main");
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "level", (int)level);
    
    /* Also provide human-readable name */
    const char *level_names[] = {"none", "error", "warn", "info", "debug", "verbose"};
    cJSON_AddStringToObject(root, "level_name", level_names[level]);
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/logs/level - sets log level
 * Body: {"level": 3} where 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=verbose
 */
static esp_err_t api_logs_level_post_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    char content[64];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[received] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *level_json = cJSON_GetObjectItem(root, "level");
    if (!level_json || !cJSON_IsNumber(level_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing level");
        return ESP_FAIL;
    }
    
    int level = level_json->valueint;
    cJSON_Delete(root);
    
    if (level < 0 || level > 5) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid level (0-5)");
        return ESP_FAIL;
    }
    
    /* Set log level for all components */
    esp_log_level_set("*", (esp_log_level_t)level);
    
    ESP_LOGI(TAG, "Log level changed to %d", level);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/config/wifi
 */
static esp_err_t api_config_wifi_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
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
    CHECK_AUTH(req);
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
    CHECK_AUTH(req);
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
    CHECK_AUTH(req);
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
    
    /* Apply the new settings to the live client immediately so the device
     * reconnects with the updated broker/credentials without needing a
     * manual "Reconnect" click or a reboot. */
    if (err == ESP_OK) {
        mqtt_ha_reconnect();
    }
    
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
    CHECK_AUTH(req);
    ESP_LOGD(TAG, "MQTT reconnect requested");
    
    /* Rebuild the client from saved settings and reconnect. */
    mqtt_ha_reconnect();
    
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
 * @brief Estimated per-sensor overhead (ms) for reading a scratchpad off
 * the bus (reset + match-ROM + 9-byte read), used only as a fallback
 * before any real read cycle has completed. Conservative vs. the ~4-5ms
 * typically observed, to avoid under-estimating on a noisy bus.
 */
#define SENSOR_READ_OVERHEAD_MS 10

/**
 * @brief Compute the minimum read_interval (ms) that leaves reasonable
 * headroom over the actual time it takes to read every connected sensor.
 *
 * Prefers the worst observed read-cycle duration (onewire_temp's
 * min/max/avg stats) once at least one read has completed, since that
 * reflects real bus/sensor conditions. Falls back to a theoretical
 * estimate (conversion delay for the current resolution + per-sensor
 * bus overhead) before the first read cycle.
 *
 * Adds a 50% safety buffer (minimum +200ms) on top of the estimate so
 * transient slow cycles don't cause the read task's effective cadence
 * to silently exceed the configured interval.
 */
static uint32_t compute_safe_min_read_interval_ms(void)
{
    onewire_read_duration_stats_t stats;
    onewire_temp_get_read_duration_stats(&stats);

    uint32_t estimated_ms;
    if (stats.sample_count > 0) {
        estimated_ms = stats.max_ms;
    } else {
        static const uint32_t conversion_delay_ms[] = {100, 200, 400, 800}; /* 9,10,11,12-bit */
        int idx = onewire_temp_get_resolution() - 9;
        if (idx < 0) idx = 0;
        if (idx > 3) idx = 3;
        estimated_ms = conversion_delay_ms[idx] + (uint32_t)sensor_manager_get_count() * SENSOR_READ_OVERHEAD_MS;
    }

    uint32_t buffered_ms = estimated_ms + (estimated_ms / 2);
    if (buffered_ms < estimated_ms + 200) {
        buffered_ms = estimated_ms + 200;
    }
    if (buffered_ms < 5000) {
        buffered_ms = 5000; /* keep in step with the 5s hard floor */
    }
    return buffered_ms;
}

/**
 * @brief Handler for GET /api/config/sensor
 */
static esp_err_t api_config_sensor_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "read_interval", get_sensor_read_interval());
    cJSON_AddNumberToObject(root, "publish_interval", get_sensor_publish_interval());
    cJSON_AddNumberToObject(root, "resolution", onewire_temp_get_resolution());
    cJSON_AddNumberToObject(root, "min_safe_read_interval_ms", compute_safe_min_read_interval_ms());
    
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
    CHECK_AUTH(req);
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
    
    /* Apply resolution first so the read-interval safety check below
     * reflects the resolution being saved in this same request. */
    if (cJSON_IsNumber(resolution_item)) {
        resolution = (uint8_t)resolution_item->valueint;
        if (resolution >= 9 && resolution <= 12) {
            onewire_temp_set_resolution(resolution);
        }
    }

    bool read_interval_clamped_for_safety = false;
    uint32_t min_safe_read_interval_ms = compute_safe_min_read_interval_ms();

    if (cJSON_IsNumber(read_item)) {
        read_interval = (uint32_t)read_item->valueint;
        if (read_interval < 5000) read_interval = 5000;
        if (read_interval > 300000) read_interval = 300000;
        if (read_interval < min_safe_read_interval_ms) {
            read_interval = min_safe_read_interval_ms;
            read_interval_clamped_for_safety = true;
        }
        set_sensor_read_interval(read_interval);
    }
    
    if (cJSON_IsNumber(publish_item)) {
        publish_interval = (uint32_t)publish_item->valueint;
        if (publish_interval < 5000) publish_interval = 5000;
        if (publish_interval > 600000) publish_interval = 600000;
        set_sensor_publish_interval(publish_interval);
    }
    
    cJSON_Delete(root);
    
    /* Save to NVS */
    esp_err_t err = nvs_storage_save_sensor_settings(read_interval, publish_interval, resolution);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "Sensor settings saved");
    }
    cJSON_AddNumberToObject(response, "read_interval", read_interval);
    cJSON_AddBoolToObject(response, "read_interval_clamped", read_interval_clamped_for_safety);
    cJSON_AddNumberToObject(response, "min_safe_read_interval_ms", min_safe_read_interval_ms);
    
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
    CHECK_AUTH(req);
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
    CHECK_AUTH(req);
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

/**
 * @brief cJSON array callback for backup export - appends one sensor name entry
 */
static void backup_sensor_name_cb(const char *serial_hex, const char *friendly_name, void *ctx)
{
    cJSON *array = (cJSON *)ctx;
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "serial", serial_hex);
    cJSON_AddStringToObject(entry, "name", friendly_name);
    cJSON_AddItemToArray(array, entry);
}

/**
 * @brief Handler for GET /api/backup
 * Query params: mqtt=1, wifi=1, auth=1 to include those sections (default excluded)
 */
static esp_err_t api_backup_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);

    bool include_mqtt = false, include_wifi = false, include_auth = false;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *qbuf = malloc(qlen);
        if (qbuf != NULL && httpd_req_get_url_query_str(req, qbuf, qlen) == ESP_OK) {
            char val[4];
            if (httpd_query_key_value(qbuf, "mqtt", val, sizeof(val)) == ESP_OK) {
                include_mqtt = (strcmp(val, "1") == 0);
            }
            if (httpd_query_key_value(qbuf, "wifi", val, sizeof(val)) == ESP_OK) {
                include_wifi = (strcmp(val, "1") == 0);
            }
            if (httpd_query_key_value(qbuf, "auth", val, sizeof(val)) == ESP_OK) {
                include_auth = (strcmp(val, "1") == 0);
            }
        }
        free(qbuf);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_version", APP_VERSION);

    cJSON *names = cJSON_CreateArray();
    nvs_storage_enumerate_sensor_names(backup_sensor_name_cb, names);
    cJSON_AddItemToObject(root, "sensor_names", names);

    uint32_t read_interval = get_sensor_read_interval();
    uint32_t publish_interval = get_sensor_publish_interval();
    uint8_t resolution = onewire_temp_get_resolution();
    /* Prefer NVS-saved values (what a restore would actually apply); fall
     * back to current runtime values if nothing has been saved yet */
    nvs_storage_load_sensor_settings(&read_interval, &publish_interval, &resolution);

    cJSON *settings = cJSON_CreateObject();
    cJSON_AddNumberToObject(settings, "read_interval_ms", read_interval);
    cJSON_AddNumberToObject(settings, "publish_interval_ms", publish_interval);
    cJSON_AddNumberToObject(settings, "resolution", resolution);
    cJSON_AddItemToObject(root, "sensor_settings", settings);

    if (include_mqtt) {
        char uri[128] = "", user[64] = "", pass[64] = "";
        nvs_storage_load_mqtt_config(uri, sizeof(uri), user, sizeof(user), pass, sizeof(pass));
        cJSON *mqtt = cJSON_CreateObject();
        cJSON_AddStringToObject(mqtt, "broker_uri", uri);
        cJSON_AddStringToObject(mqtt, "username", user);
        cJSON_AddStringToObject(mqtt, "password", pass);
        cJSON_AddItemToObject(root, "mqtt", mqtt);
    }

    if (include_wifi) {
        char ssid[64] = "", pass[64] = "";
        nvs_storage_load_wifi_config(ssid, sizeof(ssid), pass, sizeof(pass));
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi, "ssid", ssid);
        cJSON_AddStringToObject(wifi, "password", pass);
        cJSON_AddItemToObject(root, "wifi", wifi);
    }

    if (include_auth) {
        bool enabled = false;
        char user[32] = "", pass[64] = "", api_key[64] = "";
        nvs_storage_load_auth_config(&enabled, user, sizeof(user), pass, sizeof(pass), api_key, sizeof(api_key));
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddBoolToObject(auth, "enabled", enabled);
        cJSON_AddStringToObject(auth, "username", user);
        cJSON_AddStringToObject(auth, "password", pass);
        cJSON_AddStringToObject(auth, "api_key", api_key);
        cJSON_AddItemToObject(root, "auth", auth);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"thermux-backup.json\"");
    httpd_resp_send(req, json, strlen(json));
    free(json);

    return ESP_OK;
}

/**
 * @brief Handler for POST /api/backup/restore
 * Accepts a JSON backup (as produced by GET /api/backup), applies any
 * sections present, then restarts the device.
 */
static esp_err_t api_backup_restore_post_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);

    if (req->content_len == 0 || req->content_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid backup size");
        return ESP_FAIL;
    }

    char *content = malloc(req->content_len + 1);
    if (content == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, content + received, req->content_len - received);
        if (ret <= 0) {
            free(content);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        received += ret;
    }
    content[received] = '\0';

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    int names_restored = 0;
    cJSON *names = cJSON_GetObjectItem(root, "sensor_names");
    if (cJSON_IsArray(names)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, names) {
            cJSON *serial = cJSON_GetObjectItem(entry, "serial");
            cJSON *name = cJSON_GetObjectItem(entry, "name");
            if (cJSON_IsString(serial) && cJSON_IsString(name) && strlen(serial->valuestring) == 12) {
                if (nvs_storage_save_sensor_name_by_serial(serial->valuestring, name->valuestring) == ESP_OK) {
                    names_restored++;
                }
            }
        }
    }

    cJSON *settings = cJSON_GetObjectItem(root, "sensor_settings");
    if (cJSON_IsObject(settings)) {
        cJSON *ri = cJSON_GetObjectItem(settings, "read_interval_ms");
        cJSON *pi = cJSON_GetObjectItem(settings, "publish_interval_ms");
        cJSON *res = cJSON_GetObjectItem(settings, "resolution");
        if (cJSON_IsNumber(ri) && cJSON_IsNumber(pi) && cJSON_IsNumber(res)) {
            /* Apply resolution first so the read-interval safety floor
             * below reflects the resolution being restored. */
            if (res->valueint >= 9 && res->valueint <= 12) {
                onewire_temp_set_resolution((uint8_t)res->valueint);
            }

            uint32_t read_interval = (uint32_t)ri->valueint;
            if (read_interval < 5000) read_interval = 5000;
            if (read_interval > 300000) read_interval = 300000;
            uint32_t min_safe_read_interval_ms = compute_safe_min_read_interval_ms();
            if (read_interval < min_safe_read_interval_ms) {
                read_interval = min_safe_read_interval_ms;
            }

            uint32_t publish_interval = (uint32_t)pi->valueint;
            if (publish_interval < 5000) publish_interval = 5000;
            if (publish_interval > 600000) publish_interval = 600000;

            nvs_storage_save_sensor_settings(read_interval, publish_interval, onewire_temp_get_resolution());
            set_sensor_read_interval(read_interval);
            set_sensor_publish_interval(publish_interval);
        }
    }

    bool mqtt_restored = false, wifi_restored = false, auth_restored = false;

    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        cJSON *uri = cJSON_GetObjectItem(mqtt, "broker_uri");
        cJSON *user = cJSON_GetObjectItem(mqtt, "username");
        cJSON *pass = cJSON_GetObjectItem(mqtt, "password");
        if (cJSON_IsString(uri)) {
            nvs_storage_save_mqtt_config(uri->valuestring,
                                          cJSON_IsString(user) ? user->valuestring : "",
                                          cJSON_IsString(pass) ? pass->valuestring : "");
            mqtt_restored = true;
        }
    }

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
        cJSON *pass = cJSON_GetObjectItem(wifi, "password");
        if (cJSON_IsString(ssid)) {
            nvs_storage_save_wifi_config(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
            wifi_restored = true;
        }
    }

    cJSON *auth = cJSON_GetObjectItem(root, "auth");
    if (cJSON_IsObject(auth)) {
        cJSON *enabled = cJSON_GetObjectItem(auth, "enabled");
        cJSON *user = cJSON_GetObjectItem(auth, "username");
        cJSON *pass = cJSON_GetObjectItem(auth, "password");
        cJSON *api_key = cJSON_GetObjectItem(auth, "api_key");
        nvs_storage_save_auth_config(cJSON_IsTrue(enabled),
                                      cJSON_IsString(user) ? user->valuestring : "",
                                      cJSON_IsString(pass) ? pass->valuestring : "",
                                      cJSON_IsString(api_key) ? api_key->valuestring : "");
        auth_restored = true;
    }

    cJSON_Delete(root);

    ESP_LOGW(TAG, "Backup restored: %d sensor name(s), mqtt=%d wifi=%d auth=%d",
             names_restored, mqtt_restored, wifi_restored, auth_restored);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddNumberToObject(response, "sensor_names_restored", names_restored);
    cJSON_AddBoolToObject(response, "mqtt_restored", mqtt_restored);
    cJSON_AddBoolToObject(response, "wifi_restored", wifi_restored);
    cJSON_AddBoolToObject(response, "auth_restored", auth_restored);
    cJSON_AddStringToObject(response, "message", "Restore complete. Restarting...");

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

/* Login page HTML - embedded directly since it's small and special */
static const char *login_html = 
"<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Login - Thermux</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#1a1a2e 0%,#16213e 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".login-card{background:rgba(255,255,255,0.05);border-radius:16px;padding:40px;width:100%;max-width:360px;box-shadow:0 8px 32px rgba(0,0,0,0.3)}"
".logo{text-align:center;margin-bottom:30px}"
".logo svg{width:64px;height:auto}"
"h1{color:#fff;text-align:center;margin-bottom:30px;font-size:1.5em;font-weight:500}"
".form-group{margin-bottom:20px}"
"label{display:block;color:#aaa;margin-bottom:8px;font-size:0.9em}"
"input{width:100%;padding:12px 16px;border:1px solid rgba(255,255,255,0.1);border-radius:8px;background:rgba(0,0,0,0.2);color:#fff;font-size:1em;transition:border-color 0.2s}"
"input::-ms-reveal{filter:invert(1)}input::-webkit-credentials-auto-fill-button{filter:invert(1)}"
"input:focus{outline:none;border-color:#4da6ff}"
".btn{width:100%;padding:14px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);border:none;border-radius:8px;color:#fff;font-size:1em;font-weight:600;cursor:pointer;transition:transform 0.2s,box-shadow 0.2s}"
".btn:hover{transform:translateY(-2px);box-shadow:0 4px 20px rgba(102,126,234,0.4)}"
".btn:active{transform:translateY(0)}"
".error{background:rgba(255,82,82,0.2);border:1px solid rgba(255,82,82,0.5);color:#ff5252;padding:12px;border-radius:8px;margin-bottom:20px;text-align:center;display:none}"
".error.show{display:block}"
"</style></head><body>"
"<div class=\"login-card\">"
"<div class=\"logo\"><svg viewBox=\"0 0 200 160\" aria-hidden=\"true\"><g fill=\"#cbd5e1\"><path d=\"M 76.00,71.00 L 76.00,60.00 L 75.97,59.48 L 75.86,58.96 L 75.70,58.47 L 75.46,58.00 L 75.17,57.56 L 74.83,57.17 L 74.44,56.83 L 74.00,56.54 L 73.53,56.30 L 73.04,56.14 L 72.52,56.03 L 72.00,56.00 L 56.00,56.00 L 56.00,64.00 L 68.00,64.00 L 68.00,75.00 L 68.03,75.52 L 68.14,76.04 L 68.30,76.53 L 68.54,77.00 L 68.83,77.44 L 69.17,77.83 L 69.56,78.17 L 70.00,78.46 L 70.47,78.70 L 70.96,78.86 L 71.48,78.97 L 72.00,79.00 L 97.00,79.00 L 97.00,71.00 Z\"/><path d=\"M 64.00,93.00 L 63.48,93.03 L 62.96,93.14 L 62.47,93.30 L 62.00,93.54 L 61.56,93.83 L 61.17,94.17 L 60.83,94.56 L 60.54,95.00 L 60.30,95.47 L 60.14,95.96 L 60.03,96.48 L 60.00,97.00 L 60.00,111.00 L 48.00,111.00 L 48.00,119.00 L 64.00,119.00 L 64.52,118.97 L 65.04,118.86 L 65.53,118.70 L 66.00,118.46 L 66.44,118.17 L 66.83,117.83 L 67.17,117.44 L 67.46,117.00 L 67.70,116.53 L 67.86,116.04 L 67.97,115.52 L 68.00,115.00 L 68.00,101.00 L 97.00,101.00 L 97.00,93.00 Z\"/><path d=\"M 128.00,79.00 L 128.52,78.97 L 129.04,78.86 L 129.53,78.70 L 130.00,78.46 L 130.44,78.17 L 130.83,77.83 L 131.17,77.44 L 131.46,77.00 L 131.70,76.53 L 131.86,76.04 L 131.97,75.52 L 132.00,75.00 L 132.00,64.00 L 144.00,64.00 L 144.00,56.00 L 128.00,56.00 L 127.48,56.03 L 126.96,56.14 L 126.47,56.30 L 126.00,56.54 L 125.56,56.83 L 125.17,57.17 L 124.83,57.56 L 124.54,58.00 L 124.30,58.47 L 124.14,58.96 L 124.03,59.48 L 124.00,60.00 L 124.00,71.00 L 103.00,71.00 L 103.00,79.00 Z\"/><path d=\"M 132.00,101.00 L 132.00,115.00 L 132.03,115.52 L 132.14,116.04 L 132.30,116.53 L 132.54,117.00 L 132.83,117.44 L 133.17,117.83 L 133.56,118.17 L 134.00,118.46 L 134.47,118.70 L 134.96,118.86 L 135.48,118.97 L 136.00,119.00 L 152.00,119.00 L 152.00,111.00 L 140.00,111.00 L 140.00,97.00 L 139.97,96.48 L 139.86,95.96 L 139.70,95.47 L 139.46,95.00 L 139.17,94.56 L 138.83,94.17 L 138.44,93.83 L 138.00,93.54 L 137.53,93.30 L 137.04,93.14 L 136.52,93.03 L 136.00,93.00 L 103.00,93.00 L 103.00,101.00 Z\"/></g><g><circle cx=\"54\" cy=\"60\" r=\"9\" fill=\"#cbd5e1\"/><circle cx=\"54\" cy=\"60\" r=\"4\" fill=\"#16213e\"/><circle cx=\"46\" cy=\"115\" r=\"9\" fill=\"#cbd5e1\"/><circle cx=\"46\" cy=\"115\" r=\"4\" fill=\"#16213e\"/><circle cx=\"146\" cy=\"60\" r=\"9\" fill=\"#cbd5e1\"/><circle cx=\"146\" cy=\"60\" r=\"4\" fill=\"#16213e\"/><circle cx=\"154\" cy=\"115\" r=\"9\" fill=\"#cbd5e1\"/><circle cx=\"154\" cy=\"115\" r=\"4\" fill=\"#16213e\"/></g><rect x=\"87.5\" y=\"42.5\" width=\"25\" height=\"95\" rx=\"12.5\" fill=\"#cbd5e1\"/><rect x=\"92.5\" y=\"47.5\" width=\"15\" height=\"85\" rx=\"7.5\" fill=\"#16213e\"/><circle cx=\"100\" cy=\"125\" r=\"19.5\" fill=\"#cbd5e1\"/><circle cx=\"100\" cy=\"125\" r=\"14.5\" fill=\"#16213e\"/><rect x=\"96\" y=\"53\" width=\"8\" height=\"78\" rx=\"4\" fill=\"#ef4444\"/><circle cx=\"100\" cy=\"125\" r=\"11\" fill=\"#ef4444\"/></svg></div>"
"<h1>Thermux</h1>"
"<div class=\"error\" id=\"error\">Invalid username or password</div>"
"<form id=\"loginForm\">"
"<div class=\"form-group\"><label>Username</label><input type=\"text\" id=\"username\" autocomplete=\"username\" autocapitalize=\"none\" autocorrect=\"off\" spellcheck=\"false\" enterkeyhint=\"next\" required></div>"
"<div class=\"form-group\"><label>Password</label><input type=\"password\" id=\"password\" autocomplete=\"current-password\" enterkeyhint=\"done\" required></div>"
"<button type=\"submit\" class=\"btn\">Sign In</button>"
"</form></div>"
"<script>"
"document.getElementById('loginForm').addEventListener('submit',async(e)=>{"
"e.preventDefault();"
"const u=document.getElementById('username').value;"
"const p=document.getElementById('password').value;"
"try{"
"const r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});"
"const d=await r.json();"
"if(d.success){window.location.href='/';}else{document.getElementById('error').classList.add('show');}"
"}catch(err){document.getElementById('error').classList.add('show');}"
"});"
"document.getElementById('username').focus();"
"</script></body></html>";

/**
 * @brief Handler for GET /login - login page
 */
static esp_err_t login_page_handler(httpd_req_t *req)
{
    /* If auth is disabled or already logged in, redirect to home */
    if (!s_auth_enabled || is_session_valid(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, login_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/auth/login - authenticate and create session
 */
static esp_err_t api_auth_login_handler(httpd_req_t *req)
{
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *username = cJSON_GetObjectItem(root, "username");
    cJSON *password = cJSON_GetObjectItem(root, "password");

    bool success = false;
    const char *session_token = NULL;
    if (cJSON_IsString(username) && cJSON_IsString(password)) {
        if (strcmp(username->valuestring, s_auth_username) == 0 &&
            strcmp(password->valuestring, s_auth_password) == 0) {
            success = true;
            session_token = generate_session_token();
            ESP_LOGI(TAG, "User '%s' logged in", s_auth_username);
        } else {
            ESP_LOGW(TAG, "Failed login attempt for user '%s'", 
                     username->valuestring ? username->valuestring : "(null)");
        }
    }
    cJSON_Delete(root);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);

    if (success && session_token) {
        /* Set session cookie */
        char cookie[80];
        snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly; SameSite=Strict", session_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    }

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);

    return ESP_OK;
}

/**
 * @brief Handler for POST /api/auth/logout - destroy session
 */
static esp_err_t api_auth_logout_handler(httpd_req_t *req)
{
    /* Get the session token from cookie and clear that specific session */
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len > 0) {
        char *cookie = malloc(cookie_len + 1);
        if (cookie && httpd_req_get_hdr_value_str(req, "Cookie", cookie, cookie_len + 1) == ESP_OK) {
            char *session_start = strstr(cookie, "session=");
            if (session_start) {
                session_start += 8;
                char token[33] = {0};
                int i = 0;
                while (session_start[i] && session_start[i] != ';' && i < 32) {
                    token[i] = session_start[i];
                    i++;
                }
                /* Find and clear matching session */
                for (int j = 0; j < MAX_SESSIONS; j++) {
                    if (strcmp(token, s_sessions[j].token) == 0) {
                        s_sessions[j].token[0] = '\0';
                        s_sessions[j].expiry = 0;
                        break;
                    }
                }
            }
        }
        free(cookie);
    }
    ESP_LOGI(TAG, "User logged out");

    /* Clear cookie */
    httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; HttpOnly; Max-Age=0");

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
 * @brief Handler for GET /api/auth/status - check if logged in
 */
static esp_err_t api_auth_status_handler(httpd_req_t *req)
{
    bool logged_in = !s_auth_enabled || is_session_valid(req);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "auth_enabled", s_auth_enabled);
    cJSON_AddBoolToObject(response, "logged_in", logged_in);
    if (logged_in && s_auth_enabled) {
        cJSON_AddStringToObject(response, "username", s_auth_username);
    }

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);

    return ESP_OK;
}

/**
 * @brief Handler for GET /api/config/auth
 */
static esp_err_t api_config_auth_get_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", s_auth_enabled);
    cJSON_AddStringToObject(root, "username", s_auth_username);
    /* Don't send password for security, but do send API key (user needs to see it to use it) */
    if (strlen(s_api_key) > 0) {
        cJSON_AddStringToObject(root, "api_key", s_api_key);
    }
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/auth
 */
static esp_err_t api_config_auth_post_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    
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
    
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON *username = cJSON_GetObjectItem(root, "username");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    
    /* Update local cache */
    if (cJSON_IsBool(enabled)) {
        s_auth_enabled = cJSON_IsTrue(enabled);
    }
    if (cJSON_IsString(username) && strlen(username->valuestring) > 0) {
        strncpy(s_auth_username, username->valuestring, sizeof(s_auth_username) - 1);
    }
    if (cJSON_IsString(password) && strlen(password->valuestring) > 0) {
        strncpy(s_auth_password, password->valuestring, sizeof(s_auth_password) - 1);
    }
    
    /* Generate API key if enabling auth and none exists */
    if (s_auth_enabled && strlen(s_api_key) == 0) {
        generate_api_key();
    }
    
    cJSON_Delete(root);
    
    /* Save to NVS */
    esp_err_t err = nvs_storage_save_auth_config(s_auth_enabled, s_auth_username, s_auth_password, s_api_key);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "Auth configuration saved");
        ESP_LOGI(TAG, "Auth config saved (enabled=%d, user=%s)", s_auth_enabled, s_auth_username);
    } else {
        cJSON_AddStringToObject(response, "error", "Failed to save");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/auth/regenerate-key
 * Regenerates the API key
 */
static esp_err_t api_config_auth_regenerate_key_handler(httpd_req_t *req)
{
    CHECK_AUTH(req);
    
    /* Generate new API key */
    generate_api_key();
    
    /* Save to NVS */
    esp_err_t err = nvs_storage_save_auth_config(s_auth_enabled, s_auth_username, s_auth_password, s_api_key);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "api_key", s_api_key);
        cJSON_AddStringToObject(response, "message", "API key regenerated");
        ESP_LOGI(TAG, "API key regenerated");
    } else {
        cJSON_AddStringToObject(response, "error", "Failed to save new key");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    /* Load auth config from NVS */
    load_auth_config();
    
    ESP_LOGD(TAG, "Starting web server on port %d", CONFIG_WEB_SERVER_PORT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 40;  /* 34 endpoints + room for future */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return err;
    }

    /* Helper macro to register URI handler with error checking */
    #define REGISTER_URI(uri_cfg) do { \
        esp_err_t ret = httpd_register_uri_handler(s_server, &uri_cfg); \
        if (ret != ESP_OK) { \
            ESP_LOGE(TAG, "ERROR: Failed to register %s - increase max_uri_handlers!", uri_cfg.uri); \
        } \
    } while(0)

    /* Register URI handlers */
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    REGISTER_URI(index_uri);

    /* Login page and auth endpoints (no auth required for these) */
    httpd_uri_t login_uri = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_page_handler,
    };
    REGISTER_URI(login_uri);

    httpd_uri_t auth_login_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = api_auth_login_handler,
    };
    REGISTER_URI(auth_login_uri);

    httpd_uri_t auth_logout_uri = {
        .uri = "/api/auth/logout",
        .method = HTTP_POST,
        .handler = api_auth_logout_handler,
    };
    REGISTER_URI(auth_logout_uri);

    httpd_uri_t auth_status_uri = {
        .uri = "/api/auth/status",
        .method = HTTP_GET,
        .handler = api_auth_status_handler,
    };
    REGISTER_URI(auth_status_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    REGISTER_URI(status_uri);

    httpd_uri_t sensors_uri = {
        .uri = "/api/sensors",
        .method = HTTP_GET,
        .handler = api_sensors_get_handler,
    };
    REGISTER_URI(sensors_uri);

    httpd_uri_t rescan_uri = {
        .uri = "/api/sensors/rescan",
        .method = HTTP_POST,
        .handler = api_sensors_rescan_handler,
    };
    REGISTER_URI(rescan_uri);

    httpd_uri_t error_stats_reset_uri = {
        .uri = "/api/sensors/error-stats/reset",
        .method = HTTP_POST,
        .handler = api_error_stats_reset_handler,
    };
    REGISTER_URI(error_stats_reset_uri);

    httpd_uri_t sensor_name_uri = {
        .uri = "/api/sensors/*",
        .method = HTTP_POST,
        .handler = api_sensor_name_handler,
    };
    REGISTER_URI(sensor_name_uri);

    httpd_uri_t ota_check_uri = {
        .uri = "/api/ota/check",
        .method = HTTP_POST,
        .handler = api_ota_check_handler,
    };
    REGISTER_URI(ota_check_uri);

    httpd_uri_t ota_channel_get_uri = {
        .uri = "/api/ota/channel",
        .method = HTTP_GET,
        .handler = api_ota_channel_get_handler,
    };
    REGISTER_URI(ota_channel_get_uri);

    httpd_uri_t ota_channel_post_uri = {
        .uri = "/api/ota/channel",
        .method = HTTP_POST,
        .handler = api_ota_channel_post_handler,
    };
    REGISTER_URI(ota_channel_post_uri);

    httpd_uri_t ota_status_uri = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = api_ota_status_handler,
    };
    REGISTER_URI(ota_status_uri);

    httpd_uri_t ota_update_uri = {
        .uri = "/api/ota/update",
        .method = HTTP_POST,
        .handler = api_ota_update_handler,
    };
    REGISTER_URI(ota_update_uri);

    httpd_uri_t ota_upload_uri = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = api_ota_upload_handler,
    };
    REGISTER_URI(ota_upload_uri);

    /* Configuration page */
    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    REGISTER_URI(config_uri);

    /* WiFi scan endpoint */
    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
    };
    REGISTER_URI(wifi_scan_uri);

    /* Log viewer endpoints */
    httpd_uri_t logs_get_uri = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_get_handler,
    };
    REGISTER_URI(logs_get_uri);

    httpd_uri_t logs_clear_uri = {
        .uri = "/api/logs/clear",
        .method = HTTP_POST,
        .handler = api_logs_clear_handler,
    };
    REGISTER_URI(logs_clear_uri);

    httpd_uri_t logs_level_get_uri = {
        .uri = "/api/logs/level",
        .method = HTTP_GET,
        .handler = api_logs_level_get_handler,
    };
    REGISTER_URI(logs_level_get_uri);

    httpd_uri_t logs_level_post_uri = {
        .uri = "/api/logs/level",
        .method = HTTP_POST,
        .handler = api_logs_level_post_handler,
    };
    REGISTER_URI(logs_level_post_uri);

    /* WiFi config endpoints */
    httpd_uri_t wifi_config_get_uri = {
        .uri = "/api/config/wifi",
        .method = HTTP_GET,
        .handler = api_config_wifi_get_handler,
    };
    REGISTER_URI(wifi_config_get_uri);

    httpd_uri_t wifi_config_post_uri = {
        .uri = "/api/config/wifi",
        .method = HTTP_POST,
        .handler = api_config_wifi_post_handler,
    };
    REGISTER_URI(wifi_config_post_uri);

    /* MQTT config endpoints */
    httpd_uri_t mqtt_config_get_uri = {
        .uri = "/api/config/mqtt",
        .method = HTTP_GET,
        .handler = api_config_mqtt_get_handler,
    };
    REGISTER_URI(mqtt_config_get_uri);

    httpd_uri_t mqtt_config_post_uri = {
        .uri = "/api/config/mqtt",
        .method = HTTP_POST,
        .handler = api_config_mqtt_post_handler,
    };
    REGISTER_URI(mqtt_config_post_uri);

    httpd_uri_t mqtt_reconnect_uri = {
        .uri = "/api/mqtt/reconnect",
        .method = HTTP_POST,
        .handler = api_mqtt_reconnect_handler,
    };
    REGISTER_URI(mqtt_reconnect_uri);

    /* Sensor config endpoints */
    httpd_uri_t sensor_config_get_uri = {
        .uri = "/api/config/sensor",
        .method = HTTP_GET,
        .handler = api_config_sensor_get_handler,
    };
    REGISTER_URI(sensor_config_get_uri);

    httpd_uri_t sensor_config_post_uri = {
        .uri = "/api/config/sensor",
        .method = HTTP_POST,
        .handler = api_config_sensor_post_handler,
    };
    REGISTER_URI(sensor_config_post_uri);

    /* System endpoints */
    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = api_system_restart_handler,
    };
    REGISTER_URI(system_restart_uri);

    httpd_uri_t factory_reset_uri = {
        .uri = "/api/system/factory-reset",
        .method = HTTP_POST,
        .handler = api_system_factory_reset_handler,
    };
    REGISTER_URI(factory_reset_uri);

    /* Backup / restore endpoints */
    httpd_uri_t backup_get_uri = {
        .uri = "/api/backup",
        .method = HTTP_GET,
        .handler = api_backup_get_handler,
    };
    REGISTER_URI(backup_get_uri);

    httpd_uri_t backup_restore_uri = {
        .uri = "/api/backup/restore",
        .method = HTTP_POST,
        .handler = api_backup_restore_post_handler,
    };
    REGISTER_URI(backup_restore_uri);

    /* Auth config endpoints */
    httpd_uri_t auth_config_get_uri = {
        .uri = "/api/config/auth",
        .method = HTTP_GET,
        .handler = api_config_auth_get_handler,
    };
    REGISTER_URI(auth_config_get_uri);

    httpd_uri_t auth_config_post_uri = {
        .uri = "/api/config/auth",
        .method = HTTP_POST,
        .handler = api_config_auth_post_handler,
    };
    REGISTER_URI(auth_config_post_uri);

    httpd_uri_t auth_regenerate_key_uri = {
        .uri = "/api/config/auth/regenerate-key",
        .method = HTTP_POST,
        .handler = api_config_auth_regenerate_key_handler,
    };
    REGISTER_URI(auth_regenerate_key_uri);

    ESP_LOGD(TAG, "Web server started");
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
