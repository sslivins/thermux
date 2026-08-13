/**
 * @file time_sync.c
 * @brief SNTP wall-clock time synchronization
 */

#include "time_sync.h"

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "time_sync";

static volatile bool s_synced = false;
static time_t s_boot_time = 0;

/**
 * @brief SNTP notification callback (runs when the clock is set)
 *
 * Records the wall-clock boot time exactly once, on the first sync. Boot time
 * is derived from the current wall clock minus the monotonic uptime, so it
 * stays stable across subsequent re-syncs instead of jittering by a second.
 */
static void on_time_synced(struct timeval *tv)
{
    if (!s_synced) {
        int64_t uptime_us = esp_timer_get_time();
        time_t now = tv ? tv->tv_sec : time(NULL);
        s_boot_time = now - (time_t)(uptime_us / 1000000);
        s_synced = true;

        char buf[32];
        struct tm tm_utc;
        gmtime_r(&s_boot_time, &tm_utc);
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
        ESP_LOGI(TAG, "Time synced; boot time = %s", buf);
    } else {
        ESP_LOGD(TAG, "Time re-synced");
    }
}

esp_err_t time_sync_init(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NTP_SERVER);
    config.start = true;                 /* begin syncing immediately */
    config.sync_cb = on_time_synced;     /* notified when the clock is set */

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SNTP started (server: %s)", CONFIG_NTP_SERVER);
    return ESP_OK;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

bool time_sync_get_boot_time(time_t *out_boot_time)
{
    if (!s_synced || out_boot_time == NULL) {
        return false;
    }
    *out_boot_time = s_boot_time;
    return true;
}
