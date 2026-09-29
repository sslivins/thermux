/**
 * @file modbus_server.h
 * @brief Read-only Modbus TCP server exposing sensor readings (FC 0x04)
 *
 * Register layout and slot rules are in modbus_map.h.
 */

#ifndef MODBUS_SERVER_H
#define MODBUS_SERVER_H

#include "esp_err.h"
#include "modbus_map.h"
#include <stdbool.h>
#include <stdint.h>

#define MODBUS_DEFAULT_PORT     502
#define MODBUS_DEFAULT_UNIT_ID  1
#define MODBUS_MAX_CLIENTS      3

typedef struct {
    bool enabled;
    uint16_t port;
    uint8_t unit_id;
} modbus_config_t;

typedef struct {
    modbus_config_t config;
    bool running;
    char last_error[64];      /**< Empty when the last start succeeded */
    uint32_t requests;        /**< Register reads served since the server started */
    int64_t last_request_ms;  /**< Uptime of the last served read, 0 = none */
    int slots_assigned;
    int sensors_without_slot; /**< Present sensors left out because the slot table is full */
} modbus_status_t;

/**
 * @brief Load settings and the slot table, and start the server if enabled
 *
 * Call after sensor_manager_init() and once the network stack is up.
 */
esp_err_t modbus_server_init(void);

/**
 * @brief Refresh the register image from the sensor registry
 *
 * Called after every read cycle. Also assigns slots to newly seen sensors.
 */
void modbus_server_update(void);

/**
 * @brief Validate, apply and persist new settings
 *
 * The server is restarted when running settings change. Settings are only
 * saved if the new configuration starts; otherwise the previous one is
 * restored and an error returned.
 */
esp_err_t modbus_server_apply_config(const modbus_config_t *cfg);

/** Current settings and runtime state */
void modbus_server_get_status(modbus_status_t *out);

/** Slot index assigned to @p rom, or -1 */
int modbus_server_get_slot(const uint8_t *rom);

#endif /* MODBUS_SERVER_H */
