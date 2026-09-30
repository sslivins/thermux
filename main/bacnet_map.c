/**
 * @file bacnet_map.c
 * @brief BACnet/IP config and object mapping helpers (host-testable)
 */

#include "bacnet_map.h"

#include <stdio.h>
#include <string.h>

uint32_t bacnet_default_device_instance_from_mac(const uint8_t mac[6])
{
    uint32_t tail = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    return tail % (BACNET_MAX_DEVICE_INSTANCE + 1U);
}

bool bacnet_config_valid(uint32_t port, uint32_t device_instance, uint32_t reserved_port)
{
    return port >= 1 && port <= 65535 && port != reserved_port &&
           device_instance <= BACNET_MAX_DEVICE_INSTANCE;
}

bacnet_map_reliability_t bacnet_reliability_from_modbus_status(uint16_t status)
{
    switch (status) {
    case MB_STATUS_OK:
        return BACNET_MAP_RELIABILITY_NO_FAULT_DETECTED;
    case MB_STATUS_MISSING:
        return BACNET_MAP_RELIABILITY_NO_SENSOR;
    case MB_STATUS_READ_ERROR:
        return BACNET_MAP_RELIABILITY_COMMUNICATION_FAILURE;
    case MB_STATUS_STALE:
        return BACNET_MAP_RELIABILITY_UNRELIABLE_OTHER;
    case MB_STATUS_UNASSIGNED:
    default:
        return BACNET_MAP_RELIABILITY_UNRELIABLE_OTHER;
    }
}

bool bacnet_status_fault(uint16_t status)
{
    return status != MB_STATUS_OK;
}

void bacnet_default_channel_name(unsigned channel, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "Channel %u", channel);
}

void bacnet_clean_device_name(const char *input, const char *fallback, char *out, size_t out_size)
{
    const char *src = (input && input[0]) ? input : fallback;
    if (src == NULL || src[0] == '\0') {
        src = "Thermux";
    }
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%.*s", BACNET_DEVICE_NAME_MAX, src);
}

static bool name_exists(const char *candidate, char existing[][BACNET_OBJECT_NAME_MAX + 1], size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(candidate, existing[i]) == 0) {
            return true;
        }
    }
    return false;
}

void bacnet_make_unique_ai_name(const char *base, unsigned channel, char existing[][BACNET_OBJECT_NAME_MAX + 1], size_t existing_count, char *out, size_t out_size)
{
    char fallback[24];
    bacnet_default_channel_name(channel, fallback, sizeof(fallback));
    const char *src = (base && base[0]) ? base : fallback;

    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%.*s", BACNET_OBJECT_NAME_MAX, src);
    if (!name_exists(out, existing, existing_count)) {
        return;
    }

    char suffix[16];
    snprintf(suffix, sizeof(suffix), " (%u)", channel);
    size_t suffix_len = strlen(suffix);
    size_t max_base = BACNET_OBJECT_NAME_MAX;
    if (suffix_len < max_base) {
        max_base -= suffix_len;
    }
    snprintf(out, out_size, "%.*s%s", (int)max_base, src, suffix);

    unsigned copy = 2;
    while (name_exists(out, existing, existing_count) && copy < 1000) {
        snprintf(suffix, sizeof(suffix), " (%u-%u)", channel, copy++);
        suffix_len = strlen(suffix);
        max_base = BACNET_OBJECT_NAME_MAX;
        if (suffix_len < max_base) {
            max_base -= suffix_len;
        }
        snprintf(out, out_size, "%.*s%s", (int)max_base, src, suffix);
    }
}

void bacnet_make_ai_identity(unsigned channel, const uint8_t rom[MODBUS_ROM_LEN], const char *friendly_name, char existing[][BACNET_OBJECT_NAME_MAX + 1], size_t existing_count, bacnet_ai_identity_t *out)
{
    if (out == NULL) {
        return;
    }
    bacnet_make_unique_ai_name(friendly_name, channel, existing, existing_count,
                               out->object_name, sizeof(out->object_name));
    modbus_rom_to_hex(rom, out->description);
}
