/**
 * @file modbus_map.c
 * @brief Modbus TCP register map, channel status and channel table (host-testable)
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

modbus_channel_status_t modbus_channel_status(const modbus_channel_t *channel,
                                        const modbus_sensor_input_t *sensor,
                                        uint16_t last_result,
                                        int64_t now_ms,
                                        uint32_t read_interval_ms)
{
    if (channel == NULL || !channel->assigned) {
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
                       const modbus_channel_table_t *table,
                       const modbus_sensor_input_t *sensors, int sensor_count,
                       const modbus_info_input_t *info)
{
    memset(out, 0, sizeof(*out));

    uint16_t *r = out->info;
    r[MB_INFO_MAP_VERSION] = MODBUS_MAP_VERSION;
    r[MB_INFO_FW_MAJOR] = info->fw_version[0];
    r[MB_INFO_FW_MINOR] = info->fw_version[1];
    r[MB_INFO_FW_PATCH] = info->fw_version[2];
    r[MB_INFO_CHANNEL_CAPACITY] = MODBUS_CHANNEL_COUNT;
    r[MB_INFO_CYCLE_COUNT] = (uint16_t)(info->cycle_count & 0xFFFF);
    r[MB_INFO_UPTIME_HI] = (uint16_t)(info->uptime_s >> 16);
    r[MB_INFO_UPTIME_LO] = (uint16_t)(info->uptime_s & 0xFFFF);
    for (int i = 0; i < 3; i++) {
        r[MB_INFO_MAC_0 + i] = (uint16_t)((info->mac[2 * i] << 8) | info->mac[2 * i + 1]);
    }
    r[MB_INFO_MAX_SENSORS] = info->max_sensors;
    r[MB_INFO_CHANNELS_ASSIGNED] = (uint16_t)modbus_channels_assigned_count(table);
    r[MB_INFO_SENSORS_PRESENT] = (uint16_t)(sensor_count < 0 ? 0 : sensor_count);
    r[MB_INFO_LAST_RESULT] = info->last_result;
    uint32_t interval_s = info->read_interval_ms / 1000;
    r[MB_INFO_READ_INTERVAL_S] = interval_s > 0xFFFF ? 0xFFFF : (uint16_t)interval_s;

    for (int s = 0; s < MODBUS_CHANNEL_COUNT; s++) {
        const modbus_channel_t *channel = &table->channels[s];
        const modbus_sensor_input_t *sensor =
            channel->assigned ? find_sensor(sensors, sensor_count, channel->rom) : NULL;

        modbus_channel_status_t st = modbus_channel_status(channel, sensor, info->last_result,
                                                     info->now_ms, info->read_interval_ms);
        out->status[s] = (uint16_t)st;
        out->temp[s] = (st == MB_STATUS_OK) ? modbus_temp_to_reg(sensor->temperature)
                                            : MB_TEMP_INVALID;
        out->age[s] = sensor ? modbus_age_to_reg(sensor->last_read_ms, info->now_ms)
                             : MB_AGE_NEVER;
        if (channel->assigned) {
            for (int k = 0; k < MB_REGS_PER_ROM; k++) {
                out->rom[s * MB_REGS_PER_ROM + k] =
                    (uint16_t)((channel->rom[2 * k] << 8) | channel->rom[2 * k + 1]);
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

int modbus_channels_find(const modbus_channel_table_t *table, const uint8_t *rom)
{
    for (int s = 0; s < MODBUS_CHANNEL_COUNT; s++) {
        if (table->channels[s].assigned &&
            memcmp(table->channels[s].rom, rom, MODBUS_ROM_LEN) == 0) {
            return s;
        }
    }
    return -1;
}

int modbus_channels_assigned_count(const modbus_channel_table_t *table)
{
    int n = 0;
    for (int s = 0; s < MODBUS_CHANNEL_COUNT; s++) {
        if (table->channels[s].assigned) {
            n++;
        }
    }
    return n;
}

static int rom_cmp(const void *a, const void *b)
{
    return memcmp(a, b, MODBUS_ROM_LEN);
}

int modbus_channels_auto_assign(modbus_channel_table_t *table,
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
        if (modbus_channels_find(table, roms[i]) >= 0) {
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
    for (int s = 0; s < MODBUS_CHANNEL_COUNT && next < n; s++) {
        if (!table->channels[s].assigned) {
            table->channels[s].assigned = true;
            memcpy(table->channels[s].rom, pending[next++], MODBUS_ROM_LEN);
            done++;
        }
    }
    free(pending);

    if (assigned) {
        *assigned = done;
    }
    return n - next;
}

bool modbus_channels_move(modbus_channel_table_t *table, int from, int to)
{
    if (from < 0 || from >= MODBUS_CHANNEL_COUNT || to < 0 || to >= MODBUS_CHANNEL_COUNT) {
        return false;
    }
    modbus_channel_t tmp = table->channels[to];
    table->channels[to] = table->channels[from];
    table->channels[from] = tmp;
    return true;
}

bool modbus_channels_release(modbus_channel_table_t *table, int channel)
{
    if (channel < 0 || channel >= MODBUS_CHANNEL_COUNT) {
        return false;
    }
    memset(&table->channels[channel], 0, sizeof(table->channels[channel]));
    return true;
}

static bool channel_index_ok(int channel)
{
    return channel >= 0 && channel < MODBUS_CHANNEL_COUNT;
}

modbus_channel_op_t modbus_channels_move_sensor(modbus_channel_table_t *table, int from, int to)
{
    if (!channel_index_ok(from) || !channel_index_ok(to)) {
        return MB_CHANNEL_OP_BAD_INDEX;
    }
    if (!table->channels[from].assigned) {
        return MB_CHANNEL_OP_EMPTY;
    }
    modbus_channels_move(table, from, to);
    return MB_CHANNEL_OP_OK;
}

modbus_channel_op_t modbus_channels_release_missing(modbus_channel_table_t *table, int channel,
                                              const uint8_t (*present_roms)[MODBUS_ROM_LEN],
                                              int present_count)
{
    if (!channel_index_ok(channel)) {
        return MB_CHANNEL_OP_BAD_INDEX;
    }
    if (!table->channels[channel].assigned) {
        return MB_CHANNEL_OP_EMPTY;
    }
    for (int i = 0; i < present_count && present_roms != NULL; i++) {
        if (memcmp(present_roms[i], table->channels[channel].rom, MODBUS_ROM_LEN) == 0) {
            return MB_CHANNEL_OP_PRESENT;
        }
    }
    modbus_channels_release(table, channel);
    return MB_CHANNEL_OP_OK;
}

modbus_channel_op_t modbus_channels_place(modbus_channel_table_t *table, int channel, const uint8_t *rom)
{
    if (!channel_index_ok(channel)) {
        return MB_CHANNEL_OP_BAD_INDEX;
    }
    if (table->channels[channel].assigned) {
        return MB_CHANNEL_OP_OCCUPIED;
    }
    if (modbus_channels_find(table, rom) >= 0) {
        return MB_CHANNEL_OP_DUPLICATE;
    }
    table->channels[channel].assigned = true;
    memcpy(table->channels[channel].rom, rom, MODBUS_ROM_LEN);
    return MB_CHANNEL_OP_OK;
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

void modbus_channels_serialize(const modbus_channel_table_t *table, uint8_t *buf)
{
    size_t p = 0;
    buf[p++] = (uint8_t)(MODBUS_CHANNEL_BLOB_VERSION & 0xFF);
    buf[p++] = (uint8_t)(MODBUS_CHANNEL_BLOB_VERSION >> 8);
    for (int s = 0; s < MODBUS_CHANNEL_COUNT; s++) {
        buf[p++] = table->channels[s].assigned ? 1 : 0;
        if (table->channels[s].assigned) {
            memcpy(&buf[p], table->channels[s].rom, MODBUS_ROM_LEN);
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

bool modbus_channels_deserialize(modbus_channel_table_t *table, const uint8_t *buf, size_t len)
{
    memset(table, 0, sizeof(*table));
    if (buf == NULL || len != MODBUS_CHANNEL_BLOB_SIZE) {
        return false;
    }
    uint16_t version = (uint16_t)(buf[0] | (buf[1] << 8));
    if (version != MODBUS_CHANNEL_BLOB_VERSION) {
        return false;
    }
    size_t body = MODBUS_CHANNEL_BLOB_SIZE - 4;
    uint32_t stored = (uint32_t)buf[body] | ((uint32_t)buf[body + 1] << 8) |
                      ((uint32_t)buf[body + 2] << 16) | ((uint32_t)buf[body + 3] << 24);
    if (stored != modbus_crc32(buf, body)) {
        return false;
    }

    modbus_channel_table_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    size_t p = 2;
    for (int s = 0; s < MODBUS_CHANNEL_COUNT; s++) {
        uint8_t flag = buf[p++];
        if (flag > 1) {
            return false;
        }
        if (flag == 1) {
            parsed.channels[s].assigned = true;
            memcpy(parsed.channels[s].rom, &buf[p], MODBUS_ROM_LEN);
        }
        p += MODBUS_ROM_LEN;
    }
    /* A ROM may only occupy one channel */
    for (int a = 0; a < MODBUS_CHANNEL_COUNT; a++) {
        if (!parsed.channels[a].assigned) {
            continue;
        }
        for (int b = a + 1; b < MODBUS_CHANNEL_COUNT; b++) {
            if (parsed.channels[b].assigned &&
                memcmp(parsed.channels[a].rom, parsed.channels[b].rom, MODBUS_ROM_LEN) == 0) {
                return false;
            }
        }
    }
    *table = parsed;
    return true;
}
