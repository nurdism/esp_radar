/*
 * Network time: set the local timezone and start an SNTP client. Time syncs
 * automatically once the network is up; read it with the standard time()/
 * localtime_r() functions afterwards.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the timezone and start SNTP.
 *
 * @param tz      POSIX TZ string, e.g. "EST5EDT,M3.2.0,M11.1.0".
 * @param server  NTP server hostname, e.g. "pool.ntp.org".
 */
esp_err_t timesync_start(const char *tz, const char *server);

#ifdef __cplusplus
}
#endif
