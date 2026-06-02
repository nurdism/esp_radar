#include "battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

/* Battery sense: ADC1 ch7 (GPIO8), divided by 3, with a board calibration factor. */
#define BAT_ADC_UNIT      ADC_UNIT_1
#define BAT_ADC_CHANNEL   ADC_CHANNEL_7
#define BAT_ADC_ATTEN     ADC_ATTEN_DB_12
#define BAT_DIVIDER       3.0f
#define BAT_CAL_FACTOR    0.990476f

/* Li-ion mapping and presence window. */
#define BAT_FULL_V        4.20f
#define BAT_EMPTY_V       3.30f
#define BAT_PRESENT_MIN_V 3.00f
#define BAT_PRESENT_MAX_V 4.40f

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_calibrated;

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BAT_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_calibrated = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_calibrated = (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK);
#endif
    if (!s_calibrated) {
        ESP_LOGW(TAG, "ADC not calibrated (eFuse not burnt); readings approximate");
    }
    return ESP_OK;
}

bool battery_read(float *voltage, int *percent)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
        return false;
    }

    int mv = 0;
    if (s_calibrated) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            return false;
        }
    } else {
        /* Rough fallback: 12 dB atten spans ~0..3100 mV over 12 bits. */
        mv = raw * 3100 / 4095;
    }

    float v = (mv / 1000.0f) * BAT_DIVIDER / BAT_CAL_FACTOR;

    bool present = (v >= BAT_PRESENT_MIN_V && v <= BAT_PRESENT_MAX_V);
    int pct = (int)((v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f + 0.5f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    if (voltage) *voltage = v;
    if (percent) *percent = pct;
    return present;
}
