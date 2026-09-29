/**
 * @file sensor_manager.c
 * @brief Sensor registry and management with friendly names
 *
 * Locking:
 *  - s_cycle_lock serializes everything that touches the 1-Wire bus through
 *    the registry (read cycles, rescans, stats resets). It is held during bus
 *    I/O, so a rescan can never swap the driver's device order while a read
 *    cycle is still using the old registry.
 *  - s_data_lock protects s_sensors / s_sensor_count / s_cycle_info and is
 *    only held for short copies, never during bus, NVS or network I/O.
 *    When both are needed, take s_cycle_lock first.
 *
 * Scratch arrays are heap-allocated and sized to the actual sensor count so
 * that large CONFIG_MAX_SENSORS values don't overflow task stacks.
 */

#include "sensor_manager.h"
#include "nvs_storage.h"
#include "mqtt_client_ha.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sensor_mgr";

#define CYCLE_LOCK_READ_TIMEOUT_MS   5000
#define CYCLE_LOCK_RESCAN_TIMEOUT_MS 15000

static managed_sensor_t s_sensors[CONFIG_MAX_SENSORS];
static int s_sensor_count = 0;
static sensor_cycle_info_t s_cycle_info = {0};

static SemaphoreHandle_t s_cycle_lock = NULL;
static SemaphoreHandle_t s_data_lock = NULL;

static inline void data_lock(void) { xSemaphoreTake(s_data_lock, portMAX_DELAY); }
static inline void data_unlock(void) { xSemaphoreGive(s_data_lock); }

static bool cycle_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_cycle_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void cycle_unlock(void) { xSemaphoreGive(s_cycle_lock); }

static inline int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }

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

static void finish_cycle(sensor_cycle_result_t result)
{
    data_lock();
    s_cycle_info.cycle_count++;
    s_cycle_info.last_result = result;
    s_cycle_info.last_cycle_time = uptime_ms();
    data_unlock();
}

/**
 * @brief Scan the bus and replace the registry. Caller holds s_cycle_lock.
 */
static esp_err_t scan_and_rebuild_locked(void)
{
    onewire_sensor_t *hw = calloc(CONFIG_MAX_SENSORS, sizeof(onewire_sensor_t));
    if (hw == NULL) {
        ESP_LOGE(TAG, "Out of memory for scan buffer");
        return ESP_ERR_NO_MEM;
    }

    int found = 0;
    esp_err_t err = onewire_temp_scan(hw, CONFIG_MAX_SENSORS, &found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sensor scan failed: %s", esp_err_to_name(err));
        free(hw);
        return err;
    }

    managed_sensor_t *fresh = NULL;
    if (found > 0) {
        fresh = calloc(found, sizeof(managed_sensor_t));
        if (fresh == NULL) {
            ESP_LOGE(TAG, "Out of memory for registry rebuild");
            free(hw);
            return ESP_ERR_NO_MEM;
        }
        /* NVS reads happen here, outside the data lock */
        for (int i = 0; i < found; i++) {
            memcpy(&fresh[i].hw_sensor, &hw[i], sizeof(onewire_sensor_t));
            onewire_address_to_string(fresh[i].hw_sensor.address, fresh[i].address_str);
            load_friendly_name(&fresh[i]);
        }
    }
    free(hw);

    data_lock();
    /* Keep the latest reading of sensors that are still present, so a rescan
       doesn't blank values (and Modbus ages) until the next read cycle */
    for (int i = 0; i < found; i++) {
        for (int j = 0; j < s_sensor_count; j++) {
            const managed_sensor_t *old = &s_sensors[j];
            if (memcmp(old->hw_sensor.address, fresh[i].hw_sensor.address, ONEWIRE_ROM_SIZE) == 0) {
                fresh[i].hw_sensor.temperature = old->hw_sensor.temperature;
                fresh[i].hw_sensor.valid = old->hw_sensor.valid;
                fresh[i].hw_sensor.last_read_time = old->hw_sensor.last_read_time;
                fresh[i].last_attempt_time = old->last_attempt_time;
                break;
            }
        }
    }
    memset(s_sensors, 0, sizeof(s_sensors));
    if (found > 0) {
        memcpy(s_sensors, fresh, found * sizeof(managed_sensor_t));
    }
    s_sensor_count = found;
    data_unlock();

    free(fresh);
    return ESP_OK;
}

esp_err_t sensor_manager_init(void)
{
    ESP_LOGD(TAG, "Initializing sensor manager");

    if (s_cycle_lock == NULL) {
        s_cycle_lock = xSemaphoreCreateMutex();
    }
    if (s_data_lock == NULL) {
        s_data_lock = xSemaphoreCreateMutex();
    }
    if (s_cycle_lock == NULL || s_data_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor manager locks");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_cycle_lock, portMAX_DELAY);
    esp_err_t err = scan_and_rebuild_locked();
    cycle_unlock();

    ESP_LOGD(TAG, "Sensor manager initialized with %d sensors", sensor_manager_get_count());
    return err;
}

esp_err_t sensor_manager_rescan(void)
{
    ESP_LOGD(TAG, "Rescanning for sensors...");

    if (!cycle_lock(CYCLE_LOCK_RESCAN_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Timed out waiting for the read cycle to finish");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = scan_and_rebuild_locked();
    cycle_unlock();

    ESP_LOGD(TAG, "Rescan complete: %d sensors found", sensor_manager_get_count());
    return err;
}

esp_err_t sensor_manager_read_all(void)
{
    if (!cycle_lock(CYCLE_LOCK_READ_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Skipping read cycle: a rescan is in progress");
        finish_cycle(SENSOR_CYCLE_FAILED);
        return ESP_ERR_TIMEOUT;
    }

    data_lock();
    int count = s_sensor_count;
    onewire_sensor_t *hw = NULL;
    if (count > 0) {
        hw = malloc(count * sizeof(onewire_sensor_t));
        if (hw) {
            for (int i = 0; i < count; i++) {
                memcpy(&hw[i], &s_sensors[i].hw_sensor, sizeof(onewire_sensor_t));
            }
        }
    }
    data_unlock();

    if (count == 0) {
        cycle_unlock();
        finish_cycle(SENSOR_CYCLE_NO_SENSORS);
        return ESP_OK;
    }
    if (hw == NULL) {
        cycle_unlock();
        ESP_LOGE(TAG, "Out of memory for read buffer");
        finish_cycle(SENSOR_CYCLE_FAILED);
        return ESP_ERR_NO_MEM;
    }

    int64_t start = esp_timer_get_time();
    esp_err_t err = onewire_temp_read_all(hw, count);
    int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
    int64_t now = uptime_ms();

    ESP_LOGI(TAG, "Read %d sensors in %lld ms", count, elapsed_ms);

    int ok = 0;
    data_lock();
    for (int i = 0; i < count; i++) {
        managed_sensor_t *s = &s_sensors[i];
        /* A sensor was attempted if the driver bumped its read counter. When
           the whole cycle fails (bus reset / convert command), none are, and
           the previous values must not keep being reported as valid. */
        bool attempted = hw[i].total_reads != s->hw_sensor.total_reads;
        s->hw_sensor.total_reads = hw[i].total_reads;
        s->hw_sensor.failed_reads = hw[i].failed_reads;
        s->hw_sensor.temperature = hw[i].temperature;
        s->hw_sensor.last_read_time = hw[i].last_read_time;
        s->hw_sensor.valid = attempted && hw[i].valid;
        s->last_attempt_time = now;
        if (s->hw_sensor.valid) {
            ok++;
            ESP_LOGD(TAG, "%s: %.2f°C",
                     s->has_friendly_name ? s->friendly_name : s->address_str,
                     s->hw_sensor.temperature);
        }
    }
    data_unlock();

    free(hw);
    cycle_unlock();

    finish_cycle(ok == count ? SENSOR_CYCLE_OK :
                 ok > 0      ? SENSOR_CYCLE_PARTIAL : SENSOR_CYCLE_FAILED);
    return err;
}

esp_err_t sensor_manager_publish_all(void)
{
    int64_t start = esp_timer_get_time();
    int published = 0;
    int count = 0;
    managed_sensor_t *sensors = sensor_manager_snapshot(&count);

    for (int i = 0; i < count; i++) {
        if (sensors[i].hw_sensor.valid) {
            const char *name = sensors[i].has_friendly_name ?
                               sensors[i].friendly_name : sensors[i].address_str;
            
            if (mqtt_ha_publish_temperature(sensors[i].address_str,
                                            name,
                                            sensors[i].hw_sensor.temperature) == ESP_OK) {
                published++;
            }
        }
    }
    free(sensors);
    
    /* Also publish diagnostic data (network status) */
    mqtt_ha_publish_diagnostics();
    
    int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
    ESP_LOGI(TAG, "Published %d sensors via MQTT in %lld ms", published, elapsed_ms);
    
    return ESP_OK;
}

managed_sensor_t *sensor_manager_snapshot(int *count)
{
    managed_sensor_t *copy = NULL;
    data_lock();
    int n = s_sensor_count;
    if (n > 0) {
        copy = malloc(n * sizeof(managed_sensor_t));
        if (copy) {
            memcpy(copy, s_sensors, n * sizeof(managed_sensor_t));
        }
    }
    data_unlock();

    if (n > 0 && copy == NULL) {
        ESP_LOGE(TAG, "Out of memory for sensor snapshot");
        n = 0;
    }
    *count = n;
    return copy;
}

void sensor_manager_get_cycle_info(sensor_cycle_info_t *out)
{
    data_lock();
    *out = s_cycle_info;
    data_unlock();
}

esp_err_t sensor_manager_set_friendly_name(const char *address_str, const char *friendly_name)
{
    managed_sensor_t sensor;
    if (sensor_manager_get_sensor(address_str, &sensor) != ESP_OK) {
        ESP_LOGE(TAG, "Sensor not found: %s", address_str);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = nvs_storage_save_sensor_name(sensor.hw_sensor.address, friendly_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save friendly name");
        return err;
    }

    char name[MAX_FRIENDLY_NAME_LEN];
    strncpy(name, friendly_name, MAX_FRIENDLY_NAME_LEN - 1);
    name[MAX_FRIENDLY_NAME_LEN - 1] = '\0';
    bool has_name = strlen(name) > 0;

    /* Update in memory (the sensor may have gone away in a rescan meanwhile) */
    data_lock();
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i].address_str, address_str) == 0) {
            strcpy(s_sensors[i].friendly_name, name);
            s_sensors[i].has_friendly_name = has_name;
            break;
        }
    }
    data_unlock();

    ESP_LOGI(TAG, "Set friendly name for %s: %s", address_str, name);

    /* Re-register with Home Assistant if discovery is enabled */
#if CONFIG_HA_DISCOVERY_ENABLED
    mqtt_ha_register_sensor(sensor.address_str, has_name ? name : sensor.address_str);
#endif

    return ESP_OK;
}

esp_err_t sensor_manager_get_sensor(const char *address_str, managed_sensor_t *out)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    data_lock();
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i].address_str, address_str) == 0) {
            *out = s_sensors[i];
            err = ESP_OK;
            break;
        }
    }
    data_unlock();
    return err;
}

int sensor_manager_get_count(void)
{
    data_lock();
    int n = s_sensor_count;
    data_unlock();
    return n;
}

void sensor_manager_reset_all_error_stats(void)
{
    /* Wait for any in-flight read so its copy-back can't restore old counts */
    bool locked = cycle_lock(CYCLE_LOCK_READ_TIMEOUT_MS);
    data_lock();
    for (int i = 0; i < s_sensor_count; i++) {
        s_sensors[i].hw_sensor.total_reads = 0;
        s_sensors[i].hw_sensor.failed_reads = 0;
    }
    data_unlock();
    if (locked) {
        cycle_unlock();
    }
    ESP_LOGI(TAG, "All per-sensor error stats reset");
}

esp_err_t sensor_manager_reset_sensor_error_stats(const char *address_str)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    bool locked = cycle_lock(CYCLE_LOCK_READ_TIMEOUT_MS);
    data_lock();
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_sensors[i].address_str, address_str) == 0) {
            s_sensors[i].hw_sensor.total_reads = 0;
            s_sensors[i].hw_sensor.failed_reads = 0;
            err = ESP_OK;
            break;
        }
    }
    data_unlock();
    if (locked) {
        cycle_unlock();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Error stats reset for %s", address_str);
    }
    return err;
}