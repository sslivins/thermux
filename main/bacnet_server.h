/**
 * @file bacnet_server.h
 * @brief Read-only BACnet/IP server exposing Thermux channels as Analog Inputs
 */

#ifndef BACNET_SERVER_H
#define BACNET_SERVER_H

#include "bacnet_map.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define BACNET_BOUND_IP_LEN 16

typedef struct {
    bool enabled;
    uint16_t udp_port;
    uint32_t device_instance;
    char device_name[BACNET_DEVICE_NAME_MAX + 1];
} bacnet_config_t;

typedef struct {
    bacnet_config_t config;
    bool running;
    char last_error[96];
    char bound_ip[BACNET_BOUND_IP_LEN];
    uint32_t packets;
    uint32_t objects;
    int64_t last_packet_ms;
} bacnet_status_t;

esp_err_t bacnet_server_init(void);
void bacnet_server_update(void);
esp_err_t bacnet_server_apply_config(const bacnet_config_t *cfg);
esp_err_t bacnet_server_restore(const bacnet_config_t *cfg);
void bacnet_server_get_status(bacnet_status_t *out);

#endif /* BACNET_SERVER_H */
