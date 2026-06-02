/*
 * Board support for the Waveshare ESP32-S3-Touch-LCD-1.46.
 *
 * Brings up the shared I2C bus, the TCA9554 I/O expander (used for the
 * display and touch reset lines), the SPD2010 QSPI display, the SPD2010 I2C
 * touch panel, the backlight, and the LVGL port. After board_init() the LVGL
 * active screen can be drawn to (always under lvgl_port_lock/unlock).
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel geometry of the 1.46" round display. */
#define BOARD_LCD_H_RES   412
#define BOARD_LCD_V_RES   412

/**
 * @brief Initialise all on-board peripherals and the LVGL port.
 *
 * @return ESP_OK on success, or an error from the underlying driver.
 */
esp_err_t board_init(void);

/**
 * @brief Set the display backlight brightness.
 *
 * @param percent 0 (off) .. 100 (full).
 */
void board_set_backlight(uint8_t percent);

/**
 * @brief Get the LVGL display created during board_init().
 */
lv_display_t *board_display(void);

/**
 * @brief Get the shared I2C master bus (touch, expander, RTC).
 */
i2c_master_bus_handle_t board_i2c_bus(void);

#ifdef __cplusplus
}
#endif
