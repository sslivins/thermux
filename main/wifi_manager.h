/**
 * @file wifi_manager.h
 * @brief WiFi connection management (fallback for when Ethernet unavailable)
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

/**
 * @brief Initialize WiFi subsystem
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Start WiFi connection
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Stop WiFi connection
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Check if WiFi is connected
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get WiFi IP address as string
 */
const char* wifi_manager_get_ip(void);

/**
 * @brief Set WiFi credentials (saves to NVS)
 */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/**
 * @brief Scan for available WiFi networks
 * @param ap_records Array to store scan results
 * @param max_records Maximum number of records to return
 * @param found_count Pointer to store actual count found
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t max_records, uint16_t *found_count);

#endif /* WIFI_MANAGER_H */
