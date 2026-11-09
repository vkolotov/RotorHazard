#include "c5_radio.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "heap_memory_layout.h"
#include "nvs_flash.h"
#include "soc/soc.h"

// --- Espressif PHY entry points (libphy.a, librftest.a; Apache-2.0) ---
// Signatures from our own disassembly of the ESP32-C5 libraries; see
// README.md ("Where the radio facts come from"). tools/check_phy.py re-checks the parts these
// declarations depend on in the libraries a build links.
extern "C" {
extern uint8_t phy_param[];
int phy_set_chanfreq(uint32_t mhz, uint8_t bwMode);
void phy_rfpll_set_freq(uint32_t mhz, uint32_t xtal, int32_t trim, void* sdm);
void phy_write_rfpll_sdm(void* sdm);
void phy_set_freq_i2c(uint32_t mhz);
void phy_wait_rfpll_cal_end(uint32_t mhz);   // restarts the VCO calibration and polls until it is done
void phy_i2c_sdm_en(uint32_t band5g);
void phy_ckgen_5g_cal(uint32_t mhz);
void phy_freq_mem_change_5g(void* sdm);
void phy_force_rx_gain(uint32_t enable, uint32_t index);
void phy_pbus_workmode(void);
void phy_pbus_xpd_tx_off(void);
void phy_pbus_xpd_rx_on(uint32_t on);
void phy_set_rxclk_en(uint32_t on);
// The modem's I/Q dump trigger, from the RF test library.
void adctrig(uint32_t countMinus1, uint32_t selector, uint32_t mode, uint32_t rateCode,
             uint32_t unused, uint32_t waitStyle, uint32_t w1, uint32_t w2, uint32_t w3);
// librftest's wifi.o refers to the RF test console's parser; nothing here
// uses that object, but if the linker pulls it in this satisfies it.
__attribute__((weak)) int cmd_parse(const char*) { return -1; }
}

// The capture engine writes into this SRAM bank and owns it while a capture
// runs, so keep the heap out of the whole bank.
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, rhnode_c5_capture_bank);

namespace c5 {

namespace {

const char* TAG = "c5radio";

// phy_param fields Espressif's own channel code uses
constexpr int kParamFreqTrim = 30;    // int16: frequency trim passed to the PLL
constexpr int kParam5gFlag = 42;      // uint8: 1 while on the 5 GHz band
constexpr int kParamXtal = 49;        // uint8: crystal selector passed to the PLL
constexpr int kParamCurMhz = 714;     // uint16: the PLL's current frequency

constexpr uint32_t kGainReg = 0x600A702C;    // bits 24-31 forced index, 23 force on, 8-14 max
constexpr uint32_t kDumpCtrl = 0x600A9004;   // count 0-16, done 18, trigger 19, enable 31
constexpr uint32_t kDumpOwn = 0x60095004;    // SRAM ownership: bits 8-11 = 2 and bit 16 -> modem
constexpr uintptr_t kBank = 0x40830000;      // where the engine writes
constexpr int kMaxWords = 8192;
constexpr uint32_t kSentinel = 0xDEAD5A5Au;  // an unlikely word (bits 27-31 of samples are used too)
constexpr uint32_t kGuard = 0xF00DA5A5u;

inline volatile uint32_t& reg(uint32_t a) { return *(volatile uint32_t*)a; }
inline volatile uint32_t* bank() { return (volatile uint32_t*)kBank; }

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

uint32_t C5Radio::gainReg() const { return reg(kGainReg); }

const volatile uint32_t* C5Radio::words() { return bank(); }

bool C5Radio::begin(int gain)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // NULL mode: no station or AP interface exists, so nothing can transmit;
    // promiscuous receive keeps the PHY powered and receiving.
    if (esp_wifi_init(&cfg) == ESP_OK &&
        esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK &&
        esp_wifi_set_mode(WIFI_MODE_NULL) == ESP_OK &&
        esp_wifi_start() == ESP_OK) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        st_.wifi = true;
    }
    if (!st_.wifi) {
        ESP_LOGE(TAG, "Wi-Fi did not start");
        return false;
    }
    // band setup: calibrates the 5 GHz band, wide analogue bandwidth
    phy_set_chanfreq(5180, 1);
    curLo_ = 5180;
    st_.gainMax = (int)((gainReg() >> 8) & 0x7F);
    return bootCheck(gain);
}

// 0 live; 1 incomplete capture; 2 gain field differs from the forced index;
// 3 flat (no noise: the receiver is not delivering samples)
int C5Radio::liveCheck(int gain)
{
    ensureGain(gain);
    if (!stockCapture(256))
        return 1;
    const uint32_t first = bank()[0];
    int differ = 0;
    const int g = (int)((gainReg() >> 24) & 0xFF);
    for (int i = 0; i < 256; ++i) {
        const uint32_t w = bank()[i];
        if ((w & 0xFFFFF) != (first & 0xFFFFF))
            ++differ;
        if (sampleGain(w) != (g & 0x7F))
            return 2;
    }
    return differ > 16 ? 0 : 3;
}

bool C5Radio::bootCheck(int gain)
{
    // Straight after band setup the first captures can fail the check (seen on
    // chip v1.0: live a moment later), so retry for up to ~200 ms before
    // reaching for the extra RX-on calls.
    auto settle = [&]() {
        int r = 0;
        for (int i = 0; i < 10; ++i) {
            r = liveCheck(gain);
            if (r == 0)
                return 0;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return r;
    };
    st_.firstCheck = settle();
    // transmit path off through the PHY bus, as a safety measure
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    st_.afterTxOff = settle();
    if (st_.afterTxOff != 0) {
        phy_pbus_xpd_rx_on(1);
        phy_set_rxclk_en(1);
        st_.rxOnNeeded = true;
        st_.afterTxOff = settle();
    }
    st_.live = st_.afterTxOff == 0;
    seeded_ = st_.live;   // a stock capture has programmed the engine's setup registers
    return st_.live;
}

IRAM_ATTR void C5Radio::hop(int loMhz)
{
    // phy_set_rf_freq_offset() without the fixed 300 us wait at the end of
    // phy_set_rfpll_freq(): the calibration wait has finished by the time the
    // calls return. The sequence is the one in ESP-IDF 5.5.1's libphy.a for
    // the C5 (other PHY library versions use other steps; tools/check_phy.py
    // fails the build check if the linked library differs).
    alignas(4) uint8_t sdm[16] = {};
    int16_t trim;
    memcpy(&trim, phy_param + kParamFreqTrim, 2);
    const uint32_t xtal = phy_param[kParamXtal];
    const uint16_t mhz = (uint16_t)loMhz;
    memcpy(phy_param + kParamCurMhz, &mhz, 2);
    phy_rfpll_set_freq(mhz, xtal, trim, sdm);
    phy_write_rfpll_sdm(sdm);
    phy_set_freq_i2c(mhz);
    phy_wait_rfpll_cal_end(mhz);
    phy_i2c_sdm_en(phy_param[kParam5gFlag]);
    if (!(skip_ & 1) && phy_param[kParam5gFlag])
        phy_ckgen_5g_cal(mhz);
    if (!(skip_ & 2))
        phy_freq_mem_change_5g(sdm);
    curLo_ = loMhz;
}

void C5Radio::fullTune(int loMhz)
{
    phy_set_chanfreq((uint32_t)loMhz, 1);
    curLo_ = loMhz;
}

IRAM_ATTR void C5Radio::ensureGain(int index)
{
    if (index < 0)
        index = 0;
    if (st_.gainMax > 0 && index > st_.gainMax)
        index = st_.gainMax;
    // Re-forcing a force that already holds disturbs the receiver, so only
    // write when the force is off or on another index.
    const uint32_t v = gainReg();
    if (!(v & (1u << 23)) || (int)((v >> 24) & 0xFF) != index)
        phy_force_rx_gain(1, (uint32_t)index);
}

void C5Radio::arm(int n)
{
    volatile uint32_t* w = bank();
    for (int i = 0; i < n; ++i)
        w[i] = kSentinel;
    for (int i = 0; i < 4; ++i)
        w[n + i] = kGuard;
}

bool C5Radio::check(int n) const
{
    const volatile uint32_t* w = bank();
    if (w[n - 1] == kSentinel)
        return false;   // short
    for (int i = 0; i < 4; ++i)
        if (w[n + i] != kGuard)
            return false;   // long
    return true;
}

bool C5Radio::stockCapture(int n)
{
    arm(n);
    // Receive dump only: selector 0 (software trigger), mode 0, rate code 0
    // (80 MS/s), wait style 0. Mode 1 would transmit a tone and selector 5/6
    // or mode 2 a frame; this is the only call site and these are constants.
    adctrig((uint32_t)(n - 1), 0, 0, 0, 0, 0, 0, 0, 0);
    ++st_.stockCaptures;
    return check(n);
}

IRAM_ATTR bool C5Radio::directCapture(int n)
{
    arm(n);
    bool done = false;
    portENTER_CRITICAL(&s_mux);
    const uint32_t own = reg(kDumpOwn);
    reg(kDumpOwn) = (own & ~0xF00u) | (2u << 8) | (1u << 16);
    uint32_t c = reg(kDumpCtrl);
    c &= ~(0x1FFFFu | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21));
    c |= (uint32_t)n | (1u << 31);
    reg(kDumpCtrl) = c;
    reg(kDumpCtrl) = c | (1u << 19);   // trigger pulse
    reg(kDumpCtrl) = c;
    const int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 1000) {
        if (reg(kDumpCtrl) & (1u << 18)) {
            done = true;
            break;
        }
    }
    reg(kDumpCtrl) = reg(kDumpCtrl) & ~(1u << 31);
    reg(kDumpOwn) = reg(kDumpOwn) & ~(0xF00u | (1u << 16));
    reg(kDumpOwn) = own;
    portEXIT_CRITICAL(&s_mux);
    if (!done) {
        ++st_.timeouts;
        return false;
    }
    ++st_.directCaptures;
    return check(n);
}

IRAM_ATTR bool C5Radio::capture(int n, int64_t* tUs)
{
    if (n < 16)
        n = 16;
    if (n > kMaxWords)
        n = kMaxWords;
    const int64_t a = esp_timer_get_time();
    bool ok;
    if (stockOnly_ || !seeded_) {
        ok = stockCapture(n);
        seeded_ = seeded_ || ok;
    } else {
        ok = directCapture(n);
        if (!ok)
            ok = stockCapture(n);   // in practice never needed
    }
    const int64_t b = esp_timer_get_time();
    // the dump itself is at the end of a stock call's fixed overhead
    *tUs = stockOnly_ ? b - (int64_t)n / 160 : a + (b - a) / 2;
    if (!ok)
        ++st_.incomplete;
    return ok;
}

}  // namespace c5
