/**
 * @file mqtt_client_ha.c
 * @brief MQTT client with Home Assistant discovery support
 */

#include "mqtt_client_ha.h"
#include "mqtt_client.h"
#include "sensor_manager.h"
#include "onewire_temp.h"
#include "nvs_storage.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "ota_updater.h"
#include "time_sync.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "mqtt_ha";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;

/* Forward declaration */
extern const char *APP_VERSION;
extern uint32_t get_sensor_read_interval(void);
extern uint32_t get_sensor_publish_interval(void);
extern void set_sensor_read_interval(uint32_t ms);
extern void set_sensor_publish_interval(uint32_t ms);

/**
 * @brief MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected to broker");
        s_connected = true;
        
        /* Publish online status */
        mqtt_ha_publish_status(true);
        
        /* Register all sensors with Home Assistant */
#if CONFIG_HA_DISCOVERY_ENABLED
        mqtt_ha_publish_discovery_all();
#endif

#if CONFIG_OTA_ENABLED
        /* Subscribe to the HA firmware "update" Install command and publish an
         * initial state so the entity shows the running version immediately. */
        {
            char cmd_topic[128];
            snprintf(cmd_topic, sizeof(cmd_topic), "%s/update/install", CONFIG_MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(s_mqtt_client, cmd_topic, 1);

            char latest[32] = {0};
            if (ota_is_update_available()) {
                ota_get_latest_version(latest, sizeof(latest));
            }
            mqtt_ha_publish_update_state(APP_VERSION, latest, NULL, -1);
        }
#endif

        /* Subscribe to the HA "Rescan Sensors" button command */
        {
            char rescan_cmd_topic[128];
            snprintf(rescan_cmd_topic, sizeof(rescan_cmd_topic), "%s/rescan/trigger", CONFIG_MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(s_mqtt_client, rescan_cmd_topic, 1);
        }

        /* Subscribe to the HA "1-Wire Resolution" select command */
        {
            char resolution_cmd_topic[128];
            snprintf(resolution_cmd_topic, sizeof(resolution_cmd_topic), "%s/resolution/set", CONFIG_MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(s_mqtt_client, resolution_cmd_topic, 1);
            mqtt_ha_publish_resolution_state();
        }

        /* Subscribe to the HA read/publish interval "number" commands */
        {
            char read_cmd_topic[128], publish_cmd_topic[128];
            snprintf(read_cmd_topic, sizeof(read_cmd_topic), "%s/read_interval/set", CONFIG_MQTT_BASE_TOPIC);
            snprintf(publish_cmd_topic, sizeof(publish_cmd_topic), "%s/publish_interval/set", CONFIG_MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(s_mqtt_client, read_cmd_topic, 1);
            esp_mqtt_client_subscribe(s_mqtt_client, publish_cmd_topic, 1);
            mqtt_ha_publish_interval_state();
        }

        /* Subscribe to the HA "Restart Device" button command */
        {
            char restart_cmd_topic[128];
            snprintf(restart_cmd_topic, sizeof(restart_cmd_topic), "%s/restart/trigger", CONFIG_MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(s_mqtt_client, restart_cmd_topic, 1);
        }
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        s_connected = false;
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Transport error: %s", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGD(TAG, "MQTT Data received on topic %.*s", 
                 event->topic_len, event->topic);
#if CONFIG_OTA_ENABLED
        {
            char cmd_topic[128];
            int cmd_len = snprintf(cmd_topic, sizeof(cmd_topic),
                                   "%s/update/install", CONFIG_MQTT_BASE_TOPIC);
            if (event->topic_len == cmd_len &&
                strncmp(event->topic, cmd_topic, cmd_len) == 0 &&
                event->data_len == 7 &&
                strncmp(event->data, "install", 7) == 0) {
                if (ota_is_update_available()) {
                    ESP_LOGI(TAG, "HA requested firmware install");
                    ota_start_update();
                } else {
                    ESP_LOGW(TAG, "Install requested but no update available; re-checking");
                    ota_check_for_update_async();
                }
            }
        }
#endif

        /* Rescan Sensors button */
        {
            char rescan_cmd_topic[128];
            int rescan_cmd_len = snprintf(rescan_cmd_topic, sizeof(rescan_cmd_topic),
                                          "%s/rescan/trigger", CONFIG_MQTT_BASE_TOPIC);
            if (event->topic_len == rescan_cmd_len &&
                strncmp(event->topic, rescan_cmd_topic, rescan_cmd_len) == 0) {
                ESP_LOGI(TAG, "HA requested sensor bus rescan");
                esp_err_t rescan_err = sensor_manager_rescan();
                if (rescan_err != ESP_OK) {
                    ESP_LOGW(TAG, "Sensor rescan failed: %s", esp_err_to_name(rescan_err));
                }
            }
        }

        /* 1-Wire Resolution select */
        {
            char resolution_cmd_topic[128];
            int resolution_cmd_len = snprintf(resolution_cmd_topic, sizeof(resolution_cmd_topic),
                                              "%s/resolution/set", CONFIG_MQTT_BASE_TOPIC);
            if (event->topic_len == resolution_cmd_len &&
                strncmp(event->topic, resolution_cmd_topic, resolution_cmd_len) == 0 &&
                event->data_len > 0 && event->data_len < 8) {
                char bits_str[8] = {0};
                memcpy(bits_str, event->data, event->data_len);
                int bits = atoi(bits_str);
                if (bits >= 9 && bits <= 12) {
                    ESP_LOGI(TAG, "HA requested 1-Wire resolution: %d bits", bits);
                    onewire_temp_set_resolution(bits);
                    uint32_t read_ms = get_sensor_read_interval();
                    uint32_t publish_ms = get_sensor_publish_interval();
                    nvs_storage_save_sensor_settings(read_ms, publish_ms, (uint8_t)bits);
                    mqtt_ha_publish_resolution_state();
                } else {
                    ESP_LOGW(TAG, "Ignoring invalid resolution request: '%s'", bits_str);
                }
            }
        }

        /* Read/Publish interval numbers */
        {
            char read_cmd_topic[128], publish_cmd_topic[128];
            int read_cmd_len = snprintf(read_cmd_topic, sizeof(read_cmd_topic),
                                        "%s/read_interval/set", CONFIG_MQTT_BASE_TOPIC);
            int publish_cmd_len = snprintf(publish_cmd_topic, sizeof(publish_cmd_topic),
                                           "%s/publish_interval/set", CONFIG_MQTT_BASE_TOPIC);
            if (event->data_len > 0 && event->data_len < 16) {
                char value_str[16] = {0};
                memcpy(value_str, event->data, event->data_len);
                long ms = atol(value_str);

                if (event->topic_len == read_cmd_len &&
                    strncmp(event->topic, read_cmd_topic, read_cmd_len) == 0) {
                    if (ms < 5000) ms = 5000;
                    if (ms > 300000) ms = 300000;
                    ESP_LOGI(TAG, "HA requested read interval: %ldms", ms);
                    set_sensor_read_interval((uint32_t)ms);
                    nvs_storage_save_sensor_settings((uint32_t)ms, get_sensor_publish_interval(),
                                                     (uint8_t)onewire_temp_get_resolution());
                    mqtt_ha_publish_interval_state();
                } else if (event->topic_len == publish_cmd_len &&
                          strncmp(event->topic, publish_cmd_topic, publish_cmd_len) == 0) {
                    if (ms < 5000) ms = 5000;
                    if (ms > 600000) ms = 600000;
                    ESP_LOGI(TAG, "HA requested publish interval: %ldms", ms);
                    set_sensor_publish_interval((uint32_t)ms);
                    nvs_storage_save_sensor_settings(get_sensor_read_interval(), (uint32_t)ms,
                                                     (uint8_t)onewire_temp_get_resolution());
                    mqtt_ha_publish_interval_state();
                }
            }
        }

        /* Restart Device button */
        {
            char restart_cmd_topic[128];
            int restart_cmd_len = snprintf(restart_cmd_topic, sizeof(restart_cmd_topic),
                                           "%s/restart/trigger", CONFIG_MQTT_BASE_TOPIC);
            if (event->topic_len == restart_cmd_len &&
                strncmp(event->topic, restart_cmd_topic, restart_cmd_len) == 0) {
                ESP_LOGW(TAG, "HA requested device restart");
                esp_restart();
            }
        }
        break;
        
    default:
        break;
    }
}

esp_err_t mqtt_ha_init(void)
{
    ESP_LOGD(TAG, "Initializing MQTT client");

    /* Try to load config from NVS, fall back to menuconfig defaults */
    char broker_uri[128] = {0};
    char username[64] = {0};
    char password[64] = {0};
    
    esp_err_t err = nvs_storage_load_mqtt_config(broker_uri, sizeof(broker_uri),
                                                  username, sizeof(username),
                                                  password, sizeof(password));
    if (err != ESP_OK || strlen(broker_uri) == 0) {
        strncpy(broker_uri, CONFIG_MQTT_BROKER_URI, sizeof(broker_uri) - 1);
        strncpy(username, CONFIG_MQTT_USERNAME, sizeof(username) - 1);
        strncpy(password, CONFIG_MQTT_PASSWORD, sizeof(password) - 1);
    }

    /* Build last will topic */
    char lwt_topic[128];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = strlen(username) > 0 ? username : NULL,
        .credentials.authentication.password = strlen(password) > 0 ? password : NULL,
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, 
                                   mqtt_event_handler, NULL);

    ESP_LOGD(TAG, "Starting MQTT client, broker: %s", broker_uri);
    return esp_mqtt_client_start(s_mqtt_client);
}

esp_err_t mqtt_ha_start(void)
{
    if (s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_start(s_mqtt_client);
}

esp_err_t mqtt_ha_stop(void)
{
    if (s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    mqtt_ha_publish_status(false);
    return esp_mqtt_client_stop(s_mqtt_client);
}

bool mqtt_ha_is_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_ha_publish_temperature(const char *sensor_id, const char *friendly_name, float temperature)
{
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    char payload[32];

    /* State topic: base_topic/sensor/sensor_id/state */
    snprintf(topic, sizeof(topic), "%s/sensor/%s/state", 
             CONFIG_MQTT_BASE_TOPIC, sensor_id);
    snprintf(payload, sizeof(payload), "%.2f", temperature);

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish temperature for %s", sensor_id);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published %s: %.2f°C", friendly_name, temperature);
    return ESP_OK;
}

esp_err_t mqtt_ha_register_sensor(const char *sensor_id, const char *friendly_name)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Discovery topic: homeassistant/sensor/esp32-poe-temp_sensor_id/config */
    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic), 
             "%s/sensor/%s_%s/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC, sensor_id);

    /* Build discovery payload using cJSON */
    cJSON *root = cJSON_CreateObject();
    
    /* Basic info */
    cJSON_AddStringToObject(root, "name", friendly_name);
    
    /* Unique ID */
    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_%s", CONFIG_MQTT_BASE_TOPIC, sensor_id);
    cJSON_AddStringToObject(root, "unique_id", unique_id);

    /* Pin the entity_id slug to the (stable) 1-wire sensor id rather than
     * letting HA derive it from "name" above. Without this, HA generates
     * entity_id once from the initial name and never changes it - if the
     * friendly name is set/edited later (or the sensor is repurposed), the
     * entity_id keeps referencing the old/original name forever while the
     * displayed friendly name has since diverged. object_id fixes the
     * entity_id to the sensor_id, leaving "name" free to be purely cosmetic
     * and safely editable at any time. */
    cJSON_AddStringToObject(root, "object_id", unique_id);
    
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/sensor/%s/state", 
             CONFIG_MQTT_BASE_TOPIC, sensor_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    
    /* Availability */
    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    
    /* Device class and unit */
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(root, "state_class", "measurement");
    
    /* Device info (groups all sensors under one device) */
    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "name", "Thermux");
    cJSON_AddStringToObject(device, "manufacturer", "Custom");
    cJSON_AddStringToObject(device, "model", "ESP32-POE-ISO");
    cJSON_AddStringToObject(device, "sw_version", APP_VERSION);
    
    cJSON *identifiers = cJSON_CreateArray();
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(CONFIG_MQTT_BASE_TOPIC));
    cJSON_AddItemToObject(device, "identifiers", identifiers);
    
    cJSON_AddItemToObject(root, "device", device);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to create discovery payload");
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic, 
                                          payload, 0, 1, 1);
    free(payload);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish discovery for %s", sensor_id);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Registered sensor with HA: %s (%s)", friendly_name, sensor_id);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_publish_status(bool online)
{
    if (s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);

    const char *payload = online ? "online" : "offline";
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published status: %s", payload);
    return ESP_OK;
}

esp_err_t mqtt_ha_publish_discovery_all(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    int count;
    const managed_sensor_t *sensors = sensor_manager_get_sensors(&count);
    
    for (int i = 0; i < count; i++) {
        const char *name = sensors[i].has_friendly_name ? 
                           sensors[i].friendly_name : sensors[i].address_str;
        mqtt_ha_register_sensor(sensors[i].address_str, name);
    }
    
    /* Register diagnostic entities */
    mqtt_ha_register_diagnostic_entities();
    
#if CONFIG_OTA_ENABLED
    /* Register the firmware update entity */
    mqtt_ha_register_update_entity();
#endif

    /* Register the rescan sensors button */
    mqtt_ha_register_rescan_button();

    /* Register the 1-Wire resolution select */
    mqtt_ha_register_resolution_select();

    /* Register the read/publish interval numbers and restart button */
    mqtt_ha_register_interval_numbers();
    mqtt_ha_register_restart_button();
    
    ESP_LOGD(TAG, "Published discovery for %d sensors + diagnostics", count);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

/**
 * @brief Helper to create device info JSON object (shared between entities)
 */
static cJSON* create_device_info(void)
{
    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "name", "Thermux");
    cJSON_AddStringToObject(device, "manufacturer", "Custom");
    cJSON_AddStringToObject(device, "model", "ESP32-POE-ISO");
    cJSON_AddStringToObject(device, "sw_version", APP_VERSION);
    
    cJSON *identifiers = cJSON_CreateArray();
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(CONFIG_MQTT_BASE_TOPIC));
    cJSON_AddItemToObject(device, "identifiers", identifiers);
    
    return device;
}

static esp_err_t register_diagnostic_sensor(const char *object_id,
                                            const char *name,
                                            const char *icon,
                                            const char *device_class,
                                            const char *unit,
                                            const char *state_class,
                                            bool enabled_by_default)
{
    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/sensor/%s_%s/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC, object_id);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "name", name);

    char unique_id[96];
    snprintf(unique_id, sizeof(unique_id), "%s_%s", CONFIG_MQTT_BASE_TOPIC, object_id);
    cJSON_AddStringToObject(root, "unique_id", unique_id);

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/%s",
             CONFIG_MQTT_BASE_TOPIC, object_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);

    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status",
             CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);

    cJSON_AddStringToObject(root, "entity_category", "diagnostic");
    if (icon != NULL) {
        cJSON_AddStringToObject(root, "icon", icon);
    }
    if (device_class != NULL) {
        cJSON_AddStringToObject(root, "device_class", device_class);
    }
    if (unit != NULL) {
        cJSON_AddStringToObject(root, "unit_of_measurement", unit);
    }
    if (state_class != NULL) {
        cJSON_AddStringToObject(root, "state_class", state_class);
    }
    if (!enabled_by_default) {
        cJSON_AddBoolToObject(root, "enabled_by_default", false);
    }
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic,
                                         payload, 0, 1, 1);
    free(payload);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_ha_register_update_entity(void)
{
#if CONFIG_OTA_ENABLED && CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/update/%s_firmware/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Firmware");

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_firmware", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", unique_id);

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/update/state", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "state_topic", state_topic);

    char command_topic[128];
    snprintf(command_topic, sizeof(command_topic), "%s/update/install", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "command_topic", command_topic);

    cJSON_AddStringToObject(root, "payload_install", "install");
    cJSON_AddStringToObject(root, "device_class", "firmware");

    /* The state_topic carries JSON with installed_version/latest_version/
     * update_percentage/in_progress keys, which HA's update platform parses
     * natively — no value templates needed. */
    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to create update discovery payload");
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
    free(payload);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish update entity discovery");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Registered firmware update entity with HA");
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_register_rescan_button(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/button/%s_rescan_sensors/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Rescan Sensors");

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_rescan_sensors", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", unique_id);

    char command_topic[128];
    snprintf(command_topic, sizeof(command_topic), "%s/rescan/trigger", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_press", "trigger");
    cJSON_AddStringToObject(root, "icon", "mdi:magnify-scan");
    cJSON_AddStringToObject(root, "entity_category", "config");

    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to create rescan button discovery payload");
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
    free(payload);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish rescan button discovery");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Registered rescan sensors button with HA");
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_register_resolution_select(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/select/%s_resolution/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "1-Wire Resolution");

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_resolution", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", unique_id);

    cJSON *options = cJSON_CreateArray();
    cJSON_AddItemToArray(options, cJSON_CreateString("9"));
    cJSON_AddItemToArray(options, cJSON_CreateString("10"));
    cJSON_AddItemToArray(options, cJSON_CreateString("11"));
    cJSON_AddItemToArray(options, cJSON_CreateString("12"));
    cJSON_AddItemToObject(root, "options", options);

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/resolution/state", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "state_topic", state_topic);

    char command_topic[128];
    snprintf(command_topic, sizeof(command_topic), "%s/resolution/set", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "command_topic", command_topic);

    cJSON_AddStringToObject(root, "icon", "mdi:decimal");
    cJSON_AddStringToObject(root, "entity_category", "config");

    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to create resolution select discovery payload");
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
    free(payload);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish resolution select discovery");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Registered 1-Wire resolution select with HA");
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_publish_resolution_state(void)
{
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[8];
    snprintf(payload, sizeof(payload), "%d", onewire_temp_get_resolution());

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/resolution/state", CONFIG_MQTT_BASE_TOPIC);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, state_topic, payload, 0, 1, 1);

    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

static esp_err_t register_interval_number(const char *object_id,
                                          const char *name,
                                          const char *state_topic_suffix,
                                          const char *command_topic_suffix,
                                          uint32_t min_ms,
                                          uint32_t max_ms)
{
    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/number/%s_%s/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC, object_id);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "name", name);

    char unique_id[96];
    snprintf(unique_id, sizeof(unique_id), "%s_%s", CONFIG_MQTT_BASE_TOPIC, object_id);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", unique_id);

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/%s", CONFIG_MQTT_BASE_TOPIC, state_topic_suffix);
    cJSON_AddStringToObject(root, "state_topic", state_topic);

    char command_topic[128];
    snprintf(command_topic, sizeof(command_topic), "%s/%s", CONFIG_MQTT_BASE_TOPIC, command_topic_suffix);
    cJSON_AddStringToObject(root, "command_topic", command_topic);

    cJSON_AddNumberToObject(root, "min", min_ms);
    cJSON_AddNumberToObject(root, "max", max_ms);
    cJSON_AddNumberToObject(root, "step", 1000);
    cJSON_AddStringToObject(root, "unit_of_measurement", "ms");
    cJSON_AddStringToObject(root, "mode", "box");
    cJSON_AddStringToObject(root, "icon", "mdi:timer-cog-outline");
    cJSON_AddStringToObject(root, "entity_category", "config");

    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
    free(payload);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_ha_register_interval_numbers(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    register_interval_number("read_interval", "Sensor Read Interval",
                             "read_interval/state", "read_interval/set",
                             5000, 300000);
    register_interval_number("publish_interval", "MQTT Publish Interval",
                             "publish_interval/state", "publish_interval/set",
                             5000, 600000);

    ESP_LOGD(TAG, "Registered read/publish interval numbers with HA");
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_publish_interval_state(void)
{
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[16];
    char topic[128];

    snprintf(payload, sizeof(payload), "%lu", (unsigned long)get_sensor_read_interval());
    snprintf(topic, sizeof(topic), "%s/read_interval/state", CONFIG_MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 1);

    snprintf(payload, sizeof(payload), "%lu", (unsigned long)get_sensor_publish_interval());
    snprintf(topic, sizeof(topic), "%s/publish_interval/state", CONFIG_MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 1);

    return ESP_OK;
}

esp_err_t mqtt_ha_register_restart_button(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/button/%s_restart/config",
             CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Restart Device");

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_restart", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", unique_id);

    char command_topic[128];
    snprintf(command_topic, sizeof(command_topic), "%s/restart/trigger", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_press", "trigger");
    cJSON_AddStringToObject(root, "icon", "mdi:restart");
    cJSON_AddStringToObject(root, "entity_category", "config");
    cJSON_AddStringToObject(root, "device_class", "restart");

    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to create restart button discovery payload");
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
    free(payload);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish restart button discovery");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Registered restart button with HA");
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_publish_update_state(const char *installed_version,
                                       const char *latest_version,
                                       const char *release_url,
                                       int update_percentage)
{
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Normalize away a leading "v"/"V" so HA compares "2.7.1" against "2.7.1"
     * (GitHub tags are like "v2.7.1" while APP_VERSION is "2.7.1"); otherwise
     * HA would report an update permanently. */
    const char *installed = (installed_version && installed_version[0]) ? installed_version : "unknown";
    if (installed[0] == 'v' || installed[0] == 'V') installed++;

    const char *latest = (latest_version && latest_version[0]) ? latest_version : installed;
    if (latest[0] == 'v' || latest[0] == 'V') latest++;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "installed_version", installed);
    /* Until a check completes, report latest == installed so HA shows "up to date". */
    cJSON_AddStringToObject(root, "latest_version", latest);
    if (release_url && release_url[0]) {
        cJSON_AddStringToObject(root, "release_url", release_url);
    }
    /* A number drives HA's progress bar; null clears the in-progress state
     * (per the MQTT update integration schema). */
    if (update_percentage >= 0) {
        cJSON_AddNumberToObject(root, "update_percentage", update_percentage);
        cJSON_AddBoolToObject(root, "in_progress", true);
    } else {
        cJSON_AddNullToObject(root, "update_percentage");
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/update/state", CONFIG_MQTT_BASE_TOPIC);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, state_topic, payload, 0, 1, 1);
    free(payload);

    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_ha_register_diagnostic_entities(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Remove the old retained "Uptime" (seconds) discovery, superseded by the
       "Last Boot" timestamp sensor. Empty retained payload deletes the entity. */
    {
        char old_topic[256];
        snprintf(old_topic, sizeof(old_topic), "%s/sensor/%s_uptime/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);
        esp_mqtt_client_publish(s_mqtt_client, old_topic, "", 0, 1, 1);
    }

    /* Register Ethernet Status binary sensor */
    {
        char discovery_topic[256];
        snprintf(discovery_topic, sizeof(discovery_topic), 
                 "%s/binary_sensor/%s_ethernet/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "Ethernet");
        
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_ethernet", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "unique_id", unique_id);
        
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/ethernet", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        
        char availability_topic[128];
        snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "availability_topic", availability_topic);
        
        cJSON_AddStringToObject(root, "device_class", "connectivity");
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
        
        cJSON_AddItemToObject(root, "device", create_device_info());

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
            free(payload);
            ESP_LOGD(TAG, "Registered diagnostic: Ethernet status");
        }
    }

    /* Register WiFi Status binary sensor */
    {
        char discovery_topic[256];
        snprintf(discovery_topic, sizeof(discovery_topic), 
                 "%s/binary_sensor/%s_wifi/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "WiFi");
        
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_wifi", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "unique_id", unique_id);
        
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/wifi", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        
        char availability_topic[128];
        snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "availability_topic", availability_topic);
        
        cJSON_AddStringToObject(root, "device_class", "connectivity");
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
        
        cJSON_AddItemToObject(root, "device", create_device_info());

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
            free(payload);
            ESP_LOGD(TAG, "Registered diagnostic: WiFi status");
        }
    }

    /* Register IP Address sensor */
    {
        char discovery_topic[256];
        snprintf(discovery_topic, sizeof(discovery_topic), 
                 "%s/sensor/%s_ip_address/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "IP Address");
        
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_ip_address", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "unique_id", unique_id);
        
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/ip", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        
        char availability_topic[128];
        snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "availability_topic", availability_topic);
        
        cJSON_AddStringToObject(root, "icon", "mdi:ip-network");
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        
        cJSON_AddItemToObject(root, "device", create_device_info());

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
            free(payload);
            ESP_LOGD(TAG, "Registered diagnostic: IP Address");
        }
    }

    /* Register Bus Error Rate sensor */
    {
        char discovery_topic[256];
        snprintf(discovery_topic, sizeof(discovery_topic), 
                 "%s/sensor/%s_bus_error_rate/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "Bus Lifetime Error Rate");
        
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_bus_error_rate", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "unique_id", unique_id);
        
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/bus_error_rate", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        
        char availability_topic[128];
        snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "availability_topic", availability_topic);
        
        cJSON_AddStringToObject(root, "icon", "mdi:alert-circle-outline");
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        cJSON_AddStringToObject(root, "unit_of_measurement", "%");
        cJSON_AddStringToObject(root, "state_class", "measurement");
        cJSON_AddBoolToObject(root, "enabled_by_default", false);
        
        cJSON_AddItemToObject(root, "device", create_device_info());

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
            free(payload);
            ESP_LOGD(TAG, "Registered diagnostic: Bus Error Rate");
        }
    }

    /* Register Bus Total Reads sensor */
    {
        char discovery_topic[256];
        snprintf(discovery_topic, sizeof(discovery_topic), 
                 "%s/sensor/%s_bus_total_reads/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "Bus Total Reads");
        
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_bus_total_reads", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "unique_id", unique_id);
        
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/bus_total_reads", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        
        char availability_topic[128];
        snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "availability_topic", availability_topic);
        
        cJSON_AddStringToObject(root, "icon", "mdi:counter");
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        cJSON_AddStringToObject(root, "state_class", "total_increasing");
        cJSON_AddBoolToObject(root, "enabled_by_default", false);
        
        cJSON_AddItemToObject(root, "device", create_device_info());

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
            free(payload);
            ESP_LOGD(TAG, "Registered diagnostic: Bus Total Reads");
        }
    }

    /* Register Bus Failed Reads sensor */
    {
        char discovery_topic[256];
        snprintf(discovery_topic, sizeof(discovery_topic), 
                 "%s/sensor/%s_bus_failed_reads/config",
                 CONFIG_HA_DISCOVERY_PREFIX, CONFIG_MQTT_BASE_TOPIC);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "Bus Failed Reads");
        
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_bus_failed_reads", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "unique_id", unique_id);
        
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/diagnostic/bus_failed_reads", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        
        char availability_topic[128];
        snprintf(availability_topic, sizeof(availability_topic), "%s/status", CONFIG_MQTT_BASE_TOPIC);
        cJSON_AddStringToObject(root, "availability_topic", availability_topic);
        
        cJSON_AddStringToObject(root, "icon", "mdi:alert-circle");
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        cJSON_AddStringToObject(root, "state_class", "total_increasing");
        cJSON_AddBoolToObject(root, "enabled_by_default", false);
        
        cJSON_AddItemToObject(root, "device", create_device_info());

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, discovery_topic, payload, 0, 1, 1);
            free(payload);
            ESP_LOGD(TAG, "Registered diagnostic: Bus Failed Reads");
        }
    }

    register_diagnostic_sensor("last_boot", "Last Boot", "mdi:clock-start",
                               "timestamp", NULL, NULL, true);
    register_diagnostic_sensor("bus_recent_error_rate", "Bus Recent Error Rate",
                               "mdi:alert-circle-outline", NULL, "%",
                               "measurement", true);
    register_diagnostic_sensor("bus_seconds_since_success",
                               "Seconds Since Last Successful Read",
                               "mdi:timer-check-outline", "duration", "s",
                               "measurement", true);
    register_diagnostic_sensor("bus_consecutive_failed_cycles",
                               "Consecutive Failed Bus Cycles",
                               "mdi:counter", NULL, NULL, "measurement", true);
    register_diagnostic_sensor("sensor_count", "Sensor Count",
                               "mdi:thermometer-lines", NULL, NULL,
                               "measurement", true);

    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t mqtt_ha_publish_diagnostics(void)
{
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    
    /* Publish Ethernet status */
    bool eth_connected = ethernet_manager_is_connected();
    snprintf(topic, sizeof(topic), "%s/diagnostic/ethernet", CONFIG_MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(s_mqtt_client, topic, eth_connected ? "ON" : "OFF", 0, 1, 0);
    
    /* Publish WiFi status */
    bool wifi_connected = wifi_manager_is_connected();
    snprintf(topic, sizeof(topic), "%s/diagnostic/wifi", CONFIG_MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(s_mqtt_client, topic, wifi_connected ? "ON" : "OFF", 0, 1, 0);
    
    /* Publish IP Address (prefer Ethernet, fallback to WiFi) */
    const char *ip = "";
    if (eth_connected) {
        ip = ethernet_manager_get_ip();
    } else if (wifi_connected) {
        ip = wifi_manager_get_ip();
    }
    snprintf(topic, sizeof(topic), "%s/diagnostic/ip", CONFIG_MQTT_BASE_TOPIC);
    esp_mqtt_client_publish(s_mqtt_client, topic, ip, 0, 1, 0);
    
    ESP_LOGD(TAG, "Published diagnostics: eth=%d, wifi=%d, ip=%s", eth_connected, wifi_connected, ip);

    /* Publish bus health and error statistics */
    uint32_t total_reads, failed_reads;
    onewire_temp_get_error_stats(&total_reads, &failed_reads);
    onewire_bus_health_t health;
    onewire_temp_get_bus_health(&health);
    
    char value_buf[32];
    /* Report wall-clock boot time as an ISO-8601 timestamp so HA renders it as
       a self-updating "x days ago". Empty payload => "unknown" until synced. */
    snprintf(topic, sizeof(topic), "%s/diagnostic/last_boot", CONFIG_MQTT_BASE_TOPIC);
    time_t boot_time;
    if (time_sync_get_boot_time(&boot_time)) {
        struct tm tm_utc;
        gmtime_r(&boot_time, &tm_utc);
        strftime(value_buf, sizeof(value_buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
        esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);
    } else {
        esp_mqtt_client_publish(s_mqtt_client, topic, "", 0, 1, 1);
    }

    snprintf(topic, sizeof(topic), "%s/diagnostic/bus_error_rate", CONFIG_MQTT_BASE_TOPIC);
    snprintf(value_buf, sizeof(value_buf), "%.2f", total_reads > 0 ? (double)failed_reads / total_reads * 100.0 : 0.0);
    esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);

    snprintf(topic, sizeof(topic), "%s/diagnostic/bus_recent_error_rate", CONFIG_MQTT_BASE_TOPIC);
    snprintf(value_buf, sizeof(value_buf), "%.2f",
             health.recent_total_reads > 0
                 ? (double)health.recent_failed_reads / health.recent_total_reads * 100.0
                 : 0.0);
    esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);
    
    snprintf(topic, sizeof(topic), "%s/diagnostic/bus_total_reads", CONFIG_MQTT_BASE_TOPIC);
    snprintf(value_buf, sizeof(value_buf), "%lu", (unsigned long)total_reads);
    esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);
    
    snprintf(topic, sizeof(topic), "%s/diagnostic/bus_failed_reads", CONFIG_MQTT_BASE_TOPIC);
    snprintf(value_buf, sizeof(value_buf), "%lu", (unsigned long)failed_reads);
    esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);

    snprintf(topic, sizeof(topic), "%s/diagnostic/bus_seconds_since_success",
             CONFIG_MQTT_BASE_TOPIC);
    if (health.has_successful_read) {
        snprintf(value_buf, sizeof(value_buf), "%llu",
                 (unsigned long long)health.seconds_since_last_success);
        esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);
    } else {
        esp_mqtt_client_publish(s_mqtt_client, topic, "", 0, 1, 1);
    }

    snprintf(topic, sizeof(topic), "%s/diagnostic/bus_consecutive_failed_cycles",
             CONFIG_MQTT_BASE_TOPIC);
    snprintf(value_buf, sizeof(value_buf), "%lu",
             (unsigned long)health.consecutive_failed_cycles);
    esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);

    snprintf(topic, sizeof(topic), "%s/diagnostic/sensor_count", CONFIG_MQTT_BASE_TOPIC);
    snprintf(value_buf, sizeof(value_buf), "%d", sensor_manager_get_count());
    esp_mqtt_client_publish(s_mqtt_client, topic, value_buf, 0, 1, 1);
    
    ESP_LOGD(TAG, "Published bus stats: total=%lu, failed=%lu, lifetime=%.2f%%, recent=%.2f%%, failed_cycles=%lu",
             (unsigned long)total_reads, (unsigned long)failed_reads,
             total_reads > 0 ? (double)failed_reads / total_reads * 100.0 : 0.0,
             health.recent_total_reads > 0
                 ? (double)health.recent_failed_reads / health.recent_total_reads * 100.0
                 : 0.0,
             (unsigned long)health.consecutive_failed_cycles);

    return ESP_OK;
}
