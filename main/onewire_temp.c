/**
 * @file onewire_temp.c
 * @brief 1-Wire DS18B20 temperature sensor driver using ESP-IDF onewire_bus component
 */

#include "onewire_temp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include <string.h>

static const char *TAG = "onewire_temp";

static onewire_bus_handle_t s_bus_handle = NULL;
static ds18b20_device_handle_t *s_ds18b20_handles = NULL;
static int s_device_count = 0;
static int s_resolution = 12;

/* DS18B20 family code */
#define DS18B20_FAMILY_CODE 0x28

esp_err_t onewire_temp_init(int gpio_num)
{
    ESP_LOGD(TAG, "Initializing 1-Wire bus on GPIO %d", gpio_num);

    /* Configure 1-Wire bus */
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = gpio_num,
    };
    
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,  /* 1 byte ROM command + 8 bytes ROM + 1 byte CRC */
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize 1-Wire bus: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "1-Wire bus initialized successfully");
    return ESP_OK;
}

esp_err_t onewire_temp_scan(onewire_sensor_t *sensors, int max_sensors, int *found_count)
{
    ESP_LOGD(TAG, "Scanning for DS18B20 sensors...");
    
    int count = 0;
    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_device;

    /* Create iterator */
    esp_err_t err = onewire_new_device_iter(s_bus_handle, &iter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator");
        return err;
    }

    /* Allocate handles array if needed */
    if (s_ds18b20_handles) {
        free(s_ds18b20_handles);
    }
    s_ds18b20_handles = calloc(max_sensors, sizeof(ds18b20_device_handle_t));
    
    /* Iterate through all devices */
    while (count < max_sensors) {
        err = onewire_device_iter_get_next(iter, &next_device);
        if (err == ESP_ERR_NOT_FOUND) {
            break;  /* No more devices */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Error iterating devices: %s", esp_err_to_name(err));
            continue;
        }

        /* Check if this is a DS18B20 (family code 0x28) */
        if ((next_device.address & 0xFF) != DS18B20_FAMILY_CODE) {
            ESP_LOGD(TAG, "Skipping non-DS18B20 device");
            continue;
        }

        /* Store address in sensor struct */
        memcpy(sensors[count].address, &next_device.address, ONEWIRE_ROM_SIZE);
        sensors[count].valid = false;
        sensors[count].temperature = 0.0f;
        sensors[count].last_read_time = 0;

        /* Create DS18B20 device handle */
        ds18b20_config_t ds18b20_config = {};
        err = ds18b20_new_device(&next_device, &ds18b20_config, &s_ds18b20_handles[count]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create DS18B20 handle");
            continue;
        }

        /* Set resolution */
        ds18b20_set_resolution(s_ds18b20_handles[count], (ds18b20_resolution_t)(s_resolution - 9));

        char addr_str[17];
        onewire_address_to_string(sensors[count].address, addr_str);
        ESP_LOGD(TAG, "Found DS18B20: %s", addr_str);

        count++;
    }

    /* Clean up iterator */
    onewire_del_device_iter(iter);

    s_device_count = count;
    *found_count = count;
    
    ESP_LOGI(TAG, "Found %d DS18B20 sensor(s)", count);
    return ESP_OK;
}

esp_err_t onewire_temp_read(onewire_sensor_t *sensor)
{
    /* Find the matching device handle */
    int idx = -1;
    for (int i = 0; i < s_device_count; i++) {
        if (s_ds18b20_handles[i] != NULL) {
            /* Compare addresses - need to find by index since we stored them in order */
            idx = i;
            break;
        }
    }

    if (idx < 0 || s_ds18b20_handles[idx] == NULL) {
        ESP_LOGE(TAG, "Sensor not found or handle invalid");
        sensor->valid = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* Trigger temperature conversion */
    esp_err_t err = ds18b20_trigger_temperature_conversion(s_ds18b20_handles[idx]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger conversion");
        sensor->valid = false;
        return err;
    }

    /* Wait for conversion (depends on resolution) */
    int delay_ms = 100;
    switch (s_resolution) {
        case 9:  delay_ms = 100; break;
        case 10: delay_ms = 200; break;
        case 11: delay_ms = 400; break;
        case 12: delay_ms = 750; break;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    /* Read temperature */
    float temp;
    err = ds18b20_get_temperature(s_ds18b20_handles[idx], &temp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature");
        sensor->valid = false;
        return err;
    }

    sensor->temperature = temp;
    sensor->valid = true;
    sensor->last_read_time = esp_timer_get_time() / 1000;  /* Convert to ms */

    return ESP_OK;
}

esp_err_t onewire_temp_read_all(onewire_sensor_t *sensors, int sensor_count)
{
    if (sensor_count == 0 || sensor_count > s_device_count) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Trigger conversion on all devices simultaneously using skip ROM */
    /* This requires all devices to convert at the same time */
    for (int i = 0; i < sensor_count && i < s_device_count; i++) {
        if (s_ds18b20_handles[i] != NULL) {
            ds18b20_trigger_temperature_conversion(s_ds18b20_handles[i]);
        }
    }

    /* Wait for conversion */
    int delay_ms = 750;  /* Max delay for 12-bit resolution */
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    /* Read all temperatures */
    int64_t now = esp_timer_get_time() / 1000;
    
    for (int i = 0; i < sensor_count && i < s_device_count; i++) {
        if (s_ds18b20_handles[i] != NULL) {
            float temp;
            esp_err_t err = ds18b20_get_temperature(s_ds18b20_handles[i], &temp);
            if (err == ESP_OK) {
                sensors[i].temperature = temp;
                sensors[i].valid = true;
                sensors[i].last_read_time = now;
            } else {
                sensors[i].valid = false;
                ESP_LOGW(TAG, "Failed to read sensor %d", i);
            }
        }
    }

    return ESP_OK;
}

void onewire_address_to_string(const uint8_t *address, char *str)
{
    sprintf(str, "%02X%02X%02X%02X%02X%02X%02X%02X",
            address[0], address[1], address[2], address[3],
            address[4], address[5], address[6], address[7]);
}

int onewire_temp_get_resolution(void)
{
    return s_resolution;
}

esp_err_t onewire_temp_set_resolution(int bits)
{
    if (bits < 9 || bits > 12) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_resolution = bits;
    
    /* Update all existing devices */
    for (int i = 0; i < s_device_count; i++) {
        if (s_ds18b20_handles[i] != NULL) {
            ds18b20_set_resolution(s_ds18b20_handles[i], (ds18b20_resolution_t)(bits - 9));
        }
    }
    
    ESP_LOGD(TAG, "Resolution set to %d bits", bits);
    return ESP_OK;
}
