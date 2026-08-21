/**
 * @file nvs_storage.c
 * @brief NVS storage implementation for persistent configuration
 */

#include "nvs_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nvs_storage";
static const char *NVS_NAMESPACE = "temp_monitor";

esp_err_t nvs_storage_init(void)
{
    ESP_LOGD(TAG, "Initializing NVS storage");
    return ESP_OK;
}

/**
 * @brief Convert sensor address to NVS key string
 *
 * A DS18B20 8-byte ROM is [0]=family code (constant 0x28), [1..6]=unique
 * 48-bit serial, [7]=CRC (derived from the other bytes). The key must use
 * the full unique serial to avoid collisions between sensors: bytes [1..6].
 * Format "s_XXXXXXXXXXXX" = 14 chars, within the 15-char NVS key limit.
 *
 * (Previously this used only bytes [4..7], dropping unique serial bytes
 * [1..3] and mixing in the redundant CRC, so two sensors that shared bytes
 * [4..7] collided to the same key -- naming one aliased the other.)
 */
static void address_to_key(const uint8_t *address, char *key, size_t key_len)
{
    snprintf(key, key_len, "s_%02x%02x%02x%02x%02x%02x",
             address[1], address[2], address[3],
             address[4], address[5], address[6]);
}

/**
 * @brief Legacy (pre-fix) NVS key format
 *
 * The original code keyed on bytes [4..7] only ("s_XXXXXXXX", 10 chars). That
 * dropped unique serial bytes [1..3] and caused collisions. We still compute
 * it so existing devices can migrate their saved names to the new key format
 * on first boot (see nvs_storage_load_sensor_name).
 */
static void address_to_legacy_key(const uint8_t *address, char *key, size_t key_len)
{
    snprintf(key, key_len, "s_%02x%02x%02x%02x",
             address[4], address[5], address[6], address[7]);
}

esp_err_t nvs_storage_save_sensor_name(const uint8_t *sensor_address, const char *friendly_name)
{
    nvs_handle_t handle;
    esp_err_t err;
    char key[16];

    address_to_key(sensor_address, key, sizeof(key));

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, friendly_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save sensor name: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGD(TAG, "Saved sensor name: %s -> %s", key, friendly_name);
    return err;
}

esp_err_t nvs_storage_load_sensor_name(const uint8_t *sensor_address, char *friendly_name, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err;
    char key[16];

    address_to_key(sensor_address, key, sizeof(key));

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_str(handle, key, friendly_name, &required_size);
    nvs_close(handle);

    if (err == ESP_OK) {
        return err;
    }

    /* New key not found: attempt a one-time migration from the legacy key
     * format (bytes 4-7). If a name was saved under the old scheme, copy it to
     * the new key and erase the legacy entry so it doesn't linger. This runs
     * transparently the first time each connected sensor's name is loaded after
     * updating firmware. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        char legacy_key[16];
        address_to_legacy_key(sensor_address, legacy_key, sizeof(legacy_key));

        nvs_handle_t mh;
        esp_err_t merr = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &mh);
        if (merr != ESP_OK) {
            return err;  /* report original not-found */
        }

        size_t legacy_size = max_len;
        merr = nvs_get_str(mh, legacy_key, friendly_name, &legacy_size);
        if (merr == ESP_OK) {
            /* Re-key: write under the new key, then drop the legacy entry. */
            esp_err_t set_err = nvs_set_str(mh, key, friendly_name);
            if (set_err == ESP_OK) {
                nvs_erase_key(mh, legacy_key);
                nvs_commit(mh);
                ESP_LOGI(TAG, "Migrated sensor name %s -> %s (%s)",
                         legacy_key, key, friendly_name);
                err = ESP_OK;
            } else {
                /* Migration write failed: keep the legacy value we read so the
                 * name still shows this boot; retry migration next time. */
                ESP_LOGW(TAG, "Failed to migrate sensor name %s: %s",
                         legacy_key, esp_err_to_name(set_err));
                err = ESP_OK;
            }
        }
        nvs_close(mh);
    }

    return err;
}

esp_err_t nvs_storage_delete_sensor_name(const uint8_t *sensor_address)
{
    nvs_handle_t handle;
    esp_err_t err;
    char key[16];

    address_to_key(sensor_address, key, sizeof(key));

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

esp_err_t nvs_storage_enumerate_sensor_names(nvs_storage_sensor_name_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_STR, &it);

    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        /* Only the new-format keys ("s_" + 12 hex chars = 14 chars total).
         * Skip legacy 10-char keys ("s_" + 8 hex chars) - if one still
         * lingers it will be migrated to the new format the next time that
         * sensor's name is loaded (see nvs_storage_load_sensor_name), so it
         * would otherwise show up as a stale duplicate here. */
        if (strncmp(info.key, "s_", 2) == 0 && strlen(info.key) == 14) {
            nvs_handle_t handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
                char value[MAX_FRIENDLY_NAME_LEN];
                size_t len = sizeof(value);
                if (nvs_get_str(handle, info.key, value, &len) == ESP_OK) {
                    cb(info.key + 2, value, ctx);
                }
                nvs_close(handle);
            }
        }

        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return ESP_OK;
}

esp_err_t nvs_storage_save_sensor_name_by_serial(const char *serial_hex, const char *friendly_name)
{
    if (serial_hex == NULL || strlen(serial_hex) != 12) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    snprintf(key, sizeof(key), "s_%s", serial_hex);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, friendly_name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    } else {
        ESP_LOGE(TAG, "Failed to save sensor name by serial: %s", esp_err_to_name(err));
    }
    nvs_close(handle);

    return err;
}

esp_err_t nvs_storage_save_mqtt_config(const char *broker_uri, const char *username, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(handle, "mqtt_uri", broker_uri);
    nvs_set_str(handle, "mqtt_user", username);
    nvs_set_str(handle, "mqtt_pass", password);

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved MQTT configuration");
    return err;
}

esp_err_t nvs_storage_load_mqtt_config(char *broker_uri, size_t uri_len,
                                        char *username, size_t user_len,
                                        char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = uri_len;
    nvs_get_str(handle, "mqtt_uri", broker_uri, &len);
    
    len = user_len;
    nvs_get_str(handle, "mqtt_user", username, &len);
    
    len = pass_len;
    nvs_get_str(handle, "mqtt_pass", password, &len);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_storage_save_wifi_config(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(handle, "wifi_ssid", ssid);
    nvs_set_str(handle, "wifi_pass", password);

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved WiFi configuration");
    return err;
}

esp_err_t nvs_storage_load_wifi_config(char *ssid, size_t ssid_len,
                                        char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_len;
    nvs_get_str(handle, "wifi_ssid", ssid, &len);
    
    len = pass_len;
    nvs_get_str(handle, "wifi_pass", password, &len);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_storage_factory_reset(void)
{
    ESP_LOGW(TAG, "Performing factory reset - erasing all configuration!");
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "Factory reset complete");
    return err;
}

esp_err_t nvs_storage_save_sensor_settings(uint32_t read_interval_ms, uint32_t publish_interval_ms, uint8_t resolution)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, "read_interval", read_interval_ms);
    nvs_set_u32(handle, "pub_interval", publish_interval_ms);
    nvs_set_u8(handle, "resolution", resolution);

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGD(TAG, "Saved sensor settings: read=%lums, publish=%lums, resolution=%d bits",
             read_interval_ms, publish_interval_ms, resolution);
    return err;
}

esp_err_t nvs_storage_load_sensor_settings(uint32_t *read_interval_ms, uint32_t *publish_interval_ms, uint8_t *resolution)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(handle, "read_interval", read_interval_ms);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    nvs_get_u32(handle, "pub_interval", publish_interval_ms);
    nvs_get_u8(handle, "resolution", resolution);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_storage_save_auth_config(bool enabled, const char *username, const char *password, const char *api_key)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(handle, "auth_enabled", enabled ? 1 : 0);
    nvs_set_str(handle, "auth_user", username);
    nvs_set_str(handle, "auth_pass", password);
    if (api_key != NULL && strlen(api_key) > 0) {
        nvs_set_str(handle, "api_key", api_key);
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved auth configuration (enabled=%d)", enabled);
    return err;
}

esp_err_t nvs_storage_load_auth_config(bool *enabled, char *username, size_t username_len,
                                        char *password, size_t password_len,
                                        char *api_key, size_t api_key_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t auth_enabled = 0;
    err = nvs_get_u8(handle, "auth_enabled", &auth_enabled);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    *enabled = (auth_enabled != 0);

    size_t len = username_len;
    nvs_get_str(handle, "auth_user", username, &len);

    len = password_len;
    nvs_get_str(handle, "auth_pass", password, &len);

    /* Load API key if buffer provided */
    if (api_key != NULL && api_key_len > 0) {
        len = api_key_len;
        err = nvs_get_str(handle, "api_key", api_key, &len);
        if (err != ESP_OK) {
            api_key[0] = '\0';  /* No API key stored */
        }
    }

    nvs_close(handle);
    return ESP_OK;
}
