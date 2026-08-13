/**
 * @file time_sync.h
 * @brief SNTP wall-clock time synchronization
 *
 * Starts SNTP once the network is up and caches the device's wall-clock boot
 * time the first time the clock is set. The boot time is what Home Assistant
 * needs to show a self-updating "Last Boot" timestamp sensor.
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start SNTP time synchronization (non-blocking)
 *
 * Safe to call once after the network is connected. The clock is set
 * asynchronously; use time_sync_get_boot_time() to know when it is ready.
 */
esp_err_t time_sync_init(void);

/**
 * @brief Whether the wall clock has been set at least once via SNTP
 */
bool time_sync_is_synced(void);

/**
 * @brief Get the device's wall-clock boot time
 *
 * @param out_boot_time Receives the boot time as a UTC epoch value
 * @return true if the clock has synced and a boot time is available,
 *         false if time is not yet known (out_boot_time is untouched)
 */
bool time_sync_get_boot_time(time_t *out_boot_time);

#endif /* TIME_SYNC_H */
