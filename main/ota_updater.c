/**
 * @file ota_updater.c
 * @brief OTA firmware updates from GitHub Releases
 * 
 * This module checks GitHub releases for new firmware versions and
 * downloads/installs updates using ESP-IDF's HTTPS OTA functionality.
 */

#include "ota_updater.h"
#include "version_utils.h"
#include "nvs_storage.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "release_scan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_updater";

/* Forward declaration */
extern const char *APP_VERSION;

/* GitHub API URL for the latest full (non-draft, non-prerelease) release.
 * This is what GitHub's own /releases/latest semantics guarantee - it never
 * returns a pre-release, regardless of tag naming. */
#define GITHUB_API_URL_LATEST "https://api.github.com/repos/%s/%s/releases/latest"
/* GitHub API URL for the release list (newest first), used when the
 * pre-release/beta channel is enabled so drafts+prereleases are visible.
 * The response is scanned as it streams in (see release_scan.c), so its size
 * does not affect RAM; per_page=2 just keeps the download short while leaving
 * one slot to skip a draft. */
#define GITHUB_API_URL_LIST "https://api.github.com/repos/%s/%s/releases?per_page=2"

static char s_latest_version[32] = {0};
static char s_download_url[512] = {0};
static bool s_update_available = false;
static bool s_include_prerelease = false;
static bool s_latest_is_prerelease = false;

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

/* Per-check state handed to the HTTP event handler. */
typedef struct {
    release_scan_t scan;
    int bytes;
} release_check_ctx_t;

/**
 * @brief HTTP event handler for the GitHub API request
 *
 * Feeds each body chunk straight into the release scanner instead of
 * buffering the whole response. Buffering it (9-18 KB) and then parsing it
 * with cJSON needed several times that in free heap on top of the TLS
 * session, which a unit running BACnet, MQTT and ten sensors no longer had.
 * esp_http_client has already removed any chunked transfer encoding.
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    release_check_ctx_t *ctx = (release_check_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx != NULL &&
        esp_http_client_get_status_code(evt->client) == 200) {
        release_scan_feed(&ctx->scan, (const char *)evt->data, evt->data_len);
        ctx->bytes += evt->data_len;
    }
    return ESP_OK;
}

esp_err_t ota_updater_init(void)
{
    ESP_LOGD(TAG, "OTA updater initialized");
    ESP_LOGD(TAG, "Current version: %s", APP_VERSION);
    ESP_LOGD(TAG, "GitHub repo: %s/%s", CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
    
    s_update_available = false;
    s_latest_version[0] = '\0';
    s_download_url[0] = '\0';

    /* Load persisted pre-release channel preference; default to disabled
     * (stable-only) if never configured. */
    bool prerelease = false;
    if (nvs_storage_load_ota_prerelease_channel(&prerelease) == ESP_OK) {
        s_include_prerelease = prerelease;
    }
    ESP_LOGD(TAG, "Pre-release channel: %s", s_include_prerelease ? "enabled" : "disabled");
    
    return ESP_OK;
}

void ota_updater_set_include_prerelease(bool enabled)
{
    s_include_prerelease = enabled;
}

bool ota_updater_get_include_prerelease(void)
{
    return s_include_prerelease;
}

/* Held for each GitHub check attempt and for the whole firmware download, so
 * the two never run TLS sessions side by side: the 60 s boot check starting
 * during a download left ~6 KB of heap, which stalled the download and made
 * the check fail with out-of-memory. */
static SemaphoreHandle_t github_lock(void)
{
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t lock = NULL;
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&buf);
    }
    return lock;
}

/* Retry configuration */
#define OTA_CHECK_MAX_RETRIES   3
#define OTA_CHECK_RETRY_DELAY_MS 2000

/**
 * @brief Internal function to perform single OTA check attempt
 */
static esp_err_t ota_check_for_update_internal(void)
{
    /* Build GitHub API URL. When the pre-release/beta channel is enabled,
     * query the release list (newest first) so drafts+prereleases are
     * visible; GitHub's /releases/latest endpoint always skips them. */
    char url[256];
    if (s_include_prerelease) {
        snprintf(url, sizeof(url), GITHUB_API_URL_LIST, CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
    } else {
        snprintf(url, sizeof(url), GITHUB_API_URL_LATEST, CONFIG_GITHUB_OWNER, CONFIG_GITHUB_REPO);
    }
    ESP_LOGD(TAG, "API URL: %s", url);

    /* Build the result in locals and publish it only if the whole check
     * succeeds, so a check in flight (or a failed one) never hides an update
     * found earlier: clearing the flag up front made "Update" answer "No
     * update available" while the 60 s boot check or a daily check ran. */
    char latest[sizeof(s_latest_version)] = {0};
    char dl_url[sizeof(s_download_url)] = {0};
    bool available = false;
    bool is_prerelease = false;

    release_check_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Out of memory for update check");
        return ESP_ERR_NO_MEM;
    }
    /* Releases also ship bootloader.bin and partition-table.bin, which must
     * NOT be flashed as the app: doing so corrupts the OTA slot and forces a
     * bootloader fallback to the factory image. The scanner prefers the exact
     * app asset "<repo>.bin" and otherwise falls back to any other .bin. */
    char expected_app[64];
    snprintf(expected_app, sizeof(expected_app), "%s.bin", CONFIG_GITHUB_REPO);
    release_scan_init(&ctx->scan, expected_app);
    ctx->bytes = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(ctx);
        return ESP_FAIL;
    }

    /* User-Agent is required by the GitHub API */
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Updater");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");

    ESP_LOGD(TAG, "Sending request to GitHub API...");
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGD(TAG, "HTTP response: status=%d, %d bytes", status, ctx->bytes);

        if (status != 200) {
            ESP_LOGE(TAG, "GitHub API returned status %d", status);
            err = ESP_FAIL;
        } else if (ctx->bytes == 0) {
            ESP_LOGE(TAG, "HTTP 200 but no response data received");
            err = ESP_FAIL;
        } else if (!release_scan_finish(&ctx->scan)) {
            /* Malformed or truncated JSON, or no non-draft release. Fail so
             * the retry logic in ota_check_for_update() kicks in instead of
             * reporting success with an empty/"unknown" version. */
            ESP_LOGW(TAG, "No usable release in GitHub response (%d bytes)", ctx->bytes);
            err = ESP_FAIL;
        } else {
            strncpy(latest, ctx->scan.tag, sizeof(latest) - 1);
            /* Lets the UI label a beta clearly. Always false in stable mode:
             * /releases/latest never returns a pre-release. */
            is_prerelease = ctx->scan.prerelease;
            ESP_LOGD(TAG, "Latest version: %s", latest);

            if (version_compare(latest, APP_VERSION) > 0) {
                if (ctx->scan.url[0] != '\0') {
                    available = true;
                    strncpy(dl_url, ctx->scan.url, sizeof(dl_url) - 1);
                    if (!ctx->scan.url_is_exact) {
                        ESP_LOGW(TAG, "App asset '%s' not found; using fallback: %s",
                                 expected_app, dl_url);
                    }
                    ESP_LOGI(TAG, "Update available: %s -> %s", APP_VERSION, latest);
                    ESP_LOGD(TAG, "Firmware URL: %s", dl_url);
                } else {
                    ESP_LOGE(TAG, "No suitable firmware .bin asset found in release %s", latest);
                }
            } else {
                ESP_LOGD(TAG, "Already up to date");
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    free(ctx);

    if (err == ESP_OK && s_update_state != OTA_UPDATE_DOWNLOADING) {
        memcpy(s_latest_version, latest, sizeof(s_latest_version));
        memcpy(s_download_url, dl_url, sizeof(s_download_url));
        s_latest_is_prerelease = is_prerelease;
        s_update_available = available;
    }

    esp_http_client_cleanup(client);
    return err;
}

/**
 * @brief Check for firmware updates with retry
 */
esp_err_t ota_check_for_update(void)
{
    ESP_LOGD(TAG, "Checking for updates...");
    
    esp_err_t err = ESP_FAIL;
    int retry_delay_ms = OTA_CHECK_RETRY_DELAY_MS;
    
    for (int attempt = 1; attempt <= OTA_CHECK_MAX_RETRIES; attempt++) {
        if (s_update_state == OTA_UPDATE_DOWNLOADING) {
            ESP_LOGI(TAG, "Skipping update check: firmware download in progress");
            return ESP_ERR_INVALID_STATE;
        }
        if (xSemaphoreTake(github_lock(), pdMS_TO_TICKS(30000)) != pdTRUE) {
            ESP_LOGW(TAG, "Skipping update check: GitHub connection busy");
            return ESP_ERR_TIMEOUT;
        }
        if (s_update_state == OTA_UPDATE_DOWNLOADING) {
            xSemaphoreGive(github_lock());
            ESP_LOGI(TAG, "Skipping update check: firmware download in progress");
            return ESP_ERR_INVALID_STATE;
        }
        err = ota_check_for_update_internal();
        xSemaphoreGive(github_lock());

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
    
    ESP_LOGD(TAG, "OTA check complete");
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
    
    /* The previous result stays visible until this check succeeds */
    s_check_state = OTA_CHECK_IN_PROGRESS;
    
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

bool ota_is_latest_prerelease(void)
{
    return s_latest_is_prerelease;
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
    if (s_download_url[0] == '\0') {
        ESP_LOGE(TAG, "OTA update aborted: no firmware download URL set");
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Starting OTA update from: %s (free heap %u, largest block %u)", s_download_url,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    s_update_state = OTA_UPDATE_DOWNLOADING;
    s_download_progress = 0;
    s_download_total = 0;
    s_download_received = 0;

    /* Wait for an update check that is mid-request; new ones now skip. */
    if (xSemaphoreTake(github_lock(), pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "OTA update aborted: update check did not finish");
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    
    /* Heap is tight on a busy unit (BACnet + MQTT + many sensors), and the
     * TLS handshake with GitHub's download host verifies an RSA-4096 root,
     * which needs several KB of working memory on top of the TLS buffers.
     * So: one streamed GET (partial/range downloads opened a new TLS session
     * for every 64 KB, ~20 handshakes per image), and 4 KB buffers, since
     * esp_https_ota allocates a second buffer of the same size. */
    esp_http_client_config_t config = {
        .url = s_download_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    ESP_LOGD(TAG, "Connecting to GitHub...");
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s (free heap %u, largest block %u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        xSemaphoreGive(github_lock());
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    
    /* Get total image size - may be 0 if server doesn't provide Content-Length */
    int real_total = esp_https_ota_get_image_size(ota_handle);
    ESP_LOGD(TAG, "Image size from server: %d bytes (0 means unknown)", real_total);
    
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
            ESP_LOGD(TAG, "Download: %d KB / %d KB (%d%%)", 
                     s_download_received / 1024, s_download_total / 1024, s_download_progress);
            last_logged_pct = s_download_progress;
        }
        
        /* Yield to allow HTTP server to respond to status requests */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s (free heap %u, largest block %u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        esp_https_ota_abort(ota_handle);
        xSemaphoreGive(github_lock());
        s_update_state = OTA_UPDATE_FAILED;
        vTaskDelete(NULL);
        return;
    }
    
    /* Verify and finish */
    if (esp_https_ota_is_complete_data_received(ota_handle) != true) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(ota_handle);
        xSemaphoreGive(github_lock());
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
        xSemaphoreGive(github_lock());
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
