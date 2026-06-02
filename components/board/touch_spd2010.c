#include "touch_spd2010.h"

#include <string.h>
#include "esp_rom_sys.h"

#define SPD2010_TOUCH_ADDR   0x53
#define I2C_TIMEOUT_MS       100
#define MAX_POINTS           10

/* One reported touch point. */
typedef struct {
    uint8_t  id;
    uint16_t x;
    uint16_t y;
    uint8_t  weight;
} tp_report_t;

typedef struct {
    tp_report_t rpt[MAX_POINTS];
    uint8_t  touch_num;
    uint8_t  gesture;
    uint8_t  down;
    uint8_t  up;
} spd2010_touch_t;

/* Decoded status header. */
typedef struct {
    uint8_t  pt_exist;
    uint8_t  gesture;
    uint8_t  aux;
    uint8_t  tic_in_bios;
    uint8_t  tic_in_cpu;
    uint8_t  cpu_run;
    uint16_t read_len;
} tp_status_t;

typedef struct {
    uint8_t  status;
    uint16_t next_packet_len;
} tp_hdp_status_t;

static i2c_master_dev_handle_t s_dev;
static spd2010_touch_t s_touch;

/* --------------------------------------------------------------- I2C helpers */
static esp_err_t tp_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), data, len, I2C_TIMEOUT_MS);
}

static esp_err_t tp_write(uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 2 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)reg;
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(s_dev, buf, len + 2, I2C_TIMEOUT_MS);
}

/* --------------------------------------------------------------- TP commands */
static void write_point_mode_cmd(void) { uint8_t d[2] = {0x00, 0x00}; tp_write(0x5000, d, 2); esp_rom_delay_us(200); }
static void write_start_cmd(void)      { uint8_t d[2] = {0x00, 0x00}; tp_write(0x4600, d, 2); esp_rom_delay_us(200); }
static void write_cpu_start_cmd(void)  { uint8_t d[2] = {0x01, 0x00}; tp_write(0x0400, d, 2); esp_rom_delay_us(200); }
static void write_clear_int_cmd(void)  { uint8_t d[2] = {0x01, 0x00}; tp_write(0x0200, d, 2); esp_rom_delay_us(200); }

static void read_status_length(tp_status_t *st)
{
    uint8_t d[4] = {0};
    tp_read(0x2000, d, sizeof(d));
    st->pt_exist    = (d[0] & 0x01);
    st->gesture     = (d[0] & 0x02);
    st->aux         = (d[0] & 0x08);
    st->tic_in_bios = (d[1] & 0x40) >> 6;
    st->tic_in_cpu  = (d[1] & 0x20) >> 5;
    st->cpu_run     = (d[1] & 0x08) >> 3;
    st->read_len    = (uint16_t)(d[3] << 8 | d[2]);
}

static void read_hdp(tp_status_t *st, spd2010_touch_t *t)
{
    uint8_t d[4 + MAX_POINTS * 6];
    uint16_t len = st->read_len;
    if (len > sizeof(d)) {
        len = sizeof(d);
    }
    tp_read(0x0003, d, len);

    uint8_t check_id = d[4];
    if (check_id <= 0x0A && st->pt_exist) {
        t->touch_num = (uint8_t)((len - 4) / 6);
        if (t->touch_num > MAX_POINTS) {
            t->touch_num = MAX_POINTS;
        }
        t->gesture = 0;
        for (int i = 0; i < t->touch_num; i++) {
            int o = i * 6;
            t->rpt[i].id     = d[4 + o];
            t->rpt[i].x      = ((d[7 + o] & 0xF0) << 4) | d[5 + o];
            t->rpt[i].y      = ((d[7 + o] & 0x0F) << 8) | d[6 + o];
            t->rpt[i].weight = d[8 + o];
        }
    } else {
        t->touch_num = 0;
        t->gesture = 0;
    }
}

static void read_hdp_status(tp_hdp_status_t *hs)
{
    uint8_t d[8] = {0};
    tp_read(0xFC02, d, sizeof(d));
    hs->status = d[5];
    hs->next_packet_len = (uint16_t)(d[2] | d[3] << 8);
}

static void read_hdp_remain(tp_hdp_status_t *hs)
{
    uint8_t d[32];
    uint16_t len = hs->next_packet_len;
    if (len > sizeof(d)) {
        len = sizeof(d);
    }
    tp_read(0x0003, d, len);
}

static void tp_read_data(spd2010_touch_t *t)
{
    tp_status_t st = {0};
    tp_hdp_status_t hs = {0};
    read_status_length(&st);

    if (st.tic_in_bios) {
        write_clear_int_cmd();
        write_cpu_start_cmd();
    } else if (st.tic_in_cpu) {
        write_point_mode_cmd();
        write_start_cmd();
        write_clear_int_cmd();
    } else if (st.cpu_run && st.read_len == 0) {
        write_clear_int_cmd();
    } else if (st.pt_exist || st.gesture) {
        read_hdp(&st, t);
        do {
            read_hdp_status(&hs);
            if (hs.status == 0x82) {
                write_clear_int_cmd();
                break;
            } else if (hs.status == 0x00) {
                read_hdp_remain(&hs);
            } else {
                break;
            }
        } while (1);
    } else if (st.cpu_run && st.aux) {
        write_clear_int_cmd();
    }
}

/* --------------------------------------------------------------- public API */
esp_err_t touch_spd2010_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPD2010_TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
}

bool touch_spd2010_get_point(uint16_t *x, uint16_t *y)
{
    spd2010_touch_t t = {0};
    tp_read_data(&t);

    if (t.touch_num > 0 && t.rpt[0].weight > 0) {
        s_touch = t;
        *x = t.rpt[0].x;
        *y = t.rpt[0].y;
        return true;
    }
    return false;
}
