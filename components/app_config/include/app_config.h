#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolved, ready-to-use runtime configuration.
 *
 * Values come from the `config` flash partition when the browser-based flasher
 * has written one; any field left unset there falls back to the compiled-in
 * menuconfig default (see main/Kconfig.projbuild). */
typedef struct {
    char   wifi_ssid[33];
    char   wifi_pass[64];
    char   zip[16];
    double lat;
    double lon;
    char   tz[48];
    char   ntp[48];
    int    range_nm;
    int    refresh_sec;
    bool   beep;

    /* UI element toggles. */
    bool   ui_clock;
    bool   ui_rings;
    bool   ui_battery;
    bool   ui_header;
    bool   ui_status;
    bool   ui_labels;
    bool   ui_leaders;
} app_config_t;

/* Load configuration into a static instance and return a pointer to it. Reads
 * the `config` partition if present and valid, otherwise uses build defaults.
 * Always succeeds; never returns NULL. */
const app_config_t *app_config_load(void);

#ifdef __cplusplus
}
#endif
