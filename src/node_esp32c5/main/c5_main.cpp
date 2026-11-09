// RotorHazard node firmware for the ESP32-C5: up to eight RotorHazard
// nodes measured by the C5's own 5 GHz radio, no RX5808 modules.
//
// One task does everything, in turn: a scan cycle (one capture per distinct
// seat frequency, each reading fed to RotorHazard's RssiNode), then any
// bytes waiting on the links. The C5 is single-core; a cycle takes about a
// millisecond, which is also how long a command can wait.
//
// Links: the native USB port (USB Serial/JTAG) for a direct cable to the
// timer, and optionally a UART (RHNODE_C5_LINK_TX/RX) for an RS422 or radio
// link. One host at a time: a link that has just been used keeps the node
// for kLinkHoldMs.
//
// Console: a line starting with '!' on either link (RH never sends 0x21),
// or any line on UART0. '!help' lists the commands.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "c5_radio.h"
#include "c5_rhglue.h"
#include "c5_scan.h"

#include "config.h"
#include "RssiNode.h"
#include "commands.h"

#ifndef RHNODE_C5_LINK_TX
#define RHNODE_C5_LINK_TX -1
#endif
#ifndef RHNODE_C5_LINK_RX
#define RHNODE_C5_LINK_RX -1
#endif
#ifndef RHNODE_C5_BOARD
#define RHNODE_C5_BOARD "c5"
#endif

// informational strings (c5_rhglue.cpp)
extern const char *firmwareVersionString;
extern const char *firmwareProcTypeString;

extern "C" uint32_t rhc5_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
extern "C" uint32_t rhc5_micros(void) { return (uint32_t)esp_timer_get_time(); }
extern "C" void rhc5_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

namespace {

using namespace c5;

const char* TAG = "rhnode";
constexpr uint32_t kLinkHoldMs = 3000;
// The RotorHazard link on the board's TX/RX pads (GPIO11/12) is UART0 itself;
// the text console on UART0 is then off (the link is binary). On other pins
// the link is UART1 and UART0 keeps the console.
constexpr bool kLinkOnUart0 = RHNODE_C5_LINK_TX == 11 && RHNODE_C5_LINK_RX == 12;
constexpr uart_port_t kLinkUart = kLinkOnUart0 ? UART_NUM_0 : UART_NUM_1;
constexpr uart_port_t kConsoleUart = UART_NUM_0;

C5Radio g_radio;
Scanner g_scanner(g_radio);
int g_seats = MULTI_RHNODE_MAX;

// Antenna: the C5-Zero's RF switch (ANT_Ctrl = GPIO26, pulled low by 200k):
// 0 = the on-board ceramic antenna (the board's default), 1 = the u.FL socket.
constexpr gpio_num_t kAntCtrlGpio = GPIO_NUM_26;
int g_antenna = 0;

void applyAntenna()
{
    gpio_reset_pin(kAntCtrlGpio);
    gpio_set_direction(kAntCtrlGpio, GPIO_MODE_OUTPUT);
    gpio_set_level(kAntCtrlGpio, g_antenna ? 1 : 0);
}
bool g_settingsDirty = false;
uint32_t g_dirtySinceMs = 0;

uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }

// --- transports ---

class UsbTransport : public Transport {
public:
    void write(const uint8_t* d, size_t n) override { usb_serial_jtag_write_bytes(d, n, pdMS_TO_TICKS(5)); }
};

class UartTransport : public Transport {
public:
    explicit UartTransport(uart_port_t port) : port_(port) {}
    void write(const uint8_t* d, size_t n) override { uart_write_bytes(port_, d, n); }

private:
    uart_port_t port_;
};

UsbTransport g_usbOut;
UartTransport g_linkOut(kLinkUart);
UartTransport g_consoleOut(kConsoleUart);
RhLink g_usbLink(g_usbOut);
RhLink g_uartLink(g_linkOut);
bool g_uartLinkOn = false;

struct Owner {
    RhLink* link = nullptr;
    uint32_t lastMs = 0;
} g_owner;

void feedLink(RhLink& link, const uint8_t* d, size_t n)
{
    const uint32_t now = rhc5_millis();
    if (g_owner.link && g_owner.link != &link && now - g_owner.lastMs < kLinkHoldMs) {
        // another host has the node; let only console lines through
        if (d[0] != '!')
            return;
    }
    const uint32_t before = link.commands();
    link.feed(d, n);
    if (link.commands() != before) {
        g_owner.link = &link;
        g_owner.lastMs = now;
    }
}

// --- settings ---

void loadSettings()
{
    nvs_handle_t h;
    if (nvs_open("rhc5", NVS_READONLY, &h) != ESP_OK)
        return;
    ScanConfig& c = g_scanner.config();
    int32_t v;
    if (nvs_get_i32(h, "gain", &v) == ESP_OK) c.gain = v;
    if (nvs_get_i32(h, "n", &v) == ESP_OK) c.captureN = v;
    if (nvs_get_i32(h, "image", &v) == ESP_OK) c.image = (ImageMode)v;
    if (nvs_get_i32(h, "floor", &v) == ESP_OK) c.floorDbQ8 = v;
    if (nvs_get_i32(h, "cpd", &v) == ESP_OK) c.countsPerDb = v;
    if (nvs_get_i32(h, "hclip", &v) == ESP_OK) c.holdClipMin = v;
    if (nvs_get_i32(h, "hoob", &v) == ESP_OK) c.holdOobDbQ8 = v;
    if (nvs_get_i32(h, "skip", &v) == ESP_OK) g_radio.setHopSkip(v);
    if (nvs_get_i32(h, "seats", &v) == ESP_OK && v >= 1 && v <= MULTI_RHNODE_MAX) g_seats = v;
    if (nvs_get_i32(h, "ant", &v) == ESP_OK) g_antenna = v ? 1 : 0;
    int32_t taps = 32, hop = 8;
    nvs_get_i32(h, "taps", &taps);
    nvs_get_i32(h, "hop", &hop);
    if (!g_scanner.configureMeter(taps, hop))
        g_scanner.configureMeter(32, 8);
    CalTable t;
    size_t len = sizeof(t);
    if (nvs_get_blob(h, "cal", &t, &len) == ESP_OK && len == sizeof(t) && t.n <= kMaxCalPoints)
        g_scanner.cal() = t;
    nvs_close(h);
}

void saveSettings()
{
    nvs_handle_t h;
    if (nvs_open("rhc5", NVS_READWRITE, &h) != ESP_OK)
        return;
    const ScanConfig& c = g_scanner.config();
    const Visit* v = g_scanner.visitCount() ? &g_scanner.visit(0) : nullptr;
    nvs_set_i32(h, "gain", c.gain);
    nvs_set_i32(h, "n", c.captureN);
    nvs_set_i32(h, "image", (int32_t)c.image);
    nvs_set_i32(h, "floor", c.floorDbQ8);
    nvs_set_i32(h, "cpd", c.countsPerDb);
    nvs_set_i32(h, "hclip", c.holdClipMin);
    nvs_set_i32(h, "hoob", c.holdOobDbQ8);
    nvs_set_i32(h, "skip", g_radio.hopSkip());
    nvs_set_i32(h, "seats", g_seats);
    nvs_set_i32(h, "ant", g_antenna);
    if (v && v->meter) {
        nvs_set_i32(h, "taps", v->meter->taps());
        nvs_set_i32(h, "hop", v->meter->hop());
    }
    nvs_set_blob(h, "cal", &g_scanner.cal(), sizeof(CalTable));
    nvs_commit(h);
    nvs_close(h);
}

void markDirty()
{
    g_settingsDirty = true;
    g_dirtySinceMs = rhc5_millis();
}

// --- console ---

void say(Transport& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void say(Transport& out, const char* fmt, ...)
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
    out.write((const uint8_t*)buf, (size_t)n);
}

float db(int32_t q8) { return q8 / 256.0f; }

// Readings at both LO sides for one pilot, with the meter on the pilot and
// on its mirror: the mirror figure is what an image would add to another seat.
void rfReport(Transport& out, int pilot)
{
    BandMeter normal, mirror;
    for (int side = -1; side <= 1; side += 2) {
        const int lo = pilot + side * kOffsetMhz;
        const int sign = side < 0 ? kSpectrumSign : -kSpectrumSign;
        normal.configure(32, 8, sign);
        mirror.configure(32, 8, -sign);
        int64_t t;
        int32_t bandSum = 0, mirSum = 0, wideSum = 0, clip = 0;
        const int reps = 32;
        for (int i = 0; i < reps; ++i) {
            g_radio.hop(lo);
            g_radio.ensureGain(g_scanner.config().gain);
            g_radio.capture(2048, &t);
            const MeterResult a = normal.measure(g_radio.words(), 2048);
            const MeterResult b = mirror.measure(g_radio.words(), 2048);
            bandSum += a.bandDbQ8;
            mirSum += b.bandDbQ8;
            wideSum += a.wideDbQ8;
            clip += a.clipped;
        }
        say(out, "rf %d LO %d (%s): pilot %.2f dB, mirror %.2f dB, wide %.2f dB, clipped %ld",
            pilot, lo, side < 0 ? "below" : "above", db(bandSum / reps), db(mirSum / reps),
            db(wideSum / reps), (long)clip);
    }
}

void scanReport(Transport& out, int cycles)
{
    const int n = g_scanner.visitCount();
    int64_t sum[kMaxSeats] = {}, wide[kMaxSeats] = {};
    int32_t mx[kMaxSeats], clip[kMaxSeats] = {};
    for (int i = 0; i < kMaxSeats; ++i)
        mx[i] = -100 * 256;
    const ScanStats s0 = g_scanner.stats();
    for (int c = 0; c < cycles; ++c) {
        g_scanner.cycle(nowUs);
        for (int i = 0; i < n; ++i) {
            const MeterResult& r = g_scanner.lastResult(i);
            sum[i] += r.bandDbQ8;
            wide[i] += r.wideDbQ8;
            if (r.bandDbQ8 > mx[i])
                mx[i] = r.bandDbQ8;
            clip[i] += r.clipped;
        }
    }
    const ScanStats& s = g_scanner.stats();
    for (int i = 0; i < n; ++i) {
        const Visit& v = g_scanner.visit(i);
        int32_t br, bi;
        v.image.beta(&br, &bi);
        say(out, "%d MHz LO %d%c seats %02x: mean %.2f max %.2f wide %.2f clip %ld beta %.4f%+.4fj",
            v.pilotMhz, v.loMhz, v.side < 0 ? '-' : '+', v.seatMask, db((int32_t)(sum[i] / cycles)),
            db(mx[i]), db((int32_t)(wide[i] / cycles)), (long)clip[i], br / 32768.0, bi / 32768.0);
    }
    say(out, "%d cycles, last %lu us, held %lu, failed %lu", cycles, (unsigned long)s.lastCycleUs,
        (unsigned long)(s.held - s0.held), (unsigned long)(s.failed - s0.failed));
}

// Average every visit's raw band power over ms (in linear power), for
// calibration. Results per visit: mean dB, max dB.
void calMeasure(int ms, int32_t* meanDb, int32_t* maxDb)
{
    uint64_t sum[kMaxSeats] = {};
    int32_t mx[kMaxSeats];
    uint32_t cnt[kMaxSeats] = {};
    for (int i = 0; i < kMaxSeats; ++i)
        mx[i] = -100 * 256;
    const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        g_scanner.cycle(nowUs);
        for (int i = 0; i < g_scanner.visitCount(); ++i) {
            const int32_t d = g_scanner.lastResult(i).bandDbQ8;
            sum[i] += powerFromDbQ8(d);
            ++cnt[i];
            if (d > mx[i])
                mx[i] = d;
        }
    }
    for (int i = 0; i < g_scanner.visitCount(); ++i) {
        meanDb[i] = cnt[i] ? dbQ8FromPower(sum[i] / cnt[i]) - dbQ8FromPower(1u << 20) : -100 * 256;
        maxDb[i] = mx[i];
    }
}

void calConsole(const char* line, Transport& out)
{
    char sub[12] = {};
    float a = 0, b = 0;
    const int n = sscanf(line, "%*s %11s %f %f", sub, &a, &b) - 1;
    CalTable& t = g_scanner.cal();
    const int32_t fallback = g_scanner.config().noiseFloorDbQ8;
    if (!strcmp(sub, "show")) {
        say(out, "{\"cal\":\"%s\",\"points\":%d}", t.enabled ? "on" : "off", t.n);
        for (int i = 0; i < t.n; ++i)
            say(out, "{\"mhz\":%u,\"gain_db\":%.2f,\"floor_db\":%.2f}", t.p[i].mhz, t.p[i].gainDbQ8 / 256.0,
                (t.p[i].floorDbQ8 == INT16_MIN ? fallback : t.p[i].floorDbQ8) / 256.0);
    } else if (!strcmp(sub, "set") && n >= 2) {
        if (b < -20 || b > 20 || !t.set((int)a, (int32_t)lroundf(b * 256))) {
            say(out, "err cal set <MHz> <gain dB, -20..20> (max %d points)", kMaxCalPoints);
            return;
        }
    } else if (!strcmp(sub, "clear")) {
        t = CalTable();
    } else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        t.enabled = !strcmp(sub, "on");
    } else if (!strcmp(sub, "floor")) {
        // nothing transmitting: store the noise floor at every scanned frequency
        int32_t mean[kMaxSeats], mx[kMaxSeats];
        calMeasure(n >= 1 && a > 0 ? (int)a : 3000, mean, mx);
        for (int i = 0; i < g_scanner.visitCount(); ++i) {
            const int mhz = g_scanner.visit(i).pilotMhz;
            if (mx[i] - mean[i] > 6 * 256) {
                say(out, "err %d MHz is not quiet (max %.1f dB over its mean): turn every VTX off", mhz,
                    (mx[i] - mean[i]) / 256.0);
                return;
            }
            t.setFloor(mhz, mean[i]);
            say(out, "{\"mhz\":%d,\"floor_db\":%.2f}", mhz, mean[i] / 256.0);
        }
    } else if (!strcmp(sub, "capture")) {
        // raw mean (linear power) and max per scanned frequency over ms
        int32_t mean[kMaxSeats], mx[kMaxSeats];
        calMeasure(n >= 1 && a > 0 ? (int)a : 1000, mean, mx);
        for (int i = 0; i < g_scanner.visitCount(); ++i) {
            const int mhz = g_scanner.visit(i).pilotMhz;
            const int32_t fl = t.floorAt(mhz, fallback);
            say(out, "{\"mhz\":%d,\"db\":%.2f,\"max_db\":%.2f,\"floor_db\":%.2f,\"gain_db\":%.2f}", mhz,
                mean[i] / 256.0, mx[i] / 256.0, fl / 256.0, t.gainAt(mhz) / 256.0);
        }
    } else if (!strcmp(sub, "save")) {
        saveSettings();
    } else {
        say(out, "err cal show | set <MHz> <dB> | clear | on | off | floor [ms] | capture [ms] | save");
        return;
    }
    say(out, "ok");
}

void console(const char* line, Transport& out)
{
    char cmd[16] = {};
    long a = 0, b = 0;
    const int nargs = sscanf(line, "%15s %ld %ld", cmd, &a, &b) - 1;
    ScanConfig& c = g_scanner.config();
    const RadioStatus& rs = g_radio.status();

    if (!strcmp(cmd, "cal")) {
        calConsole(line, out);
        return;
    }
    if (!strcmp(cmd, "help")) {
        say(out, "status | scan [cycles] | rf MHz | raw [LO] [stock] | freq seat MHz | gain N | n samples");
        say(out, "meter taps hop | image off|cancel | map floorDb countsPerDb | hold clipMin oobDb");
        say(out, "skip mask | stock 0|1 | seats N | ant 0|1 (0 ceramic, 1 u.FL) | stats | save | reboot");
        say(out, "cal show | set <MHz> <dB> | clear | on | off | floor [ms] | capture [ms] | save");
    } else if (!strcmp(cmd, "status")) {
        say(out, "%s %s, board %s, API %d, seats %d", firmwareVersionString, firmwareProcTypeString,
            RHNODE_C5_BOARD, NODE_API_LEVEL, RssiNode::multiRssiNodeCount);
        say(out, "wifi=%d live=%d (boot checks %d/%d) rxon=%d gainmax=%d gain=%d gainreg=%08lx", rs.wifi,
            rs.live, rs.firstCheck, rs.afterTxOff, rs.rxOnNeeded, rs.gainMax, c.gain,
            (unsigned long)g_radio.gainReg());
        say(out, "captures stock %lu direct %lu timeouts %lu incomplete %lu", (unsigned long)rs.stockCaptures,
            (unsigned long)rs.directCaptures, (unsigned long)rs.timeouts, (unsigned long)rs.incomplete);
        const Visit* v0 = g_scanner.visitCount() ? &g_scanner.visit(0) : nullptr;
        say(out, "n=%d meter %d taps hop %d, image %s, map floor %.1f dB x%d, hold clip>=%d oob>=%.1f dB, skip %d",
            c.captureN, v0 ? v0->meter->taps() : 0, v0 ? v0->meter->hop() : 0,
            c.image == ImageMode::Cancel ? "cancel" : "off", db(c.floorDbQ8), c.countsPerDb, c.holdClipMin,
            db(c.holdOobDbQ8), g_radio.hopSkip());
        say(out, "links: usb %lu cmds, uart %s (UART%d TX %d RX %d, %d baud) %lu cmds, %lu bad checksums",
            (unsigned long)g_usbLink.commands(), g_uartLinkOn ? "on" : "off", (int)kLinkUart, RHNODE_C5_LINK_TX,
            RHNODE_C5_LINK_RX, SERIAL_BAUD_RATE, (unsigned long)g_uartLink.commands(),
            (unsigned long)g_uartLink.badChecksums());
    } else if (!strcmp(cmd, "scan")) {
        scanReport(out, nargs >= 1 && a > 0 ? (int)a : 200);
    } else if (!strcmp(cmd, "stats")) {
        const ScanStats& s = g_scanner.stats();
        say(out, "cycles %lu captures %lu failed %lu held %lu clipped %lu cycle %lu us (max %lu)",
            (unsigned long)s.cycles, (unsigned long)s.captures, (unsigned long)s.failed, (unsigned long)s.held,
            (unsigned long)s.clippedCaptures, (unsigned long)s.lastCycleUs, (unsigned long)s.maxCycleUs);
        g_scanner.clearStats();
    } else if (!strcmp(cmd, "raw")) {
        // one capture at LO a (default 5790): sample statistics, for bring-up
        const int lo = nargs >= 1 && a > 0 ? (int)a : 5790;
        const bool stock = nargs >= 2 && b != 0;
        g_radio.setStockOnly(stock);
        g_radio.hop(lo);
        g_radio.ensureGain(c.gain);
        int64_t t;
        const bool ok = g_radio.capture(256, &t);
        g_radio.setStockOnly(false);
        const volatile uint32_t* w = g_radio.words();
        int imin = 9999, imax = -9999, qmin = 9999, qmax = -9999, gains[128] = {};
        long si = 0, sq = 0;
        for (int i = 0; i < 256; ++i) {
            const int I = sampleI(w[i]), Q = sampleQ(w[i]);
            imin = I < imin ? I : imin; imax = I > imax ? I : imax;
            qmin = Q < qmin ? Q : qmin; qmax = Q > qmax ? Q : qmax;
            si += I; sq += Q;
            ++gains[sampleGain(w[i])];
        }
        say(out, "raw LO %d %s ok=%d I %d..%d mean %ld, Q %d..%d mean %ld, gainreg %08lx", lo,
            stock ? "stock" : "direct", ok, imin, imax, si / 256, qmin, qmax, sq / 256,
            (unsigned long)g_radio.gainReg());
        char hist[96] = "gain field:";
        for (int g = 0; g < 128; ++g)
            if (gains[g])
                snprintf(hist + strlen(hist), sizeof(hist) - strlen(hist), " %d x%d", g, gains[g]);
        say(out, "%s", hist);
        say(out, "words %08lx %08lx %08lx %08lx %08lx %08lx", (unsigned long)w[0], (unsigned long)w[1],
            (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[254], (unsigned long)w[255]);
    } else if (!strcmp(cmd, "trace") && nargs >= 2) {
        // trace <seat mask> <ms>: every scan cycle's raw band power for up to
        // two seats, with each capture's own time; then one line per sample:
        // "t <us> <dB> [<dB>]". For pass-shape checks.
        int seats[2] = {-1, -1}, ns = 0;
        for (int sIdx = 0; sIdx < kMaxSeats && ns < 2; ++sIdx)
            if (a & (1L << sIdx))
                seats[ns++] = sIdx;
        int vis[2] = {-1, -1};
        g_scanner.cycle(nowUs);   // make sure the visit list is current
        for (int k = 0; k < ns; ++k)
            for (int i = 0; i < g_scanner.visitCount(); ++i)
                if (g_scanner.visit(i).seatMask & (1u << seats[k]))
                    vis[k] = i;
        if (!ns || vis[0] < 0) {
            say(out, "err no such seat in the scan (set its frequency first)");
            return;
        }
        const int maxN = 6000;
        struct Rec { uint32_t t; int16_t db[2]; };
        Rec* rec = (Rec*)malloc(sizeof(Rec) * maxN);
        if (!rec) {
            say(out, "err no memory");
            return;
        }
        int nrec = 0;
        const int64_t end = esp_timer_get_time() + (int64_t)b * 1000;
        while (esp_timer_get_time() < end && nrec < maxN) {
            g_scanner.cycle(nowUs);
            rec[nrec].t = (uint32_t)g_scanner.lastTimeUs(vis[0]);
            for (int k = 0; k < 2; ++k)
                rec[nrec].db[k] = vis[k] >= 0 ? (int16_t)(g_scanner.lastResult(vis[k]).bandDbQ8 / 16) : 0;
            ++nrec;
        }
        for (int i = 0; i < nrec; ++i) {
            if (ns > 1)
                say(out, "t %lu %.2f %.2f", (unsigned long)rec[i].t, rec[i].db[0] / 16.0, rec[i].db[1] / 16.0);
            else
                say(out, "t %lu %.2f", (unsigned long)rec[i].t, rec[i].db[0] / 16.0);
        }
        free(rec);
    } else if (!strcmp(cmd, "prof")) {
        // where a visit's time goes: hop, capture, meter, RH node update
        const int lo = nargs >= 1 && a > 0 ? (int)a : 5790;
        const int reps = 200;
        int64_t tHop = 0, tCap = 0, tMeter = 0, tNode = 0, t;
        BandMeter m;
        m.configure(32, 8, kSpectrumSign);
        for (int i = 0; i < reps; ++i) {
            int64_t t0 = esp_timer_get_time();
            g_radio.hop(lo + (i & 1) * 37);   // alternate, like a scan
            int64_t t1 = esp_timer_get_time();
            g_radio.ensureGain(c.gain);
            g_radio.capture(c.captureN, &t);
            int64_t t2 = esp_timer_get_time();
            const MeterResult r = m.measure(g_radio.words(), c.captureN);
            int64_t t3 = esp_timer_get_time();
            rhSeatSample(nullptr, 0, (uint32_t)(t / 1000), r.bandDbQ8, false);
            int64_t t4 = esp_timer_get_time();
            tHop += t1 - t0; tCap += t2 - t1; tMeter += t3 - t2; tNode += t4 - t3;
        }
        say(out, "per visit (us, n=%d): hop %.1f capture %.1f meter %.1f node %.1f", c.captureN,
            tHop / (double)reps, tCap / (double)reps, tMeter / (double)reps, tNode / (double)reps);
        for (int skip = 0; skip < 4; ++skip) {
            const int old = g_radio.hopSkip();
            g_radio.setHopSkip(skip);
            int64_t s0 = esp_timer_get_time();
            for (int i = 0; i < reps; ++i)
                g_radio.hop(lo + (i & 1) * 37);
            say(out, "hop skip %d: %.1f us", skip, (esp_timer_get_time() - s0) / (double)reps);
            g_radio.setHopSkip(old);
        }
    } else if (!strcmp(cmd, "rf") && nargs >= 1) {
        rfReport(out, (int)a);
    } else if (!strcmp(cmd, "freq") && nargs >= 2 && a >= 0 && a < g_seats) {
        RssiNode::rssiNodeArray[a].setVtxFreq((uint16_t)b);
        RssiNode::rssiNodeArray[a].setRxModuleToFreq((uint16_t)b);
    } else if (!strcmp(cmd, "gain") && nargs >= 1) {
        c.gain = (int)a;
        markDirty();
    } else if (!strcmp(cmd, "n") && nargs >= 1 && a >= 64 && a <= kMaxCapture) {
        c.captureN = (int)a;
        markDirty();
    } else if (!strcmp(cmd, "meter") && nargs >= 2) {
        if (!g_scanner.configureMeter((int)a, (int)b)) {
            say(out, "err taps and hop must be multiples of 8, taps 8-64");
            return;
        }
        markDirty();
    } else if (!strcmp(cmd, "image")) {
        c.image = strstr(line, "cancel") ? ImageMode::Cancel : ImageMode::Off;
        markDirty();
    } else if (!strcmp(cmd, "map") && nargs >= 2) {
        c.floorDbQ8 = (int32_t)(a * 256);
        c.countsPerDb = (int)b;
        markDirty();
    } else if (!strcmp(cmd, "hold") && nargs >= 2) {
        c.holdClipMin = (int)a;
        c.holdOobDbQ8 = (int32_t)(b * 256);
        markDirty();
    } else if (!strcmp(cmd, "skip") && nargs >= 1) {
        g_radio.setHopSkip((int)a & 3);
        markDirty();
    } else if (!strcmp(cmd, "stock") && nargs >= 1) {
        g_radio.setStockOnly(a != 0);
    } else if (!strcmp(cmd, "seats") && nargs >= 1 && a >= 1 && a <= MULTI_RHNODE_MAX) {
        g_seats = (int)a;
        saveSettings();
        say(out, "ok, restart to apply");
        return;
    } else if (!strcmp(cmd, "ant")) {
        if (nargs >= 1) {
            g_antenna = a ? 1 : 0;
            applyAntenna();
            markDirty();
        }
        say(out, "antenna %d (%s)", g_antenna, g_antenna ? "u.FL" : "on-board ceramic");
    } else if (!strcmp(cmd, "save")) {
        saveSettings();
        g_settingsDirty = false;
    } else if (!strcmp(cmd, "reboot")) {
        say(out, "ok");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else {
        say(out, "err unknown command (try !help)");
        return;
    }
    say(out, "ok");
}

// --- the node task ---

void pollLinks()
{
    uint8_t buf[64];
    int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
    if (n > 0)
        feedLink(g_usbLink, buf, (size_t)n);
    if (g_uartLinkOn) {
        size_t avail = 0;
        if (uart_get_buffered_data_len(kLinkUart, &avail) == ESP_OK && avail > 0) {
            n = uart_read_bytes(kLinkUart, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
            if (n > 0)
                feedLink(g_uartLink, buf, (size_t)n);
        }
    }
    if (kLinkOnUart0)
        return;
    // UART0: plain console lines (no '!' needed)
    static char line[96];
    static size_t len = 0;
    size_t avail = 0;
    if (uart_get_buffered_data_len(kConsoleUart, &avail) == ESP_OK && avail > 0) {
        n = uart_read_bytes(kConsoleUart, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
        for (int i = 0; i < n; ++i) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                if (len) {
                    line[len] = '\0';
                    console(line[0] == '!' ? line + 1 : line, g_consoleOut);
                }
                len = 0;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = (char)buf[i];
            }
        }
    }
}

void nodeTask(void*)
{
    uint32_t k = 0;
    for (;;) {
        g_scanner.cycle(nowUs);
        pollLinks();
        if ((++k & 63) == 0 && g_settingsDirty && rhc5_millis() - g_dirtySinceMs > 2000) {
            saveSettings();
            g_settingsDirty = false;
        }
    }
}

void startLinks()
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = 1024;
    usb.tx_buffer_size = 1024;
    usb_serial_jtag_driver_install(&usb);

    uart_config_t u = {};
    u.baud_rate = 115200;
    u.data_bits = UART_DATA_8_BITS;
    u.parity = UART_PARITY_DISABLE;
    u.stop_bits = UART_STOP_BITS_1;
    u.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    u.source_clk = UART_SCLK_DEFAULT;
    if (!kLinkOnUart0) {
        uart_driver_install(kConsoleUart, 512, 512, 0, nullptr, 0);
        uart_param_config(kConsoleUart, &u);
    }

    if (RHNODE_C5_LINK_TX >= 0 && RHNODE_C5_LINK_RX >= 0) {
        u.baud_rate = SERIAL_BAUD_RATE;
        uart_driver_install(kLinkUart, 1024, 1024, 0, nullptr, 0);
        uart_param_config(kLinkUart, &u);
        uart_set_pin(kLinkUart, RHNODE_C5_LINK_TX, RHNODE_C5_LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        g_uartLinkOn = true;
    }
    g_usbLink.setConsole(console);
    g_uartLink.setConsole(console);
}

}  // namespace

extern "C" void app_main(void)
{
    // links first: nothing is sent until a host asks, and RH's discovery
    // waits about 2 s after opening the port, which boot must fit inside
    startLinks();
    // NVS before loadSettings: C5Radio::begin() initialises it too, but later,
    // and every saved setting (cal table, antenna, gain) was silently skipped
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    loadSettings();
    applyAntenna();
    if (!g_radio.begin(g_scanner.config().gain))
        ESP_LOGE(TAG, "radio boot check failed: captures are not live");
    rhBegin(g_scanner, g_seats);
    xTaskCreate(nodeTask, "rhnode", 6144, nullptr, 5, nullptr);
}
