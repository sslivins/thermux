#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief EXPERIMENTAL. Run Stage A of the OTA-only partition migration
 * (drop dead `factory` partition, grow ota_0). Must be called while the
 * device is running from ota_1. On success this function does not return
 * (it reboots); on failure it returns an error and the device is left in
 * its original, still-bootable state (the partition table is only written
 * after all image staging is verified).
 *
 * See partition_migration.c for the full safety writeup. Test-device only
 * until proven via issue #48 - never call this against 192.168.10.127.
 *
 * @param stage_a_table_bin Raw bytes of the compiled Stage A partition table
 *                          (from partitions_stageA.csv, built via gen_esp32part.py).
 * @param table_len         Length of stage_a_table_bin, must be <= 0x1000.
 */
esp_err_t partition_migration_run_stage_a(const uint8_t *stage_a_table_bin, size_t table_len);

/**
 * @brief EXPERIMENTAL. Run Stage B of the OTA-only partition migration
 * (shrink ota_0 back down, grow ota_1 to match - final equal-size layout).
 * Must be called while the device is running from the Stage A ota_0.
 * Same success/failure semantics as partition_migration_run_stage_a().
 *
 * @param stage_b_table_bin Raw bytes of the compiled Stage B partition table
 *                          (from partitions_stageB.csv, built via gen_esp32part.py).
 * @param table_len         Length of stage_b_table_bin, must be <= 0x1000.
 */
esp_err_t partition_migration_run_stage_b(const uint8_t *stage_b_table_bin, size_t table_len);

#ifdef __cplusplus
}
#endif
