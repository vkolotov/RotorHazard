// Host tests for the ESP32-C5 node: the meter, the scanner and RotorHazard's
// own node core (RssiNode.cpp, commands.cpp) driven over its serial protocol.
//   make -C test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "c5_meter.h"
#include "c5_rhglue.h"
#include "c5_scan.h"
#include "config.h"
#include "RssiNode.h"
#include "commands.h"
#include "sim_radio.h"

using namespace c5;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        if (cond) {                                                               \
            ++g_pass;                                                             \
        } else {                                                                  \
            ++g_fail;                                                             \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                \
            printf(__VA_ARGS__);                                                  \
            printf("\n");                                                         \
        }                                                                         \
    } while (0)

// --- platform clock for RH's millis() ---
static uint32_t g_ms = 0;
extern "C" uint32_t rhc5_millis(void) { return g_ms; }
extern "C" uint32_t rhc5_micros(void) { return g_ms * 1000u; }
extern "C" void rhc5_delay_ms(uint32_t ms) { g_ms += ms; }
static uint32_t fakeUs() { return g_ms * 1000u; }

static double dbOf(int32_t q8) { return q8 / 256.0; }

// mean band dB over several captures (each capture has random phases)
static double meanBand(SimRadio& r, const BandMeter& m, int n = 256, int reps = 64,
                       int32_t br = 0, int32_t bi = 0)
{
    double s = 0;
    for (int i = 0; i < reps; ++i) {
        int64_t t;
        r.capture(n, &t);
        s += dbOf(m.measure(r.words(), n, br, bi).bandDbQ8);
    }
    return s / reps;
}

static void testLog()
{
    CHECK(dbQ8FromPower(1) == 0, "%d", dbQ8FromPower(1));
    const double d = dbOf(dbQ8FromPower(1000000));
    CHECK(std::fabs(d - 60.0) < 0.01, "%f", d);
    const double d2 = dbOf(dbQ8FromPower(3));
    CHECK(std::fabs(d2 - 4.7712) < 0.01, "%f", d2);
    const double d3 = dbOf(dbQ8FromPower(123456789012ull));
    CHECK(std::fabs(d3 - 10 * std::log10(123456789012.0)) < 0.01, "%f", d3);
}

static void testMeterTone()
{
    // pilot 5800, LO below: a tone of amplitude A reads 20*log10(A) dB
    BandMeter m;
    CHECK(m.configure(32, 8, kSpectrumSign), "configure");
    SimRadio r;
    r.lo = 5790;
    r.noise = 0.0;
    r.frontEnd = false;
    r.emitters = {{5800, 100}};
    const double db = meanBand(r, m);
    CHECK(std::fabs(db - 40.0) < 0.3, "tone 100 LSB read %.2f dB", db);

    // DC offset is removed
    r.dc = {30, -20};
    const double dbDc = meanBand(r, m);
    CHECK(std::fabs(dbDc - 40.0) < 0.3, "with DC read %.2f dB", dbDc);
    r.dc = {0, 0};

    // a tone 4 MHz off the pilot is still in the flat part
    r.emitters = {{5804, 100}};
    const double dbOff = meanBand(r, m);
    CHECK(std::fabs(dbOff - 40.0) < 1.0, "+4 MHz read %.2f dB", dbOff);

    // noise floor: 1 LSB rms per component, a ~12 MHz band out of 80
    r.emitters.clear();
    r.noise = 1.0;
    const double floor = meanBand(r, m);
    CHECK(floor > -8 && floor < -2, "floor %.2f dB", floor);
    printf("  meter floor (1 LSB rms noise): %.2f dB\n", floor);
}

static void testMeterSelectivity()
{
    // a strong VTX one Raceband channel away (37 MHz) must stay near the floor
    BandMeter m;
    m.configure(32, 8, kSpectrumSign);
    SimRadio r;
    r.lo = 5658 - 10;   // R1, LO below
    r.noise = 1.0;
    r.frontEnd = false;   // the FIR on its own
    const double floor = meanBand(r, m);
    for (double off : {-37.0, 37.0, -20.0, 20.0}) {
        r.emitters = {{5658 + off, 300}};   // ~49.5 dB, near full scale
        const double db = meanBand(r, m);
        // +-20 MHz: a pilot at a 20 MHz-spaced neighbour (A/B/E bands)
        CHECK(db - floor < 1.5, "neighbour at %+.0f MHz raised R1 by %.2f dB", off, db - floor);
        printf("  neighbour %+.0f MHz at 49.5 dB: R1 %+.2f dB over floor\n", off, db - floor);
    }
    // the designed response, for the record
    printf("  response: 6 MHz %.1f dB, 10 MHz %.1f, 15 MHz %.1f, 20 MHz %.1f, 37 MHz %.1f\n",
           m.responseDb(6), m.responseDb(10), m.responseDb(15), m.responseDb(20), m.responseDb(37));
    CHECK(m.responseDb(14) < -50, "14 MHz %.1f", m.responseDb(14));
    CHECK(m.responseDb(4.5) > -1.0, "4.5 MHz %.1f", m.responseDb(4.5));
}

static void testImage()
{
    // I/Q imbalance: a VTX on the pilot's mirror (pilot - 20 MHz with the LO
    // below) leaks into the pilot band. Cancellation removes it.
    BandMeter m;
    m.configure(32, 8, kSpectrumSign);
    SimRadio r;
    r.lo = 5800 - 10;
    r.noise = 1.0;
    r.frontEnd = false;
    r.nu = {0.10, 0.05};   // ~-19 dB image, like the bench's ~18 dB
    const double floor = meanBand(r, m);
    r.emitters = {{5780, 200}};   // 46 dB on the mirror
    const double ghost = meanBand(r, m);
    printf("  mirror VTX at 46 dB, |nu| -19 dB: ghost %.2f dB (floor %.2f)\n", ghost, floor);
    CHECK(ghost - floor > 20, "uncorrected image should show: %.2f", ghost - floor);

    // estimate nu blind from the captures themselves
    ImageEstimator est;
    for (int i = 0; i < 1024; ++i) {
        int64_t t;
        r.capture(256, &t);
        est.add(m.measure(r.words(), 256));
    }
    int32_t br, bi;
    est.beta(&br, &bi);
    printf("  estimated nu %.4f%+.4fj (true 0.1000+0.0500j)\n", br / 32768.0, bi / 32768.0);
    CHECK(std::fabs(br / 32768.0 - 0.10) < 0.01 && std::fabs(bi / 32768.0 - 0.05) < 0.01, "nu estimate");
    const double fixed = meanBand(r, m, 256, 64, br, bi);
    printf("  with cancellation: %.2f dB (%.1f dB better)\n", fixed, ghost - fixed);
    CHECK(fixed - floor < 6, "cancelled image still %.2f dB over floor", fixed - floor);

    // cancellation must not disturb the pilot itself
    r.emitters = {{5800, 100}};
    const double pilot = meanBand(r, m, 256, 64, br, bi);
    CHECK(std::fabs(pilot - 40.0) < 0.5, "pilot with cancellation %.2f", pilot);
}

static void testChooseSide()
{
    const uint16_t rb[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    printf("  Raceband worst leak per side (model):");
    for (int i = 0; i < 8; ++i) {
        const int s = Scanner::chooseSide(rb[i], rb, 8);
        const float below = Scanner::leakDb(rb[i], -1, rb, 8), above = Scanner::leakDb(rb[i], 1, rb, 8);
        printf(" R%d %.0f/%.0f%s", i + 1, below, above, s < 0 ? "-" : "+");
        // the chosen side is never the clearly worse one
        CHECK((s < 0 ? below : above) <= (s < 0 ? above : below) + 1.0f || (i == 7 && s < 0), "R%d side", i + 1);
    }
    printf("\n");
    CHECK(Scanner::chooseSide(5917, rb, 8) == -1, "R8 LO above is out of range");
    // Boscam A, 20 MHz-ish spacing: 5865 5845 5825 5805 5785 5765 5745 5725
    const uint16_t a[] = {5725, 5745, 5765, 5785, 5805, 5825, 5845, 5865};
    // the lowest pilot's low-side image (5705) is clear
    CHECK(Scanner::chooseSide(5725, a, 8) == -1, "A8 low side");
    // the highest pilot's high-side image (5885) is clear, its low one (5845) is a pilot
    CHECK(Scanner::chooseSide(5865, a, 8) == 1, "A1 high side");
    // two pilots: 5800 and 5780 -> 5800 must put its image above (5820)
    const uint16_t two[] = {5780, 5800};
    CHECK(Scanner::chooseSide(5800, two, 2) == 1, "5800 with 5780");
    CHECK(Scanner::chooseSide(5780, two, 2) == -1, "5780 with 5800");
    // edges: 5180's low-side LO (5170) is in range; 5917's high one (5927) is not
    CHECK(Scanner::chooseSide(5917, rb, 1) == -1, "5917");
}

struct SinkLog {
    int count[kMaxSeats] = {};
    int32_t last[kMaxSeats] = {};
    int held = 0;
};
static void logSink(void* ctx, int seat, uint32_t, int32_t db, bool held)
{
    SinkLog* l = (SinkLog*)ctx;
    ++l->count[seat];
    l->last[seat] = db;
    if (held)
        ++l->held;
}

static void testScannerAndHold()
{
    SimRadio r;
    Scanner s(r);
    SinkLog log;
    s.setSink(logSink, &log);
    s.setSeatFreq(0, 5658);
    s.setSeatFreq(1, 5695);
    s.setSeatFreq(2, 5658);   // shares seat 0's capture
    s.setSeatFreq(3, 5945);   // E8: out of range, stays off
    s.cycle(fakeUs);
    CHECK(s.visitCount() == 2, "visits %d", s.visitCount());
    CHECK(r.hops == 2, "hops %d", r.hops);
    CHECK(log.count[0] == 1 && log.count[1] == 1 && log.count[2] == 1 && log.count[3] == 0, "sink counts");
    CHECK(s.visit(0).loMhz == 5648 && s.visit(1).loMhz == 5685, "LOs %d %d", s.visit(0).loMhz, s.visit(1).loMhz);

    // out-of-band overload: a VTX 20 MHz away (A/B/E-band spacing) so strong
    // that the ADC clips while tuned for this seat. The seat must hold its
    // reading, not jump.
    s.setSeatFreq(0, 5800);
    s.setSeatFreq(1, 5820);
    s.setSeatFreq(2, 0);
    for (int i = 0; i < 20; ++i)
        s.cycle(fakeUs);
    const int32_t before = log.last[0];
    r.emitters = {{5820, 5000}};   // clips the ADC at 5800's LO too
    log.held = 0;
    for (int i = 0; i < 20; ++i)
        s.cycle(fakeUs);
    printf("  5820 overload: 5800 %+.2f dB (held %d of 20, clipped %d), 5820 %.2f dB\n",
           dbOf(log.last[0] - before), log.held, s.lastResult(0).clipped, dbOf(log.last[1]));
    CHECK(s.lastResult(0).clipped > 0, "5800 capture clipped");
    CHECK(log.held >= 20, "5800 readings held: %d", log.held);
    CHECK(log.last[0] == before, "5800 moved %.2f", dbOf(log.last[0] - before));
    // 5820 clips too, but its own band holds most of the power: not held
    CHECK(dbOf(log.last[1]) > 45, "5820 reads %.2f", dbOf(log.last[1]));

    // a failed capture repeats the last reading
    r.emitters.clear();
    s.cycle(fakeUs);
    const int32_t good = log.last[0];
    r.failNext = true;
    s.cycle(fakeUs);
    CHECK(log.last[0] == good, "failed capture held");
    CHECK(s.stats().failed == 1, "failed %u", s.stats().failed);

    // seat off and back on
    s.setSeatFreq(1, 1111);
    s.cycle(fakeUs);
    CHECK(s.visitCount() == 1, "after off: %d", s.visitCount());
    CHECK(s.visit(0).pilotMhz == 5800, "remaining %u", s.visit(0).pilotMhz);

    // scale mapping
    s.config().floorDbQ8 = -8 * 256;
    s.config().countsPerDb = 64;
    CHECK(s.rssiCounts(-8 * 256, true) == 1, "clamp low");
    CHECK(s.rssiCounts(0, true) == 512, "%d", s.rssiCounts(0, true));
    CHECK(s.rssiCounts(0, false) == 32, "%d", s.rssiCounts(0, false));
    CHECK(s.rssiCounts(100 * 256, true) == 4094, "clamp high");
}

// --- RotorHazard protocol, against RH's own node core ---

class CaptureOut : public Transport {
public:
    std::vector<uint8_t> bytes;
    void write(const uint8_t* d, size_t n) override { bytes.insert(bytes.end(), d, d + n); }
};

static bool readCmd(RhLink& link, CaptureOut& out, uint8_t cmd, size_t size, std::vector<uint8_t>* data)
{
    out.bytes.clear();
    link.feed(&cmd, 1);
    if (out.bytes.size() != size + 1)
        return false;
    uint8_t sum = 0;
    for (size_t i = 0; i < size; ++i)
        sum += out.bytes[i];
    if (sum != out.bytes[size])
        return false;
    data->assign(out.bytes.begin(), out.bytes.begin() + size);
    return true;
}

static void writeCmd(RhLink& link, uint8_t cmd, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> b = {cmd};
    uint8_t sum = 0;
    for (uint8_t p : payload) {
        b.push_back(p);
        sum += p;
    }
    b.push_back(sum);
    link.feed(b.data(), b.size());
}

static uint16_t be16(const std::vector<uint8_t>& d, size_t at) { return (uint16_t)((d[at] << 8) | d[at + 1]); }

static void testRhProtocol()
{
    SimRadio r;
    r.stepUs = 1000 / 8;   // ~1 kHz per seat with 8 visits
    Scanner s(r);
    rhBegin(s, 8);
    CaptureOut out;
    RhLink link(out);
    std::vector<uint8_t> d;

    CHECK(readCmd(link, out, READ_REVISION_CODE, 2, &d) && be16(d, 0) == ((0x25 << 8) | NODE_API_LEVEL),
          "revision %02x%02x", d.size() > 1 ? d[0] : 0, d.size() > 1 ? d[1] : 0);
    CHECK(readCmd(link, out, READ_MULTINODE_COUNT, 1, &d) && d[0] == 8, "multinode");
    CHECK(readCmd(link, out, READ_RHFEAT_FLAGS, 2, &d) && be16(d, 0) == 0, "features");
    CHECK(readCmd(link, out, READ_FW_PROCTYPE, 16, &d) && !memcmp(d.data(), "ESP32C5", 7), "proctype");

    // Raceband on all eight seats, 12-bit RSSI, thresholds on the 12-bit scale
    const uint16_t rb[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    for (int i = 0; i < 8; ++i) {
        writeCmd(link, WRITE_CURNODE_INDEX, {(uint8_t)i});
        writeCmd(link, WRITE_FREQUENCY, {(uint8_t)(rb[i] >> 8), (uint8_t)rb[i]});
        CHECK(readCmd(link, out, TEST_RX_REGISTER, 1, &d) && d[0] == 1, "seat %d register test", i);
        writeCmd(link, WRITE_ADC_RESOLUTION, {12});
        CHECK(readCmd(link, out, READ_ADC_RESOLUTION, 1, &d) && d[0] == 12, "adc res");
        // floor reads ~-5 dB -> ~190 counts; enter 30 dB above it, exit 20
        const uint16_t enter = 2400, exit = 1700;
        writeCmd(link, WRITE_ENTER_AT_LEVEL, {(uint8_t)(enter >> 8), (uint8_t)enter});
        writeCmd(link, WRITE_EXIT_AT_LEVEL, {(uint8_t)(exit >> 8), (uint8_t)exit});
    }
    CHECK(link.badChecksums() == 0, "checksums");
    CHECK(s.visitCount() == 0 || s.visitCount() == 8, "visits %d", s.visitCount());

    // an unsupported frequency fails RH's register test
    writeCmd(link, WRITE_CURNODE_INDEX, {7});
    writeCmd(link, WRITE_FREQUENCY, {(uint8_t)(5945 >> 8), (uint8_t)(5945 & 0xFF)});
    CHECK(readCmd(link, out, TEST_RX_REGISTER, 1, &d) && d[0] == 0, "E8 register test");
    writeCmd(link, WRITE_FREQUENCY, {(uint8_t)(5917 >> 8), (uint8_t)(5917 & 0xFF)});

    // quiet for a second, then a quad flies past seat 3 (R4, 5769): a
    // triangle from the floor to 45 dB and back over 600 ms, peak at t0+300
    auto run = [&](int ms, double peakAmp, int peakAtMs, int halfWidthMs, uint32_t t0) {
        for (int k = 0; k < ms; ++k) {
            const int t = (int)(g_ms - t0);
            double a = 0;
            const int dt = std::abs(t - peakAtMs);
            if (peakAmp > 0 && dt < halfWidthMs)
                a = peakAmp * (1.0 - (double)dt / halfWidthMs);
            r.emitters.clear();
            if (a > 0.5)
                r.emitters.push_back({5769, a});
            g_ms += 1;
            r.clockUs = (int64_t)g_ms * 1000;
            s.cycle(fakeUs);
        }
    };
    run(1200, 0, 0, 0, g_ms);
    writeCmd(link, WRITE_CURNODE_INDEX, {3});
    CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 11, &d), "pass stats");
    const uint8_t lap0 = d[0];
    const uint16_t floorRssi = be16(d, 3);
    printf("  seat 4 floor RSSI %u (12-bit)\n", floorRssi);
    CHECK(floorRssi > 100 && floorRssi < 400, "floor %u", floorRssi);

    const uint32_t t0 = g_ms;
    run(1500, 178, 300, 300, t0);   // 178 LSB = 45 dB
    CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 11, &d), "pass stats");
    const uint8_t lap1 = d[0];
    const uint16_t msSince = be16(d, 1);
    const uint16_t passPeak = be16(d, 7);
    const int peakTime = (int)(g_ms - msSince) - (int)t0;
    printf("  pass: laps %u -> %u, peak RSSI %u, timed at t0%+d ms (true peak t0+300)\n", lap0, lap1, passPeak, peakTime);
    CHECK(lap1 == (uint8_t)(lap0 + 1), "one lap: %u -> %u", lap0, lap1);
    CHECK(std::abs(peakTime - 300) <= 25, "pass time t0%+d", peakTime);
    CHECK(passPeak > 3000, "peak %u", passPeak);

    // the other seats counted nothing; print what leaked (front-end model)
    printf("  other seats' rise during the pass:");
    for (int i = 0; i < 8; ++i) {
        if (i == 3)
            continue;
        writeCmd(link, WRITE_CURNODE_INDEX, {(uint8_t)i});
        CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 11, &d), "pass stats %d", i);
        CHECK(d[0] == 0, "seat %d counted a lap", i);
        const uint16_t peak = be16(d, 5);   // node peak since frequency set
        printf(" %d:%+.1fdB", i + 1, ((int)peak - (int)floorRssi) / 64.0);
        CHECK(peak < 1700, "seat %d node peak %u reached ExitAt", i, peak);
    }
    printf("\n");

    // normalisation coefficients reach the reading: an offset of +100 counts
    writeCmd(link, WRITE_CURNODE_INDEX, {3});
    writeCmd(link, WRITE_NORM_PIVOT, {0, 1});
    writeCmd(link, WRITE_NORM_OFFSET, {(uint8_t)((uint16_t)-100 >> 8), (uint8_t)((uint16_t)-100 & 0xFF)});
    run(600, 0, 0, 0, g_ms);
    CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 11, &d), "pass stats");
    const uint16_t shifted = be16(d, 3);
    CHECK(std::abs((int)shifted - (int)floorRssi - 100) < 30, "normalised %u vs %u", shifted, floorRssi);

    // legacy 8-bit scale: 1/16
    writeCmd(link, WRITE_NORM_PIVOT, {0, 0});
    writeCmd(link, WRITE_ADC_RESOLUTION, {10});
    run(600, 0, 0, 0, g_ms);
    CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 11, &d), "pass stats");
    const uint16_t legacy = be16(d, 3);
    CHECK(std::abs((int)legacy - (int)floorRssi / 16) <= 3, "legacy %u vs %u", legacy, floorRssi / 16);

    // console escape
    bool consoleSaw = false;
    static bool* flag;
    flag = &consoleSaw;
    link.setConsole([](const char* line, Transport&) { *flag = !strcmp(line, "status"); });
    const char* cl = "!status\n";
    link.feed((const uint8_t*)cl, strlen(cl));
    CHECK(consoleSaw, "console line");
    CHECK(readCmd(link, out, READ_REVISION_CODE, 2, &d), "protocol after console");
}

static void testCalibration()
{
    // powerFromDbQ8 is the inverse of dbQ8FromPower on the 2^20 scale
    double worst = 0;
    for (int d = -20 * 256; d <= 55 * 256; d += 97) {   // the range in use: gains to -20 dB, SNR to 55
        const int32_t back = dbQ8FromPower(powerFromDbQ8(d)) - dbQ8FromPower(1u << 20);
        worst = std::max(worst, std::fabs((back - d) / 256.0));
    }
    CHECK(worst < 0.02, "power/dB round trip worst %.4f dB", worst);

    // correction: floor, below it and up to 3 dB above unchanged; from 10 dB
    // above (or 3 + |gain| + 1 dB for big gains) the gain comes off in full
    // as a dB offset; linear fade in between; never decreasing
    const int32_t fl = -3 * 256;
    CHECK(correctDbQ8(fl, fl, 4 * 256) == fl, "floor unchanged");
    CHECK(correctDbQ8(fl - 300, fl, -4 * 256) == fl - 300, "below floor unchanged");
    CHECK(correctDbQ8(fl + 3 * 256, fl, -4 * 256) == fl + 3 * 256, "3 dB above floor unchanged");
    for (double snr : {10.0, 20.0, 40.0}) {
        for (double g : {-6.0, -2.0, 2.0, 4.0}) {
            const int32_t in = fl + (int32_t)(snr * 256);
            const double got = (in - correctDbQ8(in, fl, (int32_t)(g * 256))) / 256.0;
            CHECK(std::fabs(got - g) < 0.01, "snr %.0f gain %+.0f: offset %.3f", snr, g, got);
        }
    }
    // 4 dB gain: fade 3..10 dB above the floor, half way at 6.5 dB -> 2 dB off
    CHECK(std::abs((fl + 6 * 256 + 128) - correctDbQ8(fl + 6 * 256 + 128, fl, 4 * 256) - 2 * 256) <= 1,
          "half way through the fade: half the gain");
    // 7 dB gain: the fade widens to 8 dB (|gain| + 1), half way at 7 dB -> 3.5 dB off
    CHECK(std::abs((fl + 7 * 256) - correctDbQ8(fl + 7 * 256, fl, 7 * 256) - 7 * 128) <= 1,
          "wide fade for a big gain");
    for (double g : {-20.0, -6.0, -2.0, 2.0, 6.0, 12.0, 20.0}) {
        int32_t prev = INT32_MIN;
        bool mono = true;
        for (int32_t in = fl - 5 * 256; in <= fl + 50 * 256; ++in) {
            const int32_t o = correctDbQ8(in, fl, (int32_t)(g * 256));
            mono = mono && o >= prev;
            prev = o;
        }
        CHECK(mono, "gain %+.0f: output never falls as input rises", g);
        const int32_t far = fl + 50 * 256;
        CHECK(far - correctDbQ8(far, fl, (int32_t)(g * 256)) == (int32_t)(g * 256), "gain %+.0f: full offset", g);
    }

    // table: interpolation, nearest floor, and the scanner applying it
    CalTable t;
    t.set(5658, 4 * 256);
    t.set(5917, -2 * 256);
    t.setFloor(5658, -3 * 256);
    CHECK(t.gainAt(5600) == 4 * 256 && t.gainAt(6000) == -2 * 256, "clamped ends");
    const int32_t mid = t.gainAt((5658 + 5917) / 2);
    CHECK(std::abs(mid - 256) <= 3, "interpolated mid %d", mid);
    CHECK(t.floorAt(5917, -9) == -3 * 256, "nearest floor %d", t.floorAt(5917, -9));
    CalTable empty;
    CHECK(empty.floorAt(5800, -777) == -777, "fallback floor");

    SimRadio r;
    Scanner s(r);
    SinkLog log;
    s.setSink(logSink, &log);
    s.setSeatFreq(0, 5658);
    r.emitters = {{5658, 100}};   // ~40 dB
    for (int i = 0; i < 30; ++i)
        s.cycle(fakeUs);
    const int32_t raw = s.lastResult(0).bandDbQ8;
    const int32_t seen = log.last[0];
    s.cal().set(5658, 4 * 256);
    s.cal().setFloor(5658, -3 * 256);
    for (int i = 0; i < 30; ++i)
        s.cycle(fakeUs);
    const int32_t seenCal = log.last[0];
    printf("  scanner: raw %.2f dB, delivered %.2f dB uncalibrated, %.2f dB with +4 dB gain removed\n",
           raw / 256.0, seen / 256.0, seenCal / 256.0);
    CHECK(std::fabs((seen - seenCal) / 256.0 - 4.0) < 0.6, "about 4 dB off at high SNR: %.2f", (seen - seenCal) / 256.0);
    s.cal().enabled = false;
    s.cycle(fakeUs);
    CHECK(std::abs(log.last[0] - s.lastResult(0).bandDbQ8) < 1, "disabled: raw passes through");
}

int main()
{
    printf("log\n");
    testLog();
    printf("meter tone\n");
    testMeterTone();
    printf("meter selectivity\n");
    testMeterSelectivity();
    printf("image\n");
    testImage();
    printf("LO side\n");
    testChooseSide();
    printf("scanner and overload hold\n");
    testScannerAndHold();
    printf("calibration\n");
    testCalibration();
    printf("RotorHazard protocol\n");
    testRhProtocol();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
