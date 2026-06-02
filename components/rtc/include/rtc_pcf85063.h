/*
 * PCF85063 real-time-clock driver (I2C address 0x51).
 *
 * Used as a time fallback: seed the system clock from the RTC at boot so the
 * analog clock shows immediately, and write the RTC back whenever SNTP syncs
 * so the time survives reboots and Wi-Fi outages.
 *
 * All times are local (the RTC holds local wall-clock time); callers must have
 * set the TZ environment variable so mktime()/localtime_r() agree.
 */
#pragma once

#include <time.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach the PCF85063 to @p bus and make sure its clock is running.
 */
esp_err_t pcf85063_init(i2c_master_bus_handle_t bus);

/**
 * @brief Read the RTC into @p out (local time).
 *
 * @return ESP_OK if the time is valid, ESP_ERR_INVALID_STATE if the oscillator
 *         stopped (power loss / never set), or an I2C error.
 */
esp_err_t pcf85063_get(struct tm *out);

/**
 * @brief Write @p t (local time) into the RTC.
 */
esp_err_t pcf85063_set(const struct tm *t);

#ifdef __cplusplus
}
#endif
