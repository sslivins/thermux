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
    int64_t last_attempt_time;                 /**< Uptime (ms) of the last read attempt, 0 if never */
} managed_sensor_t;

/**
 * @brief Outcome of the most recent read cycle
 */
typedef enum {
    SENSOR_CYCLE_OK = 0,          /**< Every sensor read successfully */
    SENSOR_CYCLE_PARTIAL = 1,     /**< Some sensors failed */
    SENSOR_CYCLE_FAILED = 2,      /**< No sensor read successfully (bus failure, lock timeout, ...) */
    SENSOR_CYCLE_NO_SENSORS = 3,  /**< No sensors discovered */
} sensor_cycle_result_t;

/**
 * @brief Read-cycle bookkeeping, updated after every scheduled read attempt
 */
typedef struct {
    uint32_t cycle_count;              /**< Increments after every attempted cycle (wraps) */
    sensor_cycle_result_t last_result; /**< Result of the most recent cycle */
    int64_t last_cycle_time;           /**< Uptime (ms) when the last cycle finished, 0 if none */
} sensor_cycle_info_t;

/**
 * @brief Initialize sensor manager and discover sensors
 */
esp_err_t sensor_manager_init(void);

/**
 * @brief Re-scan for sensors (hot-plug support)
 *
 * Serialized with read cycles, so a read never pairs the old registry with
 * the newly scanned device order.
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
 * @brief Copy the current sensor registry
 *
 * Returns a heap-allocated copy that the caller owns and must free(). The
 * copy is consistent (taken under the registry lock) and stays valid across
 * rescans.
 *
 * @param count Output: number of sensors in the copy
 * @return Array of @p count sensors, or NULL if there are none or allocation failed
 */
managed_sensor_t *sensor_manager_snapshot(int *count);

/**
 * @brief Get read-cycle bookkeeping
 */
void sensor_manager_get_cycle_info(sensor_cycle_info_t *out);

/**
 * @brief Set friendly name for a sensor
 * @param address_str Sensor address as hex string
 * @param friendly_name Friendly name to set
 */
esp_err_t sensor_manager_set_friendly_name(const char *address_str, const char *friendly_name);

/**
 * @brief Copy a single sensor by address string
 * @param address_str Sensor address as hex string
 * @param out Output copy of the sensor
 * @return ESP_OK, or ESP_ERR_NOT_FOUND if no sensor has that address
 */
esp_err_t sensor_manager_get_sensor(const char *address_str, managed_sensor_t *out);

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
