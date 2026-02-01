/**
 * @file ethernet_manager.c
 * @brief Ethernet/POE connection management for ESP32-POE-ISO
 * 
 * The ESP32-POE-ISO uses a LAN8720 PHY with the following configuration:
 * - RMII CLK Mode: GPIO17 output (50MHz from ESP32 APLL)
 * - MDIO: GPIO18
 * - MDC: GPIO23
 * - PHY Address: 0
 * - PHY Reset/Power: GPIO12
 */

#include "ethernet_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "eth_manager";

/* ESP32-POE-ISO specific pin configuration */
#define ETH_PHY_ADDR        0
#define ETH_PHY_RST_GPIO    12  /* Also controls PHY power on POE-ISO */
#define ETH_MDC_GPIO        23
#define ETH_MDIO_GPIO       18
#define ETH_CLK_OUT_GPIO    17  /* RMII clock output from ESP32 */

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static bool s_connected = false;
static char s_ip_addr[16] = {0};

/**
 * @brief Ethernet event handler
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGD(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], 
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        s_connected = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGD(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGD(TAG, "Ethernet Stopped");
        s_connected = false;
        break;
    default:
        break;
    }
}

/**
 * @brief IP event handler for Ethernet
 */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    
    if (event->esp_netif == s_eth_netif) {
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Ethernet Got IP Address: %s", s_ip_addr);
        s_connected = true;
    }
}

esp_err_t ethernet_manager_init(void)
{
    ESP_LOGD(TAG, "Initializing Ethernet for ESP32-POE-ISO");

    /* Enable PHY power (GPIO12 on POE-ISO) */
    gpio_reset_pin(ETH_PHY_RST_GPIO);
    gpio_set_direction(ETH_PHY_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ETH_PHY_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Create default event loop if not already created */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    /* Configure Ethernet MAC */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    
    /* Configure RMII clock output on GPIO17 (matches ESPHome CLK_OUT mode) */
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    esp32_emac_config.clock_config.rmii.clock_gpio = ETH_CLK_OUT_GPIO;
    
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

    /* Configure LAN8720 PHY */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;  /* We handle reset manually above */
    
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    /* Install Ethernet driver */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth_handle));

    /* Attach Ethernet driver to TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, 
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, 
                                                &got_ip_event_handler, NULL));

    ESP_LOGD(TAG, "Ethernet initialization complete");
    return ESP_OK;
}

esp_err_t ethernet_manager_start(void)
{
    if (s_eth_handle == NULL) {
        ESP_LOGE(TAG, "Ethernet not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Starting Ethernet");
    return esp_eth_start(s_eth_handle);
}

esp_err_t ethernet_manager_stop(void)
{
    if (s_eth_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_eth_stop(s_eth_handle);
}

bool ethernet_manager_is_connected(void)
{
    return s_connected;
}

const char* ethernet_manager_get_ip(void)
{
    return s_ip_addr;
}
