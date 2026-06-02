#include "power.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "power";

#define PIN_PWR_HOLD      7    /* output: high = stay powered */
#define PIN_PWR_BUTTON    6    /* input:  low  = button pressed */

#define POLL_MS           100
#define SHUTDOWN_HOLD_MS  3000 /* long-press duration to power off */

static void (*s_on_shutdown)(void);

void power_off(void)
{
    ESP_LOGI(TAG, "powering off");
    gpio_set_level(PIN_PWR_HOLD, 0);
}

static void power_task(void *arg)
{
    bool armed = false;          /* don't count the press that powered us on */
    bool triggered = false;      /* fire shutdown handling only once per hold */
    int held_ms = 0;

    while (1) {
        bool pressed = (gpio_get_level(PIN_PWR_BUTTON) == 0);

        if (!armed) {
            if (!pressed) {
                armed = true;    /* button released after power-on */
            }
        } else if (pressed) {
            held_ms += POLL_MS;
            if (held_ms >= SHUTDOWN_HOLD_MS && !triggered) {
                triggered = true;
                ESP_LOGI(TAG, "long-press detected -> shutting down");
                if (s_on_shutdown) {
                    s_on_shutdown();   /* e.g. backlight off (visible feedback) */
                }
                /* Drop the latch. On battery the board powers down as soon as
                 * the button is released; keep it low until then. */
                power_off();
            } else if (triggered) {
                power_off();           /* hold the latch low while still pressed */
            }
        } else {
            held_ms = 0;
            triggered = false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t power_init(void (*on_shutdown)(void))
{
    s_on_shutdown = on_shutdown;

    /* Latch power on immediately. */
    gpio_config_t hold = {
        .pin_bit_mask = 1ULL << PIN_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&hold), TAG, "hold config failed");
    gpio_set_level(PIN_PWR_HOLD, 1);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_PWR_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button config failed");

    xTaskCreate(power_task, "power", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "power latched (hold GPIO%d high)", PIN_PWR_HOLD);
    return ESP_OK;
}
