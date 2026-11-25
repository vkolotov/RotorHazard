// Battery voltage: a resistor divider from the battery to an ADC1 pin
// (RHNODE_C5_VBAT_GPIO; GPIO1 on the C5-Zero gate). Sampled once a second
// between scan cycles, outside any capture, and smoothed. Reported over the
// link as READ 0x48; the divider ratio is a saved setting (WRITE 0x7F, or
// the console's `vbat`), so a known battery voltage calibrates it.
#include "c5_battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "nvs.h"

#ifndef RHNODE_C5_VBAT_GPIO
#define RHNODE_C5_VBAT_GPIO -1
#endif
// Battery mV per pin mV, x1000: two equal resistors unless set otherwise.
#ifndef RHNODE_C5_VBAT_RATIO_MILLI
#define RHNODE_C5_VBAT_RATIO_MILLI 2000
#endif

namespace c5 {

static const char* TAG = "vbat";
static adc_oneshot_unit_handle_t g_adc = nullptr;
static adc_cali_handle_t g_cali = nullptr;
static adc_channel_t g_channel;
static uint16_t g_ratioMilli = RHNODE_C5_VBAT_RATIO_MILLI;
static float g_pinMv = -1;   // smoothed; < 0 until the first sample

static void saveRatio()
{
    nvs_handle_t h;
    if (nvs_open("rhc5", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u16(h, "vbat_ratio", g_ratioMilli);
    nvs_commit(h);
    nvs_close(h);
}

void batteryInit()
{
    if (RHNODE_C5_VBAT_GPIO < 0)
        return;
    nvs_handle_t h;
    if (nvs_open("rhc5", NVS_READONLY, &h) == ESP_OK) {
        uint16_t r;
        if (nvs_get_u16(h, "vbat_ratio", &r) == ESP_OK && r >= kBatteryRatioMin && r <= kBatteryRatioMax)
            g_ratioMilli = r;
        nvs_close(h);
    }
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(RHNODE_C5_VBAT_GPIO, &unit, &g_channel) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", RHNODE_C5_VBAT_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = ADC_UNIT_1;
    ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&ucfg, &g_adc) != ESP_OK) {
        g_adc = nullptr;
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(g_adc, g_channel, &ccfg);
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = ADC_UNIT_1;
    cal.chan = g_channel;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &g_cali) != ESP_OK) {
        g_cali = nullptr;
        ESP_LOGW(TAG, "no ADC calibration in eFuse: uncalibrated readings");
    }
}

void batterySample()
{
    if (!g_adc)
        return;
    int sum = 0, n = 0;
    for (int i = 0; i < 16; ++i) {
        int raw;
        if (adc_oneshot_read(g_adc, g_channel, &raw) == ESP_OK) {
            sum += raw;
            ++n;
        }
    }
    if (!n)
        return;
    const int raw = (sum + n / 2) / n;
    int mv;
    if (!g_cali || adc_cali_raw_to_voltage(g_cali, raw, &mv) != ESP_OK)
        mv = raw * 3300 / 4095;
    // Light smoothing (about 4 s): steady figures, and a sag still shows.
    g_pinMv = g_pinMv < 0 ? (float)mv : g_pinMv + ((float)mv - g_pinMv) * 0.25f;
}

void batteryRead(NodeBattery* out)
{
    *out = NodeBattery();
    out->ratioMilli = g_ratioMilli;
    if (!g_adc || g_pinMv < 0)
        return;
    out->pinMv = (uint16_t)(g_pinMv + 0.5f);
    out->batteryMv = (uint16_t)(g_pinMv * g_ratioMilli / 1000.0f + 0.5f);
    if (out->pinMv >= kBatteryPinMinMv)
        out->flags |= kBatteryWired;
}

bool batterySetRatio(uint16_t ratioMilli)
{
    if (ratioMilli == 0)
        ratioMilli = RHNODE_C5_VBAT_RATIO_MILLI;
    if (ratioMilli < kBatteryRatioMin || ratioMilli > kBatteryRatioMax)
        return false;
    g_ratioMilli = ratioMilli;
    saveRatio();
    return true;
}

}  // namespace c5
