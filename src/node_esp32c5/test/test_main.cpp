// Host tests for the ESP32-C5 node: the meter, the scanner and RotorHazard's
// own node core (RssiNode.cpp, commands.cpp) driven over its serial protocol.
//   make -C test
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "c5_filter.h"
#include "c5_meter.h"
#include "c5_policy.h"
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
    CHECK(s.lastObservation(0).flags == 6, "overload marked held+clipped");
    CHECK(s.lastObservation(0).goodAgeUs > 0, "held sample has age");
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
    CHECK(s.lastObservation(0).flags == 3, "failed marked failed+held");
    CHECK(s.lastObservation(0).goodAgeUs == 2 * r.stepUs, "failure preserves last valid time");
    CHECK(s.lastObservation(1).flags == 0 && s.lastObservation(1).goodAgeUs == 0,
          "fresh measurement has no flags and zero age");

    // seat off and back on
    s.setSeatFreq(1, 1111);
    s.cycle(fakeUs);
    CHECK(s.visitCount() == 1, "after off: %d", s.visitCount());
    CHECK(s.visit(0).pilotMhz == 5800, "remaining %u", s.visit(0).pilotMhz);

    // scale mapping
    s.config().floorDbQ8 = -8 * 256;
    CHECK(s.rssiCounts(-8 * 256) == 1, "clamp low");
    CHECK(s.rssiCounts(0) == 32, "%d", s.rssiCounts(0));
    CHECK(s.rssiCounts(100 * 256) == 254, "clamp high");
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
static int16_t signedBe16(const std::vector<uint8_t>& d, size_t at) { return (int16_t)be16(d, at); }

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
    CHECK(readCmd(link, out, READ_RHFEAT_FLAGS, 2, &d) &&
          be16(d, 0) == (RHFEAT_C5_DETECTION_CONFIG | RHFEAT_C5_CALIBRATION_CONFIG | RHFEAT_C5_NODE_ID | RHFEAT_C5_NODE_STATS),
          "features");
    // Its health (0x45): the firmware's part, the scanner's and the links' counters.
    rhSetSystemInfo([](c5::NodeSystem* sys) {
        sys->uptimeS = 3723;
        sys->heap = 150000;
        sys->heapMin = 120000;
        sys->resetReason = 11;
        sys->flags = c5::kNodeStatsUartLink;
    });
    {
        CaptureOut uartOut;
        RhLink uart(uartOut);
        rhSetStatsLinks(&link, &uart);
        const uint8_t noisy[] = {'!', 0x05, READ_MULTINODE_COUNT};   // a stray '!', then RH's next command
        uart.feed(noisy, sizeof(noisy));
        CHECK(uart.noise() == 1, "noise counted %u", (unsigned)uart.noise());
        const uint32_t before = link.commands();
        CHECK(readCmd(link, out, c5::kReadNodeStats, c5::kNodeStatsSize, &d), "stats read");
        auto u32 = [&](int at) { return ((uint32_t)d[at] << 24) | ((uint32_t)d[at + 1] << 16) | ((uint32_t)d[at + 2] << 8) | d[at + 3]; };
        CHECK(d[0] == c5::kNodeStatsVersion && d[1] == 11 && d[2] == c5::kNodeStatsUartLink, "stats header");
        CHECK(u32(4) == 3723 && u32(8) == 150000 && u32(12) == 120000, "stats system");
        CHECK(u32(40) == 2 && u32(44) == 0 && u32(48) == 1, "stats uart link %u %u %u", u32(40), u32(44), u32(48));
        CHECK(u32(52) == before, "stats usb link %u", u32(52));
        rhSetStatsLinks(nullptr, nullptr);
        rhSetSystemInfo(nullptr);
    }
    // The node's identity: whatever it was given (the factory MAC on the C5).
    const uint8_t id[6] = {0x3c, 0x84, 0x27, 0x0a, 0x1b, 0x2c};
    rhSetNodeId(id);
    CHECK(readCmd(link, out, c5::kReadNodeId, 6, &d) && !memcmp(d.data(), id, 6), "node id");
    // Its name: written over the link, kept by the store, read back zero padded.
    static std::string stored;
    rhSetNameStore([](const char* name) { stored = name; });
    std::vector<uint8_t> name(c5::kNodeNameSize, 0);
    memcpy(name.data(), "Start gate", 10);
    writeCmd(link, c5::kWriteNodeName, name);
    CHECK(stored == "Start gate" && std::string(rhNodeName()) == "Start gate", "name stored '%s'", stored.c_str());
    CHECK(readCmd(link, out, c5::kReadNodeName, c5::kNodeNameSize, &d) && d == name, "name read back");
    CHECK(readCmd(link, out, READ_FW_PROCTYPE, 16, &d) && !memcmp(d.data(), "ESP32C5", 7), "proctype");

    // C5 detection configuration v3: one input, the shortest pass (ms); the
    // firmware picks the least-jitter point of its curve (c5_policy.h) whose
    // boundary the pass reaches; past the reliable end, faster extreme points.
    // No channels yet: the policy assumes eight visits.
    CHECK(policyRevisitUs(0) == 5400 && policyRevisitUs(1) == 675 && policyRevisitUs(8, 512) == 3296 &&
          policyRevisitUs(8, 128) == 1712, "policy revisit");
    CHECK(kPolicyCurvePoints == 12 && policyBoundaryMs(kPolicyCurve[0], 8) == 17 &&
          policyBoundaryMs(kPolicyCurve[1], 8) == 16 && policyBoundaryMs(kPolicyCurve[9], 8) == 5 &&
          policyBoundaryMs(kPolicyCurve[10], 8) == 2 && policyBoundaryMs(kPolicyCurve[11], 8) == 1 &&
          policyBoundaryMs(kPolicyCurve[0], 1) == 3, "policy boundaries");
    CHECK(policyIndexFor(400, 8) == 0 && policyIndexFor(16, 8) == 1 && policyIndexFor(10, 8) == 5 &&
          policyIndexFor(5, 8) == 9 && policyIndexFor(4, 8) == 10 && policyIndexFor(2, 8) == 10 &&
          policyIndexFor(1, 8) == 11, "policy curve");
    for (int i = 1; i < kPolicyCurvePoints; ++i)
        CHECK(kPolicyCurve[i].jitterX10 >= kPolicyCurve[i - 1].jitterX10 &&
              kPolicyCurve[i].margin >= kPolicyCurve[i - 1].margin &&
              (!kPolicyCurve[i - 1].extreme || kPolicyCurve[i].extreme),
              "jitter, margin and extreme only grow along the curve at %d", i);

    const size_t c0 = 14, cs = 6;   // curve offset and point size in the readback
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d), "read detection config");
    CHECK(d.size() == kDetectionConfigReadSize && d[0] == 3 && d[1] == 3 && be16(d, 2) == 0 &&
          d[7] == 0 && d[13] == kDetectionConfigOk &&
          be16(d, c0) == 17 && d[c0 + 2] == 10 && d[c0 + 3] == 5 && d[c0 + 4] == 64 && d[c0 + 5] == 12 &&
          be16(d, c0 + 9 * cs) == 5 && d[c0 + 9 * cs + 5] == 3 &&
          be16(d, c0 + 11 * cs) == 1 && d[c0 + 11 * cs + 2] == 85 && d[c0 + 11 * cs + 3] == 18 &&
          d[c0 + 11 * cs + 4] == 8 && d[c0 + 11 * cs + 5] == (0x80 | 2),
          "initial detection config and curve");

    writeCmd(link, kWriteDetectionConfig, {3, 0x01, 0x90});   // 400 ms
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 400 &&
          (d[1] & 0x04) && !(d[1] & 0x08) && be16(d, 4) == 1024 && d[6] == 12 &&
          d[7] == 10 && d[8] == 5 && be16(d, 9) == 65 && be16(d, 11) == 5400 && d[13] == kDetectionConfigOk,
          "400 ms: 12 readings");
    CHECK(s.config().captureN == 1024 && rhUsingPowerMean() && rhPowerMeanLength() == 12,
          "policy reached scanner/filter");

    writeCmd(link, kWriteDetectionConfig, {3, 0, 10});
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 10 &&
          !(d[1] & 0x08) && d[6] == 7 && d[7] == 12 && d[8] == 6 && be16(d, 9) == 38,
          "10 ms: 7 readings");

    writeCmd(link, kWriteDetectionConfig, {3, 0, 4});
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 4 &&
          (d[1] & 0x08) && be16(d, 4) == 512 && d[6] == 2 && d[7] == 40 && d[8] == 10 &&
          be16(d, 11) == 3296 && be16(d, 9) == 7 && rhDetectionExtreme() && s.config().captureN == 512,
          "4 ms: extreme, 512 samples, mean 2");

    writeCmd(link, kWriteDetectionConfig, {3, 0, 1});
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 1 &&
          (d[1] & 0x08) && be16(d, 4) == 128 && d[6] == 2 && d[7] == 85 && d[8] == 18 && be16(d, 11) == 1712,
          "1 ms: extreme, 128 samples, mean 2");

    writeCmd(link, kWriteDetectionConfig, {3, 0, 0});
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 1 &&
          d[13] == kDetectionConfigBadPassMs, "reject zero pass atomically");
    writeCmd(link, kWriteDetectionConfig, {3, 0x07, 0xd1});
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 1 &&
          d[13] == kDetectionConfigBadPassMs, "reject 2001 ms atomically");
    writeCmd(link, kWriteDetectionConfig, {2, 0, 8});
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 2) == 1 &&
          d[13] == kDetectionConfigBadVersion, "reject old version atomically");

    // One channel for every seat: the revisit shrinks and a 4 ms pass gets
    // the full 12-reading mean on the once-a-second refresh.
    writeCmd(link, kWriteDetectionConfig, {3, 0, 4});
    for (int i = 0; i < 8; ++i)
        s.setSeatFreq(i, 5769);
    s.cycle(fakeUs);
    rhRefreshManagedDetection();
    CHECK(readCmd(link, out, kReadDetectionConfig, kDetectionConfigReadSize, &d) && be16(d, 11) == 675 &&
          d[6] == 12 && be16(d, 4) == 1024 && !(d[1] & 0x08) && be16(d, c0) == 3 && rhPowerMeanLength() == 12,
          "curve follows the channel plan");
    for (int i = 0; i < 8; ++i)
        s.setSeatFreq(i, 0);
    s.cycle(fakeUs);
    rhClearManagedDetection();
    rhSetPowerMeanLength(2);
    s.config().captureN = 256;

    // Calibration v2: the enabled switch and sixteen fixed trims (L1-L8,
    // R1-R8), replaced together; positive raises the reported RSSI.
    int stores = 0;
    static int* storeCount = nullptr;
    storeCount = &stores;
    rhSetCalibrationStore([] { ++*storeCount; });
    CHECK(readCmd(link, out, kReadCalibrationConfig, kCalibrationConfigDataSize, &d) &&
          d[0] == 2 && d[1] == 1 && d[2] == kCalibrationConfigOk && d[3] == 0 &&
          std::all_of(d.begin() + 4, d.end(), [](uint8_t b) { return b == 0; }),
          "initial calibration config");
    std::vector<uint8_t> cal(kCalibrationConfigDataSize, 0);
    cal[0] = 2;
    auto trim = [&](int index, int16_t q8) {
        cal[4 + index * 2] = (uint8_t)((uint16_t)q8 >> 8);
        cal[5 + index * 2] = (uint8_t)q8;
    };
    trim(8, 64);        // R1 +0.25 dB
    trim(9, -2 * 256);  // R2 -2 dB
    writeCmd(link, kWriteCalibrationConfig, cal);
    CHECK(readCmd(link, out, kReadCalibrationConfig, kCalibrationConfigDataSize, &d) &&
          d[1] == 0 && d[2] == kCalibrationConfigOk && signedBe16(d, 20) == 64 && signedBe16(d, 22) == -512,
          "apply enabled and trims");
    CHECK(!s.cal().enabled && s.trim().adjDbQ8[8] == 64 && stores == 1, "trims reached scanner and were stored");
    CHECK(s.gainAt(5658) == 0, "disabled: no correction");
    cal[1] = 1;
    writeCmd(link, kWriteCalibrationConfig, cal);
    // Between R1 and R2 the trim is interpolated: 5676 is halfway.
    CHECK(s.gainAt(5658) == -64 && s.gainAt(5695) == 512 && s.gainAt(5676) == -(64 + (-576) * 18 / 37) &&
          s.gainAt(5300) == 0, "trims interpolate between fixed channels");
    s.cal().set(5658, 2 * 256);   // console table: reads 2 dB high there
    CHECK(s.gainAt(5658) == 2 * 256 - 64, "trims add to the table");

    std::vector<uint8_t> badCal = cal;
    badCal[4] = (uint8_t)((uint16_t)(-19 * 256) >> 8);   // L1 -19 dB with the table's -2 dB: -21 dB
    badCal[5] = 0;
    writeCmd(link, kWriteCalibrationConfig, badCal);
    CHECK(readCmd(link, out, kReadCalibrationConfig, kCalibrationConfigDataSize, &d) &&
          d[2] == kCalibrationConfigBadAdjustment && signedBe16(d, 4) == 0 && signedBe16(d, 20) == 64 && stores == 2,
          "reject trims past 20 dB in total atomically");
    badCal = cal;
    badCal[0] = 1;
    writeCmd(link, kWriteCalibrationConfig, badCal);
    CHECK(readCmd(link, out, kReadCalibrationConfig, kCalibrationConfigDataSize, &d) &&
          d[2] == kCalibrationConfigBadVersion, "reject protocol v1");

    std::fill(cal.begin() + 4, cal.end(), 0);
    writeCmd(link, kWriteCalibrationConfig, cal);
    CHECK(readCmd(link, out, kReadCalibrationConfig, kCalibrationConfigDataSize, &d) &&
          d[1] == 1 && d[2] == kCalibrationConfigOk && s.gainAt(5658) == 2 * 256 && s.cal().n == 1,
          "zero trims leave the table");
    s.cal() = CalTable();
    rhSetCalibrationStore(nullptr);

    // Raceband on all eight seats, using upstream main's 8-bit RSSI protocol.
    const uint16_t rb[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    for (int i = 0; i < 8; ++i) {
        writeCmd(link, WRITE_CURNODE_INDEX, {(uint8_t)i});
        writeCmd(link, WRITE_FREQUENCY, {(uint8_t)(rb[i] >> 8), (uint8_t)rb[i]});
        CHECK(readCmd(link, out, TEST_RX_REGISTER, 1, &d) && d[0] == 1, "seat %d register test", i);
        // The C5 map is four 8-bit counts/dB: enter about 30 dB above the
        // measured floor and leave enough hysteresis for a clean exit.
        writeCmd(link, WRITE_ENTER_AT_LEVEL, {150});
        writeCmd(link, WRITE_EXIT_AT_LEVEL, {100});
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
    CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 8, &d), "pass stats");
    const uint8_t lap0 = d[0];
    const uint8_t floorRssi = d[3];
    printf("  seat 4 floor RSSI %u (8-bit)\n", floorRssi);
    CHECK(floorRssi > 5 && floorRssi < 40, "floor %u", floorRssi);

    const uint32_t t0 = g_ms;
    run(1500, 178, 300, 300, t0);   // 178 LSB = 45 dB
    CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 8, &d), "pass stats");
    const uint8_t lap1 = d[0];
    const uint16_t msSince = be16(d, 1);
    const uint8_t passPeak = d[5];
    const int peakTime = (int)(g_ms - msSince) - (int)t0;
    printf("  pass: laps %u -> %u, peak RSSI %u, timed at t0%+d ms (true peak t0+300)\n", lap0, lap1, passPeak, peakTime);
    CHECK(lap1 == (uint8_t)(lap0 + 1), "one lap: %u -> %u", lap0, lap1);
    CHECK(std::abs(peakTime - 300) <= 25, "pass time t0%+d", peakTime);
    CHECK(passPeak > 150, "peak %u", passPeak);

    // the other seats counted nothing; print what leaked (front-end model)
    printf("  other seats' rise during the pass:");
    for (int i = 0; i < 8; ++i) {
        if (i == 3)
            continue;
        writeCmd(link, WRITE_CURNODE_INDEX, {(uint8_t)i});
        CHECK(readCmd(link, out, READ_LAP_PASS_STATS, 8, &d), "pass stats %d", i);
        CHECK(d[0] == 0, "seat %d counted a lap", i);
        const uint8_t peak = d[4];   // node peak since frequency set
        printf(" %d:%+.1fdB", i + 1, ((int)peak - (int)floorRssi) / 4.0);
        CHECK(peak < 100, "seat %d node peak %u reached ExitAt", i, peak);
    }
    printf("\n");

    // console escape
    bool consoleSaw = false;
    static bool* flag;
    flag = &consoleSaw;
    link.setConsole([](const char* line, Transport&) { *flag = !strcmp(line, "status"); });
    const char* cl = "!status\n";
    link.feed((const uint8_t*)cl, strlen(cl));
    CHECK(consoleSaw, "console line");
    CHECK(readCmd(link, out, READ_REVISION_CODE, 2, &d), "protocol after console");

    // A stray '!' (noise on the radio link) must not swallow the commands after it.
    consoleSaw = false;
    const uint8_t noise[] = {'!', 0xF8, 'x'};
    link.feed(noise, sizeof noise);
    // The next binary command (a control byte, as most of RotorHazard's are) is answered at once.
    CHECK(readCmd(link, out, READ_RHFEAT_FLAGS, 2, &d), "first command after a stray '!'");
    CHECK(readCmd(link, out, READ_REVISION_CODE, 2, &d), "protocol after a stray '!'");
    CHECK(!consoleSaw, "no console line from noise");
    // UTF-8 in a console line is still text.
    link.feed((const uint8_t*)"!status \xC3\xA9\n", 11);
    CHECK(readCmd(link, out, READ_REVISION_CODE, 2, &d), "protocol after a UTF-8 console line");
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

static void testRuntimeMedian()
{
    // identical to RotorHazard's 255-reading median, value and timestamp
    MedianFilter<rssi_t, SmoothingSamples, 0> rh;
    RuntimeMedian rt;
    rt.setLength(255);
    uint32_t seed = 7;
    int mismatches = 0;
    for (int i = 0; i < 3000; ++i) {
        seed = seed * 1103515245u + 12345u;
        const rssi_t v = (rssi_t)((seed >> 16) % 255);
        rh.addRawValue((mtime_t)(i * 2), v);
        rt.addRawValue((mtime_t)(i * 2), v);
        if (rh.isFilled() != rt.isFilled())
            ++mismatches;
        else if (rh.isFilled() && (rh.getFilteredValue() != rt.getFilteredValue() ||
                                   rh.getFilterTimestamp() != rt.getFilterTimestamp()))
            ++mismatches;
    }
    CHECK(mismatches == 0, "runtime median differs from RH's in %d of 3000 steps", mismatches);
    CHECK(medianLengthFor(250, 437) == 109, "250 ms at 437/s -> %d", medianLengthFor(250, 437));
    CHECK(medianLengthFor(250, 1000) == 251, "250 ms at 1000/s -> %d", medianLengthFor(250, 1000));
    CHECK(medianLengthFor(5000, 1000) == 255 && medianLengthFor(1, 100) == 1, "clamps");

    // A pass above EnterAt for 200 ms (a fast quad), fed at the C5's measured
    // 437 readings/s per seat into RotorHazard's own RssiNode: RH's 255-reading
    // median (0.58 s here) loses it; the same median sized to 250 ms keeps it.
    auto lapsFor = [](Filter<rssi_t>* f, int* peakTimeErrMs) {
        RssiNode node;
        node.rssiSetFilter(f);
        node.rssiInit();
        node.setActivatedFlag(true);
        node.setEnterAtLevel(100);
        node.setExitAtLevel(70);
        const double rate = 437.0;
        const uint8_t lap0 = node.getLastPass().lap;
        const int passMid = 2000;   // ms
        for (int i = 0; i < (int)(4.0 * rate); ++i) {
            const int tMs = (int)(i * 1000.0 / rate);
            // floor 30, a symmetric pass above 100 for 200 ms around passMid.
            const int d = std::abs(tMs - passMid);
            int v = 30;
            if (d < 250)
                v = 150 - d / 2;
            node.rssiProcessValue((mtime_t)tMs, (rssi_t)v);
        }
        *peakTimeErrMs = (int)node.getLastPass().timestamp - passMid;
        return (int)(uint8_t)(node.getLastPass().lap - lap0);
    };
    MedianFilter<rssi_t, SmoothingSamples, 0> stock;
    RuntimeMedian timed;
    timed.setLength(medianLengthFor(250, 437));
    int errStock = 0, errTimed = 0;
    const int lapsStock = lapsFor(&stock, &errStock), lapsTimed = lapsFor(&timed, &errTimed);
    printf("  200 ms pass at 437 readings/s: RH 255-reading median %d lap(s); 250 ms (%d readings) %d lap(s), "
           "pass time %+d ms\n", lapsStock, timed.length(), lapsTimed, errTimed);
    CHECK(lapsStock == 0, "stock median should lose this pass (%d)", lapsStock);
    CHECK(lapsTimed == 1, "time-sized median should count it (%d)", lapsTimed);
    CHECK(std::abs(errTimed) <= 5, "pass time %+d ms", errTimed);

    ShortPowerMean mean;
    mean.setLength(2);
    mtime_t mt = 0;
    int32_t md = 0;
    CHECK(!mean.add(100, 0, false, &mt, &md), "mean waits for two readings");
    CHECK(mean.add(102, 10 * 256, false, &mt, &md), "mean produces second reading");
    const int32_t expected = dbQ8FromPower((powerFromDbQ8(0) + powerFromDbQ8(10 * 256)) / 2)
                           - dbQ8FromPower(1u << 20);
    CHECK(std::abs(md - expected) <= 1, "linear-power mean %.3f vs %.3f dB",
          dbOf(md), dbOf(expected));
    CHECK(mt == 102, "mean timestamp follows strongest raw sample: %u", (unsigned)mt);
    CHECK(!mean.add(104, 20 * 256, true, &mt, &md), "held reading breaks mean window");
    CHECK(!mean.add(106, 2 * 256, false, &mt, &md), "fresh window refills after hold");
    CHECK(mean.add(108, 1 * 256, false, &mt, &md) && mt == 106,
          "refilled mean uses strongest fresh timestamp");

    LatestFilter latest;
    CHECK(!latest.isFilled(), "pass-through starts empty");
    latest.addRawValue(55, 123);
    CHECK(latest.isFilled() && latest.getFilteredValue() == 123 && latest.getFilterTimestamp() == 55,
          "pass-through preserves value and timestamp");
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
    printf("runtime median\n");
    testRuntimeMedian();
    printf("calibration\n");
    testCalibration();
    printf("RotorHazard protocol\n");
    testRhProtocol();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
