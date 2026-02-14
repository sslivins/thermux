/**
 * @file sensor_manager.h
 * @brief Sensor registry and management with friendly names
 */

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "esp_err.h"
#include "onewire_temp.h"
#include <stdbool.h>

#define MAX_FRIENDLY_NAME_LEN 32

/**
 * @brief Managed sensor with friendly name
 */
typedef struct {
    onewire_sensor_t hw_sensor;               /**< Hardware sensor data */
    char friendly_name[MAX_FRIENDLY_NAME_LEN]; /**< User-assigned friendly name */
    bool has_friendly_name;                    /**< True if friendly name is set */
    char address_str[17];                      /**< Address as hex string */
} managed_sensor_t;

/**
 * @brief Initialize sensor manager and discover sensors
 */
esp_err_t sensor_manager_init(void);

/**
 * @brief Re-scan for sensors (hot-plug support)
 */
esp_err_t sensor_manager_rescan(void);

/**
 * @brief Read temperatures from all sensors
 */
esp_err_t sensor_manager_read_all(void);

/**
 * @brief Publish all sensor readings via MQTT
 */
esp_err_t sensor_manager_publish_all(void);

/**
 * @brief Get all managed sensors
 * @param count Output: number of sensors
 * @return Array of managed sensors (do not free)
 */
const managed_sensor_t* sensor_manager_get_sensors(int *count);

/**
 * @brief Set friendly name for a sensor
 * @param address_str Sensor address as hex string
 * @param friendly_name Friendly name to set
 */
esp_err_t sensor_manager_set_friendly_name(const char *address_str, const char *friendly_name);

/**
 * @brief Get friendly name for a sensor
 * @param address_str Sensor address as hex string
 * @return Friendly name or address string if no name set
 */
const char* sensor_manager_get_display_name(const char *address_str);

/**
 * @brief Get sensor by address string
 */
const managed_sensor_t* sensor_manager_get_sensor(const char *address_str);

/**
 * @brief Get number of sensors
 */
int sensor_manager_get_count(void);

/**
 * @brief Reset error stats for all sensors
 */
void sensor_manager_reset_all_error_stats(void);

/**
 * @brief Reset error stats for a specific sensor
 * @param address_str Sensor address as hex string
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if sensor not found
 */
esp_err_t sensor_manager_reset_sensor_error_stats(const char *address_str);

#endif /* SENSOR_MANAGER_H */
