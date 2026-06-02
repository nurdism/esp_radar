/*
 * Minimal Wi-Fi station helper: initialise NVS + the Wi-Fi stack, connect to
 * a single AP, auto-reconnect, and report state changes via a callback.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_STA_DISCONNECTED,
} wifi_sta_state_t;

typedef void (*wifi_sta_cb_t)(wifi_sta_state_t state, void *ctx);

/**
 * @brief Start the Wi-Fi station and begin connecting (non-blocking).
 *
 * @param ssid      Network name.
 * @param password  Network password ("" for an open network).
 * @param cb        Optional state-change callback (may be NULL).
 * @param ctx       User context passed to @p cb.
 */
esp_err_t wifi_sta_start(const char *ssid, const char *password,
                         wifi_sta_cb_t cb, void *ctx);

/**
 * @brief Whether the station currently has an IP address.
 */
bool wifi_sta_is_connected(void);

#ifdef __cplusplus
}
#endif
