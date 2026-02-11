/**
 * @file onewire_temp.h
 * @brief 1-Wire DS18B20 temperature sensor driver
 */

#ifndef ONEWIRE_TEMP_H
#define ONEWIRE_TEMP_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define ONEWIRE_ROM_SIZE 8

/**
 * @brief DS18B20 sensor data structure
 */
typedef struct {
    uint8_t address[ONEWIRE_ROM_SIZE];  /**< 64-bit ROM address */
    float temperature;                    /**< Last read temperature in Celsius */
    bool valid;                           /**< True if last reading was valid */
    int64_t last_read_time;              /**< Timestamp of last reading */
    uint32_t total_reads;                /**< Total read attempts for this sensor */
    uint32_t failed_reads;               /**< Failed read count for this sensor */
} onewire_sensor_t;

/**
 * @brief Initialize 1-Wire bus
 * @param gpio_num GPIO pin connected to 1-Wire data line
 */
esp_err_t onewire_temp_init(int gpio_num);

/**
 * @brief Scan bus and discover all connected sensors
 * @param sensors Array to store discovered sensors
 * @param max_sensors Maximum number of sensors to discover
 * @param found_count Output: actual number of sensors found
 */
esp_err_t onewire_temp_scan(onewire_sensor_t *sensors, int max_sensors, int *found_count);

/**
 * @brief Read temperature from a specific sensor by index
 * @param sensor Sensor to update with reading
 * @param index Index of sensor in discovered array (0-based)
 */
esp_err_t onewire_temp_read(onewire_sensor_t *sensor, int index);

/**
 * @brief Read temperature from all sensors
 * @param sensors Array of sensors to read
 * @param sensor_count Number of sensors in array
 */
esp_err_t onewire_temp_read_all(onewire_sensor_t *sensors, int sensor_count);

/**
 * @brief Convert sensor address to hex string
 * @param address 8-byte sensor address
 * @param str Output string buffer (must be at least 17 bytes)
 */
void onewire_address_to_string(const uint8_t *address, char *str);

/**
 * @brief Get resolution in bits (9-12)
 */
int onewire_temp_get_resolution(void);

/**
 * @brief Set resolution (9-12 bits)
 */
esp_err_t onewire_temp_set_resolution(int bits);

/**
 * @brief Get bus error statistics
 * @param total_reads Output: total individual sensor reads attempted
 * @param failed_reads Output: number of failed reads (CRC errors, etc.)
 */
void onewire_temp_get_error_stats(uint32_t *total_reads, uint32_t *failed_reads);

/**
 * @brief Reset bus error statistics counters to zero
 */
void onewire_temp_reset_error_stats(void);

#endif /* ONEWIRE_TEMP_H */
