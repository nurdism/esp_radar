/*
 * Battery monitor for the ESP32-S3-Touch-LCD-1.46.
 *
 * The battery voltage is divided by 3 and fed to ADC1 channel 7 (GPIO8).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set up the battery-sense ADC.
 */
esp_err_t battery_init(void);

/**
 * @brief Read the battery state.
 *
 * @param voltage  Optional: receives the pack voltage in volts.
 * @param percent  Optional: receives an approximate 0..100 charge estimate.
 * @return true if a battery appears to be connected (plausible voltage).
 */
bool battery_read(float *voltage, int *percent);

#ifdef __cplusplus
}
#endif
