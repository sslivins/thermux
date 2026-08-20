/**
 * @file partition_migration.c
 * @brief EXPERIMENTAL, test-device-only partition table migration (issue #48).
 *
 * Grows ota_0/ota_1 by reclaiming the dead `factory` partition, entirely
 * over OTA (no serial/USB). This is intentionally NOT used on the
 * production field device (192.168.10.127) until proven rock-solid on a
 * spare test device, including power-cycle fault injection at each risky
 * step. See issue #48 and the `experiment/partition-migration-test` branch.
 *
 * Two-stage design so the currently *executing* flash region is never
 * touched by an erase/write:
 *
 *   Stage A (must be running from ota_1):
 *     1. Write+verify the current app image into the OLD factory+ota_0
 *        region (0x20000, spans old factory 0x170000 + old ota_0 0x130000
 *        = 0x2A0000 bytes) using plain esp_partition_write - this region is
 *        guaranteed not to be the running partition (ota_1).
 *     2. Erase+write the new partition table (drops factory, grows ota_0 to
 *        0x20000/0x2A0000, leaves ota_1 untouched).
 *     3. esp_ota_set_boot_partition() on a manually-built esp_partition_t
 *        describing the new (larger) ota_0, then reboot.
 *
 *   Stage B (must be running from the Stage A ota_0):
 *     1. Write+verify the current app image into the ota_1 region under the
 *        FINAL table (0x208000, 0x1E8000 bytes) - untouched by the running
 *        Stage A ota_0 (0x20000-0x2C0000).
 *     2. Erase+write the final partition table (ota_0 shrinks back to
 *        0x20000/0x1E8000, ota_1 grows to 0x208000/0x1E8000).
 *     3. esp_ota_set_boot_partition() on the new ota_1, reboot.
 *
 * Each stage independently verifies the freshly-written image (byte-for-byte
 * readback compare) BEFORE touching the partition table, and the partition
 * table write happens LAST, because it's the only genuinely non-atomic step
 * (single 4 KB sector, erase+write).
 *
 * SAFETY NOTE: a power loss during the partition-table erase/write step
 * (~a few ms) is a real, understood risk with no automatic recovery. This
 * module must only run on a device with physical/serial recovery access.
 */

#include "partition_migration.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "part_migration";

/* Old (current, shipping) layout constants */
#define OLD_FACTORY_OTA0_ADDR   0x20000
#define OLD_FACTORY_OTA0_SIZE   0x2A0000   /* old factory (0x170000) + old ota_0 (0x130000) */

/* Stage A target layout constants (see partitions_stageA.csv) */
#define STAGEA_OTA0_ADDR        0x20000
#define STAGEA_OTA0_SIZE        0x2A0000
#define STAGEA_OTA1_ADDR        0x2C0000
#define STAGEA_OTA1_SIZE        0x130000

/* Stage B / final layout constants (see partitions_stageB.csv) */
#define STAGEB_OTA0_ADDR        0x20000
#define STAGEB_OTA0_SIZE        0x1E8000
#define STAGEB_OTA1_ADDR        0x208000
#define STAGEB_OTA1_SIZE        0x1E8000

#define PARTITION_TABLE_ADDR    0x8000
#define PARTITION_TABLE_SIZE    0x1000

/**
 * @brief Write the currently-running app image into an arbitrary raw flash
 * region and verify it (byte-for-byte readback compare), without going
 * through esp_ota_begin/write/end (which require a table-registered
 * partition). Used to stage the migrated image into a region that is not
 * (yet) described as an app partition by the currently-loaded table.
 */
static esp_err_t stage_running_image_to(uint32_t dest_addr, uint32_t dest_size)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "Could not get running partition");
        return ESP_FAIL;
    }

    /* running->size is the *partition container* size, not the actual app
     * image length - it can be larger than dest_size (e.g. Stage B copies
     * out of the Stage-A-enlarged ota_0 into the smaller final ota_1). Cap
     * the copy to dest_size: this is safe because the real firmware
     * (~1.1 MB) is always far smaller than any of our target regions, and
     * flash beyond the real image is erased (0xFF) padding on both source
     * and destination, so truncating it changes nothing the bootloader
     * reads. If the real image genuinely doesn't fit, esp_ota_set_boot_partition
     * will reject the too-small partition later and we bail out before that
     * point regardless. */
    uint32_t copy_size = running->size < dest_size ? running->size : dest_size;

    ESP_LOGI(TAG, "Erasing dest region 0x%lx (size 0x%lx)",
             (unsigned long)dest_addr, (unsigned long)dest_size);
    esp_err_t err = esp_flash_erase_region(NULL, dest_addr, dest_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        return err;
    }

    const size_t CHUNK = 4096;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t off = 0; off < copy_size; off += CHUNK) {
        size_t len = (copy_size - off) < CHUNK ? (copy_size - off) : CHUNK;
        err = esp_partition_read(running, off, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read from running partition failed at 0x%lx: %s",
                     (unsigned long)off, esp_err_to_name(err));
            free(buf);
            return err;
        }
        err = esp_flash_write(NULL, buf, dest_addr + off, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write to 0x%lx failed: %s",
                     (unsigned long)(dest_addr + off), esp_err_to_name(err));
            free(buf);
            return err;
        }
    }
    free(buf);

    /* Verify: re-read every chunk from flash and compare with the source
     * partition, byte-for-byte. This is the mandatory verify-before-table-swap
     * gate; if this fails, we bail out having never touched the partition
     * table, so the device is still in its original bootable state. */
    uint8_t *verify_buf = malloc(CHUNK);
    uint8_t *src_buf = malloc(CHUNK);
    if (!verify_buf || !src_buf) {
        free(verify_buf);
        free(src_buf);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t off = 0; off < copy_size; off += CHUNK) {
        size_t len = (copy_size - off) < CHUNK ? (copy_size - off) : CHUNK;
        if (esp_partition_read(running, off, src_buf, len) != ESP_OK ||
            esp_flash_read(NULL, verify_buf, dest_addr + off, len) != ESP_OK) {
            ESP_LOGE(TAG, "verify read failed at 0x%lx", (unsigned long)off);
            free(verify_buf);
            free(src_buf);
            return ESP_FAIL;
        }
        if (memcmp(src_buf, verify_buf, len) != 0) {
            ESP_LOGE(TAG, "verify MISMATCH at offset 0x%lx - aborting, table NOT touched",
                     (unsigned long)off);
            free(verify_buf);
            free(src_buf);
            return ESP_ERR_INVALID_CRC;
        }
    }
    free(verify_buf);
    free(src_buf);

    ESP_LOGI(TAG, "Staged + verified %lu bytes at 0x%lx",
             (unsigned long)copy_size, (unsigned long)dest_addr);
    return ESP_OK;
}

/**
 * @brief Erase+write the partition table sector from an in-memory blob.
 * This is the single non-atomic, highest-risk step in the whole migration:
 * a power loss here can leave no valid partition table. Callers MUST have
 * already staged+verified all app images before calling this.
 */
static esp_err_t write_partition_table(const uint8_t *table_bin, size_t table_len)
{
    if (table_len > PARTITION_TABLE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGW(TAG, "Writing new partition table at 0x%x - device MUST NOT lose power now",
             PARTITION_TABLE_ADDR);
    esp_err_t err = esp_flash_erase_region(NULL, PARTITION_TABLE_ADDR, PARTITION_TABLE_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    return esp_flash_write(NULL, (void *)table_bin, PARTITION_TABLE_ADDR, table_len);
}

/**
 * @brief Build an esp_partition_t describing an app partition at an
 * arbitrary address/size, for use with esp_ota_set_boot_partition() before
 * esp_partition_find() would recognize it (table not reloaded yet).
 */
static void make_app_partition(esp_partition_t *p, uint32_t addr, uint32_t size,
                                esp_partition_subtype_t subtype, const char *label)
{
    memset(p, 0, sizeof(*p));
    p->type = ESP_PARTITION_TYPE_APP;
    p->subtype = subtype;
    p->address = addr;
    p->size = size;
    strncpy(p->label, label, sizeof(p->label) - 1);
    p->encrypted = false;
}

esp_err_t partition_migration_run_stage_a(const uint8_t *stage_a_table_bin, size_t table_len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_FAIL;
    }
    if (strcmp(running->label, "ota_1") != 0) {
        ESP_LOGE(TAG, "Stage A must run while booted from ota_1 (currently: %s). "
                      "OTA once more to flip to ota_1, then retry.", running->label);
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Stage + verify the image into the old factory+ota_0 region. */
    esp_err_t err = stage_running_image_to(OLD_FACTORY_OTA0_ADDR, OLD_FACTORY_OTA0_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stage A image staging failed, aborting (table untouched): %s",
                 esp_err_to_name(err));
        return err;
    }

    /* 2. Write the new partition table (drops factory, grows ota_0). */
    err = write_partition_table(stage_a_table_bin, table_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stage A partition table write failed: %s - DEVICE MAY NEED "
                      "SERIAL RECOVERY", esp_err_to_name(err));
        return err;
    }

    /* 3. Point boot partition at the new (larger) ota_0 and reboot. */
    esp_partition_t new_ota0;
    make_app_partition(&new_ota0, STAGEA_OTA0_ADDR, STAGEA_OTA0_SIZE, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    err = esp_ota_set_boot_partition(&new_ota0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition (stage A) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Stage A complete, rebooting into enlarged ota_0...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* unreachable */
}

esp_err_t partition_migration_run_stage_b(const uint8_t *stage_b_table_bin, size_t table_len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_FAIL;
    }
    if (strcmp(running->label, "ota_0") != 0 || running->size != STAGEA_OTA0_SIZE) {
        ESP_LOGE(TAG, "Stage B must run while booted from the Stage A ota_0 "
                      "(size 0x%x). Currently: %s, size 0x%lx",
                 STAGEA_OTA0_SIZE, running->label, (unsigned long)running->size);
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Stage + verify the image into the final ota_1 region (untouched by
     * the currently-running Stage A ota_0, which spans 0x20000-0x2C0000). */
    esp_err_t err = stage_running_image_to(STAGEB_OTA1_ADDR, STAGEB_OTA1_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stage B image staging failed, aborting (table untouched): %s",
                 esp_err_to_name(err));
        return err;
    }

    /* 2. Write the final partition table (ota_0 shrinks, ota_1 grows to match). */
    err = write_partition_table(stage_b_table_bin, table_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stage B partition table write failed: %s - DEVICE MAY NEED "
                      "SERIAL RECOVERY", esp_err_to_name(err));
        return err;
    }

    /* 3. Point boot partition at the new ota_1 and reboot. */
    esp_partition_t new_ota1;
    make_app_partition(&new_ota1, STAGEB_OTA1_ADDR, STAGEB_OTA1_SIZE, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    err = esp_ota_set_boot_partition(&new_ota1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition (stage B) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Stage B complete, rebooting into final equal-size ota_1...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* unreachable */
}
