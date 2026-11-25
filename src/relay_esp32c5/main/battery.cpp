// The relay's own battery: a resistor divider to an ADC1 pin (RELAY_VBAT_GPIO;
// GPIO1 on the S3 boards). ADC2 is unusable while Wi-Fi is on. Sampled once
// a second by the timer task and smoothed; the divider ratio is a saved
// setting (console `vbat`), calibrated against a known battery voltage.
#include "battery.h"

#include <atomic>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "nvs.h"

#ifndef RELAY_VBAT_GPIO
#define RELAY_VBAT_GPIO -1
#endif

namespace relay {

static const char* TAG = "vbat";
constexpr int RATIO_DEFAULT = 2000;  // battery mV per pin mV x1000: two equal resistors
constexpr int RATIO_MIN = 1000, RATIO_MAX = 20000;
constexpr int PIN_MIN_MV = 150;      // below: no divider wired

static adc_oneshot_unit_handle_t g_adc = nullptr;
static adc_cali_handle_t g_cali = nullptr;
static adc_channel_t g_channel;
static std::atomic<int> g_ratio{RATIO_DEFAULT};
static std::atomic<int> g_pinMv{-1};  // smoothed; -1 until the first sample
static float g_smooth = -1;

void batteryInit()
{
    if (RELAY_VBAT_GPIO < 0)
        return;
    nvs_handle_t h;
    if (nvs_open("relay", NVS_READONLY, &h) == ESP_OK) {
        int32_t r;
        if (nvs_get_i32(h, "vbat_ratio", &r) == ESP_OK && r >= RATIO_MIN && r <= RATIO_MAX)
            g_ratio = r;
        nvs_close(h);
    }
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(RELAY_VBAT_GPIO, &unit, &g_channel) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", RELAY_VBAT_GPIO);
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
    g_smooth = g_smooth < 0 ? (float)mv : g_smooth + ((float)mv - g_smooth) * 0.25f;
    g_pinMv = (int)(g_smooth + 0.5f);
}

Battery batteryRead()
{
    Battery b;
    b.ratioMilli = g_ratio;
    const int pin = g_pinMv;
    if (pin < 0)
        return b;
    b.pinMv = pin;
    b.batteryMv = (int)((int64_t)pin * b.ratioMilli / 1000);
    b.wired = pin >= PIN_MIN_MV;
    return b;
}

bool batterySetRatio(int ratioMilli)
{
    if (ratioMilli == 0)
        ratioMilli = RATIO_DEFAULT;
    if (ratioMilli < RATIO_MIN || ratioMilli > RATIO_MAX)
        return false;
    g_ratio = ratioMilli;
    nvs_handle_t h;
    if (nvs_open("relay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "vbat_ratio", ratioMilli);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

}  // namespace relay
