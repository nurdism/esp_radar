/*
 * Short audible beep through the on-board PCM5101 I2S DAC.
 *
 * The PCM5101 needs no I2C control: it plays whatever it receives on the I2S
 * bus (BCLK=48, WS=38, DATA=47). beeper_init() sets up a TX channel and
 * pre-renders a tone; beeper_beep() plays it (blocks for the tone duration).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the I2S TX channel and pre-render the beep tone.
 */
esp_err_t beeper_init(void);

/**
 * @brief Play the beep. Blocks for roughly the tone duration (~120 ms).
 */
void beeper_beep(void);

#ifdef __cplusplus
}
#endif
