/**
 * @file modbus_server.h
 * @brief Read-only Modbus TCP server exposing sensor readings (FC 0x04)
 *
 * Register layout and channel rules are in modbus_map.h.
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
    int channels_assigned;
    int sensors_without_channel; /**< Present sensors left out because the channel table is full */
} modbus_status_t;

/**
 * @brief Load settings and the channel table, and start the server if enabled
 *
 * Call after sensor_manager_init() and once the network stack is up.
 */
esp_err_t modbus_server_init(void);

/**
 * @brief Refresh the register image from the sensor registry
 *
 * Called after every read cycle. Also assigns channels to newly seen sensors.
 */
void modbus_server_update(void);

/** Copy of the channel table */
void modbus_server_get_channels(modbus_channel_table_t *out);

/** Registers for every channel exactly as currently served (one consistent image).
 *  Channels whose status is MB_STATUS_UNASSIGNED have no sensor. */
void modbus_server_get_channel_regs(uint16_t status[MODBUS_CHANNEL_COUNT],
                                 uint16_t temp[MODBUS_CHANNEL_COUNT],
                                 uint16_t rom[MB_REG_ROM_COUNT]);

/**
 * @brief Move the sensor in channel @p from to channel @p to (swapping if occupied)
 *
 * Saved to NVS and applied to the registers before returning.
 *
 * @param[out] op Why the move was refused (may be NULL)
 * @return ESP_OK, ESP_ERR_INVALID_ARG if refused (see @p op), or an NVS error
 */
esp_err_t modbus_server_move_channel(int from, int to, modbus_channel_op_t *op);

/**
 * @brief Release a channel whose sensor is no longer on the bus
 *
 * @param[out] op Why the release was refused (may be NULL)
 * @return ESP_OK, ESP_ERR_INVALID_ARG if refused (see @p op), or an NVS error
 */
esp_err_t modbus_server_release_channel(int channel, modbus_channel_op_t *op);

/**
 * @brief Save settings and/or a channel table from a backup
 *
 * Settings are only written to NVS and take effect after the restart that
 * follows a restore. The channel table replaces the current one immediately so
 * a read cycle can't write the old table back before the restart.
 *
 * @param cfg   Settings to save, or NULL to leave them alone
 * @param channels Channel table to use, or NULL to leave it alone
 */
esp_err_t modbus_server_restore(const modbus_config_t *cfg, const modbus_channel_table_t *channels);

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

/** Channel index assigned to @p rom, or -1 */
int modbus_server_get_channel(const uint8_t *rom);

#endif /* MODBUS_SERVER_H */
