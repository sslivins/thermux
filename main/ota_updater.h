/**
 * @file ota_updater.h
 * @brief OTA firmware updates from GitHub Releases
 */

#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize OTA updater
 */
esp_err_t ota_updater_init(void);

/**
 * @brief Check GitHub releases for available update (blocking)
 * @note Use ota_check_for_update_async() from HTTP handlers to avoid stack overflow
 */
esp_err_t ota_check_for_update(void);

/**
 * @brief Start async check for updates (non-blocking)
 * @return ESP_OK if check started, ESP_ERR_INVALID_STATE if already in progress
 */
esp_err_t ota_check_for_update_async(void);

/**
 * @brief Check if async OTA check is in progress
 */
bool ota_check_in_progress(void);

/**
 * @brief Get async OTA check result
 * @return 1 = complete, 0 = in progress, -1 = failed
 */
int ota_get_check_result(void);

/**
 * @brief Check if an update is available
 */
bool ota_is_update_available(void);

/**
 * @brief Get latest available version string
 */
esp_err_t ota_get_latest_version(char *version, size_t max_len);

/**
 * @brief Start OTA update process
 * 
 * Downloads and installs the latest firmware, then restarts device
 */
esp_err_t ota_start_update(void);

/**
 * @brief Check if OTA update/download is in progress
 */
bool ota_update_in_progress(void);

/**
 * @brief Get current update state
 * @return 0=idle, 1=downloading, 2=complete (rebooting), -1=failed
 */
int ota_get_update_state(void);

/**
 * @brief Get current download progress (0-100)
 */
int ota_get_download_progress(void);

/**
 * @brief Get download statistics
 * @param received Bytes received so far (can be NULL)
 * @param total Total bytes to download (can be NULL)
 */
void ota_get_download_stats(int *received, int *total);

/**
 * @brief Get current firmware version
 */
const char* ota_get_current_version(void);

#endif /* OTA_UPDATER_H */
