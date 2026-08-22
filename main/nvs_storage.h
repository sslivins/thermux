/**
 * @file nvs_storage.h
 * @brief NVS storage interface for persistent configuration
 */

#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_FRIENDLY_NAME_LEN 32
#define SENSOR_ADDRESS_LEN 8

/**
 * @brief Initialize NVS storage subsystem
 */
esp_err_t nvs_storage_init(void);

/**
 * @brief Save a sensor's friendly name
 * @param sensor_address 8-byte sensor ROM address
 * @param friendly_name Friendly name string (max 32 chars)
 */
esp_err_t nvs_storage_save_sensor_name(const uint8_t *sensor_address, const char *friendly_name);

/**
 * @brief Load a sensor's friendly name
 * @param sensor_address 8-byte sensor ROM address
 * @param friendly_name Buffer to store friendly name
 * @param max_len Maximum length of buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no name saved
 */
esp_err_t nvs_storage_load_sensor_name(const uint8_t *sensor_address, char *friendly_name, size_t max_len);

/**
 * @brief Delete a sensor's friendly name
 * @param sensor_address 8-byte sensor ROM address
 */
esp_err_t nvs_storage_delete_sensor_name(const uint8_t *sensor_address);

/**
 * @brief Callback invoked once per saved sensor name during enumeration
 * @param serial_hex 12-char hex string identifying the sensor (ROM bytes 1-6,
 *                   i.e. the internal NVS key without the "s_" prefix)
 * @param friendly_name The saved friendly name
 * @param ctx Opaque context pointer passed through from the caller
 */
typedef void (*nvs_storage_sensor_name_cb_t)(const char *serial_hex, const char *friendly_name, void *ctx);

/**
 * @brief Enumerate all saved sensor friendly names, including ones for
 * sensors that are not currently connected/detected. Used for backup export.
 * @param cb Callback invoked once per saved name
 * @param ctx Opaque context pointer passed through to the callback
 */
esp_err_t nvs_storage_enumerate_sensor_names(nvs_storage_sensor_name_cb_t cb, void *ctx);

/**
 * @brief Save a sensor friendly name directly by its serial key (as returned
 * by nvs_storage_enumerate_sensor_names), bypassing the need for the raw
 * 8-byte ROM address. Used to restore names for sensors that may not be
 * currently connected.
 * @param serial_hex 12-char hex string identifying the sensor (ROM bytes 1-6)
 * @param friendly_name Friendly name string (max 32 chars)
 */
esp_err_t nvs_storage_save_sensor_name_by_serial(const char *serial_hex, const char *friendly_name);

/**
 * @brief Save MQTT configuration
 */
esp_err_t nvs_storage_save_mqtt_config(const char *broker_uri, const char *username, const char *password);

/**
 * @brief Load MQTT configuration
 */
esp_err_t nvs_storage_load_mqtt_config(char *broker_uri, size_t uri_len,
                                        char *username, size_t user_len,
                                        char *password, size_t pass_len);

/**
 * @brief Save WiFi credentials
 */
esp_err_t nvs_storage_save_wifi_config(const char *ssid, const char *password);

/**
 * @brief Load WiFi credentials
 */
esp_err_t nvs_storage_load_wifi_config(char *ssid, size_t ssid_len,
                                        char *password, size_t pass_len);

/**
 * @brief Factory reset - erase all stored configuration
 */
esp_err_t nvs_storage_factory_reset(void);

/**
 * @brief Save sensor timing and resolution settings
 * @param read_interval_ms Sensor read interval in milliseconds
 * @param publish_interval_ms MQTT publish interval in milliseconds
 * @param resolution Sensor resolution (9-12 bits)
 */
esp_err_t nvs_storage_save_sensor_settings(uint32_t read_interval_ms, uint32_t publish_interval_ms, uint8_t resolution);

/**
 * @brief Load sensor timing and resolution settings
 * @param read_interval_ms Output: Sensor read interval in milliseconds
 * @param publish_interval_ms Output: MQTT publish interval in milliseconds
 * @param resolution Output: Sensor resolution (9-12 bits)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t nvs_storage_load_sensor_settings(uint32_t *read_interval_ms, uint32_t *publish_interval_ms, uint8_t *resolution);

/**
 * @brief Save web authentication settings
 * @param enabled Whether auth is enabled
 * @param username Username (max 32 chars)
 * @param password Password (max 64 chars)
 * @param api_key API key (max 64 chars), can be NULL
 */
esp_err_t nvs_storage_save_auth_config(bool enabled, const char *username, const char *password, const char *api_key);

/**
 * @brief Load web authentication settings
 * @param enabled Output: Whether auth is enabled
 * @param username Output: Username buffer
 * @param username_len Username buffer size
 * @param password Output: Password buffer
 * @param password_len Password buffer size
 * @param api_key Output: API key buffer (can be NULL)
 * @param api_key_len API key buffer size
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t nvs_storage_load_auth_config(bool *enabled, char *username, size_t username_len,
                                        char *password, size_t password_len,
                                        char *api_key, size_t api_key_len);

/**
 * @brief Save whether the cloud OTA update check should include GitHub
 * pre-releases (beta channel), not just the latest full release.
 */
esp_err_t nvs_storage_save_ota_prerelease_channel(bool enabled);

/**
 * @brief Load whether the cloud OTA update check should include GitHub
 * pre-releases (beta channel).
 * @param enabled Output: whether the pre-release channel is enabled
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if never configured (caller
 *         should default to false/disabled in that case)
 */
esp_err_t nvs_storage_load_ota_prerelease_channel(bool *enabled);

#endif /* NVS_STORAGE_H */
