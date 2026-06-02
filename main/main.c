/*
 * ESP Radar - a tiny flight radar for the Waveshare ESP32-S3-Touch-LCD-1.46.
 *
 * Boot flow:
 *   1. Bring up the board (I2C, display, touch, backlight, LVGL port).
 *   2. Build the radar UI.
 *   3. Connect to Wi-Fi.
 *   4. Poll adsb.lol for nearby aircraft and plot them, forever.
 */

#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "wifi_sta.h"
#include "flight_data.h"
#include "radar_ui.h"
#include "beeper.h"
#include "timesync.h"
#include "rtc_pcf85063.h"
#include "battery.h"
#include "power.h"

static const char *TAG = "esp_radar";

/* Pulled from menuconfig (see main/Kconfig.projbuild). */
#define HOME_LAT     (atof(CONFIG_ESP_RADAR_HOME_LAT))
#define HOME_LON     (atof(CONFIG_ESP_RADAR_HOME_LON))
#define RANGE_NM     (CONFIG_ESP_RADAR_RANGE_NM)
#define REFRESH_SEC  (CONFIG_ESP_RADAR_REFRESH_SEC)

/* Upper bound on aircraft we keep per refresh. */
#define MAX_AIRCRAFT  64

/* Seed the system clock from the RTC so the analog clock shows immediately,
 * before Wi-Fi/SNTP have a chance to run. */
static void seed_time_from_rtc(void)
{
    struct tm t;
    esp_err_t err = pcf85063_get(&t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC time not valid yet (%s); waiting for SNTP",
                 esp_err_to_name(err));
        return;
    }
    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "seeded clock from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

/* Called when the power button is long-pressed, just before the latch drops. */
static void on_shutdown(void)
{
    radar_ui_set_status("OFF");
    board_set_backlight(0);
}

static void on_wifi_state(wifi_sta_state_t state, void *ctx)
{
    static bool time_started = false;

    switch (state) {
    case WIFI_STA_CONNECTING:
        radar_ui_set_status("WIFI...");
        break;
    case WIFI_STA_CONNECTED:
        radar_ui_set_status("LINK UP");
        /* Start SNTP now that the network is up so the first request goes out
         * immediately (starting earlier hits a long retry back-off). */
        if (!time_started) {
            time_started = true;
            timesync_start(CONFIG_ESP_RADAR_TZ, CONFIG_ESP_RADAR_NTP_SERVER);
        }
        break;
    case WIFI_STA_DISCONNECTED:
        radar_ui_set_status("NO LINK");
        break;
    }
}

static void radar_task(void *arg)
{
    aircraft_t *aircraft = calloc(MAX_AIRCRAFT, sizeof(aircraft_t));
    if (!aircraft) {
        ESP_LOGE(TAG, "out of memory for aircraft list");
        vTaskDelete(NULL);
        return;
    }

    int fails = 0;

    while (1) {
        /* Battery indicator (updates regardless of network state). */
        float volts = 0;
        int pct = 0;
        bool present = battery_read(&volts, &pct);
        ESP_LOGI(TAG, "battery %.2fV %d%% present=%d", volts, pct, present);
        radar_ui_set_battery(pct, present);

        if (wifi_sta_is_connected()) {
            size_t count = 0;
            esp_err_t err = flight_data_fetch(HOME_LAT, HOME_LON, RANGE_NM,
                                              aircraft, MAX_AIRCRAFT, &count);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "%u aircraft within %d NM", (unsigned)count, RANGE_NM);
                radar_ui_update(aircraft, count);
                radar_ui_set_status("LINK UP");
                fails = 0;
#if CONFIG_ESP_RADAR_BEEP
                beeper_beep();
#endif
            } else {
                ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(err));
                /* Tolerate the odd transient miss before alarming. */
                if (++fails >= 2) {
                    radar_ui_set_status("API ERR");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_SEC * 1000));
    }
}

void app_main(void)
{
    /* Latch power on first so the board stays up on battery once the button is
     * released (also enables long-press shutdown). */
    ESP_ERROR_CHECK(power_init(on_shutdown));

    ESP_LOGI(TAG, "ESP Radar starting (home %s, %s  range %d NM)",
             CONFIG_ESP_RADAR_HOME_LAT, CONFIG_ESP_RADAR_HOME_LON, RANGE_NM);

    /* Set the timezone up front so RTC<->system-time conversions agree. */
    setenv("TZ", CONFIG_ESP_RADAR_TZ, 1);
    tzset();

    ESP_ERROR_CHECK(board_init());
#if CONFIG_ESP_RADAR_BEEP
    ESP_ERROR_CHECK(beeper_init());
#endif

    /* RTC fallback: seed the clock now; SNTP will refine it once online. */
    ESP_ERROR_CHECK(pcf85063_init(board_i2c_bus()));
    seed_time_from_rtc();

    ESP_ERROR_CHECK(battery_init());

    radar_ui_create(CONFIG_ESP_RADAR_ZIP_CODE, HOME_LAT, HOME_LON, RANGE_NM);
    radar_ui_set_status("WIFI...");

    /* Wi-Fi connection drives time sync: SNTP is started from on_wifi_state()
     * once the link is up (see above). */
    ESP_ERROR_CHECK(wifi_sta_start(CONFIG_ESP_RADAR_WIFI_SSID,
                                   CONFIG_ESP_RADAR_WIFI_PASSWORD,
                                   on_wifi_state, NULL));

    xTaskCreate(radar_task, "radar", 8192, NULL, 4, NULL);
}
