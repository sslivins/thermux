/**
 * @file mqtt_client_ha.h
 * @brief MQTT client with Home Assistant discovery support
 */

#ifndef MQTT_CLIENT_HA_H
#define MQTT_CLIENT_HA_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Snapshot of the MQTT connection status for diagnostics/UI
 */
typedef struct {
    bool connected;              /*!< True once CONNECTED, false otherwise */
    bool connecting;             /*!< True while an attempt is in flight / retrying */
    char last_error[96];         /*!< Human-readable last connect error ("" if none) */
    int64_t last_error_uptime_s; /*!< Device uptime (s) when last_error was recorded, 0 if none */
} mqtt_ha_status_t;

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
 * @brief Tear down and re-create the MQTT client from the latest saved
 * config, then start it. Applies changed broker/credentials without a
 * reboot and forces an immediate reconnect attempt.
 */
esp_err_t mqtt_ha_reconnect(void);

/**
 * @brief Check if MQTT is connected
 */
bool mqtt_ha_is_connected(void);

/**
 * @brief Get a snapshot of the current MQTT connection status
 * @param out Pointer to a struct that receives the status (must not be NULL)
 */
void mqtt_ha_get_status(mqtt_ha_status_t *out);

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

/**
 * @brief Register the Home Assistant firmware "update" entity (MQTT discovery)
 *
 * Exposes an HA `update` entity with an Install button, current/latest version
 * and a live progress bar, backed by the ota_updater module.
 */
esp_err_t mqtt_ha_register_update_entity(void);
/**
 * @brief Publish the firmware update entity state
 * @param installed_version Currently running firmware version
 * @param latest_version Latest available version (empty/NULL => reported equal to installed)
 * @param release_url Optional link to the release notes (empty/NULL to omit)
 * @param update_percentage Download progress 0-100 while installing, or <0 when idle
 */
esp_err_t mqtt_ha_publish_update_state(const char *installed_version,
                                       const char *latest_version,
                                       const char *release_url,
                                       int update_percentage);

/**
 * @brief Register the Home Assistant "Rescan Sensors" button entity (MQTT discovery)
 *
 * Exposes an HA `button` entity that triggers sensor_manager_rescan() when
 * pressed, so a 1-Wire bus rescan can be initiated from Home Assistant
 * without needing to open the device's web UI.
 */
esp_err_t mqtt_ha_register_rescan_button(void);

/**
 * @brief Register the Home Assistant "1-Wire Resolution" select entity
 * (MQTT discovery)
 *
 * Exposes an HA `select` entity listing the DS18B20 resolution options
 * (9/10/11/12-bit) that lets the resolution be changed from Home Assistant
 * without needing to open the device's web UI. The choice is persisted to
 * NVS and applied immediately via onewire_temp_set_resolution().
 */
esp_err_t mqtt_ha_register_resolution_select(void);

/**
 * @brief Publish the current 1-Wire resolution state to Home Assistant
 */
esp_err_t mqtt_ha_publish_resolution_state(void);

/**
 * @brief Register HA `number` entities for read/publish intervals and the
 * "Restart Device" button (MQTT discovery)
 *
 * Lets the sensor read interval and MQTT publish interval be tuned directly
 * from Home Assistant (mirrors the web UI's sensor settings page), and
 * exposes a button that triggers a device restart.
 */
esp_err_t mqtt_ha_register_interval_numbers(void);
esp_err_t mqtt_ha_register_restart_button(void);

/**
 * @brief Publish the current read/publish interval values to Home Assistant
 */
esp_err_t mqtt_ha_publish_interval_state(void);

#endif /* MQTT_CLIENT_HA_H */
