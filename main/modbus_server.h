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

/** Copy of the slot table */
void modbus_server_get_slots(modbus_slot_table_t *out);

/** Registers for every slot exactly as currently served (one consistent image).
 *  Slots whose status is MB_STATUS_UNASSIGNED have no sensor. */
void modbus_server_get_slot_regs(uint16_t status[MODBUS_SLOT_COUNT],
                                 uint16_t temp[MODBUS_SLOT_COUNT],
                                 uint16_t rom[MB_REG_ROM_COUNT]);

/**
 * @brief Move the sensor in slot @p from to slot @p to (swapping if occupied)
 *
 * Saved to NVS and applied to the registers before returning.
 *
 * @param[out] op Why the move was refused (may be NULL)
 * @return ESP_OK, ESP_ERR_INVALID_ARG if refused (see @p op), or an NVS error
 */
esp_err_t modbus_server_move_slot(int from, int to, modbus_slot_op_t *op);

/**
 * @brief Release a slot whose sensor is no longer on the bus
 *
 * @param[out] op Why the release was refused (may be NULL)
 * @return ESP_OK, ESP_ERR_INVALID_ARG if refused (see @p op), or an NVS error
 */
esp_err_t modbus_server_release_slot(int slot, modbus_slot_op_t *op);

/**
 * @brief Save settings and/or a slot table from a backup
 *
 * Settings are only written to NVS and take effect after the restart that
 * follows a restore. The slot table replaces the current one immediately so
 * a read cycle can't write the old table back before the restart.
 *
 * @param cfg   Settings to save, or NULL to leave them alone
 * @param slots Slot table to use, or NULL to leave it alone
 */
esp_err_t modbus_server_restore(const modbus_config_t *cfg, const modbus_slot_table_t *slots);

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
