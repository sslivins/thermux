/**
 * @file bacnet_map.h
 * @brief BACnet/IP config and object mapping helpers (host-testable)
 */

#ifndef BACNET_MAP_H
#define BACNET_MAP_H

#include "modbus_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACNET_DEFAULT_UDP_PORT 47808
#define BACNET_MAX_DEVICE_INSTANCE 4194302U
#define BACNET_DEVICE_NAME_MAX 63
/* Friendly names are at most 31 chars; leaves room for a " (NN)" suffix. */
#define BACNET_OBJECT_NAME_MAX 40
#define BACNET_COV_INCREMENT_C 0.1f

typedef enum {
    BACNET_MAP_RELIABILITY_NO_FAULT_DETECTED = 0,
    BACNET_MAP_RELIABILITY_NO_SENSOR = 1,
    BACNET_MAP_RELIABILITY_COMMUNICATION_FAILURE = 2,
    BACNET_MAP_RELIABILITY_UNRELIABLE_OTHER = 3,
} bacnet_map_reliability_t;

typedef struct {
    char object_name[BACNET_OBJECT_NAME_MAX + 1];
    char description[MODBUS_ROM_LEN * 2 + 1];
} bacnet_ai_identity_t;

uint32_t bacnet_default_device_instance_from_mac(const uint8_t mac[6]);
bool bacnet_config_valid(uint32_t port, uint32_t device_instance, uint32_t reserved_port);
bacnet_map_reliability_t bacnet_reliability_from_modbus_status(uint16_t status);
bool bacnet_status_fault(uint16_t status);
void bacnet_default_channel_name(unsigned channel, char *out, size_t out_size);
void bacnet_clean_device_name(const char *input, const char *fallback, char *out, size_t out_size);
void bacnet_make_unique_ai_name(const char *base, unsigned channel, const char *const existing[], size_t existing_count, char *out, size_t out_size);
void bacnet_make_ai_identity(unsigned channel, const uint8_t rom[MODBUS_ROM_LEN], const char *friendly_name, const char *const existing[], size_t existing_count, bacnet_ai_identity_t *out);

#endif /* BACNET_MAP_H */
