/**
 * @file config_utils.h
 * @brief Configuration validation utilities (host-testable)
 */

#ifndef CONFIG_UTILS_H
#define CONFIG_UTILS_H

#include <stdint.h>

/* Configuration limits */
#define CONFIG_READ_INTERVAL_MIN_MS     5000      /* 5 seconds */
#define CONFIG_READ_INTERVAL_MAX_MS     300000    /* 5 minutes */
#define CONFIG_PUBLISH_INTERVAL_MIN_MS  5000      /* 5 seconds */
#define CONFIG_PUBLISH_INTERVAL_MAX_MS  600000    /* 10 minutes */
#define CONFIG_RESOLUTION_MIN           9
#define CONFIG_RESOLUTION_MAX           12

/* Friendly name limits */
#define CONFIG_FRIENDLY_NAME_MIN_LEN    1
#define CONFIG_FRIENDLY_NAME_MAX_LEN    31

/* MQTT limits */
#define CONFIG_MQTT_URI_MAX_LEN         127
#define CONFIG_MQTT_USERNAME_MAX_LEN    63
#define CONFIG_MQTT_PASSWORD_MAX_LEN    63

/* WiFi limits */
#define CONFIG_WIFI_SSID_MAX_LEN        31
#define CONFIG_WIFI_PASSWORD_MAX_LEN    63

/**
 * @brief Validation result codes
 */
typedef enum {
    CONFIG_VALID = 0,
    CONFIG_ERR_NULL_INPUT,
    CONFIG_ERR_TOO_LOW,
    CONFIG_ERR_TOO_HIGH,
    CONFIG_ERR_TOO_SHORT,
    CONFIG_ERR_TOO_LONG,
    CONFIG_ERR_INVALID_FORMAT,
    CONFIG_ERR_INVALID_CHARS,
} config_error_t;

/**
 * @brief Validate and clamp read interval
 * 
 * @param interval_ms Input interval in milliseconds
 * @param clamped_ms Output clamped value (NULL to skip)
 * @return CONFIG_VALID if within range, error code otherwise
 */
config_error_t config_validate_read_interval(uint32_t interval_ms, uint32_t *clamped_ms);

/**
 * @brief Validate and clamp publish interval
 * 
 * @param interval_ms Input interval in milliseconds
 * @param clamped_ms Output clamped value (NULL to skip)
 * @return CONFIG_VALID if within range, error code otherwise
 */
config_error_t config_validate_publish_interval(uint32_t interval_ms, uint32_t *clamped_ms);

/**
 * @brief Validate sensor resolution
 * 
 * @param resolution Resolution in bits (9-12)
 * @return CONFIG_VALID if valid, error code otherwise
 */
config_error_t config_validate_resolution(uint8_t resolution);

/**
 * @brief Validate friendly name
 * 
 * @param name Name string to validate
 * @return CONFIG_VALID if valid, error code otherwise
 */
config_error_t config_validate_friendly_name(const char *name);

/**
 * @brief Validate MQTT URI format
 * 
 * Basic validation: checks for mqtt:// or mqtts:// prefix and length
 * 
 * @param uri URI string to validate
 * @return CONFIG_VALID if valid, error code otherwise
 */
config_error_t config_validate_mqtt_uri(const char *uri);

/**
 * @brief Validate WiFi SSID
 * 
 * @param ssid SSID string to validate
 * @return CONFIG_VALID if valid, error code otherwise
 */
config_error_t config_validate_wifi_ssid(const char *ssid);

/**
 * @brief Check if string contains only printable ASCII characters
 * 
 * @param str String to check
 * @return 1 if valid, 0 if contains non-printable chars
 */
int config_is_printable_ascii(const char *str);

/**
 * @brief Get human-readable error message
 * 
 * @param error Error code
 * @return Error message string
 */
const char *config_error_str(config_error_t error);

#endif /* CONFIG_UTILS_H */
