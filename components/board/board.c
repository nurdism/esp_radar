#include "board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_spd2010.h"

#include "esp_lvgl_port.h"
#include "touch_spd2010.h"

static const char *TAG = "board";

/* ------------------------------------------------------------------ pins */
/* Shared I2C bus (touch + I/O expander). */
#define PIN_I2C_SDA           11
#define PIN_I2C_SCL           10
#define I2C_FREQ_HZ           400000

/* SPD2010 display - QSPI on SPI2. */
#define LCD_SPI_HOST          SPI2_HOST
#define PIN_LCD_SCK           40
#define PIN_LCD_D0            46
#define PIN_LCD_D1            45
#define PIN_LCD_D2            42
#define PIN_LCD_D3            41
#define PIN_LCD_CS            21
#define LCD_PIXEL_CLOCK_HZ    (80 * 1000 * 1000)
#define LCD_CMD_BITS          32
#define LCD_PARAM_BITS        8
#define LCD_BITS_PER_PIXEL    16
#define LCD_DRAW_BUF_LINES    24   /* partial LVGL buffer height, in rows */

/* SPD2010 touch - on the shared I2C bus. */
#define PIN_TOUCH_INT         4   /* unused (polled); reset is via the expander */

/* Backlight (PWM via LEDC). */
#define PIN_BACKLIGHT         5
#define LEDC_BL_TIMER         LEDC_TIMER_0
#define LEDC_BL_CHANNEL       LEDC_CHANNEL_0
#define LEDC_BL_MODE          LEDC_LOW_SPEED_MODE
#define LEDC_BL_RES           LEDC_TIMER_13_BIT
#define LEDC_BL_MAX_DUTY      ((1 << 13) - 1)

/* TCA9554 I/O expander: address, registers, and the reset-line bit masks.
 * EXIO1 (bit 0) -> touch reset, EXIO2 (bit 1) -> display reset. */
#define TCA9554_ADDR          0x20
#define TCA9554_REG_OUTPUT    0x01
#define TCA9554_REG_CONFIG    0x03
#define EXP_BIT_TOUCH_RST     (1 << 0)
#define EXP_BIT_LCD_RST       (1 << 1)
#define EXP_RESET_MASK        (EXP_BIT_TOUCH_RST | EXP_BIT_LCD_RST)

/* ----------------------------------------------------------------- state */
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_expander;
static uint8_t s_expander_out;        /* shadow of the output register */
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;

/* --------------------------------------------------------------- I2C bus */
static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

/* ----------------------------------------------------------- I/O expander */
static esp_err_t expander_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_expander, buf, sizeof(buf), 100);
}

/* Drive the masked output pins to @level (0/1), leaving the others untouched. */
static esp_err_t expander_set(uint8_t mask, int level)
{
    if (level) {
        s_expander_out |= mask;
    } else {
        s_expander_out &= ~mask;
    }
    return expander_write_reg(TCA9554_REG_OUTPUT, s_expander_out);
}

static esp_err_t expander_init(void)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_expander),
                        TAG, "expander add device failed");

    /* Reset lines start asserted (low); make the two reset pins outputs,
     * everything else stays an input (config bit 1 = input). */
    s_expander_out = 0x00;
    ESP_RETURN_ON_ERROR(expander_write_reg(TCA9554_REG_OUTPUT, s_expander_out),
                        TAG, "expander output init failed");
    ESP_RETURN_ON_ERROR(expander_write_reg(TCA9554_REG_CONFIG, (uint8_t)~EXP_RESET_MASK),
                        TAG, "expander config failed");

    /* Pulse both reset lines low then high. */
    expander_set(EXP_RESET_MASK, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_set(EXP_RESET_MASK, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/* --------------------------------------------------------------- display */
static esp_err_t display_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_panel_io),
        TAG, "panel io failed");

    spd2010_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,                 /* reset is driven by the expander */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(s_panel_io, &panel_cfg, &s_panel),
                        TAG, "panel create failed");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    return ESP_OK;
}

/* ----------------------------------------------------------------- touch */
static esp_err_t touch_init(void)
{
    /* The controller was hardware-reset via the expander in expander_init(). */
    return touch_spd2010_init(s_i2c_bus);
}

/* The SPD2010 QSPI panel only accepts column ranges aligned to 4 px, so round
 * every invalidated area outward before LVGL renders/flushes it. */
static void lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~3;          /* round x1 down to a multiple of 4 */
    area->x2 |= 3;           /* round x2 up to (multiple of 4) - 1 */
}

/* LVGL input-device read callback (runs inside the LVGL port task). */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    if (touch_spd2010_get_point(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ------------------------------------------------------------- backlight */
static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_BL_MODE,
        .timer_num = LEDC_BL_TIMER,
        .duty_resolution = LEDC_BL_RES,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t ch = {
        .speed_mode = LEDC_BL_MODE,
        .channel = LEDC_BL_CHANNEL,
        .timer_sel = LEDC_BL_TIMER,
        .gpio_num = PIN_BACKLIGHT,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void board_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)LEDC_BL_MAX_DUTY * percent / 100;
    ledc_set_duty(LEDC_BL_MODE, LEDC_BL_CHANNEL, duty);
    ledc_update_duty(LEDC_BL_MODE, LEDC_BL_CHANNEL);
}

/* --------------------------------------------------------------- LVGL */
static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,             /* internal DMA RAM: SPI can transfer it directly */
            .swap_bytes = true,           /* QSPI panel expects byte-swapped RGB565 */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return ESP_FAIL;
    }

    /* Align flushed columns to 4 px (SPD2010 QSPI requirement) and register
     * our SPD2010 touch as an LVGL pointer device. */
    lvgl_port_lock(0);
    lv_display_add_event_cb(s_disp, lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_indev, s_disp);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    lvgl_port_unlock();
    return ESP_OK;
}

/* ----------------------------------------------------------------- public */
esp_err_t board_init(void)
{
    ESP_ERROR_CHECK(i2c_init());
    ESP_ERROR_CHECK(expander_init());
    backlight_init();
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(lvgl_init());

    board_set_backlight(80);
    ESP_LOGI(TAG, "board ready");
    return ESP_OK;
}

lv_display_t *board_display(void)
{
    return s_disp;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}
