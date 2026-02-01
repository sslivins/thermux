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
 */
static void address_to_key(const uint8_t *address, char *key, size_t key_len)
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
