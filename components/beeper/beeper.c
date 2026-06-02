#include "beeper.h"

#include <math.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"

static const char *TAG = "beeper";

/* PCM5101 I2S pins on the ESP32-S3-Touch-LCD-1.46. */
#define I2S_PORT        I2S_NUM_0
#define PIN_I2S_BCLK    48
#define PIN_I2S_WS      38
#define PIN_I2S_DOUT    47

#define SAMPLE_RATE_HZ  16000
#define TONE_FREQ_HZ    1000
#define TONE_MS         120
#define TONE_AMPLITUDE  7000     /* of 32767; comfortable, not blaring */
#define FADE_SAMPLES    96       /* ~6 ms linear fade in/out to avoid clicks */

#define TONE_FRAMES     (SAMPLE_RATE_HZ * TONE_MS / 1000)

static i2s_chan_handle_t s_tx;
static int16_t *s_tone;          /* interleaved L,R stereo frames */

esp_err_t beeper_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  /* output silence after the tone drains */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init std failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable failed");

    /* Pre-render a sine tone with short fades, duplicated to L and R. */
    s_tone = malloc(TONE_FRAMES * 2 * sizeof(int16_t));
    if (!s_tone) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < TONE_FRAMES; i++) {
        float env = 1.0f;
        if (i < FADE_SAMPLES) {
            env = (float)i / FADE_SAMPLES;
        } else if (i > TONE_FRAMES - FADE_SAMPLES) {
            env = (float)(TONE_FRAMES - i) / FADE_SAMPLES;
        }
        float s = sinf(2.0f * (float)M_PI * TONE_FREQ_HZ * i / SAMPLE_RATE_HZ);
        int16_t sample = (int16_t)(s * env * TONE_AMPLITUDE);
        s_tone[i * 2 + 0] = sample;   /* left  */
        s_tone[i * 2 + 1] = sample;   /* right */
    }

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

void beeper_beep(void)
{
    if (!s_tx || !s_tone) {
        return;
    }
    size_t written = 0;
    i2s_channel_write(s_tx, s_tone, TONE_FRAMES * 2 * sizeof(int16_t), &written, 500);
}
