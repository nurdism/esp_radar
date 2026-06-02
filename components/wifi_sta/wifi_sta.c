#include "wifi_sta.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

static wifi_sta_cb_t s_cb;
static void *s_ctx;
static volatile bool s_connected;
static int s_retry;
static int s_max_retry = CONFIG_ESP_RADAR_WIFI_MAX_RETRY;

static void notify(wifi_sta_state_t state)
{
    if (s_cb) {
        s_cb(state, s_ctx);
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        notify(WIFI_STA_CONNECTING);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < s_max_retry) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retry, s_max_retry);
            esp_wifi_connect();
            notify(WIFI_STA_CONNECTING);
        } else {
            ESP_LOGE(TAG, "connect failed after %d retries", s_max_retry);
            notify(WIFI_STA_DISCONNECTED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        notify(WIFI_STA_CONNECTED);
    }
}

esp_err_t wifi_sta_start(const char *ssid, const char *password,
                         wifi_sta_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg.capable = true,
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    if (strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}
