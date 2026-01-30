/**
 * @file wifi_manager.c
 * @brief WiFi connection management (fallback for when Ethernet unavailable)
 */

#include "wifi_manager.h"
#include "nvs_storage.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_manager";

#define WIFI_MAXIMUM_RETRY  5

static esp_netif_t *s_wifi_netif = NULL;
static bool s_connected = false;
static char s_ip_addr[16] = {0};
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_connected = false;
        ESP_LOGI(TAG, "Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_addr);
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi");

    s_wifi_event_group = xEventGroupCreate();

    s_wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    /* Try to load saved credentials, otherwise use menuconfig defaults */
    char ssid[32] = {0};
    char password[64] = {0};
    
    esp_err_t err = nvs_storage_load_wifi_config(ssid, sizeof(ssid), 
                                                  password, sizeof(password));
    if (err != ESP_OK || strlen(ssid) == 0) {
        /* Use menuconfig defaults */
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "WiFi initialization complete");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi");
    s_retry_num = 0;
    return esp_wifi_start();
}

esp_err_t wifi_manager_stop(void)
{
    return esp_wifi_stop();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char* wifi_manager_get_ip(void)
{
    return s_ip_addr;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    /* Save to NVS */
    esp_err_t err = nvs_storage_save_wifi_config(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    /* Update running config */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    ESP_LOGI(TAG, "WiFi credentials updated");
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t max_records, uint16_t *found_count)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");
    
    /* Configure scan */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    
    /* Start blocking scan */
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed to start: %s", esp_err_to_name(err));
        return err;
    }
    
    /* Get scan results */
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "WiFi scan found %d networks", ap_count);
    
    if (ap_count > max_records) {
        ap_count = max_records;
    }
    
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(err));
        return err;
    }
    
    *found_count = ap_count;
    return ESP_OK;
}
