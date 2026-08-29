/**
 * @file log_persist.h
 * @brief Persists the RAM log ring buffer to the unused "storage" flash
 *        partition so log history survives reboots/crashes.
 */

#ifndef LOG_PERSIST_H
#define LOG_PERSIST_H

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Locate the persistence partition and recover the most recent
 *        snapshot written before the last reboot (if any).
 *
 * @param out_buffer Buffer to receive the recovered log text
 * @param out_size   Size of out_buffer
 * @param out_len    Set to the number of bytes recovered (0 if none/invalid)
 * @return ESP_OK if the partition was found (even if nothing was recovered),
 *         ESP_ERR_NOT_FOUND if the partition doesn't exist in the partition table
 */
esp_err_t log_persist_init(char *out_buffer, size_t out_size, size_t *out_len);

/**
 * @brief Snapshot the current live log buffer to the next flash slot.
 *        Safe to call repeatedly (rotates across slots to spread flash wear).
 *        Matches the shutdown_handler_t signature so it can be registered
 *        with esp_register_shutdown_handler().
 */
void log_persist_flush(void);

/**
 * @brief Start a periodic background timer that calls log_persist_flush().
 * @param interval_sec How often to flush, in seconds
 */
void log_persist_start_periodic_flush(int interval_sec);

#endif /* LOG_PERSIST_H */
