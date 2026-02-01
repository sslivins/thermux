/**
 * @file mqtt_client_ha.c
 * @brief MQTT client with Home Assistant discovery support
 */

#include "mqtt_client_ha.h"
#include "mqtt_client.h"
#include "sensor_manager.h"
#include "nvs_storage.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_ha";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;

/* Forward declaration */
extern const char *APP_VERSION;

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

esp_err_t mqtt_ha_register_diagnostic_entities(void)
{
#if CONFIG_HA_DISCOVERY_ENABLED
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
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
    return ESP_OK;
}
