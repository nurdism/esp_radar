/*
 * Power latch + button for the ESP32-S3-Touch-LCD-1.46 on battery.
 *
 * The power button only momentarily applies power; firmware must drive the
 * power-hold pin (GPIO7) high to keep the board latched on after the button is
 * released. A long press then drops the latch to power off.
 *
 * Call power_init() as early as possible in app_main so the user only has to
 * hold the button through boot.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Latch power on and start monitoring the button for a long-press
 *        shutdown.
 *
 * @param on_shutdown  Optional callback invoked once when the long-press is
 *                     detected, before the latch is dropped (e.g. to turn off
 *                     the backlight). May be NULL.
 */
esp_err_t power_init(void (*on_shutdown)(void));

/**
 * @brief Immediately drop the power latch (turns the board off on battery).
 */
void power_off(void);

#ifdef __cplusplus
}
#endif
