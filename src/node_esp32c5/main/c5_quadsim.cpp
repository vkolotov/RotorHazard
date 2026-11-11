// Quad simulator: an ESP32-C5 that emulates VTXs for bench-testing the C5
// node. It transmits a continuous carrier (ESP-IDF's RF test API,
// esp_phy_wifi_tx_tone) and emulates passes by ramping the transmit
// attenuation down to a peak and back up, as a quad approaching and leaving
// the gate.
//
// Build: tools/build.sh c5zero -DRHNODE_C5_APP=quadsim
//
// Transmits. Frequencies 5170-5917 MHz (the C5 PLL's tuning range, which
// covers Raceband R1-R8 like a real VTX); the default power is low, a bench
// needs very little.
//
// USB console, one command per line:
//   on <MHz> [backoff]       carrier on (backoff in 0.25 dB steps, 0-56, default 40 = -10 dB)
//   off
//   pass <MHz> <peakBackoff> <ms> [floorBackoff]
//                            one pass: carrier on at floorBackoff (default 56), ramps to
//                            peakBackoff and back over ms, then off;
//                            prints "peak <ms>" (this board's clock) at the top
//   laps <MHz> <peakBackoff> <passMs> <periodMs> <count> [floorBackoff]
//   exact 0|1                retune the PLL to the exact MHz after starting the
//                            tone (needed for non-multiples of 5 MHz)
//   level <MHz> <dB>         carrier at <dB> below the API's full power (fast path)
//   fly <MHz> <km/h> <closest cm> <beam deg> [sidelobe dB] [peak dB]
//                            one pass through a gate antenna's beam: free-space loss
//                            from the distance, a Gaussian main lobe (-3 dB at
//                            +-beam deg along the track) floored at the sidelobe level
//                            (default -20 dB), power updated every 250 us; prints
//                            "peak <us>" (this board's clock) and the pass length
//   flaps <MHz> <km/h> <cm> <deg> <periodMs> <count> [sidelobe dB] [peak dB]
//   quad <1|2> <MHz> <km/h> <closest cm> <beam deg, 0 = dome> <start ms> [sidelobe dB] [peak dB]
//                            define one of two quads flown together by "go". Both
//                            come from one radio: its PLL sits between them and each
//                            quad is a tone offset from it, so the two must be within
//                            ~40 MHz (adjacent Raceband channels at most).
//   ref <cm>                 default reference distance for quads/passes defined after it:
//                            closer than it the quad reads at full power, farther it falls
//                            off as 1/r^2 (0 = relative model: 0 dB at every closest point)
//   go [laps] [periodMs]     fly the defined quads, repeated laps times (start to start
//                            periodMs); any byte received between laps stops the run.
//                            One JSON line per quad per lap:
//                            {"evt":"pass","lap":1,"quad":1,"mhz":5732,"peak_us":..,"on_ms":..,"range_db":..}
//                            then {"evt":"done","laps":N,"stopped":false}
//   info                     one JSON line: app, version, frequency range, limits, clock
//   clear                    forget the defined quads
//   status
//
// Fast power control: esp_phy_wifi_tx_tone() writes the tone into register
// 0x600A041C (bits 10-17: attenuation in 0.25 dB steps, stored negated;
// bit 18: tone on; bits 0-9: offset from the LO, 0), but each call restarts
// the tone (~1 ms). The level and fly commands start the tone once at the
// API's full power and then only add attenuation in that field, so the
// carrier is never stronger than the API itself sets. Measured from our
// disassembly of ESP-IDF 5.5.1's libphy/librftest (wifiscwout and
// phy_start_tx_tone_step).
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "nvs.h"
#include "esp_phy_cert_test.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "c5_radio.h"

namespace {

constexpr int kMinMhz = 5170, kMaxMhz = 5917;
constexpr const char* kQuadsimVersion = "0.6-diag";
// Measured with an RX5808 timer (2026-11-09): backoff 0-56 attenuates as
// documented (~14 dB); 63 and above come out at full power or more, so the
// setting is out of range there.
constexpr int kMaxBackoff = 56;

c5::C5Radio g_radio;   // only its PLL hop is used
bool g_exact = true;
bool g_on = false;
int g_preparedPll = 0; // prepared/muted source: start RF before acquisition
int g_mhz = 0, g_backoff = 0, g_chan = 0;

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    usb_serial_jtag_write_bytes(buf, (size_t)n, pdMS_TO_TICKS(20));
}

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Antenna: the C5-Zero's RF switch (ANT_Ctrl = GPIO26, pulled low by 200k):
// 0 = on-board ceramic (default), 1 = the u.FL socket. Kept in NVS.
int g_antenna = 0;
void applyAntenna()
{
    gpio_reset_pin(GPIO_NUM_26);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_26, g_antenna ? 1 : 0);
}
void saveAntenna()
{
    nvs_handle_t h;
    if (nvs_open("quadsim", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "ant", g_antenna);
        nvs_commit(h);
        nvs_close(h);
    }
}
void loadAntenna()
{
    nvs_handle_t h;
    int32_t v;
    if (nvs_open("quadsim", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "ant", &v) == ESP_OK)
            g_antenna = v ? 1 : 0;
        nvs_close(h);
    }
}

constexpr uint32_t kToneReg = 0x600A041C;
constexpr int kToneAttenShift = 10;
constexpr uint32_t kToneAttenMask = 0xFFu << kToneAttenShift;
constexpr uint32_t kToneEnable = 1u << 18;
constexpr int kMaxAtten = 127;   // the field is a signed byte, stored negated
int g_baseAtten = -1;            // what the API set at backoff 0

inline volatile uint32_t& toneReg() { return *(volatile uint32_t*)kToneReg; }

int toneAttenNow()
{
    return -(int)(int8_t)((toneReg() & kToneAttenMask) >> kToneAttenShift);
}

// Set the tone's attenuation directly, in 0.25 dB steps above the API's own
// full-power setting; beyond the field's range the tone is switched off.
inline void fastLevel(int extraQuarterDb)
{
    int atten = g_baseAtten + (extraQuarterDb < 0 ? 0 : extraQuarterDb);
    uint32_t v = toneReg();
    if (atten > kMaxAtten) {
        v &= ~kToneEnable;
    } else {
        v = (v & ~kToneAttenMask) | ((uint32_t)(uint8_t)(-atten) << kToneAttenShift) | kToneEnable;
    }
    toneReg() = v;
}

bool inBand(int mhz) { return mhz >= kMinMhz && mhz <= kMaxMhz; }

void toneOff()
{
    g_preparedPll = 0;
    if (g_on)
        esp_phy_wifi_tx_tone(0, g_chan, 0);
    g_on = false;
}

// Carrier at mhz with this attenuation. The tone starts on the nearest 5 GHz
// Wi-Fi channel (5000 + 5 * n MHz); with exact on, the PLL is then moved to
// the exact frequency.
void toneOn(int mhz, int backoff)
{
    if (backoff < 0)
        backoff = 0;
    if (backoff > kMaxBackoff)
        backoff = kMaxBackoff;
    // The API takes a 5 GHz Wi-Fi channel (5000 + 5 * n MHz); above channel
    // 177 (5885) and below 36 (5180) it does not start the tone, so start on
    // the nearest valid one and move the PLL to the exact frequency.
    int chan = (mhz - 5000 + 2) / 5;
    if (chan > 177)
        chan = 177;
    if (chan < 36)
        chan = 36;
    if (!g_on || chan != g_chan || backoff != g_backoff) {
        if (g_on)
            esp_phy_wifi_tx_tone(0, g_chan, 0);
        esp_phy_wifi_tx_tone(1, (uint32_t)chan, (uint32_t)backoff);
        g_on = true;
        if (mhz != 5000 + 5 * chan && (g_exact || chan == 177 || chan == 36))
            g_radio.hop(mhz);
    }
    g_chan = chan;
    g_mhz = mhz;
    g_backoff = backoff;
}

// Tone on at the API's full power, then levels by register.
bool fastStart(int mhz)
{
    toneOn(mhz, 0);
    g_baseAtten = toneAttenNow();
    return g_baseAtten >= 0 && g_baseAtten <= kMaxAtten;
}

// Received power (dB) of a quad x metres along the track from the point
// closest to the gate antenna, d metres away. beamDeg <= 0 is a dome (no
// horn): distance alone shapes the pass.
//
// refM <= 0: relative model, 0 dB at the closest point whatever d is (d
// only sets the pass's width). refM > 0: absolute model, a quad refM away or
// closer reads at full power (the receiver saturates) and a farther one
// falls off as 1/r^2, so d sets the pass's strength as well: a close pass
// gets a flat top as wide as it stays within refM.
float g_refCm = 100;  // default reference distance for new quads and passes ("ref" command); 0 = relative model

float passDb(float x, float d, float beamDeg, float sidelobeDb, float refM = 0)
{
    const float r2 = d * d + x * x;
    float path;
    if (refM > 0) {
        const float ref2 = refM * refM;
        path = r2 <= ref2 ? 0.0f : -10.0f * log10f(r2 / ref2);
    } else {
        path = -10.0f * log10f(r2 / (d * d));
    }
    if (beamDeg <= 0)
        return path;
    const float theta = atanf(fabsf(x) / d) * 57.29578f;
    float g = -3.0f * (theta / beamDeg) * (theta / beamDeg);
    if (g < sidelobeDb)
        g = sidelobeDb;
    return path + g;
}

// One pass at speed through the beam. The fast path covers only the top
// (127 - base) / 4 dB, about 14 dB (the attenuation field is a signed byte
// and the API's full power already uses ~70 of it); the carrier is on while
// the pass is within that range and off outside it. peakDb is extra
// attenuation at the closest point.
void fly(int mhz, float kmh, float cm, float beamDeg, float sidelobeDb, float peakDb)
{
    const float v = kmh / 3.6f, d = cm / 100.0f;
    if (!fastStart(mhz)) {
        toneOff();
        say("err tone attenuation not readable");
        return;
    }
    const float rangeDb = (kMaxAtten - g_baseAtten) / 4.0f - peakDb;
    // how far either side of the closest point the pass stays within range
    float xEnd = d * 0.01f;
    while (xEnd < 200.0f && passDb(xEnd, d, beamDeg, sidelobeDb, g_refCm / 100.0f) - peakDb > -rangeDb)
        xEnd *= 1.02f;
    const int64_t halfUs = (int64_t)(xEnd / v * 1e6f);
    fastLevel(kMaxAtten + 1);   // silent until the pass starts
    const int64_t t0 = esp_timer_get_time() + 2000;
    const int64_t tPeak = t0 + halfUs;
    int64_t aboveUs = 0, last = t0, t;
    while ((t = esp_timer_get_time()) < t0 + 2 * halfUs) {
        if (t < t0)
            continue;
        const float x = v * (float)(t - tPeak) * 1e-6f;
        const float db = passDb(x, d, beamDeg, sidelobeDb, g_refCm / 100.0f) - peakDb;
        fastLevel((int)(-db * 4.0f + 0.5f));
        if (db > -3.0f - peakDb)
            aboveUs += t - last;
        last = t;
        const int64_t next = t + 250;
        while (esp_timer_get_time() < next) {
        }
    }
    toneOff();
    say("peak %lld us, carrier on %.1f ms (top %.1f dB), within 3 dB of peak %.1f ms, %.1f m/s",
        (long long)tPeak, 2 * halfUs / 1000.0, rangeDb, aboveUs / 1000.0, v);
}

// --- two quads from one radio (tone generators 1 and 2) ---

constexpr uint32_t kTone2Reg = 0x600A0420;
constexpr uint32_t kToneLowReg = 0x600A0428;   // bits 0-1 tone 1, 2-3 tone 2: low bits of the offset
constexpr float kOffsetUnitsPerMhz = 4096.0f / 240.0f;   // measured: ~58.6 kHz per step
constexpr float kMaxOffsetMhz = 22.0f;   // beyond ~+-20 MHz the TX filter removes the tone

struct Quad {
    bool used = false;
    int mhz = 0;
    float kmh = 0, cm = 0, beam = 0, startMs = 0, side = -20, peak = 0, refCm = 0;
};
Quad g_quads[2];

inline volatile uint32_t& reg(uint32_t a) { return *(volatile uint32_t*)a; }

// Set tone i (0, 1): offset word, attenuation above base (quarter-dB), on/off.
inline void toneSet(int i, int offsetWord, int extraQuarterDb, bool on)
{
    volatile uint32_t& r = reg(i ? kTone2Reg : kToneReg);
    int atten = g_baseAtten + (extraQuarterDb < 0 ? 0 : extraQuarterDb);
    if (atten > kMaxAtten)
        on = false;
    if (atten > kMaxAtten)
        atten = kMaxAtten;
    const uint32_t w = (uint32_t)offsetWord & 0xFFF;
    r = (r & 0xF0000000u) | (w >> 2) | ((uint32_t)(uint8_t)(-atten) << kToneAttenShift) | (on ? kToneEnable : 0);
    const int sh = i ? 2 : 0;
    reg(kToneLowReg) = (reg(kToneLowReg) & ~(3u << sh)) | ((w & 3) << sh);
}

bool goQuads(int lap)
{
    int n = 0, lo = 99999, hi = 0;
    for (const Quad& q : g_quads)
        if (q.used) {
            ++n;
            lo = q.mhz < lo ? q.mhz : lo;
            hi = q.mhz > hi ? q.mhz : hi;
        }
    if (!n) {
        say("err no quads defined");
        return false;
    }
    const int pll = (lo + hi) / 2;
    if (hi - pll > kMaxOffsetMhz || pll - lo > kMaxOffsetMhz) {
        say("err quads %d and %d MHz are too far apart for one radio (max %.0f MHz)", lo, hi, 2 * kMaxOffsetMhz);
        return false;
    }
    const bool prepared = g_preparedPll == pll && g_on;
    if (!prepared && !fastStart(pll)) {
        toneOff();
        say("err tone attenuation not readable");
        return false;
    }
    const float rangeDb = (kMaxAtten - g_baseAtten) / 4.0f;
    int64_t half[2] = {}, peakAt[2] = {}, end = 0;
    int word[2] = {};
    const int64_t t0 = esp_timer_get_time() + 2000;
    for (int i = 0; i < 2; ++i) {
        const Quad& q = g_quads[i];
        toneSet(i, 0, 0, false);
        if (!q.used)
            continue;
        const float d = q.cm / 100.0f, v = q.kmh / 3.6f;
        float x = d * 0.01f;
        while (x < 500.0f && passDb(x, d, q.beam, q.side, q.refCm / 100.0f) - q.peak > -rangeDb)
            x *= 1.02f;
        half[i] = (int64_t)(x / v * 1e6f);
        peakAt[i] = t0 + (int64_t)(q.startMs * 1000) + half[i];
        word[i] = (int)lroundf((q.mhz - pll) * kOffsetUnitsPerMhz);
        if (peakAt[i] + half[i] > end)
            end = peakAt[i] + half[i];
    }
    int64_t t;
    while ((t = esp_timer_get_time()) < end) {
        for (int i = 0; i < 2; ++i) {
            const Quad& q = g_quads[i];
            if (!q.used)
                continue;
            const int64_t dt = t - peakAt[i];
            if (dt < -half[i] || dt > half[i]) {
                toneSet(i, word[i], 0, false);
                continue;
            }
            const float x = q.kmh / 3.6f * (float)dt * 1e-6f;
            const float db = passDb(x, q.cm / 100.0f, q.beam, q.side, q.refCm / 100.0f) - q.peak;
            toneSet(i, word[i], (int)(-db * 4.0f + 0.5f), true);
        }
        const int64_t next = t + 250;
        while (esp_timer_get_time() < next) {
        }
    }
    toneSet(0, 0, 0, false);
    toneSet(1, 0, 0, false);
    if (!prepared) toneOff();
    for (int i = 0; i < 2; ++i)
        if (g_quads[i].used)
            say("{\"evt\":\"pass\",\"lap\":%d,\"quad\":%d,\"mhz\":%d,\"peak_us\":%lld,\"on_ms\":%.1f,"
                "\"range_db\":%.1f}",
                lap, i + 1, g_quads[i].mhz, (long long)peakAt[i], 2 * half[i] / 1000.0, rangeDb);
    return true;
}

// Bytes waiting on the console: used to stop a run of laps.
bool inputWaiting()
{
    uint8_t b;
    return usb_serial_jtag_read_bytes(&b, 1, 0) > 0;
}

// One pass: attenuation falls linearly (in dB) from floor to peak and back.
void pass(int mhz, int peak, int ms, int floorBackoff)
{
    const int64_t t0 = esp_timer_get_time();
    const int64_t half = (int64_t)ms * 500;
    bool peaked = false;
    for (;;) {
        const int64_t t = esp_timer_get_time() - t0;
        if (t >= 2 * half)
            break;
        const int64_t d = t < half ? half - t : t - half;
        const int b = peak + (int)((int64_t)(floorBackoff - peak) * d / half);
        toneOn(mhz, b);
        if (!peaked && t >= half) {
            peaked = true;
            say("peak %lu", (unsigned long)((t0 + half) / 1000));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    toneOff();
}

// --- race scripts: a whole heat, played from RAM ---
//
// A script is a list of passes, each with the time of its closest approach
// (t_ms after the start), a frequency and the pass parameters. "script check"
// plans it: overlapping passes form clusters; each cluster gets one PLL
// setting and up to two tones; a pass that cannot share the radio with the
// passes already in its cluster is skipped (and reported). "play" then runs
// the plan against the board's clock, updating levels every 250 us.
//
// Every pass is played as a tone offset +-18.5 MHz from the PLL, also when
// it is alone: an offset tone is ~10 dB weaker than one on the PLL, and a
// fixed offset keeps every pass at the same strength through the race. Odd
// Raceband channels sit below the PLL and even ones above, so adjacent
// Raceband pairs (R1+R2, R3+R4, ...) share a PLL.

constexpr int kMaxPasses = 1500;
constexpr float kPassOffsetMhz = 18.5f;
constexpr int64_t kRetuneUs = 2000;   // tone restart + PLL hop (~1.2 ms) with margin

struct ScriptPass {
    int32_t tMs;        // closest approach, ms after the start
    uint16_t mhz;
    uint8_t id;         // the quad it belongs to (for reports), 0 if not given
    int8_t tone;        // planned tone slot 0/1, -1 = skipped
    float kmh, cm, beam, side, peak, refCm;
    int32_t halfUs;     // carrier on from tMs - half to tMs + half
    int16_t pllMhz;     // planned PLL (integer MHz)
    int16_t word;       // planned tone offset word
    uint8_t skipWhy;    // 0 ok, 1 too far from the cluster's other pass, 2 three at once, 3 no time to retune
};

ScriptPass* g_script = nullptr;
int g_nScript = 0;
bool g_planned = false;
int g_nSkipped = 0, g_nClusters = 0;

// Non-blocking console output, for use while playing.
void emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void emit(const char* fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    usb_serial_jtag_write_bytes(buf, (size_t)n, 0);
}

// Half the time a pass spends within the drawable range, by bisection
// (passDb falls monotonically with |x|).
int32_t passHalfUs(const ScriptPass& p, float rangeDb)
{
    const float d = p.cm / 100.0f, v = p.kmh / 3.6f;
    float lo = 0, hi = 500.0f;
    if (passDb(hi, d, p.beam, p.side, p.refCm / 100.0f) - p.peak > -rangeDb)
        return (int32_t)(hi / v * 1e6f);
    for (int i = 0; i < 32; ++i) {
        const float mid = (lo + hi) / 2;
        if (passDb(mid, d, p.beam, p.side, p.refCm / 100.0f) - p.peak > -rangeDb)
            lo = mid;
        else
            hi = mid;
    }
    return (int32_t)(hi / v * 1e6f);
}

// Which side of the PLL a lone pass sits on: odd Raceband channels below,
// even ones above, anything else below.
int offsetSign(int mhz)
{
    static const int rb[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    for (int i = 0; i < 8; ++i)
        if (rb[i] == mhz)
            return (i % 2 == 0) ? -1 : 1;
    return -1;
}

void planScript(float rangeDb)
{
    g_nSkipped = g_nClusters = 0;
    // sort by carrier start (insertion sort: scripts are short and loaded in order)
    for (int i = 0; i < g_nScript; ++i)
        g_script[i].halfUs = passHalfUs(g_script[i], rangeDb);
    for (int i = 1; i < g_nScript; ++i) {
        ScriptPass key = g_script[i];
        const int64_t ks = (int64_t)key.tMs * 1000 - key.halfUs;
        int j = i - 1;
        while (j >= 0 && (int64_t)g_script[j].tMs * 1000 - g_script[j].halfUs > ks) {
            g_script[j + 1] = g_script[j];
            --j;
        }
        g_script[j + 1] = key;
    }
    int64_t clusterEnd = INT64_MIN;
    int pll = 0;
    int64_t toneEnd[2] = {INT64_MIN, INT64_MIN};
    int clusterLo = 0, clusterHi = 0;
    for (int i = 0; i < g_nScript; ++i) {
        ScriptPass& p = g_script[i];
        const int64_t start = (int64_t)p.tMs * 1000 - p.halfUs, end = (int64_t)p.tMs * 1000 + p.halfUs;
        p.tone = -1;
        p.skipWhy = 0;
        if (start >= clusterEnd + kRetuneUs) {
            // a new cluster: the radio is free, put the PLL for this pass
            ++g_nClusters;
            pll = (int)lroundf(p.mhz - offsetSign(p.mhz) * kPassOffsetMhz);
            clusterLo = clusterHi = p.mhz;
            toneEnd[0] = toneEnd[1] = INT64_MIN;
        } else if (start < clusterEnd + kRetuneUs && start >= clusterEnd) {
            p.skipWhy = 3;   // the radio is free, but not for long enough to retune
        } else {
            // overlaps the cluster: it must fit the PLL already chosen
            const int lo = p.mhz < clusterLo ? p.mhz : clusterLo, hi = p.mhz > clusterHi ? p.mhz : clusterHi;
            if (fabsf((float)(p.mhz - pll)) > kMaxOffsetMhz || hi - lo > 2 * kMaxOffsetMhz)
                p.skipWhy = 1;
        }
        if (!p.skipWhy) {
            const int slot = toneEnd[0] <= start ? 0 : (toneEnd[1] <= start ? 1 : -1);
            if (slot < 0) {
                p.skipWhy = 2;
            } else {
                p.tone = (int8_t)slot;
                toneEnd[slot] = end;
                p.pllMhz = (int16_t)pll;
                p.word = (int16_t)lroundf((p.mhz - pll) * kOffsetUnitsPerMhz);
                clusterLo = p.mhz < clusterLo ? p.mhz : clusterLo;
                clusterHi = p.mhz > clusterHi ? p.mhz : clusterHi;
                if (end > clusterEnd)
                    clusterEnd = end;
            }
        }
        if (p.skipWhy)
            ++g_nSkipped;
    }
    g_planned = true;
}

const char* skipReason(int why)
{
    switch (why) {
    case 1: return "too far in frequency from a pass it overlaps (needs another board)";
    case 2: return "a third pass at the same time (needs another board)";
    case 3: return "too soon after the previous pass to retune (needs another board)";
    default: return "";
    }
}

float scriptRangeDb()
{
    // the drawable range depends on the API's base attenuation, which is
    // read when a tone starts; use the last value seen, else the measured 14 dB
    return g_baseAtten >= 0 ? (kMaxAtten - g_baseAtten) / 4.0f : 14.0f;
}

void scriptSummary()
{
    int64_t last = 0;
    for (int i = 0; i < g_nScript; ++i) {
        const int64_t e = (int64_t)g_script[i].tMs * 1000 + g_script[i].halfUs;
        if (e > last)
            last = e;
    }
    say("{\"evt\":\"plan\",\"passes\":%d,\"skipped\":%d,\"clusters\":%d,\"duration_ms\":%lld,\"range_db\":%.1f}",
        g_nScript, g_nSkipped, g_nClusters, (long long)(last / 1000), scriptRangeDb());
    for (int i = 0; i < g_nScript; ++i)
        if (g_script[i].tone < 0)
            say("{\"evt\":\"skip\",\"t_ms\":%ld,\"mhz\":%u,\"id\":%u,\"reason\":\"%s\"}", (long)g_script[i].tMs,
                g_script[i].mhz, g_script[i].id, skipReason(g_script[i].skipWhy));
}

// Play the planned script with t = 0 at board time t0Us. Any byte received
// stops it. Returns the number of passes played.
void playScript(int64_t t0Us)
{
    // wait for the start, stoppable
    while (esp_timer_get_time() < t0Us - 3000) {
        if (inputWaiting()) {
            say("{\"evt\":\"done\",\"played\":0,\"stopped\":true}");
            return;
        }
        vTaskDelay(1);
    }
    int next = 0;            // next pass to start
    int live[2] = {-1, -1};  // pass index on each tone
    int curPll = 0;
    int played = 0;
    bool stopped = false;
    int64_t lastProgress = t0Us, tick = 0;
    // skip to the first planned pass
    while (next < g_nScript && g_script[next].tone < 0)
        ++next;
    toneSet(0, 0, 0, false);
    toneSet(1, 0, 0, false);
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const int64_t t = now - t0Us;
        // retune for the next cluster while both tones are idle
        if (next < g_nScript && live[0] < 0 && live[1] < 0) {
            const ScriptPass& p = g_script[next];
            const int64_t start = (int64_t)p.tMs * 1000 - p.halfUs;
            if (p.pllMhz != curPll && t >= start - kRetuneUs) {
                fastStart(p.pllMhz);
                toneSet(0, 0, 0, false);
                toneSet(1, 0, 0, false);
                curPll = p.pllMhz;
            }
        }
        // start passes whose carrier is due
        while (next < g_nScript) {
            const ScriptPass& p = g_script[next];
            if (p.tone < 0) {
                ++next;
                continue;
            }
            if (t < (int64_t)p.tMs * 1000 - p.halfUs || p.pllMhz != curPll)
                break;
            live[p.tone] = next;
            ++next;
        }
        // levels
        for (int k = 0; k < 2; ++k) {
            const int i = live[k];
            if (i < 0)
                continue;
            const ScriptPass& p = g_script[i];
            const int64_t dt = t - (int64_t)p.tMs * 1000;
            if (dt > p.halfUs) {
                toneSet(k, p.word, 0, false);
                live[k] = -1;
                ++played;
                emit("{\"evt\":\"pass\",\"t_ms\":%ld,\"mhz\":%u,\"id\":%u,\"peak_us\":%lld}", (long)p.tMs, p.mhz, p.id,
                     (long long)(t0Us + (int64_t)p.tMs * 1000));
                continue;
            }
            const float x = p.kmh / 3.6f * (float)dt * 1e-6f;
            const float db = passDb(x, p.cm / 100.0f, p.beam, p.side, p.refCm / 100.0f) - p.peak;
            toneSet(k, p.word, (int)(-db * 4.0f + 0.5f), true);
        }
        if (next >= g_nScript && live[0] < 0 && live[1] < 0)
            break;
        // stop and progress, about every 1 ms / 1 s
        if ((++tick & 3) == 0 && inputWaiting()) {
            stopped = true;
            break;
        }
        if (now - lastProgress >= 1000000) {
            lastProgress = now;
            emit("{\"evt\":\"progress\",\"t_ms\":%lld,\"played\":%d}", (long long)(t / 1000), played);
        }
        const int64_t until = now + 250;
        while (esp_timer_get_time() < until) {
        }
    }
    toneSet(0, 0, 0, false);
    toneSet(1, 0, 0, false);
    toneOff();
    say("{\"evt\":\"done\",\"played\":%d,\"skipped\":%d,\"stopped\":%s}", played, g_nSkipped,
        stopped ? "true" : "false");
}

void scriptCommand(const char* line, int nOut)
{
    char sub[12] = {};
    sscanf(line, "%*s %11s", sub);
    if (!strcmp(sub, "clear")) {
        g_nScript = 0;
        g_planned = false;
    } else if (!strcmp(sub, "add")) {
        float f[9] = {};
        const int nf = sscanf(line, "%*s %*s %f %f %f %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                              &f[6], &f[7], &f[8]);
        if (nf < 7 || !inBand((int)f[1]) || f[2] <= 0 || f[3] <= 0 || f[6] < 0 || f[8] < 0) {
            say("err script add <t_ms> <MHz> <km/h> <closest cm> <beam deg, 0 = dome> <sidelobe dB> <peak dB> [id]"
                " [ref cm]");
            return;
        }
        if (!g_script)
            g_script = (ScriptPass*)malloc(sizeof(ScriptPass) * kMaxPasses);
        if (!g_script || g_nScript >= kMaxPasses) {
            say("err script full (%d passes)", kMaxPasses);
            return;
        }
        ScriptPass& p = g_script[g_nScript++];
        p = ScriptPass{};
        p.tMs = (int32_t)f[0];
        p.mhz = (uint16_t)f[1];
        p.kmh = f[2];
        p.cm = f[3];
        p.beam = f[4];
        p.side = f[5];
        p.peak = f[6];
        p.id = nf >= 8 ? (uint8_t)f[7] : 0;
        p.refCm = nf >= 9 ? f[8] : g_refCm;
        g_planned = false;
    } else if (!strcmp(sub, "check")) {
        planScript(scriptRangeDb());
        scriptSummary();
    } else if (!strcmp(sub, "count")) {
        say("{\"passes\":%d}", g_nScript);
    } else {
        say("err script clear | add ... | check | count");
        return;
    }
    (void)nOut;
    say("ok");
}

void command(char* line)
{
    char cmd[12] = {};
    long a[6] = {};
    const int n = sscanf(line, "%11s %ld %ld %ld %ld %ld %ld", cmd, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) - 1;
    if (!strcmp(cmd, "on") && n >= 1) {
        if (!inBand((int)a[0])) {
            say("err %ld MHz is outside %d-%d", a[0], kMinMhz, kMaxMhz);
            return;
        }
        toneOn((int)a[0], n >= 2 ? (int)a[1] : 40);
    } else if (!strcmp(cmd, "off")) {
        toneOff();
    } else if (!strcmp(cmd, "pass") && n >= 3) {
        if (!inBand((int)a[0])) {
            say("err outside %d-%d MHz", kMinMhz, kMaxMhz);
            return;
        }
        pass((int)a[0], (int)a[1], (int)a[2], n >= 4 ? (int)a[3] : kMaxBackoff);
    } else if (!strcmp(cmd, "laps") && n >= 5) {
        if (!inBand((int)a[0])) {
            say("err outside %d-%d MHz", kMinMhz, kMaxMhz);
            return;
        }
        for (long i = 0; i < a[4]; ++i) {
            const uint32_t start = nowMs();
            pass((int)a[0], (int)a[1], (int)a[2], n >= 6 ? (int)a[5] : kMaxBackoff);
            const uint32_t spent = nowMs() - start;
            if ((long)spent < a[3])
                vTaskDelay(pdMS_TO_TICKS(a[3] - spent));
        }
    } else if (!strcmp(cmd, "tpow")) {
        // how long the RF test API takes to start, stop and re-level a tone
        const int mhz = n >= 1 ? (int)a[0] : 5769;
        int64_t t0 = esp_timer_get_time();
        esp_phy_wifi_tx_tone(1, (uint32_t)((mhz - 5000 + 2) / 5), 40);
        int64_t t1 = esp_timer_get_time();
        g_radio.hop(mhz);
        int64_t t2 = esp_timer_get_time();
        esp_phy_wifi_tx_tone(0, (uint32_t)((mhz - 5000 + 2) / 5), 0);
        int64_t t3 = esp_timer_get_time();
        say("tone on %lld us, hop %lld us, tone off %lld us", (long long)(t1 - t0), (long long)(t2 - t1),
            (long long)(t3 - t2));
        g_on = false;
    } else if (!strcmp(cmd, "prepare") && n >= 1) {
        if (!inBand((int)a[0])) { say("err prepare <MHz>"); return; }
        toneOff();
        if (!fastStart((int)a[0])) { toneOff(); say("err tone attenuation"); return; }
        toneSet(0, 0, 0, false);
        toneSet(1, 0, 0, false);
        g_preparedPll = (int)a[0];
        say("prepared %d MHz muted, base attenuation %d", g_preparedPll, g_baseAtten);
    } else if (!strcmp(cmd, "level") && n >= 2) {
        g_preparedPll = 0;
        if (!inBand((int)a[0])) {
            say("err outside %d-%d MHz", kMinMhz, kMaxMhz);
            return;
        }
        if (!g_on || g_mhz != (int)a[0] || g_backoff != 0)
            fastStart((int)a[0]);
        fastLevel((int)(a[1] * 4));
        say("level -%ld dB: atten %d (base %d), reg %08lx", a[1], toneAttenNow(), g_baseAtten,
            (unsigned long)toneReg());
    } else if (!strcmp(cmd, "fly") || !strcmp(cmd, "flaps")) {
        float f[8] = {};
        const int nf = sscanf(line, "%*s %f %f %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                              &f[6], &f[7]);
        if (!inBand((int)f[0])) {
            say("err outside %d-%d MHz", kMinMhz, kMaxMhz);
            return;
        }
        const bool laps = !strcmp(cmd, "flaps");
        const float side = laps ? (nf >= 7 ? f[6] : -20) : (nf >= 5 ? f[4] : -20);
        const float peak = laps ? (nf >= 8 ? f[7] : 0) : (nf >= 6 ? f[5] : 0);
        const int count = laps ? (int)f[5] : 1;
        for (int i = 0; i < count; ++i) {
            const uint32_t start = nowMs();
            fly((int)f[0], f[1], f[2], f[3], side, peak);
            const uint32_t spent = nowMs() - start;
            if (laps && (float)spent < f[4])
                vTaskDelay(pdMS_TO_TICKS((uint32_t)f[4] - spent));
        }
    } else if (!strcmp(cmd, "pidx")) {
        // TX power index (register 0x600A0408: bit 23 force, bits 17-22 index;
        // a higher index is less power). Only indices at or above the one the
        // chip chose are allowed: this can only lower the power.
        volatile uint32_t& r = *(volatile uint32_t*)0x600A0408;
        const uint32_t v = r;
        const int cur = (int)((v >> 17) & 0x3F);
        if (n >= 1 && a[0] < 0) {
            r = v & ~(1u << 23);
        } else if (n >= 1) {
            static int chipIdx = -1;
            if (chipIdx < 0 || !(v & (1u << 23)))
                chipIdx = cur;
            int idx = (int)a[0] < chipIdx ? chipIdx : (int)a[0];
            if (idx > 63)
                idx = 63;
            r = (v & ~(0x3Fu << 17)) | ((uint32_t)idx << 17) | (1u << 23);
        }
        const uint32_t w = r;
        say("pwr index %d forced %d (reg %08lx)", (int)((w >> 17) & 0x3F), (int)((w >> 23) & 1), (unsigned long)w);
    } else if (!strcmp(cmd, "tone2") && n >= 4) {
        // Raw tone generator fields, for finding out what they do:
        //   tone2 <which 1|2> <offset> <extraAtten quarter-dB> <on 0|1>
        // offset: the 12-bit frequency word (low 2 bits in 0x600A0428, the
        // rest in bits 0-9 of the tone register). Attenuation is added to the
        // API's full-power base, never below it.
        volatile uint32_t& r1 = *(volatile uint32_t*)0x600A041C;
        volatile uint32_t& r2 = *(volatile uint32_t*)0x600A0420;
        volatile uint32_t& rl = *(volatile uint32_t*)0x600A0428;
        if (g_baseAtten < 0) {
            say("err start a tone first (level <MHz> 0)");
            return;
        }
        volatile uint32_t& r = a[0] == 2 ? r2 : r1;
        const uint32_t off = (uint32_t)a[1] & 0xFFF;
        int atten = g_baseAtten + (a[2] < 0 ? 0 : (int)a[2]);
        if (atten > kMaxAtten)
            atten = kMaxAtten;
        const uint32_t keepTop = r & 0xF0000000u;
        r = keepTop | (off >> 2) | ((uint32_t)(uint8_t)(-atten) << 10) | (a[3] ? (1u << 18) : 0);
        const int lowShift = a[0] == 2 ? 2 : 0;
        rl = (rl & ~(3u << lowShift)) | ((off & 3) << lowShift);
        say("tone regs 41c %08lx 420 %08lx 428 %08lx", (unsigned long)r1, (unsigned long)r2, (unsigned long)rl);
    } else if (!strcmp(cmd, "quad")) {
        float f[9] = {};
        const int nf = sscanf(line, "%*s %f %f %f %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                              &f[6], &f[7], &f[8]);
        const int i = (int)f[0] - 1;
        if (nf < 6 || i < 0 || i > 1 || !inBand((int)f[1]) || f[2] <= 0 || f[3] <= 0 || f[8] < 0) {
            say("err quad <1|2> <MHz> <km/h> <closest cm> <beam deg, 0 = dome> <start ms> [sidelobe dB] [peak dB]"
                " [ref cm]");
            return;
        }
        Quad& q = g_quads[i];
        q.used = true;
        q.mhz = (int)f[1];
        q.kmh = f[2];
        q.cm = f[3];
        q.beam = f[4];
        q.startMs = f[5];
        q.side = nf >= 7 ? f[6] : -20;
        q.peak = nf >= 8 ? f[7] : 0;
        q.refCm = nf >= 9 ? f[8] : g_refCm;
    } else if (!strcmp(cmd, "go")) {
        // go [laps] [periodMs]: fly the defined quads, repeatedly; any byte
        // received between laps stops the run (the byte is discarded).
        const int laps = n >= 1 && a[0] > 0 ? (int)a[0] : 1;
        const long period = n >= 2 ? a[1] : 0;
        int done = 0;
        bool stopped = false;
        for (int lap = 1; lap <= laps; ++lap) {
            const uint32_t start = nowMs();
            if (!goQuads(lap))
                return;   // goQuads said why
            ++done;
            if (lap == laps)
                break;
            while ((long)(nowMs() - start) < period) {
                if (inputWaiting()) {
                    stopped = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (stopped || inputWaiting()) {
                stopped = true;
                break;
            }
        }
        say("{\"evt\":\"done\",\"laps\":%d,\"stopped\":%s}", done, stopped ? "true" : "false");
    } else if (!strcmp(cmd, "clear")) {
        g_quads[0].used = g_quads[1].used = false;
    } else if (!strcmp(cmd, "script")) {
        scriptCommand(line, n);
        return;   // scriptCommand says ok/err
    } else if (!strcmp(cmd, "clock")) {
        say("{\"clock_us\":%lld}", (long long)esp_timer_get_time());
    } else if (!strcmp(cmd, "play")) {
        // play at <board clock us> | play in <ms>
        char how[4] = {};
        long long when = 0;
        if (sscanf(line, "%*s %3s %lld", how, &when) != 2 || (strcmp(how, "at") && strcmp(how, "in"))) {
            say("err play at <board clock us> | play in <ms>");
            return;
        }
        if (!g_nScript) {
            say("err no script loaded");
            return;
        }
        if (g_baseAtten < 0) {   // learn the tone's base attenuation (and so the range) once
            fastStart(5800);
            toneOff();
        }
        if (!g_planned)
            planScript(scriptRangeDb());
        const int64_t t0 = !strcmp(how, "at") ? (int64_t)when : esp_timer_get_time() + when * 1000;
        if (t0 < esp_timer_get_time()) {
            say("err start time is in the past");
            return;
        }
        playScript(t0);
    } else if (!strcmp(cmd, "ant")) {
        if (n >= 1) {
            g_antenna = a[0] ? 1 : 0;
            applyAntenna();
            saveAntenna();
        }
        say("antenna %d (%s)", g_antenna, g_antenna ? "u.FL" : "on-board ceramic");
    } else if (!strcmp(cmd, "ref")) {
        // ref <cm>: default reference distance for quads and passes defined
        // after it (0 = the relative model, as before)
        float v = 0;
        if (sscanf(line, "%*s %f", &v) == 1) {
            if (v < 0 || v > 10000) {
                say("err ref <cm>, 0 = relative model");
                return;
            }
            g_refCm = v;
        }
        say("{\"ref_cm\":%.0f}", g_refCm);
    } else if (!strcmp(cmd, "info")) {
        say("{\"app\":\"quadsim\",\"version\":\"%s\",\"min_mhz\":%d,\"max_mhz\":%d,\"max_quads\":2,"
            "\"max_pair_mhz\":%.0f,\"range_db\":%.1f,\"ref_cm\":%.0f,\"clock_us\":%lld}",
            kQuadsimVersion, kMinMhz, kMaxMhz, 2 * kMaxOffsetMhz,
            g_baseAtten >= 0 ? (kMaxAtten - g_baseAtten) / 4.0 : 14.0, g_refCm, (long long)esp_timer_get_time());
    } else if (!strcmp(cmd, "exact") && n >= 1) {
        g_exact = a[0] != 0;
    } else if (!strcmp(cmd, "status")) {
        say("quadsim: %s %d MHz (channel %d) backoff %d (-%.2f dB), exact %d, clock %lu ms",
            g_on ? "on" : "off", g_mhz, g_chan, g_backoff, g_backoff / 4.0, g_exact, (unsigned long)nowMs());
    } else {
        say("err unknown command: info clock script play quad go clear level on off fly flaps pass laps status (bench: pidx tone2 exact tpow)");
        return;
    }
    say("ok");
}

void consoleTask(void*)
{
    char line[96];
    size_t len = 0;
    for (;;) {
        uint8_t buf[64];
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; ++i) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                if (len) {
                    line[len] = '\0';
                    command(line);
                }
                len = 0;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = (char)buf[i];
            }
        }
    }
}

}  // namespace

extern "C" void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb);
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    // RF test library setup, as ESP-IDF's examples/phy/cert_test
    loadAntenna();
    applyAntenna();
    esp_wifi_power_domain_on();
    esp_phy_rftest_config(1);
    esp_phy_rftest_init();
    xTaskCreate(consoleTask, "quadsim", 6144, nullptr, 20, nullptr);
}
