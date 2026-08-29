/**
 * @file log_persist.c
 * @brief Persists the RAM log ring buffer to the unused "storage" flash
 *        partition (declared but never used elsewhere in this project) so
 *        log history survives reboots/crashes.
 *
 * The partition is divided into fixed-size slots (matching LOG_BUFFER_SIZE)
 * written round-robin so no single flash sector is erased on every flush -
 * this spreads wear across all slots and gives the partition a long life
 * even with a periodic flush interval of a few minutes.
 */

#include "log_persist.h"
#include "log_buffer.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

static const char *TAG = "log_persist";

#define LOG_PERSIST_MAGIC 0x474F4C50u /* "PLOG" */
#define SLOT_SIZE          LOG_BUFFER_SIZE
#define HEADER_SIZE        sizeof(log_slot_header_t)
#define SLOT_PAYLOAD_CAP   (SLOT_SIZE - HEADER_SIZE)
/* Only use 2 of the partition's available slots (32KB of the 64KB
 * "storage" partition) - leaves the rest free for future use. */
#define LOG_PERSIST_MAX_SLOTS 2

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint32_t length;
    uint32_t crc32;
} log_slot_header_t;

static const esp_partition_t *s_part = NULL;
static size_t s_num_slots = 0;
static uint32_t s_next_sequence = 0;
static size_t s_next_slot = 0;
static esp_timer_handle_t s_flush_timer = NULL;

/* Small self-contained CRC32 (no table) - only run once per flush on <=16KB */
static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
        }
    }
    return ~crc;
}

esp_err_t log_persist_init(char *out_buffer, size_t out_size, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_DATA_NVS,
                                       "storage");
    if (!s_part) {
        ESP_LOGW(TAG, "'storage' partition not found - log persistence disabled");
        return ESP_ERR_NOT_FOUND;
    }

    s_num_slots = s_part->size / SLOT_SIZE;
    if (s_num_slots == 0) {
        ESP_LOGW(TAG, "'storage' partition too small for a single log slot");
        s_part = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    if (s_num_slots > LOG_PERSIST_MAX_SLOTS) {
        s_num_slots = LOG_PERSIST_MAX_SLOTS; /* intentionally leave the rest of the partition unused */
    }

    ESP_LOGI(TAG, "Using '%s' partition (%" PRIu32 " bytes, %u slots of %u bytes)",
             s_part->label, s_part->size, (unsigned)s_num_slots, (unsigned)SLOT_SIZE);

    /* Scan every slot header to find the most recently written valid slot */
    int best_slot = -1;
    uint32_t best_seq = 0;
    log_slot_header_t hdr;

    for (size_t i = 0; i < s_num_slots; i++) {
        size_t offset = i * SLOT_SIZE;
        if (esp_partition_read(s_part, offset, &hdr, sizeof(hdr)) != ESP_OK) {
            continue;
        }
        if (hdr.magic != LOG_PERSIST_MAGIC) {
            continue; /* erased/never written slot */
        }
        if (hdr.length > SLOT_PAYLOAD_CAP) {
            continue; /* corrupt header */
        }
        if (best_slot < 0 || (int32_t)(hdr.sequence - best_seq) > 0) {
            best_slot = (int)i;
            best_seq = hdr.sequence;
        }
    }

    if (best_slot >= 0) {
        size_t offset = (size_t)best_slot * SLOT_SIZE;
        if (esp_partition_read(s_part, offset, &hdr, sizeof(hdr)) == ESP_OK) {
            size_t copy_len = hdr.length;
            if (copy_len > out_size) {
                copy_len = out_size;
            }
            if (copy_len > 0 &&
                esp_partition_read(s_part, offset + HEADER_SIZE, out_buffer, copy_len) == ESP_OK) {
                uint32_t crc = crc32_calc((const uint8_t *)out_buffer, copy_len);
                if (crc == hdr.crc32 && copy_len == hdr.length) {
                    if (out_len) {
                        *out_len = copy_len;
                    }
                    ESP_LOGI(TAG, "Recovered %u bytes of log from previous boot (slot %d, seq %" PRIu32 ")",
                             (unsigned)copy_len, best_slot, hdr.sequence);
                } else {
                    ESP_LOGW(TAG, "Previous log slot %d failed CRC check - discarding", best_slot);
                }
            }
        }
        s_next_sequence = best_seq + 1;
        s_next_slot = ((size_t)best_slot + 1) % s_num_slots;
    } else {
        ESP_LOGI(TAG, "No previous log snapshot found (first boot or empty flash)");
        s_next_sequence = 1;
        s_next_slot = 0;
    }

    return ESP_OK;
}

void log_persist_flush(void)
{
    if (!s_part) {
        return;
    }

    static char buf[SLOT_SIZE];
    size_t len = log_buffer_get(buf, sizeof(buf));
    if (len > SLOT_PAYLOAD_CAP) {
        len = SLOT_PAYLOAD_CAP;
    }

    log_slot_header_t hdr = {
        .magic = LOG_PERSIST_MAGIC,
        .sequence = s_next_sequence,
        .length = (uint32_t)len,
        .crc32 = crc32_calc((const uint8_t *)buf, len),
    };

    size_t offset = s_next_slot * SLOT_SIZE;

    if (esp_partition_erase_range(s_part, offset, SLOT_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to erase log slot %u", (unsigned)s_next_slot);
        return;
    }
    if (esp_partition_write(s_part, offset, &hdr, sizeof(hdr)) != ESP_OK ||
        (len > 0 && esp_partition_write(s_part, offset + HEADER_SIZE, buf, len) != ESP_OK)) {
        ESP_LOGW(TAG, "Failed to write log slot %u", (unsigned)s_next_slot);
        return;
    }

    ESP_LOGD(TAG, "Persisted %u bytes of log to slot %u (seq %" PRIu32 ")",
             (unsigned)len, (unsigned)s_next_slot, s_next_sequence);

    s_next_sequence++;
    s_next_slot = (s_next_slot + 1) % s_num_slots;
}

static void flush_timer_cb(void *arg)
{
    (void)arg;
    log_persist_flush();
}

void log_persist_start_periodic_flush(int interval_sec)
{
    if (!s_part || s_flush_timer) {
        return;
    }

    const esp_timer_create_args_t args = {
        .callback = &flush_timer_cb,
        .name = "log_persist_flush",
    };
    if (esp_timer_create(&args, &s_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_flush_timer, (uint64_t)interval_sec * 1000000ULL);
        ESP_LOGI(TAG, "Periodic log persistence flush every %d seconds", interval_sec);
    }
}
