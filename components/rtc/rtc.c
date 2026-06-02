#include "rtc_pcf85063.h"

#include "esp_log.h"

static const char *TAG = "rtc";

#define PCF85063_ADDR     0x51
#define REG_CTRL_1        0x00
#define REG_SECONDS       0x04   /* seconds..years span 7 bytes from here */
#define SEC_OS_FLAG       0x80   /* oscillator-stop flag in the seconds register */
#define CTRL_1_RUN_12_5PF 0x01   /* clock running, 24h, 12.5 pF load */
#define RTC_YEAR_BASE     1970

static i2c_master_dev_handle_t s_dev;

static uint8_t dec2bcd(int v) { return (uint8_t)((v / 10 * 16) + (v % 10)); }
static int     bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    for (size_t i = 0; i < len; i++) {
        buf[i + 1] = data[i];
    }
    return i2c_master_transmit(s_dev, buf, len + 1, 100);
}

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    /* Make sure the oscillator is running (does not touch the time). */
    uint8_t ctrl = CTRL_1_RUN_12_5PF;
    return reg_write(REG_CTRL_1, &ctrl, 1);
}

esp_err_t pcf85063_get(struct tm *out)
{
    uint8_t b[7];
    esp_err_t err = reg_read(REG_SECONDS, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    if (b[0] & SEC_OS_FLAG) {        /* oscillator stopped -> time not trustworthy */
        return ESP_ERR_INVALID_STATE;
    }
    out->tm_sec  = bcd2dec(b[0] & 0x7F);
    out->tm_min  = bcd2dec(b[1] & 0x7F);
    out->tm_hour = bcd2dec(b[2] & 0x3F);
    out->tm_mday = bcd2dec(b[3] & 0x3F);
    out->tm_wday = bcd2dec(b[4] & 0x07);
    out->tm_mon  = bcd2dec(b[5] & 0x1F) - 1;
    out->tm_year = bcd2dec(b[6]) + RTC_YEAR_BASE - 1900;
    out->tm_isdst = -1;
    return ESP_OK;
}

esp_err_t pcf85063_set(const struct tm *t)
{
    int year = t->tm_year + 1900 - RTC_YEAR_BASE;
    if (year < 0 || year > 99) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[7] = {
        dec2bcd(t->tm_sec),          /* bit7 (OS flag) cleared -> marks time valid */
        dec2bcd(t->tm_min),
        dec2bcd(t->tm_hour),
        dec2bcd(t->tm_mday),
        dec2bcd(t->tm_wday),
        dec2bcd(t->tm_mon + 1),
        dec2bcd(year),
    };
    esp_err_t err = reg_write(REG_SECONDS, b, sizeof(b));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    }
    return err;
}
