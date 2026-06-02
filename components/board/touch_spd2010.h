/*
 * Minimal SPD2010 capacitive-touch driver on the new i2c_master driver.
 *
 * The upstream esp_lcd_touch_spd2010 component issues a zero-length I2C write
 * during its register reads, which the i2c_master backend rejects. This driver
 * implements the same SPD2010 read protocol (ported from the vendor demo) using
 * i2c_master_transmit_receive, whose reads always send the 2-byte register
 * address first.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach the touch controller (I2C address 0x53) to @p bus.
 *
 * The controller must already have been hardware-reset (done via the TCA9554).
 */
esp_err_t touch_spd2010_init(i2c_master_bus_handle_t bus);

/**
 * @brief Read the current primary touch point.
 *
 * @param x  Receives the X coordinate.
 * @param y  Receives the Y coordinate.
 * @return true if a finger is down, false otherwise.
 */
bool touch_spd2010_get_point(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
