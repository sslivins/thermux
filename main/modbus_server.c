/**
 * @file modbus_server.c
 * @brief Read-only Modbus TCP server (esp-modbus glue, settings, channel table)
 *
 * Locking:
 *  - s_update_lock serializes register-image rebuilds and channel-table saves,
 *    so a slower rebuild can never overwrite a newer table or image.
 *  - s_life_lock serializes start/stop/restart and register-image updates.
 *  - s_state_lock guards the channel table, settings and status fields; only
 *    held for short copies (never across NVS, network or esp-modbus calls).
 *  - mbc_slave_lock() is held only while copying the finished image into
 *    the registers esp-modbus serves.
 */

#include "modbus_server.h"
#include "sensor_manager.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbcontroller.h"
#include "mdns.h"
#include "nvs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "modbus";

/* Same namespace as the rest of the settings, so factory reset clears these */
#define NVS_NS              "temp_monitor"
#define NVS_KEY_ENABLED     "mb_enabled"
#define NVS_KEY_PORT        "mb_port"
#define NVS_KEY_UID         "mb_uid"
#define NVS_KEY_CHANNELS       "mb_channels"

#define MDNS_SERVICE        "_mbap"
#define MDNS_PROTO          "_tcp"

#define LIFE_LOCK_TIMEOUT_MS  3000
#define DRAIN_WAIT_MS         500
#define LISTEN_CHECK_TRIES    10
#define LISTEN_CHECK_STEP_MS  100
#define WATCHDOG_PERIOD_MS    30000

extern uint32_t get_sensor_read_interval(void);
extern const char *APP_VERSION;

/* esp-modbus hands handlers a pointer to the PDU inside the received MBAP
 * frame, so the MBAP unit ID is the byte just before it (MB_TCP_UID = 6,
 * MB_TCP_FUNC = 7 in esp-modbus 2.1.3). The reply reuses that header, so the
 * client's unit ID is echoed back. */
#define MBAP_UID_BEFORE_PDU 1

static SemaphoreHandle_t s_update_lock;
static SemaphoreHandle_t s_life_lock;
static SemaphoreHandle_t s_state_lock;

/* Registers served by esp-modbus; the area descriptors point into this */
static modbus_regs_t s_regs;

static void *s_handle;
static mb_fn_handler_fp s_default_fc04;
static volatile uint8_t s_active_uid;
static TaskHandle_t s_drain_task;
static volatile bool s_drain_stop;

static modbus_config_t s_cfg = {
    .enabled = false,
    .port = MODBUS_DEFAULT_PORT,
    .unit_id = MODBUS_DEFAULT_UNIT_ID,
};
static modbus_channel_table_t s_channels;
static bool s_running;
static char s_last_error[64];
static uint32_t s_requests;
static int64_t s_last_request_ms;
static int s_without_channel;
static uint8_t s_mac[6];
static uint16_t s_fw_version[3];
static int64_t s_next_watchdog_ms;

static inline int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }

static void state_lock(void) { xSemaphoreTake(s_state_lock, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_state_lock); }

/* ---- NVS ----------------------------------------------------------------- */

static void load_config(modbus_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t en = 0;
    uint16_t port = 0;
    uint8_t uid = 0;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &en) == ESP_OK) {
        cfg->enabled = en != 0;
    }
    if (nvs_get_u16(h, NVS_KEY_PORT, &port) == ESP_OK &&
        nvs_get_u8(h, NVS_KEY_UID, &uid) == ESP_OK &&
        modbus_config_valid(port, uid, CONFIG_WEB_SERVER_PORT)) {
        cfg->port = port;
        cfg->unit_id = uid;
    }
    nvs_close(h);
}

static esp_err_t save_config(const modbus_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_ENABLED, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(h, NVS_KEY_PORT, cfg->port);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_UID, cfg->unit_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void load_channels(modbus_channel_table_t *table)
{
    memset(table, 0, sizeof(*table));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t *blob = malloc(MODBUS_CHANNEL_BLOB_SIZE);
    size_t len = MODBUS_CHANNEL_BLOB_SIZE;
    if (blob && nvs_get_blob(h, NVS_KEY_CHANNELS, blob, &len) == ESP_OK) {
        if (!modbus_channels_deserialize(table, blob, len)) {
            ESP_LOGW(TAG, "Stored channel table is invalid; starting with an empty table");
        }
    }
    free(blob);
    nvs_close(h);
}

static esp_err_t save_channels(const modbus_channel_table_t *table)
{
    uint8_t *blob = malloc(MODBUS_CHANNEL_BLOB_SIZE);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    modbus_channels_serialize(table, blob);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_CHANNELS, blob, MODBUS_CHANNEL_BLOB_SIZE);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save channel table: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---- mDNS ---------------------------------------------------------------- */

static void mdns_advertise(const modbus_config_t *cfg)
{
    char unit[4];
    char id[13];
    snprintf(unit, sizeof(unit), "%u", cfg->unit_id);
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    mdns_txt_item_t txt[] = {
        {"unit", unit},
        {"id", id},
        {"map", "1"},
    };
    esp_err_t err = mdns_service_add("Thermux", MDNS_SERVICE, MDNS_PROTO, cfg->port, txt, 3);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS advertise failed: %s", esp_err_to_name(err));
    }
}

static void mdns_withdraw(void)
{
    mdns_service_remove(MDNS_SERVICE, MDNS_PROTO);
}

/* ---- esp-modbus lifecycle -------------------------------------------------- */

/* esp-modbus queues a notification per request and waits up to 10 ticks when
 * the queue is full, so it must be drained. Doubles as a request counter. */
static void drain_task(void *arg)
{
    void *handle = arg;
    mb_param_info_t info;
    while (!s_drain_stop) {
        if (mbc_slave_get_param_info(handle, &info, DRAIN_WAIT_MS) == ESP_OK) {
            state_lock();
            s_requests++;
            s_last_request_ms = uptime_ms();
            state_unlock();
        }
    }
    s_drain_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t add_area(void *handle, uint16_t start, void *addr, size_t bytes)
{
    mb_register_area_descriptor_t area = {
        .start_offset = start,
        .type = MB_PARAM_INPUT,
        .access = MB_ACCESS_RO,
        .address = addr,
        .size = bytes,
    };
    return mbc_slave_set_descriptor(handle, area);
}

static void stop_locked(void)
{
    if (s_handle == NULL) {
        return;
    }
    mdns_withdraw();

    s_drain_stop = true;
    for (int i = 0; i < (DRAIN_WAIT_MS / 50) + 10 && s_drain_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_drain_task != NULL) {
        ESP_LOGW(TAG, "Drain task did not exit; deleting it");
        vTaskDelete(s_drain_task);
        s_drain_task = NULL;
    }

    esp_err_t err = mbc_slave_delete(s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_delete failed: %s", esp_err_to_name(err));
    }
    s_handle = NULL;

    state_lock();
    s_running = false;
    state_unlock();
    ESP_LOGI(TAG, "Modbus TCP server stopped");
}

/* Runs in the esp-modbus task for every FC 0x04 request */
static mb_exception_t fc04_uid_guard(void *inst, uint8_t *frame, uint16_t *len)
{
    uint8_t uid = frame[-MBAP_UID_BEFORE_PDU];
    if (!modbus_unit_id_accepted(uid, s_active_uid)) {
        return MB_EX_GATEWAY_TGT_FAILED;
    }
    return s_default_fc04(inst, frame, len);
}

/* esp-modbus opens its listening socket later, in its own task, and gives up
 * silently after two failed binds, so mbc_slave_start() succeeding does not
 * mean clients can connect. Check by binding the port ourselves without
 * SO_REUSEADDR: EADDRINUSE means a listener (or a live connection) holds it.
 * Any other failure is treated as "present" so we never restart on doubt. */
static bool listener_present(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return true;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bool present = true;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        present = false;
    } else if (errno != EADDRINUSE) {
        ESP_LOGD(TAG, "Listener check on port %u: errno %d", port, errno);
    }
    close(sock);
    return present;
}

static bool wait_for_listener(uint16_t port)
{
    for (int i = 0; i < LISTEN_CHECK_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(LISTEN_CHECK_STEP_MS));
        if (listener_present(port)) {
            return true;
        }
    }
    return false;
}

static esp_err_t start_locked(const modbus_config_t *cfg)
{
    mb_communication_info_t comm;
    memset(&comm, 0, sizeof(comm));
    comm.tcp_opts.mode = MB_TCP;
    comm.tcp_opts.port = cfg->port;
    /* Unit ID filtering is done by fc04_uid_guard(), with FMB_TCP_UID_ENABLED
     * off so esp-modbus accepts every frame. esp-modbus 2.1.3 wedges for good
     * if it drops a frame for a foreign unit ID itself: the transaction
     * resource is never released and every later request goes unanswered. */
    comm.tcp_opts.uid = cfg->unit_id;
    /* Explicit IPv4 wildcard: with IPv6 enabled, an empty address can end up
     * IPv6-only. Binding to all interfaces keeps working across Ethernet and
     * Wi-Fi without a restart. */
    comm.tcp_opts.addr_type = MB_IPV4;
    comm.tcp_opts.ip_addr_table = (void *)"0.0.0.0";
    comm.tcp_opts.ip_netif_ptr = NULL;

    void *handle = NULL;
    esp_err_t err = mbc_slave_create_tcp(&comm, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_create_tcp failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Only FC 0x04 (read input registers); everything else gets 0x01 */
    static const uint8_t unsupported[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x0F, 0x10, 0x11, 0x17};
    for (size_t i = 0; i < sizeof(unsupported); i++) {
        mbc_delete_handler(handle, unsupported[i]);
    }

    s_active_uid = cfg->unit_id;
    err = mbc_get_handler(handle, 0x04, &s_default_fc04);
    if (err == ESP_OK && s_default_fc04 == NULL) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) err = mbc_set_handler(handle, 0x04, fc04_uid_guard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install unit ID filter: %s", esp_err_to_name(err));
        mbc_slave_delete(handle);
        return err;
    }

    if (err == ESP_OK) err = add_area(handle, MB_REG_INFO_START, s_regs.info, sizeof(s_regs.info));
    if (err == ESP_OK) err = add_area(handle, MB_REG_TEMP_START, s_regs.temp, sizeof(s_regs.temp));
    if (err == ESP_OK) err = add_area(handle, MB_REG_STATUS_START, s_regs.status, sizeof(s_regs.status));
    if (err == ESP_OK) err = add_area(handle, MB_REG_AGE_START, s_regs.age, sizeof(s_regs.age));
    if (err == ESP_OK) err = add_area(handle, MB_REG_ROM_START, s_regs.rom, sizeof(s_regs.rom));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set register areas: %s", esp_err_to_name(err));
        mbc_slave_delete(handle);
        return err;
    }

    err = mbc_slave_start(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_start failed on port %u: %s", cfg->port, esp_err_to_name(err));
        mbc_slave_delete(handle);
        return err;
    }

    s_drain_stop = false;
    if (xTaskCreate(drain_task, "mb_drain", 2560, handle, 3, &s_drain_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create drain task");
        s_drain_task = NULL;
        mbc_slave_delete(handle);
        return ESP_ERR_NO_MEM;
    }

    s_handle = handle;

    if (!wait_for_listener(cfg->port)) {
        ESP_LOGE(TAG, "Modbus TCP server could not open port %u", cfg->port);
        stop_locked();
        return ESP_FAIL;
    }

    mdns_advertise(cfg);

    state_lock();
    s_running = true;
    s_requests = 0;
    s_last_request_ms = 0;
    state_unlock();

    ESP_LOGI(TAG, "Modbus TCP server listening on port %u, unit ID %u", cfg->port, cfg->unit_id);
    return ESP_OK;
}

static void set_last_error(esp_err_t err, const modbus_config_t *cfg)
{
    state_lock();
    if (err == ESP_OK) {
        s_last_error[0] = '\0';
    } else {
        snprintf(s_last_error, sizeof(s_last_error), "Could not start on port %u (%s)",
                 cfg->port, esp_err_to_name(err));
    }
    state_unlock();
}

/* ---- Register image ------------------------------------------------------ */

/* Rebuild the register image. Saves the channel table when @p force_save is set
 * or new sensors were given channels. Returns the save result. */
static esp_err_t update_image(bool force_save)
{
    if (s_life_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_update_lock, portMAX_DELAY);
    esp_err_t save_err = ESP_OK;

    int count = 0;
    managed_sensor_t *snap = sensor_manager_snapshot(&count);
    sensor_cycle_info_t cycle;
    sensor_manager_get_cycle_info(&cycle);

    modbus_sensor_input_t *inputs = NULL;
    uint8_t (*roms)[MODBUS_ROM_LEN] = NULL;
    if (count > 0 && snap != NULL) {
        inputs = calloc(count, sizeof(*inputs));
        roms = malloc((size_t)count * MODBUS_ROM_LEN);
    }
    if (count > 0 && (snap == NULL || inputs == NULL || roms == NULL)) {
        ESP_LOGE(TAG, "Out of memory building the register image");
        free(snap);
        free(inputs);
        free(roms);
        xSemaphoreGive(s_update_lock);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < count; i++) {
        const managed_sensor_t *s = &snap[i];
        memcpy(inputs[i].rom, s->hw_sensor.address, MODBUS_ROM_LEN);
        memcpy(roms[i], s->hw_sensor.address, MODBUS_ROM_LEN);
        inputs[i].temperature = s->hw_sensor.temperature;
        inputs[i].valid = s->hw_sensor.valid;
        inputs[i].last_read_ms = s->hw_sensor.last_read_time;
        inputs[i].last_attempt_ms = s->last_attempt_time;
    }
    free(snap);

    modbus_channel_table_t *table = malloc(sizeof(*table));
    modbus_regs_t *image = malloc(sizeof(*image));
    if (table == NULL || image == NULL) {
        ESP_LOGE(TAG, "Out of memory building the register image");
        save_err = ESP_ERR_NO_MEM;
        goto out;
    }

    int newly = 0;
    state_lock();
    int left_over = modbus_channels_auto_assign(&s_channels, (const uint8_t (*)[MODBUS_ROM_LEN])roms,
                                             count, &newly);
    bool full_changed = left_over != s_without_channel;
    s_without_channel = left_over;
    *table = s_channels;
    state_unlock();

    if (newly > 0) {
        ESP_LOGI(TAG, "Assigned Modbus channels to %d new sensor(s)", newly);
    }
    if (newly > 0 || force_save) {
        save_err = save_channels(table);
    }
    if (full_changed && left_over > 0) {
        ESP_LOGW(TAG, "Modbus channel table is full: %d sensor(s) have no channel", left_over);
    }

    modbus_info_input_t info = {
        .fw_version = {s_fw_version[0], s_fw_version[1], s_fw_version[2]},
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .max_sensors = CONFIG_MAX_SENSORS,
        .cycle_count = cycle.cycle_count,
        .last_result = (uint16_t)cycle.last_result,
        .read_interval_ms = get_sensor_read_interval(),
        .now_ms = uptime_ms(),
    };
    memcpy(info.mac, s_mac, sizeof(info.mac));
    modbus_build_regs(image, table, inputs, count, &info);

    if (xSemaphoreTake(s_life_lock, pdMS_TO_TICKS(LIFE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Skipped register update: server is restarting");
        goto out;
    }
    bool locked = s_handle != NULL && mbc_slave_lock(s_handle) == ESP_OK;
    /* Cycle counter last, so a client bracketing its reads with register 5
     * never pairs a new counter with old data */
    uint16_t counter = image->info[MB_INFO_CYCLE_COUNT];
    image->info[MB_INFO_CYCLE_COUNT] = s_regs.info[MB_INFO_CYCLE_COUNT];
    memcpy(s_regs.temp, image->temp, sizeof(s_regs.temp));
    memcpy(s_regs.status, image->status, sizeof(s_regs.status));
    memcpy(s_regs.age, image->age, sizeof(s_regs.age));
    memcpy(s_regs.rom, image->rom, sizeof(s_regs.rom));
    memcpy(s_regs.info, image->info, sizeof(s_regs.info));
    s_regs.info[MB_INFO_CYCLE_COUNT] = counter;
    if (locked) {
        mbc_slave_unlock(s_handle);
    }
    xSemaphoreGive(s_life_lock);

out:
    free(table);
    free(image);
    free(inputs);
    free(roms);
    xSemaphoreGive(s_update_lock);
    return save_err;
}

/* Restart the server if it should be running but clients can't connect:
 * either the listener vanished or an earlier start failed. */
static void listener_watchdog(void)
{
    int64_t now = uptime_ms();
    if (now < s_next_watchdog_ms) {
        return;
    }
    s_next_watchdog_ms = now + WATCHDOG_PERIOD_MS;

    if (xSemaphoreTake(s_life_lock, pdMS_TO_TICKS(LIFE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    state_lock();
    modbus_config_t cfg = s_cfg;
    bool running = s_running;
    state_unlock();

    if (cfg.enabled && (!running || !listener_present(cfg.port))) {
        ESP_LOGW(TAG, "Modbus TCP server not accepting connections on port %u; restarting",
                 cfg.port);
        stop_locked();
        esp_err_t err = start_locked(&cfg);
        set_last_error(err, &cfg);
    }
    xSemaphoreGive(s_life_lock);
}

void modbus_server_update(void)
{
    update_image(false);
    if (s_life_lock != NULL) {
        listener_watchdog();
    }
}

/* ---- Channel management ------------------------------------------------------- */

void modbus_server_get_channels(modbus_channel_table_t *out)
{
    if (s_state_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    state_lock();
    *out = s_channels;
    state_unlock();
}

void modbus_server_get_channel_regs(uint16_t status[MODBUS_CHANNEL_COUNT],
                                 uint16_t temp[MODBUS_CHANNEL_COUNT],
                                 uint16_t rom[MB_REG_ROM_COUNT])
{
    /* s_regs is only written under s_life_lock */
    bool locked = s_life_lock != NULL &&
                  xSemaphoreTake(s_life_lock, pdMS_TO_TICKS(LIFE_LOCK_TIMEOUT_MS)) == pdTRUE;
    memcpy(status, s_regs.status, sizeof(s_regs.status));
    memcpy(temp, s_regs.temp, sizeof(s_regs.temp));
    if (rom != NULL) {
        memcpy(rom, s_regs.rom, sizeof(s_regs.rom));
    }
    if (locked) {
        xSemaphoreGive(s_life_lock);
    }
}

esp_err_t modbus_server_move_channel(int from, int to, modbus_channel_op_t *op)
{
    if (s_state_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    state_lock();
    modbus_channel_op_t res = modbus_channels_move_sensor(&s_channels, from, to);
    state_unlock();
    if (op) {
        *op = res;
    }
    if (res != MB_CHANNEL_OP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Moved Modbus channel %d to %d", from, to);
    return update_image(true);
}

esp_err_t modbus_server_release_channel(int channel, modbus_channel_op_t *op)
{
    if (s_state_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int count = 0;
    managed_sensor_t *snap = sensor_manager_snapshot(&count);
    uint8_t (*present)[MODBUS_ROM_LEN] = NULL;
    if (count > 0 && snap != NULL) {
        present = malloc((size_t)count * MODBUS_ROM_LEN);
        if (present == NULL) {
            free(snap);
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i < count; i++) {
            memcpy(present[i], snap[i].hw_sensor.address, MODBUS_ROM_LEN);
        }
    } else {
        count = 0;
    }
    free(snap);

    state_lock();
    modbus_channel_op_t res = modbus_channels_release_missing(
        &s_channels, channel, (const uint8_t (*)[MODBUS_ROM_LEN])present, count);
    state_unlock();
    free(present);
    if (op) {
        *op = res;
    }
    if (res != MB_CHANNEL_OP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Released Modbus channel %d", channel);
    return update_image(true);
}

esp_err_t modbus_server_restore(const modbus_config_t *cfg, const modbus_channel_table_t *channels)
{
    if (s_state_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (cfg != NULL) {
        if (!modbus_config_valid(cfg->port, cfg->unit_id, CONFIG_WEB_SERVER_PORT)) {
            return ESP_ERR_INVALID_ARG;
        }
        err = save_config(cfg);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (channels != NULL) {
        state_lock();
        s_channels = *channels;
        state_unlock();
        err = update_image(true);
    }
    return err;
}

/* ---- Public API ------------------------------------------------------------ */

esp_err_t modbus_server_init(void)
{
    if (s_life_lock == NULL) {
        s_update_lock = xSemaphoreCreateMutex();
        s_life_lock = xSemaphoreCreateMutex();
        s_state_lock = xSemaphoreCreateMutex();
        if (s_update_lock == NULL || s_life_lock == NULL || s_state_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_read_mac(s_mac, ESP_MAC_ETH);
    /* APP_VERSION, not esp_app_desc: the latter is git describe at build time,
     * which lags a release built before its tag exists. */
    modbus_parse_version(APP_VERSION, s_fw_version);

    modbus_config_t cfg = s_cfg;
    load_config(&cfg);
    modbus_channel_table_t *table = malloc(sizeof(*table));
    if (table == NULL) {
        return ESP_ERR_NO_MEM;
    }
    load_channels(table);

    state_lock();
    s_cfg = cfg;
    s_channels = *table;
    state_unlock();
    free(table);

    /* Fill the image before anyone can connect */
    update_image(false);
    s_next_watchdog_ms = uptime_ms() + WATCHDOG_PERIOD_MS;

    if (!cfg.enabled) {
        ESP_LOGI(TAG, "Modbus TCP server disabled");
        return ESP_OK;
    }

    xSemaphoreTake(s_life_lock, portMAX_DELAY);
    esp_err_t err = start_locked(&cfg);
    xSemaphoreGive(s_life_lock);
    set_last_error(err, &cfg);
    return err;
}

esp_err_t modbus_server_apply_config(const modbus_config_t *cfg)
{
    if (cfg == NULL || !modbus_config_valid(cfg->port, cfg->unit_id, CONFIG_WEB_SERVER_PORT)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_life_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_life_lock, pdMS_TO_TICKS(LIFE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    state_lock();
    modbus_config_t old = s_cfg;
    bool was_running = s_running;
    state_unlock();

    bool restart = cfg->enabled != was_running ||
                   (cfg->enabled && (cfg->port != old.port || cfg->unit_id != old.unit_id));

    esp_err_t err = ESP_OK;
    if (restart) {
        stop_locked();
        if (cfg->enabled) {
            err = start_locked(cfg);
            if (err != ESP_OK && was_running) {
                ESP_LOGW(TAG, "Restoring previous Modbus settings");
                esp_err_t back = start_locked(&old);
                set_last_error(back, &old);
            } else {
                set_last_error(err, cfg);
            }
        } else {
            set_last_error(ESP_OK, cfg);
        }
    }
    /* Update the settings before releasing s_life_lock, so the listener
     * watchdog never acts on the old settings */
    if (err == ESP_OK) {
        state_lock();
        s_cfg = *cfg;
        state_unlock();
    }
    xSemaphoreGive(s_life_lock);

    if (err != ESP_OK) {
        return err;
    }
    return save_config(cfg);
}

void modbus_server_get_status(modbus_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_state_lock == NULL) {
        out->config = s_cfg;
        return;
    }
    state_lock();
    out->config = s_cfg;
    out->running = s_running;
    strncpy(out->last_error, s_last_error, sizeof(out->last_error) - 1);
    out->requests = s_requests;
    out->last_request_ms = s_last_request_ms;
    out->channels_assigned = modbus_channels_assigned_count(&s_channels);
    out->sensors_without_channel = s_without_channel;
    state_unlock();
}

int modbus_server_get_channel(const uint8_t *rom)
{
    if (s_state_lock == NULL) {
        return -1;
    }
    state_lock();
    int channel = modbus_channels_find(&s_channels, rom);
    state_unlock();
    return channel;
}
