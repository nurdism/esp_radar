#include "timesync.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "rtc_pcf85063.h"

static const char *TAG = "timesync";

static void on_sync(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "time synced: %s", buf);

    /* Persist to the RTC so the time survives reboots and Wi-Fi outages. */
    pcf85063_set(&tm);
}

esp_err_t timesync_start(const char *tz, const char *server)
{
    setenv("TZ", tz, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.start = true;                 /* begin syncing as soon as the network is up */
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SNTP started (server %s, TZ %s)", server, tz);
    return ESP_OK;
}
