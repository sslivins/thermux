/**
 * @file modbus_map.c
 * @brief Modbus TCP register map, slot status and slot table (host-testable)
 */

#include "modbus_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint16_t modbus_temp_to_reg(float celsius)
{
    if (isnan(celsius)) {
        return MB_TEMP_INVALID;
    }
    float scaled = celsius * 100.0f;
    /* Clamp to +-32767 so a valid reading can never look like 0x8000 */
    if (scaled > 32767.0f) {
        scaled = 32767.0f;
    } else if (scaled < -32767.0f) {
        scaled = -32767.0f;
    }
    int16_t v = (int16_t)lroundf(scaled);
    return (uint16_t)v;
}

uint16_t modbus_age_to_reg(int64_t last_read_ms, int64_t now_ms)
{
    if (last_read_ms <= 0) {
        return MB_AGE_NEVER;
    }
    if (now_ms <= last_read_ms) {
        return 0;
    }
    int64_t s = (now_ms - last_read_ms) / 1000;
    if (s >= MB_AGE_NEVER) {
        return MB_AGE_NEVER - 1;
    }
    return (uint16_t)s;
}

int64_t modbus_stale_limit_ms(uint32_t read_interval_ms)
{
    int64_t limit = (int64_t)read_interval_ms * MB_STALE_INTERVALS;
    return limit > MB_STALE_MIN_MS ? limit : MB_STALE_MIN_MS;
}

modbus_slot_status_t modbus_slot_status(const modbus_slot_t *slot,
                                        const modbus_sensor_input_t *sensor,
                                        uint16_t last_result,
                                        int64_t now_ms,
                                        uint32_t read_interval_ms)
{
    if (slot == NULL || !slot->assigned) {
        return MB_STATUS_UNASSIGNED;
    }
    if (sensor == NULL) {
        return MB_STATUS_MISSING;
    }
    if (last_result == MB_CYCLE_FAILED) {
        return MB_STATUS_READ_ERROR;
    }
    if (sensor->last_attempt_ms > 0 && !sensor->valid) {
        return MB_STATUS_READ_ERROR;
    }
    if (sensor->last_read_ms <= 0 || !sensor->valid) {
        return MB_STATUS_STALE;
    }
    if (now_ms - sensor->last_read_ms > modbus_stale_limit_ms(read_interval_ms)) {
        return MB_STATUS_STALE;
    }
    return MB_STATUS_OK;
}

void modbus_parse_version(const char *version, uint16_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (version == NULL) {
        return;
    }
    const char *p = version;
    while (*p && (*p < '0' || *p > '9')) {
        p++;
    }
    for (int part = 0; part < 3 && *p >= '0' && *p <= '9'; part++) {
        unsigned long v = strtoul(p, (char **)&p, 10);
        out[part] = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
        if (*p != '.') {
            break;
        }
        p++;
    }
}

static const modbus_sensor_input_t *find_sensor(const modbus_sensor_input_t *sensors,
                                                int count, const uint8_t *rom)
{
    for (int i = 0; i < count; i++) {
        if (memcmp(sensors[i].rom, rom, MODBUS_ROM_LEN) == 0) {
            return &sensors[i];
        }
    }
    return NULL;
}

void modbus_build_regs(modbus_regs_t *out,
                       const modbus_slot_table_t *table,
                       const modbus_sensor_input_t *sensors, int sensor_count,
                       const modbus_info_input_t *info)
{
    memset(out, 0, sizeof(*out));

    uint16_t *r = out->info;
    r[MB_INFO_MAP_VERSION] = MODBUS_MAP_VERSION;
    r[MB_INFO_FW_MAJOR] = info->fw_version[0];
    r[MB_INFO_FW_MINOR] = info->fw_version[1];
    r[MB_INFO_FW_PATCH] = info->fw_version[2];
    r[MB_INFO_SLOT_CAPACITY] = MODBUS_SLOT_COUNT;
    r[MB_INFO_CYCLE_COUNT] = (uint16_t)(info->cycle_count & 0xFFFF);
    r[MB_INFO_UPTIME_HI] = (uint16_t)(info->uptime_s >> 16);
    r[MB_INFO_UPTIME_LO] = (uint16_t)(info->uptime_s & 0xFFFF);
    for (int i = 0; i < 3; i++) {
        r[MB_INFO_MAC_0 + i] = (uint16_t)((info->mac[2 * i] << 8) | info->mac[2 * i + 1]);
    }
    r[MB_INFO_MAX_SENSORS] = info->max_sensors;
    r[MB_INFO_SLOTS_ASSIGNED] = (uint16_t)modbus_slots_assigned_count(table);
    r[MB_INFO_SENSORS_PRESENT] = (uint16_t)(sensor_count < 0 ? 0 : sensor_count);
    r[MB_INFO_LAST_RESULT] = info->last_result;
    uint32_t interval_s = info->read_interval_ms / 1000;
    r[MB_INFO_READ_INTERVAL_S] = interval_s > 0xFFFF ? 0xFFFF : (uint16_t)interval_s;

    for (int s = 0; s < MODBUS_SLOT_COUNT; s++) {
        const modbus_slot_t *slot = &table->slots[s];
        const modbus_sensor_input_t *sensor =
            slot->assigned ? find_sensor(sensors, sensor_count, slot->rom) : NULL;

        modbus_slot_status_t st = modbus_slot_status(slot, sensor, info->last_result,
                                                     info->now_ms, info->read_interval_ms);
        out->status[s] = (uint16_t)st;
        out->temp[s] = (st == MB_STATUS_OK) ? modbus_temp_to_reg(sensor->temperature)
                                            : MB_TEMP_INVALID;
        out->age[s] = sensor ? modbus_age_to_reg(sensor->last_read_ms, info->now_ms)
                             : MB_AGE_NEVER;
        if (slot->assigned) {
            for (int k = 0; k < MB_REGS_PER_ROM; k++) {
                out->rom[s * MB_REGS_PER_ROM + k] =
                    (uint16_t)((slot->rom[2 * k] << 8) | slot->rom[2 * k + 1]);
            }
        }
    }
}

bool modbus_config_valid(uint32_t port, uint32_t unit_id, uint32_t reserved_port)
{
    return port >= 1 && port <= 65535 && port != reserved_port &&
           unit_id >= 1 && unit_id <= 247;
}

bool modbus_unit_id_accepted(uint8_t request_uid, uint8_t configured_uid)
{
    return request_uid == configured_uid || request_uid == 0 || request_uid == 255;
}

int modbus_slots_find(const modbus_slot_table_t *table, const uint8_t *rom)
{
    for (int s = 0; s < MODBUS_SLOT_COUNT; s++) {
        if (table->slots[s].assigned &&
            memcmp(table->slots[s].rom, rom, MODBUS_ROM_LEN) == 0) {
            return s;
        }
    }
    return -1;
}

int modbus_slots_assigned_count(const modbus_slot_table_t *table)
{
    int n = 0;
    for (int s = 0; s < MODBUS_SLOT_COUNT; s++) {
        if (table->slots[s].assigned) {
            n++;
        }
    }
    return n;
}

static int rom_cmp(const void *a, const void *b)
{
    return memcmp(a, b, MODBUS_ROM_LEN);
}

int modbus_slots_auto_assign(modbus_slot_table_t *table,
                             const uint8_t (*roms)[MODBUS_ROM_LEN], int rom_count,
                             int *assigned)
{
    if (assigned) {
        *assigned = 0;
    }
    if (rom_count <= 0 || roms == NULL) {
        return 0;
    }

    uint8_t (*pending)[MODBUS_ROM_LEN] = malloc((size_t)rom_count * MODBUS_ROM_LEN);
    if (pending == NULL) {
        return rom_count;
    }
    int n = 0;
    for (int i = 0; i < rom_count; i++) {
        if (modbus_slots_find(table, roms[i]) >= 0) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (memcmp(pending[j], roms[i], MODBUS_ROM_LEN) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            memcpy(pending[n++], roms[i], MODBUS_ROM_LEN);
        }
    }
    qsort(pending, (size_t)n, MODBUS_ROM_LEN, rom_cmp);

    int next = 0;
    int done = 0;
    for (int s = 0; s < MODBUS_SLOT_COUNT && next < n; s++) {
        if (!table->slots[s].assigned) {
            table->slots[s].assigned = true;
            memcpy(table->slots[s].rom, pending[next++], MODBUS_ROM_LEN);
            done++;
        }
    }
    free(pending);

    if (assigned) {
        *assigned = done;
    }
    return n - next;
}

bool modbus_slots_move(modbus_slot_table_t *table, int from, int to)
{
    if (from < 0 || from >= MODBUS_SLOT_COUNT || to < 0 || to >= MODBUS_SLOT_COUNT) {
        return false;
    }
    modbus_slot_t tmp = table->slots[to];
    table->slots[to] = table->slots[from];
    table->slots[from] = tmp;
    return true;
}

bool modbus_slots_release(modbus_slot_table_t *table, int slot)
{
    if (slot < 0 || slot >= MODBUS_SLOT_COUNT) {
        return false;
    }
    memset(&table->slots[slot], 0, sizeof(table->slots[slot]));
    return true;
}

static bool slot_index_ok(int slot)
{
    return slot >= 0 && slot < MODBUS_SLOT_COUNT;
}

modbus_slot_op_t modbus_slots_move_sensor(modbus_slot_table_t *table, int from, int to)
{
    if (!slot_index_ok(from) || !slot_index_ok(to)) {
        return MB_SLOT_OP_BAD_INDEX;
    }
    if (!table->slots[from].assigned) {
        return MB_SLOT_OP_EMPTY;
    }
    modbus_slots_move(table, from, to);
    return MB_SLOT_OP_OK;
}

modbus_slot_op_t modbus_slots_release_missing(modbus_slot_table_t *table, int slot,
                                              const uint8_t (*present_roms)[MODBUS_ROM_LEN],
                                              int present_count)
{
    if (!slot_index_ok(slot)) {
        return MB_SLOT_OP_BAD_INDEX;
    }
    if (!table->slots[slot].assigned) {
        return MB_SLOT_OP_EMPTY;
    }
    for (int i = 0; i < present_count && present_roms != NULL; i++) {
        if (memcmp(present_roms[i], table->slots[slot].rom, MODBUS_ROM_LEN) == 0) {
            return MB_SLOT_OP_PRESENT;
        }
    }
    modbus_slots_release(table, slot);
    return MB_SLOT_OP_OK;
}

modbus_slot_op_t modbus_slots_place(modbus_slot_table_t *table, int slot, const uint8_t *rom)
{
    if (!slot_index_ok(slot)) {
        return MB_SLOT_OP_BAD_INDEX;
    }
    if (table->slots[slot].assigned) {
        return MB_SLOT_OP_OCCUPIED;
    }
    if (modbus_slots_find(table, rom) >= 0) {
        return MB_SLOT_OP_DUPLICATE;
    }
    table->slots[slot].assigned = true;
    memcpy(table->slots[slot].rom, rom, MODBUS_ROM_LEN);
    return MB_SLOT_OP_OK;
}

void modbus_rom_to_hex(const uint8_t *rom, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < MODBUS_ROM_LEN; i++) {
        out[i * 2] = hex[rom[i] >> 4];
        out[i * 2 + 1] = hex[rom[i] & 0x0F];
    }
    out[MODBUS_ROM_LEN * 2] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool modbus_rom_from_hex(const char *str, uint8_t *rom)
{
    if (str == NULL || strlen(str) != MODBUS_ROM_LEN * 2) {
        return false;
    }
    uint8_t tmp[MODBUS_ROM_LEN];
    for (int i = 0; i < MODBUS_ROM_LEN; i++) {
        int hi = hex_nibble(str[i * 2]);
        int lo = hex_nibble(str[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        tmp[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(rom, tmp, sizeof(tmp));
    return true;
}

uint32_t modbus_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

void modbus_slots_serialize(const modbus_slot_table_t *table, uint8_t *buf)
{
    size_t p = 0;
    buf[p++] = (uint8_t)(MODBUS_SLOT_BLOB_VERSION & 0xFF);
    buf[p++] = (uint8_t)(MODBUS_SLOT_BLOB_VERSION >> 8);
    for (int s = 0; s < MODBUS_SLOT_COUNT; s++) {
        buf[p++] = table->slots[s].assigned ? 1 : 0;
        if (table->slots[s].assigned) {
            memcpy(&buf[p], table->slots[s].rom, MODBUS_ROM_LEN);
        } else {
            memset(&buf[p], 0, MODBUS_ROM_LEN);
        }
        p += MODBUS_ROM_LEN;
    }
    uint32_t crc = modbus_crc32(buf, p);
    buf[p++] = (uint8_t)(crc & 0xFF);
    buf[p++] = (uint8_t)((crc >> 8) & 0xFF);
    buf[p++] = (uint8_t)((crc >> 16) & 0xFF);
    buf[p++] = (uint8_t)((crc >> 24) & 0xFF);
}

bool modbus_slots_deserialize(modbus_slot_table_t *table, const uint8_t *buf, size_t len)
{
    memset(table, 0, sizeof(*table));
    if (buf == NULL || len != MODBUS_SLOT_BLOB_SIZE) {
        return false;
    }
    uint16_t version = (uint16_t)(buf[0] | (buf[1] << 8));
    if (version != MODBUS_SLOT_BLOB_VERSION) {
        return false;
    }
    size_t body = MODBUS_SLOT_BLOB_SIZE - 4;
    uint32_t stored = (uint32_t)buf[body] | ((uint32_t)buf[body + 1] << 8) |
                      ((uint32_t)buf[body + 2] << 16) | ((uint32_t)buf[body + 3] << 24);
    if (stored != modbus_crc32(buf, body)) {
        return false;
    }

    modbus_slot_table_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    size_t p = 2;
    for (int s = 0; s < MODBUS_SLOT_COUNT; s++) {
        uint8_t flag = buf[p++];
        if (flag > 1) {
            return false;
        }
        if (flag == 1) {
            parsed.slots[s].assigned = true;
            memcpy(parsed.slots[s].rom, &buf[p], MODBUS_ROM_LEN);
        }
        p += MODBUS_ROM_LEN;
    }
    /* A ROM may only occupy one slot */
    for (int a = 0; a < MODBUS_SLOT_COUNT; a++) {
        if (!parsed.slots[a].assigned) {
            continue;
        }
        for (int b = a + 1; b < MODBUS_SLOT_COUNT; b++) {
            if (parsed.slots[b].assigned &&
                memcmp(parsed.slots[a].rom, parsed.slots[b].rom, MODBUS_ROM_LEN) == 0) {
                return false;
            }
        }
    }
    *table = parsed;
    return true;
}
