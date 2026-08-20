/**
 * @file onewire_temp.c
 * @brief 1-Wire DS18B20 temperature sensor driver using ESP-IDF onewire_bus component
 */

#include "onewire_temp.h"
#include "bus_stats.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "onewire_cmd.h"
#include "onewire_crc.h"
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
static bus_stats_window_t s_recent_reads = {0};
static uint32_t s_consecutive_failed_cycles = 0;
static int64_t s_last_successful_read_us = 0;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

/* DS18B20 family code and commands */
#define DS18B20_FAMILY_CODE     0x28
#define DS18B20_CMD_CONVERT     0x44
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE

/*
 * Genuine-chip detection.
 *
 * Most "knock-off"/counterfeit DS18B20 sensors sold on ebay/AliExpress/Amazon
 * are clones that don't fully replicate the internal behavior of the
 * authentic Maxim/Analog Devices part. This is a best-effort heuristic (not
 * a guarantee) based on checks documented in Chris Petrich's
 * "counterfeit_DS18B20" project (https://github.com/cpetrich/counterfeit_DS18B20):
 *
 *   1. ROM pattern: genuine parts follow 28-xx-xx-xx-xx-00-00-xx, i.e. the
 *      two serial bytes immediately before the CRC byte (address[5] and
 *      address[6]) are always 0x00.
 *   2. Scratchpad bytes 6/7 (COUNT_REMAIN / COUNT_PER_C): on a genuine part
 *      COUNT_PER_C is always 0x10 and COUNT_REMAIN never exceeds it. Many
 *      clones leave these bytes fixed at the wrong value or violate that
 *      relationship.
 *   3. 12-bit conversion time: the time a chip actually takes to complete a
 *      12-bit temperature conversion is a reproducible side-channel that's
 *      characteristic of the underlying silicon and hard for a clone to
 *      fake without copying Maxim's exact die design. Authentic parts
 *      (Family A1) consistently take ~580-615ms. Many clone families are
 *      dramatically faster (as low as ~11ms for Family D1, ~28-30ms for
 *      Family C, etc.) because they don't replicate the original's slow
 *      ADC/analog design. A sensor converting in under ~400ms cannot be an
 *      authentic part, even if it fakes the ROM pattern/scratchpad bytes.
 *      This check is only meaningful when the sensor is in 12-bit
 *      resolution mode, since conversion time scales with resolution.
 *
 * A sensor failing any check is flagged as "not genuine" so the UI can
 * surface a non-blocking notice. This never prevents the sensor from being
 * used - it's purely informational.
 *
 * The raw COUNT_REMAIN/COUNT_PER_C bytes and measured conversion time (and
 * whether each check was able to complete) are written back to the sensor
 * struct so they can be inspected remotely via /api/sensors, since some
 * clone families forge the ROM pattern and scratchpad too, and a plain
 * pass/fail bool isn't enough to diagnose what's actually happening on a
 * sensor that "shouldn't" be genuine.
 */

/** Below this conversion time (ms), a 12-bit reading cannot be from an
 *  authentic DS18B20 (which take ~580-615ms per Petrich's measurements).
 *  Set comfortably below the authentic range to avoid false positives from
 *  minor timing jitter. */
#define DS18B20_MIN_GENUINE_CONVERSION_MS 400
/** Generous upper bound in case a sensor's busy line never comes back;
 *  avoids hanging the scan loop indefinitely on a misbehaving device. */
#define DS18B20_MAX_CONVERSION_WAIT_MS 1000

/**
 * @brief Measure how long a 12-bit temperature conversion actually takes.
 *
 * Issues Match ROM + Convert T, then polls the bus (a genuine/most clone
 * DS18B20 pulls the line low while busy and releases it to high once the
 * conversion completes) until it goes high or DS18B20_MAX_CONVERSION_WAIT_MS
 * elapses. Assumes the device is in external-power mode (not parasitic),
 * which is the case for all sensors driven by this project's driver.
 *
 * @return Measured time in milliseconds, or -1 if the measurement couldn't
 *         be completed (bus error, or busy line never released).
 */
static int ds18b20_measure_conversion_time_ms(const uint8_t *address)
{
    uint8_t tx_buffer[10];
    tx_buffer[0] = ONEWIRE_CMD_MATCH_ROM;
    memcpy(&tx_buffer[1], address, ONEWIRE_ROM_SIZE);
    tx_buffer[9] = DS18B20_CMD_CONVERT;

    if (onewire_bus_reset(s_bus_handle) != ESP_OK) {
        return -1;
    }
    if (onewire_bus_write_bytes(s_bus_handle, tx_buffer, sizeof(tx_buffer)) != ESP_OK) {
        return -1;
    }

    int64_t start_us = esp_timer_get_time();
    int64_t deadline_us = start_us + (int64_t)DS18B20_MAX_CONVERSION_WAIT_MS * 1000;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t bit = 0;
        if (onewire_bus_read_bit(s_bus_handle, &bit) != ESP_OK) {
            return -1;
        }
        if (bit) {
            /* Conversion complete */
            return (int)((esp_timer_get_time() - start_us) / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return -1;  /* Never completed within the wait window */
}

static bool ds18b20_check_genuine(const uint8_t *address, bool *check_ok,
                                   uint8_t *count_remain_out, uint8_t *count_per_c_out,
                                   int *conversion_time_ms_out)
{
    *check_ok = false;
    *count_remain_out = 0;
    *count_per_c_out = 0;
    *conversion_time_ms_out = -1;

    /* Check 1: ROM pattern 28-xx-xx-xx-xx-00-00-xx */
    if (address[5] != 0x00 || address[6] != 0x00) {
        return false;
    }

    /* Check 2: read the scratchpad and inspect COUNT_REMAIN/COUNT_PER_C */
    uint8_t tx_buffer[10];
    tx_buffer[0] = ONEWIRE_CMD_MATCH_ROM;
    memcpy(&tx_buffer[1], address, ONEWIRE_ROM_SIZE);
    tx_buffer[9] = DS18B20_CMD_READ_SCRATCHPAD;

    if (onewire_bus_reset(s_bus_handle) != ESP_OK) {
        /* Can't verify - assume genuine rather than falsely flagging it */
        return true;
    }
    if (onewire_bus_write_bytes(s_bus_handle, tx_buffer, sizeof(tx_buffer)) != ESP_OK) {
        return true;
    }

    uint8_t scratchpad[9];
    if (onewire_bus_read_bytes(s_bus_handle, scratchpad, sizeof(scratchpad)) != ESP_OK) {
        return true;
    }
    if (onewire_crc8(0, scratchpad, 8) != scratchpad[8]) {
        /* CRC failure - don't make a genuineness claim off bad data */
        return true;
    }


    *check_ok = true;
    uint8_t count_remain = scratchpad[6];
    uint8_t count_per_c = scratchpad[7];
    *count_remain_out = count_remain;
    *count_per_c_out = count_per_c;
    if (count_per_c != 0x10 || count_remain > count_per_c) {
        return false;
    }

    /* Check 3: 12-bit conversion timing side-channel. Only meaningful when
     * running at 12-bit resolution, since conversion time scales with it. */
    if (s_resolution == 12) {
        int conv_ms = ds18b20_measure_conversion_time_ms(address);
        *conversion_time_ms_out = conv_ms;
        if (conv_ms >= 0 && conv_ms < DS18B20_MIN_GENUINE_CONVERSION_MS) {
            return false;
        }
    }

    return true;
}

static void record_read_attempt(bool failed)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_total_reads++;
    if (failed) {
        s_failed_reads++;
    }
    bus_stats_window_record(&s_recent_reads, failed);
    portEXIT_CRITICAL(&s_stats_lock);
}

static void record_read_cycle(bool had_successful_read)
{
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_stats_lock);
    if (had_successful_read) {
        s_last_successful_read_us = now_us;
        s_consecutive_failed_cycles = 0;
    } else {
        s_consecutive_failed_cycles++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

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
        sensors[count].genuine = ds18b20_check_genuine(sensors[count].address,
                                                         &sensors[count].genuine_check_ok,
                                                         &sensors[count].count_remain,
                                                         &sensors[count].count_per_c,
                                                         &sensors[count].conversion_time_ms);

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
        if (!sensors[count].genuine) {
            ESP_LOGW(TAG, "Sensor %s does not match a genuine DS18B20 pattern (likely a clone)", addr_str);
        }
        if (!sensors[count].genuine_check_ok) {
            ESP_LOGW(TAG, "Sensor %s: scratchpad genuineness check could not complete (bus/CRC error) - assuming genuine", addr_str);
        } else {
            ESP_LOGD(TAG, "Sensor %s scratchpad: COUNT_REMAIN=0x%02X COUNT_PER_C=0x%02X",
                     addr_str, sensors[count].count_remain, sensors[count].count_per_c);
        }
        if (sensors[count].conversion_time_ms >= 0) {
            ESP_LOGD(TAG, "Sensor %s 12-bit conversion time: %dms", addr_str, sensors[count].conversion_time_ms);
            if (sensors[count].conversion_time_ms < DS18B20_MIN_GENUINE_CONVERSION_MS) {
                ESP_LOGW(TAG, "Sensor %s converted in %dms, too fast for a genuine DS18B20 (likely a clone)",
                         addr_str, sensors[count].conversion_time_ms);
            }
        }

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
        record_read_cycle(false);
        return err;
    }
    
    /* Step 2: Send Skip ROM + Convert command to all devices at once */
    uint8_t cmd[2] = {ONEWIRE_CMD_SKIP_ROM, DS18B20_CMD_CONVERT};
    err = onewire_bus_write_bytes(s_bus_handle, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send convert command");
        record_read_cycle(false);
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
    bool had_successful_read = false;
    
    for (int i = 0; i < sensor_count && i < s_device_count; i++) {
        if (s_ds18b20_handles[i] != NULL) {
            float temp;
            sensors[i].total_reads++;
            err = ds18b20_get_temperature(s_ds18b20_handles[i], &temp);
            record_read_attempt(err != ESP_OK);
            if (err == ESP_OK) {
                sensors[i].temperature = temp;
                sensors[i].valid = true;
                sensors[i].last_read_time = now;
                had_successful_read = true;
            } else {
                sensors[i].failed_reads++;
                sensors[i].valid = false;
                result = err;
                ESP_LOGW(TAG, "Failed to read sensor %d", i);
            }
        }
    }

    record_read_cycle(had_successful_read);

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
    portENTER_CRITICAL(&s_stats_lock);
    if (total_reads) *total_reads = s_total_reads;
    if (failed_reads) *failed_reads = s_failed_reads;
    portEXIT_CRITICAL(&s_stats_lock);
}

void onewire_temp_get_bus_health(onewire_bus_health_t *health)
{
    if (health == NULL) {
        return;
    }

    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_stats_lock);
    bus_stats_window_get(&s_recent_reads,
                         &health->recent_total_reads,
                         &health->recent_failed_reads);
    health->consecutive_failed_cycles = s_consecutive_failed_cycles;
    health->has_successful_read = s_last_successful_read_us > 0;
    health->seconds_since_last_success = health->has_successful_read
        ? (uint64_t)((now_us - s_last_successful_read_us) / 1000000)
        : 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

void onewire_temp_reset_error_stats(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_total_reads = 0;
    s_failed_reads = 0;
    bus_stats_window_reset(&s_recent_reads);
    s_consecutive_failed_cycles = 0;
    portEXIT_CRITICAL(&s_stats_lock);
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
