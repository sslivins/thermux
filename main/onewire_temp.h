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
    bool genuine;                         /**< True if the sensor's ROM/scratchpad pattern
                                                matches a genuine Maxim/Analog Devices DS18B20.
                                                False indicates a likely clone/counterfeit chip. */
    bool genuine_check_ok;                /**< True if the genuineness check was able to read/verify
                                                the scratchpad (bus + CRC ok). False means the check
                                                could not be completed and "genuine" defaulted to true. */
    uint8_t count_remain;                  /**< Raw scratchpad byte 6 (COUNT_REMAIN), used in the
                                                genuineness check. Only valid if genuine_check_ok. */
    uint8_t count_per_c;                   /**< Raw scratchpad byte 7 (COUNT_PER_C), used in the
                                                genuineness check. Only valid if genuine_check_ok. */
} onewire_sensor_t;

typedef struct {
    uint32_t recent_total_reads;
    uint32_t recent_failed_reads;
    uint32_t consecutive_failed_cycles;
    uint64_t seconds_since_last_success;
    bool has_successful_read;
} onewire_bus_health_t;

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
 * @brief Get recent and cycle-level bus health statistics
 */
void onewire_temp_get_bus_health(onewire_bus_health_t *health);

/**
 * @brief Reset bus error statistics counters to zero
 */
void onewire_temp_reset_error_stats(void);

#endif /* ONEWIRE_TEMP_H */
