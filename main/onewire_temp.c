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
#include "onewire_cmd.h"
#include "ds18b20.h"
#include <string.h>

static const char *TAG = "onewire_temp";

static onewire_bus_handle_t s_bus_handle = NULL;
static ds18b20_device_handle_t *s_ds18b20_handles = NULL;
static int s_device_count = 0;
static int s_resolution = 12;

/* Bus error statistics */
static uint32_t s_total_reads = 0;
static uint32_t s_failed_reads = 0;

/* DS18B20 family code and commands */
#define DS18B20_FAMILY_CODE     0x28
#define DS18B20_CMD_CONVERT     0x44

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
        sensors[count].total_reads = 0;
        sensors[count].failed_reads = 0;

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

    /* Check if we hit the limit (more devices may be on the bus) */
    if (count >= max_sensors) {
        ESP_LOGW(TAG, "Maximum sensor limit reached (%d). Additional sensors on the bus will be ignored. "
                 "Increase CONFIG_MAX_SENSORS in menuconfig to support more.", max_sensors);
    }

    s_device_count = count;
    *found_count = count;
    
    ESP_LOGI(TAG, "Found %d DS18B20 sensor(s)", count);
    return ESP_OK;
}

esp_err_t onewire_temp_read(onewire_sensor_t *sensor, int index)
{
    if (index < 0 || index >= s_device_count || s_ds18b20_handles[index] == NULL) {
        ESP_LOGE(TAG, "Invalid sensor index %d", index);
        sensor->valid = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* Trigger temperature conversion (library handles resolution-based delay) */
    esp_err_t err = ds18b20_trigger_temperature_conversion(s_ds18b20_handles[index]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger conversion for sensor %d", index);
        sensor->valid = false;
        return err;
    }

    /* Read temperature */
    float temp;
    err = ds18b20_get_temperature(s_ds18b20_handles[index], &temp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature from sensor %d", index);
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

    int64_t start_time = esp_timer_get_time();

    /* Step 1: Reset bus */
    esp_err_t err = onewire_bus_reset(s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bus reset failed");
        return err;
    }
    
    /* Step 2: Send Skip ROM + Convert command to all devices at once */
    uint8_t cmd[2] = {ONEWIRE_CMD_SKIP_ROM, DS18B20_CMD_CONVERT};
    err = onewire_bus_write_bytes(s_bus_handle, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send convert command");
        return err;
    }
    
    /* Step 3: Wait for conversion (based on resolution) */
    const int delays_ms[] = {100, 200, 400, 800};  /* 9, 10, 11, 12 bit */
    int delay_idx = s_resolution - 9;
    if (delay_idx < 0) delay_idx = 0;
    if (delay_idx > 3) delay_idx = 3;
    vTaskDelay(pdMS_TO_TICKS(delays_ms[delay_idx]));
    
    /* Step 4: Read temperature from each sensor */
    int64_t now = esp_timer_get_time() / 1000;
    esp_err_t result = ESP_OK;
    
    for (int i = 0; i < sensor_count && i < s_device_count; i++) {
        if (s_ds18b20_handles[i] != NULL) {
            float temp;
            s_total_reads++;
            sensors[i].total_reads++;
            err = ds18b20_get_temperature(s_ds18b20_handles[i], &temp);
            if (err == ESP_OK) {
                sensors[i].temperature = temp;
                sensors[i].valid = true;
                sensors[i].last_read_time = now;
            } else {
                s_failed_reads++;
                sensors[i].failed_reads++;
                sensors[i].valid = false;
                result = err;
                ESP_LOGW(TAG, "Failed to read sensor %d", i);
            }
        }
    }

    int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
    ESP_LOGD(TAG, "Read %d sensors in %lld ms", sensor_count, elapsed_ms);

    return result;
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

void onewire_temp_get_error_stats(uint32_t *total_reads, uint32_t *failed_reads)
{
    if (total_reads) *total_reads = s_total_reads;
    if (failed_reads) *failed_reads = s_failed_reads;
}

void onewire_temp_reset_error_stats(void)
{
    s_total_reads = 0;
    s_failed_reads = 0;
    ESP_LOGI(TAG, "Error statistics reset");
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
