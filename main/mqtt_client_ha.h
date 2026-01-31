/**
 * @file mqtt_client_ha.h
 * @brief MQTT client with Home Assistant discovery support
 */

#ifndef MQTT_CLIENT_HA_H
#define MQTT_CLIENT_HA_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize MQTT client
 */
esp_err_t mqtt_ha_init(void);

/**
 * @brief Start MQTT client connection
 */
esp_err_t mqtt_ha_start(void);

/**
 * @brief Stop MQTT client
 */
esp_err_t mqtt_ha_stop(void);

/**
 * @brief Check if MQTT is connected
 */
bool mqtt_ha_is_connected(void);

/**
 * @brief Publish temperature reading
 * @param sensor_id Unique sensor ID (address string)
 * @param friendly_name Display name for the sensor
 * @param temperature Temperature value in Celsius
 */
esp_err_t mqtt_ha_publish_temperature(const char *sensor_id, const char *friendly_name, float temperature);

/**
 * @brief Register sensor with Home Assistant discovery
 * @param sensor_id Unique sensor ID (address string)
 * @param friendly_name Display name for the sensor
 */
esp_err_t mqtt_ha_register_sensor(const char *sensor_id, const char *friendly_name);

/**
 * @brief Publish device status
 * @param online True if device is online
 */
esp_err_t mqtt_ha_publish_status(bool online);

/**
 * @brief Publish all sensor discoveries to Home Assistant
 */
esp_err_t mqtt_ha_publish_discovery_all(void);

/**
 * @brief Register diagnostic entities with Home Assistant
 * (Ethernet status, WiFi status, IP address)
 */
esp_err_t mqtt_ha_register_diagnostic_entities(void);

/**
 * @brief Publish diagnostic data (network status)
 */
esp_err_t mqtt_ha_publish_diagnostics(void);

#endif /* MQTT_CLIENT_HA_H */
