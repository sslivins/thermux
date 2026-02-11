/**
 * @file sensor_manager.c
 * @brief Sensor registry and management with friendly names
 */

#include "sensor_manager.h"
#include "nvs_storage.h"
#include "mqtt_client_ha.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "sensor_mgr";

static managed_sensor_t s_sensors[CONFIG_MAX_SENSORS];
static int s_sensor_count = 0;

/**
 * @brief Load friendly name from NVS for a sensor
 */
static void load_friendly_name(managed_sensor_t *sensor)
{
    char name[MAX_FRIENDLY_NAME_LEN];
    esp_err_t err = nvs_storage_load_sensor_name(sensor->hw_sensor.address, name, sizeof(name));
    
    if (err == ESP_OK && strlen(name) > 0) {
        strncpy(sensor->friendly_name, name, MAX_FRIENDLY_NAME_LEN - 1);
        sensor->friendly_name[MAX_FRIENDLY_NAME_LEN - 1] = '\0';
        sensor->has_friendly_name = true;
        ESP_LOGD(TAG, "Loaded friendly name for %s: %s", sensor->address_str, sensor->friendly_name);
    } else {
        sensor->friendly_name[0] = '\0';
        sensor->has_friendly_name = false;
    }
}

esp_err_t sensor_manager_init(void)
{
    ESP_LOGD(TAG, "Initializing sensor manager");
    
    memset(s_sensors, 0, sizeof(s_sensors));
    s_sensor_count = 0;

    /* Scan for sensors */
    onewire_sensor_t hw_sensors[CONFIG_MAX_SENSORS];
    int found = 0;
    
    esp_err_t err = onewire_temp_scan(hw_sensors, CONFIG_MAX_SENSORS, &found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to scan for sensors");
        return err;
    }

    /* Copy to managed sensors and load friendly names */
    for (int i = 0; i < found; i++) {
        memcpy(&s_sensors[i].hw_sensor, &hw_sensors[i], sizeof(onewire_sensor_t));
        onewire_address_to_string(s_sensors[i].hw_sensor.address, s_sensors[i].address_str);
        load_friendly_name(&s_sensors[i]);
    }
    
    s_sensor_count = found;
    ESP_LOGD(TAG, "Sensor manager initialized with %d sensors", s_sensor_count);
    
    return ESP_OK;
}

esp_err_t sensor_manager_rescan(void)
{
    ESP_LOGD(TAG, "Rescanning for sensors...");
    
    /* Save current friendly names */
    char saved_names[CONFIG_MAX_SENSORS][MAX_FRIENDLY_NAME_LEN];
    uint8_t saved_addresses[CONFIG_MAX_SENSORS][ONEWIRE_ROM_SIZE];
    int saved_count = s_sensor_count;
    
    for (int i = 0; i < s_sensor_count; i++) {
        memcpy(saved_addresses[i], s_sensors[i].hw_sensor.address, ONEWIRE_ROM_SIZE);
        strncpy(saved_names[i], s_sensors[i].friendly_name, MAX_FRIENDLY_NAME_LEN);
    }

    /* Re-scan */
    onewire_sensor_t hw_sensors[CONFIG_MAX_SENSORS];
    int found = 0;
    
    esp_err_t err = onewire_temp_scan(hw_sensors, CONFIG_MAX_SENSORS, &found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to rescan sensors");
        return err;
    }

    /* Clear and rebuild sensor list */
    memset(s_sensors, 0, sizeof(s_sensors));
    
    for (int i = 0; i < found; i++) {
        memcpy(&s_sensors[i].hw_sensor, &hw_sensors[i], sizeof(onewire_sensor_t));
        onewire_address_to_string(s_sensors[i].hw_sensor.address, s_sensors[i].address_str);
        load_friendly_name(&s_sensors[i]);
    }
    
    s_sensor_count = found;
    
    ESP_LOGD(TAG, "Rescan complete: %d sensors found", s_sensor_count);
    return ESP_OK;
}

esp_err_t sensor_manager_read_all(void)
{
    if (s_sensor_count == 0) {
        return ESP_OK;
    }

    /* Extract hardware sensor array */
    onewire_sensor_t hw_sensors[CONFIG_MAX_SENSORS];
    for (int i = 0; i < s_sensor_count; i++) {
        memcpy(&hw_sensors[i], &s_sensors[i].hw_sensor, sizeof(onewire_sensor_t));
    }

    /* Read all temperatures */
    int64_t start = esp_timer_get_time();
    esp_err_t err = onewire_temp_read_all(hw_sensors, s_sensor_count);
    int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
    
    ESP_LOGI(TAG, "Read %d sensors in %lld ms", s_sensor_count, elapsed_ms);
    
    /* Copy back results */
    for (int i = 0; i < s_sensor_count; i++) {
        s_sensors[i].hw_sensor.temperature = hw_sensors[i].temperature;
        s_sensors[i].hw_sensor.valid = hw_sensors[i].valid;
        s_sensors[i].hw_sensor.last_read_time = hw_sensors[i].last_read_time;
        s_sensors[i].hw_sensor.total_reads = hw_sensors[i].total_reads;
        s_sensors[i].hw_sensor.failed_reads = hw_sensors[i].failed_reads;
        
        if (hw_sensors[i].valid) {
            const char *name = s_sensors[i].has_friendly_name ? 
                               s_sensors[i].friendly_name : s_sensors[i].address_str;
            ESP_LOGD(TAG, "%s: %.2f°C", name, hw_sensors[i].temperature);
        }
    }

    return err;
}

esp_err_t sensor_manager_publish_all(void)
{
    int64_t start = esp_timer_get_time();
    int published = 0;
    
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].hw_sensor.valid) {
            const char *name = s_sensors[i].has_friendly_name ? 
                               s_sensors[i].friendly_name : s_sensors[i].address_str;
            
            if (mqtt_ha_publish_temperature(s_sensors[i].address_str, 
                                            name,
                                            s_sensors[i].hw_sensor.temperature) == ESP_OK) {
                published++;
            }
        }
    }
    
    /* Also publish diagnostic data (network status) */
    mqtt_ha_publish_diagnostics();
    
    int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
    ESP_LOGI(TAG, "Published %d sensors via MQTT in %lld ms", published, elapsed_ms);
    
    return ESP_OK;
}

const managed_sensor_t* sensor_manager_get_sensors(int *count)
{
    *count = s_sensor_count;
    return s_sensors;
}

esp_err_t sensor_manager_set_friendly_name(const char *address_str, const char *friendly_name)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i].address_str, address_str) == 0) {
            /* Save to NVS */
            esp_err_t err = nvs_storage_save_sensor_name(s_sensors[i].hw_sensor.address, friendly_name);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save friendly name");
                return err;
            }
            
            /* Update in memory */
            strncpy(s_sensors[i].friendly_name, friendly_name, MAX_FRIENDLY_NAME_LEN - 1);
            s_sensors[i].friendly_name[MAX_FRIENDLY_NAME_LEN - 1] = '\0';
            s_sensors[i].has_friendly_name = (strlen(friendly_name) > 0);
            
            ESP_LOGI(TAG, "Set friendly name for %s: %s", address_str, friendly_name);
            
            /* Re-register with Home Assistant if discovery is enabled */
#if CONFIG_HA_DISCOVERY_ENABLED
            mqtt_ha_register_sensor(s_sensors[i].address_str, 
                                   s_sensors[i].has_friendly_name ? 
                                   s_sensors[i].friendly_name : s_sensors[i].address_str);
#endif
            
            return ESP_OK;
        }
    }
    
    ESP_LOGE(TAG, "Sensor not found: %s", address_str);
    return ESP_ERR_NOT_FOUND;
}

const char* sensor_manager_get_display_name(const char *address_str)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i].address_str, address_str) == 0) {
            return s_sensors[i].has_friendly_name ? 
                   s_sensors[i].friendly_name : s_sensors[i].address_str;
        }
    }
    return address_str;
}

const managed_sensor_t* sensor_manager_get_sensor(const char *address_str)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i].address_str, address_str) == 0) {
            return &s_sensors[i];
        }
    }
    return NULL;
}

int sensor_manager_get_count(void)
{
    return s_sensor_count;
}
