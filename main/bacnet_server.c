/**
 * @file bacnet_server.c
 * @brief Read-only BACnet/IP server exposing Thermux channels as Analog Inputs
 */

#include "bacnet_server.h"

#include "modbus_server.h"
#include "nvs_storage.h"
#include "sensor_manager.h"

#include "bacnet/apdu.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/npdu.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bacnet";

/* Same namespace as the rest of the settings, so factory reset clears these. */
#define NVS_NS "temp_monitor"
#define NVS_KEY_ENABLED "bn_enabled"
#define NVS_KEY_PORT "bn_port"
#define NVS_KEY_INSTANCE "bn_inst"
#define NVS_KEY_NAME "bn_name"

/* Measured peak ~4.9 KB (RPM "all", confirmed COV, Who-Has) with the string
 * limits set in components/bacnet-stack/CMakeLists.txt; see stack_free_min. */
#define TASK_STACK 6656
#define TASK_PRIO 4
#define STOP_WAIT_MS 2000
#define START_WAIT_MS 3000
#define LOOP_TIMEOUT_MS 20
#define PDU_BUFFER_SIZE 1500

extern const char *APP_VERSION;

static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_snapshot_lock;
static SemaphoreHandle_t s_start_sem;
static TaskHandle_t s_task;
static volatile bool s_stop_task;
static esp_err_t s_task_start_err;

static bacnet_config_t s_cfg = {
    .enabled = false,
    .udp_port = BACNET_DEFAULT_UDP_PORT,
    .device_instance = 0,
    .device_name = "",
};
static bool s_running;
static char s_last_error[96];
static char s_bound_ip[BACNET_BOUND_IP_LEN];
static uint32_t s_packets;
static uint32_t s_objects;
static int64_t s_last_packet_ms;
static uint8_t s_mac[6];

/* Working tables, heap-allocated only while the server runs so a unit with
 * BACnet disabled pays (almost) nothing. Guarded by s_snapshot_lock:
 * bacnet_server_update() fills the pending part, the BACnet task applies it.
 * Object names/descriptions are allocated per active object because the
 * stack keeps the pointers (Analog_Input_Name_Set does not copy). */
typedef struct {
    modbus_channel_table_t channels;
    uint16_t status[MODBUS_CHANNEL_COUNT];
    uint16_t temp[MODBUS_CHANNEL_COUNT];
    bool dirty;
    char *ai_name[MODBUS_CHANNEL_COUNT];
    char *ai_desc[MODBUS_CHANNEL_COUNT];
} bacnet_tables_t;

static bacnet_tables_t *s_tbl;

static void tables_free_locked(void)
{
    if (s_tbl == NULL) return;
    for (unsigned ch = 0; ch < MODBUS_CHANNEL_COUNT; ch++) {
        free(s_tbl->ai_name[ch]);
        free(s_tbl->ai_desc[ch]);
    }
    free(s_tbl);
    s_tbl = NULL;
}

static void refresh_pending_locked(void)
{
    modbus_server_get_channels(&s_tbl->channels);
    modbus_server_get_channel_regs(s_tbl->status, s_tbl->temp, NULL);
    s_tbl->dirty = true;
}

static inline int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }
static void state_lock(void) { xSemaphoreTake(s_state_lock, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_state_lock); }

/* A real Thermux product should request its own ASHRAE BACnet vendor ID. */
#define THERMUX_BACNET_VENDOR_ID 260

static object_functions_t s_object_table[] = {
    { OBJECT_DEVICE,
      NULL,
      Device_Count,
      Device_Index_To_Instance,
      Device_Valid_Object_Instance_Number,
      Device_Object_Name,
      Device_Read_Property_Local,
      NULL,
      Device_Property_Lists,
      DeviceGetRRInfo,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      Device_Writable_Property_List },
    { OBJECT_ANALOG_INPUT,
      Analog_Input_Init,
      Analog_Input_Count,
      Analog_Input_Index_To_Instance,
      Analog_Input_Valid_Instance,
      Analog_Input_Object_Name,
      Analog_Input_Read_Property,
      NULL,
      Analog_Input_Property_Lists,
      NULL,
      NULL,
      Analog_Input_Encode_Value_List,
      Analog_Input_Change_Of_Value,
      Analog_Input_Change_Of_Value_Clear,
      NULL,
      NULL,
      NULL,
      Analog_Input_Create,
      Analog_Input_Delete,
      NULL,
      Analog_Input_Writable_Property_List },
    { MAX_BACNET_OBJECT_TYPE, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static void load_config(bacnet_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t en = 0;
    uint16_t port = 0;
    uint32_t inst = 0;
    size_t len = sizeof(cfg->device_name);
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &en) == ESP_OK) cfg->enabled = en != 0;
    if (nvs_get_u16(h, NVS_KEY_PORT, &port) == ESP_OK) cfg->udp_port = port;
    if (nvs_get_u32(h, NVS_KEY_INSTANCE, &inst) == ESP_OK) cfg->device_instance = inst;
    (void)nvs_get_str(h, NVS_KEY_NAME, cfg->device_name, &len);
    nvs_close(h);
    if (!bacnet_config_valid(cfg->udp_port, cfg->device_instance, CONFIG_WEB_SERVER_PORT)) {
        cfg->udp_port = BACNET_DEFAULT_UDP_PORT;
        cfg->device_instance = bacnet_default_device_instance_from_mac(s_mac);
    }
}

static esp_err_t save_config(const bacnet_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_ENABLED, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(h, NVS_KEY_PORT, cfg->udp_port);
    if (err == ESP_OK) err = nvs_set_u32(h, NVS_KEY_INSTANCE, cfg->device_instance);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_NAME, cfg->device_name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void set_last_error(esp_err_t err, const bacnet_config_t *cfg)
{
    state_lock();
    if (err == ESP_OK) {
        s_last_error[0] = '\0';
    } else {
        snprintf(s_last_error, sizeof(s_last_error), "Could not start UDP %u (%s)", cfg->udp_port, esp_err_to_name(err));
    }
    state_unlock();
}

static BACNET_RELIABILITY reliability_from_status(uint16_t status)
{
    switch (bacnet_reliability_from_modbus_status(status)) {
    case BACNET_MAP_RELIABILITY_NO_FAULT_DETECTED: return RELIABILITY_NO_FAULT_DETECTED;
    case BACNET_MAP_RELIABILITY_NO_SENSOR: return RELIABILITY_NO_SENSOR;
    case BACNET_MAP_RELIABILITY_COMMUNICATION_FAILURE: return RELIABILITY_COMMUNICATION_FAILURE;
    case BACNET_MAP_RELIABILITY_UNRELIABLE_OTHER:
    default: return RELIABILITY_UNRELIABLE_OTHER;
    }
}

static float temp_from_reg(uint16_t reg)
{
    if (reg == MB_TEMP_INVALID) return NAN;
    int16_t signed_reg = (int16_t)reg;
    return (float)signed_reg / 100.0f;
}

static void configure_device(const bacnet_config_t *cfg)
{
    char name[BACNET_DEVICE_NAME_MAX + 1];
    bacnet_clean_device_name(cfg->device_name, CONFIG_MDNS_HOSTNAME, name, sizeof(name));
    BACNET_CHARACTER_STRING object_name;
    characterstring_init_ansi(&object_name, name);

    Device_Init(s_object_table);
    Device_Set_Object_Instance_Number(cfg->device_instance);
    Device_Set_Object_Name(&object_name);
    Device_Set_Vendor_Name("Thermux", strlen("Thermux"));
    Device_Set_Vendor_Identifier(THERMUX_BACNET_VENDOR_ID);
    Device_Set_Model_Name("Thermux", strlen("Thermux"));
    Device_Set_Firmware_Revision(APP_VERSION, strlen(APP_VERSION));
    Device_Set_Application_Software_Version(APP_VERSION, strlen(APP_VERSION));
    Device_Set_Description("Thermux temperature hub", strlen("Thermux temperature hub"));
    Device_Reinitialize_Password_Set("disabled");
}

static void register_handlers(void)
{
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    handler_cov_init();
}

/* Friendly name for a ROM: live sensor list first, then the saved name (the
 * sensor may be missing right now). */
static bool lookup_name(const uint8_t rom[MODBUS_ROM_LEN], const managed_sensor_t *sensors, int count,
                        char *out, size_t out_size)
{
    for (int i = 0; sensors != NULL && i < count; i++) {
        if (memcmp(sensors[i].hw_sensor.address, rom, MODBUS_ROM_LEN) == 0) {
            if (!sensors[i].has_friendly_name) break;
            strlcpy(out, sensors[i].friendly_name, out_size);
            return true;
        }
    }
    return nvs_storage_load_sensor_name(rom, out, out_size) == ESP_OK;
}

static bool replace_string(char **slot, const char *value)
{
    if (*slot != NULL && strcmp(*slot, value) == 0) return false;
    char *copy = strdup(value);
    if (copy == NULL) return false;
    free(*slot);
    *slot = copy;
    return true;
}

static void apply_snapshot_to_stack(void)
{
    if (xSemaphoreTake(s_snapshot_lock, pdMS_TO_TICKS(5)) != pdTRUE) return;
    if (s_tbl == NULL || !s_tbl->dirty) {
        xSemaphoreGive(s_snapshot_lock);
        return;
    }
    /* Applied in place while holding the lock; bacnet_server_update() just
     * waits briefly. */
    int sensor_count = 0;
    managed_sensor_t *sensors = sensor_manager_snapshot(&sensor_count);
    const modbus_channel_table_t *channels = &s_tbl->channels;
    uint32_t object_count = 0;
    bool revision_needed = false;

    for (unsigned ch = 0; ch < MODBUS_CHANNEL_COUNT; ch++) {
        bool active = s_tbl->ai_name[ch] != NULL;
        if (!channels->channels[ch].assigned) {
            if (active) {
                Analog_Input_Delete(ch);
                free(s_tbl->ai_name[ch]);
                free(s_tbl->ai_desc[ch]);
                s_tbl->ai_name[ch] = NULL;
                s_tbl->ai_desc[ch] = NULL;
                revision_needed = true;
            }
            continue;
        }
        char friendly[MAX_FRIENDLY_NAME_LEN];
        bool has_name = lookup_name(channels->channels[ch].rom, sensors, sensor_count, friendly, sizeof(friendly));
        /* ai_name[0..ch-1] already hold this pass's final names (NULL for
         * unused channels), so they double as the uniqueness list. */
        bacnet_ai_identity_t ident;
        bacnet_make_ai_identity(ch, channels->channels[ch].rom, has_name ? friendly : NULL,
                                (const char *const *)s_tbl->ai_name, ch, &ident);

        if (!active) {
            if (!replace_string(&s_tbl->ai_name[ch], ident.object_name)) continue; /* out of memory: retry next cycle */
            Analog_Input_Create(ch);
            Analog_Input_Units_Set(ch, UNITS_DEGREES_CELSIUS);
            Analog_Input_Out_Of_Service_Set(ch, false);
            Analog_Input_COV_Increment_Set(ch, BACNET_COV_INCREMENT_C);
            Analog_Input_Name_Set(ch, s_tbl->ai_name[ch]);
            revision_needed = true;
        } else if (replace_string(&s_tbl->ai_name[ch], ident.object_name)) {
            Analog_Input_Name_Set(ch, s_tbl->ai_name[ch]);
            revision_needed = true;
        }
        if (replace_string(&s_tbl->ai_desc[ch], ident.description)) {
            Analog_Input_Description_Set(ch, s_tbl->ai_desc[ch]);
            revision_needed = true;
        }

        float value = temp_from_reg(s_tbl->temp[ch]);
        if (!isnan(value)) {
            Analog_Input_Present_Value_Set(ch, value);
        }
        Analog_Input_Reliability_Set(ch, reliability_from_status(s_tbl->status[ch]));
        object_count++;
    }
    s_tbl->dirty = false;
    xSemaphoreGive(s_snapshot_lock);
    free(sensors);
    if (revision_needed) {
        Device_Inc_Database_Revision();
    }
    state_lock();
    s_objects = object_count;
    state_unlock();
}

bool bip_espidf_refresh_address(void);

static void update_bound_ip(void)
{
    BACNET_IP_ADDRESS addr;
    if (bip_get_addr(&addr)) {
        snprintf(s_bound_ip, sizeof(s_bound_ip), "%u.%u.%u.%u", addr.address[0], addr.address[1], addr.address[2], addr.address[3]);
    } else {
        s_bound_ip[0] = '\0';
    }
}

static void bacnet_task(void *arg)
{
    bacnet_config_t cfg = *(bacnet_config_t *)arg;
    free(arg);

    uint8_t rx[PDU_BUFFER_SIZE];
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    tables_free_locked();
    s_tbl = calloc(1, sizeof(*s_tbl));
    if (s_tbl != NULL) {
        refresh_pending_locked();
    }
    xSemaphoreGive(s_snapshot_lock);
    if (s_tbl == NULL) {
        s_task_start_err = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_start_sem);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    bip_set_port(cfg.udp_port);
    configure_device(&cfg);
    register_handlers();
    if (!bip_init(NULL)) {
        Analog_Input_Cleanup();
        xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
        tables_free_locked();
        xSemaphoreGive(s_snapshot_lock);
        s_task_start_err = ESP_FAIL;
        xSemaphoreGive(s_start_sem);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    update_bound_ip();
    Send_I_Am(&rx[0]);

    state_lock();
    s_running = true;
    s_packets = 0;
    s_last_packet_ms = 0;
    state_unlock();

    s_task_start_err = ESP_OK;
    xSemaphoreGive(s_start_sem);
    uint32_t cov_elapsed_ms = 0;
    uint32_t addr_check_ms = 0;
    int64_t last_ms = uptime_ms();
    while (!s_stop_task) {
        apply_snapshot_to_stack();
        BACNET_ADDRESS src = {0};
        uint16_t pdu_len = bip_receive(&src, rx, sizeof(rx), LOOP_TIMEOUT_MS);
        if (pdu_len > 0) {
            npdu_handler(&src, rx, pdu_len);
            state_lock();
            s_packets++;
            s_last_packet_ms = uptime_ms();
            state_unlock();
        }
        int64_t now_ms = uptime_ms();
        uint32_t elapsed_ms = (uint32_t)(now_ms - last_ms);
        if (elapsed_ms == 0 && pdu_len == 0) {
            /* receive returned at once (e.g. socket error while the link is
             * down): yield so lower-priority tasks and the idle task run. */
            vTaskDelay(pdMS_TO_TICKS(LOOP_TIMEOUT_MS));
            now_ms = uptime_ms();
            elapsed_ms = (uint32_t)(now_ms - last_ms);
        }
        last_ms = now_ms;
        tsm_timer_milliseconds(elapsed_ms);
        Device_Timer(elapsed_ms);
        cov_elapsed_ms += elapsed_ms;
        if (cov_elapsed_ms >= 1000) {
            handler_cov_timer_seconds(cov_elapsed_ms / 1000);
            cov_elapsed_ms %= 1000;
        }
        handler_cov_task();
        addr_check_ms += elapsed_ms;
        if (addr_check_ms >= 5000) {
            addr_check_ms = 0;
            if (bip_espidf_refresh_address()) {
                state_lock();
                update_bound_ip();
                state_unlock();
                ESP_LOGI(TAG, "IP address changed to %s; announcing", s_bound_ip);
                Send_I_Am(&rx[0]);
            }
        }
    }

    bip_cleanup();
    Analog_Input_Cleanup();
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    tables_free_locked();
    xSemaphoreGive(s_snapshot_lock);
    state_lock();
    s_running = false;
    s_objects = 0;
    s_bound_ip[0] = '\0';
    state_unlock();
    s_task = NULL;
    vTaskDelete(NULL);
}

static void stop_server(void)
{
    if (s_task == NULL) return;
    s_stop_task = true;
    for (int i = 0; i < STOP_WAIT_MS / 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "BACnet task did not exit; deleting it");
        vTaskDelete(s_task);
        s_task = NULL;
        bip_cleanup();
        Analog_Input_Cleanup();
        xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
        tables_free_locked();
        xSemaphoreGive(s_snapshot_lock);
    }
    state_lock();
    s_running = false;
    s_objects = 0;
    state_unlock();
}

static esp_err_t start_server(const bacnet_config_t *cfg)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    bacnet_config_t *arg = malloc(sizeof(*arg));
    if (arg == NULL) return ESP_ERR_NO_MEM;
    *arg = *cfg;
    s_stop_task = false;
    s_task_start_err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_start_sem, 0);
    if (xTaskCreate(bacnet_task, "bacnet", TASK_STACK, arg, TASK_PRIO, &s_task) != pdPASS) {
        free(arg);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_start_sem, pdMS_TO_TICKS(START_WAIT_MS)) != pdTRUE) {
        stop_server();
        return ESP_ERR_TIMEOUT;
    }
    return s_task_start_err;
}

esp_err_t bacnet_server_init(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    s_snapshot_lock = xSemaphoreCreateMutex();
    s_start_sem = xSemaphoreCreateBinary();
    if (!s_state_lock || !s_snapshot_lock || !s_start_sem) return ESP_ERR_NO_MEM;

    esp_read_mac(s_mac, ESP_MAC_ETH);
    s_cfg.device_instance = bacnet_default_device_instance_from_mac(s_mac);
    load_config(&s_cfg);
    bacnet_clean_device_name(s_cfg.device_name, CONFIG_MDNS_HOSTNAME, s_cfg.device_name, sizeof(s_cfg.device_name));
    esp_err_t err = ESP_OK;
    if (s_cfg.enabled) {
        err = start_server(&s_cfg);
        set_last_error(err, &s_cfg);
    }
    return err;
}

void bacnet_server_update(void)
{
    /* Called from temp_task (4 KB stack) every read cycle: cheap no-op unless
     * the server is running; nothing channel-sized goes on the stack. */
    if (s_snapshot_lock == NULL) return;
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    if (s_tbl != NULL) {
        refresh_pending_locked();
    }
    xSemaphoreGive(s_snapshot_lock);
}

esp_err_t bacnet_server_apply_config(const bacnet_config_t *cfg)
{
    if (cfg == NULL || !bacnet_config_valid(cfg->udp_port, cfg->device_instance, CONFIG_WEB_SERVER_PORT)) return ESP_ERR_INVALID_ARG;
    bacnet_config_t clean = *cfg;
    bacnet_clean_device_name(clean.device_name, CONFIG_MDNS_HOSTNAME, clean.device_name, sizeof(clean.device_name));

    bacnet_config_t old;
    state_lock();
    old = s_cfg;
    state_unlock();

    bool restart = old.enabled != clean.enabled || old.udp_port != clean.udp_port || old.device_instance != clean.device_instance || strcmp(old.device_name, clean.device_name) != 0;
    esp_err_t err = ESP_OK;
    if (restart) {
        stop_server();
        if (clean.enabled) {
            err = start_server(&clean);
        }
        if (err != ESP_OK) {
            if (old.enabled) (void)start_server(&old);
            set_last_error(err, &clean);
            return err;
        }
    }
    err = save_config(&clean);
    if (err != ESP_OK) return err;
    state_lock();
    s_cfg = clean;
    state_unlock();
    set_last_error(ESP_OK, &clean);
    return ESP_OK;
}

esp_err_t bacnet_server_restore(const bacnet_config_t *cfg)
{
    if (cfg == NULL) return ESP_OK;
    if (!bacnet_config_valid(cfg->udp_port, cfg->device_instance, CONFIG_WEB_SERVER_PORT)) return ESP_ERR_INVALID_ARG;
    bacnet_config_t clean = *cfg;
    bacnet_clean_device_name(clean.device_name, CONFIG_MDNS_HOSTNAME, clean.device_name, sizeof(clean.device_name));
    esp_err_t err = save_config(&clean);
    if (err == ESP_OK) {
        state_lock();
        s_cfg = clean;
        state_unlock();
    }
    return err;
}

void bacnet_server_get_status(bacnet_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_state_lock == NULL) {
        out->config = s_cfg;
        return;
    }
    state_lock();
    out->config = s_cfg;
    out->running = s_running;
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    strlcpy(out->bound_ip, s_bound_ip, sizeof(out->bound_ip));
    out->packets = s_packets;
    out->objects = s_objects;
    out->last_packet_ms = s_last_packet_ms;
    if (s_running && s_task != NULL) {
        out->stack_free_min = uxTaskGetStackHighWaterMark(s_task);
    }
    state_unlock();
}
