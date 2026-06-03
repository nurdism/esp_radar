#include "app_config.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "app_config";

/* 8 bytes including the trailing NUL. Bump the trailing digit if the on-flash
 * layout ever changes incompatibly. */
#define CFG_MAGIC "ESPRDR1"

/* Kconfig bools vanish (rather than become 0) when disabled; normalise them so
 * the struct initialiser below always has a value. */
#ifndef CONFIG_ESP_RADAR_UI_CLOCK
#define CONFIG_ESP_RADAR_UI_CLOCK 0
#endif
#ifndef CONFIG_ESP_RADAR_UI_RINGS
#define CONFIG_ESP_RADAR_UI_RINGS 0
#endif
#ifndef CONFIG_ESP_RADAR_UI_BATTERY
#define CONFIG_ESP_RADAR_UI_BATTERY 0
#endif
#ifndef CONFIG_ESP_RADAR_UI_HEADER
#define CONFIG_ESP_RADAR_UI_HEADER 0
#endif
#ifndef CONFIG_ESP_RADAR_UI_STATUS
#define CONFIG_ESP_RADAR_UI_STATUS 0
#endif
#ifndef CONFIG_ESP_RADAR_UI_LABELS
#define CONFIG_ESP_RADAR_UI_LABELS 0
#endif
#ifndef CONFIG_ESP_RADAR_UI_LEADERS
#define CONFIG_ESP_RADAR_UI_LEADERS 0
#endif

/* On-flash layout written by the browser flasher. Fixed offsets; every field is
 * a NUL-terminated ASCII string so the web side can patch it without caring
 * about integer endianness or floating-point encoding.
 *
 * IMPORTANT: keep this byte-for-byte in sync with web/flasher.js (FIELDS). */
typedef struct __attribute__((packed)) {
    char magic[8];
    char wifi_ssid[33];
    char wifi_pass[64];
    char zip[16];
    char lat[16];
    char lon[16];
    char tz[48];
    char ntp[48];
    char range_nm[8];
    char refresh_sec[8];
    char beep[4];
    /* UI toggles ("1"/"0"); appended so older offsets stay stable. */
    char ui_clock[4];
    char ui_rings[4];
    char ui_battery[4];
    char ui_header[4];
    char ui_status[4];
    char ui_labels[4];
    char ui_leaders[4];
} cfg_blob_t;

static app_config_t s_cfg;

/* Copy a fixed-width, possibly-unterminated source field into dst as a proper C
 * string. Returns false (leaving dst untouched) when the source is empty or has
 * no NUL within its bounds, i.e. "not set / not valid". */
static bool field_str(char *dst, size_t dst_sz, const char *src, size_t src_sz)
{
    size_t n = strnlen(src, src_sz);
    if (n == 0 || n == src_sz) {
        return false;
    }
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

const app_config_t *app_config_load(void)
{
    /* Start from the compiled-in menuconfig defaults. */
    s_cfg = (app_config_t){
        .lat         = atof(CONFIG_ESP_RADAR_HOME_LAT),
        .lon         = atof(CONFIG_ESP_RADAR_HOME_LON),
        .range_nm    = CONFIG_ESP_RADAR_RANGE_NM,
        .refresh_sec = CONFIG_ESP_RADAR_REFRESH_SEC,
#if CONFIG_ESP_RADAR_BEEP
        .beep        = true,
#else
        .beep        = false,
#endif
        .ui_clock    = CONFIG_ESP_RADAR_UI_CLOCK,
        .ui_rings    = CONFIG_ESP_RADAR_UI_RINGS,
        .ui_battery  = CONFIG_ESP_RADAR_UI_BATTERY,
        .ui_header   = CONFIG_ESP_RADAR_UI_HEADER,
        .ui_status   = CONFIG_ESP_RADAR_UI_STATUS,
        .ui_labels   = CONFIG_ESP_RADAR_UI_LABELS,
        .ui_leaders  = CONFIG_ESP_RADAR_UI_LEADERS,
    };
    snprintf(s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid, "%s", CONFIG_ESP_RADAR_WIFI_SSID);
    snprintf(s_cfg.wifi_pass, sizeof s_cfg.wifi_pass, "%s", CONFIG_ESP_RADAR_WIFI_PASSWORD);
    snprintf(s_cfg.zip,       sizeof s_cfg.zip,       "%s", CONFIG_ESP_RADAR_ZIP_CODE);
    snprintf(s_cfg.tz,        sizeof s_cfg.tz,        "%s", CONFIG_ESP_RADAR_TZ);
    snprintf(s_cfg.ntp,       sizeof s_cfg.ntp,       "%s", CONFIG_ESP_RADAR_NTP_SERVER);

    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "config");
    if (!p) {
        ESP_LOGI(TAG, "no config partition; using build defaults");
        return &s_cfg;
    }

    cfg_blob_t b;
    if (esp_partition_read(p, 0, &b, sizeof b) != ESP_OK ||
        memcmp(b.magic, CFG_MAGIC, sizeof b.magic) != 0) {
        ESP_LOGI(TAG, "config partition blank/invalid; using build defaults");
        return &s_cfg;
    }

    /* Overlay any field the flasher actually wrote on top of the defaults. */
    char tmp[16];
    field_str(s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid, b.wifi_ssid, sizeof b.wifi_ssid);
    field_str(s_cfg.zip,       sizeof s_cfg.zip,       b.zip,       sizeof b.zip);
    field_str(s_cfg.tz,        sizeof s_cfg.tz,        b.tz,        sizeof b.tz);
    field_str(s_cfg.ntp,       sizeof s_cfg.ntp,       b.ntp,       sizeof b.ntp);

    /* Password may legitimately be empty (open network), so honour it verbatim
     * whenever the blob is valid rather than treating empty as "unset". */
    size_t pn = strnlen(b.wifi_pass, sizeof b.wifi_pass);
    if (pn < sizeof b.wifi_pass) {
        memcpy(s_cfg.wifi_pass, b.wifi_pass, pn);
        s_cfg.wifi_pass[pn] = '\0';
    }

    if (field_str(tmp, sizeof tmp, b.lat,         sizeof b.lat))         s_cfg.lat = atof(tmp);
    if (field_str(tmp, sizeof tmp, b.lon,         sizeof b.lon))         s_cfg.lon = atof(tmp);
    if (field_str(tmp, sizeof tmp, b.range_nm,    sizeof b.range_nm))    s_cfg.range_nm = atoi(tmp);
    if (field_str(tmp, sizeof tmp, b.refresh_sec, sizeof b.refresh_sec)) s_cfg.refresh_sec = atoi(tmp);
    if (field_str(tmp, sizeof tmp, b.beep,        sizeof b.beep))        s_cfg.beep = (atoi(tmp) != 0);

    if (field_str(tmp, sizeof tmp, b.ui_clock,   sizeof b.ui_clock))   s_cfg.ui_clock = (atoi(tmp) != 0);
    if (field_str(tmp, sizeof tmp, b.ui_rings,   sizeof b.ui_rings))   s_cfg.ui_rings = (atoi(tmp) != 0);
    if (field_str(tmp, sizeof tmp, b.ui_battery, sizeof b.ui_battery)) s_cfg.ui_battery = (atoi(tmp) != 0);
    if (field_str(tmp, sizeof tmp, b.ui_header,  sizeof b.ui_header))  s_cfg.ui_header = (atoi(tmp) != 0);
    if (field_str(tmp, sizeof tmp, b.ui_status,  sizeof b.ui_status))  s_cfg.ui_status = (atoi(tmp) != 0);
    if (field_str(tmp, sizeof tmp, b.ui_labels,  sizeof b.ui_labels))  s_cfg.ui_labels = (atoi(tmp) != 0);
    if (field_str(tmp, sizeof tmp, b.ui_leaders, sizeof b.ui_leaders)) s_cfg.ui_leaders = (atoi(tmp) != 0);

    ESP_LOGI(TAG, "loaded config from flash (ssid '%s', %.4f/%.4f, %d NM, %d s, beep %d)",
             s_cfg.wifi_ssid, s_cfg.lat, s_cfg.lon, s_cfg.range_nm, s_cfg.refresh_sec, s_cfg.beep);
    return &s_cfg;
}
